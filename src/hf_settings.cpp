#include "hf_settings.hpp"

#include "hf_common.hpp"

#include "duckdb/main/config.hpp"

#include "duckdb/common/file_opener.hpp"

namespace duckdb {

bool HFSettingAsBool(optional_ptr<FileOpener> opener, const char *key, bool fallback) {
	if (!opener) {
		return fallback;
	}
	// settings have to be read while the opener is still alive: see HFWriteHandle, which caches them for the same
	// reason, and HFCommit/HFUploadLocalFile, which take the resolved flag rather than looking it up later
	Value result;
	FileOpenerInfo info;
	if (!FileOpener::TryGetCurrentSetting(opener, Identifier(key), result, info) || result.IsNull()) {
		return fallback;
	}
	return result.GetValue<bool>();
}

static void RequireGlobalScope(SetScope scope, const char *setting) {
	if (scope == SetScope::SESSION) {
		throw InvalidInputException("%s can only be set globally", setting);
	}
}

static void SetHFEndpoint(ClientContext &, SetScope scope, Value &parameter) {
	RequireGlobalScope(scope, "hf_endpoint");
	if (parameter.IsNull()) {
		parameter = Value(HF_DEFAULT_ENDPOINT);
		return;
	}
	auto value = StringUtil::Lower(StringValue::Get(parameter));
	if (!value.empty() && value.find("://") == string::npos) {
		if (value == "localhost" || value.find("localhost:") == 0 || value == "127.0.0.1" ||
		    value.find("127.0.0.1:") == 0) {
			value = "http://" + value;
		} else {
			value = "https://" + value;
		}
	}
	while (!value.empty() && value.back() == '/') {
		value.pop_back();
	}
	parameter = Value(value);
}

static void SetHFCacheDir(ClientContext &, SetScope scope, Value &parameter) {
	RequireGlobalScope(scope, "hf_cache_dir");
	if (parameter.IsNull()) {
		parameter = Value("");
		return;
	}
	parameter = Value(HFUtil::ExpandPath(StringValue::Get(parameter)));
}

void HFSettings::Register(DBConfig &config) {
	config.AddExtensionOption("hf_token", "Hugging Face API token (defaults to HF_TOKEN or ~/.cache/huggingface/token)",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("hf_endpoint", "Hugging Face endpoint to use", LogicalType::VARCHAR,
	                          Value(HF_DEFAULT_ENDPOINT), SetHFEndpoint, SetScope::GLOBAL);
	config.AddExtensionOption("hf_cache_dir", "Directory used to buffer files before uploading them to the Hub",
	                          LogicalType::VARCHAR, Value(""), SetHFCacheDir, SetScope::GLOBAL);
	config.AddExtensionOption("hf_max_per_page", "Limit the number of entries fetched per listing request",
	                          LogicalType::UBIGINT, Value::UBIGINT(0));
	config.AddExtensionOption("hf_commit_message", "Commit message used for writes to the Hub", LogicalType::VARCHAR,
	                          Value("Upload files with DuckDB"));
	config.AddExtensionOption("hf_create_repo", "Create the target repository if it does not exist yet",
	                          LogicalType::BOOLEAN, Value(false));
	config.AddExtensionOption("hf_xet_write",
	                          "Upload files through Xet content-defined chunking (deduplicated, incremental); "
	                          "falls back to a plain LFS upload when the content store cannot be reached",
	                          LogicalType::BOOLEAN, Value(true));
	config.AddExtensionOption("hf_read_buffer_size", "Read-ahead buffer size (in bytes) for hf:// reads",
	                          LogicalType::UBIGINT, Value::UBIGINT(HF_DEFAULT_READ_BUFFER));
	config.AddExtensionOption("hf_read_cache_size", "How many fetched bytes a single hf:// file may keep in memory (shared by all readers of that file)",
	                          LogicalType::UBIGINT, Value::UBIGINT(HF_DEFAULT_READ_CACHE));
	config.AddExtensionOption("hf_read_max_parallel_requests",
	                          "How many range requests one hf:// read-ahead window is fetched with in parallel",
	                          LogicalType::UBIGINT, Value::UBIGINT(HF_DEFAULT_READ_MAX_PARALLEL));
	config.AddExtensionOption("hf_read_cache_total_size",
	                          "How many fetched bytes are kept around over all hf:// files (an in-memory cache, nothing is written to disk)",
	                          LogicalType::UBIGINT, Value::UBIGINT(HF_DEFAULT_READ_CACHE_TOTAL));
}

} // namespace duckdb
