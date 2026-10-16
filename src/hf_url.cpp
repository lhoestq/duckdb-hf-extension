#include "hf_common.hpp"


namespace duckdb {

bool HFRepoType::IsValid(const string &repo_type) {
	return repo_type == DATASETS || repo_type == MODELS || repo_type == SPACES || repo_type == KERNELS ||
	       repo_type == BUCKETS;
}

string HFRepoType::ToApiSegment(const string &repo_type) {
	return repo_type;
}

string HFRepoType::ToSingular(const string &repo_type) {
	if (repo_type.size() > 1 && repo_type.back() == 's') {
		return repo_type.substr(0, repo_type.size() - 1);
	}
	return repo_type;
}

string HFRepoType::ToGitPrefix(const string &repo_type) {
	// model repos are served straight off the domain root - asking for /models/.../resolve/... 404s
	if (repo_type == MODELS) {
		return "";
	}
	return repo_type + "/";
}

void HFUrl::ThrowParseError(const string &url, const string &reason) {
	if (reason.empty()) {
		throw IOException(
		    "Failed to parse '%s'. Please format urls like: "
		    "'hf://datasets/my-user/my-dataset/path/to/file.parquet' (also supported: models, spaces, "
		    "kernels, buckets and an '@revision' after the repository name)",
		    url);
	}
	throw IOException("Failed to parse '%s': %s", url, reason);
}

string HFUtil::Join(const string &base, const string &part) {
	if (base.empty()) {
		return part;
	}
	if (part.empty()) {
		return base;
	}
	auto ends_with_slash = base.back() == '/';
	auto starts_with_slash = part.front() == '/';
	if (ends_with_slash && starts_with_slash) {
		return base + part.substr(1);
	}
	if (!ends_with_slash && !starts_with_slash) {
		return base + "/" + part;
	}
	return base + part;
}

string HFUtil::Parent(const string &path) {
	auto slash = path.find_last_of('/');
	if (slash == string::npos) {
		return "";
	}
	if (slash == 0) {
		return "/";
	}
	return path.substr(0, slash);
}

string HFUtil::BaseName(const string &path) {
	auto slash = path.find_last_of('/');
	if (slash == string::npos) {
		return path;
	}
	return path.substr(slash + 1);
}

bool HFUtil::HasGlob(const string &path) {
	return path.find_first_of("*?[") != string::npos;
}

static bool ShouldEncode(unsigned char c) {
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
		return false;
	}
	switch (c) {
	case '-':
	case '.':
	case '_':
	case '~':
	case '/':
		return false;
	default:
		return true;
	}
}

string HFUrl::EncodePath(const string &path) {
	string result;
	for (auto c : path) {
		auto uc = static_cast<unsigned char>(c);
		if (!ShouldEncode(uc)) {
			result += c;
			continue;
		}
		static const char *HEX = "0123456789ABCDEF";
		result += '%';
		result += HEX[uc >> 4];
		result += HEX[uc & 0x0F];
	}
	return result;
}

HFUrl HFUrl::WithPath(const string &new_path) const {
	HFUrl result = *this;
	result.path = new_path;
	return result;
}

string HFUrl::ToString() const {
	string result = "hf://" + repo_type + "/" + repository;
	if (!IsBucket() && revision != "main") {
		result += "@" + revision;
	}
	return result + path;
}

string HFUrl::ResolveUrl() const {
	// buckets are not versioned but still go through a `/resolve/` route: `{endpoint}/buckets/{id}/resolve/{path}`
	auto url = endpoint + "/" + HFRepoType::ToGitPrefix(repo_type) + repository;
	if (!IsBucket()) {
		url = HFUtil::Join(url, "resolve");
		url = HFUtil::Join(url, revision);
		return url + EncodePath(path);
	}
	url = HFUtil::Join(url, "resolve");
	return url + EncodePath(path);
}

string HFUrl::InfoUrl() const {
	auto url = HFUtil::Join(endpoint, "api");
	url = HFUtil::Join(url, HFRepoType::ToApiSegment(repo_type));
	return HFUtil::Join(url, repository);
}

string HFUrl::RefsUrl() const {
	return HFUtil::Join(InfoUrl(), "refs");
}

string HFUrl::CommitsUrl(idx_t limit) const {
	auto url = HFUtil::Join(InfoUrl(), "commits");
	url = HFUtil::Join(url, EncodePath(revision).empty() ? string("main") : EncodePath(revision));
	if (limit > 0) {
		url += "?limit=" + to_string(limit);
	}
	return url;
}

string HFUrl::TreeUrl(idx_t limit, bool recursive) const {
	auto url = HFUtil::Join(endpoint, "api");
	url = HFUtil::Join(url, HFRepoType::ToApiSegment(repo_type));
	url = HFUtil::Join(url, repository);
	url = HFUtil::Join(url, "tree");
	if (!IsBucket()) {
		url = HFUtil::Join(url, revision);
	}
	auto query = EncodePath(path);
	// the tree endpoint takes the path appended to the revision; the repo root is `/tree/{revision}` (no slash)
	if (query == "/") {
		query = "";
	} else if (!query.empty() && query[0] != '/') {
		query = "/" + query;
	}
	url += query;
	vector<string> params;
	// the tree endpoint is flat by default for repos, and recursive by default for buckets: always be explicit
	params.push_back(StringUtil::Format("recursive=%s", recursive ? "true" : "false"));
	if (limit > 0) {
		params.push_back(StringUtil::Format("limit=%llu", limit));
	}
	for (idx_t i = 0; i < params.size(); i++) {
		url += (i == 0) ? "?" : "&";
		url += params[i];
	}
	return url;
}

HFUrl HFUrl::Parse(const string &url, const string &endpoint) {
	HFUrl result;
	result.endpoint = endpoint;

	if (!StringUtil::StartsWith(url, "hf://")) {
		throw InternalException("Not an hf url: %s", url);
	}
	auto body = url.substr(5);
	if (body.empty() || body == "/") {
		ThrowParseError(url);
	}

	// strip a trailing slash so that `hf://datasets/user/dset/` parses like `hf://datasets/user/dset`
	while (body.size() > 1 && body.back() == '/') {
		body.pop_back();
	}

	// Repository type (optional in the short form)
	auto first_slash = body.find('/');
	auto first = first_slash == string::npos ? body : body.substr(0, first_slash);
	string repo_type;
	bool inferred = false;
	auto at = first.find('@');
	auto first_name = at == string::npos ? first : first.substr(0, at);
	if (HFRepoType::IsValid(first_name)) {
		repo_type = first_name;
	} else {
		// short form: hf://user/dataset/... reads like s3://bucket/..., assume datasets
		repo_type = HFRepoType::DATASETS;
		inferred = true;
	}
	result.repo_type = repo_type;
	result.inferred_repo_type = inferred;

	auto remainder = first_slash == string::npos ? string() : body.substr(first_slash + 1);
	if (!inferred) {
		if (remainder.empty()) {
			ThrowParseError(url, "the repository name is missing");
		}
	} else {
		// the first element is part of the repository name
		remainder = body;
	}

	// repository + optional @revision: for datasets/models/spaces/kernels the repository is `namespace/name`
	idx_t repo_len = 0;
	if (result.IsBucket()) {
		// buckets are `hf://buckets/<user>/<bucket>[/<path>]` and have no revisions
		auto first_part = remainder.find('/');
		if (first_part == string::npos) {
			ThrowParseError(url, "expected 'hf://buckets/<user>/<bucket>[/<path>]' - a bucket needs a namespace and a "
			                    "bucket name, like 'hf://buckets/lhoestq/my-bucket/data.parquet'");
		}
		auto second_part = remainder.find('/', first_part + 1);
		repo_len = second_part == string::npos ? remainder.size() : second_part;
	} else {
		// two path segments (namespace/name), optionally followed by @revision
		auto slash_a = remainder.find('/');
		if (slash_a == string::npos) {
			ThrowParseError(url, inferred ? "expected 'hf://<user>/<repo>/<path>'"
			                              : "expected 'hf://<repo_type>/<namespace>/<name>/<path>'");
		}
		auto slash_b = remainder.find('/', slash_a + 1);
		auto end_of_repo = slash_b == string::npos ? remainder.size() : slash_b;
		auto repo_at = remainder.find('@', slash_a + 1);
		if (repo_at != string::npos && repo_at < end_of_repo) {
			repo_len = repo_at;
			auto revision_end = slash_b == string::npos ? remainder.size() : slash_b;
			result.revision = remainder.substr(repo_at + 1, revision_end - repo_at - 1);
			if (result.revision.empty()) {
				ThrowParseError(url, "empty revision");
			}
			if (slash_b == string::npos) {
				slash_b = remainder.size();
			}
		} else {
			repo_len = end_of_repo;
			if (slash_b == string::npos) {
				slash_b = remainder.size();
			}
		}
	}
	result.repository = remainder.substr(0, repo_len);
	if (result.repository.empty()) {
		ThrowParseError(url, "the repository name is missing");
	}
	if (result.IsBucket()) {
		result.revision.clear();
	}

	auto path_start = remainder.find('/', repo_len);
	result.path = path_start == string::npos ? "/" : remainder.substr(path_start);
	if (result.path.empty()) {
		result.path = "/";
	}
	return result;
}

string HFRepoCacheKey(const HFUrl &url) {
	// identifies everything a commit to this repository can change, so a commit can invalidate the read
	// caches of exactly that repository
	return url.endpoint + "|" + url.repo_type + "|" + url.repository;
}

} // namespace duckdb
