#pragma once

namespace duckdb {

struct DBConfig;

//! Extension options (`SET hf_...`). Everything is optional: tokens are discovered from
//! secrets, then the `hf_token` setting, then `HF_TOKEN`, then `HF_TOKEN_PATH`, then
//! `~/.cache/huggingface/token` - exactly like huggingface_hub does.
struct HFSettings {
	static void Register(DBConfig &config);
};

} // namespace duckdb
