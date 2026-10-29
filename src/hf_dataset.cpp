#include "hf_dataset.hpp"

#include "hf_fs.hpp"
#include "hf_http.hpp"
#include "hf_secrets.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <algorithm>

namespace duckdb {

//===----------------------------------------------------------------------===//
// The datasets-server's /parquet route
//===----------------------------------------------------------------------===//

string HFDatasetServerEndpoint(optional_ptr<FileOpener> opener) {
	if (opener) {
		Value value;
		FileOpenerInfo info;
		info.file_path = "hf://datasets/";
		if (FileOpener::TryGetCurrentSetting(opener, Identifier("hf_datasets_server_endpoint"), value, info) &&
		    !value.IsNull() && !StringValue::Get(value).empty()) {
			return StringValue::Get(value);
		}
	}
	return HF_DEFAULT_DATASETS_SERVER;
}

static string HFDatasetFilesUrl(const string &server, const HFDatasetRequest &request) {
	auto url = HFUtil::Join(server, "parquet");
	// the route filters what it is asked to; the answer is filtered again once it is parsed
	vector<pair<string, string>> params;
	params.emplace_back("dataset", request.dataset);
	if (!request.config.empty()) {
		params.emplace_back("config", request.config);
	}
	if (!request.split.empty()) {
		params.emplace_back("split", request.split);
	}
	if (!request.revision.empty()) {
		params.emplace_back("revision", request.revision);
	}
	for (idx_t i = 0; i < params.size(); i++) {
		url += (i == 0) ? "?" : "&";
		url += params[i].first + "=" + HFUrl::EncodePath(params[i].second);
	}
	return url;
}

//! Percent-decode a path fragment (`a%20b` -> `a b`). A stray `%` that does not start a hex pair is kept,
//! because refusing a whole dataset over one odd filename would be a bad trade.
static string HFUrlDecode(const string &value) {
	auto hex_value = [](char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	};
	string result;
	for (idx_t i = 0; i < value.size(); i++) {
		if (value[i] == '%' && i + 2 < value.size()) {
			auto high = hex_value(value[i + 1]);
			auto low = hex_value(value[i + 2]);
			if (high >= 0 && low >= 0) {
				result += static_cast<char>((high << 4) + low);
				i += 2;
				continue;
			}
		}
		result += value[i];
	}
	return result;
}

//! The `/parquet` route answers with `https://` urls:
//!
//!   https://huggingface.co/datasets/lhoestq/demo1/resolve/refs%2Fconvert%2Fparquet/default/train/0000.parquet
//!
//! Reading that through httpfs fetches it anonymously, which a gated or a private dataset refuses. The same
//! file behind an `hf://` url goes through this extension's read path - range requests, the shared cache -
//! and carries the token a `CREATE SECRET (TYPE HUGGINGFACE, …)` or `HF_TOKEN` provided:
//!
//!   hf://datasets/lhoestq/demo1@refs%2Fconvert%2Fparquet/default/train/0000.parquet
//!
//! The revision stays percent-encoded because that is what the Hub serves: the same url with a plain
//! `refs/convert/parquet` answers 404 (checked against the live Hub, not inferred from the format).
//!
//! A url that is not a Hub `/resolve/` url comes back unchanged: a dataset's files can live anywhere, and
//! `read_parquet` reads an https url over httpfs - unauthenticated, which is all such a url can promise.
static string HFDatasetToHFUrl(const string &url) {
	auto scheme = url.find("://");
	if (scheme == string::npos) {
		return url;
	}
	auto path_start = url.find('/', scheme + 3);
	if (path_start == string::npos) {
		return url;
	}
	static const std::string RESOLVE = "/resolve/";
	auto path = url.substr(path_start);
	auto resolve = path.find(RESOLVE);
	if (resolve == string::npos) {
		return url;
	}
	// `<repo_type>/<namespace>/<name>`, the form the Hub uses for everything but models. Only that form is
	// unambiguous - `hf://` cannot tell a model repo from a path otherwise - and it is all this route returns
	auto repo_part = path.substr(1, resolve - 1);
	auto type_end = repo_part.find('/');
	if (type_end == string::npos || !HFRepoType::IsValid(repo_part.substr(0, type_end))) {
		return url;
	}
	auto repository = repo_part.substr(type_end + 1);
	auto rest = path.substr(resolve + RESOLVE.size());
	// the revision runs to the first *unescaped* slash: inside a branch name the Hub escapes it as %2F, and
	// the branch here (`refs/convert/parquet`) always arrives that way
	idx_t revision_end = 0;
	while (revision_end < rest.size()) {
		if (rest[revision_end] == '/') {
			break;
		}
		if (rest[revision_end] == '%' && revision_end + 2 < rest.size()) {
			revision_end += 3;
			continue;
		}
		revision_end++;
	}
	if (revision_end == 0 || revision_end == rest.size()) {
		return url; // nothing but a revision: there is no file in there to read
	}
	auto result = "hf://" + repo_part.substr(0, type_end) + "/" + repository + "@" + rest.substr(0, revision_end) +
	              HFUrlDecode(rest.substr(revision_end));
	HFUrl::Parse(result); // parsed here so a url this cannot make sense of fails with this extension's message
	return result;
}

static void AddUnique(vector<string> &list, const string &value) {
	if (value.empty() || std::find(list.begin(), list.end(), value) != list.end()) {
		return;
	}
	list.push_back(value);
}

static string JoinList(const vector<string> &list, const string &separator) {
	string result;
	for (idx_t i = 0; i < list.size(); i++) {
		if (i > 0) {
			result += separator;
		}
		result += list[i];
	}
	return result;
}

static idx_t CountJSONObjects(const string &array) {
	idx_t count = 0;
	HFJson::ForEachObject(array, [&](const string &) { count++; });
	return count;
}

//! What the answer says about itself, plus the files the selection kept
static void HFParseDatasetParquet(const string &body, const HFDatasetRequest &request, HFDatasetFiles &result) {
	bool found = false;
	auto error = HFJson::GetString(body, "error", found);
	if (found && !error.empty()) {
		// the route answers 200 for some of its refusals, where a status code would have said it
		throw IOException("The hub's datasets-server refused '%s': %s", request.dataset, error);
	}
	bool partial_found = false;
	result.partial = HFJson::AsBool(HFJson::GetRaw(body, "partial", partial_found)) && partial_found;
	result.pending = CountJSONObjects(HFJson::FindArray(body, "pending"));
	result.failed = CountJSONObjects(HFJson::FindArray(body, "failed"));

	HFJson::ForEachObject(HFJson::FindArray(body, "parquet_files"), [&](const string &object) {
		HFDatasetFile file;
		file.url = HFJson::GetString(object, "url", found);
		if (file.url.empty()) {
			return;
		}
		file.filename = HFJson::GetString(object, "filename", found);
		file.config = HFJson::GetString(object, "config", found);
		file.split = HFJson::GetString(object, "split", found);
		file.size = NumericCast<idx_t>(HFJson::AsInt(HFJson::GetRaw(object, "size", found)));
		AddUnique(result.configs, file.config);
		AddUnique(result.splits, file.split);
		// The route filters, but only for a `config` it recognises: a `split` it does not know is answered
		// with every split of the config (measured against the production server), so the selection is applied
		// again here rather than trusted
		if (!request.config.empty() && file.config != request.config) {
			return;
		}
		if (!request.split.empty() && file.split != request.split) {
			return;
		}
		file.url = HFDatasetToHFUrl(file.url);
		result.files.push_back(std::move(file));
	});
}

//! An empty selection is nearly always one name spelled wrong, and the answer already holds the other
//! name's alternatives - so it says which of the two is wrong, and what it could have been
static string HFDatasetSelectionHint(const HFDatasetRequest &request, const HFDatasetFiles &available) {
	if (!request.config.empty() && !available.configs.empty()) {
		return StringUtil::Format(" - the dataset has no config named '%s', its configs are %s", request.config,
		                          JoinList(available.configs, ", "));
	}
	if (!request.split.empty() && !available.splits.empty()) {
		return StringUtil::Format(" - the dataset has no split named '%s', its splits are %s", request.split,
		                          JoinList(available.splits, ", "));
	}
	return " - the hub has not converted it to parquet";
}

HFDatasetFiles HFLoadDatasetParquetFiles(DatabaseInstance &db, optional_ptr<FileOpener> opener,
                                         const HFDatasetRequest &request) {
	HFDatasetFiles result;
	// the dataset's own url, because that is what the endpoint and the token are resolved *for*: a secret
	// scoped to `hf://datasets/my-org/` has to cover a listing of my-org's files as well as reading them
	auto dataset_url = string("hf://datasets/") + request.dataset;
	if (!request.revision.empty()) {
		dataset_url += "@" + request.revision;
	}
	auto target = HFResolveTarget(dataset_url, opener, db);
	auto client = HFMakeClient(db, opener, target);
	auto server = HFDatasetServerEndpoint(opener);
	try {
		HFParseDatasetParquet(client->Get(HFDatasetFilesUrl(server, request)), request, result);
	} catch (IOException &error) {
		// Only an answer is worth asking twice about. A timeout retried without the filters would blame the
		// config for the network, so the retry happens for the case it can actually resolve: `HTTP 404` and
		// `HTTP 401` are what this extension's own client writes when the service answered, and a `config`
		// the dataset does not have is refused with the same bare "Not found." as a dataset that does not exist.
		auto refused_by_the_service = StringUtil::Contains(error.what(), "HTTP 404") ||
		                                StringUtil::Contains(error.what(), "HTTP 401");
		if (!refused_by_the_service || (request.config.empty() && request.split.empty())) {
			throw;
		}
		// The route answers a `config` it does not know with the same 404 it answers for a dataset that does
		// not exist, and "Not found." is a poor answer to a typo. Ask once for the whole dataset to tell the
		// two apart: refused again, whatever the first answer said is about the dataset itself and stands;
		// answered, the name that was passed is the problem and the answer holds the alternatives.
		auto everything = request;
		everything.config.clear();
		everything.split.clear();
		HFDatasetFiles available;
		HFParseDatasetParquet(client->Get(HFDatasetFilesUrl(server, everything)), everything, available);
		throw IOException("No parquet files to read for dataset '%s'%s", request.dataset,
		                  HFDatasetSelectionHint(request, available));
	}
	if (result.files.empty()) {
		// Three states answer as an empty list, and one of them is worth waiting for. A conversion that is
		// still queued says so - `pending` filled in, `partial` still false, measured on a dataset created for
		// the purpose - which asks for a retry rather than for a different dataset name.
		if (result.pending > 0) {
			throw IOException("Dataset '%s' is not ready to read: the hub has %llu conversion step(s) queued for "
			                  "it and no parquet file yet - retry in a moment",
			                  request.dataset, result.pending);
		}
		if (result.failed > 0) {
			throw IOException("Dataset '%s' has no readable parquet files: the hub failed %llu conversion step(s) "
			                  "for it",
			                  request.dataset, result.failed);
		}
		throw IOException("No parquet files to read for dataset '%s'%s", request.dataset,
		                  HFDatasetSelectionHint(request, result));
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Arguments
//===----------------------------------------------------------------------===//

static string NamedArgumentAsString(TableFunctionBindInput &input, const char *key) {
	auto entry = input.named_parameters.find(Identifier(key));
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return "";
	}
	return StringValue::Get(entry->second);
}

static bool NamedArgumentAsBool(TableFunctionBindInput &input, const char *key, bool fallback) {
	auto entry = input.named_parameters.find(Identifier(key));
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return fallback;
	}
	return entry->second.GetValue<bool>();
}

//! Accepts `lhoestq/demo1`, `lhoestq/demo1@<revision>`, and the same written as an `hf://` url - which is
//! what a caller who has already looked at the repository has in their clipboard.
static HFDatasetRequest HFDatasetRequestFromInput(TableFunctionBindInput &input, const char *function_name) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("%s requires a dataset name, for example %s('lhoestq/demo1')", function_name,
		                      function_name);
	}
	HFDatasetRequest request;
	auto written = StringValue::Get(input.inputs[0]);
	if (StringUtil::StartsWith(written, "hf://")) {
		auto url = HFUrl::Parse(written);
		if (url.repo_type != HFRepoType::DATASETS) {
			throw BinderException("%s reads a dataset - '%s' names a repository of type '%s'", function_name, written,
			                      url.repo_type);
		}
		if (!url.path.empty() && url.path != "/") {
			throw BinderException("%s takes a dataset, not a path inside one - '%s' names '%s' inside it, "
			                      "select the split instead",
			                      function_name, written, url.path);
		}
		request.dataset = url.repository;
		// an `hf://` url with no @revision parses as `main`, which is not "a revision was given"; asking the
		// conversion server without one means the same thing and cannot be wrong about a dataset whose files
		// really do live on a branch called main
		if (url.revision != "main") {
			request.revision = url.revision;
		}
	} else {
		auto body = written;
		// a hub dataset page is the other form worth accepting from a clipboard. Only the name is taken from
		// it: whatever follows (`/tree/<revision>/...`, `/resolve/...`) is a browse path, and the revision it
		// spells is not a revision to read a dataset's converted files from
		if (StringUtil::StartsWith(body, "http://") || StringUtil::StartsWith(body, "https://")) {
			static const std::string DATASETS = "/datasets/";
			auto marker = body.find(DATASETS);
			if (marker == string::npos) {
				throw BinderException("%s wants a dataset, given the url '%s' - pass it as 'namespace/name', "
				                      "like 'lhoestq/demo1'",
				                      function_name, written);
			}
			body = body.substr(marker + DATASETS.size());
			auto query = body.find_first_of("?#");
			if (query != string::npos) {
				body = body.substr(0, query);
			}
			// keep the two segments that make a repository id, drop the rest of the path
			auto first = body.find('/');
			if (first != string::npos) {
				auto second = body.find('/', first + 1);
				body = second == string::npos ? body : body.substr(0, second);
			}
		}
		auto at = body.find('@');
		if (at != string::npos) {
			request.revision = body.substr(at + 1);
			body = body.substr(0, at);
		}
		while (!body.empty() && body.back() == '/') {
			body.pop_back();
		}
		request.dataset = body;
	}
	if (request.dataset.find('/') == string::npos || request.dataset.size() < 3 ||
	    request.dataset.front() == '/' || request.dataset.back() == '/') {
		throw BinderException("%s wants a dataset named '<namespace>/<name>', like %s('lhoestq/demo1') - got '%s'",
		                      function_name, function_name, written);
	}
	request.config = NamedArgumentAsString(input, "config");
	request.split = NamedArgumentAsString(input, "split");
	auto named_revision = NamedArgumentAsString(input, "revision");
	if (!named_revision.empty()) {
		request.revision = named_revision;
	}
	return request;
}

//! The configs of a selection, in the order the server listed them
static vector<string> DistinctConfigs(const HFDatasetFiles &files) {
	vector<string> configs;
	for (auto &file : files.files) {
		AddUnique(configs, file.config);
	}
	return configs;
}

//===----------------------------------------------------------------------===//
// hf_dataset_files
//===----------------------------------------------------------------------===//

struct HFDatasetFilesBindData : public TableFunctionData {
	HFDatasetRequest request;
};

struct HFDatasetFilesState : public GlobalTableFunctionState {
	HFDatasetFiles files;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HFDatasetFilesBind(ClientContext &, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<HFDatasetFilesBindData>();
	bind_data->request = HFDatasetRequestFromInput(input, "hf_dataset_files");

	names.emplace_back("dataset");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("config");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("split");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("filename");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("size");
	return_types.push_back(LogicalType::UBIGINT);
	//! Repeated on every row, like hf_xet_upload's file-level columns: it describes the answer, not the file
	names.emplace_back("partial");
	return_types.push_back(LogicalType::BOOLEAN);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HFDatasetFilesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto state = make_uniq<HFDatasetFilesState>();
	// the request goes out here rather than at bind time: a listing is still a listing when it is only
	// explained, and EXPLAIN should not cost a round trip
	state->files = HFLoadDatasetParquetFiles(db, &file_opener, input.bind_data->Cast<HFDatasetFilesBindData>().request);
	return std::move(state);
}

static void HFDatasetFilesFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<HFDatasetFilesState>();
	auto &bind_data = data.bind_data->Cast<HFDatasetFilesBindData>();
	idx_t count = 0;
	while (state.offset < state.files.files.size() && count < STANDARD_VECTOR_SIZE) {
		auto &file = state.files.files[state.offset++];
		output.data[0].SetValue(count, Value(bind_data.request.dataset));
		output.data[1].SetValue(count, Value(std::move(file.config)));
		output.data[2].SetValue(count, Value(std::move(file.split)));
		output.data[3].SetValue(count, Value(std::move(file.url)));
		output.data[4].SetValue(count, Value(std::move(file.filename)));
		output.data[5].SetValue(count, Value::UBIGINT(file.size));
		output.data[6].SetValue(count, Value::BOOLEAN(state.files.partial));
		count++;
	}
	output.SetChildCardinality(count_t(count));
}

//===----------------------------------------------------------------------===//
// hf_dataset
//===----------------------------------------------------------------------===//

static unique_ptr<TableRef> HFDatasetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto request = HFDatasetRequestFromInput(input, "hf_dataset");
	auto allow_partial = NamedArgumentAsBool(input, "allow_partial", false);
	auto &db = DatabaseInstance::GetDatabase(context);
	ClientContextFileOpener file_opener(context);
	auto files = HFLoadDatasetParquetFiles(db, &file_opener, request);

	if (files.partial && !allow_partial) {
		throw IOException(
		    "The hub has converted only part of dataset '%s' to parquet (%llu files ready, %llu still pending, "
		    "%llu failed) - hf_dataset refuses to answer for less than the whole dataset, pass "
		    "allow_partial := true to read what is converted",
		    request.dataset, files.files.size(), files.pending, files.failed);
	}
	if (request.config.empty()) {
		// two configs are two different schemas. Refusing is the honest answer: reading them together would
		// be a union by name over columns that were never the same column
		auto configs = DistinctConfigs(files);
		if (configs.size() > 1) {
			throw BinderException("Dataset '%s' has %llu configs (%s) and hf_dataset reads one of them - pass one "
			                      "as config := '%s'",
			                      request.dataset, configs.size(), JoinList(configs, ", "), configs[0]);
		}
	}

	auto list = Value::LIST(LogicalType::VARCHAR, [&]() {
		vector<Value> urls;
		urls.reserve(files.files.size());
		for (auto &file : files.files) {
			urls.emplace_back(file.url);
		}
		return urls;
	}());

	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(ConstantExpression::FromValue(std::move(list)));
	// Named arguments of a table function ref are a comparison against a column reference that binds to
	// nothing - the same shape DuckDB's own relational API builds for `read_parquet([...], union_by_name := …)`
	children.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(Identifier("union_by_name")),
	    ConstantExpression::Boolean(true)));

	auto function = make_uniq<FunctionExpression>(Identifier("read_parquet"), std::move(children));
	auto ref = make_uniq<TableFunctionRef>();
	ref->function = std::move(function);
	return std::move(ref);
}

void HFDatasetFunctions::Register(ExtensionLoader &loader) {
	TableFunction hf_dataset(Identifier("hf_dataset"), {LogicalType::VARCHAR}, nullptr, nullptr);
	// the whole function is a bind-time rewrite into read_parquet([...]), so there is no bind or function
	// to register: bind_replace answers with a different table reference and the planner never sees hf_dataset
	hf_dataset.bind_replace = HFDatasetBindReplace;
	// a keyword-only parameter without a default is mandatory in duckdb 2.0, so NULL means "not given"
	hf_dataset.GetSignature().AddKeywordOnly("config", LogicalType::VARCHAR, Value());
	hf_dataset.GetSignature().AddKeywordOnly("split", LogicalType::VARCHAR, Value());
	hf_dataset.GetSignature().AddKeywordOnly("revision", LogicalType::VARCHAR, Value());
	hf_dataset.GetSignature().AddKeywordOnly("allow_partial", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	loader.RegisterFunction(hf_dataset);

	TableFunction hf_dataset_files(Identifier("hf_dataset_files"), {LogicalType::VARCHAR}, HFDatasetFilesFunction,
	                               HFDatasetFilesBind, HFDatasetFilesInit);
	hf_dataset_files.GetSignature().AddKeywordOnly("config", LogicalType::VARCHAR, Value());
	hf_dataset_files.GetSignature().AddKeywordOnly("split", LogicalType::VARCHAR, Value());
	hf_dataset_files.GetSignature().AddKeywordOnly("revision", LogicalType::VARCHAR, Value());
	loader.RegisterFunction(hf_dataset_files);
}

} // namespace duckdb
