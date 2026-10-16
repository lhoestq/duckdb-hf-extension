#pragma once

#include "hf_common.hpp"

#include "duckdb.hpp"

namespace duckdb {

class DatabaseInstance;
class FileOpener;
struct FileOpenerInfo;
class ExtensionLoader;

//! Resolved connection settings for a single url/path.
struct HFConnection {
	string endpoint = HF_DEFAULT_ENDPOINT;
	string token;
	//! Where the token came from, for nice error messages (`secret`, `hf_token`, `HF_TOKEN`, ...)
	string token_source;

	bool HasToken() const {
		return !token.empty();
	}
};

//! Secrets + settings + environment resolution. Order of precedence for the token:
//! 1. a matching `(TYPE HUGGINGFACE)` secret, 2. the `hf_token` setting, 3. `HF_TOKEN`,
//! 4. `HF_TOKEN_PATH`, 5. `$HF_HOME/token`, 6. `~/.cache/huggingface/token`.
struct HFSecrets {
	//! Register the `huggingface` secret type and its providers (tolerating httpfs' registrations)
	static void Register(ExtensionLoader &loader);
	//! Resolve endpoint + token for `path` (an `hf://...` url). `db` is used as a fallback when there is no
	//! opener to read (global) settings and secrets from - e.g. while uploading from a file handle.
	static HFConnection Resolve(optional_ptr<FileOpener> opener, optional_ptr<FileOpenerInfo> info, const string &path,
	                            optional_ptr<DatabaseInstance> db = nullptr);
	//! Resolve the token only (settings -> env -> token files)
	static string ResolveToken(optional_ptr<FileOpener> opener, optional_ptr<DatabaseInstance> db = nullptr);
	//! Resolve the endpoint from settings/env
	static string ResolveEndpoint(optional_ptr<FileOpener> opener, optional_ptr<DatabaseInstance> db = nullptr);
};

} // namespace duckdb
