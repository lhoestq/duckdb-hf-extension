#include "hf_common.hpp"
#include "hf_dataset.hpp"
#include "hf_fs.hpp"
#include "hf_functions.hpp"
#include "hf_http.hpp"
#include "hf_secrets.hpp"
#include "hf_settings.hpp"

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	HFSettings::Register(DBConfig::GetConfig(db));
	// Order matters against HFSecrets::Register below, which is where hf takes the `huggingface` secret type:
	// an httpfs that is already on disk has to be loaded before that, or httpfs can never initialise in this
	// process again (it registers the same type and does not tolerate a collision). Both attempts below are
	// offline - `LOAD httpfs` does not install, it says "Install it first" - because loading hf is not a
	// request to touch the network. An absent httpfs stays absent until the first request, which may install.
	// Only hf's own offline load runs here. Asking the *client* to `LOAD httpfs` at this point is what a
	// user would type, and it does not work: loading an extension from inside another extension's load is
	// refused, and a refused httpfs initialisation still leaves the secret type it registered behind, which
	// then blocks every later `LOAD httpfs`. At request time, where nothing is being loaded, the same
	// attempt is both legal and useful - see HFHttpfs::Acquire.
	string transport_reason;
	HFHttpfs::AcquireInstalled(db, transport_reason);
	HFSecrets::Register(loader);
	HFFileSystem::Register(loader);
	HFFunctions::Register(loader);
	HFDatasetFunctions::Register(loader);
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(hf, loader) {
	duckdb::LoadInternal(loader);
}
}
