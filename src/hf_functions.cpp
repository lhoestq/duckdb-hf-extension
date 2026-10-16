#include "hf_fs.hpp"

#include "hf_crypto.hpp"
#include "hf_functions.hpp"
#include "hf_settings.hpp"

#include "duckdb/common/file_opener.hpp"
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

static int64_t NamedArgumentOr(TableFunctionBindInput &input, const char *key, int64_t fallback) {
	auto entry = input.named_parameters.find(Identifier(key));
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return fallback;
	}
	return entry->second.GetValue<int64_t>();
}

static string NamedArgumentAsString(TableFunctionBindInput &input, const char *key) {
	auto entry = input.named_parameters.find(Identifier(key));
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return "";
	}
	return StringValue::Get(entry->second);
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
// hf_list_revisions / hf_list_refs
//===----------------------------------------------------------------------===//

struct HFRepoInfoBindData : public TableFunctionData {
	HFTarget target;
	idx_t limit = 20;
};

struct HFRepoInfoState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> HFRevisionsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HFRepoInfoBindData>();
	auto state = make_uniq<HFRepoInfoState>();
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	HFListRevisions(db, &file_opener, bind_data.target, bind_data.limit, [&](const HFRevisionInfo &revision) {
		vector<Value> row;
		row.emplace_back(bind_data.target.url.repository);
		row.emplace_back(revision.sha);
		row.emplace_back(revision.title);
		row.emplace_back(revision.author);
		row.emplace_back(revision.date);
		state->rows.push_back(std::move(row));
	});
	return std::move(state);
}

static unique_ptr<GlobalTableFunctionState> HFRefsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HFRepoInfoBindData>();
	auto state = make_uniq<HFRepoInfoState>();
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	HFListRefs(db, &file_opener, bind_data.target, [&](const HFRefInfo &ref) {
		vector<Value> row;
		row.emplace_back(bind_data.target.url.repository);
		row.emplace_back(ref.kind);
		row.emplace_back(ref.name);
		row.emplace_back(ref.ref);
		row.emplace_back(ref.target_commit);
		state->rows.push_back(std::move(row));
	});
	return std::move(state);
}

static unique_ptr<FunctionData> HFRepoInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names,
                                               const char *function_name, bool with_limit) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException(std::string(function_name) + " requires an hf:// path, for example " +
		                      function_name + "('hf://datasets/user/repo')");
	}
	ClientContextFileOpener file_opener(context);
	auto bind_data = make_uniq<HFRepoInfoBindData>();
	bind_data->target = HFResolveTarget(StringValue::Get(input.inputs[0]), &file_opener, DatabaseInstance::GetDatabase(context));
	if (with_limit) {
		auto limit = NamedArgumentOr(input, "max_revisions", int64_t(20));
		if (limit <= 0) {
			throw BinderException("%s wants a positive 'max_revisions'", function_name);
		}
		bind_data->limit = NumericCast<idx_t>(limit);
	}

	names.emplace_back("repository");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("revision");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("title");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("author");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("date");
	return_types.push_back(LogicalType::VARCHAR);
	if (!with_limit) {
		// hf_list_refs reuses the same bind data with different columns
		names.clear();
		return_types.clear();
		names.emplace_back("repository");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("kind");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("name");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("ref");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("target_commit");
		return_types.push_back(LogicalType::VARCHAR);
	}
	return std::move(bind_data);
}

static unique_ptr<FunctionData> HFRevisionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	return HFRepoInfoBind(context, input, return_types, names, "hf_list_revisions", true);
}

static unique_ptr<FunctionData> HFRefsBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	return HFRepoInfoBind(context, input, return_types, names, "hf_list_refs", false);
}

static void HFRepoInfoFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFRepoInfoState>();
	idx_t out = 0;
	while (state.offset < state.rows.size() && out < STANDARD_VECTOR_SIZE) {
		const auto &row = state.rows[state.offset++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.data[col].SetValue(out, row[col]);
		}
		out++;
	}
	output.SetChildCardinality(out);
}

//===----------------------------------------------------------------------===//
// hf_create_repo
//===----------------------------------------------------------------------===//

struct HFCreateRepoBindData : public TableFunctionData {
	string url;
	HFTarget target;
	bool private_repo = true;
};

static unique_ptr<FunctionData> HFCreateRepoBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("hf_create_repo requires an hf:// repo url, for example "
		                      "hf_create_repo('hf://datasets/user/my-dataset')");
	}
	ClientContextFileOpener file_opener(context);
	auto bind_data = make_uniq<HFCreateRepoBindData>();
	bind_data->target = HFResolveTarget(StringValue::Get(input.inputs[0]), &file_opener, DatabaseInstance::GetDatabase(context));
	if (!bind_data->target.url.path.empty() && bind_data->target.url.path != "/") {
		throw BinderException("hf_create_repo takes a repository url without a path, got '%s'",
		                      bind_data->target.url.ToString());
	}
	bind_data->url = StringValue::Get(input.inputs[0]);
	bind_data->private_repo = NamedArgumentOr(input, "private", true);

	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("created");
	return_types.push_back(LogicalType::BOOLEAN);
	return std::move(bind_data);
}

static void HFCreateRepoFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFOnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind_data = data.bind_data->Cast<HFCreateRepoBindData>();
	ClientContextFileOpener file_opener(context);
	auto created = HFCreateRepo(DatabaseInstance::GetDatabase(context), &file_opener, bind_data.target,
	                           bind_data.private_repo);
	output.data[0].SetValue(0, Value(bind_data.url));
	output.data[1].SetValue(0, Value::BOOLEAN(created));
	output.SetChildCardinality(1);
}

//===----------------------------------------------------------------------===//
// hf_status
//===----------------------------------------------------------------------===//

struct HFStatusState : public GlobalTableFunctionState {
	vector<Value> row;
	bool done = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> HFStatusInit(ClientContext &context, TableFunctionInitInput &) {
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto state = make_uniq<HFStatusState>();

	// a benign path: only used to resolve the endpoint/token the same way a read would
	static const string probe_url = "hf://datasets/huggingface/nothing";
	auto connection = HFSecrets::Resolve(&file_opener, nullptr, probe_url, &db);

	string user, fullname, whoami_error;
	bool is_pro = false;
	HFWhoami(db, &file_opener, probe_url, user, fullname, is_pro, whoami_error);

	// the name of the registered HTTP transport: "httpfs" in practice, "none" until one is loaded
	string transport = HTTPUtil::Get(db).GetName();
	if (transport.empty()) {
		transport = "none";
	}

	auto setting = [&](const char *key, Value fallback) {
		Value value;
		auto result = db.TryGetCurrentSetting(Identifier(key), value);
		if (result && !value.IsNull()) {
			return value;
		}
		return fallback;
	};

	state->row.emplace_back(Value(string(DuckDB::LibraryVersion())));
	state->row.emplace_back(Value(connection.endpoint));
	state->row.emplace_back(Value(connection.token_source.empty() ? "none" : connection.token_source));
	state->row.emplace_back(Value(user));
	state->row.emplace_back(Value(fullname));
	state->row.emplace_back(Value(whoami_error));
	state->row.emplace_back(Value(transport));
	state->row.emplace_back(setting("hf_read_buffer_size", Value::UBIGINT(HF_DEFAULT_READ_BUFFER)));
	state->row.emplace_back(setting("hf_read_cache_size", Value::UBIGINT(HF_DEFAULT_READ_CACHE)));
	state->row.emplace_back(
	    setting("hf_read_max_parallel_requests", Value::UBIGINT(HF_DEFAULT_READ_MAX_PARALLEL)));
	state->row.emplace_back(
	    setting("hf_read_cache_total_size", Value::UBIGINT(HF_DEFAULT_READ_CACHE_TOTAL)));
	state->row.emplace_back(setting("hf_max_per_page", Value::UBIGINT(HF_DEFAULT_LIST_PAGE_SIZE)));
	state->row.emplace_back(setting("hf_create_repo", Value::BOOLEAN(false)));
	state->row.emplace_back(setting("hf_cache_dir", Value("")));
	return std::move(state);
}

static unique_ptr<FunctionData> HFStatusBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<Identifier> &names) {
	names.emplace_back("duckdb_version");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("endpoint");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("token_source");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("user");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("fullname");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("whoami_error");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("http_transport");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("hf_read_buffer_size");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("hf_read_cache_size");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("hf_read_max_parallel_requests");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("hf_read_cache_total_size");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("hf_max_per_page");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("hf_create_repo");
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("hf_cache_dir");
	return_types.push_back(LogicalType::VARCHAR);
	return make_uniq<TableFunctionData>();
}

static void HFStatusFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFStatusState>();
	if (state.done) {
		return;
	}
	state.done = true;
	for (idx_t col = 0; col < state.row.size(); col++) {
		output.data[col].SetValue(0, state.row[col]);
	}
	output.SetChildCardinality(1);
}

//===----------------------------------------------------------------------===//
// hf_read_stats
//===----------------------------------------------------------------------===//

struct HFReadStatsBindData : public TableFunctionData {
	bool reset = false;
};

struct HFReadStatsState : public GlobalTableFunctionState {
	vector<Value> row;
	bool done = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HFReadStatsBind(ClientContext &, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<HFReadStatsBindData>();
	bind_data->reset = NamedArgumentOr(input, "reset", false);

	names.emplace_back("files_opened");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("range_hits");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("coalesced_waits");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("window_fetches");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("part_requests");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("straddling_fetches");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("evictions");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("bytes_fetched");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("invalidations");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("retained_bytes");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("retained_files");
	return_types.push_back(LogicalType::UBIGINT);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HFReadStatsInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HFReadStatsBindData>();
	auto state = make_uniq<HFReadStatsState>();
	auto &counters = HFReadCountersGet();
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.files_opened.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.range_hits.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.coalesced_waits.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.window_fetches.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.part_requests.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.straddling_fetches.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.evictions.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.bytes_fetched.load())));
	state->row.emplace_back(Value::UBIGINT(NumericCast<idx_t>(counters.invalidations.load())));
	state->row.emplace_back(Value::UBIGINT(HFReadCacheRetainedBytes()));
	state->row.emplace_back(Value::UBIGINT(HFReadCacheRetainedFiles()));
	if (bind_data.reset) {
		HFReadCountersReset();
	}
	return std::move(state);
}

static void HFReadStatsFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFReadStatsState>();
	if (state.done) {
		return;
	}
	state.done = true;
	for (idx_t col = 0; col < state.row.size(); col++) {
		output.data[col].SetValue(0, state.row[col]);
	}
	output.SetChildCardinality(1);
}

//===----------------------------------------------------------------------===//
// hf_read_cache_clear
//===----------------------------------------------------------------------===//

struct HFReadCacheClearState : public GlobalTableFunctionState {
	vector<Value> row;
	bool done = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HFReadCacheClearBind(ClientContext &, TableFunctionBindInput &,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("cleared_bytes");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("cleared_files");
	return_types.push_back(LogicalType::UBIGINT);
	return make_uniq<TableFunctionData>();
}

static unique_ptr<GlobalTableFunctionState> HFReadCacheClearInit(ClientContext &, TableFunctionInitInput &) {
	auto state = make_uniq<HFReadCacheClearState>();
	// report what was held, then forget it: what a reader still holds on to is dropped with its window
	state->row.emplace_back(Value::UBIGINT(HFReadCacheRetainedBytes()));
	state->row.emplace_back(Value::UBIGINT(HFReadCacheRetainedFiles()));
	HFReadCacheClear();
	return std::move(state);
}

static void HFReadCacheClearFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFReadCacheClearState>();
	if (state.done) {
		return;
	}
	state.done = true;
	for (idx_t col = 0; col < state.row.size(); col++) {
		output.data[col].SetValue(0, state.row[col]);
	}
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
	auto message = NamedArgumentAsString(input, "message");
	if (message.empty()) {
		FileOpenerInfo info;
		info.file_path = bind_data->target.url.ToString();
		Value configured;
		if (FileOpener::TryGetCurrentSetting(&file_opener, Identifier("hf_commit_message"), configured, info) &&
		    !configured.IsNull()) {
			message = StringValue::Get(configured);
		}
	}
	if (message.empty()) {
		message = HFDefaultCommitMessage(bind_data->target.url);
	}
	bind_data->commit_message = std::move(message);

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
	// a keyword-only parameter without a default is mandatory in duckdb 2.0, so the empty string means "derive it"
	hf_upload.GetSignature().AddKeywordOnly("message", LogicalType::VARCHAR, Value(""));
	loader.RegisterFunction(hf_upload);

	TableFunction hf_list_revisions(Identifier("hf_list_revisions"), {LogicalType::VARCHAR}, HFRepoInfoFunction,
	                                HFRevisionsBind, HFRevisionsInit);
	hf_list_revisions.GetSignature().AddKeywordOnly("max_revisions", LogicalType::BIGINT, Value::BIGINT(20));
	loader.RegisterFunction(hf_list_revisions);

	TableFunction hf_list_refs(Identifier("hf_list_refs"), {LogicalType::VARCHAR}, HFRepoInfoFunction, HFRefsBind,
	                           HFRefsInit);
	loader.RegisterFunction(hf_list_refs);

	TableFunction hf_create_repo(Identifier("hf_create_repo"), {LogicalType::VARCHAR}, HFCreateRepoFunction,
	                             HFCreateRepoBind, HFOnceInit);
	hf_create_repo.GetSignature().AddKeywordOnly("private", LogicalType::BOOLEAN, Value::BOOLEAN(true));
	loader.RegisterFunction(hf_create_repo);

	TableFunction hf_status(Identifier("hf_status"), {}, HFStatusFunction, HFStatusBind, HFStatusInit);
	loader.RegisterFunction(hf_status);

	TableFunction hf_read_stats(Identifier("hf_read_stats"), {}, HFReadStatsFunction, HFReadStatsBind, HFReadStatsInit);
	hf_read_stats.GetSignature().AddKeywordOnly("reset", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	loader.RegisterFunction(hf_read_stats);

	TableFunction hf_read_cache_clear(Identifier("hf_read_cache_clear"), {}, HFReadCacheClearFunction,
	                                  HFReadCacheClearBind, HFReadCacheClearInit);
	loader.RegisterFunction(hf_read_cache_clear);

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
