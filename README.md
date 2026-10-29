# DuckDB `hf` extension — read *and* write Hugging Face assets with `hf://`
<p align="center">
<img width="400" alt="image" src="https://github.com/user-attachments/assets/456c0f7a-aded-400e-a12d-a996ae6f11c3" />
</p>

A native C++ DuckDB extension to read/write to `hf://` buckets, datasets, etc.

* **Read**: using the Hub's `/resolve/` endpoints (range requests + a shared read-ahead cache);
* **Write**: using the Hub's commit API (same as `huggingface_hub`) and [Xet](https://huggingface.co/docs/hub/en/xet/index) for efficient uploads via deduplication.
* Targets **DuckDB 2.0.0** (`v2.0.0`, *Cyanoptera*).
* Published at [hf.co/duckdb-extensions](https://hf.co/duckdb-extensions).

```sql
INSTALL hf FROM hf_repo; LOAD hf FROM hf_repo;   -- one-time setup: see Installation

SELECT * FROM 'hf://datasets/lhoestq/demo1/*.parquet';
SELECT * FROM hf_ls('hf://datasets/lhoestq/demo1');
SELECT count(*) FROM hf_dataset('lhoestq/demo1', split := 'train');   -- the dataset itself, not the files someone chose
COPY (SELECT * FROM range(10) t(i)) TO 'hf://datasets/me/demo/out.parquet';
COPY (SELECT * FROM read_parquet('hf://datasets/me/demo/*.parquet'))
  TO 'hf://datasets/me/demo/partitioned' (FORMAT PARQUET, PARTITION_BY (kind));
```

## Installation

Make sure you are using DuckDB 2.0:

```sql
SELECT version();   -- v2.0.0        <- the identifier, and the directory name a repository serves;
                                     --   a pre-2.0 client answers v2.0.0-alpha47600 instead, and wants
                                     --   a different binary: one directory per identifier is the mapping
PRAGMA platform;    -- linux_arm64         <- one of linux_amd64, linux_arm64, osx_amd64, osx_arm64,
                                            --   windows_amd64, windows_amd64_mingw,
                                            --   linux_amd64_musl, linux_arm64_musl
```

Install the extension:

* in DuckDB CLI:

```sql
SET allow_extension_repositories = 'allowed';   -- the gate for adding *and* installing from a repository
CREATE OR REPLACE EXTENSION REPOSITORY hf_repo
  WITH PREFIX 'https://hf.co/buckets/duckdb-extensions/builds/resolve';
INSTALL hf FROM hf_repo;
```

* in Python:

```python
import duckdb
con = duckdb.connect(config={"allow_extension_repositories": "allowed"})
con.execute("CREATE OR REPLACE EXTENSION REPOSITORY hf_repo WITH PREFIX "
            "'https://hf.co/buckets/duckdb-extensions/builds/resolve'")
con.execute("INSTALL hf FROM hf_repo")
```

## Usage

### Load and use

```sql
LOAD hf FROM hf_repo;
SELECT * FROM hf_ls('hf://datasets/lhoestq/demo1');
```

### Reads and writes, by repo type

Reads work for all five types, with or without `@revision`:

```sql
SELECT * FROM read_parquet('hf://datasets/wikimedia/wikipedia/20231101.ab/*.parquet');
SELECT content FROM read_text('hf://models/google-bert/bert-base-uncased/config.json');
SELECT * FROM glob('hf://models/nvidia/**/*.safetensors');
SELECT * FROM hf_ls('hf://spaces/dalle-mini/dalle-mini');
SELECT content FROM read_text('hf://kernels/kernels-community/flash-attn3/README.md');
SELECT * FROM read_parquet('hf://buckets/lhoestq/playground/duckdb/*.parquet');
```

A **bucket** file *is* Xet content — no pointer, no branch, no commit — so a bucket write chunks the file,
and uploads it:

```sql
COPY (SELECT * FROM large_table) TO 'hf://buckets/me/playground/export.parquet' (FORMAT PARQUET);
COPY (SELECT * FROM events) TO 'hf://buckets/me/playground/events/' (FORMAT PARQUET, PARTITION_BY (day));
```

`hf_bucket_copy` enables free file copies since files are pointers in Xet.

A **dataset** repository is a table you can load with `hf_dataset()`:

```sql
SELECT * FROM hf_dataset('stanfordnlp/imdb');
SELECT * FROM hf_dataset('stanfordnlp/imdb', split := 'train');
SELECT * FROM hf_dataset('Idavidrein/gpqa', config := 'gpqa_diamond', split := 'train');
```

## Authentication

* with the `hf` CLI:

```bash
hf auth login
```

* manually with a token:

```sql
CREATE SECRET (TYPE HUGGINGFACE, TOKEN 'hf_...');
```

Token resolution has the same order as `huggingface_hub`: a matching `CREATE SECRET (TYPE HUGGINGFACE, …)` →
`SET hf_token` → `HF_TOKEN` → `HF_TOKEN_PATH` → `$HF_HOME/token` → `~/.cache/huggingface/token`.


## Settings

| setting | default | meaning |
| --- | --- | --- |
| `hf_token` | `` | API token (secrets and env vars take precedence as above) |
| `hf_endpoint` | `https://huggingface.co` | Hub endpoint |
| `hf_datasets_server_endpoint` | `https://datasets-server.huggingface.co` | the service that converts datasets to parquet, which is what `hf_dataset()` reads the file list from |
| `hf_cache_dir` | `` | where to buffer files before uploading (falls back to `temp_directory`/`TMPDIR`) |
| `hf_read_buffer_size` | `8388608` | smallest read-ahead window fetched for a `hf://` file (also one range request when a window is split) |
| `hf_read_cache_size` | `268435456` | fetched bytes one `hf://` file may keep in memory (shared by all its readers) |
| `hf_read_cache_total_size` | `1073741824` | fetched bytes the process keeps over all files, and how that budget splits between files open at once |
| `hf_read_max_parallel_requests` | `4` | range requests one missing window is pulled with (`1` disables the fan-out) |
| `hf_max_per_page` | `0` (=1000) | page size for tree listings |
| `hf_commit_message` | `Upload files with DuckDB` | commit message for writes |
| `hf_create_repo` | `false` | create the target repository when it does not exist |
| `hf_xet_write` | `true` | send file content through Xet (content-defined, deduplicated chunks). If the content store is unreachable the write falls back to a plain LFS upload, so a dead CAS costs time, not correctness. `false` forces LFS |
| `hf_write_max_parallel_uploads` | `3` | how many of a file's xorbs are on the wire at once (`1` = sequential). Each one waiting costs up to 64 MiB in memory, which is also how far ahead of the network the chunker may run |
| `hf_xet_compression` | `lz4` | chunk payloads inside an xorb: LZ4 frames, or `none`. Changes bytes on the wire and nothing else — an xorb's hash covers uncompressed content, so dedup, shards and other clients are unaffected. Incompressible chunks come out ~16 B larger (stored raw inside a frame, as reference clients do). Anything else, including `bg4`, is refused at the `SET` |
| `hf_xet_upload_dedup` | `true` | ask the content store whether it already holds a chunk before uploading it, and write a pointer instead of the bytes when it does. Costs one small request per 256 chunks (plus the file's first chunk); `false` uploads everything and produces the byte-identical shard an upload without the feature would have written |

## Table functions

| function | result |
| --- | --- |
| `hf_ls(path, recursive := false)` | `url, path, is_dir, size, sha256` |
| `hf_meta(path)` | `url, exists, size, etag, repo_type, repository, revision` |
| `hf_dataset(name[, config := …][, split := …][, revision := …][, allow_partial := bool])` | the rows of a Hub dataset, read from the parquet files the Hub converted it to — `hf_dataset('lhoestq/demo1')`, or `'hf://datasets/lhoestq/demo1@main'`, or its web page url. More than one config is refused until one is named; a half-converted dataset is refused until `allow_partial := true`; a private or gated dataset is listed and read with the token, as any `hf://` read does |
| `hf_dataset_files(name[, config := …][, split := …][, revision := …])` | `dataset, config, split, url, filename, size, partial` — what `hf_dataset()` would read, one row per converted file, with the `hf://` url it reads it from |
| `hf_upload(local_path, hf_path, message := …)` | `url, size, sha256` |
| `hf_delete(path_or_list, missing_ok := false)` | `url` — one commit per repository in the list |
| `hf_bucket_copy(source, target[, hash_wait := n])` | `source, url, path, size, xet_hash, status` — points a bucket at content the Hub already holds; `hash_wait` (default 30s) is how long to wait for the Hub to finish hashing a file committed moments ago |
| `hf_list_revisions(path, max_revisions := 20)` | `repository, revision, title, author, date` (`limit` is a reserved word) |
| `hf_list_refs(path)` | `repository, kind, name, ref, target_commit` |
| `hf_create_repo(path, private := true)` | `url, created` |
| `hf_status()` | version, DuckDB version, endpoint, token source, whoami, transport, every read/write setting — never the token |
| `hf_read_stats(reset := false)` | the read path's counters: `bytes_fetched / <file size>` is the amplification (a scan should be 1.0), `evictions` says the cache is too small, `retained_bytes` what it still holds |
| `hf_read_cache_clear()` | `cleared_bytes, cleared_files` |
| `hf_xet_upload(path[, scope := 'hf://…'][, publish := bool])` | `xorb, num_chunks, bytes, uncompressed_bytes, inserted, xet_hash, shard_bytes, shard_hash, shard_result, published, deduped_chunks, dedup_queries, dedup_hits, dedup_shard_failures` — the Xet write path by hand; `inserted`/`shard_result` are the CAS's dedup answers, `scope` names the repository whose token the bytes go up against, `publish` registers it as a bucket file, and the last four are per *file* (they repeat on every xorb row; a fully deduplicated upload is one row with `xorb` NULL) |
| `hf_xet_selftest()` | 1090 rows of `(check_name, assert_passed, expected, got, …)` — the Xet port replayed against reference vectors generated from `xet-core`. Offline, no token |

`HF_READ_DEBUG=1` logs every window the read path claims on stderr. `hf_delete` groups paths by
`(endpoint, repo_type, repository, revision)` and commits once per repository; a directory is deleted file
by file (the commit API has no folder delete), and a missing path is an error unless `missing_ok := true`.

## Where the rest is

| file | what it answers |
| --- | --- |
| [BENCHMARKS.md](BENCHMARKS.md) | what reading costs, measured against httpfs, and how to re-measure |
| [DEVELOPMENT.md](DEVELOPMENT.md) | the test suites, building from source, publishing an extension repository, source layout |
| [docs/XET.md](docs/XET.md) | the Xet protocol port: hash chain, formats, upload, verification, M9 |
| [docs/datasets.md](docs/datasets.md) | `hf_dataset()`: the datasets-server route, why its urls are rewritten, what `partial` refuses, which 404 is which |
| [CHANGELOG.md](CHANGELOG.md) | what each release changed |
| [STATE.md](STATE.md) | working handover: current position, next steps, constraints |

## Limitations

* **On a released 2.0.0 client, `hf_status().duckdb_version` answers `v2.0.0-alpha49307`.** The binary is built
  for and loads only in DuckDB `v2.0.0` — the footer `INSTALL` and `LOAD` check says `v2.0.0`, and that is the
  only version string the loader compares. What differs is a *second* string, compiled in from the static-libs
  archive DuckDB's CI hands to extension builds: the commit tagged `v2.0.0` was staged for packaging as
  `v2.0.0-alpha49307`, and `DuckDB::LibraryVersion()` — which `hf_status()` reports — carries that label. Same
  commit (`d54cbbe3df`), same code, different name written down. Official DuckDB extensions do not carry it, so
  this is ours to explain rather than a house convention. Read the other strings with this in mind: `version()`
  and `pragma_version().library_version` answer about *your client* and are correct, and the version of *this
  build* is the footer the loader just checked — the directory it is served from, or `scripts/ext_footer.py`
  against the file. `hf_status().duckdb_version` is the one string that reports the archive label, because
  `DuckDB::LibraryVersion()` here resolves to a copy compiled into the extension rather than to the process
  that loaded it.
* **Writes buffer to a local temp file first** (the commit API needs sha256 + size up front; a bucket write
  needs the file on disk to chunk it), and upload + commit — or the Xet publish — happen on `Close()`. One
  commit per file, serialized per repository in a process; concurrent writers rely on the Hub's conflict
  detection. A Xet upload is per file, per statement, so a partitioned write publishes one file at a time.
* **Dedup is strong on content that repeats and weak on content that is merely similar.**
  Measured (`scripts/dedup_measure.py`, 237 MB):
  an identical re-upload costs **0 bytes and 1.4 s instead of 235 MB and 21.5 s**; appending 20 000 lines to a file
  costs **0.2 MB**. Inserting bytes mid-file keeps
  the prefix and loses the rest. **Efficient deduplication requires [content defined chunking of Parquet pages](https://huggingface.co/blog/parquet-cdc), coming soon.**
* `hf_write_max_parallel_uploads` is a fixed client-side policy, not the reference client's adaptive limiter,
  and it applies per file: a `COPY` of ten files does not get thirty streams. The in-flight cap is also the
  memory cap.
* Small files go as base64 in the commit; larger ones use git-LFS *basic* (one presigned PUT, held in memory
  once) or, above ~10 MB, *basic multipart* (parts streamed from disk). The presigned PUT must not carry the
  API token, and a commit header must omit `parentCommit` rather than send it null — the Hub rejects both.
  Writing to a **protected revision** opens a pull request instead, surfaced as an error carrying the PR url.
* **A repository cannot be deleted from SQL**: the Hub wants its name as a JSON body on a `DELETE`, and
  DuckDB's HTTP transport sends `DELETE` without a body. `hf_delete` removes the files inside;
  `scripts/delete_repo.sh`, `hf repos delete` or `huggingface_hub.delete_repo()` remove the repository.
* The read cache is in-memory only and retained outside DuckDB's buffer manager: on a small machine lower
  `hf_read_cache_total_size` / `hf_read_cache_size` (or set `hf_read_max_parallel_requests = 1`). Retention is
  per process, so a file another client rewrote keeps serving stale bytes until eviction; writes from *this*
  process invalidate immediately. All cache state is guarded because DuckDB hands one handle to several
  concurrent tasks — an unguarded `std::string` cache corrupted itself into a `SIGSEGV`.
* Sequential whole-file reads (`read_blob` of a big file) are behind httpfs — see `BENCHMARKS.md`.
* Every request goes through DuckDB's `HTTPUtil` and the transport must speak https — DuckDB's built-in client
  ships without TLS in official builds. The extension asks for `httpfs` when the registered transport is the
  placeholder or `Built-In`, and explains what to do when that auto-load is blocked.
* `hf_xet_write`'s fallback cannot be *made* to fail from SQL, so that branch is covered by reading and by the
  explicit `hf_xet_write = false` route; a test-only seam in the upload path seemed worse than the gap.
