// Measures the CPU phase of a Xet write - chunking, keyed BLAKE3 per chunk, xorb serialization - with no
// network and no DuckDB in the way, so that "is the write path hash-bound?" is answered by a number
// instead of by an intuition. It is not: on 100 MB of incompressible data the whole phase is ~150 ms,
// while the same upload takes 12-70 s of wall clock depending on the link (see xet_bench.sh's header).
//
// Linked against the objects a `make release` already produced, so it measures the same code a client
// runs - including whichever BLAKE3 path the build selected for this ISA.
#include "hf_xet_chunk.hpp"
#include "hf_xet_xorb.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace duckdb;
using clk = std::chrono::steady_clock;

static double Ms(clk::time_point from, clk::time_point to) {
	return std::chrono::duration<double, std::milli>(to - from).count();
}

static double MBps(size_t bytes, double ms) {
	return double(bytes) / 1e6 / (ms / 1000.0);
}

int main(int argc, char **argv) {
	const char *input = argc > 1 ? argv[1] : "/tmp/xet_bench_100mb.bin";
	FILE *f = fopen(input, "rb");
	if (!f) {
		printf("cannot open '%s' - pass a file: scripts/xet_bench.sh /tmp/100mb.bin\n", input);
		return 1;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		printf("cannot seek '%s'\n", input);
		fclose(f);
		return 1;
	}
	size_t size = size_t(ftell(f));
	rewind(f);
	std::vector<uint8_t> buffer;
	buffer.resize(size);
	if (fread(buffer.data(), 1, size, f) != size) {
		printf("short read on '%s'\n", input);
		fclose(f);
		return 1;
	}
	fclose(f);
	const_data_ptr_t data = const_data_ptr_cast(buffer.data());
	printf("input: %zu bytes\n", size);

	// gear hashing alone: the chunk boundaries, which are the only thing the format's own chunking
	// definition cares about
	auto begin = clk::now();
	idx_t chunks = 0;
	{
		HFXetChunker chunker;
		auto count = [&](const_data_ptr_t, idx_t) { chunks++; };
		chunker.Process(data, size, count);
		chunker.Finish(count);
	}
	auto chunked = clk::now();
	printf("  chunking:                 %7.1f ms (%5.0f MB/s), %llu chunks\n", Ms(begin, chunked),
	       MBps(size, Ms(begin, chunked)), (unsigned long long)chunks);

	// the whole CPU phase: chunk, keyed-hash every chunk, serialize xorbs (merkle roots included)
	chunks = 0;
	idx_t xorbs = 0;
	idx_t serialized = 0;
	HFXetXorbBuilder builder;
	auto flush = [&]() {
		if (builder.ChunkCount() == 0) {
			return;
		}
		auto xorb = builder.Finish();
		xorbs++;
		serialized += xorb.bytes.size();
		builder = HFXetXorbBuilder();
	};
	auto add = [&](const_data_ptr_t chunk_data, idx_t len) {
		if (builder.WouldOverflow(len)) {
			flush();
		}
		builder.AddChunk(chunk_data, len);
		chunks++;
	};
	begin = clk::now();
	{
		HFXetChunker chunker;
		chunker.Process(data, size, add);
		chunker.Finish(add);
		flush();
	}
	auto done = clk::now();
	printf("  chunk + hash + xorb:      %7.1f ms (%5.0f MB/s), %llu xorbs, %llu bytes serialized\n",
	       Ms(begin, done), MBps(size, Ms(begin, done)), (unsigned long long)xorbs,
	       (unsigned long long)serialized);
	printf("\ncompare with a real upload of this same file (SELECT count(*) FROM hf_upload(...)): the CPU\n"
	       "phase above is the part of it that is not the wire.\n");
	return 0;
}
