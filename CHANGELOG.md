# Changelog

Two version numbers are in play, and they are independent:

* **this project's version** (below) — what the extension can do. `hf_status()` reports it as
  `hf_version`.
* the **DuckDB version identifier** (`v2.0.0-alpha…`) — a loadable extension binary is tied to
  exactly one of these, and a binary built for a different identifier is refused at `LOAD` time.
  `hf_status()` reports it as `duckdb_version`. A release of this extension is therefore one binary
  per DuckDB identifier per platform, and rebuilding for a newer DuckDB pre-release is a rebuild,
  not a version bump.

## 0.0.2-pre3 — installable by someone else, and a git repo written through Xet

* **Writing to a git-backed repository now puts the content in Xet.** `COPY (…) TO
  'hf://datasets/you/thing/data.parquet'` chunks the file, stores its xorbs and its metadata shard, and
  then sends *the same* `lfsFile {path, algo: sha256, oid, size}` commit it always sent — it just stops
  uploading the bytes to the LFS endpoint, because the shard's file entry now carries the sha256 as its
  metadata extension, the one place where the content's xet hash and the sha256 the commit names it by
  are stated together. Proven live on a fresh 6 MB of `/dev/urandom` (a blob no LFS store had ever seen):
  commit accepted, file readable at its url, bytes identical.
* `hf_xet_write` (default on) is the switch, and the plain LFS upload is its fallback rather than a
  second mode: if the content store cannot be reached the bytes go the old way and the commit is still
  the same one, so an unreachable CAS costs a slow write, not a failed one. If the write fails anyway the
  error says the Xet upload had already failed and why.
* That sha256 is parsed as a `DataHash` — four little-endian words, the same convention every other hash
  in the shard uses, *not* 32 plain bytes. The wrong reading still produces 32 bytes and a valid-looking
  shard, so nothing short of a reference vector notices: two of them (`xet_oracle.py`) now pin the
  extension one-segment and multi-segment, and `hf_xet_selftest()` checks the file entry's flags word too,
  since a shard that writes the extension without flagging it (or flags it without writing it) is
  unreadable in a way a byte comparison alone would not name.
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
    version identifier, and every client would have refused to `LOAD` them. The input for that is
    `duckdb_tag`, which is what this release first passed — and which turned out not to work, for a
    reason further down; the working answer is a mirrored DuckDB that carries the tag itself.
* `test/sql/hf_selftest.test` holds the offline reference checks and two url-parser refusals, and it is no
  longer the only place they can run. They could not run where they were: the file opens with `require hf`,
  DuckDB autoloads only extensions on its compiled-in `AUTOLOADABLE_EXTENSIONS` list, a third-party name
  cannot get onto it, so `make test_release` and the matrix's test step both answered `require hf: 1` and
  moved on — and **a skipped sqllogictest file reports success**, which is how a suite of reference
  vectors sat there testing nothing while looking green. `scripts/ci_selftest.sh` now loads a built
  `hf.duckdb_extension` into the duckdb binary that build's own `test-support.tar.gz` ships and compares
  answers and counts itself, and a new `selftest` job runs it on the artifact other jobs produced — the
  bytes a client downloads, verified by something that cannot mistake a skip for a pass. Opening that
  file was worth doing for its own sake: its last line counted `hf_*` functions with
  `LIKE 'hf\_%'`, and DuckDB's `LIKE` takes the backslash literally — it asserted `>= 12` about a query
  that returns 0, in a file no run had ever executed.
* The selftest job repaid being written the first time it ran. It loaded the `linux_amd64` artifact and
  was refused: *built specifically for DuckDB version `d4355d01c5…`* while the shell was `vd4355d01c5…` —
  i.e. the matrix had been green for three runs producing artifacts no client can load, because a build
  that succeeds proves only that compiling worked. The cause is a precedence in DuckDB's Makefile
  (`DUCKDB_VERSION` from the environment beats `OVERRIDE_GIT_DESCRIBE`), fed by extension-ci-tools
  exporting `DUCKDB_VERSION` as the sha given in `duckdb_version`; `duckdb_tag` does create a git tag, and
  nothing downstream reads it. Since `v2.0.0-alpha*` identifiers are generated by DuckDB's release CI and
  tagged nowhere (`git ls-remote --tags origin 'v2.0.0-alpha*'`: empty), CI now clones
  `lhoestq/duckdb` — one commit, tree object identical to upstream `d4355d01c5`, carrying
  `v2.0.0-alpha47600` as its only tag — and passes that tag as `duckdb_version`. A released DuckDB
  retires the arrangement: upstream tags those, and both inputs go back to defaults.
* BLAKE3 is compiled per ISA: NEON on aarch64 (mandatory in ARMv8, so it is autodetected — but
  `blake3_neon.c` has to be in the link for the symbol to exist), SSE2/SSE4.1/AVX2/AVX-512 with
  CPUID-time dispatch on x86-64 GNU/Clang, portable C elsewhere (MSVC on x86-64, where upstream uses
  hand-written `.asm`). Bit-exactness of the SIMD paths is checked by the 325 reference checks on this
  machine and, for the x86 paths, by the selftest job on the built artifact — the only place an x86 SIMD
  disagreement could surface, since nothing here runs x86.
* `scripts/xet_bench.sh` measures the CPU phase of a Xet write without the network, because the answer
  reorders what to optimize next: chunking 100 MB is 31 ms, chunk + hash + xorb is 152 ms (portable
  BLAKE3: 177–193 ms), and the same upload end to end is 12–70 s depending on the link. A 100 MB write
  spends ~1% of its wall clock on CPU, so `hf_xet_write` being on by default makes *sequential xorb
  uploads* the next thing a user feels — not hashing. End-to-end numbers on this link vary 10x
  (LFS vs Xet routes of the same 100 MB measured 6.6 vs 4.3 and 6.9 vs 27.4 s back to back), which is why
  the project quotes the repeatable offline figure and treats single-shot timings as anecdotes.
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
