#include "hf_xet_xorb.hpp"

#include "hf_xet_chunk.hpp"

#include "lz4frame.h"

#include <cstring>

namespace duckdb {

namespace {

//! The section identifiers, verbatim from xorb_object_format.rs. A xorb reader finds the footer by
//! its trailing length, then expects these at the front of each section.
const uint8_t HF_XET_XORB_IDENT[7] = {'X', 'E', 'T', 'B', 'L', 'O', 'B'};
const uint8_t HF_XET_XORB_IDENT_HASHES[7] = {'X', 'B', 'L', 'B', 'H', 'S', 'H'};
const uint8_t HF_XET_XORB_IDENT_BOUNDARIES[7] = {'X', 'B', 'L', 'B', 'B', 'N', 'D'};
constexpr uint8_t HF_XET_XORB_FORMAT_VERSION = 1;
constexpr uint8_t HF_XET_XORB_HASHES_VERSION = 0;
constexpr uint8_t HF_XET_XORB_BOUNDARIES_VERSION = 1;
//! a serialized chunk header: version, 3-byte compressed length, scheme, 3-byte uncompressed length
constexpr uint8_t HF_XET_XORB_CHUNK_HEADER_VERSION = 0;
constexpr idx_t HF_XET_XORB_CHUNK_HEADER_LEN = 8;
constexpr uint64_t HF_XET_XORB_MAX_STORED_LENGTH = (1ULL << 24) - 1;

//! Compress one chunk into exactly one LZ4 frame - which is what the format's per-chunk scheme byte
//! and 3-byte compressed length describe, and what xet-core does with lz4_flex's FrameEncoder.
//!
//! The bytes do not have to match what the Rust encoder would have produced. An xorb's hash is over
//! *uncompressed* content, and a reader decodes a chunk from the length and scheme it is told, so any
//! conforming frame works; the default preferences (64 KiB blocks, no content checksum) are the plain
//! ones. What has to be true is that the frame decodes, which is why CompressChunk wraps this with a
//! round-trip check rather than trusting it.
idx_t CompressChunkLZ4(const_data_ptr_t data, idx_t len, string &out) {
	auto bound = LZ4F_compressFrameBound(size_t(len), nullptr);
	out.resize(size_t(bound));
	auto written = LZ4F_compressFrame(out.data(), bound, const_data_ptr_cast(data), size_t(len), nullptr);
	if (LZ4F_isError(written)) {
		throw IOException("LZ4 compression of a %llu byte Xet chunk failed: %s", len,
		                  LZ4F_getErrorName(written));
	}
	out.resize(size_t(written));
	return written;
}

//! Decode one LZ4 frame back, and fail loudly if it does not come out as the bytes that went in.
//! The uncompressed length is known here (it is the chunk size the merkle tree was built over), so
//! the check is exact rather than a plausibility test.
void DecompressChunkLZ4(const string &frame, const_data_ptr_t expected, idx_t expected_len) {
	LZ4F_dctx *ctx = nullptr;
	auto status = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
	if (LZ4F_isError(status)) {
		throw IOException("could not create an LZ4 decompressor: %s", LZ4F_getErrorName(status));
	}
	string out;
	out.resize(size_t(expected_len));
	idx_t written = 0;
	idx_t consumed = 0;
	try {
		while (true) {
			size_t dst_size = size_t(expected_len) - size_t(written);
			size_t src_size = size_t(frame.size()) - size_t(consumed);
			auto result = LZ4F_decompress(ctx, out.data() + written, &dst_size, frame.data() + consumed, &src_size,
			                              nullptr);
			if (LZ4F_isError(result)) {
				throw IOException("LZ4 decompression of a written Xet chunk failed: %s",
				                  LZ4F_getErrorName(result));
			}
			written += dst_size;
			consumed += src_size;
			if (result == 0) {
				break; // the frame is complete
			}
			if (dst_size == 0 && src_size == 0) {
				throw IOException("LZ4 decompression of a written Xet chunk made no progress");
			}
		}
	} catch (...) {
		LZ4F_freeDecompressionContext(ctx);
		throw;
	}
	LZ4F_freeDecompressionContext(ctx);
	if (written != expected_len || memcmp(out.data(), const_data_ptr_cast(expected), size_t(expected_len)) != 0) {
		throw IOException("LZ4 round-trip check failed: a %llu byte Xet chunk came back as %llu bytes of "
		                  "something else",
		                  expected_len, written);
	}
}

void AppendU32(string &out, uint32_t value) {
	out.push_back(char(value & 0xFF));
	out.push_back(char((value >> 8) & 0xFF));
	out.push_back(char((value >> 16) & 0xFF));
	out.push_back(char((value >> 24) & 0xFF));
}

void AppendU24(string &out, uint32_t value) {
	out.push_back(char(value & 0xFF));
	out.push_back(char((value >> 8) & 0xFF));
	out.push_back(char((value >> 16) & 0xFF));
}

} // namespace

HFXetXorbBuilder::HFXetXorbBuilder(HFXetCompressionScheme scheme) : scheme(scheme) {
	if (scheme != HFXetCompressionScheme::NONE && scheme != HFXetCompressionScheme::LZ4) {
		// The scheme byte exists in the format and the xorb hash does not care which one is used - it
		// covers *uncompressed* contents - but every scheme has to produce bytes the Hub's reader can
		// decode, so each one is added deliberately and checked against the reference (docs/XET.md).
		// BYTE_GROUPING_4_LZ4 additionally needs the bg4 splitter, and Auto is a policy, not a codec.
		throw NotImplementedException("Xet chunk compression scheme %d is not implemented by the hf "
		                              "extension yet; see docs/XET.md",
		                              int(scheme));
	}
}

idx_t HFXetXorbBuilder::FooterLength(idx_t num_chunks) {
	// three 7-byte idents, three version bytes, five u32s (num_chunks three times and the two
	// section offsets), the xorb hash, the reserved buffer - then per chunk a hash and two u32s
	return 3 * sizeof(HF_XET_XORB_IDENT) + 3 + 5 * sizeof(uint32_t) + 32 + HF_XET_XORB_FOOTER_BUFFER_LEN +
	       num_chunks * (32 + 2 * sizeof(uint32_t));
}

bool HFXetXorbBuilder::IsFull() const {
	return chunks.size() >= HF_XET_MAX_XORB_CHUNKS || uncompressed_bytes >= HF_XET_MAX_XORB_BYTES;
}

bool HFXetXorbBuilder::WouldOverflow(idx_t len) const {
	return chunks.size() + 1 > HF_XET_MAX_XORB_CHUNKS || uncompressed_bytes + len > HF_XET_MAX_XORB_BYTES;
}

void HFXetXorbBuilder::AddChunk(const_data_ptr_t data, idx_t len) {
	if (len > HF_XET_MAX_CHUNK_SIZE) {
		throw InternalException("Xet chunk of %llu bytes exceeds the %llu byte maximum", len,
		                        uint64_t(HF_XET_MAX_CHUNK_SIZE));
	}
	if (len > HF_XET_XORB_MAX_STORED_LENGTH) {
		// unreachable while the chunk maximum is smaller than the header's 3-byte field, and worth
		// naming if those two ever stop agreeing
		throw InternalException("Xet chunk of %llu bytes does not fit a 3-byte chunk header length", len);
	}
	if (WouldOverflow(len)) {
		// the caller is expected to check, and a xorb that silently exceeds the limits is one the CAS
		// refuses without telling us which of its own numbers we broke
		throw InternalException("Xet xorb cannot take a %llu byte chunk after %llu chunks / %llu bytes "
		                        "(limits: %llu chunks, %llu bytes)",
		                        len, chunks.size(), uncompressed_bytes, HF_XET_MAX_XORB_CHUNKS,
		                        uint64_t(HF_XET_MAX_XORB_BYTES));
	}
	auto hash = HFXetChunkHash(data, len);
	// Compress when the scheme says to, and store exactly what the frame came out as - including when
	// the frame is *larger* than the chunk, which is what an incompressible chunk (already-compressed
	// parquet columns, random weights) does. Reference clients behave the same way: `Auto` resolves to
	// LZ4 or bg4-LZ4 and never to "leave it alone", so a chunk that grew by a few bytes of framing is
	// what the ecosystem uploads, and a per-chunk decision would make our xorbs differ from theirs for
	// no gain - the CAS stores either way, and the hash is of the bytes before this line either way.
	string compressed;
	const_data_ptr_t stored = data;
	idx_t stored_len = len;
	if (scheme == HFXetCompressionScheme::LZ4) {
		// The CAS object we are about to create cannot be edited and is addressed by the hash of the
		// *uncompressed* bytes, so a frame that only this one encoder could read would be a corrupt
		// object forever, with the hash looking fine the whole way through. Decompressing what we just
		// compressed costs milliseconds against a network round trip, and turns "our LZ4 works" from an
		// assumption about vendored code into a property of the bytes being posted.
		CompressChunkLZ4(data, len, compressed);
		DecompressChunkLZ4(compressed, data, len);
		stored = const_data_ptr_cast(compressed.data());
		stored_len = compressed.size();
		if (stored_len > HF_XET_XORB_MAX_STORED_LENGTH) {
			throw InternalException("LZ4-compressed Xet chunk of %llu bytes does not fit a 3-byte chunk "
			                        "header length",
			                        stored_len);
		}
	}
	body.push_back(char(HF_XET_XORB_CHUNK_HEADER_VERSION));
	AppendU24(body, uint32_t(stored_len));
	body.push_back(char(uint8_t(scheme)));
	AppendU24(body, uint32_t(len));
	body.append(const_char_ptr_cast(stored), stored_len);

	uncompressed_bytes += len;
	chunks.push_back({hash, uint32_t(len), uint32_t(body.size())});
	nodes.push_back({hash, len});
}

HFXetHash HFXetXorbBuilder::Hash() const {
	return HFXetXorbHash(nodes);
}

HFXetXorb HFXetXorbBuilder::Finish(const uint8_t *nonce) {
	HFXetXorb xorb;
	xorb.hash = Hash();
	xorb.chunks = chunks;

	string footer;
	footer.reserve(size_t(FooterLength(chunks.size())));

	// the hash is over chunk contents only, so it can be written into the footer it describes
	footer.append(const_char_ptr_cast(HF_XET_XORB_IDENT), sizeof(HF_XET_XORB_IDENT));
	footer.push_back(char(HF_XET_XORB_FORMAT_VERSION));
	footer.append(const_char_ptr_cast(xorb.hash.bytes), 32);

	footer.append(const_char_ptr_cast(HF_XET_XORB_IDENT_HASHES), sizeof(HF_XET_XORB_IDENT_HASHES));
	footer.push_back(char(HF_XET_XORB_HASHES_VERSION));
	AppendU32(footer, uint32_t(chunks.size()));
	for (auto &chunk : chunks) {
		footer.append(const_char_ptr_cast(chunk.hash.bytes), 32);
	}

	footer.append(const_char_ptr_cast(HF_XET_XORB_IDENT_BOUNDARIES), sizeof(HF_XET_XORB_IDENT_BOUNDARIES));
	footer.push_back(char(HF_XET_XORB_BOUNDARIES_VERSION));
	AppendU32(footer, uint32_t(chunks.size()));
	for (auto &chunk : chunks) {
		AppendU32(footer, chunk.physical_end);
	}
	uint32_t unpacked_end = 0;
	for (auto &chunk : chunks) {
		unpacked_end += chunk.uncompressed_size;
		AppendU32(footer, unpacked_end);
	}

	// the fixed-size tail, from which the two sections are found by counting backwards
	AppendU32(footer, uint32_t(chunks.size()));
	// Both offsets count back from the end of the footer struct - which does not include the trailing
	// length u32 - so each is the section's own size plus everything after it.
	auto boundary_offset_from_end =
	    uint32_t(sizeof(HF_XET_XORB_IDENT_BOUNDARIES) + 1 + sizeof(uint32_t) +
	             chunks.size() * 2 * sizeof(uint32_t) + /* the tail: */ 3 * sizeof(uint32_t) +
	             HF_XET_XORB_FOOTER_BUFFER_LEN);
	auto hashes_offset_from_end = uint32_t(sizeof(HF_XET_XORB_IDENT_HASHES) + 1 + sizeof(uint32_t) +
	                                       chunks.size() * 32 + boundary_offset_from_end);
	AppendU32(footer, hashes_offset_from_end);
	AppendU32(footer, boundary_offset_from_end);

	string reserved(HF_XET_XORB_FOOTER_BUFFER_LEN, '\0');
	if (nonce) {
		reserved.replace(0, HF_XET_XORB_NONCE_LEN, const_char_ptr_cast(nonce), HF_XET_XORB_NONCE_LEN);
	}
	footer += reserved;

	xorb.bytes = std::move(body);
	body.clear();
	xorb.bytes += footer;
	AppendU32(xorb.bytes, uint32_t(footer.size()));

	// the builder is single-use by convention, but an object that has handed over its bytes has to
	// report itself empty in every respect - a stale byte count reads as "full" to the next caller
	chunks.clear();
	nodes.clear();
	body.clear();
	uncompressed_bytes = 0;
	return xorb;
}

//===----------------------------------------------------------------------===//
// self-test
//===----------------------------------------------------------------------===//

namespace {

string FormatSizes(const vector<uint32_t> &sizes) {
	string out;
	for (idx_t i = 0; i < sizes.size(); i++) {
		if (i > 0) {
			out += ",";
		}
		out += to_string(sizes[i]);
	}
	return out;
}

} // namespace

idx_t HFXetXorbSelfTest(vector<std::tuple<string, bool, string, string>> &results) {
	idx_t failures = 0;
	auto check = [&](const string &name, const string &expected, const string &got) {
		results.emplace_back(name, expected == got, expected, got);
		if (expected != got) {
			failures++;
		}
	};

	// An empty xorb is what an empty file produces, and it is the one case where the whole object is
	// footer; pinning its length pins the framing arithmetic everything else inherits.
	{
		HFXetXorbBuilder empty_xorb;
		auto xorb = empty_xorb.Finish();
		check("empty xorb chunk count", "0", to_string(xorb.chunks.size()));
		check("empty xorb hash", HFXetHash().Hex(), xorb.hash.Hex());
		check("empty xorb length", "96", to_string(xorb.bytes.size()));
		// the trailing length u32 does not count itself, hence 92 + 4
		check("empty xorb footer length", "92", to_string(xorb.bytes.size() - 4));
	}

	for (auto &test : HFXetXorbVectors()) {
		auto name = string(test.name);
		auto input = HFXetTestInputBytes(test.input, test.size, test.seed, test.pattern);

		HFXetXorbBuilder builder(test.scheme);
		HFXetChunker chunker;
		auto on_chunk = [&](const_data_ptr_t data, idx_t len) {
			if (builder.IsFull()) {
				throw InternalException("xorb vector %s is larger than one xorb", name);
			}
			builder.AddChunk(data, len);
		};
		chunker.Process(const_data_ptr_cast(input.data()), input.size(), on_chunk);
		chunker.Finish(on_chunk);

		auto xorb = builder.Finish();
		check(name + " xorb chunk count", to_string(test.num_chunks), to_string(xorb.chunks.size()));
		check(name + " xorb length", to_string(test.length), to_string(xorb.bytes.size()));
		check(name + " xorb hash", test.xorb_hash, xorb.hash.Hex());
		// the fingerprint covers every byte we would upload, footer included: the only way to tell a
		// layout bug apart from a hash that happens to still come out right
		check(name + " xorb bytes", test.fingerprint,
		      HFXetChunkHash(const_data_ptr_cast(xorb.bytes.data()), xorb.bytes.size()).Hex());

		vector<uint32_t> sizes;
		vector<uint32_t> physical;
		for (auto &chunk : xorb.chunks) {
			sizes.push_back(chunk.uncompressed_size);
			physical.push_back(chunk.physical_end);
		}
		check(name + " xorb chunk sizes", test.chunk_sizes, FormatSizes(sizes));
		check(name + " xorb boundaries", test.chunk_physical_ends, FormatSizes(physical));

		// The two footer offsets are how a reader finds the sections without knowing the chunk count,
		// so they are checked by following them rather than by trusting the arithmetic: the bytes they
		// point at have to be the section identifiers.
		auto length = xorb.bytes.size();
		auto read_u32_at = [&](idx_t position) {
			return uint32_t(uint8_t(xorb.bytes[position])) | (uint32_t(uint8_t(xorb.bytes[position + 1])) << 8) |
			       (uint32_t(uint8_t(xorb.bytes[position + 2])) << 16) |
			       (uint32_t(uint8_t(xorb.bytes[position + 3])) << 24);
		};
		auto ident_at = [&](idx_t position, const uint8_t *expected) {
			return position + 7 <= length && memcmp(xorb.bytes.data() + position, expected, 7) == 0;
		};
		auto info_length = read_u32_at(length - 4);
		auto footer_start = length - 4 - info_length;
		check(name + " xorb footer ident", "true", ident_at(footer_start, HF_XET_XORB_IDENT) ? "true" : "false");
		check(name + " xorb chunk section ends at the footer", to_string(footer_start),
		      xorb.chunks.empty() ? "0" : to_string(xorb.chunks.back().physical_end));
		// the fixed tail, back to front: nonce, boundary offset, hashes offset, chunk count
		auto footer_end = footer_start + info_length;
		auto boundary_offset_from_end = read_u32_at(footer_end - 20);
		auto hashes_offset_from_end = read_u32_at(footer_end - 24);
		// offsets count back from the end of the footer struct, which excludes the trailing length
		check(name + " xorb boundaries section is where the footer says", "true",
		      ident_at(length - 4 - boundary_offset_from_end, HF_XET_XORB_IDENT_BOUNDARIES) ? "true" : "false");
		check(name + " xorb hashes section is where the footer says", "true",
		      ident_at(length - 4 - hashes_offset_from_end, HF_XET_XORB_IDENT_HASHES) ? "true" : "false");
	}

	// LZ4 frames. These are deliberately *not* reference-pinned byte-for-byte: two conforming LZ4
	// encoders produce different frames for the same input and a reader cannot tell them apart, since
	// it decodes a chunk from the stored length and scheme byte. What would break in a way a reader
	// notices is the framing arithmetic and the hash, so those are what gets checked - plus one check
	// per direction of the size change, because a "compression" that never compresses and one that
	// shrinks random data are both bugs.
	for (auto &test : HFXetXorbVectors()) {
		auto lz4_name = string(test.name);
		auto lz4_input = HFXetTestInputBytes(test.input, test.size, test.seed, test.pattern);

		HFXetXorbBuilder lz4(HFXetCompressionScheme::LZ4);
		HFXetChunker lz4_chunker;
		auto lz4_on_chunk = [&](const_data_ptr_t data, idx_t len) {
			if (lz4.IsFull()) {
				throw InternalException("xorb vector %s is larger than one xorb", lz4_name);
			}
			lz4.AddChunk(data, len);
		};
		lz4_chunker.Process(const_data_ptr_cast(lz4_input.data()), lz4_input.size(), lz4_on_chunk);
		lz4_chunker.Finish(lz4_on_chunk);
		auto xorb = lz4.Finish();

		// The one reference number an LZ4 xorb does have: its hash, which is over the chunk contents as
		// they were before compression. If a codec ever touched what gets hashed, this is where it shows.
		check(lz4_name + " lz4 xorb hash is the uncompressed one", test.xorb_hash, xorb.hash.Hex());
		check(lz4_name + " lz4 xorb chunk count", to_string(test.num_chunks), to_string(xorb.chunks.size()));

		uint64_t uncompressed_total = 0;
		for (auto &chunk : xorb.chunks) {
			uncompressed_total += chunk.uncompressed_size;
		}
		check(lz4_name + " lz4 xorb unpacked sizes still sum to the input", to_string(lz4_input.size()),
		      to_string(uncompressed_total));

		// physical_end is the compressed space now, so the two offset arrays can no longer be the same
		// numbers: the chunk section has to end where the footer starts, and the footer has to still be
		// findable by its trailing length
		auto length = xorb.bytes.size();
		// the trailing u32, read forward: it is little-endian, so the byte at length-4 is the low one
		auto info_length = uint32_t(uint8_t(xorb.bytes[length - 4])) | (uint32_t(uint8_t(xorb.bytes[length - 3])) << 8) |
		                   (uint32_t(uint8_t(xorb.bytes[length - 2])) << 16) |
		                   (uint32_t(uint8_t(xorb.bytes[length - 1])) << 24);
		auto footer_start = length - 4 - info_length;
		check(lz4_name + " lz4 xorb chunk section ends at the footer", to_string(footer_start),
		      xorb.chunks.empty() ? "0" : to_string(xorb.chunks.back().physical_end));
		check(lz4_name + " lz4 xorb footer ident", "true",
		      memcmp(xorb.bytes.data() + footer_start, HF_XET_XORB_IDENT, 7) == 0 ? "true" : "false");

		auto compressible = test.input == HFXetTestInput::REPEAT && test.size >= 1000;
		check(lz4_name + " lz4 xorb size moved the expected way", compressible ? "smaller" : "not smaller",
		      xorb.bytes.size() < test.length ? "smaller" : "not smaller");
	}

	return failures;
}

} // namespace duckdb
