#include "hf_xet_shard.hpp"

#include "duckdb/common/string_util.hpp"
#include "hf_xet_chunk.hpp"

#include <algorithm>

#include <cstdint>
#include <cstring>

namespace duckdb {

namespace {

//! The key the reference uses for range hashes - `chunk_verification::VERIFICATION_KEY`. Third key in
//! the format, after the data and interior-node keys: it is what makes a verification entry useless to
//! anyone who does not know the format is keyed at all.
const uint8_t HF_XET_VERIFICATION_KEY[32] = {127, 24,  87,  214, 206, 86,  237, 102, 18,  127, 249,
                                            19,  231, 165, 195, 243, 164, 205, 38,  213, 181, 219,
                                            73,  230, 65,  36,  152, 127, 40,  251, 148, 195};

//! The shard header tag: `HFRepoMetaData` then a strictly increasing byte sequence. It exists so a
//! reader can tell a shard from anything else in the same store, which means getting it wrong is
//! rejected by the CAS with a message about magic numbers rather than silently.
const uint8_t HF_XET_SHARD_TAG[32] = {'H', 'F',  'R',  'e',  'p',  'o',  'M',  'e',
                                      't', 'a',  'D',  'a',  't',  'a',  0,    85,
                                      105, 103,  69,   106,  123,  129,  87,   131,
                                      165, 189,  217,  92,   205,  209,  74,   169};

void AppendU32(string &out, uint32_t value) {
	for (idx_t shift = 0; shift < 32; shift += 8) {
		out.push_back(static_cast<char>(uint8_t(value >> shift)));
	}
}

void AppendU64(string &out, uint64_t value) {
	for (idx_t shift = 0; shift < 64; shift += 8) {
		out.push_back(static_cast<char>(uint8_t(value >> shift)));
	}
}

void AppendHash(string &out, const HFXetHash &hash) {
	out.append(reinterpret_cast<const char *>(hash.bytes), 32);
}

void AppendZeros(string &out, idx_t count) {
	out.append(count, '\0');
}

//! The all-ones hash that ends both sections. A sequential reader stops there; it is not a file and
//! not a xorb, and the reference compares the hash alone, flags and counts included as zeros.
HFXetHash BookendHash() {
	HFXetHash hash;
	for (idx_t i = 0; i < 32; i++) {
		hash.bytes[i] = 0xFF;
	}
	return hash;
}

} // namespace

HFXetHash HFXetRangeHash(const vector<HFXetHash> &chunk_hashes, idx_t start, idx_t end) {
	HFXetKeyedHasher hasher(HF_XET_VERIFICATION_KEY);
	for (idx_t i = start; i < end; i++) {
		hasher.Update(const_data_ptr_cast(chunk_hashes[i].bytes), 32);
	}
	return hasher.Finalize();
}

void HFXetShardBuilder::AddFile(HFXetShardFile file) {
	files.push_back(std::move(file));
}

void HFXetShardBuilder::AddXorb(HFXetShardXorb xorb) {
	xorbs.push_back(std::move(xorb));
}

idx_t HFXetShardBuilder::SegmentCount() const {
	idx_t total = 0;
	for (auto &file : files) {
		total += file.segments.size();
	}
	return total;
}

int HFXetShardBuilder::CompareByTruncatedHash(const HFXetHash &a, const HFXetHash &b) {
	// xet-core orders these maps by the hash's first word and truncates lookups to it, so ordering by
	// the first word and then word by word through the rest is the same order, including the ties
	// where the truncated keys agree but the hashes do not
	for (idx_t word = 0; word < 4; word++) {
		auto left = a.Word(word);
		auto right = b.Word(word);
		if (left != right) {
			return left < right ? -1 : 1;
		}
	}
	return 0;
}

string HFXetShardBuilder::Serialize() const {
	// ordered copies: the sections are what the reference's lookup tables index into, and a CAS that
	// rebuilds those tables from the bytes expects them in the order the hashes sort to
	vector<HFXetShardFile> sorted_files = files;
	std::stable_sort(sorted_files.begin(), sorted_files.end(),
	                 [](const HFXetShardFile &a, const HFXetShardFile &b) {
		                 return CompareByTruncatedHash(a.file_hash, b.file_hash) < 0;
	                 });
	vector<HFXetShardXorb> sorted_xorbs = xorbs;
	std::stable_sort(sorted_xorbs.begin(), sorted_xorbs.end(),
	                 [](const HFXetShardXorb &a, const HFXetShardXorb &b) {
		                 return CompareByTruncatedHash(a.hash, b.hash) < 0;
	                 });

	string out;
	// An uploaded shard has no footer, and says so: the header's footer_size is rewritten to zero on
	// the way out. Leaving it at the local file's value would advertise a trailer that is not there.
	out.append(reinterpret_cast<const char *>(HF_XET_SHARD_TAG), 32);
	AppendU64(out, HF_XET_SHARD_HEADER_VERSION);
	AppendU64(out, 0);

	for (auto &file : sorted_files) {
		auto bookend = BookendHash();
		if (file.file_hash == bookend) {
			// the reader stops at the bookend, so a file with that hash could not be described here
			throw InternalException("Xet file hash collides with the shard bookend marker");
		}
		uint32_t flags = HF_XET_FILE_FLAG_WITH_VERIFICATION | (file.has_sha256 ? HF_XET_FILE_FLAG_WITH_METADATA_EXT : 0);
		AppendHash(out, file.file_hash);
		AppendU32(out, flags);
		AppendU32(out, uint32_t(file.segments.size()));
		AppendU64(out, 0);

		for (auto &segment : file.segments) {
			AppendHash(out, segment.xorb_hash);
			AppendU32(out, segment.xorb_flags);
			AppendU32(out, segment.unpacked_bytes);
			AppendU32(out, segment.chunk_index_start);
			AppendU32(out, segment.chunk_index_end);
		}
		// one verification entry per segment, in segment order, over that segment's chunk range - which
		// is a range in the *file's* list, not in the xorb the segment points at
		for (auto &segment : file.segments) {
			if (segment.file_chunk_end > file.chunk_hashes.size()) {
				throw InternalException("Xet shard segment names file chunks %llu..%llu but the file has "
				                        "%llu chunks",
				                        static_cast<unsigned long long>(segment.file_chunk_start),
				                        static_cast<unsigned long long>(segment.file_chunk_end),
				                        static_cast<unsigned long long>(file.chunk_hashes.size()));
			}
			AppendHash(out, HFXetRangeHash(file.chunk_hashes, segment.file_chunk_start, segment.file_chunk_end));
			AppendZeros(out, 16);
		}
		if (file.has_sha256) {
			out.append(reinterpret_cast<const char *>(file.sha256), 32);
			AppendZeros(out, 16);
		}
	}
	AppendHash(out, BookendHash());
	AppendU32(out, 0);
	AppendU32(out, 0);
	AppendU64(out, 0);

	for (auto &xorb : sorted_xorbs) {
		uint64_t num_bytes = 0;
		for (auto &chunk : xorb.chunks) {
			num_bytes += chunk.unpacked_bytes;
		}
		AppendHash(out, xorb.hash);
		AppendU32(out, 0); // xorb_flags
		AppendU32(out, uint32_t(xorb.chunks.size()));
		AppendU32(out, uint32_t(num_bytes));
		AppendU32(out, 0); // num_bytes_on_disk: what it costs stored, which a writer does not know

		for (auto &chunk : xorb.chunks) {
			AppendHash(out, chunk.hash);
			AppendU32(out, chunk.byte_range_start);
			AppendU32(out, chunk.unpacked_bytes);
			// Which chunks this shard marks for cross-repository dedup: the chunk that begins a file, and
			// nothing else. The reference's second cause - one chunk hash in 1024 - is deliberately *not*
			// stored here, it is recomputed by whoever reads the shard (xet-core's `is_global_dedup_eligible`
			// is `flag || hash_is_global_dedup_eligible`, and `global_dedup_eligible_chunks` is what a CAS
			// indexes from that). Setting the bit for sampled chunks is not a harmless extra either: it
			// changes the shard bytes, and the reference vector for splitmix64 n=4000000 seed=11 - which does
			// contain a sampled-eligible chunk - says those four bytes stay zero. `GlobalDedup` in the reader
			// is where both causes do have to meet.
			AppendU32(out, chunk.starts_a_file ? HF_XET_CHUNK_FLAG_GLOBAL_DEDUP : 0);
			AppendU32(out, 0); // _unused
		}
	}
	AppendHash(out, BookendHash());
	AppendU32(out, 0);
	AppendU32(out, 0);
	AppendU32(out, 0);
	AppendU32(out, 0);

	return out;
}

//===----------------------------------------------------------------------===//
// Reading a shard
//===----------------------------------------------------------------------===//

namespace {

//! Bounds-checked cursor over a shard's bytes. Everything parsed here arrived from the network, and the
//! failure a parser must not have is the confident one: an unchecked read past the end of a truncated
//! shard yields whatever the allocator happens to be holding, which parses as a plausible entry and
//! describes a file nobody uploaded.
class ShardCursor {
public:
	ShardCursor(const_data_ptr_t data_p, idx_t len_p) : data(data_p), len(len_p) {}

	idx_t Remaining() const {
		return len - pos;
	}

	void Require(idx_t need, const char *what) {
		// written as `need > remaining` rather than `pos + need > len` so a hostile count cannot overflow
		if (need > Remaining()) {
			throw IOException("Xet shard is truncated: %s needs %llu bytes at offset %llu, %llu remain",
			                  what, static_cast<unsigned long long>(need), static_cast<unsigned long long>(pos),
			                  static_cast<unsigned long long>(Remaining()));
		}
	}

	uint32_t U32() {
		Require(4, "a u32");
		uint32_t value = 0;
		for (idx_t shift = 0; shift < 32; shift += 8) {
			value |= uint32_t(data[pos + (shift / 8)]) << shift;
		}
		pos += 4;
		return value;
	}

	uint64_t U64() {
		Require(8, "a u64");
		uint64_t value = 0;
		for (idx_t shift = 0; shift < 64; shift += 8) {
			value |= uint64_t(data[pos + (shift / 8)]) << shift;
		}
		pos += 8;
		return value;
	}

	HFXetHash Hash() {
		Require(32, "a hash");
		HFXetHash hash;
		memcpy(hash.bytes, data + pos, 32);
		pos += 32;
		return hash;
	}

	void Skip(idx_t count) {
		Require(count, "bytes to skip");
		pos += count;
	}

private:
	const_data_ptr_t data;
	idx_t len;
	idx_t pos = 0;
};

} // namespace

HFXetShardView HFXetShardView::Parse(const string &bytes) {
	HFXetShardView view;
	ShardCursor cursor(const_data_ptr_cast(bytes.data()), bytes.size());
	cursor.Require(HF_XET_SHARD_HEADER_SIZE, "the header");
	for (idx_t i = 0; i < 32; i++) {
		if (uint8_t(bytes[i]) != HF_XET_SHARD_TAG[i]) {
			throw IOException("Xet dedup query response is not a metadata shard: the header tag does not match");
		}
	}
	// The tag is compared against `bytes` rather than through the cursor because it is not a number - and
	// the cursor then has to be told about it by hand. Skipping that reads the version out of the tag and
	// reports a header version of "HFRepoMe", which is exactly what this line first failed as.
	cursor.Skip(32);
	auto version = cursor.U64();
	auto footer_size = cursor.U64();
	if (version != HF_XET_SHARD_HEADER_VERSION) {
		throw IOException("Xet metadata shard has header version %llu, this build reads %llu",
		                  static_cast<unsigned long long>(version),
		                  static_cast<unsigned long long>(HF_XET_SHARD_HEADER_VERSION));
	}
	// The sections come first and the lookup tables plus footer (if any) after them, so the sections can
	// be read without knowing how the tail is arranged. The tail is still checked for length: a shard
	// that promises a footer it does not carry is a truncated one, and the sections would parse fine.
	if (footer_size > cursor.Remaining()) {
		throw IOException("Xet metadata shard advertises a %llu byte footer with only %llu bytes after its "
		                  "sections",
		                  static_cast<unsigned long long>(footer_size),
		                  static_cast<unsigned long long>(cursor.Remaining()));
	}

	while (true) {
		cursor.Require(HF_XET_SHARD_ENTRY_SIZE, "a file entry");
		auto file_hash = cursor.Hash();
		if (file_hash == BookendHash()) {
			cursor.Skip(16); // flags, segment count, and a reserved u64, all zero
			break;
		}
		HFXetShardFileView file;
		file.file_hash = file_hash;
		file.flags = cursor.U32();
		auto num_segments = cursor.U32();
		cursor.Skip(8);
		if (num_segments > cursor.Remaining() / HF_XET_SHARD_ENTRY_SIZE) {
			throw IOException("Xet metadata shard file entry claims %llu segments with %llu entries left",
			                  static_cast<unsigned long long>(num_segments),
			                  static_cast<unsigned long long>(cursor.Remaining() / HF_XET_SHARD_ENTRY_SIZE));
		}
		// the format stores xorb-local indices and says nothing about file-wide ones: they are implied by
		// the segments tiling the file in order, which is also what each verification entry covers
		uint32_t file_position = 0;
		for (uint32_t i = 0; i < num_segments; i++) {
			HFXetShardSegment segment;
			segment.xorb_hash = cursor.Hash();
			segment.xorb_flags = cursor.U32();
			segment.unpacked_bytes = cursor.U32();
			segment.chunk_index_start = cursor.U32();
			segment.chunk_index_end = cursor.U32();
			if (segment.chunk_index_end < segment.chunk_index_start) {
				throw IOException("Xet metadata shard segment %llu ends before it starts (%llu..%llu)",
				                  static_cast<unsigned long long>(i),
				                  static_cast<unsigned long long>(segment.chunk_index_start),
				                  static_cast<unsigned long long>(segment.chunk_index_end));
			}
			segment.file_chunk_start = file_position;
			file_position += segment.chunk_index_end - segment.chunk_index_start;
			segment.file_chunk_end = file_position;
			file.segments.push_back(segment);
		}
		if (file.WithVerification()) {
			for (uint32_t i = 0; i < num_segments; i++) {
				file.verification.push_back(cursor.Hash());
				cursor.Skip(16);
			}
		}
		if ((file.flags & HF_XET_FILE_FLAG_WITH_METADATA_EXT) != 0) {
			// the sha256 travels as a DataHash in these 32 bytes, exactly as it was written
			file.has_sha256 = true;
			auto sha256 = cursor.Hash();
			memcpy(file.sha256, sha256.bytes, 32);
			cursor.Skip(16);
		}
		view.files.push_back(std::move(file));
	}

	while (true) {
		cursor.Require(HF_XET_SHARD_ENTRY_SIZE, "a xorb entry");
		auto xorb_hash = cursor.Hash();
		if (xorb_hash == BookendHash()) {
			cursor.Skip(16);
			break;
		}
		HFXetShardXorbView xorb;
		xorb.hash = xorb_hash;
		xorb.flags = cursor.U32();
		auto num_chunks = cursor.U32();
		cursor.Skip(8); // num_bytes, num_bytes_on_disk
		if (num_chunks > cursor.Remaining() / HF_XET_SHARD_ENTRY_SIZE) {
			throw IOException("Xet metadata shard xorb entry claims %llu chunks with %llu entries left",
			                  static_cast<unsigned long long>(num_chunks),
			                  static_cast<unsigned long long>(cursor.Remaining() / HF_XET_SHARD_ENTRY_SIZE));
		}
		for (uint32_t i = 0; i < num_chunks; i++) {
			HFXetShardChunkView chunk;
			chunk.hash = cursor.Hash();
			chunk.byte_range_start = cursor.U32();
			chunk.unpacked_bytes = cursor.U32();
			chunk.flags = cursor.U32();
			cursor.Skip(4); // _unused
			xorb.chunks.push_back(chunk);
		}
		view.xorbs.push_back(std::move(xorb));
	}

	// The footer, when there is one. It sits at the very end of the shard (the reference seeks to
	// `End(-footer_size)` to find it, and the lookup tables live in front of it), and reading it is not
	// optional: the key inside it is what every chunk hash in the sections above is stored under. An
	// uploaded shard advertises a footer size of zero and carries none; a stored one advertises 200.
	if (footer_size == HF_XET_SHARD_FOOTER_SIZE) {
		ShardCursor footer(const_data_ptr_cast(bytes.data()) + (bytes.size() - HF_XET_SHARD_FOOTER_SIZE),
		                   HF_XET_SHARD_FOOTER_SIZE);
		auto footer_version = footer.U64();
		if (footer_version != HF_XET_SHARD_FOOTER_VERSION) {
			throw IOException("Xet metadata shard footer has version %llu, this build reads %llu",
			                  static_cast<unsigned long long>(footer_version),
			                  static_cast<unsigned long long>(HF_XET_SHARD_FOOTER_VERSION));
		}
		// The key sits 72 bytes into the footer, and the version field has already been consumed, so the
		// skip is measured from offset 8 rather than from the start - which is the kind of off-by-8 that
		// reads a key from the wrong place and then matches nothing while looking entirely healthy.
		footer.Skip(HF_XET_SHARD_FOOTER_KEY_OFFSET - 8);
		view.chunk_hash_hmac_key = footer.Hash();
		// 88 bytes between the key and the last field: two timestamps, six reserved slots, three byte
		// counts. The last field says where the footer started, the one claim here that can be checked
		// against the buffer rather than against the format - a shard lying about its own length is not
		// one whose key should be trusted.
		footer.Skip(HF_XET_SHARD_FOOTER_SIZE - HF_XET_SHARD_FOOTER_KEY_OFFSET - 32 - 8);
		auto claimed_start = footer.U64();
		if (claimed_start + HF_XET_SHARD_FOOTER_SIZE != bytes.size()) {
			throw IOException("Xet metadata shard footer claims to start at %llu in a %llu byte shard",
			                  static_cast<unsigned long long>(claimed_start),
			                  static_cast<unsigned long long>(bytes.size()));
		}
	}
	return view;
}

const HFXetShardXorbView *HFXetShardView::FindXorb(const HFXetHash &hash) const {
	for (auto &xorb : xorbs) {
		if (xorb.hash == hash) {
			return &xorb;
		}
	}
	return nullptr;
}

//===----------------------------------------------------------------------===//
// Self-test
//===----------------------------------------------------------------------===//

namespace {

string FormatHashList(const vector<HFXetHash> &hashes) {
	string out;
	for (idx_t i = 0; i < hashes.size(); i++) {
		if (i > 0) {
			out += ",";
		}
		out += hashes[i].Hex();
	}
	return out;
}

} // namespace



idx_t HFXetShardSelfTest(vector<std::tuple<string, bool, string, string>> &results) {
	idx_t failures = 0;
	auto check = [&](const string &name, const string &expected, const string &got) {
		results.emplace_back(name, expected == got, expected, got);
		if (expected != got) {
			failures++;
		}
	};
	// one whole reference shard, kept for the framing tests after the loop
	string sample;

	for (auto &test : HFXetShardVectors()) {
		auto name = string(test.name);
		auto input = HFXetTestInputBytes(test.kind, test.size, test.seed, test.pattern);
		// A vector that cannot be built or read back is a failed check, not a failed selftest: an exception
		// leaving this function reports nothing at all, which is the worst possible diagnosis for "one of the
		// shard vectors does not parse". So the failure becomes a row, like any other wrong answer.
		try {

			// Build the file the way an upload does: chunk it, group the chunks into xorbs within the
			// format's limits, and describe the file as one segment per xorb it landed in.
			auto chunks = HFXetSelfTestChunks(input);

			auto max_xorb_bytes = test.max_xorb_bytes == 0 ? HF_XET_MAX_XORB_BYTES : test.max_xorb_bytes;

			HFXetShardBuilder shard;
			vector<HFXetHash> chunk_hashes;
			for (auto &chunk : chunks) {
				chunk_hashes.push_back(chunk.hash);
			}
			vector<HFXetShardSegment> segments;
			vector<HFXetHash> xorb_order;
			vector<HFXetShardXorb> built_xorbs;
			idx_t position = 0;
			while (position < chunks.size()) {
				idx_t start = position;
				uint64_t bytes = 0;
				while (position < chunks.size() && (position - start) < HF_XET_MAX_XORB_CHUNKS &&
				       bytes + chunks[position].size <= max_xorb_bytes) {
					bytes += chunks[position].size;
					position++;
				}
				if (position == start) {
					// a chunk bigger than the xorb limit still needs a xorb of its own; a grouping rule
					// without this case just spins
					bytes += chunks[position].size;
					position++;
				}
				vector<HFXetHashNode> nodes;
				for (idx_t i = start; i < position; i++) {
					nodes.push_back({chunks[i].hash, uint64_t(chunks[i].size)});
				}
				auto xorb_hash = HFXetXorbHash(nodes);

				HFXetShardXorb xorb;
				xorb.hash = xorb_hash;
				uint64_t offset = 0;
				for (idx_t i = start; i < position; i++) {
					HFXetShardXorbChunk chunk;
					chunk.hash = chunks[i].hash;
					chunk.byte_range_start = uint32_t(offset);
					chunk.unpacked_bytes = uint32_t(chunks[i].size);
					// the file's first chunk, plus anything the dedup sampling rule picks up - both of which
					// put the chunk in the CAS's cross-repository index, which is what the read-back below checks
					chunk.starts_a_file = (i == 0);
					xorb.chunks.push_back(chunk);
					offset += chunks[i].size;
				}
				built_xorbs.push_back(xorb);
				shard.AddXorb(std::move(xorb));

				HFXetShardSegment segment;
				segment.xorb_hash = xorb_hash;
				segment.unpacked_bytes = uint32_t(bytes);
				segment.chunk_index_start = 0;
				segment.chunk_index_end = uint32_t(position - start);
				segment.file_chunk_start = uint32_t(start);
				segment.file_chunk_end = uint32_t(position);
				segments.push_back(segment);
				xorb_order.push_back(xorb_hash);
			}

			HFXetShardFile file;
			auto root_nodes_for_file = [](const vector<HFXetHashNode> &all) {
				vector<HFXetHashNode> nodes;
				for (auto &chunk : all) {
					nodes.push_back({chunk.hash, uint64_t(chunk.size)});
				}
				return nodes;
			};
			file.file_hash = HFXetFileHash(root_nodes_for_file(chunks));
			file.segments = segments;
			file.chunk_hashes = chunk_hashes;
			if (test.sha256) {
				// The sha256 travels as a DataHash, and DataHash::from_hex reads the same four little-endian
				// words as MerkleHash::from_hex does - each group of 16 hex digits becomes one word, `.to_le()`.
				// Parsing the hex as 32 plain bytes instead still fills 32 bytes, still writes 32 bytes, and
				// disagrees with the reference on the order of every eighth of them.
				file.has_sha256 = true;
				auto parsed = HFXetHash::FromHex(string(test.sha256));
				for (idx_t i = 0; i < 32; i++) {
					file.sha256[i] = parsed.bytes[i];
				}
			}
			shard.AddFile(std::move(file));

			// the reference orders its sections by hash, so the expected lists are in that order too
			std::stable_sort(xorb_order.begin(), xorb_order.end(), [](const HFXetHash &a, const HFXetHash &b) {
				return HFXetShardBuilder::CompareByTruncatedHash(a, b) < 0;
			});
			vector<HFXetHash> verification;
			for (auto &segment : segments) {
				// over the file's chunks for that segment, not the xorb's (which is what the entry stores)
				verification.push_back(HFXetRangeHash(chunk_hashes, segment.file_chunk_start, segment.file_chunk_end));
			}

			auto bytes = shard.Serialize();
			check(name + " shard length", to_string(test.num_bytes), to_string(bytes.size()));
			check(name + " shard bytes", test.fingerprint,
			      HFXetChunkHash(const_data_ptr_cast(bytes.data()), bytes.size()).Hex());
			check(name + " shard file hash", test.file_hash, file.file_hash.Hex());
			// the flag is what tells a reader to expect 48 more bytes per file entry; a shard that writes the
			// extension without saying so (or says so without writing it) is unreadable, and the byte
			// comparison alone would not name which of the two went wrong
			uint32_t expected_flags = HF_XET_FILE_FLAG_WITH_VERIFICATION |
			                          (test.sha256 ? HF_XET_FILE_FLAG_WITH_METADATA_EXT : 0);
			uint32_t stored_flags = 0;
			for (idx_t i = 0; i < 4; i++) {
				stored_flags |= uint32_t(uint8_t(bytes[HF_XET_SHARD_HEADER_SIZE + 32 + i])) << (i * 8);
			}
			check(name + " shard file flags", to_string(expected_flags), to_string(stored_flags));
			check(name + " shard xorb hashes", test.xorb_hashes, FormatHashList(xorb_order));
			check(name + " shard verification", test.verification_hashes, FormatHashList(verification));

			// as stored: xorb local ranges, so a segment covering its whole xorb reads 0..len
			string ranges;
			for (idx_t i = 0; i < segments.size(); i++) {
				if (i > 0) {
					ranges += ",";
				}
				ranges += to_string(segments[i].chunk_index_start) + "-" + to_string(segments[i].chunk_index_end);
			}
			check(name + " shard segments", test.segment_ranges, ranges);

			// Read back what matters about the framing itself: the header tag, the version, and the
			// footer_size we promise, which for an uploaded shard is a promise that there is no footer.
			auto read_u64_at = [&](idx_t position) {
				uint64_t value = 0;
				for (idx_t shift = 0; shift < 64; shift += 8) {
					value |= uint64_t(uint8_t(bytes[position + (shift / 8)])) << shift;
				}
				return value;
			};
			check(name + " shard tag", "HFRepoMetaData", bytes.substr(0, 14));
			check(name + " shard version", "2", to_string(read_u64_at(32)));
			// not the local file's footer size: an uploaded shard is the local one with the lookup tables
			// and footer cut off, and this field is how a reader knows not to look for them
			check(name + " shard footer size", "0", to_string(read_u64_at(40)));
			// after the 48-byte header, the rest is nothing but 48-byte entries; a length that is not a
			// multiple of the entry size means something was written at the wrong width
			check(name + " shard entries are 48 bytes", "0", to_string((bytes.size() - 48) % HF_XET_SHARD_ENTRY_SIZE));

			//=== Read back ================================================================
			// Reading is the other half of the format, and the half a dedup pointer depends on: what a CAS
			// dedup query hands back *is* these bytes, so a reader that disagrees with the writer about any
			// field's width or order does not fail - it resolves a pointer into the wrong chunk and reads back
			// a file nobody uploaded. The byte comparison above cannot catch that, because it never parses.
			auto view = HFXetShardView::Parse(bytes);
			sample = bytes;
			check(name + " read file count", "1", to_string(view.files.size()));
			check(name + " read xorb count", to_string(built_xorbs.size()), to_string(view.xorbs.size()));
			if (view.files.size() == 1) {
				auto &file_view = view.files[0];
				check(name + " read file hash", file.file_hash.Hex(), file_view.file_hash.Hex());
				check(name + " read file flags", to_string(expected_flags), to_string(file_view.flags));
				check(name + " read segment count", to_string(segments.size()), to_string(file_view.segments.size()));

				auto segment_text = [](const vector<HFXetShardSegment> &list) {
					string out;
					for (auto &segment : list) {
						if (!out.empty()) {
							out += ",";
						}
						// all four coordinates, plus the xorb's flags: a dedup pointer has to carry them, and a
						// reader that drops them on the floor produces a segment the CAS will not verify
						out += segment.xorb_hash.Hex() + ":" + to_string(segment.xorb_flags) + ":" +
						       to_string(segment.chunk_index_start) + "-" + to_string(segment.chunk_index_end) + ":" +
						       to_string(segment.unpacked_bytes) + ":" + to_string(segment.file_chunk_start) + "-" +
						       to_string(segment.file_chunk_end);
					}
					return out;
				};
				check(name + " read segments", segment_text(segments), segment_text(file_view.segments));
				check(name + " read verification", FormatHashList(verification), FormatHashList(file_view.verification));
				if (file.has_sha256) {
					check(name + " read sha256 flag", "true", file_view.has_sha256 ? "true" : "false");
					string expected_sha256, got_sha256;
					for (idx_t i = 0; i < 32; i++) {
						expected_sha256 += to_string(file.sha256[i]);
						got_sha256 += to_string(file_view.sha256[i]);
					}
					check(name + " read sha256 bytes", expected_sha256, got_sha256);
				} else {
					check(name + " read sha256 flag", "false", file_view.has_sha256 ? "true" : "false");
				}
			}

			// The xorb section is the table a dedup query is answered from, so it is read field by field -
			// including the dedup flag, whose two causes (file start, hash sampling) are recomputed here
			// independently. A shard whose flags column disagrees with the sampling rule is a shard whose
			// chunks the CAS will never offer to anybody, and nothing else in the format would notice.
			std::stable_sort(built_xorbs.begin(), built_xorbs.end(), [](const HFXetShardXorb &a, const HFXetShardXorb &b) {
				return HFXetShardBuilder::CompareByTruncatedHash(a.hash, b.hash) < 0;
			});
			auto xorb_text = [](const vector<HFXetHash> &order) {
				string out;
				for (auto &hash : order) {
					if (!out.empty()) {
						out += ",";
					}
					out += hash.Hex();
				}
				return out;
			};
			check(name + " read xorb order", xorb_text(xorb_order), [&]() {
				vector<HFXetHash> read_order;
				for (auto &xorb : view.xorbs) {
					read_order.push_back(xorb.hash);
				}
				return xorb_text(read_order);
			}());
			// How many chunks are dedup-eligible *only* by sampling, so must be stored with the bit clear and
			// read back as eligible. The asymmetry is what the format turns on, so it gets rows of its own
			// instead of hiding inside the byte fingerprint: writing the bit for these chunks (which reads
			// like "more dedup, why not") breaks the fingerprint, and a reader that stops recomputing the rule
			// silently stops finding 1/1024 of what the CAS indexes.
			idx_t sampled_only = 0;
			for (auto &built_xorb : built_xorbs) {
				for (auto &chunk : built_xorb.chunks) {
					if (HFXetGlobalDedupEligible(chunk.hash) && !chunk.starts_a_file) {
						sampled_only++;
					}
				}
			}
			idx_t read_sampled_only = 0;
			auto chunk_text = [](const HFXetHash &hash, const vector<HFXetShardXorbChunk> &chunks) {
				string out;
				for (auto &chunk : chunks) {
					out += "|" + chunk.hash.Hex() + ":" + to_string(chunk.byte_range_start) + ":" +
					       to_string(chunk.unpacked_bytes) + ":" +
					       ((chunk.starts_a_file || HFXetGlobalDedupEligible(chunk.hash)) ? "1" : "0");
				}
				return hash.Hex() + out;
			};
			for (idx_t i = 0; i < built_xorbs.size() && i < view.xorbs.size(); i++) {
				string got;
				for (auto &chunk : view.xorbs[i].chunks) {
					if (!(chunk.flags & HF_XET_CHUNK_FLAG_GLOBAL_DEDUP) && chunk.GlobalDedup()) {
						read_sampled_only++;
					}
					got += "|" + chunk.hash.Hex() + ":" + to_string(chunk.byte_range_start) + ":" +
					     to_string(chunk.unpacked_bytes) + ":" + (chunk.GlobalDedup() ? "1" : "0");
				}
				got = view.xorbs[i].hash.Hex() + got;
				check(name + " read xorb " + to_string(i) + " chunks", chunk_text(built_xorbs[i].hash, built_xorbs[i].chunks),
				      got);
				check(name + " read xorb " + to_string(i) + " chunk count", to_string(built_xorbs[i].chunks.size()),
				      to_string(view.xorbs[i].chunks.size()));
			}
			check(name + " sampled-eligible chunks", to_string(sampled_only), to_string(read_sampled_only));
		} catch (std::exception &ex) {
			check(name + " build and read", "no exception", ex.what());
		}
	}

	// A shard in the shape a dedup query answers with. These bytes are xet-core's own keyed export, and
	// nothing on this port's upload path can produce them: a session writes an unkeyed shard, uploads it,
	// and reads back only what it wrote. What a query returns is somebody else's shard - footered, with no
	// file sections to speak of, and with every chunk hash keyed under a key the footer carries. The key is
	// not optional reading: a matcher that compares plain hashes against these bytes finds nothing, and
	// reports that as "the CAS had nothing to give" while the CAS answered every query. That is a real
	// regression this port shipped, offline-green and live-zero, before this fixture existed.
	for (auto &test : HFXetStoredShardVectors()) {
		auto name = string("stored shard ") + test.name;
		try {
			auto bytes = HFXetSelfTestBytesFromHex(test.shard_hex);
			if (bytes.size() != test.stored_bytes) {
				check(name + " fixture length", to_string(test.stored_bytes), to_string(bytes.size()));
				continue;
			}
			auto view = HFXetShardView::Parse(bytes);
			// the key is the footer's own 32 bytes, read as bytes: taking them through a hex string would
			// reverse them inside every word and still look like a key
			HFXetHash claimed;
			memcpy(claimed.bytes,
			       const_data_ptr_cast(bytes.data()) + bytes.size() - (HF_XET_SHARD_FOOTER_SIZE - HF_XET_SHARD_FOOTER_KEY_OFFSET),
			       32);
			check(name + " reads the footer's key", "true", view.chunk_hash_hmac_key == claimed ? "true" : "false");
			check(name + " key is not the zero sentinel", "false", view.chunk_hash_hmac_key.IsZero() ? "true" : "false");
			check(name + " hex matches the key it was exported with", test.key, view.chunk_hash_hmac_key.Hex());
			// the file section of a CAS answer is a bookend and nothing else, measured live twice over: the
			// xorbs are the whole of the answer, so a reader that needed a file entry would register nothing
			check(name + " holds no file entries", "0", to_string(view.files.size()));
			auto chunks = HFXetSelfTestChunks(HFXetTestInputBytes(test.kind, test.size, test.seed, test.pattern));
			check(name + " holds one xorb", "1", to_string(view.xorbs.size()));
			check(name + " xorb holds every chunk", to_string(chunks.size()), to_string(view.xorbs[0].chunks.size()));
			// the one fact the matcher lives or dies on: these stored hashes are keyed, and the file's own
			// hashes are not. Both halves - "they differ" and "they differ in exactly this way" - matter,
			// since a broken keying would also make them differ.
			if (!view.xorbs.empty() && !view.xorbs[0].chunks.empty()) {
				check(name + " stored hash is not the plain chunk hash", "false",
				      view.xorbs[0].chunks[0].hash == chunks[0].hash ? "true" : "false");
				check(name + " stored hash is keyed(key, chunk hash)",
				      HFXetKeyedChunkHash(chunks[0].hash, view.chunk_hash_hmac_key).Hex(),
				      view.xorbs[0].chunks[0].hash.Hex());
			}
		} catch (std::exception &ex) {
			// a stored shard this parser cannot read is a failed row, not a thrown selftest: see the note in
			// the vector loop above about what an exception reports
			check(name + " parses", "no exception", ex.what());
		}
	}

	// And the rejections. A parser that has only ever met well-formed bytes has not been tested, and the
	// bytes a dedup query returns come off a network: a truncated answer that parses anyway becomes a
	// shard with a plausible xorb table and chunks in it that are not in it.
	{
		auto outcome = [](const string &shard_bytes) {
			try {
				HFXetShardView::Parse(shard_bytes);
				return "accepted";
			} catch (IOException &) {
				return "rejected";
			}
		};
		if (!sample.empty()) {
			check("shard read back", "accepted", outcome(sample));
			// cut mid-entry: the last bookend is gone, and a cursor that did not bounds-check would read
			// past the end and report a shard that merely has fewer xorbs in it
			check("shard truncated mid entry", "rejected", outcome(sample.substr(0, sample.size() - 24)));
			auto wrong_tag = sample;
			wrong_tag[5] = char(uint8_t(wrong_tag[5]) ^ 0xFF);
			check("shard with a foreign tag", "rejected", outcome(wrong_tag));
			auto wrong_version = sample;
			wrong_version[32] = char(99);
			check("shard from the future", "rejected", outcome(wrong_version));
			auto huge_footer = sample;
			for (idx_t i = 40; i < 48; i++) {
				huge_footer[i] = char(0xFF);
			}
			check("shard advertising a footer it lacks", "rejected", outcome(huge_footer));
			auto inflated = sample;
			// the first file entry's segment count, at header + hash + flags
			for (idx_t i = 0; i < 4; i++) {
				inflated[HF_XET_SHARD_HEADER_SIZE + 36 + i] = char(0xFF);
			}
			check("shard with an impossible segment count", "rejected", outcome(inflated));

			// The synthetic version of the same shape: a footer of a size the reader does not know, which
			// has to be *tolerated* (an uploaded shard advertises zero and carries none, and the real stored
			// shape above is what a 200 byte footer looks like). The advertised length is believed just
			// enough to notice when the bytes do not back it up.
			auto stored = sample;
			uint64_t advertised = 56;
			for (idx_t shift = 0; shift < 64; shift += 8) {
				stored[40 + (shift / 8)] = char(uint8_t(advertised >> shift));
			}
			stored.append(56, '\x5a');
			check("stored shard with an unknown footer reads", "accepted", outcome(stored));
			auto with_footer = HFXetShardView::Parse(stored);
			auto without = HFXetShardView::Parse(sample);
			check("a footer of a size this reader does not know yields no key", "true",
			      with_footer.chunk_hash_hmac_key.IsZero() ? "true" : "false");
			check("the footer changes nothing", to_string(without.files.size()) + "/" + to_string(without.xorbs.size()),
			      to_string(with_footer.files.size()) + "/" + to_string(with_footer.xorbs.size()));

			// The real stored shape, tampered with where only a footer-reader would notice. Both of these
			// arrive over the network as bytes that look exactly like a shard.
			for (auto &test : HFXetStoredShardVectors()) {
				auto keyed = HFXetSelfTestBytesFromHex(test.shard_hex);
				auto wrong_footer_version = keyed;
				wrong_footer_version[keyed.size() - HF_XET_SHARD_FOOTER_SIZE] = char(9);
				check("stored shard with a footer from the future", "rejected", outcome(wrong_footer_version));
				// the last field says where the footer began; a shard that lies about its own length is not
				// one whose key should be trusted, and the key is the only thing read out of it
				auto lying_offset = keyed;
				for (idx_t i = 0; i < 8; i++) {
					lying_offset[keyed.size() - 8 + i] = char(0xFF);
				}
				check("stored shard whose footer miscounts itself", "rejected", outcome(lying_offset));
				// and the check must be about the footer rather than about the whole buffer: drop one byte
				// off the end and the advertised 200 no longer fits at all
				// and one byte off the end moves the footer to where it does not start: the parse is
				// rejected, though which of the footer's own fields notices first is not the point
				check("stored shard truncated by one byte", "rejected", outcome(keyed.substr(0, keyed.size() - 1)));
			}
		}
	}

	return failures;
}

} // namespace duckdb
