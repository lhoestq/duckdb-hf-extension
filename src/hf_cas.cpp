#include "hf_cas.hpp"

#include "hf_xet_dedup.hpp"

#include "hf_http.hpp"
#include "hf_xet_chunk.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
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

bool HFXetCasClient::QueryChunk(const HFXetHash &chunk_hash, string &shard_bytes, const string &prefix) {
	string cas_url, access_token;
	Authorize(cas_url, access_token);

	auto url = HFUtil::Join(cas_url, "v1/chunks/" + prefix + "/" + chunk_hash.Hex());
	HTTPHeaders headers;
	headers.Insert("Authorization", "Bearer " + access_token);

	HFClient client(db, opener, connection, url);
	// The retry policy is the one the uploads run with (429 and 503 are retried, a 404 is an answer and
	// is not). What is deliberately not here is a retry of a non-200 outside that: a dedup query that
	// fails costs an upload of content that may already be stored, which is slow and correct, whereas a
	// dedup query that is *treated* as a miss when the service was actually refusing the token is slow,
	// correct and invisible - so the status has to be reported, not swallowed.
	string body;
	auto status = client.GetStatus(url, body, headers, false);
	if (status == 404) {
		shard_bytes.clear();
		return false;
	}
	if (status != 200) {
		// HFClient::ThrowHTTPError wants an HTTPResponse, and rebuilding one to phrase this is worse than
		// just saying what happened: the status, the chunk, and what the CAS bothered to answer.
		throw IOException("the Xet content store answered %d for a dedup query about chunk %s: %s", status,
		                  chunk_hash.Hex(), body.size() > 200 ? body.substr(0, 200) : body);
	}
	shard_bytes = std::move(body);
	return true;
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

//! Whether to ask the CAS what it already holds before uploading a file's chunks.
//!
//! Asking costs a round trip per 256 chunks and, on a hit, a shard to parse. Not asking costs uploading
//! bytes the store already has, which is the one thing content-defined chunking exists to avoid - so the
//! default is on, and the setting is there for the case that earns it: a dedup pointer names an xorb this
//! repository never uploaded, so a file whose shard points at someone else's content can only be read by
//! something that can reach that content too (docs/XET.md, M9).
bool DedupQueries(optional_ptr<FileOpener> opener, const string &path) {
	if (opener) {
		Value value;
		FileOpenerInfo info;
		info.file_path = path;
		if (FileOpener::TryGetCurrentSetting(opener, Identifier("hf_xet_upload_dedup"), value, info) &&
		    !value.IsNull()) {
			return value.GetValue<bool>();
		}
	}
	return true;
}

//! How much the re-reader holds at a time: ~16 chunks, which is what turns a run of wanted chunks into
//! one read instead of sixteen syscalls. Bigger would only read more of a skipped run; the chunker's own
//! maximum chunk size (128 KiB) is what has to fit, four times over.
constexpr idx_t HF_UPLOAD_REREAD_BUFFER = 1 * 1024 * 1024;

//! Re-reads the chunks a two-pass upload still needs, in file order.
//!
//! Pass A worked out where every chunk starts and how big it is; pass B reads back only the ones that are
//! not already stored. Those arrive in file order but not contiguously - a matched run is skipped - so the
//! naive version is one seek and one read per 64 KiB chunk, which turns a storage-bound upload into some
//! 16 000 syscalls per GiB for content that is sitting next to each other. Buffering makes a skip a seek
//! only when the wanted chunk lies past what is held, and bounds the over-read of a long skipped run at
//! one buffer.
class HFChunkReader {
public:
	HFChunkReader(FileSystem &fs_p, FileHandle &file_p, idx_t buffer_size_p)
	: fs(fs_p), file(file_p), buffer(buffer_size_p, '\0') {
	}

	//! The bytes of one chunk, valid until the next call. A chunk bigger than the buffer is a bug rather
	//! than a case to handle: the chunker's own limit is what bounds a chunk, and it is a quarter of this.
	const_data_ptr_t Read(uint64_t offset, idx_t len) {
		if (len > buffer.size()) {
			throw IOException("a %llu-byte chunk does not fit the %llu-byte upload buffer", len, buffer.size());
		}
		if (offset >= held_start && offset + len <= held_start + held) {
			return const_data_ptr_cast(buffer.data()) + (offset - held_start);
		}
		// Refill from the wanted chunk's own offset - reading what lies before it would be bytes nobody
		// asked for, and after a long skipped run there can be a great many of them.
		auto file_size = file.GetFileSize();
		auto want = std::min<idx_t>(buffer.size(), file_size - offset);
		fs.Read(file, buffer.data(), int64_t(want), offset);
		held_start = offset;
		held = want;
		return const_data_ptr_cast(buffer.data());
	}

private:
	FileSystem &fs;
	FileHandle &file;
	string buffer;
	//! where in the file what is held starts, and how much of it there is. Both zero before the first read.
	uint64_t held_start = 0;
	idx_t held = 0;
};

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

	// ---- pass A: chunk and hash the file before uploading any of it ----
	//
	// Two passes rather than one because the chunks that should not be uploaded can only be known by
	// asking, and asking needs the file's chunk hashes. The reference has the same shape (query for the
	// file, then upload what came back unmatched), and it is the only shape in which a dedup pointer can
	// be acted on rather than discovered after the bytes have gone up. What it costs is a hash and a size
	// per chunk: 40 bytes, about 2 MB per GiB at the chunker's 64 KiB average - against the 64 MiB of
	// in-flight xorb buffers already in play, so this is not what bounds file size.
	vector<HFXetHashNode> file_chunks;
	{
		HFXetChunker chunker;
		string buffer(HF_DEFAULT_READ_BUFFER, '\0');
		auto file_size = file.GetFileSize();
		idx_t offset = 0;
		while (offset < file_size) {
			auto want = std::min<idx_t>(buffer.size(), file_size - offset);
			// FileSystem::Read fills exactly `want` bytes or throws, so there is no short read to handle
			// here - and a chunker must not see a partial block anyway, since gear positions depend on the
			// bytes that precede them.
			fs.Read(file, buffer.data(), int64_t(want), offset);
			offset += want;
			chunker.Process(const_data_ptr_cast(buffer.data()), want,
			                [&](const_data_ptr_t data, idx_t len) { file_chunks.push_back({HFXetChunkHash(data, len), len}); });
		}
		chunker.Finish([&](const_data_ptr_t data, idx_t len) { file_chunks.push_back({HFXetChunkHash(data, len), len}); });
	}

	vector<HFXetHash> chunk_hashes;
	chunk_hashes.reserve(file_chunks.size());
	for (auto &chunk : file_chunks) {
		chunk_hashes.push_back(chunk.hash);
	}

	// ---- what the CAS says it already holds ----
	//
	// Every query goes out before a single byte is uploaded. An empty table matches nothing, which is how
	// the setting turns the whole thing off: the declaration stays, the loop does not run.
	HFXetDedupTable stored;
	if (!chunk_hashes.empty() && DedupQueries(opener, path)) {
		auto queried = HFXetDedupQuery(cas, chunk_hashes, stored);
		result.dedup_queries = queried.queries;
		result.dedup_hits = queried.hits;
		result.dedup_shard_failures = queried.parse_failures;
	}

	HFXetXorbBuilder builder(ChunkCompression(opener, path));
	// The shard is written at the end, but it describes what happened here, so the upload keeps the
	// bookkeeping as it goes: one shard xorb per xorb uploaded, and a segment per stretch of the file that
	// landed in one.
	vector<HFXetShardXorb> shard_xorbs;
	vector<HFXetShardSegment> segments;
	// one flag per xorb, in the same order as `result.xorbs`: a worker writes its own, and a shared_ptr
	// keeps it at a stable address while that vector grows
	vector<shared_ptr<std::atomic<bool>>> insert_flags;

	//! A stretch of consecutive chunks of this file that went into one xorb. "Piece" because a run of new
	//! data ends in two different situations that are not the same event: the CAS turns out to hold the
	//! next part of the file (the run ends, the xorb carries on), or the xorb reaches its limits (the xorb
	//! ends, the run carries on). Segments are per piece, which is the only way both can be true.
	struct Piece {
		idx_t file_chunk_start = 0;
		idx_t xorb_chunk_start = 0;
		idx_t count = 0;
		uint64_t bytes = 0;
	};
	//! pieces closed inside the xorb currently open, in the order they were closed
	vector<Piece> pieces;
	//! the piece being fed right now, empty when the last chunk fed belonged to a matched run
	Piece piece;
	//! which chunk of the file each chunk of the open xorb came from. Needed because a xorb's chunks are
	//! no longer one contiguous stretch of the file once a matched run has been skipped over - and the
	//! file's first chunk is looked up through it (see starts_a_file below).
	vector<idx_t> xorb_file_positions;

	auto close_piece = [&]() {
		if (piece.count > 0) {
			pieces.push_back(piece);
			piece.count = 0;
		}
	};

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
			// The reference marks the chunk that begins a file, which is what lets the CAS offer this
			// content to a *different* repository's dedup query later. Which chunk that is is a question
			// about the file, not about this xorb: with dedup the file's first chunk can be one the CAS
			// already had, and then no chunk in any xorb uploaded here starts the file - the shard that
			// already holds it is what indexes it, and flagging a chunk of our own on top of that would
			// describe the same content twice.
			chunk.starts_a_file = xorb_file_positions[i] == 0;
			shard_xorb.chunks.push_back(chunk);
			byte_range_start += xorb.chunks[i].uncompressed_size;
		}

		// Segments, one per piece. The piece being fed ends in this xorb whether or not its run continues
		// into the next one, so it is closed here and reopened by the next chunk that is added.
		if (piece.count > 0) {
			pieces.push_back(piece);
			piece.count = 0;
		}
		for (auto &closed : pieces) {
			HFXetShardSegment segment;
			segment.xorb_hash = xorb.hash;
			// Xorb local, not positions in the file: a reader finds the xorb by hash and then slices *its*
			// chunk list, so the indices are into the block above. A shard that spells these as file
			// offsets is refused by the CAS with "invalid shard failed to verify file", which is how it was
			// found. One segment per xorb is the case with nothing deduped: 0 to the whole xorb.
			segment.chunk_index_start = uint32_t(closed.xorb_chunk_start);
			segment.chunk_index_end = uint32_t(closed.xorb_chunk_start + closed.count);
			segment.file_chunk_start = uint32_t(closed.file_chunk_start);
			segment.file_chunk_end = uint32_t(closed.file_chunk_start + closed.count);
			// the sum of this piece's chunks' uncompressed sizes, which is what a segment has to say and
			// what the CAS checks the chunk table against. Bounded by the xorb's own 64 MiB limit.
			segment.unpacked_bytes = uint32_t(closed.bytes);
			segments.push_back(segment);
		}
		pieces.clear();
		xorb_file_positions.clear();
		shard_xorbs.push_back(std::move(shard_xorb));

		// only now is the xorb handed over - Submit takes it, and the bookkeeping above is the last thing
		// that reads it here
		uploader.Submit(std::move(xorb), inserted);
	};

	auto add_chunk = [&](idx_t file_index, const_data_ptr_t data, idx_t len) {
		if (builder.WouldOverflow(len)) {
			flush_xorb(true);
		}
		if (piece.count == 0) {
			// where a piece starts in the xorb is wherever the builder is up to; where it starts in the
			// file is wherever the walk has got to. Both are only knowable at the moment its first chunk
			// goes in, which is why a piece starts here rather than being declared by the walk.
			piece.file_chunk_start = file_index;
			piece.xorb_chunk_start = builder.ChunkCount();
			piece.bytes = 0;
		}
		builder.AddChunk(data, len);
		piece.count++;
		piece.bytes += len;
		xorb_file_positions.push_back(file_index);
	};

	// ---- pass B: read and upload what nobody else has already paid for ----
	//
	// The chunk boundaries come from pass A, so a chunk is re-read by position rather than re-chunked:
	// re-chunking a subrange would move the boundaries, and content-defined chunking whose boundaries move
	// depending on what you hand it is not content-defined.
	HFChunkReader reader(fs, file, HF_UPLOAD_REREAD_BUFFER);
	uint64_t file_offset = 0;
	idx_t chunk_index = 0;
	while (chunk_index < file_chunks.size()) {
		HFXetDedupRun match;
		if (stored.Match(chunk_hashes, chunk_index, match) > 0) {
			// These bytes are already stored. Nothing from the file is read for them and nothing is
			// uploaded: the shard names the xorb they live in, and a file described that way depends on
			// content this repository may never have stored itself - the reason the asking is a setting.
			close_piece();
			HFXetShardSegment segment;
			segment.xorb_hash = match.xorb_hash;
			// Copied from the xorb that is pointed into, not from anything this upload knows: the CAS
			// verifies a segment against the stored xorb, and its own flags are part of what it stored.
			segment.xorb_flags = match.xorb_flags;
			segment.chunk_index_start = match.chunk_index_start;
			segment.chunk_index_end = match.chunk_index_end;
			segment.file_chunk_start = uint32_t(match.file_chunk);
			segment.file_chunk_end = uint32_t(match.file_chunk + match.count);
			segment.unpacked_bytes = uint32_t(match.unpacked_bytes);
			segments.push_back(segment);
			result.deduped_chunks += match.count;
			for (idx_t i = 0; i < match.count; i++) {
				file_offset += file_chunks[chunk_index + i].size;
			}
			chunk_index += match.count;
			continue;
		}
		auto size = file_chunks[chunk_index].size;
		add_chunk(chunk_index, reader.Read(file_offset, size), size);
		file_offset += size;
		chunk_index++;
	}
	flush_xorb(true);

	// Segments were emitted whenever the fact about them was known - a matched run as soon as it was
	// found, a piece of new data when its xorb closed - which is not file order once the two interleave.
	// A shard's segments are read as a sequence over the file's chunk positions, so they go in that order
	// before the shard is built. Stable, so that a file with nothing deduped - where they were appended in
	// order already - is described byte for byte the way this function described it before dedup existed.
	std::stable_sort(segments.begin(), segments.end(),
	                 [](const HFXetShardSegment &a, const HFXetShardSegment &b) {
		                 return a.file_chunk_start < b.file_chunk_start;
	                 });

	// Nothing names this file to the CAS until every one of its xorbs has landed: the shard below is
	// resolved against them, and a shard for content still on the wire would be refused.
	uploader.Finish();
	for (idx_t i = 0; i < result.xorbs.size(); i++) {
		result.xorbs[i].inserted = insert_flags[i]->load();
	}

	// The file's own hash is a keyed hash of its merkle root, un-salted: an upload session has no
	// reason to pick a salt, and a salt would make the same content hash differently per upload. It is
	// computed over every chunk pass A found, including the ones dedup meant were never uploaded - a
	// file's identity is its content, not the subset of it that this upload happened to send.
	result.file_hash = HFXetFileHash(file_chunks);

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
