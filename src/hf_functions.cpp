#include "hf_fs.hpp"

#include "hf_crypto.hpp"
#include "hf_functions.hpp"
#include "hf_settings.hpp"
#include "hf_xet_hash.hpp"
#include "hf_xet_xorb.hpp"

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
		for (auto &file : fs.Glob(bind_data.target.url.ToString(), nullptr)) {
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

	// looked up through the context, not the database: `SET hf_create_repo = true` is session scoped and
	// a database-level lookup would keep reporting false, contradicting what duckdb_settings() shows
	auto setting = [&](const char *key, Value fallback) {
		Value value;
		auto result = context.TryGetCurrentSetting(Identifier(key), value);
		if (result && !value.IsNull()) {
			return value;
		}
		return fallback;
	};

	state->row.emplace_back(Value(string(HF_EXTENSION_VERSION)));
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
	names.emplace_back("hf_version");
	return_types.push_back(LogicalType::VARCHAR);
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
	HFUploadLocalFile(db, &file_opener, bind_data.target, bind_data.local_path, blob, bind_data.commit_message,
	                  HFSettingAsBool(&file_opener, "hf_create_repo", false));

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
	bool missing_ok = false; //!< tolerate paths that are not there, so cleanup scripts can be idempotent
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
	bind_data->missing_ok = NamedArgumentOr(input, "missing_ok", false);

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
		vector<string> urls; //!< reported once the commit behind them actually went through
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
			group.urls.push_back(target.url.ToString());
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
			if (bind_data.missing_ok) {
				continue; // cleanup scripts ask for paths that may not be there
			}
			throw IOException("Nothing to delete at '%s' - no file or folder exists there", target.url.ToString());
		}
		if (children.empty()) {
			if (bind_data.missing_ok) {
				continue;
			}
			throw IOException("Nothing to delete at '%s' - the folder is empty", target.url.ToString());
		}
		for (auto &child : children) {
			group.files.push_back("/" + child);
			group.urls.push_back(target.url.WithPath("/" + child).ToString());
		}
	}
	for (auto &entry : groups) {
		try {
			HFDeletePaths(db, &file_opener, entry.second.target, entry.second.files, entry.second.folders,
			              "Delete with DuckDB");
		} catch (std::exception &) {
			if (!bind_data.missing_ok) {
				throw;
			} // a repository that is already gone, a revision that is read only: nothing was deleted, carry on
			continue;
		}
		for (auto &url : entry.second.urls) {
			state->deleted.push_back(url);
		}
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
// hf_bucket_copy
//===----------------------------------------------------------------------===//

struct HFBucketCopyBindData : public TableFunctionData {
	HFTarget source;
	HFTarget bucket;
	//! how long to wait for the hub to finish hashing a freshly committed source file
	int64_t hash_wait_ms = HF_COPY_HASH_WAIT_MS;
};

struct HFCopyRow {
	string source;
	string url;
	string path;
	idx_t size = 0;
	string xet_hash;
	string status;
};

struct HFBucketCopyState : public GlobalTableFunctionState {
	vector<HFCopyRow> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HFBucketCopyBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.size() < 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("hf_bucket_copy requires a source hf:// path and a bucket hf:// path, for example "
		                      "hf_bucket_copy('hf://datasets/user/repo/data.parquet', "
		                      "'hf://buckets/user/bucket/data.parquet')");
	}
	ClientContextFileOpener file_opener(context);
	auto &db = DatabaseInstance::GetDatabase(context);
	auto bind_data = make_uniq<HFBucketCopyBindData>();
	bind_data->source = HFResolveTarget(StringValue::Get(input.inputs[0]), &file_opener, db);
	bind_data->bucket = HFResolveTarget(StringValue::Get(input.inputs[1]), &file_opener, db);
	if (!bind_data->bucket.url.IsBucket()) {
		throw BinderException("hf_bucket_copy writes into a bucket - '%s' is a %s, use 'hf://buckets/user/bucket/...' "
		                      "as destination",
		                      StringValue::Get(input.inputs[1]), bind_data->bucket.url.repo_type);
	}
	auto hash_wait_seconds = NamedArgumentOr(input, "hash_wait", HF_COPY_HASH_WAIT_MS / 1000);
	if (hash_wait_seconds < 0) {
		throw BinderException("hf_bucket_copy's hash_wait takes a non-negative number of seconds, got %lld",
		                      hash_wait_seconds);
	}
	bind_data->hash_wait_ms = hash_wait_seconds * 1000;
	if (bind_data->source.url.endpoint != bind_data->bucket.url.endpoint) {
		throw BinderException("hf_bucket_copy copies inside one Hub - '%s' and '%s' are on different endpoints",
	                          bind_data->source.url.ToString(), bind_data->bucket.url.ToString());
	}

	names.emplace_back("source");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("path");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("size");
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("xet_hash");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HFBucketCopyInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto &bind_data = input.bind_data->Cast<HFBucketCopyBindData>();
	auto state = make_uniq<HFBucketCopyState>();

	// what is being copied: one file, or everything below a prefix (a folder, or a glob)
	const auto strip = [](string path) {
		while (!path.empty() && path.front() == '/') {
			path = path.substr(1);
		}
		while (!path.empty() && path.back() == '/') {
			path.pop_back();
		}
		return path;
	};
	auto source_path = strip(bind_data.source.url.path);
	vector<string> paths;
	string base; //!< the prefix the mirrored structure is relative to (empty = a single file)
	if (!source_path.empty()) {
		// is the source one file? the hub answers with an entry per path it actually has
		if (!HFXetHashes(db, &file_opener, bind_data.source, vector<string> {source_path}).empty()) {
			paths = {source_path};
		}
	}
	if (paths.empty()) {
		// not a file: take everything below it. `**` is the one glob the Hub tree listing serves in one call
		auto glob_url = bind_data.source.url;
		glob_url.path = source_path.empty() ? "/**" : "/" + source_path + "/**";
		if (HFUtil::HasGlob(source_path)) {
			// the caller already wrote a pattern, keep it and mirror relative to its fixed part
			glob_url.path = "/" + source_path;
			base = source_path.substr(0, source_path.find('*'));
			while (!base.empty() && base.back() == '/') {
				base.pop_back();
			}
		} else {
			base = source_path;
		}
		auto &fs = FileSystem::GetFileSystem(context);
		// the opener is pushed automatically by the wrapper filesystem - passing one here is an internal error
		for (auto &found : fs.Glob(glob_url.ToString(), nullptr)) {
			auto found_path = strip(HFResolveTarget(found.path, &file_opener, db).url.path);
			if (found_path.empty() || found_path == ".gitattributes" ||
			    found_path == base + "/.gitattributes") { // git bookkeeping, not data
				continue;
			}
			paths.push_back(std::move(found_path));
		}
	}
	if (paths.empty()) {
		throw IOException("Nothing found at '%s' to copy into '%s'", bind_data.source.url.ToString(),
		                  bind_data.bucket.url.ToString());
	}

	// the hub answers with the content hash of every file it has, in batches
	map<string, HFXetFile> hashes;
 unordered_set<string> pending_hash(paths.begin(), paths.end());
	const auto fetch_hashes = [&](const vector<string> &wanted) {
		for (idx_t from = 0; from < wanted.size(); from += 100) {
			vector<string> batch;
			for (idx_t i = from; i < MinValue<idx_t>(from + 100, wanted.size()); i++) {
				batch.push_back(wanted[i]);
			}
			for (auto &file : HFXetHashes(db, &file_opener, bind_data.source, batch)) {
				hashes[file.path] = file;
			}
		}
	};
	fetch_hashes(paths);
	// what is still missing an xetHash decides whether any waiting happens at all, so prune before the
	// first sleep rather than after it - a copy of already hashed files should not pause
	const auto prune_pending = [&]() {
		for (auto it = pending_hash.begin(); it != pending_hash.end();) {
			auto found = hashes.find(*it);
			if (found != hashes.end() && !found->second.xet_hash.empty()) {
				it = pending_hash.erase(it);
			} else {
				++it;
			}
		}
	};
	prune_pending();
	// A file committed a moment ago is hashed by the hub in its own time. Waiting is the difference
	// between "copied" and a confusing refusal for content the user wrote seconds earlier, so ask again,
	// backing off, until the budget runs out - files genuinely outside Xet are then reported skipped.
	idx_t waited_ms = 0;
	idx_t backoff = HF_COPY_HASH_BACKOFF;
	while (!pending_hash.empty() && int64_t(waited_ms) < bind_data.hash_wait_ms) {
		auto remaining = idx_t(bind_data.hash_wait_ms) - waited_ms;
		auto pause = MinValue<idx_t>(backoff, remaining);
		HFClient::SleepMs(pause);
		waited_ms += pause;
		backoff = MinValue<idx_t>(backoff * 2, HF_COPY_HASH_MAX_BACKOFF);
		vector<string> missing;
		for (auto &path : pending_hash) {
			missing.push_back(path);
		}
		fetch_hashes(missing);
		prune_pending();
	}

	string dest_prefix = strip(bind_data.bucket.url.path);
	// one file into a destination that is not a directory lands on exactly that name; anything else is mirrored
	// below the destination as a prefix. Whether the destination is a directory is decided the way the hub's own
	// client decides it: an existing file wins, then an existing prefix, then a trailing slash.
	const bool single_file = !source_path.empty() && paths.size() == 1 && paths[0] == source_path;
	bool dest_is_dir = !single_file || dest_prefix.empty() || bind_data.bucket.url.path.back() == '/';
	if (!dest_is_dir) {
		// an existing file with that name wins, otherwise anything living under that prefix makes it a directory
		if (HFXetHashes(db, &file_opener, bind_data.bucket, vector<string> {dest_prefix}).empty()) {
			auto listing = bind_data.bucket;
			listing.url.path = "/" + dest_prefix;
			HFListEntries(
			    db, &file_opener, listing, false,
			    [&](const HFListEntry &entry) {
				    auto found = strip(entry.path);
				    if (found == dest_prefix || StringUtil::StartsWith(found, dest_prefix + "/")) {
					    dest_is_dir = true;
				    }
				},
			    false, true);
		}
	}
	if (dest_is_dir && !dest_prefix.empty()) {
		dest_prefix += "/";
	}
	const auto basename = [](const string &path) {
		auto slash = path.rfind('/');
		return slash == string::npos ? path : path.substr(slash + 1);
	};

	vector<HFBucketCopyOp> operations;
	for (auto &path : paths) {
		string relative;
		if (base.empty()) {
			relative = basename(path);
		} else if (path.size() > base.size() + 1 && path[base.size()] == '/') {
			relative = path.substr(base.size() + 1);
		} else {
			relative = basename(path);
		}
		HFCopyRow row;
		row.source = bind_data.source.url.WithPath("/" + path).ToString();
		row.path = dest_is_dir ? dest_prefix + relative : dest_prefix;
		row.url = bind_data.bucket.url.WithPath("/" + row.path).ToString();
		auto found = hashes.find(path);
		if (found == hashes.end() || found->second.xet_hash.empty()) {
			row.size = found == hashes.end() ? 0 : found->second.size;
			row.status = "skipped: the hub reports no Xet hash for this file, so a bucket cannot point at it";
		} else {
			HFBucketCopyOp op;
			op.source_repo_type = HFRepoType::ToSingular(bind_data.source.url.repo_type);
			op.source_repo_id = bind_data.source.url.repository;
			op.xet_hash = found->second.xet_hash;
			op.path = row.path;
			op.size = found->second.size;
			op.source_path = path;
			operations.push_back(std::move(op));
			row.size = found->second.size;
			row.xet_hash = found->second.xet_hash;
			row.status = "copied";
		}
		state->rows.push_back(std::move(row));
	}
	HFCopyIntoBucket(db, &file_opener, bind_data.bucket, operations);
	return std::move(state);
}

static void HFBucketCopyFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFBucketCopyState>();
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		output.data[0].SetValue(count, Value(std::move(row.source)));
		output.data[1].SetValue(count, Value(std::move(row.url)));
		output.data[2].SetValue(count, Value(std::move(row.path)));
		output.data[3].SetValue(count, Value::UBIGINT(row.size));
		output.data[4].SetValue(count, Value(std::move(row.xet_hash)));
		output.data[5].SetValue(count, Value(std::move(row.status)));
		count++;
	}
	output.SetChildCardinality(count_t(count));
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// hf_xet_selftest()
//===----------------------------------------------------------------------===//

struct HFXetSelfTestBindData : public TableFunctionData {
	idx_t failures = 0;
	vector<std::tuple<string, bool, string, string>> results;
};

struct HFXetSelfTestState : public GlobalTableFunctionState {
	idx_t cursor = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> HFXetSelfTestInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<HFXetSelfTestState>();
}

static unique_ptr<FunctionData> HFXetSelfTestBind(ClientContext &, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<HFXetSelfTestBindData>();
	// The checks are pure computation over reference vectors compiled into the extension, so they run
	// at bind time with no connection, token, or network.
	bind_data->failures = HFXetHashSelfTest(bind_data->results);
	bind_data->failures += HFXetXorbSelfTest(bind_data->results);

	names.push_back(Identifier("check_name"));
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back(Identifier("assert_passed"));
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back(Identifier("expected"));
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back(Identifier("got"));
	return_types.push_back(LogicalType::VARCHAR);
	// repeated on every row so a suite can assert on the whole run without a wrapper query
	names.push_back(Identifier("assert_no_failures"));
	return_types.push_back(LogicalType::BOOLEAN);
	return std::move(bind_data);
}

static void HFXetSelfTestFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<HFXetSelfTestBindData>();
	auto &state = data.global_state->Cast<HFXetSelfTestState>();
	idx_t count = 0;
	while (state.cursor < bind_data.results.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = bind_data.results[state.cursor++];
		output.SetValue(0, count, Value(std::get<0>(entry)));
		output.SetValue(1, count, Value(std::get<1>(entry)));
		output.SetValue(2, count, Value(std::get<2>(entry)));
		output.SetValue(3, count, Value(std::get<3>(entry)));
		output.SetValue(4, count, Value(bind_data.failures == 0));
		count++;
	}
	output.SetCardinality(count);
}

void HFFunctions::Register(ExtensionLoader &loader) {
	TableFunction hf_ls(Identifier("hf_ls"), {LogicalType::VARCHAR}, HFListFunction, HFListBind, HFListInit);
	hf_ls.GetSignature().AddKeywordOnly("recursive", LogicalType::BOOLEAN, Value(false));
	loader.RegisterFunction(hf_ls);

	TableFunction hf_meta(Identifier("hf_meta"), {LogicalType::VARCHAR}, HFMetaFunction, HFMetaBind, HFOnceInit);
	loader.RegisterFunction(hf_meta);

	TableFunction hf_bucket_copy(Identifier("hf_bucket_copy"), {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                             HFBucketCopyFunction, HFBucketCopyBind, HFBucketCopyInit);
	// how long a copy of a just-committed file waits for the hub to finish hashing it
	hf_bucket_copy.GetSignature().AddKeywordOnly("hash_wait", LogicalType::BIGINT, Value::BIGINT(30));
	loader.RegisterFunction(hf_bucket_copy);

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

	TableFunction hf_xet_selftest(Identifier("hf_xet_selftest"), {}, HFXetSelfTestFunction, HFXetSelfTestBind,
	                              HFXetSelfTestInit);
	loader.RegisterFunction(hf_xet_selftest);

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
	hf_delete_str.GetSignature().AddKeywordOnly("missing_ok", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	hf_delete_list.GetSignature().AddKeywordOnly("missing_ok", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	hf_delete.AddFunction(hf_delete_str);
	hf_delete.AddFunction(hf_delete_list);
	loader.RegisterFunction(std::move(hf_delete));
}

} // namespace duckdb
