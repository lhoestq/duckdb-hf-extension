#include "hf_xet_hash.hpp"

#include "hf_crypto.hpp"
#include "hf_xet_chunk.hpp"

#include <cstring>

#include <cstdint>

namespace duckdb {

namespace {

//! xet_core_structures::merklehash::data_hash - DATA_HASH_KEY and the internal node key, copied
//! verbatim. These keys are part of the format: hashing chunk data under any other key produces a
//! hash that means nothing to the Hub.
const uint8_t HF_XET_DATA_KEY[32] = {102, 151, 245, 119, 91, 149, 80, 222, 49, 53, 203, 172, 165, 151, 24, 28, 157, 
    228, 33, 16, 155, 235, 43, 88, 180, 208, 176, 75, 147, 173, 242, 41};
const uint8_t HF_XET_INTERNAL_NODE_KEY[32] = {1, 126, 197, 199, 165, 71, 41, 150, 253, 148, 102, 102, 180, 138, 2, 
    230, 93, 221, 83, 111, 55, 199, 109, 210, 248, 99, 82, 230, 74, 83, 113, 63};
const uint8_t HF_XET_ZERO_SALT[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

//! mean tree: branching factor 4, so a group holds 2..2*4+1 children (aggregated_hashes.rs)
constexpr idx_t MEAN_TREE_BRANCHING_FACTOR = 4;
constexpr idx_t MIN_MERGE_GROUP = 2;
constexpr idx_t MAX_MERGE_GROUP = 2 * MEAN_TREE_BRANCHING_FACTOR + 1;

uint64_t LoadLE64(const uint8_t *bytes) {
	uint64_t value = 0;
	for (idx_t i = 0; i < 8; i++) {
		value |= uint64_t(bytes[i]) << (8 * i);
	}
	return value;
}

void StoreLE64(uint8_t *bytes, uint64_t value) {
	for (idx_t i = 0; i < 8; i++) {
		bytes[i] = uint8_t(value >> (8 * i));
	}
}

HFXetHash HFBlake3Keyed(const uint8_t key[32], const_data_ptr_t data, idx_t len) {
	HFXetKeyedHasher hasher(key);
	hasher.Update(data, len);
	return hasher.Finalize();
}

//! Interior node over a group of children: keyed hash of their "hex : size\n" lines, with the size
//! covered being the children's sizes summed.
HFXetHashNode HFMergedHashOfSequence(const vector<HFXetHashNode> &nodes, idx_t start, idx_t end) {
	string lines;
	uint64_t total_size = 0;
	for (idx_t i = start; i < end; i++) {
		lines += nodes[i].hash.Hex();
		lines += " : ";
		lines += to_string(nodes[i].size);
		lines += "\n";
		total_size += nodes[i].size;
	}
	HFXetHashNode result;
	result.hash = HFBlake3Keyed(HF_XET_INTERNAL_NODE_KEY, const_data_ptr_cast(lines.data()), lines.size());
	result.size = total_size;
	return result;
}

//! How many nodes the next group swallows: up to the first child whose hash triggers a cut, capped
//! at MAX_MERGE_GROUP, and never below MIN_MERGE_GROUP.
idx_t HFNextMergeCut(const vector<HFXetHashNode> &nodes, idx_t start) {
	auto remaining = nodes.size() - start;
	if (remaining <= MIN_MERGE_GROUP) {
		return nodes.size();
	}
	idx_t end = start + MinValue<idx_t>(MAX_MERGE_GROUP, remaining);
	for (idx_t i = start + MIN_MERGE_GROUP; i < end; i++) {
		if (nodes[i].hash.IsNaturalCut()) {
			return i + 1;
		}
	}
	return end;
}

//===----------------------------------------------------------------------===//
// self-test helpers
//===----------------------------------------------------------------------===//

uint32_t HexDigit(char c) {
	if (c >= '0' && c <= '9') {
		return uint32_t(c - '0');
	}
	if (c >= 'a' && c <= 'f') {
		return uint32_t(c - 'a') + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return uint32_t(c - 'A') + 10;
	}
	return 0xFFFFFFFF;
}

//! digest bytes given as plain hex - only the self-test needs this, since it compares against the
//! reference's `plain_hex` output
HFXetHash HashFromRawHex(const string &raw_hex) {
	HFXetHash result;
	if (raw_hex.size() != 64) {
		return result;
	}
	for (idx_t i = 0; i < 32; i++) {
		auto high = HexDigit(raw_hex[i * 2]);
		auto low = HexDigit(raw_hex[i * 2 + 1]);
		if (high == 0xFFFFFFFF || low == 0xFFFFFFFF) {
			return HFXetHash();
		}
		result.bytes[i] = uint8_t(high * 16 + low);
	}
	return result;
}

string HashToRawHex(const HFXetHash &hash) {
	static const char *HEX_DIGITS = "0123456789abcdef";
	string out;
	for (idx_t i = 0; i < 32; i++) {
		out += HEX_DIGITS[hash.bytes[i] >> 4];
		out += HEX_DIGITS[hash.bytes[i] & 0xF];
	}
	return out;
}

} // namespace

//===----------------------------------------------------------------------===//
// HFXetHash
//===----------------------------------------------------------------------===//

bool HFXetHash::IsZero() const {
	for (idx_t i = 0; i < 32; i++) {
		if (bytes[i] != 0) {
			return false;
		}
	}
	return true;
}

uint64_t HFXetHash::Word(idx_t i) const {
	return LoadLE64(bytes + i * 8);
}

bool HFXetHash::IsNaturalCut() const {
	return Word(3) % MEAN_TREE_BRANCHING_FACTOR == 0;
}

bool HFXetGlobalDedupEligible(const HFXetHash &hash) {
	// The same word the mean tree reads for a natural cut, at the reference's dedup sampling rate. That
	// this word is the one the CAS indexes by - and not merely *a* word, which would still produce a
	// plausible 1-in-1024 sample - is what HFXetDedupVectors exists to prove.
	return hash.Word(3) % HF_XET_GLOBAL_DEDUP_MODULUS == 0;
}

string HFXetHash::Hex() const {
	static const char *HEX_DIGITS = "0123456789abcdef";
	string out;
	out.reserve(64);
	for (idx_t i = 0; i < 4; i++) {
		auto word = Word(i);
		// 15 down to 0, and `digit < 16` keeps the countdown from wrapping at 0
		for (idx_t digit = 15; digit < 16; digit--) {
			out += HEX_DIGITS[(word >> (4 * digit)) & 0xF];
		}
	}
	return out;
}

HFXetHash HFXetHash::FromHex(const string &hex) {
	HFXetHash result;
	if (hex.size() != 64) {
		return result;
	}
	for (idx_t i = 0; i < 4; i++) {
		uint64_t word = 0;
		for (idx_t digit = 0; digit < 16; digit++) {
			auto value = HexDigit(hex[i * 16 + digit]);
			if (value == 0xFFFFFFFF) {
				return HFXetHash();
			}
			word = (word << 4) | value;
		}
		StoreLE64(result.bytes + i * 8, word);
	}
	return result;
}

string HFXetHash::Base64() const {
	return HFBase64::EncodeUrlSafeNoPad(const_data_ptr_cast(bytes), 32);
}

//===----------------------------------------------------------------------===//
// hashing
//===----------------------------------------------------------------------===//

HFXetKeyedHasher::HFXetKeyedHasher(const uint8_t key[32]) {
	blake3_hasher_init_keyed(&hasher, key);
}

HFXetKeyedHasher::HFXetKeyedHasher(bool internal_node)
    : HFXetKeyedHasher(internal_node ? HF_XET_INTERNAL_NODE_KEY : HF_XET_DATA_KEY) {
}

void HFXetKeyedHasher::Update(const_data_ptr_t data, idx_t len) {
	if (len > 0) {
		blake3_hasher_update(&hasher, data, len);
	}
}

HFXetHash HFXetKeyedHasher::Finalize() {
	HFXetHash result;
	blake3_hasher_finalize(&hasher, result.bytes, BLAKE3_OUT_LEN);
	return result;
}

HFXetHash HFXetChunkHash(const_data_ptr_t data, idx_t len) {
	return HFBlake3Keyed(HF_XET_DATA_KEY, data, len);
}

HFXetHash HFXetInternalHash(const_data_ptr_t data, idx_t len) {
	return HFBlake3Keyed(HF_XET_INTERNAL_NODE_KEY, data, len);
}

void HFXetFormatNodeLines(string &out, const vector<HFXetHashNode> &children) {
	for (auto &child : children) {
		out += child.hash.Hex();
		out += " : ";
		out += to_string(child.size);
		out += "\n";
	}
}

HFXetHash HFXetAggregatedNodeHash(const vector<HFXetHashNode> &nodes) {
	// aggregated_node_hash: collapse the list one level at a time, left to right, in groups cut out
	// of the hashes themselves, until one hash is left. An empty list is the zero hash.
	if (nodes.empty()) {
		return HFXetHash();
	}
	auto level = nodes;
	while (level.size() > 1) {
		idx_t write = 0;
		idx_t read = 0;
		while (read < level.size()) {
			auto cut = HFNextMergeCut(level, read);
			// safe in place: write never runs ahead of read
			level[write++] = HFMergedHashOfSequence(level, read, cut);
			read = cut;
		}
		level.resize(write);
	}
	return level[0].hash;
}

HFXetHash HFXetFileHashWithSalt(const vector<HFXetHashNode> &chunks, const uint8_t salt[32]) {
	// file_hash_with_salt: an empty file has no hash rather than a hash of nothing, so the empty
	// list short-circuits to zeros *before* the salt is applied
	if (chunks.empty()) {
		return HFXetHash();
	}
	auto root = HFXetAggregatedNodeHash(chunks);
	// DataHash::hmac(key) == keyed_hash(key, hash bytes)
	return HFBlake3Keyed(salt, const_data_ptr_cast(root.bytes), 32);
}

HFXetHash HFXetFileHash(const vector<HFXetHashNode> &chunks) {
	return HFXetFileHashWithSalt(chunks, HF_XET_ZERO_SALT);
}

//===----------------------------------------------------------------------===//
// self-test
//===----------------------------------------------------------------------===//

string HFXetTestInputBytes(HFXetTestInput kind, uint64_t size, uint64_t seed, const char *pattern) {
	switch (kind) {
	case HFXetTestInput::LITERAL:
		return string(pattern ? pattern : "");
	case HFXetTestInput::REPEAT: {
		string repeated = (pattern && *pattern) ? pattern : string(1, '\0');
		string out;
		while (out.size() < size) {
			out += repeated;
		}
		out.resize(size);
		return out;
	}
	case HFXetTestInput::SPLITMIX: {
		// xet_data's create_random_data: splitmix64, little-endian words, truncated to size
		string out;
		uint64_t state = seed;
		while (out.size() < size) {
			state += 0x9E3779B97F4A7C15ULL;
			uint64_t z = state;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
			z ^= z >> 31;
			for (idx_t i = 0; i < 8; i++) {
				out += char((z >> (8 * i)) & 0xFF);
			}
		}
		out.resize(size);
		return out;
	}
	default:
		return string();
	}
}

string HFXetVectorInput(const HFXetHashVector &test) {
	return HFXetTestInputBytes(test.kind, test.size, test.seed, test.pattern);
}

idx_t HFXetHashSelfTest(vector<std::tuple<string, bool, string, string>> &results) {
	auto check = [&](const string &name, const string &expected, const string &got) {
		results.emplace_back(name, expected == got, expected, got);
	};

	// renderings first: the reference's hex() reverses bytes inside every 8-byte word, and base64
	// uses the URL-safe unpadded alphabet. Both are silent failures anywhere else.
	for (auto &test : HFXetRenderingVectors()) {
		auto hash = HashFromRawHex(test.raw_hex);
		check(string(test.name) + " hex", test.hex, hash.Hex());
		check(string(test.name) + " base64", test.base64, hash.Base64());
		check(string(test.name) + " plain hex", test.raw_hex, HashToRawHex(hash));
		// parsing has to be the inverse of printing, in both forms
		check(string(test.name) + " hex round trip", test.hex, HFXetHash::FromHex(test.hex).Hex());
		check(string(test.name) + " base64 is unpadded", "false",
		      HFXetHash::FromHex(test.hex).Base64().find('=') != string::npos ? "true" : "false");
	}

	// Which chunks the CAS's cross-repository dedup index answers for. The rule is a modulus over one
	// 64-bit word of the hash, so every wrong version of it - wrong word, wrong byte order, wrong
	// modulus - still produces *a* 1-in-1024 sample, of chunks the CAS has never heard of. Nothing on
	// the upload path can detect that; only the reference's own answers can, which is why these vectors
	// exist and why they are checked before anything that uses them.
	idx_t dedup_positives = 0;
	for (auto &test : HFXetDedupVectors()) {
		check(string("dedup eligible ") + test.hash, test.eligible ? "true" : "false",
		      HFXetGlobalDedupEligible(HFXetHash::FromHex(test.hash)) ? "true" : "false");
		dedup_positives += test.eligible ? 1 : 0;
	}
	// a vectors file that had silently lost its positives would prove nothing, and 100 000 candidates
	// at 1/1024 is far too many positives to survive a rule change unnoticed
	check("dedup vectors hold both classes", "true",
	      (dedup_positives >= 16 && dedup_positives * 3 < idx_t(HFXetDedupVectors().size())) ? "true"
	                                                                                         : "false");

	for (auto &test : HFXetInternalHashVectors()) {
		check(string(test.name) + " internal hash", test.hash,
		      HFXetInternalHash(const_data_ptr_cast(test.input), strlen(test.input)).Hex());
	}

	for (auto &test : HFXetHashVectors()) {
		auto name = string(test.name);
		auto input = HFXetVectorInput(test);
		auto hash = HFXetChunkHash(const_data_ptr_cast(input.data()), input.size());
		check(name + " chunk hash", test.chunk_hash, hash.Hex());
		check(name + " chunk base64", test.chunk_base64, hash.Base64());
		check(name + " chunk plain hex", test.chunk_plain_hex, HashToRawHex(hash));

		// the file/xorb hashes are only meaningful if the chunks they cover match the reference's,
		// so this pair is simultaneously a test of the chunker
		vector<HFXetHashNode> nodes;
		HFXetChunker chunker;
		auto on_chunk = [&](const_data_ptr_t data, idx_t len) {
			nodes.push_back({HFXetChunkHash(data, len), uint64_t(len)});
		};
		chunker.Process(const_data_ptr_cast(input.data()), input.size(), on_chunk);
		chunker.Finish(on_chunk);
		uint64_t covered = 0;
		for (auto &node : nodes) {
			covered += node.size;
		}
		check(name + " chunks cover the input", to_string(input.size()), to_string(covered));
		string got_sizes;
		for (idx_t i = 0; i < nodes.size(); i++) {
			if (i > 0) {
				got_sizes += ",";
			}
			got_sizes += to_string(nodes[i].size);
		}
		check(name + " chunk boundaries", test.chunk_sizes, got_sizes);

		// the write path feeds the chunker from a file cursor in odd-sized pieces, so chunking has to
		// depend on content alone - same bytes, any buffering, same chunks
		vector<HFXetHashNode> streamed;
		HFXetChunker piecewise;
		auto on_streamed_chunk = [&](const_data_ptr_t data, idx_t len) {
			streamed.push_back({HFXetChunkHash(data, len), uint64_t(len)});
		};
		idx_t pos = 0;
		idx_t piece = 1;
		while (pos < input.size()) {
			// deliberately awkward sizes, crossing the 8 KiB minimum and 128 KiB maximum both ways
			auto take = MinValue<idx_t>(piece, input.size() - pos);
			piecewise.Process(const_data_ptr_cast(input.data() + pos), take, on_streamed_chunk);
			pos += take;
			piece = piece * 3 + 7;
			if (piece > 400000) {
				piece = 1;
			}
		}
		piecewise.Finish(on_streamed_chunk);
		string whole_serialized;
		string streamed_serialized;
		for (auto &node : nodes) {
			whole_serialized += node.hash.Hex() + ":" + to_string(node.size) + ";";
		}
		for (auto &node : streamed) {
			streamed_serialized += node.hash.Hex() + ":" + to_string(node.size) + ";";
		}
		check(name + " chunking is independent of read sizes", whole_serialized, streamed_serialized);
		check(name + " file hash", test.file_hash, HFXetFileHash(nodes).Hex());
		check(name + " xorb hash", test.xorb_hash, HFXetXorbHash(nodes).Hex());
		check(name + " saltless file hash agrees", HFXetFileHash(nodes).Hex(),
		      HFXetFileHashWithSalt(nodes, HF_XET_ZERO_SALT).Hex());
	}

	for (auto &test : HFXetTreeVectors()) {
		vector<HFXetHashNode> nodes;
		for (idx_t i = 0; i < test.count; i++) {
			nodes.push_back({HFXetHash::FromHex(test.hashes[i]), test.sizes[i]});
		}
		auto name = string(test.name);
		check(name + " xorb hash", test.xorb_hash, HFXetXorbHash(nodes).Hex());
		check(name + " file hash", test.file_hash,
		      HFXetFileHashWithSalt(nodes, HFXetHash::FromHex(test.salt).bytes).Hex());
	}

	idx_t failures = 0;
	for (auto &result : results) {
		if (!std::get<1>(result)) {
			failures++;
		}
	}
	return failures;
}

} // namespace duckdb
