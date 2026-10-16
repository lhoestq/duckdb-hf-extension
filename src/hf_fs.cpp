#include "hf_fs.hpp"

#include "hf_crypto.hpp"
#include "hf_settings.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Target resolution / clients
//===----------------------------------------------------------------------===//

string HFUtil::Redact(const string &message, const string &secret) {
	if (secret.empty() || message.find(secret) == string::npos) {
		return message;
	}
	return StringUtil::Replace(message, secret, "<redacted>");
}

//! Secrets match on the repository, so `CREATE SECRET (TYPE HUGGINGFACE, SCOPE 'hf://datasets/my-org/')` works
static string SecretScope(const HFUrl &url) {
	string scope = "hf://" + url.repo_type + "/" + url.repository;
	if (!url.IsBucket()) {
		scope += "/";
	}
	return scope;
}

HFTarget HFResolveTarget(const string &path, optional_ptr<FileOpener> opener, DatabaseInstance &db) {
	HFTarget target;
	target.url = HFUrl::Parse(path, HFSecrets::ResolveEndpoint(opener, &db));

	FileOpenerInfo info;
	info.file_path = path;
	target.connection = HFSecrets::Resolve(opener, &info, SecretScope(target.url), &db);
	target.url.endpoint = target.connection.endpoint;
	return target;
}

unique_ptr<HFClient> HFMakeClient(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target) {
	return make_uniq<HFClient>(db, opener, target.connection, target.url.ResolveUrl());
}

string HFDefaultCommitMessage(const HFUrl &url) {
	return "DuckDB write to " + HFUtil::Join(url.repo_type + "/" + url.repository, url.path);
}

//===----------------------------------------------------------------------===//
// Listings
//===----------------------------------------------------------------------===//

static void ParseListEntry(const string &object, const std::function<void(const HFListEntry &)> &callback) {
	HFListEntry entry;
	HFJson::ForEachMember(object, [&](const string &key, const string &raw) {
		if (key == "path") {
			entry.path = HFJson::AsString(raw);
		} else if (key == "type") {
			auto type = HFJson::AsString(raw);
			entry.is_dir = (type == "directory" || type == "folder");
		} else if (key == "size") {
			entry.size = NumericCast<idx_t>(MaxValue<int64_t>(0, HFJson::AsInt(raw)));
		} else if (key == "lfs") {
			// LFS files report their real size and sha256 in the nested "lfs" object
			HFJson::ForEachMember(raw, [&](const string &lfs_key, const string &lfs_raw) {
				if (lfs_key == "size") {
					entry.size = NumericCast<idx_t>(MaxValue<int64_t>(0, HFJson::AsInt(lfs_raw)));
				} else if (lfs_key == "sha256" || lfs_key == "oid") {
					entry.lfs_oid = HFJson::AsString(lfs_raw);
				}
			});
		}
	});
	if (!entry.path.empty()) {
		callback(entry);
	}
}

void HFListEntries(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &dir, bool recursive,
                   const std::function<void(const HFListEntry &)> &callback, bool with_details,
                   bool tolerate_not_found) {
	idx_t limit = HF_DEFAULT_LIST_PAGE_SIZE;
	if (opener) {
		Value page_size;
		FileOpenerInfo info;
		info.file_path = dir.url.ToString();
		if (FileOpener::TryGetCurrentSetting(opener, Identifier("hf_max_per_page"), page_size, info) &&
		    !page_size.IsNull()) {
			auto configured = page_size.GetValue<idx_t>();
			if (configured > 0) {
				limit = configured;
			}
		}
	}

	auto client = HFMakeClient(db, opener, dir);
	string next_url;
	idx_t pages = 0;
	while (true) {
		auto page = client->ListPage(dir.url, limit, recursive, next_url, tolerate_not_found);
		if (page.not_found) {
			return;
		}
		HFJson::ForEachObject(page.body, [&](const string &object) { ParseListEntry(object, callback); });
		if (!page.has_next || page.next_url.empty()) {
			return;
		}
		if (++pages > 200000) {
			throw IOException("Listing '%s' did not terminate after %llu pages", dir.url.ToString(), pages);
		}
		next_url = std::move(page.next_url);
	}
}

bool HFTryGetFileSize(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target, idx_t &size,
                      string &etag) {
	auto client = HFMakeClient(db, opener, target);
	auto response = client->Head(target.url.ResolveUrl());
	if (response->HasRequestError() || static_cast<int>(response->status) >= 400) {
		return false;
	}
	if (response->HasHeader("x-linked-size")) {
		size = NumericCast<idx_t>(std::stoll(response->GetHeaderValue("x-linked-size")));
	} else if (response->HasHeader("Content-Length")) {
		size = NumericCast<idx_t>(std::stoll(response->GetHeaderValue("Content-Length")));
	} else {
		return false;
	}
	if (response->HasHeader("ETag")) {
		etag = response->GetHeaderValue("ETag");
	}
	return true;
}

//===----------------------------------------------------------------------===//
// HFFileSystem: basics
//===----------------------------------------------------------------------===//

bool HFFileSystem::CanHandleFile(const string &fpath) {
	return StringUtil::StartsWith(StringUtil::Lower(fpath), "hf://");
}

bool HFFileSystem::IsManuallySet() {
	// httpfs also ships a read-only `hf://` filesystem, without this the last one registered would win
	return true;
}

string HFFileSystem::GetName() const {
	return "HFFileSystem";
}

bool HFFileSystem::IsPathAbsolute(const string &path) {
	return StringUtil::StartsWith(StringUtil::Lower(path), "hf://");
}

string HFFileSystem::PathSeparator(const string &path) {
	return "/";
}

void HFFileSystem::Register(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	FileSystem::GetFileSystem(db).RegisterSubSystem(make_uniq<HFFileSystem>(db));
}

string HFFileSystem::GetHFUrl(const HFUrl &dir, const string &entry_path) {
	HFUrl result = dir;
	result.path = "/" + entry_path;
	return result.ToString();
}

//===----------------------------------------------------------------------===//
// HFFileSystem: open / read / write dispatch
//===----------------------------------------------------------------------===//

unique_ptr<FileHandle> HFFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                              optional_ptr<FileOpener> opener) {
	auto target = HFResolveTarget(path, opener, db);
	if (flags.OpenForWriting() || flags.CreateFileIfNotExists() || flags.OverwriteExistingFile() ||
	    flags.OpenForAppending()) {
		if (target.url.IsBucket()) {
			throw IOException("Writing to Hugging Face buckets ('%s') is not supported yet: bucket uploads go through "
			                  "Xet storage, which only the `hf_xet` client speaks - use `hf buckets cp` to write, or "
			                  "write to a dataset/model/space repo (hf://datasets/...) which supports COPY TO",
			                  path);
		}
		return make_uniq<HFWriteHandle>(*this, path, flags, std::move(target), opener);
	}
	return make_uniq<HFReadHandle>(*this, path, flags, std::move(target), opener);
}

void HFFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	if (nr_bytes <= 0) {
		return;
	}
	handle.Cast<HFReadHandle>().ReadAt(data_ptr_cast(buffer), NumericCast<idx_t>(location),
	                                   NumericCast<idx_t>(nr_bytes));
}

int64_t HFFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	if (nr_bytes <= 0) {
		return 0;
	}
	auto &hf_handle = handle.Cast<HFReadHandle>();
	auto size = hf_handle.Size();
	auto position = hf_handle.GetPosition();
	if (position >= size) {
		return 0;
	}
	auto to_read = MinValue<idx_t>(NumericCast<idx_t>(nr_bytes), size - position);
	hf_handle.ReadAt(data_ptr_cast(buffer), position, to_read);
	hf_handle.SetPosition(position + to_read);
	return NumericCast<int64_t>(to_read);
}

void HFFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw IOException("Writing at a specific offset is not supported for hf:// files");
}

int64_t HFFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	if (nr_bytes <= 0) {
		return 0;
	}
	handle.Cast<HFWriteHandle>().Write(data_ptr_cast(buffer), NumericCast<idx_t>(nr_bytes));
	return nr_bytes;
}

bool HFFileSystem::CanSeek() {
	return true;
}

void HFFileSystem::Seek(FileHandle &handle, idx_t location) {
	handle.Cast<HFReadHandle>().SetPosition(location);
}

idx_t HFFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<HFReadHandle>().GetPosition();
}

int64_t HFFileSystem::GetFileSize(FileHandle &handle) {
	if (handle.flags.OpenForWriting()) {
		return NumericCast<int64_t>(handle.Cast<HFWriteHandle>().BytesWritten());
	}
	return NumericCast<int64_t>(handle.Cast<HFReadHandle>().Size());
}

FileType HFFileSystem::GetFileType(FileHandle &handle) {
	return FileType::FILE_TYPE_REGULAR;
}

static FileMetadata HFMetrics(const string &path, idx_t size, const string &etag) {
	FileMetadata result;
	result.file_size = NumericCast<int64_t>(size);
	result.file_type = FileType::FILE_TYPE_REGULAR;
	result.version_tag = etag;
	return result;
}

FileMetadata HFFileSystem::Stats(FileHandle &handle) {
	auto &hf_handle = handle.Cast<HFReadHandle>();
	return HFMetrics(handle.path, hf_handle.Size(), hf_handle.ETag());
}

optional<FileMetadata> HFFileSystem::GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) {
	auto target = HFResolveTarget(file.path, opener, db);
	idx_t size = 0;
	string etag;
	if (!HFTryGetFileSize(db, opener, target, size, etag)) {
		return optional<FileMetadata>();
	}
	return HFMetrics(file.path, size, etag);
}

void HFFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	throw IOException("Truncating hf:// files is not supported");
}

void HFFileSystem::FileSync(FileHandle &handle) {
}

void HFFileSystem::AbortFileWrite(FileHandle &handle) {
	if (handle.flags.OpenForWriting()) {
		handle.Cast<HFWriteHandle>().Abort();
	}
}

string HFFileSystem::GetVersionTag(FileHandle &handle) {
	return handle.Cast<HFReadHandle>().ETag();
}

bool HFFileSystem::OnDiskFile(FileHandle &handle) {
	return false;
}

timestamp_t HFFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &hf_handle = handle.Cast<HFReadHandle>();
	return Timestamp::FromEpochSeconds(hf_handle.LastModified());
}

//===----------------------------------------------------------------------===//
// HFFileSystem: directories
//===----------------------------------------------------------------------===//

bool HFFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	auto target = HFResolveTarget(directory, opener, db);
	if (target.url.path.empty() || target.url.path == "/") {
		// the repository root: it exists if it can be listed
		try {
			bool any = false;
			HFListEntries(db, opener, target, false, [&](const HFListEntry &) { any = true; });
			return any;
		} catch (std::exception &) {
			return false;
		}
	}
	auto parent = target;
	parent.url.path = HFUtil::Parent(target.url.path);
	if (parent.url.path.empty()) {
		parent.url.path = "/";
	}
	auto base = HFUtil::BaseName(target.url.path);
	bool found = false;
	try {
		HFListEntries(db, opener, parent, false, [&](const HFListEntry &entry) {
			if (entry.is_dir && entry.path == base) {
				found = true;
			}
		});
	} catch (std::exception &) {
		return false;
	}
	return found;
}

bool HFFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                           optional_ptr<FileOpener> opener) {
	// directories are implicit on the Hub, there is nothing to create
	return true;
}

bool HFFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                           optional_ptr<FileOpener> opener) {
	if (options.mode != RemoveDirectoryMode::RECURSIVE) {
		throw IOException("Removing an hf:// directory requires RECURSIVE mode");
	}
	auto target = HFResolveTarget(directory, opener, db);
	vector<string> files;
	HFListEntries(db, opener, target, true, [&](const HFListEntry &entry) {
		if (!entry.is_dir) {
			files.push_back(entry.path);
		}
	});
	if (files.empty()) {
		return false;
	}
	HFDeletePaths(db, opener, target, files, {}, "Delete directory with DuckDB");
	return true;
}

bool HFFileSystem::ListFilesExtended(const string &directory,
                                     const std::function<void(OpenFileInfo &info)> &callback,
                                     optional_ptr<FileOpener> opener) {
	auto target = HFResolveTarget(directory, opener, db);
	if (HFUtil::HasGlob(target.url.path)) {
		throw IOException("ListFiles does not accept glob patterns, got '%s'", directory);
	}
	HFListEntries(db, opener, target, false, [&](const HFListEntry &entry) {
		OpenFileInfo info(GetHFUrl(target.url, entry.path));
		info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		if (entry.is_dir) {
			info.extended_info->options["is_dir"] = Value(true);
			info.extended_info->options["type"] = "directory";
		} else {
			info.extended_info->options["file_size"] = Value::UBIGINT(entry.size);
		}
		callback(info);
	});
	return true;
}

void HFFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto target = HFResolveTarget(filename, opener, db);
	HFDeletePaths(db, opener, target, {target.url.path}, {}, "Delete file with DuckDB");
}

bool HFFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	try {
		RemoveFile(filename, opener);
		return true;
	} catch (std::exception &) {
		return false;
	}
}

void HFFileSystem::RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) {
	if (filenames.empty()) {
		return;
	}
	// files written by one query usually share a repository: group them into one commit per repository
	std::map<string, std::pair<HFTarget, vector<string>>> groups;
	for (auto &filename : filenames) {
		auto target = HFResolveTarget(filename, opener, db);
		auto key = target.url.endpoint + "|" + target.url.repo_type + "|" + target.url.repository + "|" +
		           target.url.revision;
		auto &group = groups[key];
		if (!group.first.url.repository.size()) {
			group.first = target;
		}
		group.second.push_back(target.url.path);
	}
	for (auto &entry : groups) {
		HFDeletePaths(db, opener, entry.second.first, entry.second.second, {}, "Delete files with DuckDB");
	}
}

void HFFileSystem::MoveFile(const string &source, const string &target_path, optional_ptr<FileOpener> opener) {
	auto from = HFResolveTarget(source, opener, db);
	auto to = HFResolveTarget(target_path, opener, db);
	if (from.url.repo_type != to.url.repo_type || from.url.repository != to.url.repository ||
	    from.url.endpoint != to.url.endpoint) {
		throw IOException("Moving files between Hugging Face repositories is not supported ('%s' -> '%s')", source,
		                  target_path);
	}
	// the commit API has no rename: download, re-upload, then delete the original
	LocalFileSystem local;
	auto temp_path = HFMakeTempPath(db, opener, HFUtil::BaseName(source));
	{
		auto handle = OpenFile(source, FileFlags::FILE_FLAGS_READ, opener);
		auto size = NumericCast<idx_t>(GetFileSize(*handle));
		string buffer;
		buffer.resize(size);
		if (size > 0) {
			Read(*handle, &buffer[0], NumericCast<int64_t>(size), 0);
		}
		auto temp_handle =
		    local.OpenFile(temp_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		if (size > 0) {
			local.Write(*temp_handle, &buffer[0], NumericCast<int64_t>(size), 0);
		}
		temp_handle->Close();
	}
	try {
		auto blob = HFComputeBlobInfo(db, temp_path);
		HFUploadLocalFile(db, opener, to, temp_path, blob, "Move to " + to.url.path + " with DuckDB");
		HFDeletePaths(db, opener, from, {from.url.path}, {}, "Moved away with DuckDB");
	} catch (...) {
		local.TryRemoveFile(temp_path, nullptr);
		throw;
	}
	local.TryRemoveFile(temp_path, nullptr);
}

bool HFFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	auto target = HFResolveTarget(filename, opener, db);
	idx_t size = 0;
	string etag;
	if (HFTryGetFileSize(db, opener, target, size, etag)) {
		return true;
	}
	return DirectoryExists(filename, opener);
}

//===----------------------------------------------------------------------===//
// Glob
//===----------------------------------------------------------------------===//

static bool MatchPath(vector<string>::const_iterator key, const vector<string>::const_iterator &key_end,
                      vector<string>::const_iterator pattern, const vector<string>::const_iterator &pattern_end) {
	while (key != key_end) {
		if (pattern == pattern_end) {
			return false;
		}
		if (pattern->size() == 2 && (*pattern)[0] == '*' && (*pattern)[1] == '*') {
			if (pattern_end - pattern == 1) {
				// a trailing ** matches everything that is left
				return true;
			}
			if (MatchPath(key, key_end, std::next(pattern), pattern_end)) {
				return true;
			}
			++key;
		} else {
			if (!Glob(key->data(), key->length(), pattern->data(), pattern->length())) {
				return false;
			}
			++key;
			++pattern;
		}
	}
	return pattern == pattern_end;
}

//! Can a directory at this path still contain matches for the pattern?
static bool CanDescend(const string &dir_path, const vector<string> &pattern_splits) {
	auto dir_splits = StringUtil::Split(dir_path, "/");
	for (idx_t i = 0; i < dir_splits.size(); i++) {
		if (i >= pattern_splits.size()) {
			return false;
		}
		if (pattern_splits[i] == "**") {
			return true;
		}
		if (!Glob(dir_splits[i].data(), dir_splits[i].length(), pattern_splits[i].data(), pattern_splits[i].length())) {
			return false;
		}
	}
	return true;
}

vector<OpenFileInfo> HFFileSystem::Glob(const string &path, FileOpener *opener) {
	auto optional_opener = optional_ptr<FileOpener>(opener);
	auto target = HFResolveTarget(path, optional_opener, db);
	if (!HFUtil::HasGlob(target.url.path)) {
		return {path};
	}

	// list everything below the longest glob-free prefix, then filter the results
	auto first_wildcard = target.url.path.find_first_of("*?[");
	auto shared_path = target.url.path.substr(0, first_wildcard);
	auto last_slash = shared_path.find_last_of('/');
	if (last_slash == string::npos) {
		shared_path = "/";
	} else if (last_slash == 0) {
		shared_path = "/";
	} else {
		shared_path = shared_path.substr(0, last_slash);
	}

	auto pattern_splits = StringUtil::Split(target.url.path.substr(1), "/");
	bool has_double_star = false;
	for (auto &split : pattern_splits) {
		if (split == "**") {
			has_double_star = true;
		}
	}

	vector<string> files;
	if (has_double_star) {
		// `**` needs everything below the prefix anyway: one recursive listing beats walking every directory
		auto listing = target;
		listing.url.path = shared_path;
		HFListEntries(db, optional_opener, listing, true, [&](const HFListEntry &entry) {
			if (!entry.is_dir) {
				files.push_back(entry.path);
			}
		},
		              false, true);
	} else {
		vector<string> stack {shared_path};
		idx_t listed_dirs = 0;
		while (!stack.empty()) {
			auto current = stack.back();
			stack.pop_back();
			if (++listed_dirs > 200000) {
				throw IOException("Globbing '%s' required listing more than 200000 directories", path);
			}
			auto listing = target;
			listing.url.path = current;
			// the tree endpoint reports repo-root relative paths, so entries are already complete
			HFListEntries(db, optional_opener, listing, false, [&](const HFListEntry &entry) {
				if (entry.is_dir) {
					if (CanDescend(entry.path, pattern_splits)) {
						stack.push_back("/" + entry.path);
					}
				} else {
					files.push_back(entry.path);
				}
			},
			              false, true);
		}
	}

	vector<OpenFileInfo> result;
	for (auto &file : files) {
		auto file_splits = StringUtil::Split(file, "/");
		if (MatchPath(file_splits.begin(), file_splits.end(), pattern_splits.begin(), pattern_splits.end())) {
			result.push_back(OpenFileInfo(GetHFUrl(target.url, file)));
		}
	}
	return result;
}

} // namespace duckdb
