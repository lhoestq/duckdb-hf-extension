//===----------------------------------------------------------------------===//
// Xet content-defined chunking.
//
// Port of `xet_data::deduplication::chunking::Chunker`. Chunk boundaries have to match what
// `hf_xet` produces byte for byte: boundaries *are* the chunk identity, so a file chunked
// differently from the same file chunked by the reference client dedups as nothing at all. The
// parameters and the rolling hash are the reference's own - a gear hash over a fixed 256-entry
// table - and scripts/xet_oracle.py checks boundaries against xet-core's example chunker.
//===----------------------------------------------------------------------===//

#pragma once

#include "hf_common.hpp"
#include "hf_xet_hash.hpp"

namespace duckdb {

//! xet_data::deduplication::chunker_constants, with the CLI's DEFAULT_TARGET_CHUNK_SIZE
constexpr idx_t HF_XET_TARGET_CHUNK_SIZE = 64 * 1024;
constexpr idx_t HF_XET_MIN_CHUNK_SIZE = 8 * 1024;
constexpr idx_t HF_XET_MAX_CHUNK_SIZE = 128 * 1024;

//! Called with each completed chunk's bytes, in file order.
using HFXetChunkCallback = std::function<void(const_data_ptr_t data, idx_t len)>;

//! Streaming chunker: feed bytes as they arrive, get chunks out as they complete.
class HFXetChunker {
public:
	explicit HFXetChunker(idx_t target_chunk_size = HF_XET_TARGET_CHUNK_SIZE);

	void Process(const_data_ptr_t data, idx_t len, const HFXetChunkCallback &on_chunk);
	//! No more data: emits the trailing chunk, if any, and resets for the next file.
	void Finish(const HFXetChunkCallback &on_chunk);

	idx_t MinimumChunkSize() const {
		return minimum_chunk;
	}
	idx_t MaximumChunkSize() const {
		return maximum_chunk;
	}

private:
	//! Bytes at the head of `data` that complete a chunk, or 0 when no boundary was reached.
	idx_t NextBoundary(const_data_ptr_t data, idx_t len);

	idx_t minimum_chunk;
	idx_t maximum_chunk;
	//! xet-core: ((target_chunk_size as u64) - 1) << mask.leading_zeros()
	uint64_t mask;
	//! rolling hash state, reset at every chunk boundary
	uint64_t hash = 0;
	std::vector<uint8_t> buffer;
};

//! Chunk and hash a whole buffer the way an upload does: a hash and a size per chunk, in file order,
//! including the last chunk - which only arrives at Finish, and a test that forgets it disagrees with
//! every real upload about the tail. Shared by the self-tests that need "the chunks an upload would
//! make": the shard test to build shards, the dedup test to build the hashes it queries with.
vector<HFXetHashNode> HFXetSelfTestChunks(const string &input);

} // namespace duckdb
