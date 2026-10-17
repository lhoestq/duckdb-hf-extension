//===----------------------------------------------------------------------===//
// A xorb: the object the CAS actually stores. Xet uploads are a list of these, each carrying a
// run of chunks, addressed by the merkle hash of the chunks inside it.
//
// A port of the serialization in `xet_core_structures::xorb_object` (xorb_object_format.rs and
// xorb_chunk_format.rs). The chunk section is a plain sequence of
// "<8-byte header><chunk bytes>" per chunk; the footer names the chunk hashes, the physical
// boundaries, the uncompressed boundaries, and the merkle root. Layout bugs here are not caught by
// an upload "working" - the hash covers chunk *contents*, so a misaligned footer can still produce
// the right xorb hash - which is why the self-test pins the whole serialized byte stream.
//===----------------------------------------------------------------------===//

#pragma once
#include "hf_common.hpp"
#include "hf_xet_hash.hpp"

namespace duckdb {

//! xet_core_structures::xorb_object::constants. The limits are on *uncompressed* bytes and chunk
//! count: the CAS addresses a xorb by the hash of its chunks and has no view of our layout.
constexpr idx_t HF_XET_MAX_XORB_BYTES = 64 * 1024 * 1024;
constexpr idx_t HF_XET_MAX_XORB_CHUNKS = 8 * 1024;
//! the footer's reserved trailing buffer, of which the first bytes carry a per-upload nonce
constexpr idx_t HF_XET_XORB_FOOTER_BUFFER_LEN = 16;
constexpr idx_t HF_XET_XORB_NONCE_LEN = 4;

//! The per-chunk header's compression byte. `Auto` is a *policy* choice, never a stored value: a
//! serialized chunk always says what it actually did to the bytes.
enum class HFXetCompressionScheme : uint8_t {
	NONE = 0,
	LZ4 = 1,
	BYTE_GROUPING_4_LZ4 = 2,
	AUTO = 99,
};

struct HFXetXorbChunk {
	HFXetHash hash;
	//! size in the file - the size the merkle tree is built over, and what the footer's unpacked
	//! offsets accumulate
	uint32_t uncompressed_size = 0;
	//! byte just past this chunk in the serialized object, header included
	uint32_t physical_end = 0;
};

//! A complete, serialized xorb, ready to hand to the CAS.
struct HFXetXorb {
	string bytes;
	HFXetHash hash;
	vector<HFXetXorbChunk> chunks;
};

//! Accumulates chunks into one xorb. Feed it the chunks a HFXetChunker produces; when IsFull() says
//! stop, finish this one and start the next.
class HFXetXorbBuilder {
public:
	explicit HFXetXorbBuilder(HFXetCompressionScheme scheme = HFXetCompressionScheme::NONE);

	bool IsFull() const;
	//! Hash the chunk and append it. Throws if the caller kept going past a xorb's limits, rather
	//! than silently producing an object the CAS will refuse.
	void AddChunk(const_data_ptr_t data, idx_t len);

	//! The merkle root of the chunks added so far - the name this xorb will be stored under.
	HFXetHash Hash() const;
	//! Write the footer and hand over the bytes. `nonce` goes into the footer's reserved buffer and
	//! is excluded from the hash, which is what lets two byte-identical uploads serialize to
	//! distinguishable objects; null writes zeros, which is what the reference writes and what the
	//! vectors pin.
	HFXetXorb Finish(const uint8_t *nonce = nullptr);

	idx_t ChunkCount() const {
		return chunks.size();
	}
	uint64_t UncompressedBytes() const {
		return uncompressed_bytes;
	}
	uint64_t PhysicalBytes() const {
		return body.size();
	}
	const vector<HFXetXorbChunk> &Chunks() const {
		return chunks;
	}

private:
	//! a xorb's footer is fixed per chunk count: 92 bytes of framing plus 40 per chunk
	static idx_t FooterLength(idx_t num_chunks);

	HFXetCompressionScheme scheme;
	string body;
	vector<HFXetXorbChunk> chunks;
	vector<HFXetHashNode> nodes;
	uint64_t uncompressed_bytes = 0;
};

//===----------------------------------------------------------------------===//
// Reference vectors (generated into src/hf_xet_vectors.cpp by scripts/xet_oracle.py)
//===----------------------------------------------------------------------===//

//! What the reference writer produces for an input: the whole serialized object is pinned by
//! `fingerprint` (the data-keyed hash of its bytes), and `xorb_hash` separately, so a footer bug
//! and a hashing bug do not hide each other.
struct HFXetXorbVector {
	const char *name;
	HFXetTestInput input;
	uint64_t size;
	uint64_t seed;
	const char *pattern;
	HFXetCompressionScheme scheme;
	uint32_t num_chunks;
	uint32_t length;
	const char *xorb_hash;
	const char *fingerprint;
	//! comma-separated lists, as in the hash vectors: uncompressed chunk sizes, then the footer's
	//! physical boundary offsets
	const char *chunk_sizes;
	const char *chunk_physical_ends;
};

const vector<HFXetXorbVector> &HFXetXorbVectors();

//! Replay the xorb vectors into `results` (check name, passed, expected, got); returns the count.
idx_t HFXetXorbSelfTest(vector<std::tuple<string, bool, string, string>> &results);

} // namespace duckdb
