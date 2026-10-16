#pragma once

namespace duckdb {

class ExtensionLoader;

//! Table functions that mirror what the file system can do, for callers that prefer SQL over `SELECT * FROM 'hf://...'`
struct HFFunctions {
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
