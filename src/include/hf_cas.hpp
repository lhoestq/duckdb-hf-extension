#pragma once

#include "hf_common.hpp"
#include "hf_fs.hpp"
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

//! Chunk a file, group its chunks into xorbs, and put each one where the CAS will find it by hash.
//! This is the upload half of a Xet write on its own: the content ends up addressable, but no file
//! refers to it until a metadata shard is committed, so nothing reads it back yet (docs/XET.md, M5).
vector<HFXetXorbUpload> HFXetUploadFileContents(DatabaseInstance &db, FileSystem &fs,
                                                optional_ptr<FileOpener> opener, const HFTarget &target,
                                                const string &path);

} // namespace duckdb
