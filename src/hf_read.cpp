#include "hf_fs.hpp"

#include "hf_settings.hpp"

#include "duckdb/common/file_opener.hpp"

#include <condition_variable>
#include <unordered_map>

namespace duckdb {

//! Largest read-ahead window a single file will fetch at once
static constexpr idx_t HF_MAX_READ_AHEAD = 32 * 1024 * 1024;

static idx_t GetSizeSetting(optional_ptr<FileOpener> opener, const string &path, const string &name,
                            idx_t default_value) {
	if (opener) {
		Value value;
		FileOpenerInfo info;
		info.file_path = path;
		if (FileOpener::TryGetCurrentSetting(opener, Identifier(name), value, info) && !value.IsNull()) {
			auto configured = value.GetValue<idx_t>();
			if (configured > 0) {
				return configured;
			}
		}
	}
	return default_value;
}

//===----------------------------------------------------------------------===//
// HFFileCache
//===----------------------------------------------------------------------===//

namespace {
//! Live per-file caches, keyed by the url the bytes come from. Entries vanish with the last handle on the file.
struct HFReadCacheRegistry {
	std::mutex lock;
	std::unordered_map<string, weak_ptr<HFFileCache>> caches;
};

static HFReadCacheRegistry &CacheRegistry() {
	static HFReadCacheRegistry registry;
	return registry;
}
} // namespace

HFFileCache::HFFileCache(string key_p, idx_t buffer_size_p, idx_t cache_limit_p)
    : key(std::move(key_p)), buffer_size(buffer_size_p), cache_limit(cache_limit_p) {
	fetch_length = MinValue<idx_t>(buffer_size, cache_limit);
}

shared_ptr<HFFileCache> HFFileCache::Acquire(const string &key, idx_t buffer_size, idx_t cache_limit) {
	auto &registry = CacheRegistry();
	std::lock_guard<std::mutex> guard(registry.lock);
	if (registry.caches.size() > 512) {
		// opportunistic cleanup, the entries are weak so nothing leaks either way
		for (auto it = registry.caches.begin(); it != registry.caches.end();) {
			if (it->second.expired()) {
				it = registry.caches.erase(it);
			} else {
				++it;
			}
		}
	}
	auto entry = registry.caches.find(key);
	if (entry != registry.caches.end()) {
		if (auto existing = entry->second.lock()) {
			return existing;
		}
	}
	auto created = make_shared_ptr<HFFileCache>(key, buffer_size, cache_limit);
	registry.caches[key] = created;
	return created;
}

void HFFileCache::Release(const string &key) {
	auto &registry = CacheRegistry();
	std::lock_guard<std::mutex> guard(registry.lock);
	auto entry = registry.caches.find(key);
	if (entry != registry.caches.end() && entry->second.expired()) {
		registry.caches.erase(entry);
	}
}

idx_t HFFileCache::Size(HFClient &client, const string &url, const string &path) {
	std::lock_guard<std::mutex> guard(lock);
	return SizeLocked(client, url, path);
}

idx_t HFFileCache::SizeLocked(HFClient &client, const string &url, const string &path) {
	if (file_size.IsValid()) {
		return file_size.GetIndex();
	}
	auto response = client.Head(url);
	if (!response->HasRequestError() && static_cast<int>(response->status) < 400) {
		idx_t size = 0;
		if (response->HasHeader("x-linked-size")) {
			size = NumericCast<idx_t>(std::stoll(response->GetHeaderValue("x-linked-size")));
		} else if (response->HasHeader("Content-Length")) {
			size = NumericCast<idx_t>(std::stoll(response->GetHeaderValue("Content-Length")));
		} else {
			throw IOException("Could not determine the size of '%s' - the server returned no content length", path);
		}
		if (response->HasHeader("ETag")) {
			etag = response->GetHeaderValue("ETag");
		}
		if (response->HasHeader("x-linked-last-modified")) {
			try {
				last_modified = std::stoll(response->GetHeaderValue("x-linked-last-modified"));
			} catch (std::exception &) {
				last_modified = 0;
			}
		}
		file_size = size;
		return size;
	}
	if (static_cast<int>(response->status) == 404) {
		throw IOException("No file found at '%s' - the path, the revision or the repository name may be wrong", path);
	}
	client.ThrowHTTPError(*response, "HEAD", url);
}

void HFFileCache::InsertLocked(HFByteRange fresh) {
	for (auto it = ranges.begin(); it != ranges.end();) {
		if (it->start >= fresh.start && it->End() <= fresh.End()) {
			cached_bytes -= it->Bytes();
			it = ranges.erase(it);
		} else {
			++it;
		}
	}
	ranges.push_front(std::move(fresh));
	cached_bytes += ranges.front().Bytes();
	// the window fetched most recently survives, so this stops once only it is left
	while (cached_bytes > cache_limit && ranges.size() > 1) {
		cached_bytes -= ranges.back().Bytes();
		ranges.pop_back();
	}
}

void HFFileCache::Read(HFClient &client, const string &url, const string &path, data_ptr_t buffer, idx_t offset,
                       idx_t nr_bytes) {
	if (nr_bytes == 0) {
		return;
	}
	const auto read_end = offset + nr_bytes;
	while (true) {
		shared_ptr<HFPendingRange> waiting_for;
		shared_ptr<HFPendingRange> claimed;
		idx_t fetch_start = 0;
		idx_t fetch_len = 0;
		idx_t size = 0;
		{
			std::unique_lock<std::mutex> guard(lock);
			size = SizeLocked(client, url, path);
			if (offset > size || read_end > size) {
				throw IOException("Trying to read %llu bytes at offset %llu of '%s', which is only %llu bytes long",
				                  nr_bytes, offset, path, size);
			}
			// a range request costs the hub ~0.3s whatever its size, so anything fetched is served from here
			for (auto it = ranges.begin(); it != ranges.end(); ++it) {
				if (it->start <= offset && read_end <= it->End()) {
					if (it != ranges.begin()) {
						ranges.splice(ranges.begin(), ranges, it);
					}
					memcpy(buffer, const_data_ptr_cast(it->data.data()) + (offset - it->start), nr_bytes);
					return;
				}
			}
			// someone is already pulling a window that covers (or overlaps) the read: let that fetch finish
			// instead of asking the hub for the same bytes twice
			for (auto &entry : pending) {
				if (entry->start < read_end && offset < entry->end) {
					waiting_for = entry;
					break;
				}
			}
			if (!waiting_for) {
				// the read-ahead window doubles on every miss: reading a parquet footer costs one small request,
				// while a full scan converges on a handful of big ones. Windows are aligned so that the strided
				// prefetch reads of a parquet scan fall into a window that is cached or already in flight.
				fetch_length = MinValue<idx_t>(MaxValue<idx_t>(fetch_length * 2, buffer_size), HF_MAX_READ_AHEAD);
				fetch_start = offset / fetch_length * fetch_length;
				fetch_len = MinValue<idx_t>(fetch_length, size - fetch_start);
				if (fetch_start + fetch_len < read_end) {
					// the read sticks out of the window, fetch exactly what was asked for instead
					fetch_start = offset;
					fetch_len = MinValue<idx_t>(MaxValue<idx_t>(fetch_length, nr_bytes), size - offset);
				}
				claimed = make_shared_ptr<HFPendingRange>();
				claimed->start = fetch_start;
				claimed->end = fetch_start + fetch_len;
				pending.push_back(claimed);
			}
		}

		if (waiting_for) {
			std::unique_lock<std::mutex> guard(lock);
			cv.wait(guard, [&waiting_for] {
				return waiting_for->finished;
			});
			continue;
		}

		// download without the lock: threads reading other parts of the file do it in parallel, which is how
		// the hub's per-request latency gets hidden
		string fetched;
		fetched.reserve(fetch_len);
		bool ok = client.GetRange(url, fetch_start, fetch_len, fetched);
		bool too_small = fetched.size() < fetch_len || fetch_start + fetched.size() < read_end;
		string message;
		if (!ok) {
			message = StringUtil::Format("Failed to read %llu bytes at offset %llu of '%s'", fetch_len, fetch_start,
			                             path);
		} else if (too_small) {
			message = StringUtil::Format("Incomplete read of '%s': asked for %llu bytes at offset %llu, the server "
			                             "returned %llu",
			                             path, fetch_len, fetch_start, fetched.size());
		}

		std::unique_lock<std::mutex> guard(lock);
		for (auto it = pending.begin(); it != pending.end(); ++it) {
			if (*it == claimed) {
				(*it)->finished = true;
				pending.erase(it);
				break;
			}
		}
		cv.notify_all();
		if (!message.empty()) {
			throw IOException(message);
		}

		HFByteRange range;
		range.start = fetch_start;
		range.data = std::move(fetched);
		if (nr_bytes > cache_limit) {
			// a single read bigger than the whole cache is not worth keeping, it would only evict everything
			memcpy(buffer, const_data_ptr_cast(range.data.data()) + (offset - range.start), nr_bytes);
			return;
		}
		InsertLocked(std::move(range));
		auto &fresh = ranges.front();
		memcpy(buffer, const_data_ptr_cast(fresh.data.data()) + (offset - fresh.start), nr_bytes);
		return;
	}
}

//===----------------------------------------------------------------------===//
// HFReadHandle
//===----------------------------------------------------------------------===//

HFReadHandle::HFReadHandle(HFFileSystem &file_system, const string &path, FileOpenFlags flags, HFTarget target_p,
                           optional_ptr<FileOpener> opener)
    : FileHandle(file_system, path, flags), target(std::move(target_p)) {
	client = HFMakeClient(file_system.Db(), opener, target);
	resolve_url = target.url.ResolveUrl();
	cache_key = resolve_url;
	if (!target.url.revision.empty()) {
		cache_key += "@" + target.url.revision;
	}
	const auto buffer_size = GetSizeSetting(opener, path, "hf_read_buffer_size", HF_DEFAULT_READ_BUFFER);
	const auto cache_limit = GetSizeSetting(opener, path, "hf_read_cache_size", HF_DEFAULT_READ_CACHE);
	cache = HFFileCache::Acquire(cache_key, buffer_size, cache_limit);
}

idx_t HFReadHandle::Size() {
	return cache->Size(*client, resolve_url, path);
}

void HFReadHandle::ReadAt(data_ptr_t buffer, idx_t offset, idx_t nr_bytes) {
	cache->Read(*client, resolve_url, path, buffer, offset, nr_bytes);
}

void HFReadHandle::Close() {
	cache.reset();
	HFFileCache::Release(cache_key);
}

} // namespace duckdb
