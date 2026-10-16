#include "hf_common.hpp"
#include "hf_fs.hpp"
#include "hf_functions.hpp"
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
	HFSecrets::Register(loader);
	HFFileSystem::Register(loader);
	HFFunctions::Register(loader);
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(hf, loader) {
	duckdb::LoadInternal(loader);
}
}
