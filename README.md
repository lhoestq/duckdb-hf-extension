# DuckDB `hf` extension — read *and write* Hugging Face assets with `hf://`

A native C++ DuckDB extension that makes `hf://` URLs behave like `s3://`:

```sql
INSTALL hf; LOAD hf;

SELECT * FROM 'hf://datasets/lhoestq/demo1/*.parquet';
SELECT * FROM hf_ls('hf://datasets/lhoestq/demo1');

COPY (SELECT * FROM range(10) t(i)) TO 'hf://datasets/me/demo/out.parquet';
COPY (SELECT * FROM read_parquet('hf://datasets/me/demo/*.parquet'))
  TO 'hf://datasets/me/demo/partitioned' (FORMAT PARQUET, PARTITION_BY (kind));
```

Reads go through the Hub's `/resolve/` endpoints (with range requests + read-ahead), writes go
through the Hub's commit API (`preupload` → git-LFS basic upload → ndjson commit) — the same protocol
`huggingface_hub` uses.

Target: **DuckDB 2.0** (`v2.0.0-alpha…`, codename *Cyanoptera*), including the Python pre-release
(`pip install duckdb --pre --upgrade`).

## Why not just httpfs?

`httpfs` already ships a *read-only* `hf://` filesystem. This extension:

* adds **writes** (`COPY TO`, `INSERT INTO` through the fs, `hf_upload`, `hf_delete`),
* adds `hf_ls` / `hf_meta` / `hf_upload` / `hf_delete` table functions,
* supports **all** repo types (`datasets`, `models`, `spaces`, `kernels`, `buckets`) for reads, with
  `@revision` and the short `hf://user/repo/path` form,
* registers itself with `IsManuallySet()`, so it wins over httpfs' `hf://` handler whoever loads first,
* reuses the **same** `huggingface` secret type as httpfs, so one `CREATE SECRET` serves both.

It deliberately does **not** link against httpfs' C++ classes (they are not exported stably and
httpfs may be `dlopen(RTLD_LOCAL)`); instead it drives DuckDB's own `HTTPUtil`/`HTTPParams`/
`HTTPHeaders` API and lets DuckDB autoload httpfs at runtime purely as the HTTP *transport*.

## Authentication

Token resolution order (same as `huggingface_hub`):

1. a matching `CREATE SECRET (TYPE HUGGINGFACE, ...)` secret,
2. `SET hf_token = '...'`,
3. `HF_TOKEN`,
4. `HF_TOKEN_PATH`,
5. `$HF_HOME/token`,
6. `~/.cache/huggingface/token`.

Endpoint resolution: `hf_endpoint` setting → `HF_ENDPOINT` env → `https://huggingface.co`.

```sql
CREATE SECRET (TYPE HUGGINGFACE, TOKEN 'hf_...', SCOPE 'hf://datasets/my-org/');
```

## Settings

| setting | default | meaning |
| --- | --- | --- |
| `hf_token` | `` | API token (secrets and env vars take precedence as above) |
| `hf_endpoint` | `https://huggingface.co` | Hub endpoint (global) |
| `hf_cache_dir` | `` | where to buffer files before uploading (falls back to `temp_directory`/`TMPDIR`) |
| `hf_read_buffer_size` | `8388608` | smallest read-ahead window fetched for a `hf://` file, in bytes |
| `hf_read_cache_size` | `134217728` | how many fetched bytes one `hf://` file may keep in memory (shared by every reader of that file) |
| `hf_max_per_page` | `0` (=1000) | page size for tree listings |
| `hf_commit_message` | `Upload files with DuckDB` | commit message for writes |
| `hf_create_repo` | `false` | create the target repository when it does not exist |

## Table functions

| function | result |
| --- | --- |
| `hf_ls(path, recursive := false)` | `url, path, is_dir, size, sha256` |
| `hf_meta(path)` | `url, exists, size, etag, repo_type, repository, revision` |
| `hf_upload(local_path, hf_path, message := ...)` | `url, size, sha256` |
| `hf_delete(path)` / `hf_delete([...])` | `url` (one commit per repository in the list) |

`hf_delete` accepts a mix of repository types and revisions: paths are grouped by
`(endpoint, repo_type, repository, revision)` and committed together per repository. A directory is
deleted by deleting the files below it (the commit API has no folder deletion for existing
revisions), and deleting something that does not exist is an error rather than a silent no-op.

## Reading performance

The Hub's CDN charges roughly **0.3 s of latency per range request**, no matter how small the range
(a 1 MB range request downloads at ~2 MB/s, a full GET of the same file at ~72 MB/s). Reading a
large file therefore comes down to issuing few, large, *shared* requests:

* fetched byte ranges live in a **per-file cache** shared by every handle that has the file open —
  DuckDB reads one file with many concurrent tasks, and without sharing each of them re-downloads
  the same bytes;
* a window that another thread is already downloading is **waited for instead of re-requested**,
  while reads of other parts of the file still run in parallel (this is what makes the per-request
  latency overlap the way httpfs manages to);
* the read-ahead window **doubles on every miss** (8 MB → 16 MB → 32 MB) and is **aligned**, so a
  footer read costs one small request while a full scan converges on a handful of big ones.

Measured on a 287 MB parquet file
(`hf://datasets/open-thoughts/OpenThoughts3-1.2M/data/train-00017-of-00120.parquet`), DuckDB
2.0.0-alpha + the Python pre-release, once against the live Hub:

| query | `hf` extension | `httpfs` (`https://`) |
| --- | --- | --- |
| full scan (`count(*), sum(length(...))`) | 8.8 s | 6.6 s |
| whole file through `read_blob` | 5.0 s | 4.9 s |
| footer only (`count(*)`) | 0.9 s | 0.6 s |
| small file (1.2 MB) | 0.9 s | 0.8 s |

Raise `hf_read_cache_size` (e.g. `SET hf_read_cache_size = 268435456`) when scanning files much
bigger than the cache with many threads: the cache is bounded per file, so threads stop evicting
each other's windows.

## Status

Everything below is implemented in this tree; the ✔ marks are verified against the live Hub (see
`scripts/smoke_read.sql` / `scripts/smoke_write.sql`), the ✘ marks are deliberately out of scope.

* ✔ `hf://` URL parsing (repo types, `@revision`, short form, buckets)
* ✔ settings + `huggingface` secrets (`config`, `credential_chain`), shared with httpfs
* ✔ read path: `SELECT * FROM 'hf://...'`, range reads, globbing (`*`, `**`), `hf_ls`, `hf_meta`,
  ETag/`version_tag` + mtime so DuckDB's external file cache can validate entries
* ✔ write path: `COPY ... TO 'hf://...'` (parquet/csv/json/zstd, overwrite, empty files,
  `PARTITION_BY`, unicode + spaces in paths, model + dataset repos, custom commit messages)
* ✔ `hf_upload`, `hf_delete` (single path, list of paths, mixed repositories, recursive directories)
* ✔ bucket reads and bucket deletes (`hf://buckets/...`)
* ✘ Xet uploads (`hf_xet` is a Rust library, out of scope) — an LFS fallback is used whenever the
  server allows it
* ✘ multipart LFS uploads (>5 GiB single objects) — use `PARTITION_BY` to split output
* ✘ `hf://buckets/*` writes: bucket uploads require an Xet `xetHash`, which only `hf_xet` computes;
  the extension refuses with a message pointing at `hf buckets cp`

## Layout

```
src/
  hf_common.hpp     url/util structs, shared constants
  hf_url.cpp        hf:// parsing, resolve/tree url building
  hf_crypto.{hpp,cpp}  self-contained sha256/base64/json writer (no optional duckdb deps)
  hf_secrets.{hpp,cpp} secret type + settings + env + token-file resolution
  hf_http.{hpp,cpp} HTTPUtil wrapper: auth, retries, pagination, error translation, JSON scanning,
                    transport-session reuse
  hf_fs.{hpp,cpp}   FileSystem implementation (dispatch, listing, glob, delete, move)
  hf_read.cpp       per-file range cache (shared, coalesced, growing windows)
  hf_commit.cpp     the write protocol: preupload -> lfs batch -> ndjson commit (+repo mutex)
  hf_write.cpp      write handle: temp file buffering, upload+commit on close
  hf_functions.cpp  hf_ls / hf_meta / hf_upload / hf_delete
  hf_extension.cpp  entry point
scripts/
  try.py            runs one SQL statement per line, prints OK/FAIL (used by the smoke tests)
  smoke_read.sql    read-side end-to-end checks against the live Hub
  smoke_write.sql   write-side end-to-end checks (uses the lhoestq/playground scratch repos)
  bench.py          times a query with either the `hf` filesystem or plain `httpfs`
  check.sh          compile-only syntax check of every source file (fast feedback)
```

## Building

```bash
# duckdb + extension-ci-tools are vendored in ../vendor (see STATE.md)
make release            # build/release/extension/hf/hf.duckdb_extension
```

Load it from the Python pre-release:

```python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD '/path/to/hf.duckdb_extension'")
```

## Notes / limitations

* Writes buffer to a local temp file first (the commit API needs the sha256 + size up front), then
  upload + commit happen on `Close()`. One commit per file, serialised per repository within a
  process; concurrent writers are handled by the Hub's own conflict detection.
* Files ≤ the Hub's inline threshold are sent as base64 in the commit payload; larger ones use
  git-LFS *basic* transfer (a single presigned PUT, so the file is held in memory once). The
  presigned PUT must **not** carry the API token, and a commit header must omit `parentCommit`
  rather than send it as null — both are rejected by the Hub.
* Writing to a protected revision creates a pull request instead of committing — that is surfaced as
  an error with the PR url rather than silently swallowed.
* Reads are served by the shared per-file cache described above. DuckDB 2.0 hands one file handle to
  several concurrent tasks, so all handle/cache state is guarded: an unguarded `std::string` read
  cache corrupted itself into a `SIGSEGV` (which is how the shared cache earned its mutexes).
* `hf://` listings come from `/api/{type}/{repo}/tree/...` with `Link` pagination; a folder entry
  that no longer resolves during a glob walk is treated as empty instead of failing the query.
