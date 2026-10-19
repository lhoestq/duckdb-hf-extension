//===----------------------------------------------------------------------===//
// Xet hashing.
//
// A port of `xet_core_structures::merklehash` (https://github.com/huggingface/xet-core) to C++,
// kept deliberately close to the reference so the two can be diffed by eye. Everything here has to
// be byte-exact: a hash that is merely *a* hash of the data is useless, it has to be the hash the
// Hub and `hf_xet` compute - a wrong one is silently indistinguishable from a right one at the
// point it is produced. So the port is checked against reference output, see HFXetHashSelfTest and
// scripts/xet_oracle.py.
//
// The primitives are BLAKE3 *keyed* hashes under two fixed keys, plus a merkle tree whose shape
// falls out of the hashes themselves (a "mean tree", branching factor 4).
//===----------------------------------------------------------------------===//

#pragma once

#include "hf_common.hpp"

#include "blake3.h"

namespace duckdb {

//! A 256-bit Xet hash (`MerkleHash` / `DataHash` in xet-core).
struct HFXetHash {
	uint8_t bytes[32];

	HFXetHash() {
		for (idx_t i = 0; i < 32; i++) {
			bytes[i] = 0;
		}
	}

	bool IsZero() const;

	//! The reference client's text form. Note the trap: this is *not* the hex of the digest bytes.
	//! The 32 bytes are read as four little-endian u64 words and each word is printed as 16 hex
	//! digits, so every 8-byte group comes out byte-reversed.
	string Hex() const;
	static HFXetHash FromHex(const string &hex);

	//! URL-safe base64 without padding - how the bucket and CAS endpoints spell a xetHash.
	string Base64() const;

	//! The i'th hash word, read little-endian, the way xet-core reads it.
	uint64_t Word(idx_t i) const;
	//! True when this hash triggers a natural cut in the mean tree (word 3 % branching factor).
	bool IsNaturalCut() const;


	bool operator==(const HFXetHash &other) const {
		for (idx_t i = 0; i < 32; i++) {
			if (bytes[i] != other.bytes[i]) {
				return false;
			}
		}
		return true;
	}
	bool operator!=(const HFXetHash &other) const {
		return !(*this == other);
	}
};

//! The CAS keeps a cross-repository dedup index over a *sample* of chunk hashes: a chunk is findable
//! by a later, unrelated upload when its hash reduces to zero modulo this, or when an uploader flags
//! it explicitly (HF_XET_CHUNK_FLAG_GLOBAL_DEDUP). Both halves are the reference's own -
//! `hash_is_global_dedup_eligible`, and `Rem<u64> for DataHash = self[3].to_le() % rhs` - but which
//! word that reduction reads, and in what byte order, is not something to interpret: a wrong word
//! still yields a 1-in-1024 sample, just of the wrong chunks, and nothing downstream ever notices.
//! So the answer is replayed from xet-core (HFXetDedupVectors: 100 000 candidate hashes through
//! `xet-probe dedup`, all of them agreeing with this function) rather than re-derived from Rust text.
constexpr uint64_t HF_XET_GLOBAL_DEDUP_MODULUS = 1024;
bool HFXetGlobalDedupEligible(const HFXetHash &hash);

//! The key a *stored* shard indexes its chunk hashes under: `DataHash::hmac` = BLAKE3 keyed hash with
//! the shard's footer key over the 32 bytes of the chunk hash. Both operands are byte-order traps: a
//! hash enters as the little-endian bytes of its four words (which is what `HFXetHash::bytes` holds),
//! a key read out of a footer means those same bytes, and a key *hex* string means them byte-reversed
//! inside every word. That is why the CAS's answer for a chunk is not the keyed hash of the hex the
//! query URL carries, and why the construction is replayed from xet-core (HFXetHmacVectors).
HFXetHash HFXetChunkHmac(const HFXetHash &chunk_hash, const HFXetHash &key);

//! `MDBShardInfo::keyed_chunk_hash`: the form a stored shard really holds. An all-zero footer key is
//! the "this shard is unkeyed" sentinel, under which the hash is stored as it was given.
HFXetHash HFXetKeyedChunkHash(const HFXetHash &chunk_hash, const HFXetHash &hmac_key);

//! One node of the merkle tree: a chunk hash (or an interior hash) plus the size it covers.
struct HFXetHashNode {
	HFXetHash hash;
	uint64_t size = 0;
};

//! Streaming keyed BLAKE3. The data key hashes chunk content; the other key is reserved for
//! interior tree nodes and HMAC-style keying (see HFXetFileHashWithSalt).
class HFXetKeyedHasher {
public:
	//! `internal_node = false` uses the data key, `true` the interior node key
	explicit HFXetKeyedHasher(bool internal_node);
	//! any 32-byte key, for the file-hash salt
	explicit HFXetKeyedHasher(const uint8_t key[32]);

	void Update(const_data_ptr_t data, idx_t len);
	//! Finalize into a hash. The hasher is single-use after this, like blake3's C API.
	HFXetHash Finalize();

private:
	blake3_hasher hasher;
};

//! Leaf hash of a chunk of file data.
HFXetHash HFXetChunkHash(const_data_ptr_t data, idx_t len);
//! Interior node hash over already-formatted child text (see HFXetFormatNodeLines).
HFXetHash HFXetInternalHash(const_data_ptr_t data, idx_t len);

//! Render children the way the reference does - one "<child hex> : <child size>\n" per child -
//! which is exactly the byte string the interior node hash is taken over.
void HFXetFormatNodeLines(string &out, const vector<HFXetHashNode> &children);

//! Collapse a list of (hash, size) nodes into the merkle root. This is a xorb's hash, and the base
//! of a file hash. An empty list hashes to all zeros, like `MerkleHash::default()`.
HFXetHash HFXetAggregatedNodeHash(const vector<HFXetHashNode> &nodes);
//! What a bucket's `addFile` names and what a xorb is stored under in the CAS.
inline HFXetHash HFXetXorbHash(const vector<HFXetHashNode> &chunks) {
	return HFXetAggregatedNodeHash(chunks);
}
//! A file's hash: its merkle root, keyed-hashed with the file salt (all zeros when un-salted).
HFXetHash HFXetFileHash(const vector<HFXetHashNode> &chunks);
HFXetHash HFXetFileHashWithSalt(const vector<HFXetHashNode> &chunks, const uint8_t salt[32]);

//===----------------------------------------------------------------------===//
// Reference vectors (src/hf_xet_vectors.cpp, generated by scripts/xet_oracle.py)
//===----------------------------------------------------------------------===//

//! How a vector's input bytes are produced. All three are reproducible from the vector alone - a
//! stored blob goes stale, and a vector that means something on only one machine is not a vector.
enum class HFXetTestInput : uint8_t {
	LITERAL,  //!< `pattern` is the input
	REPEAT,   //!< `pattern` (a zero byte when empty) repeated to `size` bytes
	SPLITMIX, //!< splitmix64 from `seed` as little-endian bytes, truncated to `size`: xet-core's own
};

//! File-level vectors: input bytes, the leaf hash of the whole input, and the hashes over the chunk
//! list the *reference chunker* produces for it. That last pair is where a wrong chunking, tree
//! shape or node format shows up as a wrong hash.
struct HFXetHashVector {
	const char *name;
	HFXetTestInput kind;
	const char *pattern;
	uint64_t size;
	uint64_t seed;
	const char *chunk_hash;
	const char *chunk_base64;
	//! plain hex of the same digest bytes, i.e. deliberately not `chunk_hash`
	const char *chunk_plain_hex;
	//! the reference chunker's chunk sizes for this input, comma separated. The hashes below are
	//! computed over this list, so a mismatch here says *why* they mismatch
	const char *chunk_sizes;
	const char *file_hash;
	const char *xorb_hash;
};

//! Direct checks on the interior-node hash, whose input is formatted text rather than file data.
struct HFXetInternalHashVector {
	const char *name;
	const char *input;
	const char *hash;
};

//! One digest byte string in all three of the forms the port has to agree with: raw bytes (as plain
//! hex), the reference's word-swapped `hex()`, and `base64()`.
struct HFXetRenderingVector {
	const char *name;
	const char *raw_hex;
	const char *hex;
	const char *base64;
};

//! Tree vectors lifted from xet-core's own correctness table: an explicit child list and the xorb
//! hash and salted file hash it must produce. These are what actually exercise the mean-tree cuts.
struct HFXetTreeVector {
	const char *name;
	idx_t count;
	const char *const *hashes;
	const uint64_t *sizes;
	const char *xorb_hash;
	const char *salt;
	const char *file_hash;
};

const vector<HFXetHashVector> &HFXetHashVectors();
const vector<HFXetInternalHashVector> &HFXetInternalHashVectors();
const vector<HFXetRenderingVector> &HFXetRenderingVectors();

//! (a hash as the reference prints it, whether xet-core calls it dedup eligible)
struct HFXetDedupVector {
	const char *hash;
	bool eligible;
};

//! (a key, a chunk hash, and what xet-core's `DataHash::hmac` keys it into)
struct HFXetHmacVector {
	const char *key;
	const char *hash;
	const char *keyed;
};

const vector<HFXetHmacVector> &HFXetHmacVectors();

const vector<HFXetDedupVector> &HFXetDedupVectors();
const vector<HFXetTreeVector> &HFXetTreeVectors();

//! Reproduce a vector's input bytes.
//! Materialize a vector's input bytes from its reproducible spec - shared by the hash and xorb
//! self-tests, so both run the same bytes the oracle ran.
string HFXetTestInputBytes(HFXetTestInput kind, uint64_t size, uint64_t seed, const char *pattern);
string HFXetVectorInput(const HFXetHashVector &vector);

//! Plain hex back into bytes. Generated fixtures that are whole byte streams - a stored shard, a xorb -
//! are carried as hex, and more than one self-test has to read them back.
string HFXetSelfTestBytesFromHex(const char *hex);

//! Check every stored vector. One row per check: (name, passed, expected, got). Surfaced by the
//! hf_xet_selftest() table function so these run in CI like any other test. Returns failures.
idx_t HFXetHashSelfTest(vector<std::tuple<string, bool, string, string>> &results);

} // namespace duckdb
