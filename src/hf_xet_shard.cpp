#include "hf_xet_shard.hpp"

#include "duckdb/common/string_util.hpp"

#include <algorithm>

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
			AppendU32(out, 0); // xorb_flags
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

	for (auto &test : HFXetShardVectors()) {
		auto name = string(test.name);
		auto input = HFXetTestInputBytes(test.kind, test.size, test.seed, test.pattern);

		// Build the file the way an upload does: chunk it, group the chunks into xorbs within the
		// format's limits, and describe the file as one segment per xorb it landed in.
		struct ChunkRecord {
			HFXetHash hash;
			idx_t size;
		};
		vector<ChunkRecord> chunks;
		HFXetChunker chunker;
		chunker.Process(const_data_ptr_cast(input.data()), input.size(), [&](const_data_ptr_t data, idx_t len) {
			chunks.push_back({HFXetChunkHash(data, len), len});
		});
		chunker.Finish([&](const_data_ptr_t data, idx_t len) { chunks.push_back({HFXetChunkHash(data, len), len}); });

		auto max_xorb_bytes = test.max_xorb_bytes == 0 ? HF_XET_MAX_XORB_BYTES : test.max_xorb_bytes;

		HFXetShardBuilder shard;
		vector<HFXetHash> chunk_hashes;
		for (auto &chunk : chunks) {
			chunk_hashes.push_back(chunk.hash);
		}
		vector<HFXetShardSegment> segments;
		vector<HFXetHash> xorb_order;
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
				// the first chunk of the file, and only that one, is what the reference flags
				chunk.starts_a_file = (i == 0);
				xorb.chunks.push_back(chunk);
				offset += chunks[i].size;
			}
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
		auto root_nodes_for_file = [](const vector<ChunkRecord> &all) {
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
	}

	return failures;
}

} // namespace duckdb
