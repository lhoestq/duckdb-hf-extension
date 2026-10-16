#include "hf_fs.hpp"

#include "hf_settings.hpp"

#include "duckdb/common/file_opener.hpp"

#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <vector>

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
//! The fetched windows, kept around beyond the lifetime of the handle that fetched them.
//!
//! This is deliberately *not* a weak registry. A parquet scan opens a file, reads the footer, closes the
//! handle and opens it again for the row groups, and the next query reading the same file starts from
//! scratch too. When entries died with their last handle, each of those opens downloaded the file all over
//! again - one `count(*)` over a 287MB file pulled 1.8x its own size off the CDN. Entries are owned here
//! instead, and evicted least recently used first once more than `hf_read_cache_total_size` bytes are
//! held, skipping files that a handle has open right now.
struct HFReadCacheRegistry {
	struct Entry {
		shared_ptr<HFFileCache> cache;
		string key;
		//! the repository the bytes come from, so a commit can drop exactly the entries it invalidated
		string repo_key;
		uint64_t last_used = 0;
	};

	std::mutex lock;
	std::unordered_map<string, Entry> caches;
	uint64_t clock = 0;

	//! Drop idle entries until the held bytes fit in `total_budget` - callers must hold `lock`
	void Evict(idx_t total_budget) {
		while (true) {
			idx_t total = 0;
			Entry *oldest = nullptr;
			for (auto &item : caches) {
				total += item.second.cache->CachedBytes();
				if (item.second.cache.use_count() > 1) {
					continue; // in use: dropping the registry reference would not free the bytes anyway
				}
				if (!oldest || item.second.last_used < oldest->last_used) {
					oldest = &item.second;
				}
			}
			if (total <= total_budget || !oldest) {
				return;
			}
			caches.erase(oldest->key);
			HFReadCountersGet().evictions++;
		}
	}
};

static HFReadCacheRegistry &CacheRegistry() {
	static HFReadCacheRegistry registry;
	return registry;
}
} // namespace

idx_t HFReadCacheRetainedBytes() {
	auto &registry = CacheRegistry();
	std::lock_guard<std::mutex> guard(registry.lock);
	idx_t total = 0;
	for (auto &item : registry.caches) {
		total += item.second.cache->CachedBytes();
	}
	return total;
}

idx_t HFReadCacheRetainedFiles() {
	auto &registry = CacheRegistry();
	std::lock_guard<std::mutex> guard(registry.lock);
	return registry.caches.size();
}

void HFReadCacheClear() {
	auto &registry = CacheRegistry();
	std::lock_guard<std::mutex> guard(registry.lock);
	for (auto &item : registry.caches) {
		item.second.cache->DropAllWindows();
	}
	registry.caches.clear();
}

void HFReadCacheInvalidateRepo(const string &repo_key) {
	auto &registry = CacheRegistry();
	std::lock_guard<std::mutex> guard(registry.lock);
	for (auto it = registry.caches.begin(); it != registry.caches.end();) {
		if (it->second.repo_key == repo_key) {
			// a handle may still be reading this file: dropping its windows is safe (the lock is its own) and
			// is what keeps a read after a `COPY TO` from serving the bytes written before it
			it->second.cache->DropAllWindows();
			it = registry.caches.erase(it);
			HFReadCountersGet().invalidations++;
		} else {
			++it;
		}
	}
}

static idx_t CeilDiv(idx_t a, idx_t b) {
	return (a + b - 1) / b;
}

HFReadCounters &HFReadCountersGet() {
	static HFReadCounters counters;
	return counters;
}

void HFReadCountersReset() {
	auto &counters = HFReadCountersGet();
	counters.files_opened = 0;
	counters.range_hits = 0;
	counters.coalesced_waits = 0;
	counters.window_fetches = 0;
	counters.part_requests = 0;
	counters.straddling_fetches = 0;
	counters.evictions = 0;
	counters.invalidations = 0;
	counters.bytes_fetched = 0;
}

//! Fetch a missing window, with several range requests in flight at once.
//!
//! A single https stream against the Hub CDN carries roughly 25 MB/s while the CDN as a whole serves
//! ~70 MB/s, and because a window is *shared* a single-stream fetch caps every thread waiting on it -
//! coalescing saved requests but also throttled bandwidth. The thread that claims a window has nothing
//! else to do while it is missing, so it splits the window into parts and pulls them concurrently.
bool FetchWindow(HFClient &base, const string &url, const string &path, idx_t start, idx_t len, idx_t part_size,
                 idx_t max_parallel, string &out, string &error) {
	out.clear();
	error.clear();
	auto &counters = HFReadCountersGet();
	if (max_parallel <= 1 || len <= part_size) {
		string fetched;
		counters.window_fetches++;
		counters.part_requests++;
		counters.bytes_fetched += len;
		if (!base.GetRange(url, start, len, fetched)) {
			out.clear();
			return false;
		}
		out = std::move(fetched);
		return out.size() == len;
	}

	const idx_t num_parts = MinValue<idx_t>(max_parallel, CeilDiv(len, part_size));
	const idx_t part_len = CeilDiv(len, num_parts);
	counters.window_fetches++;
	counters.part_requests += num_parts;
	counters.bytes_fetched += len;
	out.resize(num_parts * part_len); // at most part_len-1 bytes past `len`, trimmed below
	std::vector<string> failures;
	std::mutex failure_lock;

	const auto fetch_part = [&](idx_t part) {
		const idx_t part_start = start + part * part_len;
		const idx_t want = MinValue<idx_t>(part_len, start + len - part_start);
		try {
			// a client per thread: a client owns the transport session its requests run on
			HFClient worker(base.Db(), nullptr, base.Connection(), path);
			idx_t got = 0;
			const bool ok =
			    worker.GetRangeInto(url, part_start, want, data_ptr_cast(out.data()) + part * part_len, got);
			if (!ok || got != want) {
				std::lock_guard<std::mutex> guard(failure_lock);
				failures.push_back(StringUtil::Format("read %llu of %llu bytes at offset %llu of '%s'", got, want,
				                                     part_start, path));
			}
		} catch (std::exception &ex) {
			std::lock_guard<std::mutex> guard(failure_lock);
			failures.push_back(string(ex.what()));
		}
	};

	std::vector<std::thread> workers;
	idx_t started = 0;
	try {
		for (; started + 1 < num_parts; ++started) {
			workers.emplace_back(fetch_part, started);
		}
	} catch (std::exception &) {
		// thread creation failed (or one thread is already running): the remaining parts run here
	}
	for (auto &worker : workers) {
		worker.join();
	}
	for (; started < num_parts; ++started) {
		fetch_part(started);
	}

	if (!failures.empty()) {
		// the first part that gave up explains the failure to the caller
		out.clear();
		error = failures.front();
		return false;
	}
	out.resize(len);
	return out.size() == len;
}

HFFileCache::HFFileCache(string key_p, idx_t buffer_size_p, idx_t cache_limit_p, idx_t max_parallel_p)
    : key(std::move(key_p)), buffer_size(buffer_size_p), cache_limit(cache_limit_p),
      max_parallel(MaxValue<idx_t>(max_parallel_p, 1)) {
	// the grid never moves for the life of this cache: windows grow by taking more blocks, not by
	// switching to a coarser alignment (which is what used to make two windows overlap)
	grid_length = MaxValue<idx_t>(MinValue<idx_t>(buffer_size, cache_limit), 1);
	fetch_length = grid_length;
}

shared_ptr<HFFileCache> HFFileCache::Acquire(const string &key, idx_t buffer_size, idx_t cache_limit,
                                            idx_t max_parallel, idx_t total_budget, const string &repo_key) {
	auto &registry = CacheRegistry();
	std::lock_guard<std::mutex> guard(registry.lock);
	registry.clock++;
	auto entry = registry.caches.find(key);
	if (entry != registry.caches.end()) {
		entry->second.last_used = registry.clock;
		return entry->second.cache;
	}
	// a scan over many files opens one per thread, and each would otherwise claim the whole per-file cache
	// for itself: split the total budget over the files open right now so the sum stays near it. A file
	// still gets at least two read-ahead windows, below that every read is a cache miss.
	idx_t open_files = 1;
	for (auto &item : registry.caches) {
		if (item.second.cache.use_count() > 1) {
			open_files++;
		}
	}
	const auto floor_limit = MinValue<idx_t>(HF_MIN_READ_CACHE, cache_limit);
	const auto effective_limit = MinValue<idx_t>(cache_limit, MaxValue<idx_t>(total_budget / open_files, floor_limit));
	HFReadCacheRegistry::Entry fresh;
	fresh.cache = make_shared_ptr<HFFileCache>(key, buffer_size, effective_limit, max_parallel);
	fresh.key = key;
	fresh.repo_key = repo_key;
	fresh.last_used = registry.clock;
	auto inserted = registry.caches.emplace(fresh.key, std::move(fresh));
	registry.Evict(total_budget);
	return inserted.first->second.cache;
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
		HFReadCountersGet().evictions++;
	}
}

void HFFileCache::Read(HFClient &client, const string &url, const string &path, data_ptr_t buffer, idx_t offset,
                       idx_t nr_bytes) {
	if (nr_bytes == 0) {
		return;
	}
	auto &counters = HFReadCountersGet();
	const auto read_end = offset + nr_bytes;
	idx_t cursor = offset;
	idx_t written = 0;
	while (cursor < read_end) {
		shared_ptr<HFPendingRange> waiting_for;
		shared_ptr<HFPendingRange> claimed;
		idx_t fetch_start = 0;
		idx_t fetch_len = 0;
		bool keep = true;
		{
			std::unique_lock<std::mutex> guard(lock);
			const auto size = SizeLocked(client, url, path);
			if (cursor > size || read_end > size) {
				throw IOException("Trying to read %llu bytes at offset %llu of '%s', which is only %llu bytes long",
				                  nr_bytes, offset, path, size);
			}
			// a range request costs the hub ~0.3s whatever its size, so anything fetched is served from here.
			// a window only ever answers the part of the read that falls inside it, a read that straddles two
			// windows is answered twice - which is what keeps every fetch on the window grid
			bool covered = false;
			for (auto it = ranges.begin(); it != ranges.end(); ++it) {
				if (it->start <= cursor && cursor < it->End()) {
					const idx_t available = MinValue(it->End(), read_end) - cursor;
					memcpy(buffer + written, const_data_ptr_cast(it->data.data()) + (cursor - it->start), available);
					written += available;
					cursor += available;
					counters.range_hits++;
					covered = true;
					break;
				}
			}
			if (covered) {
				continue;
			}
			// someone is already pulling a window that covers (or overlaps) the read: let that fetch finish
			// instead of asking the hub for the same bytes twice
			for (auto &entry : pending) {
				if (cursor < entry->end && entry->start < read_end) {
					waiting_for = entry;
					counters.coalesced_waits++;
					break;
				}
			}
			if (!waiting_for) {
				// every window starts on a multiple of `grid_length` and is a whole number of those blocks, so two
				// windows of the same file are always disjoint or one contains the other - no byte is ever fetched
				// twice, whatever order the reads come in. The window doubles on every miss (so a parquet footer
				// costs one small request and a full scan converges on a handful of big ones) but that only adds
				// blocks, the grid itself stays where it was.
				const auto max_window = MaxValue<idx_t>(HF_MAX_READ_AHEAD, grid_length);
				fetch_length = MinValue<idx_t>(MaxValue<idx_t>(fetch_length * 2, grid_length), max_window);
				fetch_start = cursor / grid_length * grid_length;
				// wide enough to end the read, still on the grid
				fetch_len = MinValue<idx_t>(MaxValue<idx_t>(fetch_length,
				                                         CeilDiv(read_end - fetch_start, grid_length) * grid_length),
				                            size - fetch_start);
				// ... but never past a window that is already cached or being fetched: another thread may have
				// claimed a smaller window inside this one a moment earlier, and re-downloading those bytes is
				// exactly what the grid was supposed to prevent. Every window starts on the grid, so the first
				// boundary ahead of the cursor is at most one block away and the trimmed window stays useful.
				{
					auto candidate_end = fetch_start + fetch_len;
					const auto trim_at = [&](idx_t start) {
						if (start > cursor && start < candidate_end) {
							candidate_end = start;
						}
					};
					for (auto &entry : ranges) {
						trim_at(entry.start);
					}
					for (auto &entry : pending) {
						trim_at(entry->start);
					}
					fetch_len = candidate_end - fetch_start;
				}
				if (fetch_len > fetch_length) {
					counters.straddling_fetches++;
				}
				if (getenv("HF_READ_DEBUG")) {
					fprintf(stderr, "hf read: %s claim [%llu,+%llu) grid=%llu window=%llu for read [%llu,%llu)\n", path.c_str(),
					        fetch_start, fetch_len, grid_length, fetch_length, offset, read_end);
				}
				keep = nr_bytes <= cache_limit; // a read larger than the whole cache is not worth keeping
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
		string fetch_error;
		bool ok = FetchWindow(client, url, path, fetch_start, fetch_len, buffer_size, max_parallel, fetched,
		                      fetch_error);
		string message;
		if (!ok) {
			message = fetch_error.empty()
			                  ? StringUtil::Format("Failed to read %llu bytes at offset %llu of '%s'", fetch_len,
			                                       fetch_start, path)
			                  : fetch_error;
		} else if (fetched.size() < fetch_len || fetch_start + fetched.size() <= cursor) {
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
		if (!keep) {
			memcpy(buffer + written, const_data_ptr_cast(range.data.data()) + (cursor - range.start),
			       MinValue(range.End(), read_end) - cursor);
			written += MinValue(range.End(), read_end) - cursor;
			cursor = MinValue(range.End(), read_end);
			continue;
		}
		InsertLocked(std::move(range));
		auto &fresh = ranges.front();
		const idx_t available = MinValue(fresh.End(), read_end) - cursor;
		memcpy(buffer + written, const_data_ptr_cast(fresh.data.data()) + (cursor - fresh.start), available);
		written += available;
		cursor += available;
	}
}

//===----------------------------------------------------------------------===//
// HFReadHandle
//===----------------------------------------------------------------------===//

HFReadHandle::HFReadHandle(HFFileSystem &file_system, const string &path, FileOpenFlags flags, HFTarget target_p,
                           optional_ptr<FileOpener> opener)
    : FileHandle(file_system, path, flags), target(std::move(target_p)) {
	client = HFMakeClient(file_system.Db(), opener, target);
	HFReadCountersGet().files_opened++;
	resolve_url = target.url.ResolveUrl();
	cache_key = resolve_url;
	if (!target.url.revision.empty()) {
		cache_key += "@" + target.url.revision;
	}
	const auto buffer_size = GetSizeSetting(opener, path, "hf_read_buffer_size", HF_DEFAULT_READ_BUFFER);
	const auto cache_limit = GetSizeSetting(opener, path, "hf_read_cache_size", HF_DEFAULT_READ_CACHE);
	const auto total_budget = GetSizeSetting(opener, path, "hf_read_cache_total_size", HF_DEFAULT_READ_CACHE_TOTAL);
	const auto max_parallel = GetSizeSetting(opener, path, "hf_read_max_parallel_requests", HF_DEFAULT_READ_MAX_PARALLEL);
	cache = HFFileCache::Acquire(cache_key, buffer_size, cache_limit, max_parallel, total_budget,
	                             HFRepoCacheKey(target.url));
}

idx_t HFReadHandle::Size() {
	return cache->Size(*client, resolve_url, path);
}

void HFReadHandle::ReadAt(data_ptr_t buffer, idx_t offset, idx_t nr_bytes) {
	cache->Read(*client, resolve_url, path, buffer, offset, nr_bytes);
}

void HFReadHandle::Close() {
	// the cache itself stays in the registry, that is what makes the next open of this file cheap
	cache.reset();
}

} // namespace duckdb
