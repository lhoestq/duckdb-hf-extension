#include "hf_crypto.hpp"

namespace duckdb {

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

static inline uint32_t Rotr32(uint32_t x, uint32_t n) {
	return (x >> n) | (x << (32 - n));
}

HFSha256::HFSha256() : bit_length(0), buf_len(0) {
	state[0] = 0x6a09e667;
	state[1] = 0xbb67ae85;
	state[2] = 0x3c6ef372;
	state[3] = 0xa54ff53a;
	state[4] = 0x510e527f;
	state[5] = 0x9b05688c;
	state[6] = 0x1f83d9ab;
	state[7] = 0x5be0cd19;
}

void HFSha256::ProcessBlock(const uint8_t *block) {
	uint32_t w[64];
	for (idx_t i = 0; i < 16; i++) {
		w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) |
		       uint32_t(block[i * 4 + 3]);
	}
	for (idx_t i = 16; i < 64; i++) {
		auto s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		auto s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	auto a = state[0];
	auto b = state[1];
	auto c = state[2];
	auto d = state[3];
	auto e = state[4];
	auto f = state[5];
	auto g = state[6];
	auto h = state[7];
	for (idx_t i = 0; i < 64; i++) {
		auto s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
		auto ch = (e & f) ^ (~e & g);
		auto temp1 = h + s1 + ch + SHA256_K[i] + w[i];
		auto s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
		auto maj = (a & b) ^ (a & c) ^ (b & c);
		auto temp2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

void HFSha256::Update(const_data_ptr_t data, idx_t len) {
	auto ptr = const_data_ptr_cast(data);
	bit_length += uint64_t(len) * 8;
	while (len > 0) {
		auto to_copy = MinValue<idx_t>(len, 64 - buf_len);
		memcpy(block_buf + buf_len, ptr, to_copy);
		buf_len += to_copy;
		ptr += to_copy;
		len -= to_copy;
		if (buf_len == 64) {
			ProcessBlock(block_buf);
			buf_len = 0;
		}
	}
}

string HFSha256::FinalHex() {
	uint8_t padding[80];
	idx_t pad_len = 0;
	padding[pad_len++] = 0x80;
	while ((buf_len + pad_len) % 64 != 56) {
		padding[pad_len++] = 0x00;
	}
	for (idx_t i = 0; i < 8; i++) {
		padding[pad_len++] = uint8_t((bit_length >> ((7 - i) * 8)) & 0xFF);
	}
	Update(padding, pad_len);

	string result;
	static const char *HEX = "0123456789abcdef";
	for (idx_t i = 0; i < 8; i++) {
		for (idx_t j = 0; j < 4; j++) {
			auto byte = uint8_t(state[i] >> (24 - j * 8));
			result += HEX[byte >> 4];
			result += HEX[byte & 0x0F];
		}
	}
	buf_len = 0;
	bit_length = 0;
	state[0] = 0x6a09e667;
	state[1] = 0xbb67ae85;
	state[2] = 0x3c6ef372;
	state[3] = 0xa54ff53a;
	state[4] = 0x510e527f;
	state[5] = 0x9b05688c;
	state[6] = 0x1f83d9ab;
	state[7] = 0x5be0cd19;
	return result;
}

string HFSha256::Hex(const_data_ptr_t data, idx_t len) {
	HFSha256 sha;
	sha.Update(data, len);
	return sha.FinalHex();
}

static const char *B64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

string HFBase64::Encode(const_data_ptr_t data, idx_t len) {
	string out;
	out.reserve(((len + 2) / 3) * 4);
	idx_t i = 0;
	while (i + 3 <= len) {
		auto v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
		out += B64_ALPHABET[(v >> 18) & 0x3F];
		out += B64_ALPHABET[(v >> 12) & 0x3F];
		out += B64_ALPHABET[(v >> 6) & 0x3F];
		out += B64_ALPHABET[v & 0x3F];
		i += 3;
	}
	auto remaining = len - i;
	if (remaining == 1) {
		auto v = uint32_t(data[i]) << 16;
		out += B64_ALPHABET[(v >> 18) & 0x3F];
		out += B64_ALPHABET[(v >> 12) & 0x3F];
		out += "==";
	} else if (remaining == 2) {
		auto v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
		out += B64_ALPHABET[(v >> 18) & 0x3F];
		out += B64_ALPHABET[(v >> 12) & 0x3F];
		out += B64_ALPHABET[(v >> 6) & 0x3F];
		out += '=';
	}
	return out;
}

void HFJsonWriter::EscapeTo(string &out, const string &value) {
	for (auto c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				static const char *HEX = "0123456789abcdef";
				out += "\\u00";
				out += HEX[(static_cast<unsigned char>(c) >> 4) & 0xF];
				out += HEX[static_cast<unsigned char>(c) & 0xF];
			} else {
				out += c;
			}
		}
	}
}

string HFJsonWriter::Escape(const string &value) {
	string out;
	EscapeTo(out, value);
	return out;
}

} // namespace duckdb
