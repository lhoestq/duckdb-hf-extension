#include "hf_secrets.hpp"

#include "hf_common.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

static unique_ptr<BaseSecret> CreateHFSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	string token;
	for (const auto &named_param : input.options) {
		auto name = StringUtil::Lower(named_param.first);
		if (name == "token") {
			token = named_param.second.ToString();
		} else if (name == "endpoint") {
			// stored for completeness, resolved by HFSecrets::ResolveEndpoint
		} else {
			throw InvalidInputException("Unknown named parameter passed to CREATE SECRET (TYPE HUGGINGFACE): %s", name);
		}
	}
	auto scope = input.scope.empty() ? vector<string>{"hf://"} : input.scope;
	auto value = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	value->secret_map["token"] = token;
	value->redact_keys.insert("token");
	return std::move(value);
}

static unique_ptr<BaseSecret> CreateHFSecretFromCredentialChain(ClientContext &context, CreateSecretInput &input) {
	auto token = HFSecrets::ResolveToken(nullptr);
	auto scope = input.scope.empty() ? vector<string>{"hf://"} : input.scope;
	auto value = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	value->secret_map["token"] = token;
	value->redact_keys.insert("token");
	return std::move(value);
}

void HFSecrets::Register(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	auto &manager = *config.secret_manager;

	// The type may already exist: httpfs registers `huggingface` too, and both extensions can be loaded.
	static constexpr const char *TYPE_NAME = HF_SECRET_TYPE;
	bool type_exists = false;
	try {
		manager.LookupType(TYPE_NAME);
		type_exists = true;
	} catch (std::exception &) {
		type_exists = false;
	}
	if (!type_exists) {
		SecretType secret_type;
		secret_type.name = Identifier(TYPE_NAME);
		secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
		secret_type.default_provider = "config";
		secret_type.extension = "hf";
		manager.RegisterSecretType(secret_type);
	}

	// Providers are registered with IGNORE so that httpfs' providers win when it is loaded first - they are
	// equivalent, and the lookup path below is identical either way.
	CreateSecretFunction config_fun;
	config_fun.secret_type = TYPE_NAME;
	config_fun.provider = "config";
	config_fun.function = CreateHFSecretFromConfig;
	config_fun.named_parameters["token"] = LogicalType::VARCHAR;
	manager.RegisterSecretFunction(config_fun, OnCreateConflict::IGNORE_ON_CONFLICT);

	CreateSecretFunction chain_fun;
	chain_fun.secret_type = TYPE_NAME;
	chain_fun.provider = "credential_chain";
	chain_fun.function = CreateHFSecretFromCredentialChain;
	manager.RegisterSecretFunction(chain_fun, OnCreateConflict::IGNORE_ON_CONFLICT);
}

string HFUtil::ReadLocalFile(const string &path) {
	try {
		LocalFileSystem fs;
		if (!fs.FileExists(path)) {
			return "";
		}
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto size = fs.GetFileSize(*handle);
		if (size <= 0) {
			return "";
		}
		string result;
		result.resize(NumericCast<idx_t>(size));
		fs.Read(*handle, &result[0], size);
		StringUtil::Trim(result);
		return result;
	} catch (std::exception &) {
		return "";
	}
}

string HFUtil::ExpandPath(const string &path) {
	if (path == "~" || path.compare(0, 2, "~/") == 0) {
		const char *home = std::getenv("HOME");
		if (home) {
			return string(home) + path.substr(1);
		}
	}
	return path;
}

string HFUtil::TryReadTokenFile(const string &path) {
	return ReadLocalFile(ExpandPath(path));
}

static string EnvOrEmpty(const char *name) {
	const char *value = std::getenv(name);
	return value ? string(value) : string();
}

static string DefaultTokenPath() {
	auto xdg = EnvOrEmpty("XDG_CACHE_HOME");
	auto home = EnvOrEmpty("HOME");
	string base;
	if (!home.empty()) {
		base = home + "/.cache/huggingface/token";
	} else if (!xdg.empty()) {
		base = xdg + "/huggingface/token";
	}
	if (!base.empty()) {
		return base;
	}
	return "hf_token"; // last resort: a file in the cwd, never found in practice
}

//! Read a (string) setting: through the client context when there is one, otherwise from the DBConfig, which is
//! where GLOBAL settings set by another connection (or by a config) live.
static string LookupHFSetting(optional_ptr<FileOpener> opener, optional_ptr<DatabaseInstance> db, const char *name) {
	if (opener) {
		auto context = FileOpener::TryGetClientContext(opener);
		if (context) {
			Value value;
			if (context->TryGetCurrentSetting(name, value) && !value.IsNull()) {
				return StringValue::Get(value);
			}
			return "";
		}
	}
	if (db) {
		Value value;
		if (DBConfig::GetConfig(*db).TryGetCurrentSetting(Identifier(name), value) && !value.IsNull()) {
			return StringValue::Get(value);
		}
	}
	return "";
}

string HFSecrets::ResolveToken(optional_ptr<FileOpener> opener, optional_ptr<DatabaseInstance> db) {
	// 1. the hf_token setting
	auto token = LookupHFSetting(opener, db, "hf_token");
	if (!token.empty()) {
		return token;
	}
	// 2. HF_TOKEN
	token = EnvOrEmpty("HF_TOKEN");
	if (token.empty()) {
		token = EnvOrEmpty("HUGGING_FACE_HUB_TOKEN"); // legacy name used by huggingface_hub
	}
	if (!token.empty()) {
		return token;
	}
	// 3. HF_TOKEN_PATH
	auto token_path = EnvOrEmpty("HF_TOKEN_PATH");
	if (!token_path.empty()) {
		return HFUtil::ReadLocalFile(HFUtil::ExpandPath(token_path));
	}
	// 4. $HF_HOME/token
	auto hf_home = EnvOrEmpty("HF_HOME");
	if (!hf_home.empty()) {
		return HFUtil::ReadLocalFile(HFUtil::Join(hf_home, "token"));
	}
	// 5. ~/.cache/huggingface/token
	return HFUtil::ReadLocalFile(DefaultTokenPath());
}

string HFSecrets::ResolveEndpoint(optional_ptr<FileOpener> opener, optional_ptr<DatabaseInstance> db) {
	auto endpoint = LookupHFSetting(opener, db, "hf_endpoint");
	if (endpoint.empty() || endpoint == HF_DEFAULT_ENDPOINT) {
		auto env = EnvOrEmpty("HF_ENDPOINT");
		if (!env.empty()) {
			endpoint = env;
		}
	}
	while (!endpoint.empty() && endpoint.back() == '/') {
		endpoint.pop_back();
	}
	return endpoint.empty() ? string(HF_DEFAULT_ENDPOINT) : endpoint;
}

HFConnection HFSecrets::Resolve(optional_ptr<FileOpener> opener, optional_ptr<FileOpenerInfo> info,
                               const string &path, optional_ptr<DatabaseInstance> db) {
	HFConnection result;
	result.endpoint = HFSecrets::ResolveEndpoint(opener, db);

	// A secret, if any, takes precedence over settings/env (this is also what httpfs does for `hf://`)
	auto secret_manager = FileOpener::TryGetSecretManager(opener);
	if (!secret_manager && db) {
		secret_manager = &SecretManager::Get(*db);
	}
	auto transaction = FileOpener::TryGetCatalogTransaction(opener);
	if (!transaction && db) {
		transaction = make_uniq<CatalogTransaction>(CatalogTransaction::GetSystemTransaction(*db));
	}
	if (secret_manager && transaction) {
		try {
			auto match = secret_manager->LookupSecret(*transaction, path, HF_SECRET_TYPE);
			if (match.HasMatch()) {
				auto &kv_secret = match.secret_entry->secret->Cast<KeyValueSecret>();
				auto token = kv_secret.TryGetValue("token", true);
				if (!token.IsNull()) {
					result.token = StringValue::Get(token);
					result.token_source = "secret";
				}
			}
		} catch (std::exception &) {
			// an unreadable secret should not prevent falling back to env/settings
		}
	}
	if (!result.token.empty()) {
		return result;
	}
	result.token = HFSecrets::ResolveToken(opener, db);
	if (!result.token.empty()) {
		result.token_source = "settings/environment";
	}
	return result;
}

} // namespace duckdb
