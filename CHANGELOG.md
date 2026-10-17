# Changelog

Two version numbers are in play, and they are independent:

* **this project's version** (below) — what the extension can do. `hf_status()` reports it as
  `hf_version`.
* the **DuckDB version identifier** (`v2.0.0-alpha…`) — a loadable extension binary is tied to
  exactly one of these, and a binary built for a different identifier is refused at `LOAD` time.
  `hf_status()` reports it as `duckdb_version`. A release of this extension is therefore one binary
  per DuckDB identifier per platform, and rebuilding for a newer DuckDB pre-release is a rebuild,
  not a version bump.

## Unreleased — installable by someone else

* `INSTALL hf FROM 'https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/unsigned'`
  works, and so does the signed form with a pinned key. `scripts/publish_extension_repo.py` builds the
  tree into a Hub dataset and prints the exact client incantations; it reads the version directory out of
  the binary footer instead of being told it, because a repository whose directory disagrees with the
  footer of what it serves is a repository nobody can install from. `scripts/test_install.py` now takes a
  url as readily as a directory, so what is verified is what a client fetches.
* The CI matrix builds on push. It had been *running* and failing from the start, and the earlier note
  here that Actions was unavailable came from reading a 403 on one endpoint (`gh workflow list`) as a
  verdict on the whole feature instead of opening a job log. Two real causes, both visible only there:
  * DuckDB's CI images configure with a vcpkg toolchain and `VCPKG_MANIFEST_DIR` at the extension root,
    and die at `project()` without a manifest — hence `vcpkg.json` with an empty dependency list, which
    is what the extension actually has (sha256/base64/JSON self-written, blake3/gearhash vendored).
  * nothing tagged the DuckDB checkout, so binaries would have carried the right code and the wrong
    version identifier, and every client would have refused to `LOAD` them; `duckdb_tag` is the input
    that makes `git describe` say `v2.0.0-alpha47600`.
* `test/sql/hf_selftest.test` puts the 296 offline reference checks and two url-parser refusals behind
  `make test_release`, so a build with a broken hash port stops the build rather than the user.
* macOS builds run on tags and manual dispatches rather than every push: this repository is private,
  where macOS minutes bill at 10x, and every job compiles DuckDB from source because prebuilt DuckDB
  archives exist only for released versions.

## 0.0.2-pre2 — a file at the end of the shard

* **`COPY (SELECT …) TO 'hf://buckets/me/playground/x.parquet'` works, and writes through Xet.** A
  bucket file *is* Xet content — no pointer, no branch, no commit — so this is the honest bucket write,
  not a workaround: the temp file is chunked, its xorbs are stored, a metadata shard describing them as
  one file is stored, and the bucket registers the result by hash (`addFile` + `xetHash`). The file is
  readable at the url you wrote, verified `md5`-for-`md5` against what went in, including a 230 MB
  uncompressed file whose chunks are spread over several xorbs (the Hub rebuilds the read ranges from
  the shard). `hf_upload(local, 'hf://buckets/…')` takes the same route.
* `hf_xet_shard.cpp` writes the session metadata shard — hand-rolled little-endian, not protobuf, and
  byte-for-byte identical to what `xet-core` produces for the same content (its uploaded form is the
  shard cut at `file_lookup_offset` with `footer_size` rewritten to 0, which is what makes re-uploads
  dedup: the timestamp lives in the part that is dropped). One file's chunks across seven xorbs is one
  of the pinned vectors. The two coordinate systems a shard uses are documented in `docs/XET.md`: a
  segment indexes chunks **inside its xorb**, while its verification hash covers a **file-wide** range —
  mixing them up is invisible until a file spans xorbs, and the production CAS caught it for us
  (`400 (19) invalid shard failed to verify file`).
* `hf_xet_upload(...)` gained `publish := true` and the columns the full path produces:
  `xet_hash, shard_bytes, shard_hash, shard_result, published`. `xet_hash` is the value the Hub calls a
  `xetHash` — the word-swapped hex of the file hash, confirmed by having the reference chunker and hasher
  reproduce a real file already on the Hub, hash equal to what its tree entry reports.
* A trailing slash on a write target is refused rather than obeyed. `COPY … TO '…/dir/'` used to create a
  file *named* `dir` beside it (url parsing drops the slash — correctly for a read), for buckets and for
  git-backed repos alike; local DuckDB refuses the same statement, so now this does too, with a message
  pointing at `PARTITION_BY`, which writes into a directory without needing the slash.
* `hf_xet_selftest()` pins 296 checks, up from 226.
* `hf_xet_upload(path [, scope := …])` — chunks a file the way Xet does, serializes each xorb, and
  uploads it to the real content addressable storage: the write-token exchange, the CAS-scoped bearer
  token (never alongside the HF token), the reference's retry policy, and its token refresh 30s before
  expiry. One row per xorb: `xorb, num_chunks, bytes, uncompressed_bytes, inserted`, where `inserted`
  is the CAS's own dedup answer — content already stored is not sent twice, and there is nothing to
  check first because a xorb's url *is* its content. `scope` names the repository whose token the bytes
  are uploaded against, for content that is not in a repository at all (a local temp file, which is how
  a `COPY TO` will work). Nothing is readable through this yet: a file exists once a metadata shard
  refers to its chunks, which is the next milestone.
* `scripts/smoke_xet_upload.sh` — a suite against the production CAS, and the only one that can fail if
  the serialization is wrong: the CAS recomputes every xorb hash itself. Fresh content on each run, so
  "was this already stored?" is answered for the right reason; the chunking layout is asserted to be
  deterministic across re-uploads.
* Content past the 64 MiB xorb limit splits into several xorbs. Two bugs hid there, both invisible in a
  small file: `Finish()` reset the chunk list but not the byte counter, so the second xorb of a big file
  was born full; and flushing on "is it full yet" let an object finish one chunk over the limit. The
  builder is now asked before a chunk is added, and the limits are applied to uncompressed bytes — the
  only bytes the hash covers and the only ones the format measures.
* The extension can serialize a xorb — the object the CAS stores — byte for byte identically to the
  reference client: chunk headers, the `XETBLOB`/`XBLBHSH`/`XBLBBND` footer, its section offsets and
  its per-upload nonce. `hf_xet_selftest()` now pins 226 checks, including the whole serialized byte
  stream of seven reference xorbs (empty input, one chunk, many chunks). Chunks are stored
  uncompressed for now, which is a scheme of the format rather than a stub: a xorb hashes over its
  uncompressed contents, so an uncompressed xorb is as valid as a compressed one.

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
