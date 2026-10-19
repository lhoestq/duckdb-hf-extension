#pragma once

#include "hf_common.hpp"
#include "hf_fs.hpp"
#include "hf_xet_shard.hpp"
#include "hf_xet_xorb.hpp"

#include <mutex>

namespace duckdb {

//! The namespace a xorb is stored under. xet-core keeps it in a config group (`default_prefix`) whose
//! default is literally "default", and the Hub ignores it for placement - it is the CAS's own keying
//! prefix, not part of a file's identity.
#define HF_XET_DEFAULT_PREFIX "default"

//===----------------------------------------------------------------------===//
// The CAS client (docs/XET.md, M4): the token exchange, and the call that moves content.
//
// Two things about it are unlike the rest of the extension's HTTP, and both are easy to get wrong in
// a way that only shows up as a 401:
//
//   * The CAS is a different service on a different host (`cas-server.xethub.hf.co`), and the Hub is
//     the only place that says so. The url is never configured, only fetched.
//   * It authenticates with its own short-lived token, and the Hub token must not be sent along with
//     it - the same rule as presigned git-LFS uploads, for the same reason.
//
// A third thing, since a file's xorbs go out with several requests in flight: several threads share one
// of these clients, so the token is read and refreshed under a lock, and a client handed to a worker
// thread carries no FileOpener (ForWorkerThread below).
//===----------------------------------------------------------------------===//

//! Current unix time in seconds. Free, and injected rather than read inline, because a token's
//! lifetime is measured against it: a test must be able to freeze it instead of sleeping through it.
uint64_t HFXetNowSeconds();

//! What the token exchange returns: where the CAS is, and a token scoped to one repo and one
//! direction. Expiry is a unix epoch in seconds; the reference client refreshes 30 seconds early.
struct HFXetCasToken {
	string cas_url;
	string access_token;
	uint64_t expiry_unix_epoch = 0;

	bool Valid() const {
		return !cas_url.empty() && !access_token.empty();
	}

	bool ExpiresWithin(idx_t seconds) const {
		return HFXetNowSeconds() + seconds >= expiry_unix_epoch;
	}
};

class HFXetCasClient {
public:
	HFXetCasClient(DatabaseInstance &db_p, optional_ptr<FileOpener> opener_p, HFConnection connection_p,
	               string refresh_route_p)
	: db(db_p), opener(opener_p), connection(std::move(connection_p)), refresh_route(std::move(refresh_route_p)) {
	}

	//! The CAS base url and the token to use with it, exchanging the Hub token for a CAS token on first
	//! use and again whenever the one in hand is within half a minute of expiring. Copied out rather than
	//! handed back by reference: the uploads of one file run on several threads against this one client,
	//! and any of them may be refreshing the token while another is still using it.
	void Authorize(string &cas_url, string &access_token);

	//! Store one xorb under its own hash. Returns false when the CAS answers `was_inserted: false` -
	//! not a failure but a hit: those bytes are already content-addressed into the store, which is the
	//! whole deduplication story of a re-upload or a file that shares chunks with another file.
	bool UploadXorb(const HFXetXorb &xorb, const string &prefix = HF_XET_DEFAULT_PREFIX);

	//! Store a metadata shard, which is what makes uploaded chunks reachable as a file. The CAS
	//! re-serializes what it is sent and stores it under the result, so the hash it answers with is
	//! authoritative and the uploader's own is only a prediction - which is why it comes back as text
	//! rather than being compared to anything here.
	//! `result` is the CAS's own words: "exists" or "sync performed".
	void UploadShard(const string &shard_bytes, string &shard_hash, string &result);

	//! Ask the dedup index what it holds for one chunk hash: `GET {cas}/v1/chunks/default/{hash}`, with the
	//! same token the uploads use. Returns true and fills `shard_bytes` on a hit - the answer is a whole
	//! metadata shard, which `HFXetShardView::Parse` reads (measured: the bytes a session uploaded, plus
	//! the 56-byte footer it was stripped of). Returns false on a 404.
	//!
	//! A 404 is worth naming, because it is the answer for all but a sliver of chunks: the index holds a
	//! file's first chunk and one chunk hash in 1024, so querying any other chunk is wasted network. The
	//! measurement is in docs/XET.md, M9: of a 1 025-chunk file, the 4 eligible hashes all hit and 13
	//! controls all missed. Anything that is neither 200 nor 404 throws, because a refused token read as
	//! "not stored" would pay to upload content the CAS already has, silently.
	bool QueryChunk(const HFXetHash &chunk_hash, string &shard_bytes,
	                const string &prefix = HF_XET_DEFAULT_PREFIX);

	//! Where to ask for a repo's write-scoped CAS token. The route is derived rather than discovered:
	//! `{endpoint}/api/{repo_type}/{repo_id}/xet-write-token`, plus the revision as a path segment for
	//! the git-backed repo types (a bucket has no revision to name).
	static string WriteTokenUrl(const HFTarget &target);

	//! A client for the same repository and the same token, built to be shared by worker threads. It
	//! drops the FileOpener, because a `ClientContext`-backed one reads settings through the client it
	//! belongs to - which is also why the read path's parallel range fetches construct their per-thread
	//! clients with no opener (FetchWindow in hf_read.cpp). Nothing about authentication changes: the CAS
	//! token is exchanged with the already-resolved connection, and an opener was never part of it. What
	//! does change is that transport parameters (retries, user agent, headers) come from DBConfig rather
	//! than from this session's settings - for one POST per xorb, to a service that looks at none of
	//! them, which is the same trade the read path makes.
	unique_ptr<HFXetCasClient> ForWorkerThread() const;

private:
	//! Refresh if the token in hand is within 30 seconds of expiring. Callers hold `token_lock`.
	void EnsureValidLocked();
	void RefreshLocked();

	DatabaseInstance &db;
	optional_ptr<FileOpener> opener;
	HFConnection connection;
	string refresh_route;
	HFXetCasToken token;
	mutable std::mutex token_lock;
};

//! One uploaded xorb, as reported by `hf_xet_upload`.
struct HFXetXorbUpload {
	HFXetHash hash;
	uint64_t num_chunks;
	//! what goes over the wire, chunk headers and footer included
	uint64_t bytes;
	//! what the xorb's hash covers, and what the format's size limits are stated in
	uint64_t uncompressed_bytes;
	bool inserted;
};

//! The upload of one file's content: every xorb it took, the metadata shard that turns them back
//! into this file, and the hash that names the result. The file hash is what the Hub spells `xetHash`
//! - verified, not inferred: the hub's own tree API reports exactly this string for a file whose
//! bytes the reference chunker and hasher reproduce locally.
struct HFXetUploadResult {
	HFXetHash file_hash;
	vector<HFXetXorbUpload> xorbs;
	//! the shard as the CAS named it, verbatim from its answer. Not comparable to a hash computed
	//! here: the CAS re-serializes shards before keying them.
	string shard_hash;
	string shard_result;
	idx_t shard_bytes = 0;

	//! Cross-repository dedup, as of this file. These describe the *file*, not one xorb, so a caller
	//! reading one row per xorb sees the same numbers on every row (`deduped_chunks` is a property of the
	//! whole file: a chunk covered by a pointer segment was never uploaded, so it appears in no xorb).
	//!
	//! Chunks the CAS already had, out of the file's total.
	idx_t deduped_chunks = 0;
	//! how many times the CAS was asked, how many times it answered with a shard, and how many of those
	//! answers this build could not parse. A file that deduplicated nothing and a file whose dedup answers
	//! were unreadable have the same `deduped_chunks` and very different histories.
	idx_t dedup_queries = 0;
	idx_t dedup_hits = 0;
	idx_t dedup_shard_failures = 0;

	//! What a bucket's `addFile` (and `hf_bucket_copy`) calls a xetHash.
	string XetHash() const {
		return file_hash.Hex();
	}
};

//! Chunk a file, group its chunks into xorbs, upload each one where the CAS will find it by hash,
//! then upload the shard that describes the file. After this call the *content* exists in the CAS and
//! is reachable *by hash*; naming it as a file in a repository is the caller's business - a bucket
//! does it with a batch `addFile`, a git-backed repo with an LFS pointer (docs/XET.md, M5).
//!
//! `sha256_hex`, when given, is written into the file entry's metadata extension. That field is the
//! only place where the content's xet hash and the sha256 a git commit names the blob by are stated
//! together, so a git-backed upload that wants the Hub to see its content as Xet has to pass it, and a
//! bucket upload must not (a bucket file has no sha256 to be named by).
HFXetUploadResult HFXetUploadFileContents(DatabaseInstance &db, FileSystem &fs, optional_ptr<FileOpener> opener,
                                          const HFTarget &target, const string &path,
                                          const string *sha256_hex = nullptr);

} // namespace duckdb
