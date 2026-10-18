#include "hf_cas.hpp"

#include "hf_http.hpp"
#include "hf_xet_chunk.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <cstdint>

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

void HFXetCasClient::RefreshLocked() {
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

void HFXetCasClient::EnsureValidLocked() {
	// 30 seconds of slack: an upload that starts with a token about to die is refused mid-flight,
	// and a large xorb is a long time to be halfway through
	if (!token.Valid() || token.ExpiresWithin(30)) {
		RefreshLocked();
	}
}

void HFXetCasClient::Authorize(string &cas_url, string &access_token) {
	std::lock_guard<std::mutex> guard(token_lock);
	EnsureValidLocked();
	// copied out, not returned by reference: the xorbs of one file are uploaded from several threads, and
	// one of them can be refreshing the token while another is still building a header from it
	cas_url = token.cas_url;
	access_token = token.access_token;
}

unique_ptr<HFXetCasClient> HFXetCasClient::ForWorkerThread() const {
	auto worker = make_uniq<HFXetCasClient>(db, nullptr, connection, refresh_route);
	std::lock_guard<std::mutex> guard(token_lock);
	// carry the exchange over rather than make the worker do it again: it happens on the thread that owns
	// the opener, and a worker that later has to refresh will do it from the connection alone
	worker->token = token;
	return worker;
}

bool HFXetCasClient::UploadXorb(const HFXetXorb &xorb, const string &prefix) {
	string cas_url, access_token;
	Authorize(cas_url, access_token);

	// A xorb is addressed by the hash of its chunks, so the url is derived from the content and the
	// upload is idempotent by construction: re-sending one is either a second insert of identical
	// bytes (which the CAS refuses to do twice) or a bug in our writer.
	auto url = HFUtil::Join(cas_url, "v1/xorbs/" + prefix + "/" + xorb.hash.Hex());

	HTTPHeaders headers;
	headers.Insert("Authorization", "Bearer " + access_token);

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

void HFXetCasClient::UploadShard(const string &shard_bytes, string &shard_hash, string &result) {
	string cas_url, access_token;
	Authorize(cas_url, access_token);

	auto url = HFUtil::Join(cas_url, "v1/shards");
	HTTPHeaders headers;
	headers.Insert("Authorization", "Bearer " + access_token);

	HFClient client(db, opener, connection, url);
	auto response = client.RunWithRetry([&]() {
		return client.Post(url, shard_bytes, headers, false);
	});
	if (response->HasRequestError() || static_cast<int>(response->status) >= 400) {
		client.ThrowHTTPError(*response, "POST (xet shard upload)", url);
	}
	bool found = false;
	// `result` is a number in the wire format (0 = the shard was already there, 1 = the CAS merged it
	// in), spelled out here because the reference client's own log strings are the readable version
	auto result_raw = HFJson::GetRaw(response->body, "result", found);
	if (found) {
		result = HFJson::AsInt(result_raw) == 0 ? "exists" : "sync performed";
	} else {
		result = "unknown";
	}
	auto hash_raw = HFJson::GetRaw(response->body, "shard_hash", found);
	shard_hash = found ? HFJson::AsString(hash_raw) : "";
}

namespace {

//! How many of a file's xorbs are on the wire at once.
//!
//! A Xet write spends about 0.15 s of CPU per 100 MB - scripts/xet_bench.sh measures chunking, hashing and
//! serializing at ~690 MB/s - and tens of seconds waiting for the POSTs, one per xorb, to be accepted.
//! A single https stream is what caps that, which is the wall the read path ran into too and why it fans a
//! read window out over several requests (FetchWindow in hf_read.cpp). Overlapping xorbs is not just
//! allowed but the point of content addressing: no xorb depends on any other, and the order they reach
//! the CAS in is not observable. What *does* depend on all of them is the metadata shard, which is posted
//! after Finish() below returns.
//!
//! The number in flight is bounded because a xorb waiting to go is up to 64 MiB of serialized bytes held
//! in memory: Submit blocks while `max_in_flight` are already running, which is also the entire
//! backpressure story - the thread reading and chunking the file cannot get more than that much data
//! ahead of the network, so a 10 GB file costs the same memory as a 100 MB one.
class HFXetXorbUploader {
public:
	HFXetXorbUploader(HFXetCasClient &cas_p, idx_t max_in_flight_p)
	: cas(cas_p), max_in_flight(MaxValue<idx_t>(max_in_flight_p, 1)) {
	}

	//! Get a finished xorb onto the wire. `inserted` receives the CAS's answer whenever it lands: true
	//! when it took the bytes, false when it already had them (a dedup hit, not a failure). It is
	//! separately allocated because the caller's list of results grows afterwards, and a flag living in
	//! that list would move out from under the thread writing it.
	void Submit(HFXetXorb xorb, shared_ptr<std::atomic<bool>> inserted) {
		auto payload = make_shared_ptr<HFXetXorb>(std::move(xorb));
		{
			std::unique_lock<std::mutex> guard(lock);
			idle.wait(guard, [&] {
				return running < max_in_flight || !failure.empty();
			});
			if (!failure.empty()) {
				// something upstream already failed, and the shard that would name this xorb is never
				// going to be posted: stop feeding rather than upload a file nobody will reference
				return;
			}
			running++;
		}
		try {
			workers.emplace_back([this, payload, inserted]() {
				Send(*payload, *inserted);
			});
		} catch (std::exception &) {
			// no thread available (a resource limit, or the last one failed to start): send it right here.
			// Slower, correct, and the same fallback the read path takes in the same situation.
			Send(*payload, *inserted);
			GuardedFinishOne();
		}
	}

	//! Wait for everything submitted, then report the first failure any of them had.
	void Finish() {
		Join();
		std::lock_guard<std::mutex> guard(lock);
		if (!failure.empty()) {
			throw IOException("Xet upload failed: %s", failure);
		}
	}

	//! The producer can throw mid-file - a read error, an out of memory - and a worker holding a
	//! reference to this uploader has to be gone before it dies. Whatever that worker thought is not the
	//! interesting failure: the caller is already reporting something better.
	~HFXetXorbUploader() {
		try {
			Join();
		} catch (...) {
		}
	}

private:
	void Send(const HFXetXorb &xorb, std::atomic<bool> &inserted) {
		string error;
		try {
			inserted = cas.UploadXorb(xorb);
		} catch (std::exception &ex) {
			error = string(ex.what());
		}
		std::lock_guard<std::mutex> guard(lock);
		if (!error.empty() && failure.empty()) {
			// the first failure is the one a user sees, which is fair: they are the same request failing
			failure = error;
		}
		running--;
		idle.notify_all();
	}

	//! the accounting Send does, for a send that ran on the submitting thread instead of a worker
	void GuardedFinishOne() {
		std::lock_guard<std::mutex> guard(lock);
		running--;
		idle.notify_all();
	}

	//! Only ever called by the submitting thread, which is also the only thread that pushes into
	//! `workers` - so the vector itself needs no lock, only the counters above do.
	void Join() {
		for (auto &worker : workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
		workers.clear();
	}

	HFXetCasClient &cas;
	idx_t max_in_flight;
	std::mutex lock;
	std::condition_variable idle;
	idx_t running = 0;
	string failure;
	std::vector<std::thread> workers;
};

idx_t ParallelUploads(optional_ptr<FileOpener> opener, const string &path) {
	if (opener) {
		Value value;
		FileOpenerInfo info;
		info.file_path = path;
		if (FileOpener::TryGetCurrentSetting(opener, Identifier("hf_write_max_parallel_uploads"), value, info) &&
		    !value.IsNull()) {
			auto configured = value.GetValue<idx_t>();
			if (configured > 0) {
				return configured;
			}
		}
	}
	return HF_DEFAULT_WRITE_MAX_PARALLEL;
}

//! Which codec the chunks of this file's xorbs get. `hf_xet_compression` is a string rather than a
//! boolean because the format has more than two answers and one of them (bg4-LZ4) is worth adding
//! later without renaming a setting.
HFXetCompressionScheme ChunkCompression(optional_ptr<FileOpener> opener, const string &path) {
	if (opener) {
		Value value;
		FileOpenerInfo info;
		info.file_path = path;
		if (FileOpener::TryGetCurrentSetting(opener, Identifier("hf_xet_compression"), value, info) && !value.IsNull()) {
			auto configured = StringUtil::Lower(value.ToString());
			if (configured == "none") {
				return HFXetCompressionScheme::NONE;
			}
			if (configured == "lz4") {
				return HFXetCompressionScheme::LZ4;
			}
			throw InvalidInputException("hf_xet_compression must be 'lz4' or 'none', got '%s'", configured);
		}
	}
	return HFXetCompressionScheme::LZ4;
}

} // namespace

HFXetUploadResult HFXetUploadFileContents(DatabaseInstance &db, FileSystem &fs, optional_ptr<FileOpener> opener,
                                         const HFTarget &target, const string &path, const string *sha256_hex) {
	HFXetUploadResult result;

	// The token route is per repository, so the file being uploaded has to live in one - which is also
	// the honest scope: a CAS token lets you write content for one repo, not for any file you can read.
	HFXetCasClient cas(db, opener, target.connection, HFXetCasClient::WriteTokenUrl(target));

	// Exchange the token here, on the thread that owns the opener, before handing a client to the
	// uploaders below: a FileOpener is thread-affine, so those clients run without one.
	{
		string cas_url, access_token;
		cas.Authorize(cas_url, access_token);
	}
	auto worker_cas = cas.ForWorkerThread();
	HFXetXorbUploader uploader(*worker_cas, ParallelUploads(opener, path));

	// no opener passed: in practice `fs` is the context's OpenerFileSystem, which supplies the opener
	// itself and refuses a caller-provided one. `opener` is still what the token exchange below reads
	// secrets and settings from.
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("could not open '%s' to upload its content", path);
	}
	FileHandle &file = *handle;

	HFXetChunker chunker;
	HFXetXorbBuilder builder(ChunkCompression(opener, path));
	string buffer(HF_DEFAULT_READ_BUFFER, '\0');
	auto file_size = file.GetFileSize();
	idx_t offset = 0;

	// The shard is written at the end, but it describes what happened here, so the upload keeps the
	// bookkeeping as it goes: the file's chunks in file order (the verification hashes are ranges over
	// them), and one segment per xorb, since xorbs are filled in file order.
	vector<HFXetHashNode> file_chunks;
	vector<HFXetShardXorb> shard_xorbs;
	vector<HFXetShardSegment> segments;
	// one flag per xorb, in the same order as `result.xorbs`: a worker writes its own, and a shared_ptr
	// keeps it at a stable address while that vector grows
	vector<shared_ptr<std::atomic<bool>>> insert_flags;

	auto flush_xorb = [&](bool force) {
		// A xorb is closed when it is full, and only at a chunk boundary otherwise: the 64 MiB and
		// 8192 chunk limits are per object, and splitting a chunk across two xorbs would not be the
		// same content.
		if (builder.ChunkCount() == 0 || !(builder.IsFull() || force)) {
			return;
		}
		auto uncompressed = builder.UncompressedBytes();
		auto xorb = builder.Finish();

		// Everything the shard will say about this xorb is written down *here*, in file order, while the
		// bytes themselves wait for a free stream: the CAS's answer (`inserted`) is the only fact an
		// upload comes back with, and it is collected from the flags below once every POST has landed.
		HFXetXorbUpload uploaded;
		uploaded.hash = xorb.hash;
		uploaded.num_chunks = xorb.chunks.size();
		uploaded.bytes = xorb.bytes.size();
		uploaded.uncompressed_bytes = uncompressed;
		uploaded.inserted = false;
		result.xorbs.push_back(uploaded);
		auto inserted = make_shared_ptr<std::atomic<bool>>(false);
		insert_flags.push_back(inserted);

		HFXetShardXorb shard_xorb;
		shard_xorb.hash = xorb.hash;
		uint32_t byte_range_start = 0;
		for (idx_t i = 0; i < xorb.chunks.size(); i++) {
			HFXetShardXorbChunk chunk;
			chunk.hash = xorb.chunks[i].hash;
			chunk.byte_range_start = byte_range_start;
			chunk.unpacked_bytes = xorb.chunks[i].uncompressed_size;
			// the reference marks the chunk that begins a file, which is what lets the CAS offer this
			// content to a *different* repository's dedup query later. One file per upload, so this is
			// the first chunk of the first xorb and nothing else
			chunk.starts_a_file = segments.empty() && shard_xorb.chunks.empty() && result.xorbs.size() == 1;
			shard_xorb.chunks.push_back(chunk);

			auto chunk_bytes = xorb.chunks[i].uncompressed_size;
			byte_range_start += chunk_bytes;
			file_chunks.push_back({chunk.hash, chunk_bytes});
		}

		HFXetShardSegment segment;
		segment.xorb_hash = xorb.hash;
		segment.unpacked_bytes = uint32_t(uncompressed);
		// Xorb local, not positions in the file: a reader finds the xorb by hash and then slices *its*
		// chunk list, so indices are into the block above. One segment per xorb, so the segment is the
		// whole xorb - and a shard that spells this as file offsets is refused by the CAS with
		// "invalid shard failed to verify file", which is how it was found.
		segment.chunk_index_start = 0;
		segment.chunk_index_end = uint32_t(xorb.chunks.size());
		segment.file_chunk_start = uint32_t(file_chunks.size() - xorb.chunks.size());
		segment.file_chunk_end = uint32_t(file_chunks.size());
		segments.push_back(segment);
		shard_xorbs.push_back(std::move(shard_xorb));

		// only now is the xorb handed over - Submit takes it, and the bookkeeping above is the last thing
		// that reads it here
		uploader.Submit(std::move(xorb), inserted);
	};

	while (offset < file_size) {
		auto want = std::min<idx_t>(buffer.size(), file_size - offset);
		// FileSystem::Read fills exactly `want` bytes or throws, so there is no short read to handle
		// here - and a chunker must not see a partial block anyway, since gear positions depend on the
		// bytes that precede them.
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

	// Nothing names this file to the CAS until every one of its xorbs has landed: the shard below is
	// resolved against them, and a shard for content still on the wire would be refused.
	uploader.Finish();
	for (idx_t i = 0; i < result.xorbs.size(); i++) {
		result.xorbs[i].inserted = insert_flags[i]->load();
	}

	// The file's own hash is a keyed hash of its merkle root, un-salted: an upload session has no
	// reason to pick a salt, and a salt would make the same content hash differently per upload.
	result.file_hash = HFXetFileHash(file_chunks);

	vector<HFXetHash> chunk_hashes;
	for (auto &chunk : file_chunks) {
		chunk_hashes.push_back(chunk.hash);
	}
	HFXetShardFile shard_file;
	shard_file.file_hash = result.file_hash;
	shard_file.segments = segments;
	shard_file.chunk_hashes = chunk_hashes;
	// The sha256 metadata extension is what ties this content to the sha256 a git-backed commit will
	// name it by, so a git upload passes it and a bucket upload does not (huggingface_hub uploads bucket
	// adds the same way, with SKIP_SHA256). It travels as a DataHash, which parses hex as four
	// little-endian words - the same convention as every other hash in the shard, and not the naive one.
	if (sha256_hex && !sha256_hex->empty()) {
		shard_file.has_sha256 = true;
		auto parsed = HFXetHash::FromHex(*sha256_hex);
		for (idx_t i = 0; i < 32; i++) {
			shard_file.sha256[i] = parsed.bytes[i];
		}
	}

	HFXetShardBuilder shard;
	for (auto &xorb : shard_xorbs) {
		shard.AddXorb(std::move(xorb));
	}
	shard.AddFile(std::move(shard_file));

	auto shard_bytes = shard.Serialize();
	result.shard_bytes = shard_bytes.size();
	cas.UploadShard(shard_bytes, result.shard_hash, result.shard_result);

	return result;
}

} // namespace duckdb
