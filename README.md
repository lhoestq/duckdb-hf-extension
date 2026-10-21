# DuckDB `hf` extension — read *and* write Hugging Face assets with `hf://`

A native C++ DuckDB extension that makes `hf://` behave like `s3://`:

```sql
INSTALL hf FROM hf_repo; LOAD hf FROM hf_repo;   -- one-time setup: see Installation

SELECT * FROM 'hf://datasets/lhoestq/demo1/*.parquet';
SELECT * FROM hf_ls('hf://datasets/lhoestq/demo1');
COPY (SELECT * FROM range(10) t(i)) TO 'hf://datasets/me/demo/out.parquet';
COPY (SELECT * FROM read_parquet('hf://datasets/me/demo/*.parquet'))
  TO 'hf://datasets/me/demo/partitioned' (FORMAT PARQUET, PARTITION_BY (kind));
```

Reads use the Hub's `/resolve/` endpoints (range requests + a shared read-ahead cache); writes use the
Hub's commit API (`preupload` → git-LFS or Xet → ndjson commit) — the same protocol `huggingface_hub`
uses. Targets **DuckDB 2.0.0** (`v2.0.0`, *Cyanoptera*) — the released version, plain `pip install duckdb`.
A binary is tied to exactly one version identifier, so the alpha this project was built against up to and
including 0.0.2-pre8 (`v2.0.0-alpha47600`, what a pre-2.0 client reports) is a different artifact, not the
same one relabelled. [Installation](#installation) says which client to pin against which identifier.

Current release: **0.0.2-pre8**, built for `v2.0.0-alpha47600` on all eight DuckDB client platforms — cross-repository
dedup (`COPY TO` an upload can point at content another repository already stored: 0 bytes and 1.4 s to re-upload
237 MB you already have). **0.0.2-pre9** is tagged and built for `v2.0.0`; it publishes when its CI run finishes,
and until it does a client on released 2.0.0 has nothing to install — which is a refusal from `LOAD`, not a
fallback. HEAD may carry fixes beyond either.
Suites: 212 passed / 13 expected failures / 0 failed, plus 1090 offline reference checks. Release notes in
[CHANGELOG.md](CHANGELOG.md); the Xet protocol port and its measurements in [docs/XET.md](docs/XET.md).

## Why not just httpfs

`httpfs` ships a *read-only* `hf://` filesystem, and rejects `models`. This adds **writes** (`COPY TO`,
`INSERT`, `hf_upload`, `hf_delete`, `hf_bucket_copy`), the table functions below, all five repo types,
`@revision` and the short `hf://user/repo/path` form, and registers with `IsManuallySet()` so it wins over
httpfs whoever loads first. The `huggingface` **secret type** is shared, so one `CREATE SECRET` serves both.
It does not link httpfs' C++ classes (no stable ABI, extensions are `dlopen`'d `RTLD_LOCAL`); it drives
DuckDB's own `HTTPUtil`/`HTTPParams`/`HTTPHeaders` and lets DuckDB autoload httpfs purely as the transport.

## Installation

`hf` is not in DuckDB's signed extension index, so an install names where to fetch it from. A binary is
tied to exactly one DuckDB version identifier and one platform — which are exactly the two directory
names a repository serves. Look both up:

```sql
SELECT version();   -- v2.0.0        <- the identifier, and the directory name a repository serves;
                                     --   a pre-2.0 client answers v2.0.0-alpha47600 instead, and wants
                                     --   a different binary: one directory per identifier is the mapping
PRAGMA platform;    -- linux_arm64         <- one of linux_amd64, linux_arm64, osx_amd64, osx_arm64,
                                            --   windows_amd64, windows_amd64_mingw,
                                            --   linux_amd64_musl, linux_arm64_musl
```

A repository serves **one directory per version identifier**, and a client asks only for its own — so a
client a single build ahead of the last published build 404s, and `INSTALL` surfaces that as an `IO Error`
naming the url it tried. Pin the client whose identifier the tree actually serves:

```bash
pip install duckdb                        # 2.0.0, reports v2.0.0 — binaries for this identifier ship with
                                          #   0.0.2-pre9, published when that tag's CI run is green
pip install duckdb==2.0.0.dev2610151406   # reports v2.0.0-alpha47600 — what the tree serves today, and what
                                          #   0.0.2-pre8 is built for
```

(DuckDB 1.x can't install this at all — `CREATE EXTENSION REPOSITORY` is 2.0 syntax, and nothing is
built for a 1.x identifier; the homebrew `duckdb` CLI is 1.x.)

While the target was an alpha, the identifier moved with nearly every upstream DuckDB build and an extension
build happened once per release, so the moving part was always the client and pinning was the only way to have
a working install. A released identifier does not move — that is the one thing 2.0.0 made genuinely easier.

All eight platforms are published under each identifier the tree carries (`v2.0.0-alpha47600/` today,
`v2.0.0/` from 0.0.2-pre9 on) at `lhoestq/duckdb-hf-extension`: signed at the root, unsigned under
`unsigned/`. Take the signed one — `INSTALL` then checks bytes against a key instead of trusting the CDN.
`windows_amd64` vs `windows_amd64_mingw` is not about which Windows you run: DuckDB composes that name
itself, and `_mingw` appears only when DuckDB was *compiled* with mingw (`#ifdef __MINGW32__`).

### Once per machine

```sql
SET allow_extension_repositories = 'allowed';   -- the gate for adding *and* installing from a repository
CREATE OR REPLACE EXTENSION REPOSITORY hf_repo
  WITH PREFIX 'https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main';
```

The registration is written to `~/.duckdb/extension_repositories/hf_repo.json`, so later sessions find it.
No public key to paste: DuckDB reads it from the repository's
`.well-known/duckdb-extension-repo.json` (a prefix without that file cannot be registered at all, and a
binary modified after signing is refused at `INSTALL` — both verified, *Distributing* in DEVELOPMENT.md). Add
`USING PUBLIC KEY '<base64 DER>'` if you would rather pin the key yourself than take it from the same
response.

### Every session

```sql
LOAD hf FROM hf_repo;
SELECT * FROM hf_ls('hf://datasets/lhoestq/demo1');
```

Three behaviours here are DuckDB 2.0's, not this extension's, and all three were run against the live
repository rather than read out of docs:

* `LOAD` must name the repository it installed from — bare `LOAD hf` looks at the flat
  `…/<version>/<platform>/` path and calls the copy under `…/repositories/hf_repo/` *not found*.
* **an update is `SET` + `INSTALL` again, in the same session.** The registration persists; the gate does
  not. A later session loads fine with `allow_extension_repositories` still `undecided`, while `INSTALL`
  answers *requires allow_extension_repositories='allowed'*.
* nothing autoloads — DuckDB autoloads only names in its own compiled index, so an unqualified `hf_ls`
  before `LOAD` is a `Catalog Error`. Same rule keeps a bare `INSTALL hf` from working until this extension
  is in that index.

For the CLI, put the two setup lines in `~/.duckdbrc` so every session starts with the repository
registered and the gate open. In Python the gate is connect-time config:

```python
import duckdb
con = duckdb.connect(config={"allow_extension_repositories": "allowed"})
con.execute("CREATE OR REPLACE EXTENSION REPOSITORY hf_repo WITH PREFIX "
            "'https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main'")
con.execute("INSTALL hf FROM hf_repo"); con.execute("LOAD hf FROM hf_repo")
```

### The unsigned copy

For clients that can pin nothing, and for locally built binaries (unsigned by definition):

```sql
SET allow_unsigned_extensions = true;   -- in Python this must be connect-time config, not a SET
INSTALL hf FROM 'https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/unsigned';
LOAD hf;
```

No signature is checked on this path, so it is for trying things. It is also refused by default with the
same message a broken signature produces — the two look identical.

## Authentication

Token resolution, same order as `huggingface_hub`: a matching `CREATE SECRET (TYPE HUGGINGFACE, …)` →
`SET hf_token` → `HF_TOKEN` → `HF_TOKEN_PATH` → `$HF_HOME/token` → `~/.cache/huggingface/token`.
Endpoint: `hf_endpoint` → `HF_ENDPOINT` → `https://huggingface.co`.

```sql
CREATE SECRET (TYPE HUGGINGFACE, TOKEN 'hf_...', SCOPE 'hf://datasets/my-org/');
```

## Settings

| setting | default | meaning |
| --- | --- | --- |
| `hf_token` | `` | API token (secrets and env vars take precedence as above) |
| `hf_endpoint` | `https://huggingface.co` | Hub endpoint |
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

## Reads and writes, by repo type

Reads work for all five types, with or without `@revision`:

```sql
SELECT * FROM read_parquet('hf://datasets/wikimedia/wikipedia/20231101.ab/*.parquet');
SELECT content FROM read_text('hf://models/google-bert/bert-base-uncased/config.json');
SELECT * FROM glob('hf://models/nvidia/**/*.safetensors');
SELECT * FROM hf_ls('hf://spaces/dalle-mini/dalle-mini');
SELECT content FROM read_text('hf://kernels/kernels-community/flash-attn3/README.md');
SELECT * FROM read_parquet('hf://buckets/lhoestq/playground/duckdb/*.parquet');
```

Writes (parquet/csv/json, `PARTITION_BY`, overwrite, LFS-tracked files, multipart LFS for big blobs,
create-on-write with `hf_create_repo = true`) work for `datasets`, `models`, `spaces` and `kernels`. A
git-backed repo keeps its `lfsFile` pointer commit — what changes is that the bytes go to the Xet content
store instead of the LFS endpoint, joined by the sha256 the shard carries as a metadata extension. A repo
with Xet switched off still accepts the upload: the token exchange answers either way and the client picks
the route.

A **bucket** file *is* Xet content — no pointer, no branch, no commit — so a bucket write chunks the file,
stores its xorbs, stores the metadata shard that makes them a file, and registers it:

```sql
COPY (SELECT * FROM large_table) TO 'hf://buckets/me/playground/export.parquet' (FORMAT PARQUET);
COPY (SELECT * FROM events) TO 'hf://buckets/me/playground/events/' (FORMAT PARQUET, PARTITION_BY (day));
```

`hf_bucket_copy` is the free route in: it asks the Hub to point a bucket path at content it already holds
(dataset, model, space, kernel or another bucket), moving no bytes through DuckDB, so size does not matter.
One row per source file with the `xet_hash` it was registered under. Two things to know: a file committed
seconds earlier is still being hashed (it waits up to `hash_wait`, `0` to refuse), and a file the Hub keeps
as a plain git blob has no hash to point at, so it comes back `skipped: …` rather than failing the copy.

Writes never delete the previous content: rewriting a path publishes the new file hash, and the chunks the
old version used stay in the CAS — which is what content addressable storage is for.

## Where the rest is

| file | what it answers |
| --- | --- |
| [BENCHMARKS.md](BENCHMARKS.md) | what reading costs, measured against httpfs, and how to re-measure |
| [DEVELOPMENT.md](DEVELOPMENT.md) | the test suites, building from source, publishing an extension repository, source layout |
| [docs/XET.md](docs/XET.md) | the Xet protocol port: hash chain, formats, upload, verification, M9 |
| [CHANGELOG.md](CHANGELOG.md) | what each release changed |
| [STATE.md](STATE.md) | working handover: current position, next steps, constraints |

## Limitations

* **Writes buffer to a local temp file first** (the commit API needs sha256 + size up front; a bucket write
  needs the file on disk to chunk it), and upload + commit — or the Xet publish — happen on `Close()`. One
  commit per file, serialized per repository in a process; concurrent writers rely on the Hub's conflict
  detection. A Xet upload is per file, per statement, so a partitioned write publishes one file at a time.
* **Dedup is strong on content that repeats and weak on content that is merely similar.** An upload queries the CAS
  for its first chunk and for chunks whose hash clears the format's 1-in-1024 rule, at most one query per 256
  chunks, and points at what comes back instead of re-sending it. Measured (`scripts/dedup_measure.py`, 237 MB):
  an identical re-upload costs **0 bytes and 1.4 s instead of 235 MB and 21.5 s**; appending 20 000 lines to a file
  costs **0.2 MB**; adding a column to every row dedups **nothing** (0 of 6889 chunks), because a per-line change
  moves every boundary after it and content-defined chunking has nothing to anchor on. Inserting bytes mid-file keeps
  the prefix and loses the rest. Asking is not the cost: seven queries and no hit changed the wire time per MB by
  less than the run-to-run noise.
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
* Listings come from `/api/{type}/{repo}/tree/...` with `Link` pagination; a folder entry that stops resolving
  mid-glob is treated as empty rather than failing the query. Creating a Space needs an sdk, so create-on-write
  sends `sdk = static` and infers nothing else (private, named after the url).
* Every request goes through DuckDB's `HTTPUtil` and the transport must speak https — DuckDB's built-in client
  ships without TLS in official builds. The extension asks for `httpfs` when the registered transport is the
  placeholder or `Built-In`, and explains what to do when that auto-load is blocked.
* `hf_xet_write`'s fallback cannot be *made* to fail from SQL, so that branch is covered by reading and by the
  explicit `hf_xet_write = false` route; a test-only seam in the upload path seemed worse than the gap.


The long form of any claim in these files — the failed attempts, the measurements behind a decision,
the bug each caveat came from — is in `git log`, whose commit bodies carry ~9 000 words of it, and in
the CHANGELOG's per-release entries.
