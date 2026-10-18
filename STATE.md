# WORKING STATE — huggingface duckdb extension (`hf://`)

> Living scratchpad. Updated continuously. **Read `PLAN.md` first, then this file, then resume.**

## Current position (2026-10-18, ~18:55 UTC)

**0.0.2-pre7 is published for all eight DuckDB client platforms.** Version **0.0.2-pre7**, HEAD and tag
`v0.0.2-pre7` both at `39706a5`, suites at **200 passed / 12 expected failures / 0 failed** (xet 12,
xet_upload 30, read 55, write 57, models 24, buckets 22), selftest at **367 checks / 0 failed**.

* Live at `https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/` (signed, root) and
  `…/resolve/main/unsigned/`, Hub commit `10716b52` at 18:49 UTC, from CI run **`39153506221`** (green
  18:40 UTC: eight architecture builds + the linux_amd64 reference-checks job).
* Platforms: `linux_amd64`, `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`,
  `windows_amd64`, `windows_amd64_mingw`. The five extra ones build on a `workflow_dispatch` only, and that
  dispatch also passes `reduced_ci_mode: 'disabled'` so one failing cross-compile cannot cancel the
  architectures building beside it.
* **What the new architectures bought: three real bugs in our own sources**, none of them platform API.
  `idx_t` is `uint64_t`, which on Apple's libc++ is `unsigned long long` and therefore not
  `string::size_type`, so values from `std::string::find()` could not bind to the `idx_t &` the JSON helpers
  take (six call sites, invisible on Linux where both spellings are `unsigned long`); the BLAKE3 SIMD family
  was chosen from `CMAKE_SYSTEM_PROCESSOR`, which is the **host** on an Apple cross-build, so an `osx_amd64`
  build running on an arm64 runner compiled `blake3_neon.c` for x86-64 — and the Linux version of that
  mistake would have shipped a wrong-SIMD binary rather than failing; MSVC refused two zero-size arrays plus
  fourteen files that used `memcpy`/`strlen`/`snprintf`/`getenv` without naming
  `<cstring>`/`<cstdio>`/`<cstdlib>`/`<cstdint>`.
* **Verification, and its limit**: all sixteen served binaries (eight platforms × plain/gzipped,
  signed/unsigned trees) were re-downloaded and confirmed byte-identical to CI's bytes, every footer reading
  `v2.0.0-alpha47600` + `v0.0.2-pre7`; `INSTALL`/`LOAD` then ran for real over https on this machine's own
  platform, signed and unsigned, with the tamper check refusing in both registration forms (16 files
  corrupted). macOS, both Windows flavours and both musl builds are covered by **byte comparison only** —
  this machine cannot execute them, and a footer plus a sha256 is not a load test.
* `## Installation` in the README documents the client-facing shape of it, including the easy one to get
  wrong: `windows_amd64` vs `windows_amd64_mingw` is decided by how **DuckDB** was compiled
  (`#ifdef __MINGW32__` in `platform.hpp`), not by which Windows you are on — so a client that is not
  mingw-built never asks for the mingw directory, which is the directory pre6 published and why pre6 was
  unreachable from a stock Windows DuckDB.

## Position before this (2026-10-18, ~16:25 UTC)

**0.0.2-pre6 is published for three platforms, and 0.0.2-pre7 is building for every architecture DuckDB
has a runner for.** Version **0.0.2-pre7**, suites at **200 passed / 12 expected failures / 0 failed**
(xet 12, xet_upload 30, read 55, write 57, models 24, buckets 22), selftest at **367 checks**. pre7 changes
reach and documentation rather than the extension itself:

* The build matrix is shaped by the event: a push builds `linux_amd64`, `linux_arm64`,
  `windows_amd64_mingw`; a `workflow_dispatch` adds `osx_amd64`, `osx_arm64`, `linux_amd64_musl`,
  `linux_arm64_musl`, `windows_amd64`. **`opt_in_archs` beats `exclude_archs`**, contrary to that input's
  own description - the first push queued two musl builds that `exclude_archs` named explicitly, so the
  opt-in is now conditional on the event (`2028782`), and the push run `39144427990` is the check that it
  holds.
* **`windows_amd64` was missing in a way that mattered, not just absent.** DuckDB composes the platform
  string that names the directory a client installs from, and the `_mingw` postfix is applied
  `#ifdef __MINGW32__` (`src/include/duckdb/common/platform.hpp`). A client that is not mingw-built asks
  for `…/windows_amd64/`; pre6 put no file there, so its mingw artifact could only ever serve a
  mingw-built DuckDB.
* **A `## Installation` section** at the top of the README, written by running each step against the live
  repository: registration persists in `~/.duckdb/extension_repositories/<name>.json` (once per machine,
  although the `allow_extension_repositories` gate is per-session); the public key is discovered from the
  repository's `.well-known/duckdb-extension-repo.json`, and a prefix without one cannot be registered at
  all; a repository-installed extension loads as `LOAD hf FROM hf_repo` because bare `LOAD hf` looks at the
  flat path and calls the copy under `repositories/hf_repo/` "not found"; **nothing autoloads** for a name
  outside DuckDB's compiled-in index - the same rule that keeps a bare `INSTALL hf` from working;
  `SELECT version()` and `PRAGMA platform` are the two directory names; and `allow_unsigned_extensions` is
  connect-time in Python, not a `SET`.
* `test_install.py` now demands the tamper refusal from **both** registration forms - key pinned, and
  keyless (`bfa3556`) - because the README recommends the keyless one.

**In flight:** dispatch run `39144168208` on `dbd4d52` = tag `v0.0.2-pre7`, which is the run whose artifacts
become the release tree (musl and windows_amd64 ride along under `allow_failure_archs`, so a failing
cross-compile leaves its platform directory missing instead of sinking the run).

* **Why this after concurrency**: it is the other multiplier on the thing that measures slow. Measured
  offline with `tools/xet_probe` as the reference writer over a 2,103,723 byte CSV: none 2,105,403,
  lz4 881,602, bg4 1,148,898 (worse), auto 881,602 → **0.42x on the wire**, bigger than M7's 1.7x and
  multiplicative with it. Our own writer lands at 889,917 (0.9% above the reference's frame bytes, which
  is free: conforming LZ4 encoders need not agree, since each chunk is decoded from its own scheme byte +
  length).
* **The load-bearing fact**: an xorb's hash is the merkle root of the **uncompressed** chunk hashes, so
  all four schemes produced the same `xorb_hex` (`9f7738bc…b16489`). Codec is a wire decision. Live proof
  in `smoke_xet_upload.sql`: write with lz4, re-upload with `hf_xet_compression='none'` → every xorb
  `inserted = false`, from the CAS, for no new storage.
* **The shard did not move.** `chunk_byte_range_start` is cumulative *unpacked* length (xet-core books
  `unpacked_segment_bytes`; `memory_client` accumulates uncompressed sizes), so readers ask in the space
  compression happens under. LZ4 touched xorb serialization and nothing else - which is also why 325
  checks became 367 rather than why they were rewritten.
* **Decodability is the test surface now**, in four places: `AddChunk` decompresses its own frame and
  compares against the bytes it hashed before any POST; the selftest pins hashes/counts/unpacked-totals/
  footer-placement for LZ4 builds (frame bytes deliberately *not* pinned); `xet-probe xorb-read` - xet-core
  deserializing - reproduces chunk hashes and the xorb hash of a file **our** writer dumped
  (`scripts/xet_bench.sh <file> lz4 <dir>`, which is what left those files on disk); and the live re-upload.
* **Costs, stated plainly**: CPU phase ~690 → ~370 MB/s (compressible) / ~520 MB/s (random), i.e. ~1% of a
  big write's wall clock → ~2%; incompressible chunks grow ~16 B (1.001x over 5 MB random) because
  reference clients compress *every* chunk and store incompressible ones raw inside a frame. No per-chunk
  "skip" heuristic, deliberately. bg4 not implemented (needs `bg4_split` + KL predictor, and it lost on
  this data); `Auto` still refused in the ctor rather than guessed.
* **Build hygiene**: `XXH_NAMESPACE=LZ4_` + `-fvisibility=hidden` + emptying lz4's own
  `LZ4LIB_VISIBILITY`/`LZ4FLIB_VISIBILITY` macros → the extension exports **0** `LZ4_*`/`XXH*` symbols.
  DuckDB's own LZ4/xxhash are C++-namespaced, so nothing was colliding; `hf_library` deliberately keeps its
  default visibility (it holds the entry points).
* One live assertion had to change meaning, and it was the suite doing its job: `assert_serialized_is_larger`
  (`bytes > uncompressed_bytes`) is false once chunks compress, so it became `sum(bytes) <
  sum(uncompressed_bytes)` under LZ4 and the framing statement moved to the `hf_xet_compression='none'`
  block, where it is still true.
* **`hf_xet_compression` is checked where it is set** (after the pre5 tag): anything but `lz4`/`none` -
  including `bg4`, which the format defines and this build does not implement, and `auto`, whose meaning in
  the reference client is a per-chunk decision this writer never takes - now throws at the `SET` instead of
  on the next `COPY`. Uses the set-callback `hf_endpoint`/`hf_cache_dir` already register, so it is the
  file's own pattern; `ChunkCompression` keeps its check as the last line before bytes are made.
  `smoke_xet.sql` pins the three refusals, `'LZ4'` normalising, and `RESET` restoring → xet 4 → 12 passed,
  3 expected failures.
* **A packaging bug found by assembling pre4's artifacts, before publishing anything**: the repository
  script took the DuckDB version from `strings | grep '^v[0-9]'`, and the windows artifact matched a
  mingw fragment `v0.1H` → its 73 MB binary was filed under a directory no client asks for, with a green
  CI run and a successful-looking script. Now `scripts/ext_footer.py` reads the footer as the structure
  `append_metadata.cmake` defines (534 bytes: `duckdb_signature` marker, 8×32 NUL-padded fields in reverse
  order, 256-byte signature slot), `make_extension_repo.sh` cross-checks `DUCKDB_PLATFORM`/`DUCKDB_VERSION`
  against the binary, and `publish_extension_repo.py` verifies every binary's footer against its directory
  *before* any network call. Re-assembling pre4 filed all three platforms correctly afterwards.
* **And the signature gate was passing without testing anything** (found while assembling the signed tree,
  after the pre6 tag): `test_install.py`'s tamper check corrupted `os.listdir(...)[0]`, which on a
  three-platform tree is `linux_amd64` while the installing machine is `linux_arm64` - the served bytes were
  untouched - and its pass condition (`child exited nonzero`) was satisfied by the child's *success* path
  too, since `SystemExit(<str>)` exits 1. The line printed as "refused" was the child's own "LOADED DESPITE
  TAMPERING". Now all platform dirs and both file forms are corrupted, acceptance exits 0, and a refusal
  only counts when it mentions the signature; verified in both directions (6 files corrupted → `INSTALL`
  refused with "doesn't have a valid signature"; untampered → child exits 0). `signature -> verified
  against` was also an overclaim about a pinned-key comparison and now says `trusted key -> repository
  pins`. Test-only: the pre6 artifacts still come from the tagged commit.
* **Distribution is no longer the next thing to do** - it is done, for three platforms, and
  `README` "Cutting a release" now documents how (including the trap that `git push --follow-tags` carries
  `main` and silently skips a **lightweight** tag, which is why the first pre6 artifacts carried a commit
  sha in `duckdb_extensions().extension_version`, and why tags here are annotated and pushed explicitly).
* Next: the **upload-side dedup query** (`POST /v1/chunks/…` before hashing work another repository already
  paid for), then **shard-driven read prefetch** (walk a shard's chunk table instead of one range request
  per window). macOS artifacts remain an open decision: they need a `workflow_dispatch`, and private-repo
  macOS minutes cost 10x.

## Position before this (2026-10-18, ~11:25 UTC)

**M7: a file's xorbs go out over several streams, and CI is green all the way down to loading the
artifact.** Version **0.0.2-pre4**, suites at **189 passed / 9 expected failures / 0 failed** (xet 4,
xet_upload 27, read 55, write 57, models 24, buckets 22). `docs/XET.md` has the M7 write-up.

* **Why concurrency and not LZ4 first: the measurement.** `scripts/xet_bench.sh` (in the repo, links the
  objects a `make release` produced) says 100 MB costs 31 ms to chunk and 152 ms to chunk+hash+serialize,
  against 12-70 s of wall clock to upload. So CPU is ~1% of a write. Six interleaved 200 MB runs:
  concurrency 1 = 29.8/57.9/31.2 s, 3 = 17.6/20.9/18.0 s, 4 = 22.2/19.4 s → default **3** (`hf_write_max_parallel_uploads`),
  the point where it flattened and the cheaper memory bill. Not 3x: a 200 MB file is 64/64/64/8 MiB xorbs,
  and the tail is exposed either way.
* **The design constraint, because it is the part that could be subtly wrong**: the shard is order
  sensitive, uploads are not. The reading thread builds each xorb and writes down everything the shard will
  say about it *in file order*, then hands bytes to the pool; only `inserted` comes back, and it is
  collected after every POST lands (`uploader.Finish()`), before the shard is serialized. In-flight cap =
  memory cap = backpressure, all one number: 3 × up-to-64 MiB, and a 10 GB file therefore costs what a
  100 MB one does.
* **Thread-affinity, learned from the read path rather than rediscovered**: workers get a CAS client with
  no `FileOpener` (`HFXetCasClient::ForWorkerThread`) - a `ClientContext`-backed opener reads settings
  through its own client, and `FetchWindow` in `hf_read.cpp` already refuses to share one across threads.
  The token exchange is done on the main thread before any worker exists. The shared token is refreshed
  under a mutex and *copied out* (`Authorize(cas_url, access_token)`): the old `CasUrl()` returned a
  `const string&` into state another thread's refresh could rewrite mid-header.
* **p6 is the cheap test worth having**: write a multi-xorb file at concurrency 3, re-upload it at 1, and
  require every xorb to answer `inserted = false` and the shard `exists`. Xorb urls are content hashes, so
  any reordering/boundary bug shows up as `inserted = true`. Costs no new storage, so it can live in the
  live suite. Still uncovered: a failure *during* a concurrent upload (as with the LFS fallback, no SQL
  lever, and the test seam would be worse than the gap).
* **CI run `39077156438` is the first fully green one in the strongest sense**: four platforms built *and*
  the self-test job reporting `hf_version|duckdb_version|hf_xet_write = 0.0.2-pre3|v2.0.0-alpha47600|true`
  plus `325 reference checks, every one of them passing` **off the downloaded artifact** - so the
  mirror-based version identifier works, and the amd64 AVX/SSE BLAKE3 paths are now executed somewhere
  that actually runs x86.
* Next ("C", by value): **publish pre4 from CI's own artifacts** (the first time linux_amd64 and
  windows_amd64_mingw binaries can go out at all), then LZ4 frames, then the upload-side dedup query,
  then shard-driven read prefetch.

## Position before this (2026-10-17, ~21:40 UTC)

**pre3 is published, swept, and measured - and its CI was fixed by the job written to catch it.** The
Hub repository serves 0.0.2-pre3 (`lhoestq/duckdb-hf-extension`, signed at `/`, unsigned at `/unsigned/`),
both forms re-verified over https with a real query after `LOAD`, installed binary reporting
`hf_version = 0.0.2-pre3`. Live probes are swept (the 6 MB `/dev/urandom` file and an unreferenced csv out
of the staging repo, the 4 MB file out of `lhoestq/hf-ext-nonxet-probe` - the repo stays, `docs/XET.md`
cites the observation made with it).

* **The `selftest` job paid for itself on its first run.** The matrix was green on all three built
  platforms; the job loaded the `linux_amd64` artifact and was refused: *built specifically for DuckDB
  version `d4355d01c593…`*, this DuckDB is `vd4355d01c593…`. A build succeeding proves compiling worked,
  not that the artifact is loadable — three runs of green had produced artifacts no client can install.
* **Why, since it is worth exactly one paragraph**: DuckDB's Makefile takes its version override in the
  order `DUCKDB_VERSION` → `OVERRIDE_GIT_DESCRIBE` → `DUCKDB_EXPLICIT_VERSION`, extension-ci-tools exports
  `DUCKDB_VERSION` = the `duckdb_version` input (the sha) into the whole job, and
  `append_extension_metadata.py` writes the footer's `duckdb_version` from that same variable. So
  `duckdb_tag`, which *does* run `cd duckdb && git tag v2.0.0-alpha47600` (in the job log), is ignored
  downstream. And no `v2.0.0-alpha*` tag exists upstream to describe from: `git ls-remote --tags origin
  'v2.0.0-alpha*'` is empty, because that identifier is generated by DuckDB's release CI.
* **The fix is a repository**: <https://github.com/lhoestq/duckdb> — one commit whose **tree object is
  upstream's** (`9a1fc9adb2d5b1a34c08c978af7ff18d5016835e`, i.e. "DuckDB and nothing else" is checkable,
  not asserted) carrying `v2.0.0-alpha47600` as its only tag. CI passes `override_duckdb_repository` +
  `duckdb_version: v2.0.0-alpha47600`, verified client-side first: a fresh clone of it `git describe`s to
  exactly the identifier. **`duckdb_tag` must stay unset**: it runs a plain `git tag` on a tag that now
  exists, and `make` failing on "tag already exists" is how this comment earns its keep. A *released*
  DuckDB retires both inputs.
* **BLAKE3 is now per-ISA** (NEON on aarch64 — needs `blake3_neon.c` in the link, since NEON is
  autodetected rather than dispatched; SSE2→AVX-512 with CPUID dispatch on x86-64 GNU/Clang; portable C on
  MSVC/other). The 325 checks pass through the NEON path here; the x86 SIMD paths are verified on the
  *artifact* by the same selftest job, which is the only place an x86 disagreement could appear.
* **And the measurement that reorders the plan** — `scripts/xet_bench.sh`, 100 MB of `/dev/urandom`:
  chunking 31 ms (3.4 GB/s), chunk+keyed-hash+xorb 152 ms NEON / 177–193 ms portable, same upload end to
  end **12–70 s** depending on the link. ~1% of a big write's wall clock is CPU, so with
  `hf_xet_write = true` as the default the next thing a user feels is the **sequential xorb POSTs**,
  not hashing. Single-shot end-to-end timings here vary 10x (LFS vs Xet of the same 100 MB: 6.6 vs 4.3 s,
  then 6.9 vs 27.4 s) — treat them as anecdotes, quote the offline number.
* **Publishing note**: an `INSTALL` seconds after a publish can fail with a bare *Failed to install remote
  extension* — the dataset is Xet-stored and the `resolve` redirect for a just-committed file is still
  settling. Both trees verified green on re-run.
* Next ("C", by value): **parallel xorb uploads** (measured reason above), upload-side dedup query, LZ4
  frames (provable offline as bytes-on-wire), shard-driven read prefetch, robustness negatives. (the
  uploads part is done above)

## Position before this (2026-10-17, ~19:45 UTC)

**M6 is in: a git-backed repository's file content now goes through Xet.** `COPY (…) TO
'hf://datasets/…'` chunk-stores-then-commits: xorbs + shard to the CAS, and the *same*
`{"key":"lfsFile","value":{path, algo:sha256, oid, size}}` commit as before. The link between the two hash
systems is the shard's file-level metadata extension (`flags | WITH_METADATA_EXT` + 32-byte sha256), which
`huggingface_hub` only omits for buckets. New setting **`hf_xet_write`, default `true`**; a CAS failure
falls back to `UploadLFSBlob` with an unchanged commit, and if the commit then fails the message names the
earlier Xet reason. Version bumped to **0.0.2-pre3**, selftest now **325 checks**. `docs/XET.md` has the
M6 write-up.

* **The hex trap again, in a new place**: the sha256 in that extension is parsed as a `DataHash` - four
  16-hex-digit groups read as little-endian words, word-swapped, exactly like `MerkleHash::from_hex`. The
  naive byte-pair parse writes 32 plausible bytes that resolve to nothing. Use
  `HFXetHash::FromHex(…).bytes`; two oracle vectors (single- and multi-segment) plus a flags-word assertion
  are what notice.
* **There is no repo capability to preflight.** `GET api/<type>/<repo>/xet-write-token/<revision>` answers
  with a valid CAS token for datasets/models/spaces, and also for a repo created `"xetEnabled": false`
  (probe: `lhoestq/hf-ext-nonxet-probe`, whose info reports `xetEnabled: None`). Preupload says
  `uploadMode: "lfs"` even for Xet-enabled repos. The client picks the route, as `hf_xet` does.
* **Live proof method worth keeping**: content that could not possibly exist in an LFS store (6 MB of
  `/dev/urandom`), written with `hf_xet_write = true`, committed, then read back and compared by
  `read_blob` sha256 + size. The tree API exposes no usable `xetHash` for git-backed files, so that is the
  strongest signal the surface gives.
* **CI fact worth never relearning**: the matrix's test step discovers `test/sql/hf_selftest.test` and
  *skips* it - `require hf` is only satisfiable by extensions in DuckDB's hardcoded
  `AUTOLOADABLE_EXTENSIONS` (`src/include/duckdb/main/extension_entries.hpp`), and a skipped file reports
  success. `duckdb_extension_statically_link(hf)` does **not** help (tried: it puts 815 HF symbols into
  libduckdb/shell/unittest, and `CheckRequire` still consults the list, not the static loader), and a bare
  `LOAD hf` line inside a `.test` file is parsed as sqllogictest's `load` *command*, which skips the whole
  file. Both reverted. What runs the checks instead: **`scripts/ci_selftest.sh`** + a `selftest` job that
  downloads the `linux_amd64` artifact and uses the `duckdb` binary that artifact ships in
  `test-support.tar.gz` (`LD_LIBRARY_PATH=…/build/release/src`). It compares numbers: version, check count
  (read out of the `.test` file, not repeated), `bool_and(assert_passed)`, a parser refusal, a directory-write
  refusal (needs parquet - say so out loud when absent), registered function count. It found that the
  skipped file's own last assertion (`LIKE 'hf\_%'`, and LIKE takes the backslash literally) returned 0.
* **CI matrix state** (run `39052885130`, and `39063970030` after M6): generate-matrix, `linux_amd64`,
  `linux_arm64`, `windows_amd64_mingw` all green, artifacts uploaded; macOS only on tag/dispatch. "Green"
  meant *compiled* until the selftest job started loading the result - see above. `linux_arm64` prints
  "Tests are not supported for linux_arm64" and its artifact carries no `test-support.tar.gz`, so arm64
  stays locally tested here.

## Position before this (2026-10-17, ~15:10 UTC)

**M5 is in: `COPY (SELECT …) TO 'hf://buckets/…'` writes through Xet end to end.** The shard writer
(`src/hf_xet_shard.cpp`) produces the session shard byte-for-byte like `xet-core`, `HFXetCasClient::UploadShard`
posts it to `POST {cas}/v1/shards`, and `HFAddFilesToBucket` registers the result with the bucket's batch
`addFile` + `xetHash`. `hf_xet_upload(..., publish := true)` is the same route by hand; `hf_upload` and the
write handle take it too. `scripts/run_all.sh`: xet 4, xet_upload 24 (+1), read 55, write 45 (+6), models 24,
buckets 22 (+2) — all green. Selftest 296 checks. Committed as `873b0a2`, tagged `v0.0.2-pre2`.

* **The two coordinate systems, since this is the trap**: a file segment's serialized
  `chunk_index_start/end` are **xorb-local** (the reader resolves the xorb by hash, then slices that xorb's
  chunk list), while the segment's *verification hash* is a keyed blake3 over a **file-global** range of the
  file's chunk list. `HFXetShardSegment` therefore carries both (`chunk_index_*` serialized,
  `file_chunk_*` not). Single-xorb files cannot tell the difference; the production CAS caught it on the
  first multi-xorb upload (`HTTP 400 (19) invalid shard failed to verify file`).
* **`xetHash` = `MerkleHash::hex()`, the word-swapped hex** — confirmed by reproducing a real Hub file
  (`xet_probe.parquet` → `1b14d820…444d9f`, equal to the tree API's `xetHash`), not inferred.
* Bucket `addFile` needs the **repository-relative** path: HFUrl keeps the leading slash, the Hub rejects it
  (`422 Path must not start with '/'`), so `PathInRepo()` strips it. Buckets upload with `skip_sha256`, so no
  `metadata_ext` — a bucket has no LFS pointer to link.
* The uploaded shard is the local shard **truncated at `file_lookup_offset`** with the header's
  `footer_size` rewritten to 0 (`read_shard_to_bytes_remove_footer`); the timestamp is in the dropped part, so
  identical content dedups (`shard_result = exists`). CAS-reported `shard_hash` is plain hex and is the hash
  of *its* re-serialization — reported verbatim, never compared locally.
* **Fixed a pre-existing bug found by the new bucket test**: `COPY … TO '…/dir/'` created a file named `dir`
  (URL parsing drops the trailing slash — right for reads) for git-backed repos too. Local DuckDB refuses it,
  so `HFFileSystem::OpenFile` now refuses it on the raw path, and `RequireFileName()` covers the resolved path.
* Removed `HFRefuseBucketWrite` (dead guard from when buckets were refused).
* Next (M6): git-backed repos through Xet via the `metadata_ext` sha256 link; then upload concurrency, an
  upload-side dedup query, LZ4 frames. Details in `docs/XET.md`.

## Position before this (2026-10-17, ~11:55 UTC)

**Milestone: M4, the CAS client, is in and works against the production Hub. `hf_xet_upload()` chunks a
file, serializes each xorb (M3) and stores it in the real CAS - token exchange, retries, splitting, and
the CAS's own dedup answer. `scripts/run_all.sh`: xet 4, xet_upload 8 (+1 refusal), read 55, write 45
(+5), models 24, buckets 18 (+3). Nothing is readable through Xet yet: content becomes a file when a
metadata shard names its chunks, which is M5.**

* `src/hf_cas.{hpp,cpp}`: `HFXetCasClient` (write-token exchange + cached token refreshed 30s before
  expiry, `UploadXorb` with the reference's retry policy), `HFXetUploadFileContents` (read a file, chunk
  it, fill a builder, upload each finished xorb). The token route is *derived* from the url the user
  typed - `api/{repo_type}/{repo_id}/xet-write-token`, with a `/{revision}` segment for git-backed repo
  types and none for buckets; both were confirmed against the live API, and a bucket-scoped token uploads
  exactly like a dataset-scoped one.
* The CAS call carries the CAS token *and nothing else*: `HFClient::Post` grew the same `with_auth=false`
  switch `Put` has, for the same reason presigned LFS PUTs do not take the HF token.
* No exists-check exists. The xorb url is its content, so re-posting is idempotent and
  `{"was_inserted": false}` is the dedup signal. Getting this parsed right needed
  `HFJson::GetRaw()` - `GetString` only matches *quoted* values, so a `true` or a unix timestamp read as
  "absent": every upload reported `inserted: true`, and an `exp` of 0 would have refreshed the token on
  every call. A number or boolean silently defaulting is the failure mode this whole milestone kept
  hitting; the fix is a scalar-aware reader, not a default.
* `hf_xet_upload(path [, scope := 'hf://...'])`, one row per xorb:
  `xorb, num_chunks, bytes, uncompressed_bytes, inserted`. `scope` is the repository whose token the
  bytes are uploaded against - required when the content is a local file, which is precisely what a
  `COPY TO` will hand over: DuckDB fills a temp file, the target repo supplies the scope. M5 attaches to
  that seam.
* The production CAS is an independent reader of M3's output, and it complains when the bytes disagree:
  a deliberately wrong hash came back `400 BadRequest: xorb hash mismatch: expected fc79…, got 4bf3…`.
  Every accepted upload therefore re-proves the serialization from the other side of the wire.
* The first file over the 64 MiB xorb limit found two bugs that a small file cannot see: `Finish()`
  cleared the chunk list but not `uncompressed_bytes`, so a file's *second* xorb was born full and threw
  `xorb is full after 0 chunks`; and flushing on `IsFull()` - asking after the fact - let an object end
  one chunk past the limit. `WouldOverflow(len)` is asked before adding, `Finish()` resets everything, and
  the function reports both byte counts, because the format's limits are on *uncompressed* bytes
  (1026 chunks / 67,076,687 raw bytes is legal at 67,126,031 serialized).
* `scripts/smoke_xet_upload.{sql,sh}` - a new suite, wired into `run_all.sh`, against the production CAS.
  Its fixture is random on every run because half of what it asks is "was this already stored?", and a
  deterministic fixture would answer that for the wrong reason forever. It also asserts the chunking
  layout is identical across re-uploads.

## Position before this (2026-10-17, ~10:55 UTC)


**Milestone: M3, the xorb writer, is in and byte-exact. `hf_xet_selftest()` is 226 checks (was 152),
all passing; the extension's serialized xorbs are bit-identical to what xet-core's writer produces,
including the empty-input case. Tagged `v0.0.2-pre1` before this; nothing uploads through Xet yet -
M4 (CAS client) is next.**

* `src/hf_xet_xorb.cpp`: `HFXetXorbBuilder` accumulates chunks (hashing them with M1's leaf hash as
  they arrive), tracks the footer's physical and unpacked boundary lists, and `Finish()` writes the V1
  footer - `XETBLOB`+version+xorb hash, `XBLBHSH` hash section, `XBLBBND` boundaries, three copies of
  `num_chunks`, the two section offsets, the 16-byte reserved buffer (first 4 = per-upload nonce,
  excluded from the hash by design), and the footer length. Little-endian throughout, 3-byte lengths in
  the 8-byte chunk header. Limits are uncompressed bytes (64 MiB) and chunk count (8192), asserted in
  `AddChunk`, because the CAS names a xorb by its chunk hash and never sees our layout.
* Verification for a writer needs a reader, and the format's own reader is the one that matters:
  `tools/xet_probe` gained `xorb <scheme> <out>` (chunk + serialize with the reference, using
  xet-core's own `xorb_format_test_utils`) and `xorb-read <path>` (parse a xorb back with
  `XorbObject::deserialize`, follow its footer offsets, recompute every chunk hash from the bytes the
  footer points at). The probe now depends on `xet-data` as well as `xet-core-structures`; still
  dev-only, `.gitignore`d build output.
* Byte-exactness is pinned by fingerprint: each xorb vector stores `HFXetChunkHash` over the *entire*
  serialized object, so a footer byte out of place fails a check instead of quietly producing a xorb
  that hashes right and cannot be read. The self-test also follows the offsets we write and asserts the
  section identifiers sit where the footer claims - the reader's view, not the writer's arithmetic.
  Two failures on the first run were both in the test, not the writer (a footer length that excludes
  its own u32, and comparing each section's ident against `XETBLOB`); the fingerprints passed first.
* Compression stays `None`: legal, and honest. LZ4 needs the *frame* format (DuckDB vendors LZ4 block
  only), and its output does not have to match `lz4_flex` byte for byte - the hash covers uncompressed
  chunk data, so any valid frame is fine as long as the reference reader recovers the bytes. Both LZ4
  and BG4 are upload-size work, after the write path exists. The constructor refuses other schemes
  with a pointer to `docs/XET.md` rather than emitting bytes the Hub cannot decode.
* Also this session, before the tag: `hf_bucket_copy(..., hash_wait := n)` for the hub's async hashing,
  and `v0.0.2-pre1` tagged (extension version string, write-suite assertion, CHANGELOG entry).

## Previous position (2026-10-17, ~09:40 UTC)

**Milestone: Xet M1 + M2 are in and byte-exact. `hf_xet_selftest()` is 152 checks against vectors
generated from xet-core, all green, and `scripts/smoke_xet.sh` runs them as a fifth suite (`run_all.sh`:
xet 4 + read 55 + write 45 +5 refusals + models 24 + buckets 18 +3 refusals). Bucket copies got a
`hash_wait` parameter for the hashing race. Nothing uploads through Xet yet - M3 (xorb format) is next.**

* What the port is: `src/hf_xet_hash.cpp` (BLAKE3 keyed leaf/interior hashes, `hex()`/`base64()`, the mean
  tree, file/xorb/salted-file hashes), `src/hf_xet_chunk.cpp` (streaming gear-hash chunker, 64 KiB target /
  8 KiB min / 128 KiB max), `third_party/blake3` (vendored C, all SIMD disabled so all six CI platforms
  build one way), `third_party/gearhash/gearhash_table.hpp` (generated from the crate's own table).
* Verification is generated, never hand-written: `scripts/xet_oracle.py` runs xet-core's `chunk`/`hash`
  examples plus a small Rust probe (`tools/xet_probe`) and writes `src/hf_xet_vectors.cpp` - leaf/file/xorb
  hashes and chunk-size lists for 8 reproducible inputs, 36 mean-tree vectors lifted from xet-core's own
  `test_correctness` table, interior-node hashes, hex/base64 renderings. `--check` re-derives and diffs.
  Inputs are specs (literal, repeated pattern, splitmix64), not blobs, so vectors are machine-portable.
* Two bugs the vectors caught, both silent: the DATA/INTERNAL key constants were transcribed wrong (every
  hash wrong, all plausible), and the chunk mask was built over the gear hash's *low* bits instead of
  `mask << mask.leading_zeros()` - that chunks nearly every byte and *still* matches the reference on
  constant and tiny inputs, so only the splitmix vectors expose it. Both are written up in `docs/XET.md`.
* The oracle needs cargo and a `xet-core` checkout, and only to regenerate vectors; the vectors are
  committed, so CI and the smoke suite need neither. `cargo build --release -p xet-data --examples` - the
  `hash` example panics in debug (clap asserts on the `-h` collision).
* `hf_bucket_copy(…, hash_wait := n)`: was sleeping on every call because the pending set was pruned only
  after the first sleep; now the wait is bounded by a time budget, decided by what still lacks a hash, and
  configurable (default 30s, 0 disables). This is what the buckets suite flakiness really needed.

## Previous position (2026-10-16, ~23:14 UTC) — 0.0.1

**Milestone: 0.0.1 is packaged. All four live suites are green (`scripts/run_all.sh`: read 55, write 45 +5
expected refusals, models 24, buckets 16 +2 expected refusals), the extension reports its own version
(`hf_status().hf_version = 0.0.1`), and `CHANGELOG.md` + the README testing section describe exactly what
that binary does. Phase 1 of the two-phase task is done; Phase 2 (client-side Xet chunking/hashing,
against `/home/quentin/hf/xet-core`) is what 0.0.2 is for.**

* Built `dist/hf.duckdb_extension` for `v2.0.0-alpha47600` (`OVERRIDE_GIT_DESCRIBE=v2.0.0-alpha47600`),
  loads in the Python pre-release with `allow_unsigned_extensions`.
* Live smoke suites after this session: `read` 55, `write` 45 (+5 intended refusals), `models` 24,
  `buckets` 16 (+2 intended refusals), 0 failures. `scripts/try.py` now grades two ways — any column named
  `assert*` must be TRUE, and a `-- expect-fail: <substring>` marker above a statement means it must raise
  with that text — so refusals are covered by tests instead of by comments.
* What bucket writes needed, and what they still cannot do:
  * `addFile` on the bucket batch API needs a `xetHash` (probed: `sha256`, no hash, and inline `contents`
    are all rejected), and there is no plain `PUT /buckets/.../resolve/...` route;
  * but `POST /api/{repo_type}/{repo}/paths-info/{revision}` with `{"blobs":true,"lfs":true,"xet":true}`
    hands back the Hub's own `xetHash` for a file, and the bucket batch API accepts
    `{"type":"copyFile","path":...,"xetHash":...,"sourceRepoType":...,"sourceRepoId":...}` - that is the
    whole trick, and it works from datasets, models, spaces, kernels *and* other buckets;
  * so `COPY TO 'hf://buckets/...'` stays refused (bytes from a local disk still mean `hf buckets cp`), and
    a file the Hub keeps as a plain git blob (no Xet hash, typically small files) is reported as
    `skipped: ...` per row instead of failing the copy;
  * a file committed seconds earlier may not be hashed yet, so the copy re-asks up to 4 times with a
    doubling backoff (250 ms..2 s) before believing a file is outside Xet.
* What changed in the read path (all of it in `src/hf_read.cpp`, described in `README.md`):
  * the cache registry keeps `shared_ptr<HFFileCache>` entries instead of `weak_ptr`, so a file's fetched
    windows survive the handle churn of a parquet metadata open/close — this alone took a 287 MB scan from
    ~1.9x the file down to ~1.47x;
  * windows are taken on a grid that is **fixed for the life of the cache** (doubling adds blocks, it never
    re-aligns), and a candidate window is trimmed at the first boundary already cached or in flight — with
    that, `bytes_fetched == file size` for a full scan at every measured `hf_read_buffer_size` (64 KiB,
    256 KiB, 1 MiB, 8 MiB) and with the fan-out on or off;
  * a missing window is pulled in up to `hf_read_max_parallel_requests` parallel range parts
    (`GetRangeInto` streams straight into the caller's buffer, no extra copy);
  * new settings `hf_read_max_parallel_requests` (4) and `hf_read_cache_total_size` (1 GiB, split between
    the files open at once), per-file `hf_read_cache_size` default raised 128 MiB -> 256 MiB;
  * new counters + `hf_read_stats(reset := false)`, `HFReadCacheClear()`;
  * **a commit invalidates the cached windows of its repository** — retention beyond the handle had made a
    read-after-`COPY TO` return the old bytes (caught by `smoke_models.sql`, now covered by tests).
* Read cache is in-memory only: no disk I/O anywhere on the read path (verified by grep + by behaviour);
  the only local files created are write-path scratch buffers before a commit.
* Two smaller write-path fixes came with it: a first write to a *missing* repository is now bootstrapped
  before the `preupload` (create-on-write used to die on the preupload's 404 `RepoNotFound`, which the
  commit endpoint never got to fix), and `hf_delete(..., missing_ok := true)` makes cleanup scripts work
  against paths that may not exist.
* Remaining known gaps (deliberate, documented in `README.md`): Xet uploads, `COPY TO 'hf://buckets/...'`
  of DuckDB's own bytes, and async prefetch across windows (the one read pattern still behind httpfs:
  whole-file `read_blob`, 6.0 s vs 4.3 s).

Immediate next actions: (1) start Phase 2 - read `xet-core` for the CDC parameters, the chunk-hash format
and the CAS upload protocol, since client-side chunking is what unlocks real bucket writes and cheaper
uploads; (2) get one green run of `.github/workflows/build.yml` — the `gh` token is missing Actions
permission (`gh workflow list` -> HTTP 403), so either the token needs the Actions scope or the workflow
has to be started from the web UI; (3) dogfood distribution by hosting the generated repository on the Hub
and installing from `https://huggingface.co/datasets/...`.

## What the user asked for

A DuckDB extension that reads **and** writes Hugging Face Hub assets through `hf://` URLs with the
*same experience/syntax as `s3://`* (i.e. `INSTALL hf; LOAD hf; select * from 'hf://datasets/foo/bar.parquet'`,
plus `COPY (...) TO 'hf://...'`, globbing, partitioned writes). Must work with **DuckDB 2.0** and with the
Python pre-release (`pip install duckdb --pre --upgrade`). `huggingface_hub` and `opendal` were suggested as
references for the wire protocol. Plans + intermediate goals live in `README.md`/`PLAN.md`; progress here.

## Environment facts (verified)

| item | value |
|---|---|
| host | DGX Spark, `aarch64` / `linux_arm64`, Debian 12 bookworm, gcc 12.2.0, 20 cores, ~2.9 TB free |
| repo | `/home/quentin/hf/hf-duckdb-extension` (git repo, private remote `lhoestq/duckdb-hf-extension`) |
| venv | `.venv/` → `. .venv/bin/activate` (has `cmake`, `ninja`, `huggingface_hub`, `duckdb`) |
| duckdb python | `2.0.0.dev2610151406` |
| duckdb core | `v2.0.0-alpha47600`, source id **`d4355d01c59362d57036ad377735dd5340276a5b`**, codename `Cyanoptera` |
| duckdb source | shallow clone at exact commit: **`/home/quentin/hf/vendor/duckdb`** (393 MB) |
| extensions dir | `~/.duckdb/extensions/v2.0.0-alpha47600/linux_arm64/` |
| network | pypi, huggingface.co, github.com all reachable |
| HF token | `~/.cache/huggingface/token` (user `lhoestq`), picked up by the `credential_chain` provider |
| github | `gh` CLI authenticated as `lhoestq` (https git ops) |

## Ground truth about DuckDB 2.0 discovered so far

* `INSTALL httpfs; LOAD httpfs` **works** on the pre-release → core repo has matching nightly builds.
  Installed `httpfs` reports `extension_version = abdd053708` while built-ins report
  `v2.0.0-alpha47600` ⇒ 2.0 uses a new hashed extension-ABI tag. **Must understand this to ship a loadable .so.**
* `duckdb_extensions()` columns changed to: `extension_name, loaded, installed, install_path, description,
  aliases, extension_version, install_mode, installed_from, signature_key_fingerprint`.
  (gone: `name`, `installed_version`, `repository`, `supported_until`, `path_variable_names`).
* Legacy reverse syntax `from f() select ...` now raises a parser error — write normal `select … from …`.
* `duckdb_version()` no longer exists; use `PRAGMA version` / `PRAGMA platform`.
* `httpfs` is **no longer in-tree**: `extension/` now holds only
  `autocomplete core_functions delta demo_capi icu json loader parquet tpcds tpch` ⇒ ours is an
  out-of-tree extension (OOTE) built with DuckDB's root CMakeLists + `extension-ci-tools`.
* Useful in-tree helpers: `scripts/check_extension_abi.py`, `scripts/compute-extension-hash.sh`,
  `scripts/create_local_extension_repo.py`, `extension/README.md`, `AGENTS.md`.

## ⚡ MAJOR DISCOVERY: `hf://` ALREADY EXISTS (read-only) inside httpfs

`duckdb-httpfs` @ `abdd053708ff…` (the tag duckdb 2.0 pins == what `INSTALL httpfs` installed) contains
**`src/hffs.cpp` + `src/include/hffs.hpp` → `HuggingFaceFileSystem : HTTPFileSystem`, 467 lines, read-only**.

Verified live with the installed extension:
* `select * from glob('hf://datasets/lhoestq/demo1/**')` → works (lists `.gitattributes`, `README.md`, `data/*.csv`).
* `hf://datasets/…`, `hf://spaces/…`, `hf://buckets/<user>/<bucket>/…` supported; revision = `@rev` after the
  repo id (`hf://datasets/u/d@myrev/dir/f.parquet`), default `main`.
* **`hf://models/...` is REJECTED** ("Currently DuckDB only supports querying datasets, spaces, or buckets").
* Reads: HEAD/GET/GET-Range on `{endpoint}/{repo_type}/{repo}/resolve/{rev}{path}`.
* `Glob()`: `GET {endpoint}/api/{repo_type}/{repo}/tree/{rev}{path}?limit=N` + `Link` header pagination +
  a hand-rolled JSON scanner (`HFListResultParser`) + custom `**` path matcher (`Match()`).
* Auth: `secret_manager->LookupSecret(*txn, path, "huggingface")` → `KeyValueSecret["token"]` → Bearer header.
  `CREATE SECRET (TYPE HUGGINGFACE, PROVIDER config|credential_chain)` exists; credential_chain reads
  `~/.cache/huggingface/token`. (Confirmed: spaces tree listing 401s until such a secret exists.)
* Registered in `httpfs_extension.cpp`: `fs.RegisterSubSystem(make_uniq<HuggingFaceFileSystem>())`.

**⇒ Our extension must add: WRITES (COPY TO / INSERT / CREATE TABLE / delete), `models`, more URL forms,
zero-config auth, listing/robustness — while coexisting with (or superseding) httpfs' read-only hf fs.**

## Architecture fork (resolve BEFORE writing Phase 1 code)

Constraints found in duckdb 2.0 source:
* `VirtualFileSystem::FileSystemRegistry::RegisterSubSystem` **throws on duplicate `GetName()`**; registry is
  copy-on-write; `FindFileSystemInternal` keeps scanning so **the LAST registered subsystem that can handle the
  path wins**, unless a subsystem `IsManuallySet()` (short-circuits). ⇒ order hazard: if httpfs loads *after*
  ours (e.g. autoloaded later for `s3://`), its read-only HF fs silently wins and writes break.
  Tools available: `VirtualFileSystem::ExtractSubSystem(name)` / `UnregisterSubSystem(name)` (public!) to drop
  httpfs' `"HuggingFaceFileSystem"` when we load, `IsManuallySet()`/`SetManuallyInUse()` to keep priority.
* Extensions are dlopen'd with `RTLD_NOW | RTLD_LOCAL` (`src/main/extension/extension_load_dynamic.cpp:72`)
  ⇒ no cross-extension symbol resolution by default. httpfs *does* export all 989 of its `duckdb::` symbols
  (`nm -D` on the installed .so), so subclassing `HTTPFileSystem` from our .so is possible via a `DT_NEEDED`
  + `$ORIGIN` rpath hack (all extensions share one dir) — feasible but fragile/OS-specific.
* Cheaper, core-only alternative: **delegate HTTP to the instance file system itself**
  (`instance.GetFileSystem().OpenFile("https://…")` → httpfs, auto-loadable) for reads + listing (read the
  `/api/.../tree` JSON as a stream and parse it ourselves), and get POST/PUT for the commit API from
  `duckdb/main/http/http_util.hpp` ("compatibility header for existing extensions") / `DBConfig::http_util`,
  else link our own tiny client (system `libcurl`/`libssl` — availability not checked yet; else vcpkg).
* Build shape (copied from httpfs): OOTE repo with `extension_config.cmake` → `duckdb_extension_load(hf
  SOURCE_DIR …)`, root `CMakeLists.txt` using `add_extension_definitions()`, `build_static_extension`,
  `build_loadable_extension(hf -warnings ...)`, `Makefile` including `extension-ci-tools/makefiles/
  duckdb_extension.Makefile` (+ `vcpkg.Makefile`), `vcpkg.json`, and the new 2.0 entry point:
  `DUCKDB_CPP_EXTENSION_ENTRY(hf, loader) { … loader.GetDatabaseInstance() … }`.

## Decisions taken

0. **HF token IS available**: `~/.cache/huggingface/token` → `whoami` = **lhoestq** (Quentin, member of the
   `huggingface`/`nvidia` orgs), `huggingface_hub` 2.2.0 in the venv. Write tests are therefore possible; plan is
   to use a dedicated scratch repo (name TBD) rather than polluting anything existing.
1. **Language:** native C++ extension (a real `FileSystem` subclass registered on the DB instance) — this is
   the only way to get true `s3://`-equivalent behaviour (streaming, seek, glob, `COPY TO`) inside a *loadable*
   extension. Python-side hacks are not acceptable.
2. **Reference implementation:** duckdb-httpfs's `S3FileSystem` (subclass of `HTTPFileSystem`) +
   DuckDB's `ExtendedFileSystems` / `FileSystem::RegisterSubtree` machinery. Need to confirm names in 2.0.
3. **Wire protocol:** raw HTTPS against `huggingface.co` / `cdn-lfs.huggingface.co` with libcurl (already a
   httpfs/vcpkg dep): reads via `/resolve/{rev}/{path}` + `Range`, listing via
   `GET /api/{type}s/{repo_id}/tree/{rev}?recursive=1`, writes via the *preupload* flow:
   `POST /api/blobs/{sha256}` (`x-etags`) then `POST /api/{type}s/{repo_id}/commit/{rev}` (ndjson).
   `huggingface_hub` source + `opendal`'s `services/hf` are the behavioural oracles.
4. **Auth:** `hf_token`/`HF_TOKEN`, `HF_TOKEN_PATH` (`~/.cache/huggingface/token`), `HF_ENDPOINT`,
   `HF_HUB_OFFLINE`, mirrors `s3_url_style`-style settings; never leak tokens in errors/explain.
5. **Path grammar:** `hf://<repo_type>/<repo_id>/<path>` with `datasets|models|spaces|codes` + bare
   `repo_id` fallback, `@revision` suffix (mirrors `huggingface_hub` / opendal-hf). Must document loudly
   since S3 has no revision concept.

## Open blockers / risks

* **No HF write token yet** ⇒ cannot test the write path end-to-end. Need a `write`-scoped token + a scratch
  repo (I can create the repo but not the token). Ask user for `HF_TOKEN` in env or `~/.cache/huggingface/token`.
* **Loadable-extension ABI for a `-dev` core**: need to reproduce `abdd053708`-style tag; a locally built
  extension may be refused by the pip wheel. Fallbacks: (a) build against exact commit (already cloned),
  (b) custom extension repo/`SET custom_extension_repo`, (c) unsigned/`allow_unsigned_extensions`,
  (d) ship a statically-linked duckdb build in the repo for testing.
* **No cmake in apt** — using pip cmake; OK for now.
* Full DuckDB build on 20 aarch64 cores ≈ 20–45 min; must be run detached so it survives a compaction (see below).

## Rules of engagement I follow

* Everything builds/tests under `/home/quentin/hf/hf-duckdb-extension`; vendored deps under `/home/quentin/hf/vendor`.
* Never commit tokens. `.gitignore` from the start.
* Long builds run with `nohup … &` writing to `build/logs/` so progress survives session compaction.
* Update `PLAN.md` checkboxes + this file at every milestone.

## Resume procedure (paste this after a compaction)

```bash
cd /home/quentin/hf/hf-duckdb-extension && . .venv/bin/activate
cat PLAN.md && cat STATE.md            # re-read plan + state
tail -50 build/logs/last.log 2>/dev/null   # whatever was running
ls -la && ls src/huggingface 2>/dev/null
```

Then continue with the first unchecked box in `PLAN.md`. Do not restart the env setup — it is done.
For Xet work: `docs/XET.md` is the protocol reference (format findings, the CAS API, the shard layout),
`scripts/xet_oracle.py` regenerates `src/hf_xet_*_vectors.cpp` from `/home/quentin/hf/xet-core`, and
`tools/xet_probe` (cargo, `--release`) is the oracle. Offline suite: `bash scripts/smoke_xet.sh`.

## Change log (append one line per meaningful step)

* `2026-10-16 09:15` — session start, empty dir.
* `09:20` — verified network + toolchain gaps; created `.venv`; installed duckdb 2.0.0.dev2610151406, cmake, ninja, huggingface_hub.
* `09:25` — probed 2.0 surface: httpfs installable, extension table reshaped, `from…select` removed, `duckdb_version()` gone.
* `09:30` — resolved `d4355d01c5…` full sha via GitHub API; shallow-cloned DuckDB at that exact commit into `~/hf/vendor/duckdb`.
* `09:38` — read extension README top half; confirmed httpfs out-of-tree in 2.0; found ABI/hash helper scripts.
* `09:40` — **compact request #1** → wrote STATE.md + PLAN.md.
* `09:52` — cloned `duckdb-httpfs`@`abdd053708` + `extension-ci-tools` (extension_template repo no longer exists).
* `09:55` — **FOUND `HuggingFaceFileSystem` ALREADY IN httpfs (read-only)**; live-verified glob/read on a public
  dataset, `hf://models` rejected, spaces listing 401 without a `TYPE HUGGINGFACE` secret.
* `09:58` — read `hffs.cpp` fully (resolve/tree URLs, Link pagination, JSON scanner, `**` matcher, secret lookup)
  + httpfs `CMakeLists/Makefile/extension_config/vcpkg` (the OOTE skeleton to copy).
* `10:02` — read `virtual_file_system.cpp`: dup-name registration throws, last-match-wins, `ExtractSubSystem`
  public; `dlopen(RTLD_NOW|RTLD_LOCAL)`; 2.0 entry macro `DUCKDB_CPP_EXTENSION_ENTRY(name, loader)`.
* `10:05` — compact request #2 → this update.
* `10:20`→`13:40` — Phase 1/2: skeleton (`CMakeLists`, `Makefile`, `extension_config.cmake`), settings,
  `huggingface` secret type (`config` + `credential_chain`, tolerant of httpfs' duplicates), `hf_url`,
  `hf_http` (auth, retries, `Link` pagination, JSON scanning), own sha256/base64/json writer, filesystem
  dispatch (`IsManuallySet`), read handle with range reads, commit protocol, write handle, table functions.
* `13:45` — first loadable build; footer mismatch fixed with `OVERRIDE_GIT_DESCRIBE=v2.0.0-alpha47600`.
* `13:50` — live write path debugged against `lhoestq/playground`: preupload needs the nested `"files"`
  array, presigned LFS PUTs must **not** send the token, commit headers must omit `parentCommit` (null is
  rejected), model resolve URLs carry **no** `models/` prefix (`HFRepoType::ToGitPrefix`).
* `14:00` — buckets: resolve URL is `{endpoint}/buckets/{id}/resolve/{path}` (unversioned), deletes go to
  `POST /api/buckets/{id}/batch` (`{"type":"deleteFile",...}`), writes refused because `addFile` needs an
  Xet `xetHash`. Empty `"failed": []` was misread as failure, fixed.
* `14:10` — `Glob` rewritten: `**` uses one recursive listing, otherwise a bounded directory walk over
  root-relative paths (`glob('.../wikipedia/**/*.parquet')` = 555 files).
* `14:30` — `hf_delete` rewritten: group by `(endpoint, repo_type, repository, revision)` (a mixed
  dataset+model list used to commit everything to the first repo), directories expand to their child
  files (the commit API rejects `deletedFolder` for `duckdb/part`), missing paths error out clearly.
* `14:35` — **write smoke suite green** (parquet/csv/json/overwrite/empty/partitioned/unicode/model/
  custom message/zstd + 404 unknown repo, 403 protected repo, bucket-write refusal).
* `14:40` — `SIGSEGV` on a 287 MB parquet read: `gdb` showed a corrupted `std::string` inside the read
  cache ⇒ DuckDB 2.0 issues positional reads on one handle from several tasks. Handle state got a mutex
  + atomic position (later moved into the shared cache).
* `14:55` — read perf investigation: the Hub CDN costs ~0.3s per range request whatever the size (curl:
  1 MB range 2.1 MB/s, 8 MB range 14 MB/s, full GET 72 MB/s), so *number* of requests is what matters.
  Also learned DuckDB's parquet prefetch issues 2 MB reads at strided offsets, which killed strict
  "continue the sequential run" heuristics.
* `15:05` — **read path redesigned**: `HFFileCache` (per resolved url, shared by all handles, refcounted
  via a weak registry) with an LRU list of fetched windows, in-flight windows *coalesced* (waiters sleep
  on a condition variable instead of re-downloading) and downloads running **outside** the lock, plus
  doubling aligned windows. Defaults: `hf_read_buffer_size=8 MiB`, `hf_read_cache_size=128 MiB`.
  287 MB file: full scan 59s → 8.8s (httpfs 6.6s), `read_blob` 43s → 5.0s (httpfs 4.9s), footer 0.9s.
* `15:10` — listing walk tolerates folder entries that 404 (`ListPage(..., tolerate_not_found)`), tried
  `expand=true` for listing mtimes and dropped it (payload cost not worth it: the tree endpoint's `expand`
  is a boolean that drags in `securityFileStatus` per entry).
* `15:12` — both smoke suites re-run green; `README.md` rewritten with the read-performance design.
* `15:18` — `git init` + first commit pushed to the **private repo `github.com/lhoestq/duckdb-hf-extension`**
  (`gh` is authenticated as `lhoestq`; `.gitignore` keeps `.venv/`, `build/`, `dist/` and scratch out).
  Remember: commit + push after every milestone from now on.
* `16:05` — repo-type coverage verified live: datasets (smoke_write), models (`scripts/smoke_models.sql`,
  incl. git-tracked vs LFS files compared byte-for-byte with `md5(read_blob(..))` on both sides), spaces +
  kernels (`hf_ls`, `read_text`), buckets (`scripts/smoke_buckets.sh`: seed with `hf buckets cp`, then
  read/glob/ls/meta/delete through `hf://buckets/...`). Read suite now also covers
  `hf://models/google-bert/bert-base-uncased/config.json` and `hf://kernels/kernels-community/flash-attn3`.
* `16:10` — **bucket writes stay refused**, and the reason is now confirmed rather than assumed: the bucket
  `batch` endpoint only accepts `addFile` with a `xetHash`, and there is no bucket commit/credentials route
  (`/api/buckets/{id}/{credentials,storage-endpoint,s3,endpoint,upload}` all 404, `POST
  /api/buckets/{id}/commits` does not exist), so nothing short of `hf_xet` can write one. Refusal moved into
  `HFRefuseBucketWrite()` so `COPY TO` and `hf_upload` say the same thing.
* `16:15` — `hf_upload(a, b)` was a binder error ("No function matches ... hf_upload(VARCHAR, VARCHAR)"):
  in DuckDB 2.0 a **keyword-only parameter without a default is mandatory**. `message` now defaults to ""
  meaning "derive it" (explicit message → `hf_commit_message` → `Upload <path>`). Rule of thumb for this
  code base: every `AddKeywordOnly` needs a default value.
* `16:25` — Phase 4 table functions added: `hf_list_revisions` (`GET /api/{t}/{repo}/commits/{rev}?limit=`),
  `hf_list_refs` (`.../refs` → branches/tags/converts), `hf_create_repo` (`POST /api/repos/create` with
  `name`+`organization` **split apart** — a `name` still containing the namespace is rejected — 409 means
  "already created", 409 + "conflicting operation" is retried), `hf_status()` (version, endpoint,
  token source, whoami, HTTP transport name from `HTTPUtil::Get(db).GetName()`, the read settings).
  `hf_create_repo`'s `private` argument and `hf_list_revisions`' `max_revisions` are named that way because
  `private`/`limit` parse as keywords; keyword-only params need defaults (see 16:15).
* `16:35` — **multipart git-LFS uploads implemented** (previously refused, and silently misdetected): the
  batch answer quotes `chunk_size` as a string, so `HFJson::AsInt` read it as 0 and the numeric presigned
  part urls ended up as *headers* — a 20 MB blob was PUT at the `complete_multipart` url (404). Now
  `chunk_size` is parsed with `AsString`, numeric header keys become part urls, parts are streamed from disk
  (never the whole file), each PUT collects its `ETag`, and the completion `POST` sends
  `{"oid":sha,"parts":[{"partNumber":n,"etag":...}]}` with git-lfs content types. 20 MB blob + 17 MB parquet
  written by `COPY` both round-trip byte-identically.
* `16:40` — smoke suites re-run: read 28/28, write 36 OK + the 3 intended refusals, models/buckets green.
* `17:05` — **Phase 5, first half**: `scripts/make_extension_repo.sh` (assembles `<version>/<platform>/`,
  reads version+platform out of the binary footer instead of guessing, ships plain + `.gz`) and
  `scripts/test_install.py` (throwaway `home_directory`, `INSTALL` + `LOAD` + a real `hf_ls`). Verified:
  `INSTALL hf FROM 'file:///.../dist/repo/'` works; `duckdb_extensions().installed_from` records the prefix.
* `17:10` — gotcha worth remembering: pointing `custom_extension_repository` at your own repo redirects
  *every* autoinstall there, **including httpfs**, which is this extension's https transport ⇒ bootstrap
  failure (`INSTALL httpfs` never runs, first query fails). Use `INSTALL hf FROM '<prefix>'`, or keep
  `autoinstall_extension_repository='core'`. The built-in DuckDB http client has no TLS in official builds
  (`'https' scheme is not supported.` is raised from `third_party/httplib`), while *extension install* does
  have TLS — that asymmetry is the whole reason this trap exists.
* `17:20` — **signing implemented and actually verified**. The format: reserve the trailing 256 bytes, hash
  with a *two-level* sha256 (sha256 over the concatenation of the sha256 of each 1 MiB chunk), `openssl
  pkeyutl -sign -pkeyopt digest:sha256`, append. Served metadata is
  `<prefix>/.well-known/duckdb-extension-repo.json` = `{"version":1,"signature_keys":["-----BEGIN PUBLIC
  KEY-----…"]}`. SQL quirks learned: `allow_extension_repositories` is a *string* enum
  ('undecided'/'allowed'/'forbidden'); `CREATE EXTENSION REPOSITORY` accepts no bind parameters; the key
  list is `USING PUBLIC KEYS 'a', 'b'` (comma separated, **no** parentheses); keys are compact base64 DER
  (PEM body without armor works too, it gets normalized); per-repository installs land in
  `extensions/<version>/<platform>/repositories/<repo>/` and need `LOAD hf FROM <repo>`.
* `17:22` — tamper test: copy the repo, flip a byte in **both** the plain and the `.gz` copy (DuckDB sniffs
  gzip and may take either — flipping only the plain one silently "passed"), then require a fresh process
  to refuse: `Attempting to install an extension file that doesn't have a valid signature`.
* `17:24` — chased a `SIGSEGV` in GC at interpreter shutdown: it was the *test* truncating the extension
  file DuckDB had mapped (`open(path,'wb')` truncates a loaded binary). Never mutate a loaded extension in
  place — tamper with a copy of the repository in another process.
* `17:28` — `HFClient::SendRequest` had a latent infinite recursion (`return SendRequest(request)` in the
  no-session branch, unreachable until now). It drives `HTTPUtil::SendRequest(request, client)` with a
  per-request client slot instead, and transport failures that mean "no https here" are rethrown as one
  actionable error naming `INSTALL httpfs; LOAD httpfs;`. Transport auto-load now also triggers on
  `Built-In`, not just on an empty transport.
* `17:32` — Makefile auto-detects both layouts (`./duckdb` + `./extension-ci-tools` for CI, `../vendor/*`
  locally); `.github/workflows/build.yml` calls DuckDB's reusable `_extension_distribution.yml` pinned to
  the vendored ci-tools sha. **Not executed on GitHub yet** — the matrix needs a first green run.
* `17:40` — all suites re-run after the transport change: read 30 OK / 0 FAIL, write 36 OK (+ the 3
  intended refusals), models 25 OK, buckets green (2 intended refusals).
* `18:05` — byte amplification hunt, using the new `hf_read_stats()` counters. Two independent causes:
  (a) the cache registry held `weak_ptr`s, so a parquet file's cache died with the metadata handle and was
  rebuilt by the data scan (the same 287 MB file downloaded ~1.9x); (b) a read straddling a window edge
  fell back to `fetch_start = offset`, abandoning grid alignment and re-downloading the overlap. Registry
  now owns `shared_ptr` entries (LRU over `hf_read_cache_total_size`, entries with open handles skipped),
  and `Read` loops over grid-aligned windows, copying across window boundaries instead of refetching.
* `18:20` — `FetchWindow` splits one missing window into up to `hf_read_max_parallel_requests` range
  requests on separate `HFClient`s (a client owns a transport session and cannot be shared between
  threads) and joins them in the claiming thread; `HFClient::GetRangeInto` streams a range straight into the
  caller's buffer. Counters renamed `unaligned_fetches` -> `straddling_fetches`, plus `hf_read_stats()`
  exposing files/hits/waits/windows/parts/straddles/evictions/invalidations/bytes/retained.
* `18:30` — retention beyond the handle broke read-after-write: `COPY` twice to the same path, the second
  read served the pre-commit window (`smoke_models.sql` caught it). Every commit (repo *and* bucket batch)
  now calls `HFReadCacheInvalidateRepo(HFRepoCacheKey(url))`, which drops the windows of that repository
  even from caches that still have live handles. Regression tests added to `smoke_write.sql` (c1-c4).
* `18:35` — defaults: `hf_read_cache_size` 128 MiB -> 256 MiB (8 windows of the 32 MiB ceiling: at 128 MiB
  an 8-thread scan evicts its own windows), `hf_read_cache_total_size` 1 GiB with a per-file share of
  `budget / open files` (floor 64 MiB) so a scan over many files cannot multiply the budget by its
  parallelism. Full scan of the 287 MB file: **3.9 s vs httpfs 3.8 s**, 289 612 440 bytes fetched for
  287 412 556 bytes on disk.
* `19:05` — `hf_read_cache_clear()` exposed as a table function (`cleared_bytes, cleared_files`); useful on
  its own and it makes the read-path tests able to start from an empty cache.
* `19:20` — DuckDB 2.0 caches remote files *above* the filesystem (`enable_external_file_cache`, on by
  default, block sized by `external_file_cache_remote_*`, `external_file_cache_request_sizing=AUTO|GRID`):
  a re-read of a small file is served by DuckDB itself, so `hf_read_stats()` shows 0 bytes even with our
  cache cleared. The read-path tests set `enable_external_file_cache = false` to measure this extension,
  and the README says so; measured big-scan timings were within noise either way (4.5-5.0s, httpfs 5.4s).
* `19:35` — the aligned grid still leaked bytes (1.58x-1.94x on some `hf_read_buffer_size` settings):
  `fetch_length` doubling moved the *alignment* with it, and separately a widened window could re-download
  a smaller window another thread had claimed a moment earlier. Fixed by a fixed `grid_length` per cache
  plus trimming a candidate window at the first cached/pending boundary ahead of the cursor; the claim log
  behind it is env-gated (`HF_READ_DEBUG=1`).
* `19:45` — verified byte accounting: 287,412,556 fetched for a 287,412,556 byte parquet, all four buffer
  sizes and both fan-out settings, 0 straddling fetches, 0-2 evictions.
* `20:00` — bucket writes: proved `addFile` needs a real `xetHash` (`sha256`/none/inline `contents` all
  refused, no plain PUT route), then found the route that works without a Xet client - `paths-info` gives
  the Hub's hash, the bucket batch accepts `copyFile`. `hf_bucket_copy(source, target)` implements it for
  one file, a folder, a glob, and bucket-to-bucket, mirroring structure under the destination and skipping
  `.gitattributes`.
* `20:05` — `HFXetHashes` has to take a `HFTarget`, not an `HFUrl`: with only a url the `paths-info` call
  lost the authenticated connection and came back `401 Invalid username or password`.
* `20:10` — destination naming follows the hub client's rule (existing file wins, then existing prefix,
  then trailing slash), which is what makes `.../duckdb/mirrored` land *inside* the folder.
* `20:15` — create-on-write reached the `preupload`: a first write to a missing repo used to fail there;
  `HFCreateRepoIfAllowed` now runs before the retry. Fresh dataset *and* model repos both work.
* `20:20` — `hf_delete(..., missing_ok := true)`; `try.py` splits statements on `;`, so the suites are
  multi-line and readable again; bucket suite rewritten to seed fixtures through the extension itself
  (staging dataset repo) instead of the CLI, and to assert the copy semantics.
* `20:35` — suites green: read 55, models 25, write 40 (+3 intended refusals), buckets 23 (+2 intended
  refusals). README documents `hf_bucket_copy`, the skip case, and the delayed Xet hash.
* `20:55` — `straddling_fetches = 0` turned out not to be an invariant (one read at the file tail is served
  across a window boundary depending on how DuckDB interleaves tasks; bytes stayed exact either way), so the
  read suite *reports* it and only promises `bytes_fetched == file size`. Two clean 55 OK / 0 FAIL runs.
* `22:20` — create-on-write was broken by the settings lookup, not by the Hub: `HFWriteHandle` resolves
  settings at open time on purpose (the opener does not outlive the handle), so by the time `PreuploadMode`
  asked `TryGetCurrentSetting` the opener was null and `hf_create_repo` read as `false`. The flag is now a
  value passed down (`HFCommit(..., bool create_repo)`), and `HFSettingAsBool` centralises the lookup. The
  "the Hub provisions repos asynchronously" theory was a red herring caused by probing bare repo names: a
  fresh repo answers `GET /api/datasets/{ns}/{name}` in ~0.15 s and `preupload` stops 404-ing in ~0.2 s.
* `22:35` — creating a Space needs `"sdk"` in the create payload (`gradio|docker|static`); create-on-write
  sends `static`. Write suite: 45 passed, 5 expected failures, 0 failed.
* `22:50` — repo deletion is not reachable from SQL: the Hub wants `DELETE /api/repos/delete` with a JSON
  body, `DeleteRequestInfo` has no payload field, neither the curl nor the httplib client attaches one, and
  the endpoint is not routed for POST. An `hf_delete_repo` function was written, proven impossible over the
  transport, and removed; `scripts/delete_repo.sh` does it, and the smoke suites now call it. Their old
  `DELETE /api/datasets/<repo>` cleanup had been 404-ing silently for weeks - 29 leaked probe repos found
  and deleted with the correct route.
* `23:05` — suite fixes: bare `FROM 'hf://....txt'` is not snifiable, use `read_text`; buckets setup uses
  `missing_ok := true` so it is idempotent against a deleted staging repo; `run_all.sh` reads the grade line
  instead of `tail -1` (housekeeping output came after it). 140 checks + 7 refusal checks, all four suites
  green in one run.
* `23:15` — 0.0.1 packaging: `CHANGELOG.md` (with the two-axes note: this project's version vs the DuckDB
  identifier the binary is tied to), README *Testing* section documenting the `assert*` / `expect-fail`
  conventions, layout listing corrected, repo-delete + Space-sdk limitations written down.
* `23:30` — `hf_status()` read settings through `DatabaseInstance::TryGetCurrentSetting`, which only sees
  GLOBAL scope, so it kept saying `hf_create_repo = false` after `SET hf_create_repo = true`. It now looks
  them up on the `ClientContext` (the init function always had one) and agrees with `duckdb_settings()`.
  Added the missing `scripts/smoke_read.sh` so all four suites are invoked the same way.

* `2026-10-17 12:10` — M5 shard format read out of `xet-core`: sections, bookends, `FileTableIndex` key
  truncation (`truncated_hash` swaps endianness, key = first 8 LE bytes), `CompareByTruncatedHash`, section
  ordering (file by full hash, xorb by truncated key). Confirmed the *uploaded* form is
  `read_shard_to_bytes_remove_footer`: truncate at `file_lookup_offset`, rewrite header `footer_size` to 0.
* `12:40` — `src/hf_xet_shard.{hpp,cpp}` + probe `shard <max_bytes>` mode + oracle vectors: 11 reference
  shards byte-identical, including a 7-xorb file and the dedup/global-dedup flag cases. Selftest 226 → 286.
* `13:05` — `HFXetCasClient::UploadShard` (`POST /v1/shards`, raw bytes, CAS bearer). Live: small file
  published to `lhoestq/playground` and read back byte-identical; re-publish dedups (`inserted=false`,
  `shard_result=exists`). `hf_xet_upload` gains `publish` + the shard columns.
* `13:35` — bucket batch `addFile` path fix (`PathInRepo` leading slash → was `422`). `COPY TO 'hf://buckets/…'`
  writes a readable parquet; multi-xorb (230 MB uncompressed) initially rejected by CAS with
  `invalid shard failed to verify file`.
* `14:20` — root cause: segment chunk indices are xorb-local, verification hashes are file-global. Fixed
  `HFXetShardSegment` (two coordinate fields), probe + vectors regenerated, selftest 296. Multi-xorb bucket
  round-trip green, and p5 proves the Hub treats a DuckDB-written bucket file as Xet content (bucket→bucket
  copy of it succeeds).
* `14:55` — trailing-slash write refusal (bucket *and* git path; DuckDB local refuses the same statement).
  Full `run_all.sh` green: 174 statements, 9 expected failures, 0 failures. Bucket probe artifacts deleted;
  `lhoestq/playground` bucket is empty again.
* `16:40` — M6 wiring: oracle + probe learn the sha256 metadata extension (two new shard vectors),
  `HFXetUploadFileContents` takes an optional sha256, `hf_xet_write` (default true) decides per write, and
  `hf_commit.cpp` falls back to `UploadLFSBlob` when the CAS throws. Selftest 325.
* `17:30` — the extension's own sha256-ext self-check was wrong: hex groups are little-endian words
  (`DataHash::from_hex`), not byte pairs. Same trap as M1, new location; caught by the vector, then by a
  flags-word assertion added next to it.
* `18:00` — live: 6 MB of `/dev/urandom` written to the Xet-enabled staging repo with `hf_xet_write = true`,
  committed, read back identical. Non-xet probe repo (`lhoestq/hf-ext-nonxet-probe`, `xetEnabled: false`)
  still handed out a CAS token: no gate to wait for.
* `19:45` — CI verification gap closed the honest way: `scripts/ci_selftest.sh` + the `selftest` job (see
  the CI fact above; `duckdb_extension_statically_link` and `LOAD hf` in a `.test` file are dead ends, both
  tried and reverted). Version 0.0.2-pre3.
* `20:05` — 0.0.2-pre3 published to `lhoestq/duckdb-hf-extension` (signed + unsigned) and verified over
  https both ways; the installed binary answers `hf_version = 0.0.2-pre3`. First unsigned attempt failed
  with a bare `Failed to install remote extension` seconds after the commit and passed on re-run: the
  dataset is Xet-stored, and the `resolve` redirect for a just-committed file needs a moment.
* `20:20` — swept: staging repo lost `m6/xet-*.bin` and an unreferenced `xet_random.csv` (`xet_probe.parquet`
  stays, `docs/XET.md` quotes its xetHash); the non-xet probe repo lost its 4 MB file, keeps the repo.
* `20:40` — CI run `39063970030`: three platforms green, `selftest` **failed** — `linux_amd64` artifact
  footer carries `d4355d01c59362d57036ad377735dd53`, its own test-support duckdb reports `v` + the same
  sha, so nothing could load it. Read the footer bytes off the downloaded artifact to be sure it was the
  artifact and not the runner.
* `21:05` — root cause is a Makefile precedence, not a missing tag: `DUCKDB_VERSION` (exported by
  extension-ci-tools from `duckdb_version`) beats `OVERRIDE_GIT_DESCRIBE`, and the footer's version field
  is written from that env. `duckdb_tag` creates the git tag; nothing downstream reads it. No upstream tag
  to describe from either (`ls-remote --tags 'v2.0.0-alpha*'` empty).
* `21:20` — `lhoestq/duckdb`: `commit-tree` of upstream's own tree (`9a1fc9ad…`), one commit, tagged
  `v2.0.0-alpha47600`, pushed; fresh clone `git describe`s to the identifier. Workflow switched to
  `override_duckdb_repository` + `duckdb_version: v2.0.0-alpha47600`, `duckdb_tag` removed (it would `git
  tag` a tag that exists). The temp tag created in `../vendor/duckdb` was deleted, so local builds still
  need the documented `OVERRIDE_GIT_DESCRIBE` instead of a hidden local ref.
* `21:25` — BLAKE3 per-ISA (NEON here; CPUID dispatch on x86-64; portable elsewhere) + the offline CPU
  benchmark. `scripts/xet_bench.sh` exists so the "hash-bound?" answer stays a measurement: 152 ms per 100
  MB against 12–70 s of upload. Next thing to parallelize is the xorb POST sequence.
* `10:20` — CI run `39077156438` finished: four platforms **and** the self-test job green, its log saying
  `0.0.2-pre3|v2.0.0-alpha47600|true` and `325 reference checks, every one of them passing` about the
  downloaded artifact. The mirror fix works, and x86 SIMD hashing has now been executed on x86.
* `10:45` — chose the next optimization by measuring instead of by intuition: `scripts/xet_bench.sh`
  (CPU phase: 31 ms chunking, 152 ms chunk+hash+xorb per 100 MB) and six interleaved 200 MB uploads
  (c=1: 29.8/57.9/31.2, c=3: 17.6/20.9/18.0, c=4: 22.2/19.4). Concurrency, then LZ4; not SIMD, which is
  1.15-1.27x of 1%.
* `11:05` — `HFXetXorbUploader` (bounded in flight, first error wins, joins in its destructor, runs the
  send inline when a thread will not start), `Authorize()` copying the token out under a mutex instead of
  `CasUrl()` handing out a reference into state another thread refreshes, `ForWorkerThread()` dropping the
  thread-affine opener like the read path does, and `hf_write_max_parallel_uploads` (default 3). p6 added
  to the live suite: re-uploading a multi-xorb file at concurrency 1 must find every xorb `inserted =
  false` and the shard `exists` - which is the same-content proof, obtainable for no new storage.
* `11:20` — bumped to **0.0.2-pre4**; `scripts/run_all.sh --build` → 189 passed, 9 expected failures, 0
  failed. To publish: pre4 goes out from CI's artifacts, which is the first time amd64 and windows
  binaries exist at all.
* `12:05` — M8 started from the oracle numbers rather than from enthusiasm: vendored lz4 v1.10.0
  (`lz4.c`/`lz4hc.c`/`lz4frame.c`/`xxhash.c`, BSD-2) as `hf_lz4_library`, per-chunk LZ4 frames behind the
  scheme-1 chunk header, `hf_xet_compression` setting (default `lz4`), round-trip decompress-and-compare
  inside `AddChunk`.
* `12:35` — the selftest caught nothing and its own arithmetic was the bug: the LZ4 loop read the footer's
  trailing u32 with descending byte indices, which only misbehaves when the value exceeds 255 - i.e. only
  for multi-chunk xorbs, and the pinned vectors that passed were single-chunk. Read forward, and
  `footer_start == last physical_end` for every vector. Worth remembering: a selftest that fails is not
  the same as a writer that is wrong.
* `12:40` — **interop proven with the reference's own reader**: `xet-probe xorb-read` on xorbs dumped by
  our writer (`scripts/xet_bench.sh <file> lz4 <dir>`) reports `num_chunks=33`, `uncompressed=2103723`
  and `footer_xorb_hex=9f7738bc…b16489` - the same hash `xet-probe xorb lz4` computes for the reference
  writer's file, and the same hash the `none`/`bg4`/`auto` schemes produce. Random 5 MB: ours 5,247,715 vs
  reference 5,246,480 (+0.02%), xorb hash identical.
* `12:55` — symbol hygiene: `-fvisibility=hidden` alone left 75 exported LZ4 symbols (lz4.h stamps
  `visibility("default")` unless its own macros are predefined); emptying `LZ4LIB_VISIBILITY` /
  `LZ4FLIB_VISIBILITY` gets it to 0, with `hf_library`'s visibility untouched so the entry points stay
  exported. Verified by `nm -D` and by `LOAD`ing the result.
* `13:05` — live suite adapted: `assert_serialized_is_larger` → `assert_lz4_shrinks_the_wire`, plus the
  cross-scheme identity block (`hf_xet_compression='none'` re-upload must find every xorb already
  stored). xet_upload 27 → 30 checks; `run_all.sh` → **192 passed, 9 expected failures, 0 failed**;
  selftest 325 → 367 (pinned in `test/sql/hf_selftest.test`, `smoke_xet.sql`'s floor, README, docs).
* `13:06` — committed `af700f4`, tagged `v0.0.2-pre5`, pushed; CI run `39130114709` started (linux_amd64,
  linux_arm64, windows_amd64_mingw; macOS excluded on push events). It ran green about 25 minutes later -
  LZ4 and the symbol hygiene build clean under mingw and x86_64, not only here.
* `13:19` — committed `7b52e05`. Assembling pre4's CI artifacts was the pipeline rehearsal that found the
  `v0.1H` misfiling bug; with the footer reader in place the same artifacts file all three platforms under
  `v2.0.0-alpha47600/`, and `test_install.py` installs/loads/queries from that tree over `file://`.
* `13:50` — `hf_xet_compression` validated at SET time; `smoke_xet.sql` 4 → 12 passed / 3 expected
  failures; full `run_all.sh` → **200 passed, 12 expected failures, 0 failed**; version → **0.0.2-pre6**
  (constant + the `smoke_write.sql` assertion that pins it); CHANGELOG restructured so the packaging fix
  sits in the release that carries it rather than in pre5. Commit `08948d1`, tag, push → CI run
  `39133332083`.
* `~13:55` — signed tree assembled from pre4's cached artifacts (three platforms, `signed=yes` in every
  footer, key fingerprint `sha256:f007c8016bc2f3a29…`), and `test_install.py --signed` installed from it on
  arm64 - which exposed that the tamper check was vacuous (see Current position). Fixed, re-verified both
  ways → commit `39d2b5e` at `13:59`.
* `~14:10` — pre6's own artifacts assembled, and every one of them footered `ext_version=08948d1d66` rather
  than the tag. Cause: the push used `git push --follow-tags`, which does not carry a **lightweight** tag,
  so CI had nothing to describe - and `duckdb_extensions().extension_version` shows that footer field in
  SQL, right next to `hf_version`. Annotated tag created on the same commit and pushed explicitly; run
  `39133332083` rerun (same `headSha`), so nothing was rebuilt from different source.
* `~14:40` — rerun green; both trees re-assembled from the new attempt's artifacts (fresh `CACHE_DIR`, since
  the old cache held the first attempt's bytes) and every binary now footers `v0.0.2-pre6`; signed tree
  install-verified locally, tampered trees refused.
* `14:48` — **0.0.2-pre6 published** to `lhoestq/duckdb-hf-extension` (Hub commit timestamp): signed at the
  root, unsigned under `unsigned/`, `linux_amd64` + `linux_arm64` + `windows_amd64_mingw`, twelve files plus
  the `.well-known` key - the first release with more than one architecture. Verified: `test_install.py`
  over https both ways on arm64 (signed install against the pinned key, then a real `hf_ls`), and the two
  architectures that cannot run here were re-downloaded from `resolve/main` and compared against what was
  built - byte-identical, valid gzip, correct footers.
* Next up, unstarted: the upload-side dedup query, shard-driven read prefetch, and the open macOS
  `workflow_dispatch` decision.
* `15:00` — the publish written up in `6786098`, including the `--follow-tags` trap and a release procedure
  for the README, so that the next version does not rediscover it.
* `16:14` — **0.0.2-pre7** (`dbd4d52`): the event-shaped matrix, `windows_amd64`, and the README
  installation section. Tagged `v0.0.2-pre7` (annotated, pushed explicitly), pushed, then dispatched: run
  `39144168208`. The local suite was run against the rebuilt pre7 artifact first — same 200/12/0.
* `16:18` — `2028782`: `opt_in_archs` opts an architecture in even when `exclude_archs` names it, which is
  not what `_extension_distribution.yml` says ("exclusions take precedence"). Caught by reading the job list
  of a run rather than its conclusion: the push run `39144156053` had `linux_amd64_musl` and
  `linux_arm64_musl` in it, so it was cancelled and the opt-in made conditional.
* `16:22` — `bfa3556`: the keyless repository form gets its own tamper refusal.
* **Unstarted next**, in order: publish pre7 from the dispatch run once every architecture's artifact is in
  (then verify each architecture that cannot run here by re-download + byte comparison, as pre6's were),
  the upload-side dedup query, and shard-driven read prefetch.
* `17:44` — `2d3f242`: the first two toolchain bugs (`idx_t` vs `string::size_type` on macOS, `unistd.h` +
  `getpid` on MSVC), and `reduced_ci_mode: 'disabled'` on dispatch. That last one is about run shape rather
  than the build: with `auto`, fail-fast cancelled `osx_amd64` and `windows_amd64_mingw` while an unrelated
  architecture failed to compile, so the run reported three failures when two of them had never been allowed
  to compile at all.
* `18:17` — `39706a5`, the commit the release is built from: the SIMD family asked of the compiler with two
  `check_c_source_compiles` probes, the zero-size arrays given an element nothing reads, the standard headers
  named explicitly. `v0.0.2-pre7` was **moved** to here (`git tag -f -a`, `git push --force`), because the
  footer field is `git describe` output: tagged at `dbd4d52` and built two commits later would have shipped
  binaries reporting `v0.0.2-pre7-2-g…`. Nothing had been published from the earlier position, which is the
  only thing that makes moving a tag honest.
* `18:40` — CI run `39153506221` green: eight architectures plus the artifact-level reference checks; both
  architectures that failed in the two previous runs now build.
* `18:45` — artifacts assembled twice over (`dist/repo-ci-pre7`, `dist/repo-ci-pre7-signed`, cache
  `dist/ci-artifacts/39153506221`), eight footers at `v0.0.2-pre7`, both trees install-verified locally —
  signed including the tamper refusal from both the pinned and the keyless form, 16 corrupted files.
* `18:49` — **published** (Hub commit `10716b52`), then re-downloaded: sixteen served binaries identical to
  CI's bytes, `INSTALL`/`LOAD` verified over https signed and unsigned. README, CHANGELOG and this file
  changed "three platforms" into "eight" wherever it appeared.
* **Unstarted next**, in order: the upload-side dedup query (a file sharing 90% of its chunks with something
  already in the CAS is still re-sent whole), then shard-driven read prefetch.
* `19:00` — ran the README's own steps verbatim against the live repository in a throwaway home, then a
  second connection in the same home: session 1 installs (both `hf_version` = `0.0.2-pre7` and
  `duckdb_extensions().extension_version` = `v0.0.2-pre7`, so the footer finally carries the tag), session 2
  loads with the gate at its default `undecided`, and `INSTALL` in session 2 is refused. That last half
  contradicted what the README had said an hour earlier ("re-run it to update", with the gate described as
  only a registration-time thing), so the wording moved; the CLI paragraph now says why putting the two setup
  lines in `~/.duckdbrc` is what makes a later `INSTALL` work without extra typing.
* `19:15` — `e5bb74d`: the conditional exclusion string I wrote in `dbd4d52` was malformed. GitHub binds
  `&&` tighter than `||`, so `event == push || event == pull_request && ";list" || ""` returned the boolean
  `true` and the "list" became the four characters `true` glued to the end of `exclude_archs`. Consequences:
  `wasm_threads` built on every push (~13 min, artifact consumed by nobody) and macOS/`windows_amd64`/musl
  were **not** excluded on push either — six architectures where three were intended, macOS minutes at 10x.
  Verified the fix against the live matrix rather than reasoning about it: run `39157627780`'s
  `Generate matrix` log now reads the full nine-name exclusion string and its job list is
  `linux_amd64` + `linux_arm64` (+ mingw) with no wasm, no macOS.
  **Two habits this argues for**: read the matrix job's *inputs* and a run's *job list* rather than its
  conclusion, because a wrong exclusion string is green; and in a GitHub expression, keep exactly one
  comparison to the left of `&&` — `!=` on a single event name is the honest way to say "everything except a
  release".
