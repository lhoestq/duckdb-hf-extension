#pragma once

#include "hf_common.hpp"

namespace duckdb {

class DatabaseInstance;
class ExtensionLoader;
class FileOpener;

//! What a call asks the Hub's datasets-server for. An empty `config` or `split` means "everything the
//! dataset has", which is exactly what the `/parquet` route answers when its query parameter is left out.
struct HFDatasetRequest {
	string dataset;  //!< `namespace/name`
	string config;
	string split;
	string revision; //!< empty = whatever the Hub converted from by default
};

//! One converted parquet file of a dataset
struct HFDatasetFile {
	string config;
	string split;
	string url; //!< an `hf://` url for a file that lives on the Hub, anything else as the server wrote it
	string filename;
	idx_t size = 0;
};

//! The files a request selects, and what the answer says about its own completeness
struct HFDatasetFiles {
	vector<HFDatasetFile> files;
	//! `partial: true` means the Hub converted only part of the dataset. hf_dataset() refuses that unless
	//! the caller asks for it: a query that quietly returns half a dataset is indistinguishable from one
	//! that returned all of it, which is the one failure mode a reader cannot check after the fact.
	bool partial = false;
	//! Files not converted yet, and files whose conversion failed. Both mean "the list below is not
	//! everything", which is what `partial` is about, so both are named in the refusal.
	idx_t pending = 0;
	idx_t failed = 0;
	//! Every config and split the dataset has, whether or not the request selected them: an unknown
	//! `config := 'foo'` should be answered with the list of what there is, not with "not found".
	vector<string> configs;
	vector<string> splits;
};

//! The base url of the datasets-server (`{server}/parquet` is where the converted files are listed), from
//! the `hf_datasets_server_endpoint` setting.
string HFDatasetServerEndpoint(optional_ptr<FileOpener> opener);

//! Ask the datasets-server which parquet files a dataset exposes. The token is the one resolved for the
//! dataset's own `hf://` url, so a secret scoped to `hf://datasets/my-org/` authenticates a listing of
//! my-org's files the same way it authenticates reading them - which is also what makes a gated or a
//! private dataset work at all. Throws when the dataset cannot be read or the selection is empty.
HFDatasetFiles HFLoadDatasetParquetFiles(DatabaseInstance &db, optional_ptr<FileOpener> opener,
                                         const HFDatasetRequest &request);

//! `hf_dataset()` and `hf_dataset_files()`
struct HFDatasetFunctions {
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
