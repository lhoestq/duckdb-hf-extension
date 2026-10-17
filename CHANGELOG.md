# Changelog

Two version numbers are in play, and they are independent:

* **this project's version** (below) — what the extension can do. `hf_status()` reports it as
  `hf_version`.
* the **DuckDB version identifier** (`v2.0.0-alpha…`) — a loadable extension binary is tied to
  exactly one of these, and a binary built for a different identifier is refused at `LOAD` time.
  `hf_status()` reports it as `duckdb_version`. A release of this extension is therefore one binary
  per DuckDB identifier per platform, and rebuilding for a newer DuckDB pre-release is a rebuild,
  not a version bump.

## 0.0.2-pre1 — the start of Xet

The hashing half of a Xet client, verified against the reference implementation rather than assumed.
Nothing user-visible can upload through Xet yet; what landed is the part every later stage is built on.

* `hf_xet_selftest()` — 152 checks that the extension's Xet hash and chunking agree, byte for byte, with
  `xet-core`: leaf/chunk hashes, file and xorb merkle roots, chunk boundary lists, mean-tree shapes taken
  from xet-core's own correctness table (including 100-node trees and salted file hashes), interior-node
  hashes, and the `hex()`/`base64()` renderings with their round trips. Pure computation, so it needs no
  token and no network. `scripts/smoke_xet.sh` runs it as a suite.
* The reference vectors are generated (`scripts/xet_oracle.py`, writing `src/hf_xet_vectors.cpp`), never
  hand-written, and are committed so CI can check the port without cargo. `scripts/xet_oracle.py --check`
  fails when a newer xet-core disagrees with what is stored.
* Two silent bugs, and both were caught only by the generated vectors: a key constant transcribed
  incorrectly (every hash wrong, all of them plausible), and the chunker's mask built from the low bits of
  the gear hash instead of the high ones — which chunks at almost every byte, and still reproduces the
  reference boundaries on constant input. Neither is visible from a write that "works".
* `hf_bucket_copy(…, hash_wait := n)`: how long a copy waits for the Hub to finish hashing a source file
  committed moments earlier (default 30s, `0` to refuse waiting). Copies of already-hashed files no longer
  pause at all — the wait is decided by what is still missing a hash, not by whether any work remains.

## 0.0.1 — first release

`hf://` behaves like `s3://` for both directions, for every repo type the Hub serves.

### Reading

* `SELECT * FROM 'hf://datasets/…/file.parquet'`, `read_parquet`, `read_csv`, `read_json`,
  `read_blob`, `read_text`, `glob(...)`, `iceberg`-style nested paths — anything that takes a path.
* Repo types `datasets`, `models`, `spaces`, `kernels`, `buckets`; `@revision` on any of them, and
  the short form `hf://user/repo/file` (a model).
* Range reads on the Hub's `/resolve/` endpoints, with a shared per-file window cache:
  grid-aligned read-ahead, windows coalesced when several tasks ask for the same bytes, up to
  `hf_read_max_parallel_requests` range parts per window, retention that outlives the file handle
  under a process-wide budget (`hf_read_cache_total_size`).
* A full scan of a 287 MB parquet file fetches 287412556 bytes — **1.000× amplification** — and
  matches or beats `httpfs` on the same query.
* Buckets are readable directly (`hf://buckets/<user>/<bucket>/…`), recursively listable and globable.
* `hf_ls`, `hf_meta`, `glob`, `**` recursion, ETag/`version_tag` + mtime so DuckDB's own external
  file cache can validate entries.

### Writing

* `COPY (…) TO 'hf://…'` for parquet (incl. zstd), csv, json; overwrite; empty results;
  `PARTITION_BY`; unicode and spaces in paths; nested paths three deep.
* The Hub's own write protocol: `preupload` → git-LFS *basic* or *basic multipart* (above ~10 MB,
  streamed part by part) → ndjson commit. No `hf_xet`, no extra dependency.
* Works for `datasets`, `models`, `spaces` and `kernels` repositories.
* `hf_upload(local_path, hf_url, message := …)` for a file already on disk, `hf_delete(path_or_list,
  missing_ok := true)` for cleanup, per-call and session-wide commit messages (`hf_commit_message`).
* Create-on-write: with `hf_create_repo = true`, the first write to a repository that does not exist
  yet creates it (private; spaces as `sdk = static`) and retries until the Hub will accept the
  commit. A 404 without that setting says so instead of only quoting the Hub.
* Commits invalidate the read cache for the repository they changed, so a `COPY TO` followed by a
  read in the same session sees the new bytes.

### Buckets, without a Xet client

* Reads, listings, globs and deletes work from SQL.
* `hf_bucket_copy(source, target)` moves content **already on the Hub** into a bucket with a
  server-side `copyFile` — single files, folders (structure mirrored), globs, and bucket-to-bucket.
  Files the Hub keeps as plain git blobs (no Xet hash) come back as `skipped` rather than failing
  the copy.
* `COPY … TO 'hf://buckets/…'` and `hf_upload(…, 'hf://buckets/…')` are refused, with a message
  naming the two routes that do work: `hf buckets cp` for local bytes, `hf_bucket_copy` for
  Hub-held content. This is the one write path that genuinely requires Xet chunk hashes.

### Observability and settings

* `hf_status()` (`hf_version`, DuckDB version, endpoint, token source, whoami, HTTP transport, every
  setting), `hf_read_stats(reset := false)` (files opened, hits, coalesced waits, window fetches,
  parts, straddling fetches, evictions, bytes fetched, invalidations, retained bytes/files),
  `hf_read_cache_clear()`, `hf_list_revisions`, `hf_list_refs`, `hf_create_repo`.
* `HF_READ_DEBUG=1` traces window claims, `HF_WRITE_DEBUG=1` traces write attempts and create-on-write.
* 10 settings: `hf_token`, `hf_endpoint`, `hf_cache_dir`, `hf_max_per_page`, `hf_commit_message`,
  `hf_create_repo`, `hf_read_buffer_size`, `hf_read_cache_size`, `hf_read_cache_total_size`,
  `hf_read_max_parallel_requests`. The `huggingface` **secret type** is shared with httpfs, so one
  `CREATE SECRET` covers both.

### Distribution

* `scripts/make_extension_repo.sh` turns a build into a serveable extension repository
  (`<version>/<platform>/…` + `.well-known/duckdb-extension-repo.json`), optionally **signed**.
* `INSTALL hf FROM '<repo prefix>'` verified end to end against a local repository, unsigned and
  signed — with signature required and tampered binaries refused (`scripts/test_install.py`).
* `.github/workflows/build.yml` builds the platform matrix through DuckDB's reusable workflow.

### Tested

`scripts/run_all.sh` runs everything against the live Hub: 140 passing checks plus 7 checks that
assert a refusal says what it should. Suites: read path (byte accounting, cache behaviour),
write path (formats, LFS multipart, create-on-write, delete semantics), model/space/kernel repos
(git vs LFS byte integrity), buckets (server-side copies).

### Known gaps in 0.0.1

* **Xet uploads are not implemented**, which is what `COPY … TO 'hf://buckets/…'` would need; git-LFS
  covers every other write. Client-side chunking + hashing is the subject of 0.0.2.
* Whole-file sequential reads (`read_blob` of a large file) are still behind httpfs, because windows
  are fetched as the cursor reaches them instead of ahead of it.
* Deleting a **repository** is not exposed from SQL: the Hub takes the target as a JSON body on a
  `DELETE`, and DuckDB's HTTP transport sends `DELETE` without a body. Use
  `scripts/delete_repo.sh`, `hf repos delete`, or `huggingface_hub.delete_repo()`; from SQL,
  `hf_delete` removes the files inside a repository.
