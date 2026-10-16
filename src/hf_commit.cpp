#include <algorithm>
#include "hf_fs.hpp"

#include "hf_crypto.hpp"
#include "hf_settings.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

#include <atomic>
#include <map>
#include <mutex>

#include <unistd.h>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

//! Commits to the same repository are serialized within a process: the Hub commit API is a git commit, and
//! two concurrent commits on the same branch would otherwise conflict.
static std::mutex REPO_MUTEX_REGISTRY;
static std::map<string, std::weak_ptr<std::mutex>> REPO_MUTEXES;

static std::shared_ptr<std::mutex> RepoMutex(const string &key) {
	std::lock_guard<std::mutex> guard(REPO_MUTEX_REGISTRY);
	auto it = REPO_MUTEXES.find(key);
	if (it != REPO_MUTEXES.end()) {
		auto existing = it->second.lock();
		if (existing) {
			return existing;
		}
	}
	auto result = std::make_shared<std::mutex>();
	REPO_MUTEXES[key] = result;
	return result;
}

//! Point a hub-relative action url at the configured endpoint (the batch response always names the default one)
static string VerifyUrl(const string &endpoint, const string &href) {
	if (href.compare(0, strlen(HF_DEFAULT_ENDPOINT), HF_DEFAULT_ENDPOINT) != 0) {
		return href; // an s3/presigned url, leave it alone
	}
	return endpoint + href.substr(strlen(HF_DEFAULT_ENDPOINT));
}

//! `datasets` -> `dataset`, used by the repo creation endpoint
static string SingularRepoType(const string &repo_type) {
	return HFRepoType::ToSingular(repo_type);
}

//! The path inside the repository, without the leading slash the Hub does not want
static string PathInRepo(const string &path) {
	if (path.empty()) {
		return "";
	}
	return path.front() == '/' ? path.substr(1) : path;
}

//! The Hub switches a git-LFS blob to "basic multipart" once it is big enough: the batch answer then carries one
//! presigned url per part (numeric header keys), and `href` becomes the completion endpoint, which takes the
//! etags of every part. Parts are read from disk one at a time, so a big file never lands fully in memory.
static void UploadLFSParts(HFClient &client, const string &local_path, const HFBlobInfo &blob,
                           const string &completion_url, const vector<pair<int64_t, string>> &part_urls,
                           idx_t chunk_size) {
	auto &fs = FileSystem::GetLocal(client.Db());
	auto handle = fs.OpenFile(local_path, FileFlags::FILE_FLAGS_READ);
	auto file_size = MaxValue<int64_t>(0, fs.GetFileSize(*handle));
	auto expected_parts = NumericCast<idx_t>((NumericCast<idx_t>(file_size) + chunk_size - 1) / MaxValue<idx_t>(1, chunk_size));
	if (part_urls.size() != expected_parts) {
		throw IOException("The Hub asked for %zu multipart parts of '%s' but sent %zu upload urls - cannot upload",
		                  expected_parts, local_path, part_urls.size());
	}

	HFJsonWriter completion;
	completion.Raw("{\"oid\":").String(blob.sha256).Raw(",\"parts\":[");
	string buffer;
	buffer.resize(chunk_size);
	for (idx_t i = 0; i < part_urls.size(); i++) {
		auto part_number = part_urls[i].first;
		auto offset = NumericCast<idx_t>(part_number - 1) * chunk_size;
		auto len = MinValue<idx_t>(chunk_size, NumericCast<idx_t>(file_size) - offset);
		if (len > 0) {
			fs.Read(*handle, buffer.data(), NumericCast<int64_t>(len), NumericCast<int64_t>(offset));
		}
		// presigned part urls must not carry the hub token, s3 rejects a signed request with an extra Authorization
		auto put = client.Put(part_urls[i].second, const_data_ptr_cast(buffer.data()), len, HTTPHeaders(), false);
		if (put->HasRequestError() || static_cast<int>(put->status) >= 400) {
			client.ThrowHTTPError(*put, StringUtil::Format("PUT (LFS multipart part %lld)", part_number),
			                      part_urls[i].second);
		}
		string etag;
		for (const auto &header : put->headers) {
			if (StringUtil::CIEquals(header.first, "etag")) {
				etag = header.second;
			}
		}
		if (etag.empty()) {
			throw IOException("The Hub did not return an etag for part %lld of '%s' - cannot complete the "
			                  "multipart upload of %s",
			                  part_number, local_path, blob.sha256);
		}
		completion.Raw(i == 0 ? "{\"partNumber\":" : ",{\"partNumber\":").Raw(std::to_string(part_number));
		completion.Raw(",\"etag\":").String(etag).Raw("}");
	}
	completion.Raw("]}");

	HTTPHeaders headers;
	headers.Insert("Accept", "application/vnd.git-lfs+json");
	headers.Insert("Content-Type", "application/vnd.git-lfs+json");
	auto response = client.Post(completion_url, completion.out, headers);
	if (response->HasRequestError() || static_cast<int>(response->status) >= 400) {
		client.ThrowHTTPError(*response, "POST (LFS multipart completion)", completion_url);
	}
}

static bool is_number(const string &text) {
	return !text.empty() && text.find_first_not_of("0123456789") == string::npos;
}

static string ReadLocalFileToString(DatabaseInstance &db, const string &path) {
	auto &fs = FileSystem::GetLocal(db);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = fs.GetFileSize(*handle);
	string result;
	result.resize(NumericCast<idx_t>(MaxValue<int64_t>(0, size)));
	if (!result.empty()) {
		fs.Read(*handle, result.data(), NumericCast<int64_t>(result.size()), 0);
	}
	return result;
}

HFBlobInfo HFComputeBlobInfo(DatabaseInstance &db, const string &local_path) {
	auto &fs = FileSystem::GetLocal(db);
	auto handle = fs.OpenFile(local_path, FileFlags::FILE_FLAGS_READ);
	auto size = MaxValue<int64_t>(0, fs.GetFileSize(*handle));

	HFBlobInfo blob;
	blob.size = NumericCast<idx_t>(size);
	HFSha256 sha;
	static constexpr idx_t CHUNK = 1 * 1024 * 1024;
	string buffer;
	buffer.resize(MinValue<idx_t>(CHUNK, NumericCast<idx_t>(size)));
	idx_t remaining = NumericCast<idx_t>(size);
	idx_t offset = 0;
	while (remaining > 0) {
		auto to_read = MinValue<idx_t>(CHUNK, remaining);
		fs.Read(*handle, buffer.data(), NumericCast<int64_t>(to_read), offset);
		sha.Update(const_data_ptr_cast(buffer.data()), to_read);
		if (blob.sample.empty()) {
			// the Hub uses the first bytes to detect LFS candidates
			blob.sample = HFBase64::Encode(const_data_ptr_cast(buffer.data()), MinValue<idx_t>(to_read, 512));
		}
		offset += to_read;
		remaining -= to_read;
	}
	if (blob.sample.empty()) {
		blob.sample = "";
	}
	blob.sha256 = sha.FinalHex();
	return blob;
}

string HFMakeTempPathIn(DatabaseInstance &db, const string &preferred_dir, const string &extension) {
	string dir = preferred_dir;
	if (dir.empty()) {
		dir = DBConfig::GetConfig(db).options.temporary_directory;
	}
	if (dir.empty()) {
		const char *tmpdir = std::getenv("TMPDIR");
		dir = (tmpdir && *tmpdir) ? tmpdir : "/tmp";
	}
	while (dir.size() > 1 && dir.back() == '/') {
		dir.pop_back();
	}

	static std::atomic<uint64_t> counter {0};
	auto pid = static_cast<uint64_t>(getpid());
	auto name = StringUtil::Format("duckdb_hf_%llu_%llu.tmp%s", pid, counter++,
	                               extension.empty() ? "" : ("." + extension));
	auto path = HFUtil::Join(dir, name);
	if (dir != "/tmp") {
		FileSystem::GetLocal(db).CreateDirectory(dir, nullptr);
	}
	return path;
}

string HFMakeTempPath(DatabaseInstance &db, optional_ptr<FileOpener> opener, const string &extension) {
	string dir;
	if (opener) {
		Value cache_dir;
		FileOpenerInfo info;
		info.file_path = "hf://";
		if (FileOpener::TryGetCurrentSetting(opener, Identifier("hf_cache_dir"), cache_dir, info) &&
		    !cache_dir.IsNull()) {
			dir = StringValue::Get(cache_dir);
		}
	}
	return HFMakeTempPathIn(db, dir, extension);
}

//===----------------------------------------------------------------------===//
// Commit
//===----------------------------------------------------------------------===//

static string CommitNdjson(const vector<HFCommitOp> &operations, const string &commit_message) {
	HFJsonWriter out;
	// one json object per line: a header with the commit metadata, then one line per file operation
	out.Raw("{\"key\":\"header\",\"value\":{\"summary\":\"");
	out.Raw(HFJsonWriter::Escape(commit_message));
	out.Raw("\",\"description\":\"\"}");
	out.Raw("}\n");
	for (auto &op : operations) {
		switch (op.kind) {
		case HFCommitOp::Kind::WRITE:
			if (op.is_lfs) {
				out.Raw("{\"key\":\"lfsFile\",\"value\":{\"path\":");
				out.String(op.path);
				out.Raw(",\"algo\":\"sha256\",\"oid\":\"");
				out.Raw(HFJsonWriter::Escape(op.oid));
				out.Raw("\",\"size\":");
				out.Raw(std::to_string(op.size));
				out.Raw("}}\n");
			} else {
				out.Raw("{\"key\":\"file\",\"value\":{\"content\":\"");
				out.Raw(HFJsonWriter::Escape(op.content));
				out.Raw("\",\"path\":");
				out.String(op.path);
				out.Raw(",\"encoding\":\"");
				out.Raw(HFJsonWriter::Escape(op.encoding));
				out.Raw("\"}}\n");
			}
			break;
		case HFCommitOp::Kind::DELETE:
			out.Raw("{\"key\":\"deletedFile\",\"value\":{\"path\":");
			out.String(op.path);
			out.Raw("}}\n");
			break;
		case HFCommitOp::Kind::DELETE_FOLDER:
			out.Raw("{\"key\":\"deletedFolder\",\"value\":{\"path\":");
			out.String(op.path);
			out.Raw("}}\n");
			break;
		default:
			throw InternalException("Unsupported commit operation");
		}
	}
	return out.out;
}

bool HFCreateRepo(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target, bool private_repo) {
	const auto &url = target.url;
	if (url.IsBucket()) {
		throw IOException("'%s' is a bucket - create buckets with `hf buckets create <id>`: they are Xet-backed object "
		                  "storage, not git repos",
		                  url.ToString());
	}
	auto slash = url.repository.find('/');
	if (slash == string::npos || slash == 0 || slash + 1 == url.repository.size()) {
		throw IOException("'%s' has no '<namespace>/<name>' repository - expected for example "
		                  "'hf://datasets/my-user/my-dataset'",
		                  url.ToString());
	}
	// the create endpoint wants namespace and name apart: "name" is rejected when it still contains the slash
	HFJsonWriter body;
	body.Raw("{\"name\":").String(url.repository.substr(slash + 1)).Raw(",\"organization\":");
	body.String(url.repository.substr(0, slash));
	body.Raw(",\"visibility\":\"").Raw(private_repo ? "private" : "public");
	body.Raw("\",\"type\":\"").Raw(HFJsonWriter::Escape(SingularRepoType(url.repo_type))).Raw("\"}");

	HTTPHeaders headers;
	headers.Insert("Content-Type", "application/json");
	auto create_url = HFUtil::Join(url.endpoint, "api/repos/create");
	auto client = HFMakeClient(db, opener, target);
	for (idx_t attempt = 0; attempt < 4; attempt++) {
		auto response = client->Post(create_url, body.out, headers);
		auto status = static_cast<int>(response->status);
		if (!response->HasRequestError() && (status == 200 || status == 201)) {
			return true;
		}
		if (status == 409) {
			// the Hub answers 409 both for "already created" and for "another operation is in progress"
			if (response->body.find("conflicting operation") != string::npos) {
				HFClient::SleepMs(250);
				continue;
			}
			return false;
		}
		client->ThrowHTTPError(*response, "POST (create repository)", create_url);
	}
	throw IOException("Could not create '%s' - the Hub kept answering 409 (a conflicting operation is in progress)",
	                  url.ToString());
}

//! Buckets have their own batch endpoint: one ndjson line per operation. Only deletions are usable from DuckDB,
//! `addFile` requires a `xetHash` produced by the Xet client.
static void BucketBatchDelete(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                              const vector<HFCommitOp> &operations) {
	auto client = HFMakeClient(db, opener, target);
	auto url = HFUtil::Join(target.url.endpoint,
	                        HFUtil::Join(HFUtil::Join("api", target.url.repo_type), target.url.repository) + "/batch");
	HFJsonWriter out;
	for (auto &op : operations) {
		out.Raw("{\"type\":\"deleteFile\",\"path\":");
		out.String(op.path);
		out.Raw("}\n");
	}
	HTTPHeaders headers;
	headers.Insert("Content-Type", "application/x-ndjson");
	auto response = client->Post(url, out.out, headers);
	// partial failures arrive in the body of a 200, total failure in a 422
	if (static_cast<int>(response->status) != 422 && !response->HasRequestError() &&
	    static_cast<int>(response->status) < 400) {
		auto failed = HFJson::FindArray(response->body, "failed");
		// an empty `"failed":[]` is the success answer, only actual entries are problems
		if (failed.find('{') != string::npos || failed.find('"') != string::npos) {
			throw IOException("The Hugging Face Hub refused some deletion in '%s': %s", target.url.ToString(),
			                  failed.size() > 512 ? failed.substr(0, 512) + "..." : failed);
		}
		return;
	}
	client->ThrowHTTPError(*response, "POST (bucket batch)", url);
}

//! A 404 (or a 403 for a private repo you can create) on a write path is the Hub's way of saying "this repository
//! does not exist yet". `hf_create_repo` turns that into a create + retry, so a first `COPY TO` can bootstrap a
//! repository. Both the commit endpoint and the preupload endpoint hit it - preupload comes first, so a fresh
//! repository fails there before a commit is ever attempted.
static bool HFCreateRepoIfAllowed(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                                  int status) {
	if (status != 404 && status != 403) {
		return false;
	}
	Value create_repo;
	if (!opener) {
		return false;
	}
	FileOpenerInfo info;
	info.file_path = target.url.ToString();
	if (!FileOpener::TryGetCurrentSetting(opener, Identifier("hf_create_repo"), create_repo, info) ||
	    create_repo.IsNull() || !create_repo.GetValue<bool>()) {
		return false;
	}
	// true even when the repository already existed (409): in both cases the write is worth retrying
	HFCreateRepo(db, opener, target, true);
	return true;
}

void HFCommit(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &repo_target,
              const vector<HFCommitOp> &operations, const string &commit_message) {
	if (operations.empty()) {
		return;
	}
	if (repo_target.url.IsBucket()) {
		// adding files to a bucket goes through Xet chunked storage, which needs the hf_xet library - deleting does
		// not, it is a plain call to the bucket batch endpoint
		for (auto &op : operations) {
			if (op.kind != HFCommitOp::Kind::DELETE) {
				throw IOException("Writing to Hugging Face buckets ('%s') is not supported: bucket uploads go through "
				                    "Xet storage, which DuckDB cannot speak yet (only the `hf_xet` client can). Bucket "
				                    "files can be read, copied and deleted from DuckDB: write the data to a dataset or "
				                    "model repository with `COPY TO`, then move it into the bucket with "
				                    "`SELECT * FROM hf_bucket_copy('hf://datasets/user/repo/file', '%s')`",
				                    repo_target.url.ToString(), repo_target.url.ToString());
			}
		}
		BucketBatchDelete(db, opener, repo_target, operations);
		return;
	}
	auto key = repo_target.url.endpoint + "|" + repo_target.url.repo_type + "|" + repo_target.url.repository + "|" +
	           repo_target.url.revision;
	std::lock_guard<std::mutex> guard(*RepoMutex(key));

	auto client = HFMakeClient(db, opener, repo_target);
	auto url = HFUtil::Join(repo_target.url.endpoint,
	                        HFUtil::Join(HFUtil::Join(HFUtil::Join("api", repo_target.url.repo_type),
	                                                  repo_target.url.repository),
	                                     "commit/" + repo_target.url.revision));
	HTTPHeaders headers;
	headers.Insert("Content-Type", "application/x-ndjson");

	auto ndjson = CommitNdjson(operations, commit_message);
	bool created_repo = false;
	for (idx_t attempt = 0; attempt < 4; attempt++) {
		auto response = client->Post(url, ndjson, headers);
		auto status = static_cast<int>(response->status);
		if (!response->HasRequestError() && (status == 200 || status == 201)) {
			bool has_pr = false;
			auto pr = HFJson::GetString(response->body, "pullRequestUrl", has_pr);
			if (has_pr && !pr.empty()) {
				// a write to a protected revision created a PR instead of committing - make it visible
				// (the data is not in the branch yet, so this is not something to silently accept)
				throw IOException(
				    "Writing to '%s' created a pull request instead of committing: %s - the repository is likely "
				    "protected, commit the PR to see the data",
				    repo_target.url.ToString(), pr);
			}
			// the repository has new bytes at these paths now: any window cached for it is stale
			HFReadCacheInvalidateRepo(HFRepoCacheKey(repo_target.url));
			return;
		}
		if ((status == 404 || status == 403) && !created_repo) {
			// the repository does not exist yet: `hf_create_repo` turns the write into create + retry
			if (HFCreateRepoIfAllowed(db, opener, repo_target, status)) {
				created_repo = true;
				continue;
			}
		}
		if (status == 409 || status == 412 || HFClient::ShouldRetry(*response)) {
			// concurrent write or a transient failure: the payload is idempotent, just try again
			continue;
		}
		client->ThrowHTTPError(*response, "POST (commit)", url);
	}
	throw IOException("Failed to commit to '%s' after several attempts - the repository is likely being written to "
	                  "concurrently",
	                  repo_target.url.ToString());
}

//===----------------------------------------------------------------------===//
// Uploads
//===----------------------------------------------------------------------===//

//! the answer is `{"files":[{"path":...,"uploadMode":...,"shouldIgnore":...,"oid":...}]}`
static string ParsePreuploadMode(HFClient &client, const HTTPResponse &response, const HFUrl &url,
                                 const string &path_in_repo, bool &should_ignore) {
	string mode;
	auto files = HFJson::FindArray(response.body, "files");
	HFJson::ForEachObject(files, [&](const string &object) {
		string entry_path;
		string entry_mode;
		bool entry_ignore = false;
		HFJson::ForEachMember(object, [&](const string &key, const string &raw) {
			if (key == "path") {
				entry_path = HFJson::AsString(raw);
			} else if (key == "uploadMode") {
				entry_mode = HFJson::AsString(raw);
			} else if (key == "shouldIgnore") {
				entry_ignore = HFJson::AsBool(raw);
			}
		});
		if (!entry_path.empty() && entry_path != path_in_repo) {
			return; // the hub may answer with more entries than asked for
		}
		mode = entry_mode;
		if (entry_ignore) {
			should_ignore = true;
		}
	});
	if (mode.empty()) {
		throw IOException("The Hugging Face Hub returned no upload mode for '%s'", url.ToString());
	}
	return mode;
}

static string PreuploadMode(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                            const string &path_in_repo, const HFBlobInfo &blob, bool &should_ignore) {
	const auto &url = target.url;
	HFJsonWriter body;
	body.Raw("{\"files\":[{\"path\":");
	body.String(path_in_repo);
	body.Raw(",\"sample\":\"");
	body.Raw(HFJsonWriter::Escape(blob.sample));
	body.Raw("\",\"size\":");
	body.Raw(std::to_string(blob.size));
	body.Raw("}]}");

	HTTPHeaders headers;
	headers.Insert("Content-Type", "application/json");
	auto request_url = HFUtil::Join(url.endpoint, HFUtil::Join(HFUtil::Join(HFUtil::Join("api", url.repo_type),
	                                                                        url.repository),
	                                                           "preupload/" + url.revision));
	// a first write to a repository hits this endpoint before the commit does, so this is where a missing
	// repository has to be created - otherwise `hf_create_repo` would only ever report a 404
	string body_str = body.out;
	auto client = HFMakeClient(db, opener, target);
	for (idx_t attempt = 0;; attempt++) {
		auto response = client->Post(request_url, body_str, headers);
		auto status = static_cast<int>(response->status);
		if (!response->HasRequestError() && status < 400) {
			return ParsePreuploadMode(*client, *response, url, path_in_repo, should_ignore);
		}
		if (attempt == 0 && HFCreateRepoIfAllowed(db, opener, target, status)) {
			continue;
		}
		client->ThrowHTTPError(*response, "POST (preupload)", request_url);
	}
}

static void UploadLFSBlob(HFClient &client, const HFUrl &url, const HFBlobInfo &blob, const string &local_path) {
	if (blob.size > HF_MAX_SINGLE_UPLOAD_SIZE) {
		throw IOException(
		    "'%s' is %llu bytes, which is above the %llu byte limit of single-request uploads - split the output "
		    "(for example with PARTITION_BY) to upload it",
		    local_path, blob.size, HF_MAX_SINGLE_UPLOAD_SIZE);
	}

	HFJsonWriter body;
	body.Raw("{\"operation\":\"upload\",\"transfers\":[\"basic\",\"multipart\"],\"objects\":[{\"oid\":\"");
	body.Raw(HFJsonWriter::Escape(blob.sha256));
	body.Raw("\",\"size\":");
	body.Raw(std::to_string(blob.size));
	body.Raw("}],\"hash_algo\":\"sha256\",\"ref\":{\"name\":");
	body.String(url.revision);
	body.Raw("}}");

	HTTPHeaders headers;
	headers.Insert("Accept", "application/vnd.git-lfs+json");
	headers.Insert("Content-Type", "application/vnd.git-lfs+json");
	auto batch_url = HFUtil::Join(url.endpoint, HFRepoType::ToGitPrefix(url.repo_type) + url.repository +
	                                               ".git/info/lfs/objects/batch");
	auto response = client.Post(batch_url, body.out, headers);
	if (response->HasRequestError() || static_cast<int>(response->status) >= 400) {
		client.ThrowHTTPError(*response, "POST (LFS batch)", batch_url);
	}

	// the batch response tells us where to PUT the blob; a missing action means the blob is already stored
	string upload_href;
	string verify_href;
	HTTPHeaders upload_headers;
	vector<pair<int64_t, string>> upload_part_urls;
	int64_t chunk_size = 0;
	auto objects = HFJson::FindArray(response->body, "objects");
	HFJson::ForEachObject(objects, [&](const string &object) {
		bool matches = false;
		HFJson::ForEachMember(object, [&](const string &key, const string &raw) {
			if (key == "oid" && HFJson::AsString(raw) == blob.sha256) {
				matches = true;
			}
		});
		if (!matches) {
			return; // the hub may answer with more objects than we asked for
		}
		HFJson::ForEachMember(object, [&](const string &key, const string &raw) {
			if (key != "actions") {
				return;
			}
			HFJson::ForEachMember(raw, [&](const string &action_key, const string &action_raw) {
				const bool is_upload = action_key == "upload";
				if (!is_upload && action_key != "verify") {
					return;
				}
				string href;
				HTTPHeaders action_headers;
				vector<pair<int64_t, string>> part_urls;
				HFJson::ForEachMember(action_raw, [&](const string &field, const string &field_raw) {
					if (field == "href") {
						href = HFJson::AsString(field_raw);
					} else if (field == "header") {
						HFJson::ForEachMember(field_raw, [&](const string &header_key, const string &header_raw) {
							if (header_key == "chunk_size") {
								// the Hub quotes it ("chunk_size":"5242880"), so parse the unquoted text
								chunk_size = std::stoll(HFJson::AsString(header_raw));
							} else if (is_number(header_key)) {
								// multipart: one presigned url per part number, the href becomes the completion url
								part_urls.emplace_back(std::stoll(header_key), HFJson::AsString(header_raw));
							} else {
								action_headers.Insert(header_key, HFJson::AsString(header_raw));
							}
						});
					}
				});
				if (is_upload) {
					upload_href = href;
					upload_headers = action_headers;
					std::sort(part_urls.begin(), part_urls.end());
					upload_part_urls = std::move(part_urls);
				} else {
					verify_href = href;
				}
			});
		});
	});
	if (upload_href.empty()) {
		// the blob is already in storage
		return;
	}
	if (chunk_size > 0) {
		UploadLFSParts(client, local_path, blob, upload_href, upload_part_urls, NumericCast<idx_t>(chunk_size));
		return;
	}

	auto content = ReadLocalFileToString(client.Db(), local_path);
	// the presigned url carries its own credentials - sending the hub token along breaks the s3 signature
	auto put = client.Put(upload_href, const_data_ptr_cast(content.data()), content.size(), upload_headers, false);
	if (put->HasRequestError() || static_cast<int>(put->status) >= 400) {
		client.ThrowHTTPError(*put, "PUT (LFS blob)", upload_href);
	}
	if (verify_href.empty()) {
		return;
	}
	// the hub asks for a verification round-trip after the blob landed
	HFJsonWriter verify_body;
	verify_body.Raw("{\"oid\":\"");
	verify_body.Raw(HFJsonWriter::Escape(blob.sha256));
	verify_body.Raw("\",\"size\":");
	verify_body.Raw(std::to_string(blob.size));
	verify_body.Raw("}");
	HTTPHeaders verify_headers;
	verify_headers.Insert("Content-Type", "application/json");
	auto verify = client.Post(VerifyUrl(client.Connection().endpoint, verify_href), verify_body.out, verify_headers);
	if (verify->HasRequestError() || static_cast<int>(verify->status) >= 400) {
		client.ThrowHTTPError(*verify, "POST (LFS verify)", verify_href);
	}
}

void HFUploadLocalFile(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                       const string &local_path, const HFBlobInfo &blob, const string &commit_message) {
	HFRefuseBucketWrite(target.url, target.url.ToString());
	auto path_in_repo = PathInRepo(target.url.path);
	if (path_in_repo.empty()) {
		throw IOException("'%s' does not contain a file name - write to a path ending in a file name, for example "
		                  "'hf://datasets/user/repo/data.parquet'",
		                  target.url.ToString());
	}

	auto client = HFMakeClient(db, opener, target);
	bool should_ignore = false;
	auto mode = PreuploadMode(db, opener, target, path_in_repo, blob, should_ignore);
	if (should_ignore) {
		throw IOException("'%s' is ignored by the .gitignore file of the repository - refusing to upload it",
		                  target.url.ToString());
	}

	HFCommitOp op;
	op.path = path_in_repo;
	op.size = blob.size;
	if (mode == "regular" || mode == "normal") {
		auto content = ReadLocalFileToString(db, local_path);
		op.content = HFBase64::Encode(const_data_ptr_cast(content.data()), content.size());
		op.encoding = "base64";
		op.is_lfs = false;
	} else if (mode == "lfs") {
		UploadLFSBlob(*client, target.url, blob, local_path);
		op.is_lfs = true;
		op.oid = blob.sha256;
	} else {
		throw IOException(
		    "The Hugging Face Hub asked DuckDB to upload '%s' using the unsupported '%s' upload method - install or "
		    "use a repository without Xet storage, or write the data with huggingface_hub",
		    target.url.ToString(), mode);
	}

	HFCommit(db, opener, target, {op}, commit_message);
}

void HFDeletePaths(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &target,
                   const vector<string> &files, const vector<string> &folders, const string &commit_message) {
	vector<HFCommitOp> operations;
	for (auto &file : files) {
		HFCommitOp op;
		op.kind = HFCommitOp::Kind::DELETE;
		op.path = PathInRepo(file);
		if (op.path.empty()) {
			continue;
		}
		operations.push_back(std::move(op));
	}
	for (auto &folder : folders) {
		HFCommitOp op;
		op.kind = HFCommitOp::Kind::DELETE_FOLDER;
		op.path = PathInRepo(folder);
		if (op.path.empty()) {
			continue;
		}
		operations.push_back(std::move(op));
	}
	HFCommit(db, opener, target, operations, commit_message);
}

//===----------------------------------------------------------------------===//
// Server side copies (the way to write a bucket without a Xet client)
//===----------------------------------------------------------------------===//

vector<HFXetFile> HFXetHashes(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &source,
                              const vector<string> &paths) {
	vector<HFXetFile> out;
	if (paths.empty()) {
		return out;
	}
	const auto &url = source.url;
	auto client = HFMakeClient(db, opener, source);

	HFJsonWriter body;
	string request_url;
	if (url.IsBucket()) {
		// the bucket flavour takes the paths and nothing else
		request_url = HFUtil::Join(url.endpoint, HFUtil::Join(HFUtil::Join("api", url.repo_type), url.repository) +
		                                        "/paths-info");
		body.Raw("{\"paths\":[");
	} else {
		request_url = HFUtil::Join(url.endpoint, HFUtil::Join(HFUtil::Join(HFUtil::Join("api", url.repo_type),
		                                                                        url.repository),
		                                                           "paths-info/" + url.revision));
		// `xet` is what makes the hub answer with the content hash a bucket can be pointed at
		body.Raw("{\"paths\":[");
	}
	for (idx_t i = 0; i < paths.size(); i++) {
		if (i > 0) {
			body.Raw(",");
		}
		body.String(paths[i]);
	}
	body.Raw("]");
	if (!url.IsBucket()) {
		body.Raw(",\"blobs\":true,\"lfs\":true,\"xet\":true");
	}
	body.Raw("}");

	HTTPHeaders headers;
	headers.Insert("Content-Type", "application/json");
	auto response = client->Post(request_url, body.out, headers);
	if (response->HasRequestError() || static_cast<int>(response->status) >= 400) {
		client->ThrowHTTPError(*response, "POST (paths-info)", request_url);
	}
	// the answer is a top level array, the scanner looks arrays up under a key
	auto wrapped = "{\"files\":" + response->body + "}";
	auto entries = HFJson::FindArray(wrapped, "files");
	HFJson::ForEachObject(entries, [&](const string &object) {
		HFXetFile file;
		bool is_dir = false;
		HFJson::ForEachMember(object, [&](const string &key, const string &raw) {
			if (key == "path") {
				file.path = HFJson::AsString(raw);
			} else if (key == "xetHash") {
				file.xet_hash = HFJson::AsString(raw);
			} else if (key == "size") {
				file.size = NumericCast<idx_t>(MaxValue<int64_t>(0, HFJson::AsInt(raw)));
			} else if (key == "type") {
				is_dir = HFJson::AsString(raw) != "file";
			}
		});
		if (!file.path.empty() && !is_dir) {
			out.push_back(std::move(file));
		}
	});
	return out;
}

void HFCopyIntoBucket(DatabaseInstance &db, optional_ptr<FileOpener> opener, const HFTarget &bucket,
                      const vector<HFBucketCopyOp> &operations) {
	if (operations.empty()) {
		return;
	}
	if (!bucket.url.IsBucket()) {
		throw IOException("'%s' is not a bucket - copies land in a bucket, for example "
		                  "'hf://buckets/my-user/my-bucket/data/'",
		                  bucket.url.ToString());
	}
	auto client = HFMakeClient(db, opener, bucket);
	auto url = HFUtil::Join(bucket.url.endpoint,
	                        HFUtil::Join(HFUtil::Join("api", bucket.url.repo_type), bucket.url.repository) + "/batch");
	HTTPHeaders headers;
	headers.Insert("Content-Type", "application/x-ndjson");

	const auto now_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
	        .count();
	// one batch of the ndjson endpoint is a round trip, and the Hub is fine with a hundred files in it
	constexpr idx_t BATCH_SIZE = 100;
	for (idx_t from = 0; from < operations.size(); from += BATCH_SIZE) {
		HFJsonWriter out;
		for (idx_t i = from; i < MinValue<idx_t>(from + BATCH_SIZE, operations.size()); i++) {
			auto &op = operations[i];
			out.Raw("{\"type\":\"copyFile\",\"path\":");
			out.String(op.path);
			out.Raw(",\"xetHash\":");
			out.String(op.xet_hash);
			out.Raw(",\"mtime\":");
			out.Raw(std::to_string(now_ms));
			out.Raw(",\"sourceRepoType\":");
			out.String(op.source_repo_type);
			out.Raw(",\"sourceRepoId\":");
			out.String(op.source_repo_id);
			out.Raw("}\n");
		}
		auto response = client->Post(url, out.out, headers);
		if (!(response->HasRequestError() || static_cast<int>(response->status) >= 400)) {
			auto failed = HFJson::FindArray(response->body, "failed");
			if (failed.find('{') != string::npos || failed.find('"') != string::npos) {
				throw IOException("The Hugging Face Hub refused some copies into '%s': %s", bucket.url.ToString(),
				                    failed.size() > 512 ? failed.substr(0, 512) + "..." : failed);
			}
			continue;
		}
		client->ThrowHTTPError(*response, "POST (bucket batch)", url);
	}
	// the bucket has new bytes at these paths: anything cached for it is stale
	HFReadCacheInvalidateRepo(HFRepoCacheKey(bucket.url));
}

} // namespace duckdb
