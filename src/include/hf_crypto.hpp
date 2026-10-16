#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! Streaming SHA-256. The Hub's LFS/preupload endpoints identify blobs by their sha256, and duckdb's
//! EncryptionUtil is only available when an encryption backend is linked in, so we carry our own.
class HFSha256 {
public:
	HFSha256();

	void Update(const_data_ptr_t data, idx_t len);
	//! Lowercase hex digest, resets the context
	string FinalHex();

	static string Hex(const_data_ptr_t data, idx_t len);
	static string Hex(const string &input) {
		return Hex(const_data_ptr_cast(input.data()), input.size());
	}

private:
	void ProcessBlock(const uint8_t *block);

	uint32_t state[8];
	uint64_t bit_length;
	uint8_t block_buf[64];
	idx_t buf_len;
};

//! Minimal base64 (the Hub APIs speak it for `sample` and inline file content)
struct HFBase64 {
	static string Encode(const_data_ptr_t data, idx_t len);
	static string Encode(const string &input) {
		return Encode(const_data_ptr_cast(input.data()), input.size());
	}
};

//! JSON helpers: the Hub speaks small, predictable JSON, and depending on the json extension for
//! building/reading it would make `hf` a much heavier dependency than it needs to be.
struct HFJsonWriter {
	string out;

	HFJsonWriter &Raw(const string &value) {
		out += value;
		return *this;
	}
	HFJsonWriter &String(const string &value) {
		out += '"';
		EscapeTo(out, value);
		out += '"';
		return *this;
	}
	static void EscapeTo(string &out, const string &value);
	static string Escape(const string &value);
};

} // namespace duckdb
