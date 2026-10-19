#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

class DatabaseInstance;
class FileOpener;
struct FileOpenerInfo;

//! Default endpoint. Overridable through the `hf_endpoint` setting, the `HF_ENDPOINT` env var or a secret.
//! the release of this extension itself, reported by hf_status() and named in CHANGELOG.md - independent of
//! the DuckDB version identifier the binary is built for, which has to match the running DuckDB exactly
static constexpr const char *HF_EXTENSION_VERSION = "0.0.2-pre8";

static constexpr const char *HF_DEFAULT_ENDPOINT = "https://huggingface.co";

//! Page size of the Hub tree API listings
constexpr idx_t HF_DEFAULT_LIST_PAGE_SIZE = 1000;
//! Read-ahead buffer used by `HFReadHandle`
constexpr idx_t HF_DEFAULT_READ_BUFFER = 8 * 1024 * 1024;
//! Total amount of fetched bytes a single hf:// file may keep around
constexpr idx_t HF_DEFAULT_READ_CACHE = 256 * 1024 * 1024;
//! How many range requests one read-ahead window is split over (1 disables the fan-out)
constexpr idx_t HF_DEFAULT_READ_MAX_PARALLEL = 4;
//! How many fetched bytes the process keeps around over all hf:// files, handles closed or not
constexpr idx_t HF_DEFAULT_READ_CACHE_TOTAL = 1024 * 1024 * 1024;
//! Smallest per-file window cache a file is left with when the budget is split over open files
constexpr idx_t HF_MIN_READ_CACHE = 64 * 1024 * 1024;
//! How often a single GET/HEAD/range request is retried on throttling or transport errors
constexpr idx_t HF_MAX_HTTP_ATTEMPTS = 5;

//! a bucket copy needs the hub's own content hash of each file, and a file committed seconds ago is hashed by
//! the hub in its own time: ask again this many times, waiting HF_COPY_HASH_BACKOFF doubled each round
// a newly created repository is not writable the instant the create call returns: retry with backoff
constexpr idx_t HF_REPO_SETTLE_ATTEMPTS = 4;
constexpr idx_t HF_REPO_SETTLE_BACKOFF = 250; // ms, doubling: 250 + 500 + 1000 + 2000

//! A bucket copy needs the hub's own content hash of each source file, and a file committed seconds ago is
//! hashed by the hub in its own time. Ask again for up to this long, backing off each round; a file still
//! unhashed afterwards is reported as skipped rather than waited on forever.
constexpr int64_t HF_COPY_HASH_WAIT_MS = 30000;
constexpr idx_t HF_COPY_HASH_BACKOFF = 250;    // ms
constexpr idx_t HF_COPY_HASH_MAX_BACKOFF = 4000;
//! Single-PUT (non multipart) LFS uploads are capped by S3 at 5GiB
constexpr idx_t HF_MAX_SINGLE_UPLOAD_SIZE = 5ULL * 1024 * 1024 * 1024;
//! How many Xet xorb uploads one file write keeps in flight at once (1 sends them one after another).
//! The cost of a write is the wire rather than the CPU - 100 MB costs ~0.15 s to chunk, hash and
//! serialize (scripts/xet_bench.sh measures it) and tens of seconds to reach the CAS - and a single https
//! stream is what caps that, so the xorbs a file is split into are sent over several streams. In flight
//! is bounded because each xorb is up to 64 MiB of serialized bytes held in memory while it waits:
//! this many of those, plus the one being built, is the peak.
constexpr idx_t HF_DEFAULT_WRITE_MAX_PARALLEL = 3;

//! Secret type name shared with httpfs' read-only `hf://` support, so that a single
//! `CREATE SECRET (TYPE HUGGINGFACE, ...)` works for both.
static constexpr const char *HF_SECRET_TYPE = "huggingface";

//! Repository types that can appear as the first path element of an `hf://` url.
struct HFRepoType {
	static constexpr const char *DATASETS = "datasets";
	static constexpr const char *MODELS = "models";
	static constexpr const char *SPACES = "spaces";
	static constexpr const char *KERNELS = "kernels";
	static constexpr const char *BUCKETS = "buckets";

	static bool IsValid(const string &repo_type);
	static string ToApiSegment(const string &repo_type);
	//! `datasets` -> `dataset`: the singular form the Hub uses in bucket copy operations and repo creation
	static string ToSingular(const string &repo_type);
	//! The git-facing prefix used by the `/resolve/`, `/raw/` and LFS routes: models live at the root of the
	//! domain (`{endpoint}/{repo_id}/resolve/...`), everything else keeps its plural segment.
	static string ToGitPrefix(const string &repo_type);
};

//! A parsed `hf://` url. Mirrors `huggingface_hub.parse_hf_url` and httpfs' `ParsedHFUrl`:
//!
//!   hf://<repo_type>/<repo_id>/<path>                 -> revision "main"
//!   hf://<repo_type>/<repo_id>@<revision>/<path>
//!   hf://buckets/<user>/<bucket>/<path>               -> no revisions
//!
//! A short form without a repo type (`hf://user/dataset/file.parquet`) is also accepted and resolves to
//! `datasets`, which keeps `hf://` usable like `s3://` for the most common case.
struct HFUrl {
	//! Repository coordinates
	string repo_type = HFRepoType::DATASETS;
	string repository;
	string revision = "main";

	//! Request routing
	string endpoint = HF_DEFAULT_ENDPOINT;
	//! Path inside the repository, always starts with '/' (may be empty for the repo root)
	string path;

	//! Buckets are S3-like: no revisions, no `/resolve/` indirection
	bool IsBucket() const {
		return repo_type == HFRepoType::BUCKETS;
	}
	//! True when the url did not spell out a repo type (short form)
	bool inferred_repo_type = false;

	//! Re-parse the path component after a glob rewrite, keeping everything else
	HFUrl WithPath(const string &new_path) const;

	string ToString() const;
	//! `{endpoint}/{repo_type}/{repo}/resolve/{revision}{path}`
	string ResolveUrl() const;
	//! `{endpoint}/api/{repo_type}/{repo}/tree/{revision}{path}`
	string TreeUrl(idx_t limit, bool recursive) const;
	//! `{endpoint}/api/{repo_type}/{repo}/commits/{revision}?limit=N` (commit history)
	string CommitsUrl(idx_t limit) const;
	//! `{endpoint}/api/{repo_type}/{repo}/refs` (branches, tags, conversions)
	string RefsUrl() const;
	//! `{endpoint}/api/{repo_type}/{repo}` (repo info)
	string InfoUrl() const;

	//! Throw an IOException explaining the expected syntax
	[[noreturn]] static void ThrowParseError(const string &url, const string &reason = string());
	//! Parse `hf://...`. `endpoint`/`token` come from settings/secrets.
	static HFUrl Parse(const string &url, const string &endpoint = HF_DEFAULT_ENDPOINT);
	//! Percentage-encode the path component for use in a URL (keeps '/' separators)
	static string EncodePath(const string &path);
};

//! Identifies the repository a url points at (`endpoint|repo_type|repository`), used to invalidate the
//! in-memory read caches of exactly the repository a commit just changed
string HFRepoCacheKey(const HFUrl &url);

//! Small helpers shared by the filesystem, the writer and the table functions.
struct HFUtil {
	//! Join two path fragments using '/' (normalizes duplicates)
	static string Join(const string &base, const string &part);
	//! Directory of a repo path ("/a/b.csv" -> "/a")
	static string Parent(const string &path);
	//! Basename of a repo path ("/a/b.csv" -> "b.csv")
	static string BaseName(const string &path);
	//! True when the path contains a glob metacharacter
	static bool HasGlob(const string &path);
	//! Read `~/.cache/huggingface/token` (and friends). Returns "" when nothing is found.
	static string TryReadTokenFile(const string &path);
	//! Expand a leading `~`
	static string ExpandPath(const string &path);
	//! Read a file from the *local* fs, returning "" when it does not exist
	static string ReadLocalFile(const string &path);
	//! Redact a bearer token from an error message
	static string Redact(const string &message, const string &token);
};

} // namespace duckdb
