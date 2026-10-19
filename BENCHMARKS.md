# Reading performance — what the read path costs, measured

How the read path works is in [README.md](README.md); this is the measurement, and the tool that
reproduces it.

## Why few, large, shared requests win

The Hub's CDN costs roughly **0.3 s of latency per range request** regardless of size (1 MB range ≈ 2 MB/s,
full GET ≈ 72 MB/s), so reading a large file is a matter of issuing few, large, *shared* requests:

* a **per-file cache** keyed by resolved url, shared by every handle and thread — DuckDB reads one file with
  several concurrent tasks, and unshared each of them re-downloads the same bytes;
* **fetches on a fixed grid** of `hf_read_buffer_size` blocks: a window doubles on a miss but only by adding
  whole blocks, and is trimmed at the first already-cached boundary, so two windows of one file are always
  disjoint or nested — a strided scan costs one request per block and never re-downloads a byte;
* a read straddling a window edge is served from both; a window another thread is downloading is **waited
  for, not re-requested**; a missing window is pulled into up to `hf_read_max_parallel_requests` parallel
  range requests (one connection pulls ~15 MB/s) inside the read that needed it — no background threads, no
  bytes nobody asked for;
* caches **outlive the handle** under the process-wide budget (LRU; entries being read are left alone), and a
  commit invalidates exactly the repository it changed. The cache is **in-memory only**; DuckDB keeps a cache
  of its own *above* every filesystem (`enable_external_file_cache`, on by default).

Measured on a 287 MB parquet file
(`hf://datasets/open-thoughts/OpenThoughts3-1.2M/data/train-00017-of-00120.parquet`), this extension
against `httpfs` on the same URL, same conditions:

| query | `hf` | `httpfs` |
| --- | --- | --- |
| full scan (`count(*), sum(len(...))`) | **4.5 s** | 5.4 s |
| whole file through `read_blob` | 6.0 s | 4.3 s |
| footer only (`parquet_metadata`) | 0.7 s | 0.7 s |
| small file (1.2 MB) | 0.9 s | 0.8 s |

The full scan fetches 287 412 556 bytes for a 287 412 556 byte file — **exactly 1.000×** — and every
setting combination measured (64 KiB to 8 MiB windows, fan-out on and off) fetched the file once and no
more. `read_blob` of a whole file is the pattern still behind httpfs: one cursor reads strictly in order,
so each window is fetched when its turn comes rather than ahead of it; raising `hf_read_buffer_size` shrinks
the stalls, and true read-ahead across windows is the remaining gap (README *Limitations*).

```bash
scripts/bench.py hf|httpfs <url> <sql> [settings]   # prints timing + hf_read_stats()
```

