#include "hf_xet_xorb.hpp"

#include "hf_xet_chunk.hpp"

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
	if (scheme != HFXetCompressionScheme::NONE) {
		// The scheme byte exists in the format and the footer does not care which one is used - the
		// xorb hash covers *uncompressed* contents - but every non-None scheme has to produce bytes
		// the Hub's reader can decode, so each one is added deliberately and diffed against the
		// reference (docs/XET.md, M3). Uncompressed uploads are valid meanwhile.
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
	if (IsFull()) {
		throw InternalException("Xet xorb is full after %llu chunks / %llu bytes", chunks.size(),
		                        uncompressed_bytes);
	}
	// The reference falls back to storing a chunk uncompressed when compressing it did not pay; with
	// only the None scheme that decision is already taken, and `len` is both lengths.
	auto hash = HFXetChunkHash(data, len);
	body.push_back(char(HF_XET_XORB_CHUNK_HEADER_VERSION));
	AppendU24(body, uint32_t(len));
	body.push_back(char(uint8_t(scheme)));
	AppendU24(body, uint32_t(len));
	body.append(const_char_ptr_cast(data), len);

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

	chunks.clear();
	nodes.clear();
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

	return failures;
}

} // namespace duckdb
