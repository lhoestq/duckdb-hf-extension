#include "hf_fs.hpp"

#include "hf_settings.hpp"

#include "duckdb/common/file_opener.hpp"

namespace duckdb {

static string SettingAsString(optional_ptr<FileOpener> opener, const char *key, const string &path) {
	if (!opener) {
		return "";
	}
	Value value;
	FileOpenerInfo info;
	info.file_path = path;
	if (!FileOpener::TryGetCurrentSetting(opener, Identifier(key), value, info) || value.IsNull()) {
		return "";
	}
	return StringValue::Get(value);
}

HFWriteHandle::HFWriteHandle(HFFileSystem &file_system, const string &path, FileOpenFlags flags, HFTarget target_p,
                             optional_ptr<FileOpener> opener)
    : FileHandle(file_system, path, flags), target(std::move(target_p)) {
	// settings are resolved once here: the opener is not guaranteed to outlive the handle
	cache_dir = SettingAsString(opener, "hf_cache_dir", path);
	commit_message = SettingAsString(opener, "hf_commit_message", path);
	create_repo = HFSettingAsBool(opener, "hf_create_repo", false);
	if (commit_message.empty()) {
		commit_message = HFDefaultCommitMessage(target.url);
	}
}

void HFWriteHandle::EnsureTempFile() {
	if (temp_handle) {
		return;
	}
	string extension;
	auto dot = target.url.path.find_last_of('.');
	if (dot != string::npos && dot > target.url.path.find_last_of('/')) {
		extension = target.url.path.substr(dot + 1);
	}
	auto &local = FileSystem::GetLocal(file_system.Cast<HFFileSystem>().Db());
	temp_path = HFMakeTempPathIn(file_system.Cast<HFFileSystem>().Db(), cache_dir, extension);
	// CREATE_NEW also guards against a stale scratch file from a crashed run
	temp_handle = local.OpenFile(temp_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
}

void HFWriteHandle::Write(data_ptr_t buffer, idx_t nr_bytes) {
	if (aborted) {
		throw IOException("'%s' was aborted and can no longer be written to", path);
	}
	EnsureTempFile();
	auto &local = FileSystem::GetLocal(file_system.Cast<HFFileSystem>().Db());
	local.Write(*temp_handle, buffer, NumericCast<int64_t>(nr_bytes));
	bytes_written += nr_bytes;
}

void HFWriteHandle::CleanupTempFile() {
	temp_handle.reset();
	if (!temp_path.empty()) {
		FileSystem::GetLocal(file_system.Cast<HFFileSystem>().Db()).TryRemoveFile(temp_path, nullptr);
		temp_path.clear();
	}
}

void HFWriteHandle::Abort() {
	if (closed || aborted) {
		return;
	}
	aborted = true;
	CleanupTempFile();
}

void HFWriteHandle::Close() {
	if (closed) {
		return;
	}
	closed = true;
	if (aborted) {
		CleanupTempFile();
		return;
	}
	// an empty file is still a file: `COPY (SELECT 1 WHERE false) TO 'hf://...'` should create it
	EnsureTempFile();
	temp_handle->Close();
	temp_handle.reset();

	auto &db = file_system.Cast<HFFileSystem>().Db();
	try {
		auto blob = HFComputeBlobInfo(db, temp_path);
		// the opener is gone by now, so the flag resolved at open time is the one that counts
		HFUploadLocalFile(db, nullptr, target, temp_path, blob, commit_message, create_repo);
	} catch (std::exception &) {
		CleanupTempFile();
		throw;
	}
	CleanupTempFile();
}

} // namespace duckdb
