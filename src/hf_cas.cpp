#include "hf_cas.hpp"

#include "hf_http.hpp"
#include "hf_xet_chunk.hpp"

#include <chrono>

namespace duckdb {

uint64_t HFXetNowSeconds() {
	return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
	                        std::chrono::system_clock::now().time_since_epoch())
	                        .count());
}

string HFXetCasClient::WriteTokenUrl(const HFTarget &target) {
	auto &url = target.url;
	// repo_type is already the plural api segment ("datasets"), which is how the Hub spells it here too
	auto route = HFUtil::Join(url.endpoint, "api/" + string(url.repo_type) + "/" + url.repository + "/xet-write-token");
	auto revision = url.revision;
	if (!url.IsBucket() && !revision.empty()) {
		// the revision is a path segment, not a query parameter, and an empty one means "the default
		// branch" - which is what a write that has not resolved a commit yet is asking for
		route += "/" + revision;
	}
	return route;
}

void HFXetCasClient::Refresh() {
	HFClient client(db, opener, connection, refresh_route);
	// The Hub answers with the CAS url and a token scoped to this repo, in a body (`casUrl`, `exp`,
	// `accessToken`) and again in X-Xet-* headers. The body is what xet-core parses, so that is what
	// this reads. An expiry of zero means the answer did not carry one, which is treated as "expires
	// now": refreshing on every upload is dull, uploading against a dead token is not.
	auto body = client.Get(refresh_route);

	HFXetCasToken next;
	bool found_url = false;
	bool found_token = false;
	bool found_exp = false;
	// raw access, not GetString: `exp` is a number and the reference spells these cas_url / exp /
	// access_token, so both spellings are tried before deciding the exchange failed
	next.cas_url = HFJson::AsString(HFJson::GetRaw(body, "casUrl", found_url));
	if (!found_url) {
		next.cas_url = HFJson::AsString(HFJson::GetRaw(body, "cas_url", found_url));
	}
	next.access_token = HFJson::AsString(HFJson::GetRaw(body, "accessToken", found_token));
	if (!found_token) {
		next.access_token = HFJson::AsString(HFJson::GetRaw(body, "access_token", found_token));
	}
	auto exp_raw = HFJson::GetRaw(body, "exp", found_exp);
	if (!found_exp) {
		exp_raw = HFJson::GetRaw(body, "expiration_unix_epoch", found_exp);
	}
	next.expiry_unix_epoch = found_exp ? uint64_t(HFJson::AsInt(exp_raw)) : 0;

	if (!next.Valid()) {
		throw IOException("the Hub did not return Xet connection info for '%s' - the token exchange "
		                  "replied with '%s'",
		                  refresh_route, body.substr(0, 200));
	}
	token = std::move(next);
}

const string &HFXetCasClient::CasUrl() {
	// 30 seconds of slack: an upload that starts with a token about to die is refused mid-flight,
	// and a large xorb is a long time to be halfway through
	if (!token.Valid() || token.ExpiresWithin(30)) {
		Refresh();
	}
	return token.cas_url;
}

bool HFXetCasClient::UploadXorb(const HFXetXorb &xorb, const string &prefix) {
	CasUrl();

	// A xorb is addressed by the hash of its chunks, so the url is derived from the content and the
	// upload is idempotent by construction: re-sending one is either a second insert of identical
	// bytes (which the CAS refuses to do twice) or a bug in our writer.
	auto url = HFUtil::Join(token.cas_url, "v1/xorbs/" + prefix + "/" + xorb.hash.Hex());

	HTTPHeaders headers;
	headers.Insert("Authorization", "Bearer " + token.access_token);

	HFClient client(db, opener, connection, url);
	bool inserted = true;
	// RunWithRetry rather than a single Post: the CAS sheds load with 429 and 503 the way the Hub
	// does, and a xorb that fails is worth another attempt because re-sending it costs nothing.
	auto response = client.RunWithRetry([&]() {
		return client.Post(url, xorb.bytes, headers, false);
	});
	if (!response->HasRequestError() && static_cast<int>(response->status) == 200) {
		bool found = false;
		auto raw = HFJson::GetRaw(response->body, "was_inserted", found);
		if (!found) {
			raw = HFJson::GetRaw(response->body, "wasInserted", found);
		}
		// a 2xx that says nothing is taken as an insert: the content is there, which is the only thing
		// the caller can act on
		inserted = !found ? true : HFJson::AsBool(raw);
	} else {
		client.ThrowHTTPError(*response, "POST (xet xorb upload)", url);
	}
	return inserted;
}

vector<HFXetXorbUpload> HFXetUploadFileContents(DatabaseInstance &db, FileSystem &fs,
                                                optional_ptr<FileOpener> opener, const HFTarget &target,
                                                const string &path) {
	vector<HFXetXorbUpload> uploads;

	// The token route is per repository, so the file being uploaded has to live in one - which is also
	// the honest scope: a CAS token lets you write content for one repo, not for any file you can read.
	HFXetCasClient cas(db, opener, target.connection, HFXetCasClient::WriteTokenUrl(target));

	// no opener passed: in practice `fs` is the context's OpenerFileSystem, which supplies the opener
	// itself and refuses a caller-provided one. `opener` is still what the token exchange below reads
	// secrets and settings from.
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("could not open '%s' to upload its content", path);
	}
	FileHandle &file = *handle;

	HFXetChunker chunker;
	HFXetXorbBuilder builder;
	string buffer(HF_DEFAULT_READ_BUFFER, '\0');
	auto file_size = file.GetFileSize();
	idx_t offset = 0;

	auto flush_xorb = [&](bool force) {
		// A xorb is closed when it is full, and only at a chunk boundary otherwise: the 64 MiB and
		// 8192 chunk limits are per object, and splitting a chunk across two xorbs would not be the
		// same content.
		if (builder.ChunkCount() > 0 && (builder.IsFull() || force)) {
			auto uncompressed = builder.UncompressedBytes();
			auto xorb = builder.Finish();
			uploads.push_back({xorb.hash, xorb.chunks.size(), xorb.bytes.size(), uncompressed,
			                   cas.UploadXorb(xorb)});
		}
	};

	while (offset < file_size) {
		auto want = std::min<idx_t>(buffer.size(), file_size - offset);
		// FileSystem::Read fills exactly `want` bytes or throws, so there is no short read to handle
		// here - and a chunker must not see a partial block anyway, since gear positions depend on
		// the bytes that precede them.
		fs.Read(file, buffer.data(), int64_t(want), offset);
		offset += want;
		chunker.Process(const_data_ptr_cast(buffer.data()), want, [&](const_data_ptr_t data, idx_t len) {
			if (builder.WouldOverflow(len)) {
				flush_xorb(true);
			}
			builder.AddChunk(data, len);
		});
	}
	chunker.Finish([&](const_data_ptr_t data, idx_t len) {
		if (builder.WouldOverflow(len)) {
			flush_xorb(true);
		}
		builder.AddChunk(data, len);
	});
	flush_xorb(true);

	return uploads;
}

} // namespace duckdb
