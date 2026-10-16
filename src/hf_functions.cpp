#include "hf_fs.hpp"

#include "hf_crypto.hpp"
#include "hf_functions.hpp"
#include "hf_settings.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// hf_ls
//===----------------------------------------------------------------------===//

struct HFListBindData : public TableFunctionData {
	HFTarget target;
	bool recursive = false;
};

struct HFListState : public GlobalTableFunctionState {
	vector<HFListEntry> entries;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static string NamedArgumentOr(TableFunctionBindInput &input, const char *key, const string &fallback) {
	auto entry = input.named_parameters.find(Identifier(key));
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return fallback;
	}
	return entry->second.ToString();
}

static bool NamedArgumentOr(TableFunctionBindInput &input, const char *key, bool fallback) {
	auto entry = input.named_parameters.find(Identifier(key));
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return fallback;
	}
	return entry->second.GetValue<bool>();
}

static unique_ptr<FunctionData> HFListBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("hf_ls requires an hf:// path, for example hf_ls('hf://datasets/user/repo/data')");
	}
	ClientContextFileOpener file_opener(context);
	auto bind_data = make_uniq<HFListBindData>();
	bind_data->target = HFResolveTarget(StringValue::Get(input.inputs[0]), &file_opener, DatabaseInstance::GetDatabase(context));
	bind_data->recursive = NamedArgumentOr(input, "recursive", false);

	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("path");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("is_dir");
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("size");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("sha256");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HFListInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto state = make_uniq<HFListState>();
	auto &bind_data = input.bind_data->Cast<HFListBindData>();

	// listing a path that turns out to be a file returns that file, like `ls file.csv` would
	idx_t size = 0;
	string etag;
	if (!bind_data.target.url.path.empty() && bind_data.target.url.path != "/" &&
	    !HFUtil::HasGlob(bind_data.target.url.path) &&
	    HFTryGetFileSize(db, &file_opener, bind_data.target, size, etag)) {
		HFListEntry entry;
		entry.path = bind_data.target.url.path.empty() ? bind_data.target.url.path
		                                               : bind_data.target.url.path.substr(1);
		entry.size = size;
		state->entries.push_back(std::move(entry));
		return std::move(state);
	}

	if (HFUtil::HasGlob(bind_data.target.url.path)) {
		// a glob is expanded through the filesystem, which knows how to descend efficiently
		auto &fs = FileSystem::GetFileSystem(context);
		for (auto &file : fs.Glob(bind_data.target.url.ToString(), &file_opener)) {
			auto target = HFResolveTarget(file.path, &file_opener, db);
			HFListEntry entry;
			entry.path = target.url.path;
			idx_t file_size = 0;
			string file_etag;
			if (HFTryGetFileSize(db, &file_opener, target, file_size, file_etag)) {
				entry.size = file_size;
			}
			entry.path = HFUtil::BaseName(target.url.path);
			state->entries.push_back(std::move(entry));
		}
		return std::move(state);
	}

	HFListEntries(db, &file_opener, bind_data.target, bind_data.recursive,
	              [&](const HFListEntry &entry) { state->entries.push_back(entry); });
	return std::move(state);
}

static void HFListFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFListState>();
	auto &bind_data = data.bind_data->Cast<HFListBindData>();
	idx_t count = 0;
	while (state.offset < state.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = state.entries[state.offset++];
		output.data[0].SetValue(count, Value(HFFileSystem::GetHFUrl(bind_data.target.url, entry.path)));
		output.data[1].SetValue(count, Value(entry.path));
		output.data[2].SetValue(count, Value(entry.is_dir));
		output.data[3].SetValue(count, Value::UBIGINT(entry.size));
		output.data[4].SetValue(count, entry.lfs_oid.empty() ? Value() : Value(entry.lfs_oid));
		count++;
	}
	output.SetChildCardinality(count_t(count));
}

//===----------------------------------------------------------------------===//
// hf_meta
//===----------------------------------------------------------------------===//

//! One-shot table functions (hf_meta, hf_upload) still have to report "no more rows" exactly once.
struct HFOnceState : public GlobalTableFunctionState {
	bool done = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

struct HFMetaBindData : public TableFunctionData {
	HFTarget target;
};

static unique_ptr<GlobalTableFunctionState> HFOnceInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<HFOnceState>();
}

static unique_ptr<FunctionData> HFMetaBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("hf_meta requires an hf:// path");
	}
	ClientContextFileOpener file_opener(context);
	auto bind_data = make_uniq<HFMetaBindData>();
	bind_data->target = HFResolveTarget(StringValue::Get(input.inputs[0]), &file_opener, DatabaseInstance::GetDatabase(context));

	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("exists");
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("size");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("etag");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("repo_type");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("repository");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("revision");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(bind_data);
}

static void HFMetaFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFOnceState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	state.done = true;
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto &bind_data = data.bind_data->Cast<HFMetaBindData>();
	idx_t size = 0;
	string etag;
	bool exists = HFTryGetFileSize(db, &file_opener, bind_data.target, size, etag);
	output.data[0].SetValue(0, Value(bind_data.target.url.ToString()));
	output.data[1].SetValue(0, Value(exists));
	output.data[2].SetValue(0, Value::UBIGINT(exists ? size : 0));
	output.data[3].SetValue(0, etag.empty() ? Value() : Value(etag));
	output.data[4].SetValue(0, Value(bind_data.target.url.repo_type));
	output.data[5].SetValue(0, Value(bind_data.target.url.repository));
	output.data[6].SetValue(0, Value(bind_data.target.url.revision));
	output.SetChildCardinality(1);
}

//===----------------------------------------------------------------------===//
// hf_upload
//===----------------------------------------------------------------------===//

struct HFUploadBindData : public TableFunctionData {
	string local_path;
	HFTarget target;
	string commit_message;
};

static unique_ptr<FunctionData> HFUploadBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.size() < 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("hf_upload requires a local path and an hf:// path, for example "
		                      "hf_upload('out.parquet', 'hf://datasets/user/repo/out.parquet')");
	}
	ClientContextFileOpener file_opener(context);
	auto bind_data = make_uniq<HFUploadBindData>();
	bind_data->local_path = StringValue::Get(input.inputs[0]);
	bind_data->target = HFResolveTarget(StringValue::Get(input.inputs[1]), &file_opener, DatabaseInstance::GetDatabase(context));
	bind_data->commit_message = NamedArgumentOr(input, "message", HFDefaultCommitMessage(bind_data->target.url));

	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("size");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("sha256");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(bind_data);
}

static void HFUploadFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFOnceState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	state.done = true;
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto &bind_data = data.bind_data->Cast<HFUploadBindData>();
	auto &local = FileSystem::GetLocal(db);
	if (!local.FileExists(bind_data.local_path)) {
		throw IOException("Local file '%s' does not exist - hf_upload takes a path on this machine as first argument",
		                  bind_data.local_path);
	}
	auto blob = HFComputeBlobInfo(db, bind_data.local_path);
	HFUploadLocalFile(db, &file_opener, bind_data.target, bind_data.local_path, blob, bind_data.commit_message);

	output.data[0].SetValue(0, Value(bind_data.target.url.ToString()));
	output.data[1].SetValue(0, Value::UBIGINT(blob.size));
	output.data[2].SetValue(0, Value(blob.sha256));
	output.SetChildCardinality(1);
}

//===----------------------------------------------------------------------===//
// hf_delete
//===----------------------------------------------------------------------===//

struct HFDeleteBindData : public TableFunctionData {
	vector<HFTarget> targets;
};

struct HFDeleteState : public GlobalTableFunctionState {
	bool done = false;
	vector<string> deleted;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HFDeleteBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("hf_delete requires one hf:// path or a list of them");
	}
	ClientContextFileOpener file_opener(context);
	auto bind_data = make_uniq<HFDeleteBindData>();
	if (input.inputs[0].type().id() == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(input.inputs[0])) {
			if (child.IsNull()) {
				throw BinderException("hf_delete does not accept NULL paths");
			}
			bind_data->targets.push_back(
			    HFResolveTarget(StringValue::Get(child), &file_opener, DatabaseInstance::GetDatabase(context)));
		}
	} else {
		bind_data->targets.push_back(
		    HFResolveTarget(StringValue::Get(input.inputs[0]), &file_opener, DatabaseInstance::GetDatabase(context)));
	}

	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HFDeleteInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto state = make_uniq<HFDeleteState>();
	auto &bind_data = input.bind_data->Cast<HFDeleteBindData>();

	// one commit per repository: paths may point at different repos, and a commit only ever touches one
	struct Group {
		HFTarget target;
		vector<string> files;
		vector<string> folders;
	};
	std::map<string, Group> groups;
	for (auto &target : bind_data.targets) {
		auto key = target.url.endpoint + "|" + target.url.repo_type + "|" + target.url.repository + "|" +
		           target.url.revision;
		if (groups.find(key) == groups.end()) {
			groups[key].target = target;
		}
		auto &group = groups[key];
		idx_t size = 0;
		string etag;
		if (HFTryGetFileSize(db, &file_opener, target, size, etag)) {
			group.files.push_back(target.url.path);
			state->deleted.push_back(target.url.ToString());
			continue;
		}
		// not a file: delete everything below it (the hub deletes the now-empty folders along with them)
		vector<string> children;
		try {
			HFListEntries(db, &file_opener, target, true, [&](const HFListEntry &entry) {
				if (!entry.is_dir) {
					children.push_back(entry.path);
				}
			});
		} catch (std::exception &) {
			throw IOException("Nothing to delete at '%s' - no file or folder exists there", target.url.ToString());
		}
		if (children.empty()) {
			throw IOException("Nothing to delete at '%s' - the folder is empty", target.url.ToString());
		}
		for (auto &child : children) {
			group.files.push_back("/" + child);
			state->deleted.push_back(target.url.WithPath("/" + child).ToString());
		}
	}
	for (auto &entry : groups) {
		HFDeletePaths(db, &file_opener, entry.second.target, entry.second.files, entry.second.folders,
		              "Delete with DuckDB");
	}
	state->done = true;
	return std::move(state);
}

static void HFDeleteFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFDeleteState>();
	idx_t count = 0;
	while (state.offset < state.deleted.size() && count < STANDARD_VECTOR_SIZE) {
		output.data[0].SetValue(count, Value(state.deleted[state.offset++]));
		count++;
	}
	output.SetChildCardinality(count_t(count));
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void HFFunctions::Register(ExtensionLoader &loader) {
	TableFunction hf_ls(Identifier("hf_ls"), {LogicalType::VARCHAR}, HFListFunction, HFListBind, HFListInit);
	hf_ls.GetSignature().AddKeywordOnly("recursive", LogicalType::BOOLEAN, Value(false));
	loader.RegisterFunction(hf_ls);

	TableFunction hf_meta(Identifier("hf_meta"), {LogicalType::VARCHAR}, HFMetaFunction, HFMetaBind, HFOnceInit);
	loader.RegisterFunction(hf_meta);

	TableFunction hf_upload(Identifier("hf_upload"), {LogicalType::VARCHAR, LogicalType::VARCHAR}, HFUploadFunction,
	                         HFUploadBind, HFOnceInit);
	hf_upload.GetSignature().AddKeywordOnly("message", LogicalType::VARCHAR);
	loader.RegisterFunction(hf_upload);

	TableFunction hf_delete_str(Identifier("hf_delete"), {LogicalType::VARCHAR}, HFDeleteFunction, HFDeleteBind,
	                             HFDeleteInit);
	TableFunction hf_delete_list(Identifier("hf_delete"), {LogicalType::LIST(LogicalType::VARCHAR)}, HFDeleteFunction,
	                              HFDeleteBind, HFDeleteInit);
	TableFunctionSet hf_delete("hf_delete");
	hf_delete.AddFunction(hf_delete_str);
	hf_delete.AddFunction(hf_delete_list);
	loader.RegisterFunction(std::move(hf_delete));
}

} // namespace duckdb
