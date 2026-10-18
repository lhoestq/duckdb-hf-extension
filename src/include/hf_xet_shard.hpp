//===----------------------------------------------------------------------===//
// The metadata shard: the object that turns stored chunks into a readable file.
//
// A xorb says "these chunks exist". A shard says "this file is these chunks, in this order, in these
// xorbs" - without it, uploaded content is unreachable, which is exactly the state M4 leaves uploads
// in. A port of the serialization in `xet_core_structures::metadata_shard` (shard_format.rs,
// file_structs.rs, xorb_structs.rs), and specifically of what a *session* upload sends: the local
// shard file carries lookup tables and a footer, and the bytes handed to `POST /v1/shards` are that
// file truncated at `file_lookup_offset` with the header's `footer_size` rewritten to zero
// (`read_shard_to_bytes_remove_footer` in xet_data/src/processing/shard_interface/native.rs). So an
// uploaded shard is a header, one entry group per file, a bookend, one entry group per xorb, a
// bookend - and nothing else. A reader walks it sequentially for the same reason.
//
// Every entry is 48 bytes: a 32-byte hash and four u32s, little-endian, with the fields in an order
// that differs between the file-side and xorb-side structs. A reader that reads past an entry does
// not complain, it reconstructs a different file, so this file is checked against reference shards
// rather than reasoned about (HFXetShardSelfTest, scripts/xet_oracle.py).
//===----------------------------------------------------------------------===//

#pragma once
#include "hf_common.hpp"
#include "hf_xet_chunk.hpp"
#include "hf_xet_hash.hpp"
#include "hf_xet_xorb.hpp"

#include <tuple>

namespace duckdb {

//! The uploaded shard header: a 32-byte tag, a version, and a footer size that is deliberately zero
//! because the footer (and the lookup tables before it) are stripped before the bytes are sent.
constexpr uint64_t HF_XET_SHARD_HEADER_VERSION = 2;
//! tag(32) + version(8) + footer_size(8): the byte offset the file section starts at
constexpr idx_t HF_XET_SHARD_HEADER_SIZE = 48;
//! FileDataSequenceHeader.file_flags. Verification entries follow the segments one per segment; the
//! metadata extension is a single trailing entry carrying the file's sha256.
constexpr uint32_t HF_XET_FILE_FLAG_WITH_VERIFICATION = 0x80000000u;
constexpr uint32_t HF_XET_FILE_FLAG_WITH_METADATA_EXT = 0x40000000u;
//! XorbChunkSequenceEntry.flags, set on a chunk that starts a file. Advisory, and cheap to honour:
//! the CAS uses it to populate a cross-repository dedup table - measured, not assumed, and this flag is
//! the *only* chunk of ours that table ever answers for, because a reference client also flags chunks whose
//! hash is 0 mod 1024 and this writer does not (docs/XET.md, the M9 section).
constexpr uint32_t HF_XET_CHUNK_FLAG_GLOBAL_DEDUP = 0x80000000u;
//! every entry in both sections is exactly this many bytes
constexpr idx_t HF_XET_SHARD_ENTRY_SIZE = 48;

//! One run of a file's chunks that lives in one xorb.
//!
//! Two coordinate systems meet here and the shard stores only the first:
//!  * `chunk_index_start/end` are positions in the *xorb's* chunk list, because a reader resolves the
//!    xorb by hash and then slices that xorb. File-wide positions here are refused by the CAS with
//!    "invalid shard failed to verify file".
//!  * `file_chunk_start/end` are the same chunks as positions in the file's own list, which is what the
//!    segment's verification hash is computed over. The shard does not store them.
struct HFXetShardSegment {
	HFXetHash xorb_hash;
	uint32_t unpacked_bytes = 0;
	uint32_t chunk_index_start = 0;
	uint32_t chunk_index_end = 0;
	uint32_t file_chunk_start = 0;
	uint32_t file_chunk_end = 0;
};

//! A file as the shard describes it: its hash, and the segments its content is reconstructed from.
struct HFXetShardFile {
	HFXetHash file_hash;
	vector<HFXetShardSegment> segments;
	//! the file's chunk hashes in file order. The verification entries are derived from these, and
	//! they are derived per segment, so a file cannot be described by its segments alone
	vector<HFXetHash> chunk_hashes;
	//! The file's sha256, when the uploader knows it. Bucket uploads skip it (huggingface_hub passes
	//! SKIP_SHA256) because a bucket entry is addressed by its xet hash; a git-backed repo needs it,
	//! because there the commit names the file by sha256 and this is the only place the two meet.
	bool has_sha256 = false;
	uint8_t sha256[32] = {0};
};

struct HFXetShardXorbChunk {
	HFXetHash hash;
	//! where the chunk starts inside the xorb's *uncompressed* stream
	uint32_t byte_range_start = 0;
	uint32_t unpacked_bytes = 0;
	//! this chunk is the first chunk of some file in this shard
	bool starts_a_file = false;
};

struct HFXetShardXorb {
	HFXetHash hash;
	vector<HFXetShardXorbChunk> chunks;
};

//! The hash a file's verification entry carries for a range of its chunks: the chunk hashes
//! concatenated raw, keyed-hashed. A reader uses it to check a range it fetched without re-deriving
//! the whole file hash.
HFXetHash HFXetRangeHash(const vector<HFXetHash> &chunk_hashes, idx_t start, idx_t end);

//! Builds the bytes of one session shard. Files and xorbs are collected, not streamed: both sections
//! are written in ascending order of their hash's first word, which is what the lookup tables the
//! reference builds index into.
class HFXetShardBuilder {
public:
	void AddFile(HFXetShardFile file);
	void AddXorb(HFXetShardXorb xorb);

	idx_t FileCount() const {
		return files.size();
	}
	idx_t XorbCount() const {
		return xorbs.size();
	}
	//! every segment of every file, which is what a reader walks
	idx_t SegmentCount() const;

	//! The shard as a session uploads it. Deterministic by construction: the same files and xorbs
	//! produce the same bytes, and the CAS stores them under their hash, so a timestamp here would
	//! give every upload of the same content a different address.
	string Serialize() const;

	//! Ordering helper, exposed for the self-test: xet-core's `truncate_hash` is the first hash word,
	//! and both sections and their lookups are ordered by it.
	static int CompareByTruncatedHash(const HFXetHash &a, const HFXetHash &b);

private:
	vector<HFXetShardFile> files;
	vector<HFXetShardXorb> xorbs;
};

//===----------------------------------------------------------------------===//
// Reference vectors (src/hf_xet_shard_vectors.cpp, generated by scripts/xet_oracle.py)
//===----------------------------------------------------------------------===//

//! One reference shard: the file it describes, and the whole uploaded byte stream that describes it.
struct HFXetShardVector {
	const char *name;
	//! which reproducible input bytes the described file holds (see HFXetTestInput)
	HFXetTestInput kind;
	uint64_t size;
	uint64_t seed;
	const char *pattern;
	//! chunks are grouped into xorbs of at most this many uncompressed bytes. The real limit is
	//! HF_XET_MAX_XORB_BYTES, and a small override is what makes a multi-segment shard cheap to test
	//! with - a genuine second xorb needs 64 MiB of input.
	uint64_t max_xorb_bytes;
	//! DATA_KEY-keyed hash of every byte of the uploaded shard - the fingerprint, as for xorbs
	const char *fingerprint;
	const char *file_hash;
	//! the shard's xorb hashes in the order the shard stores them (ascending by first word)
	const char *xorb_hashes;
	//! the file's segments as "start-end" chunk index ranges, in file order
	const char *segment_ranges;
	const char *verification_hashes;
	uint64_t num_bytes;
	//! the file entry's sha256 metadata extension, or nullptr for a shard that carries none - which is
	//! what a bucket upload sends, since a bucket file is addressed by its xet hash and never by a
	//! sha256 the commit names
	const char *sha256;
};

const vector<HFXetShardVector> &HFXetShardVectors();

//===----------------------------------------------------------------------===//
// Self-test
//===----------------------------------------------------------------------===//

//! A shard writer's failure mode is a shard the CAS accepts and cannot use, or - worse - reads back a
//! different file. So this pins whole serialized shards generated from reference sessions, plus the
//! derived pieces (flags, verification hashes) that a byte comparison alone would not name.
idx_t HFXetShardSelfTest(vector<std::tuple<string, bool, string, string>> &results);

} // namespace duckdb
