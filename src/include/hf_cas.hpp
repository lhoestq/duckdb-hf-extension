#pragma once

#include "hf_common.hpp"
#include "hf_fs.hpp"
#include "hf_xet_shard.hpp"
#include "hf_xet_xorb.hpp"

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

	//! The CAS base url, exchanging the Hub token for a CAS token on first use and again whenever the
	//! one in hand is within half a minute of expiring.
	const string &CasUrl();

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

	//! Where to ask for a repo's write-scoped CAS token. The route is derived rather than discovered:
	//! `{endpoint}/api/{repo_type}/{repo_id}/xet-write-token`, plus the revision as a path segment for
	//! the git-backed repo types (a bucket has no revision to name).
	static string WriteTokenUrl(const HFTarget &target);

private:
	void Refresh();

	DatabaseInstance &db;
	optional_ptr<FileOpener> opener;
	HFConnection connection;
	string refresh_route;
	HFXetCasToken token;
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
