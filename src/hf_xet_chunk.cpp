#include "hf_xet_chunk.hpp"

#include "gearhash_table.hpp"

#include <cstring>

#include <cstdint>

namespace duckdb {

namespace {

//! the rolling hash from the `gearhash` crate: hash = (hash << 1) + table[byte], boundary when
//! `hash & mask == 0`
inline uint64_t HFGearStep(uint64_t hash, uint8_t byte) {
	return (hash << 1) + HF_GEAR_HASH_TABLE[byte];
}

} // namespace

HFXetChunker::HFXetChunker(idx_t target_chunk_size) {
	// chunker_constants: minimum = target / MINIMUM_CHUNK_DIVISOR, maximum = target *
	// MAXIMUM_CHUNK_MULTIPLIER - no clamping, so that a caller retuning the target gets the same
	// sizes the reference would derive
	minimum_chunk = target_chunk_size / 8;
	maximum_chunk = target_chunk_size * 2;
	// chunking::Chunker::new: mask = (target - 1) << (target - 1).leading_zeros(), i.e. the low bits
	// of the gear hash, which react to only a handful of bytes, are shifted out and the match is
	// tested against the high ones. With target = 64 KiB that is 0xFFFF000000000000 - a boundary
	// every 1/65536 steps on average. (A mask of the low bits instead cuts almost immediately, and
	// still passes on constant data, which is why the reference vectors are the only thing that
	// catches it.)
	uint64_t m = uint64_t(target_chunk_size - 1);
	idx_t leading_zeros = 0;
	while (leading_zeros < 64 && !(m & (uint64_t(1) << (63 - leading_zeros)))) {
		leading_zeros++;
	}
	mask = m << leading_zeros;
	buffer.reserve(maximum_chunk);
}

idx_t HFXetChunker::NextBoundary(const_data_ptr_t data, idx_t len) {
	// The reference's hash window is 64 bytes wide: a boundary triggered before 64 bytes have been
	// fed would depend on the state left over from the previous chunk, so the reference skips ahead
	// and then refuses any cut that has not seen 64 bytes of this chunk. Reproduced exactly.
	constexpr idx_t HASH_WINDOW_SIZE = 64;
	if (len == 0) {
		return 0;
	}
	auto previous_len = buffer.size();
	idx_t cur_index = 0;
	bool create_chunk = false;

	if (previous_len + HASH_WINDOW_SIZE < minimum_chunk) {
		auto skip = MinValue<idx_t>(minimum_chunk - previous_len - HASH_WINDOW_SIZE - 1, len);
		cur_index += skip;
	}
	auto read_end = MinValue<idx_t>(len, cur_index + (maximum_chunk - previous_len));

	// scan for a boundary, skipping triggers that land inside the minimum chunk size
	while (cur_index < read_end) {
		idx_t next_match = 0;
		bool matched = false;
		for (idx_t i = cur_index; i < read_end; i++) {
			hash = HFGearStep(hash, data[i]);
			if ((hash & mask) == 0) {
				next_match = i + 1; // bytes consumed up to and including the triggering byte
				matched = true;
				break;
			}
		}
		if (!matched) {
			cur_index = read_end; // the scan consumed everything up to read_end
			break;
		}
		cur_index = next_match;
		if (cur_index + previous_len < minimum_chunk) {
			continue; // too early to cut; keep hashing from here
		}
		create_chunk = true;
		break;
	}

	if (cur_index + previous_len >= maximum_chunk) {
		cur_index = maximum_chunk - previous_len;
		create_chunk = true;
	}

	if (!create_chunk) {
		return 0;
	}
	hash = 0; // the reference resets the rolling hash at every boundary
	return cur_index;
}

void HFXetChunker::Process(const_data_ptr_t data, idx_t len, const HFXetChunkCallback &on_chunk) {
	idx_t pos = 0;
	while (pos < len) {
		auto boundary = NextBoundary(data + pos, len - pos);
		if (boundary == 0) {
			buffer.insert(buffer.end(), data + pos, data + len);
			return;
		}
		if (buffer.empty()) {
			on_chunk(data + pos, boundary);
		} else {
			buffer.insert(buffer.end(), data + pos, data + pos + boundary);
			on_chunk(buffer.data(), buffer.size());
			buffer.clear();
		}
		pos += boundary;
	}
}

void HFXetChunker::Finish(const HFXetChunkCallback &on_chunk) {
	// At EOF everything still buffered is the last chunk. A trailing empty chunk is never emitted,
	// which is what makes `Finish` safe to call after a Process that ended exactly on a boundary.
	if (!buffer.empty()) {
		on_chunk(buffer.data(), buffer.size());
		buffer.clear();
	}
	hash = 0;
}

vector<HFXetHashNode> HFXetSelfTestChunks(const string &input) {
	vector<HFXetHashNode> chunks;
	HFXetChunker chunker;
	chunker.Process(const_data_ptr_cast(input.data()), input.size(), [&](const_data_ptr_t data, idx_t len) {
		chunks.push_back({HFXetChunkHash(data, len), uint64_t(len)});
	});
	chunker.Finish([&](const_data_ptr_t data, idx_t len) {
		chunks.push_back({HFXetChunkHash(data, len), uint64_t(len)});
	});
	return chunks;
}

} // namespace duckdb
