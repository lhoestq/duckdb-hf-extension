#pragma once

#include "hf_common.hpp"
#include "hf_http.hpp"
#include "hf_secrets.hpp"

#include "duckdb/common/file_system.hpp"

#include <atomic>
#include <list>
#include <mutex>

namespace duckdb {

class DatabaseInstance;

//! A path plus the connection/auth info resolved for it (endpoint, token)
struct HFTarget {
	HFUrl url;
	HFConnection connection;

	const HFUrl &Url() const {
		return url;
	}
};

//! One entry returned by a repository listing
struct HFListEntry {
	string path;    //!< path inside the repository (no leading slash)
	bool is_dir {};
	idx_t size {};
	string sha256;  //!< only filled in by the detail-carrying listings
	string lfs_oid; //!< empty for regular git files
	string commit_url;
};

//! sha256 + size of a local file, as needed by the Hub upload APIs
struct HFBlobInfo {
	string sha256;
	idx_t size = 0;
	//! first bytes of the file, base64 encoded (the Hub uses them to detect LFS candidates)
	string sample;
};

//! A single file operation in a commit
struct HFCommitOp {
	enum class Kind { WRITE, DELETE, DELETE_FOLDER, COPY };

	Kind kind = Kind::WRITE;
	string path;
	//! for WRITE with uploadMode == "regular": raw content (base64-encoded when encoding is "base64")
	string content;
	string encoding = "base64";
	//! for WRITE with uploadMode == "lfs" and for COPY of existing LFS files
	string oid;
	idx_t size = 0;
	bool is_lfs = false;
};

//!\name High level Hugging Face operations, shared by the file system and the table functions
//@{

//! Parse an `hf://` path and resolve the endpoint + token to use for it
HFTarget HFResolveTarget(const string &path, optional_ptr<FileOpener> opener, DatabaseInstance &db);

//! Build an HTTP client (transport + auth) for a resolved target
unique_ptr<HFClient> HFMakeClient(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target);

//! List a "directory" of a repository. `recursive` is only honoured for buckets (the tree API is flat otherwise).
//! `tolerate_not_found` keeps the listing of a missing path empty instead of throwing: the glob walk needs that,
//! as a folder entry reported for one prefix may not resolve when listed on its own.
void HFListEntries(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &dir, bool recursive,
                   const std::function<void(const HFListEntry &)> &callback, bool with_details = false,
                   bool tolerate_not_found = false);

//! HEAD a file; returns false (and leaves size untouched) when it does not exist
bool HFTryGetFileSize(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target, idx_t &size,
                      string &etag);

//! POST an ndjson commit payload to a repository, retrying on revision conflicts
void HFCommit(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &repo_target,
              const vector<HFCommitOp> &operations, const string &commit_message);

//! Upload a local file to a repository path (preupload -> blob/LFS upload -> commit)
void HFUploadLocalFile(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                       const string &local_path, const HFBlobInfo &blob, const string &commit_message);

//! Delete files / folders in one commit
void HFDeletePaths(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                   const vector<string> &files, const vector<string> &folders, const string &commit_message);

//! A file that already lives on the Hub, addressed by the content hash buckets store files under. `xet_hash` is
//! empty for the few files a bucket could not take (a repository that is not Xet-backed).
struct HFXetFile {
	string path;
	idx_t size = 0;
	string xet_hash;
};

//! Content hashes of `paths` in the repository (or bucket) `source` points at, in one API call. This is what
//! makes writing to a bucket possible without a Xet client: a file already uploaded to a dataset or model repo
//! has a hash a bucket can point at, and the copy itself happens on the Hub. `source` has to come from
//! HFResolveTarget - the endpoint and token live in its connection, not in the url.
vector<HFXetFile> HFXetHashes(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &source,
                              const vector<string> &paths);

//! One `copyFile` operation of the bucket batch endpoint
struct HFBucketCopyOp {
	string source_repo_type; //!< `dataset`, `model`, `space` or `bucket`
	string source_repo_id;
	string xet_hash;
	string path; //!< path inside the destination bucket
	idx_t size = 0;
	string source_path;
};

//! Ask a bucket to take a list of files that already exist on the Hub, server side
void HFCopyIntoBucket(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &bucket,
                      const vector<HFBucketCopyOp> &operations);

//! sha256/size/sample of a local file
HFBlobInfo HFComputeBlobInfo(DatabaseInstance &db, const string &local_path);

//! Path of a scratch file used to buffer writes before they are uploaded. `preferred_dir` (empty = auto)
string HFMakeTempPathIn(DatabaseInstance &db, const string &preferred_dir, const string &extension);
//! Same, resolving the `hf_cache_dir` setting from `opener` first
string HFMakeTempPath(DatabaseInstance &db, optional_ptr<FileOpener> opener, const string &extension);

//! One commit of a repository history
struct HFRevisionInfo {
	string sha;
	string title;
	string message;
	string author;
	string date; //!< raw ISO-8601 as returned by the Hub
};

//! A branch, tag or conversion ref of a repository
struct HFRefInfo {
	string kind; //!< branch, tag or convert
	string name;
	string ref;
	string target_commit;
};

//! Commit history of the revision a target points at. Throws for buckets, which are unversioned.
void HFListRevisions(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target, idx_t limit,
                     const std::function<void(const HFRevisionInfo &)> &callback);
//! Branches, tags and conversions of a repository
void HFListRefs(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                const std::function<void(const HFRefInfo &)> &callback);
//! Create the repository behind `target`; returns false when it already existed
bool HFCreateRepo(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target, bool private_repo);
//! Who the resolved token belongs to. Failures are reported through `error` rather than thrown, so that
//! `hf_status()` still shows the configuration when the token is missing or rejected.
void HFWhoami(DatabaseInstance &db, optional_ptr<FileOpener> opener, const string &path, string &user,
              string &fullname, bool &is_pro, string &error);

//! Buckets are Xet-backed, and only the `hf_xet` client can compute the `xetHash` that `addFile` requires, so
//! every write path (COPY, hf_upload, ...) refuses them with the same explanation
void HFRefuseBucketWrite(const HFUrl &url, const string &action);

//! Default commit message when `hf_commit_message` is not set
string HFDefaultCommitMessage(const HFUrl &url);
//@}

//! The Hugging Face file system: reads through the `/resolve/` endpoints, writes through the Hub commit API.
class HFFileSystem : public FileSystem {
public:
	explicit HFFileSystem(DatabaseInstance &db_p) : db(db_p) {}

	//! Register ourselves alongside httpfs' read-only `hf://` implementation (we win, see IsManuallySet)
	static void Register(ExtensionLoader &loader);

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener) override;

	bool CanHandleFile(const string &fpath) override;
	string GetName() const override;
	//! Win against the (read-only) `hf://` filesystem httpfs may also register, whoever registered last
	bool IsManuallySet() override;
	bool IsPathAbsolute(const string &path) override;
	string PathSeparator(const string &path) override;
	bool SupportsListFilesExtended() const override {
		return true;
	}

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	bool CanSeek() override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	int64_t GetFileSize(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;
	FileMetadata Stats(FileHandle &handle) override;
	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) override;
	void Truncate(FileHandle &handle, int64_t new_size) override;
	void FileSync(FileHandle &handle) override;
	void AbortFileWrite(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;
	bool OnDiskFile(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;

	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) override;
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener) override;
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener) override;
	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override;
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;

	//! Build the `hf://` url of a listing entry found in `dir`
	static string GetHFUrl(const HFUrl &dir, const string &entry_path);

	DatabaseInstance &Db() const {
		return db;
	}

private:
	DatabaseInstance &db;
};

//! A run of bytes fetched from the hub in one request
struct HFByteRange {
	idx_t start = 0;
	string data;

	idx_t End() const {
		return start + data.size();
	}
	idx_t Bytes() const {
		return data.capacity();
	}
};

//! A window that some thread is downloading right now
struct HFPendingRange {
	idx_t start = 0;
	idx_t end = 0;
	bool finished = false;
};

//! Fetched byte ranges of one remote file, shared by every handle that has it open. DuckDB reads a single file
//! with tasks that share handles, and the hub charges ~0.3s of latency per range request: without sharing and
//! coalescing, threads download the same bytes repeatedly and scans end up an order of magnitude slower.
//! Counters for the read path, process wide and exposed by hf_read_stats(). They exist so that a slow scan
//! can be told apart from an amplifying one: bytes_fetched / file size says how many times the bytes crossed
//! the wire, part_requests / window_fetches how parallel each window was.
struct HFReadCounters {
	std::atomic<uint64_t> files_opened {0};
	//! reads answered from a window that was already fetched
	std::atomic<uint64_t> range_hits {0};
	//! reads that waited for a window another thread was fetching
	std::atomic<uint64_t> coalesced_waits {0};
	//! windows pulled from the hub
	std::atomic<uint64_t> window_fetches {0};
	//! range requests used to pull those windows (more than one per window when a window is split)
	std::atomic<uint64_t> part_requests {0};
	//! windows that had to span more than one grid block because one read straddled a window edge
	std::atomic<uint64_t> straddling_fetches {0};
	//! windows dropped to stay under hf_read_cache_size
	std::atomic<uint64_t> evictions {0};
	//! repositories whose cached windows were thrown away because something was committed to them
	std::atomic<uint64_t> invalidations {0};
	std::atomic<uint64_t> bytes_fetched {0};
};

HFReadCounters &HFReadCountersGet();
void HFReadCountersReset();

//! What the process keeps fetched after the handles are closed (see HFReadCacheRegistry in hf_read.cpp)
idx_t HFReadCacheRetainedBytes();
idx_t HFReadCacheRetainedFiles();
//! Drop every retained cache (the caches of files that are being read right now survive until they close)
void HFReadCacheClear();
//! Forget what was fetched for one repository, after a commit changed it: a reader that opens the file
//! again must see the new bytes, not the window cached before the commit
void HFReadCacheInvalidateRepo(const string &repo_key);

class HFFileCache : public enable_shared_from_this<HFFileCache> {
public:
	HFFileCache(string key_p, idx_t buffer_size_p, idx_t cache_limit_p, idx_t max_parallel_p);

	//! Borrow (or create) the cache registered under `key`, keeping at most `total_budget` fetched bytes
	//! around over all files. `repo_key` names the repository the bytes come from, see HFRepoCacheKey.
	static shared_ptr<HFFileCache> Acquire(const string &key, idx_t buffer_size, idx_t cache_limit,
	                                      idx_t max_parallel, idx_t total_budget, const string &repo_key);

	//! Read exactly [offset, offset + nr_bytes), fetching aligned windows on a miss
	void Read(HFClient &client, const string &url, const string &path, data_ptr_t buffer, idx_t offset,
	          idx_t nr_bytes);
	//! HEAD the file once; the size, etag and mtime are then remembered
	idx_t Size(HFClient &client, const string &url, const string &path);
	//! How many fetched bytes this entry currently holds
	idx_t CachedBytes() {
		std::lock_guard<std::mutex> guard(lock);
		return cached_bytes;
	}
	//! Forget every fetched window: the repository behind this entry just changed underneath us
	void DropAllWindows() {
		std::lock_guard<std::mutex> guard(lock);
		ranges.clear();
		cached_bytes = 0;
		file_size = optional_idx();
		etag = "";
	}
	const string &ETag() const {
		return etag;
	}
	int64_t LastModified() const {
		return last_modified;
	}

private:
	//! HEAD the file if needed - callers must hold `lock`
	idx_t SizeLocked(HFClient &client, const string &url, const string &path);
	//! Insert a fetched window and trim the cache back to `cache_limit` - callers must hold `lock`
	void InsertLocked(HFByteRange fresh);

	mutable std::mutex lock;
	std::condition_variable cv;
	string key;
	idx_t buffer_size;
	idx_t cache_limit;
	//! Range requests one missing window is fetched with, see FetchWindow
	idx_t max_parallel;
	//! Grid every window is aligned to: fixed for the life of the cache, so windows stay disjoint whatever
	//! size the current one is - see the comment in HFFileCache::Read
	idx_t grid_length;
	//! Size of the next read-ahead window, it doubles on every cache miss (in whole grid blocks)
	idx_t fetch_length;
	idx_t cached_bytes = 0;
	optional_idx file_size;
	string etag;
	int64_t last_modified = 0;
	//! Fetched ranges, most recently used first
	std::list<HFByteRange> ranges;
	//! Windows currently being downloaded by some thread
	std::list<shared_ptr<HFPendingRange>> pending;
};

//! Read handle: fetches byte ranges on demand through the shared per-file cache.
class HFReadHandle : public FileHandle {
public:
	HFReadHandle(HFFileSystem &file_system, const string &path, FileOpenFlags flags, HFTarget target,
	             optional_ptr<FileOpener> opener);

	void Close() override;
	//! Read exactly [offset, offset + nr_bytes)
	void ReadAt(data_ptr_t buffer, idx_t offset, idx_t nr_bytes);
	idx_t Size();
	idx_t GetPosition() const {
		return position.load();
	}
	void SetPosition(idx_t position_p) {
		position.store(position_p);
	}
	const string &ETag() const {
		return cache->ETag();
	}
	//! Last modification time of the revision content, epoch seconds (0 when the Hub did not tell us)
	int64_t LastModified() const {
		return cache->LastModified();
	}
	const HFTarget &Target() const {
		return target;
	}
	HFClient &Client() {
		return *client;
	}

private:
	HFTarget target;
	string resolve_url;
	string cache_key;
	unique_ptr<HFClient> client;
	shared_ptr<HFFileCache> cache;
	std::atomic<idx_t> position {0};
};

//! Write handle: buffers into a local temp file, uploads + commits on Close().
class HFWriteHandle : public FileHandle {
public:
	HFWriteHandle(HFFileSystem &file_system, const string &path, FileOpenFlags flags, HFTarget target,
	              optional_ptr<FileOpener> opener);

	void Close() override;
	void Write(data_ptr_t buffer, idx_t nr_bytes);
	//! Drop the buffered data without uploading anything
	void Abort();
	idx_t BytesWritten() const {
		return bytes_written;
	}
	const HFTarget &Target() const {
		return target;
	}

private:
	void EnsureTempFile();
	void CleanupTempFile();

	HFTarget target;
	string temp_path;
	string cache_dir;
	string commit_message;
	unique_ptr<FileHandle> temp_handle;
	idx_t bytes_written = 0;
	bool closed = false;
	bool aborted = false;
};

} // namespace duckdb
