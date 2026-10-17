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

* adds **writes** (`COPY TO`, `INSERT INTO` through the fs, `hf_upload`, `hf_delete`,
  `hf_bucket_copy`),
* adds `hf_ls` / `hf_meta` / `hf_upload` / `hf_delete` / `hf_bucket_copy` table functions,
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
| `hf_read_buffer_size` | `8388608` | smallest read-ahead window fetched for a `hf://` file, in bytes (also the size of one range request when a window is split) |
| `hf_read_cache_size` | `268435456` | how many fetched bytes one `hf://` file may keep in memory (shared by every reader of that file) |
| `hf_read_cache_total_size` | `1073741824` | how many fetched bytes the process keeps around over all files, and how that budget is split between the files open at once |
| `hf_read_max_parallel_requests` | `4` | how many range requests one missing window is pulled with, in parallel (`1` disables the fan-out) |
| `hf_max_per_page` | `0` (=1000) | page size for tree listings |
| `hf_commit_message` | `Upload files with DuckDB` | commit message for writes |
| `hf_create_repo` | `false` | create the target repository when it does not exist |

## Table functions

| function | result |
| --- | --- |
| `hf_ls(path, recursive := false)` | `url, path, is_dir, size, sha256` |
| `hf_meta(path)` | `url, exists, size, etag, repo_type, repository, revision` |
| `hf_upload(local_path, hf_path, message := ...)` | `url, size, sha256` |
| `hf_delete(path)` / `hf_delete([...], missing_ok := false)` | `url` (one commit per repository in the list) |
| `hf_bucket_copy(source, target[, hash_wait := n])` | `source, url, path, size, xet_hash, status` - moves files that are already on the Hub into a bucket; `hash_wait` is how long to wait (default 30s) for the Hub to finish hashing a file committed moments ago |
| `hf_list_revisions(path, max_revisions := 20)` | `repository, revision, title, author, date` |
| `hf_list_refs(path)` | `repository, kind, name, ref, target_commit` |
| `hf_create_repo(path, private := true)` | `url, created` (`created = false` when it already existed) |
| `hf_status()` | `hf_version, duckdb_version, endpoint, token_source, user, fullname, whoami_error, http_transport, hf_read_buffer_size, hf_read_cache_size, hf_read_max_parallel_requests, hf_read_cache_total_size, hf_max_per_page, hf_create_repo, hf_cache_dir` |
| `hf_read_stats(reset := false)` | `files_opened, range_hits, coalesced_waits, window_fetches, part_requests, straddling_fetches, evictions, bytes_fetched, invalidations, retained_bytes, retained_files` |
| `hf_read_cache_clear()` | `cleared_bytes, cleared_files` (drops everything the read cache retained) |

`hf_read_stats()` is the read path's dashboard: `bytes_fetched / <file size>` is the download
amplification (a scan of one file should be exactly 1.0), `evictions` says the per-file cache is too
small for the working set, and `retained_bytes` what it is still holding after the handles closed.
`reset := true` zeroes the counters without touching the cached bytes. Running the process with
`HF_READ_DEBUG=1` also logs every window the read path claims (`path`, window, grid, and the read that
triggered it) on stderr. `straddling_fetches` counts reads served by copying across a window boundary -
windows are grid aligned, reads are not, so a few are normal and cost no extra bytes; it grows when the
read-ahead pattern and the cache shape disagree.

`max_revisions` is spelled that way because `limit` is a reserved word in DuckDB's grammar.
`hf_status()` is the "what am I connected to" command: it never prints the token, and a rejected or
missing token shows up in `whoami_error` instead of throwing, so the settings stay readable.

`hf_delete` accepts a mix of repository types and revisions: paths are grouped by
`(endpoint, repo_type, repository, revision)` and committed together per repository. A directory is
deleted by deleting the files below it (the commit API has no folder deletion for existing
revisions), and deleting something that does not exist is an error rather than a silent no-op -
until `missing_ok := true`, which is what cleanup scripts usually want.

## Every repo type

Reads work for all five repo types, with or without the `@revision` suffix:

```sql
-- datasets
SELECT * FROM read_parquet('hf://datasets/wikimedia/wikipedia/20231101.ab/*.parquet');
SELECT * FROM hf_ls('hf://datasets/lhoestq/demo1', recursive := true);

-- models (note: no `models/` prefix in the underlying url, like huggingface_hub)
SELECT content FROM read_text('hf://models/google-bert/bert-base-uncased/config.json');
SELECT * FROM glob('hf://models/nvidia/**/*.safetensors');

-- spaces and kernels
SELECT * FROM hf_ls('hf://spaces/dalle-mini/dalle-mini');
SELECT content FROM read_text('hf://kernels/kernels-community/flash-attn3/README.md');

-- buckets (S3-like storage: unversioned, addressed by id)
SELECT * FROM read_parquet('hf://buckets/lhoestq/playground/duckdb/*.parquet');
SELECT url, size FROM hf_meta('hf://buckets/lhoestq/playground/duckdb/probe.parquet');
```

Writes (parquet, csv, json, partitioned output, LFS-tracked files, multipart LFS for big blobs)
work for `datasets`, `models`, `spaces` and `kernels`:

```sql
COPY (SELECT * FROM range(10) t(i)) TO 'hf://datasets/me/demo/out.parquet';
COPY (SELECT * FROM read_parquet('hf://datasets/me/in/*.parquet'))
  TO 'hf://models/me/demo/partitioned' (FORMAT PARQUET, PARTITION_BY (kind));

-- overwrite in place (a commit that replaces the previous file)
COPY (SELECT 42 AS answer) TO 'hf://datasets/me/demo/out.parquet';

-- one-shot helpers
SELECT * FROM hf_upload('out.parquet', 'hf://datasets/me/demo/out.parquet',
                        message := 'daily export 2026-10-16');
SELECT * FROM hf_delete(['hf://datasets/me/demo/old.parquet', 'hf://models/me/demo/partitioned']);
```

Buckets are readable and deletable, and they take new data through `hf_bucket_copy`: a bucket upload
(`addFile`) requires a Xet `xetHash`, which only the `hf_xet` client computes, so DuckDB cannot push
bytes into one. What it *can* do is ask the Hub to point a bucket path at content it already stores -
from a dataset, model, space, kernel or another bucket - which is a server-side copy: no bytes go
through DuckDB, and the size does not matter.

```sql
-- one file, onto an exact name
SELECT * FROM hf_bucket_copy('hf://datasets/me/demo/out.parquet',
                             'hf://buckets/me/playground/exports/out.parquet');
-- a whole folder (or a glob): the structure below the source is mirrored under the destination
SELECT * FROM hf_bucket_copy('hf://datasets/me/demo/partitioned', 'hf://buckets/me/playground/exports/');
SELECT * FROM hf_bucket_copy('hf://datasets/me/demo/*.parquet', 'hf://buckets/me/playground/parquet/');
-- and from a bucket to a bucket
SELECT * FROM hf_bucket_copy('hf://buckets/me/playground/exports/out.parquet',
                             'hf://buckets/me/playground/archive/out.parquet');
```

One row per source file, with the `xet_hash` it was registered under and a `status`. Two things are
worth knowing: a file committed seconds earlier is being hashed by the Hub in its own time (the copy
waits up to `hash_wait` seconds for it (default 30, `hash_wait := 0` to refuse waiting), and a file the
Hub keeps outside Xet - a plain
git blob, which is how small files sometimes land - has no hash to reference and comes back as
`skipped: ...` rather than an error, because a bucket simply cannot point at it.

A direct write is still refused, and says so:

```sql
COPY (SELECT 1) TO 'hf://buckets/me/playground/x.parquet';
-- IO Error: Writing to Hugging Face buckets is not supported yet: bucket uploads (...) go through
-- Xet storage, whose `xetHash` only the `hf_xet` client computes - use `hf buckets cp` or
-- `hf_bucket_copy(...)` (...)
```
```bash
hf buckets cp out.parquet hf://buckets/lhoestq/playground/duckdb/out.parquet   # bytes from a local disk
```

Verified end to end in `scripts/smoke_buckets.sh`: fixtures are written by DuckDB into a staging
dataset repository (which also covers create-on-write), copied into a bucket, read back through
`hf://`, deleted again, and both refusal paths are exercised.

## Reading performance

The Hub's CDN charges roughly **0.3 s of latency per range request**, no matter how small the range
(a 1 MB range request downloads at ~2 MB/s, a full GET of the same file at ~72 MB/s). Reading a
large file therefore comes down to issuing few, large, *shared* requests:

* fetched bytes live in a **per-file cache**, keyed by the resolved url, shared by every handle and
  every thread reading that file — DuckDB 2.0 reads one file with several concurrent tasks, and
  without sharing each of them re-downloads the same bytes;
* **everything is fetched on a fixed grid** of `hf_read_buffer_size` blocks. A window starts on the grid
  and is a whole number of blocks wide; the window doubles on every miss but that only adds blocks, the
  grid itself never moves, and a candidate window is trimmed at the first boundary that is already cached
  or being fetched. Two windows of one file are therefore always disjoint (or one contains the other),
  whatever order the reads arrive in — a strided access pattern, which is exactly what DuckDB's parquet
  prefetch does (2 MB reads at large offsets), costs one request per block and never re-downloads a byte;
* a read that straddles a window edge is served from the two windows instead of re-fetching the overlap;
* a window another thread is already downloading is **waited for instead of re-requested**, while
  reads of other parts of the file keep running in parallel;
* a missing window is **pulled apart into up to `hf_read_max_parallel_requests` range requests** and
  fetched in parallel. One connection can only pull ~15 MB/s from the CDN, so the fan-out is what
  hides the per-request latency, and it happens inside one read: no background threads, a thread never
  downloads bytes nobody asked for;
* caches **outlive the handle that created them** and are kept until the process-wide
  `hf_read_cache_total_size` budget needs the space (LRU, entries currently being read are left
  alone), so a second query over the same file is served from memory. When the budget is split between
  the files open at once, no single file can push the sum over it. A commit invalidates the cached
  windows of exactly the repository it changed, so a read after a `COPY TO` sees the new bytes, and
  `SELECT * FROM hf_read_cache_clear()` drops everything that is retained.
* nothing is ever written to disk by this extension on the read path — its cache is **in-memory only**.
  The only local files it creates are the mandatory scratch buffers of the *write* path, before a commit.
  Note that DuckDB 2.0 has a cache of its own *above* every filesystem (`enable_external_file_cache`,
  on by default, in memory unless `external_file_cache_spill` is set), which will serve a repeated read
  of a small file before this extension is ever asked for it.

Measured on a 287 MB parquet file
(`hf://datasets/open-thoughts/OpenThoughts3-1.2M/data/train-00017-of-00120.parquet`), DuckDB
2.0.0-alpha + the Python pre-release, against the live Hub (and `httpfs` on the same URL in the same
conditions, so the numbers move a bit per run):

| query | `hf` extension | `httpfs` (`https://`) |
| --- | --- | --- |
| full scan (`count(*), sum(len(...))`) | **4.5 s** | 5.4 s |
| whole file through `read_blob` | 6.0 s | 4.3 s |
| footer only (`parquet_metadata`) | 0.7 s | 0.7 s |
| small file (1.2 MB) | 0.9 s | 0.8 s |

The full scan fetches **287 412 556 bytes for a 287 412 556 byte file (exactly 1.000×)** — every
setting combination that was measured (`hf_read_buffer_size` from 64 KiB to 8 MiB, fan-out on and off)
fetched the file once and no more. Getting there is what the earlier rounds of this section were about:
the same query used to download 1.9× the file and run in 8.8 s against httpfs' 6.6 s.

`read_blob` of a whole file is the one pattern still behind httpfs: the file is read strictly in order
by one cursor, so each window is fetched when its turn comes instead of ahead of it. Raising
`hf_read_buffer_size` (say `SET hf_read_buffer_size = 33554432`) shrinks the number of stalls; true
read-ahead prefetch across windows is the remaining known gap, see *Limitations*.

`scripts/bench.py hf|httpfs <url> <sql> [settings]` reproduces these numbers, printing the timing and
`hf_read_stats()` of the run (`python3 scripts/bench.py hf "$URL" "$SQL" "hf_read_buffer_size=33554432"`).

## Status

Everything below is implemented in this tree; the ✔ marks are verified against the live Hub by the
suites in [*Testing*](#testing), the ✘ marks are deliberately out of scope. Release notes, including
what a given binary does and does not do, are in [CHANGELOG.md](CHANGELOG.md).

* ✔ `hf://` URL parsing (repo types, `@revision`, short form, buckets)
* ✔ settings + `huggingface` secrets (`config`, `credential_chain`), shared with httpfs
* ✔ read path: `SELECT * FROM 'hf://...'`, range reads, globbing (`*`, `**`), `hf_ls`, `hf_meta`,
  ETag/`version_tag` + mtime so DuckDB's external file cache can validate entries
* ✔ write path: `COPY ... TO 'hf://...'` (parquet/csv/json/zstd, overwrite, empty files,
  `PARTITION_BY`, unicode + spaces in paths, model + dataset repos, custom commit messages)
* ✔ `hf_upload`, `hf_delete` (single path, list of paths, mixed repositories, recursive directories)
* ✔ model / space / kernel repos end to end (writes, nested paths, `**` globs, LFS and git-tracked
  files verified byte-identical with `md5(read_blob(...))`)
* ✔ bucket reads, bucket deletes (`hf://buckets/...`) and bucket writes through `hf_bucket_copy`
  (server-side copies of anything already on the Hub: files, folders, globs, bucket to bucket)
* ✔ `hf_list_revisions`, `hf_list_refs`, `hf_create_repo`, `hf_status`, `hf_read_stats`,
  `hf_read_cache_clear`
* ✔ create-on-write: with `hf_create_repo = true` the first write creates the repository (private;
  spaces as `sdk = static`), and a 404 without it explains that rather than only quoting the Hub
* ✔ distribution: `scripts/make_extension_repo.sh` + `INSTALL hf FROM '<prefix>'` verified against a
  local repository, both unsigned and signed (signature required, tampered binaries refused)
* ✔ multipart git-LFS uploads (the Hub switches a blob to `basic multipart` above ~10 MB: one
  presigned PUT per part, then a completion POST with the part etags)
* ✔ the Xet *hashing* half (`docs/XET.md`, M1+M2): chunk hashes, file/xorb merkle roots and the
  content-defined chunker agree byte for byte with `xet-core`, checked by `hf_xet_selftest()` /
  `scripts/smoke_xet.sh` against vectors generated from the reference implementation.
* ✘ Xet *uploads* are not implemented yet (M3-M5: xorb format, CAS client, shards) — the git-LFS
  transfer is used instead, which the Hub
  accepts for every repo type. Chunking + hashing client-side is the 0.0.2 plan, in
  [docs/XET.md](docs/XET.md)
* ✘ `COPY ... TO 'hf://buckets/...'`: pushing DuckDB's own bytes into a bucket needs an Xet
  `xetHash`, which only `hf_xet` computes; the extension refuses with a message pointing at
  `hf buckets cp` and `hf_bucket_copy`. The same reason makes a `hf_bucket_copy` of a file the Hub
  keeps as a plain git blob come back `skipped` - there is no hash for a bucket to point at

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
  hf_read.cpp       per-file range cache: aligned window grid, shared + coalesced, parallel window
                    fetch, budgeted retention beyond the handle, invalidation on commit
  hf_commit.cpp     the write protocol: preupload -> lfs batch -> ndjson commit (+repo mutex)
  hf_write.cpp      write handle: temp file buffering, upload+commit on close
  hf_xet_hash.cpp   BLAKE3-keyed Xet hashes, mean tree, reference self-test (M1)
  hf_xet_chunk.cpp  gear-hash content-defined chunking, byte-exact with xet-core (M2)
  hf_functions.cpp  hf_ls / hf_meta / hf_upload / hf_delete / hf_bucket_copy / hf_list_revisions /
                    hf_list_refs / hf_create_repo / hf_status / hf_read_stats / hf_read_cache_clear
  hf_extension.cpp  entry point
scripts/
  try.py            the smoke runner: one SQL statement per line, graded (see *Testing*)
  run_all.sh        builds if asked, runs every suite, prints one grade line per suite
  smoke_read.sh/.sql      read path: range reads, globbing, cache behaviour, byte accounting
  smoke_write.sh/.sql     write path: formats, LFS multipart, create-on-write, delete semantics
  smoke_models.sh/.sql    model repos: nested paths, globs, git vs LFS byte-level integrity
  smoke_buckets.sh/.sql   buckets: server-side copies in, reads, globs, deletes (+ setup fixtures)
  delete_repo.sh    drop a scratch repository for good (see the note in *Notes / limitations*)
  bench.py          times a query with either the `hf` filesystem or plain `httpfs`
  check.sh          compile-only syntax check of every source file (fast feedback)
  make_extension_repo.sh  turn a build into a serveable <version>/<platform>/ repository (+signing)
  test_install.py   INSTALL + LOAD + query from that repository, unsigned and signed (with a tamper check)
.github/workflows/
  build.yml         calls DuckDB's reusable distribution workflow for the platform matrix
```

## Testing

The suites run against the **live Hub** — they write to scratch repositories and clean up after
themselves, so they need a token with write access (`~/.cache/huggingface/token`, or `HF_TOKEN`).

```bash
scripts/run_all.sh --build    # build once, then all four suites
bash scripts/smoke_read.sh    # or one suite on its own: smoke_read / smoke_write / smoke_models
bash scripts/smoke_models.sh  # each .sh makes its own fixtures and cleans up after itself
```

```
--- read
    read: 55 passed, 0 expected failures, 0 failed
--- write
    write: 45 passed, 5 expected failures, 0 failed
--- models
    models: 24 passed, 0 expected failures, 0 failed
--- buckets
    buckets: 16 passed, 2 expected failures, 0 failed
```

`scripts/try.py` is the runner. It takes a `.sql` file, executes **one statement per line-group**
(semicolon separated), and grades the result:

* a returned column named `assert*` must be `TRUE` — that is how a check is written, so the SQL stays
  SQL and a failure names the statement that produced it;
* a `-- expect-fail: <substring>` comment above a statement means the statement must raise, with that
  substring in the message. That is how refusals are tested (writing into a bucket, `hf_upload` to a
  repository that does not exist), so a refusal that stops refusing starts failing.

Each `.sh` wrapper makes its own fixtures (random bytes for the LFS/blob checks, text files), so there
are no manual pre-steps. Scratch state: `lhoestq/playground` (datasets, models, buckets) is kept and
reused, `hf-smoke-staging` and `hf-ext-write-probe` are created per run and deleted by
`scripts/delete_repo.sh` afterwards.

## Building

```bash
# duckdb + extension-ci-tools are either checked out next to this Makefile (as ./duckdb and
# ./extension-ci-tools, which is what CI does) or vendored one level up in ../vendor
cmake --version > /dev/null || pip install cmake ninja       # the build needs cmake inside the venv
make release OVERRIDE_GIT_DESCRIBE="$(python -c \"import duckdb;print(duckdb.sql('PRAGMA version').fetchall()[0][0])\")"
# -> build/release/extension/hf/hf.duckdb_extension (copied to dist/ by convention)
```

Load it from the Python pre-release (a locally built extension is unsigned, so it needs the flag):

```python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD '/path/to/hf.duckdb_extension'")
```

## Distributing

An extension is a binary tied to **one** DuckDB version identifier and **one** platform, so a
repository is just a static file tree — any web server, `file://` directory, or Hugging Face repo
will do:

```
<prefix>/<duckdb_version>/<platform>/hf.duckdb_extension[.gz]
```

`scripts/make_extension_repo.sh` assembles that tree from a build (DuckDB 2.0 sniffs gzip, so both
the plain and the compressed file are valid; it reads the version and the platform *out of the
binary footer* rather than guessing them):

```bash
./scripts/make_extension_repo.sh dist/repo
python3 scripts/test_install.py dist/repo     # INSTALL + LOAD + a real hf_ls, in a throwaway home dir
```

Clients then say:

```sql
INSTALL hf FROM 'https://my.host/duckdb-extensions/';   -- or 'file:///home/me/dist/repo/'
LOAD hf;
```

Use `FROM '<prefix>'` rather than `SET custom_extension_repository=...`: the latter redirects *every*
installation — including the `httpfs` this extension needs as its https transport — to your own
repository, where `httpfs` is not. If you do want your repository to be the default, keep the
official one for autoinstalls with `SET autoinstall_extension_repository='core'`.

### The version identifier has to match

`LOAD` refuses a binary whose baked-in version identifier differs from the running DuckDB, which is
why the build above takes `OVERRIDE_GIT_DESCRIBE`. For a released DuckDB just build against its tag
(`v2.0.0`) and one artifact serves everyone. For a dev pre-release there is no tag: the Python wheel
`2.0.0.dev2610151406` reports `v2.0.0-alpha47600`, and the next nightly bumps that number again — so
per-dev-build artifacts mean rebuilding per dev build, with the string read from the target duckdb
(the `python -c` above) rather than guessed.

### Signing (optional, but what makes `INSTALL` safe)

DuckDB reserves the last 256 bytes of the binary for an RSA signature over a two-level sha256 (the
sha256 of the concatenation of the per-1 MiB-chunk digests). `make_extension_repo.sh` does that when
`DUCKDB_EXTENSION_SIGNING_PK` is set, and publishes the matching public key at
`<prefix>/.well-known/duckdb-extension-repo.json`:

```bash
openssl genrsa -out /tmp/repo.pem 2048
DUCKDB_EXTENSION_SIGNING_PK="$(cat /tmp/repo.pem)" ./scripts/make_extension_repo.sh dist/repo-signed
python3 scripts/test_install.py dist/repo-signed --signed   # installs without allow_unsigned_extensions
```

Clients trust the repository once, and DuckDB then verifies every install against the pinned key:

```sql
SET allow_extension_repositories = 'allowed';   -- opt-in: adding third-party repositories
CREATE EXTENSION REPOSITORY hf_repo
  WITH PREFIX 'https://my.host/duckdb-extensions/'
  USING PUBLIC KEY '<base64 of the DER public key>';
INSTALL hf FROM hf_repo;
LOAD hf FROM hf_repo;                           -- per-repository installs live under repositories/<name>/
SELECT repository_name, key_fingerprints FROM duckdb_extension_repositories();
```

`--signed` in the test script really means *required*: it connects without
`allow_unsigned_extensions`, checks the fingerprint of the key that signed the installed binary, then
copies the repository, flips a byte in it, and demands that a fresh DuckDB refuses to install it.

### Hosting it on Hugging Face

The repository is a folder of files, so the Hub can serve it — which means the extension can ship
through the very protocol it implements:

```sql
-- one file per platform, straight from the build machine
SELECT hf_upload('dist/repo/v2.0.0-alpha47600/linux_arm64/hf.duckdb_extension.gz',
                 'hf://datasets/me/duckdb-hf-repo/v2.0.0-alpha47600/linux_arm64/hf.duckdb_extension.gz');
```

```bash
# or the whole tree (every platform + the .well-known metadata) in one commit
hf upload me/duckdb-hf-repo dist/repo --repo-type dataset --private
```

and clients get `INSTALL hf FROM 'https://huggingface.co/datasets/me/duckdb-hf-repo/resolve/main/'`.

## Notes / limitations

* Writes buffer to a local temp file first (the commit API needs the sha256 + size up front), then
  upload + commit happen on `Close()`. One commit per file, serialised per repository within a
  process; concurrent writers are handled by the Hub's own conflict detection.
* Files ≤ the Hub's inline threshold are sent as base64 in the commit payload; larger ones use
  git-LFS transfer: *basic* (one presigned PUT, the file is held in memory once) or, above the
  Hub's ~10 MB threshold, *basic multipart* (parts streamed from disk one at a time). The
  presigned PUT must **not** carry the API token, and a commit header must omit `parentCommit`
  rather than send it as null — both are rejected by the Hub.
* Writing to a protected revision creates a pull request instead of committing — that is surfaced as
  an error with the PR url rather than silently swallowed.
* Reads are served by the shared per-file cache described above. DuckDB 2.0 hands one file handle to
  several concurrent tasks, so all handle/cache state is guarded: an unguarded `std::string` read
  cache corrupted itself into a `SIGSEGV` (which is how the shared cache earned its mutexes).
* The read cache is **in-memory only** — there is no on-disk read cache, and none is planned. Retained
  bytes are not tracked by DuckDB's buffer manager, so on a small machine lower
  `hf_read_cache_total_size` / `hf_read_cache_size` (or set `hf_read_max_parallel_requests = 1` to
  also cut the number of live sockets).
* Retention is per process: if another client (the `hf` CLI, a `huggingface_hub` script) rewrites a
  file while a DuckDB process already read it, that process keeps serving what it fetched until the
  cache is evicted. Writes from *this* process invalidate the affected repository immediately.
* One read pattern is still behind httpfs: pulling a whole file in one strict sequence
  (`read_blob` of a big file). Windows are fetched as the cursor reaches them rather than ahead of it,
  so a true async prefetch (with safe cancellation) is the remaining gap; see *Reading performance*.
* `hf://` listings come from `/api/{type}/{repo}/tree/...` with `Link` pagination; a folder entry
  that no longer resolves during a glob walk is treated as empty instead of failing the query.
* A **repository** cannot be deleted from SQL: the Hub wants its name as a JSON body on a `DELETE`
  request, and DuckDB's HTTP transport sends `DELETE` without a body (`DeleteRequestInfo` has no
  payload field, and neither the curl nor the httplib client attaches one). `hf_delete` deletes the
  files inside a repository; `scripts/delete_repo.sh`, `hf repos delete` and
  `huggingface_hub.delete_repo()` delete the repository itself.
* Creating a Space needs an sdk, so create-on-write sends `sdk = static` for `hf://spaces/...`.
  Nothing else is inferred about a new repository: it is private, and named after the url.
* Every request goes through DuckDB's `HTTPUtil` API, and the transport that answers has to speak
  https: the client built into DuckDB ships without TLS in the official distributions. The extension
  therefore asks for `httpfs` whenever the registered transport is the placeholder or `Built-In`, and
  explains what to do when that auto-load is blocked (`autoinstall_known_extensions`, or a
  `custom_extension_repository` that hides the official repository).
