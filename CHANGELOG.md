# Changelog

Two version numbers are in play, and they are independent:

* **this project's version** (below) — what the extension can do. `hf_status()` reports it as
  `hf_version`.
* the **DuckDB version identifier** (`v2.0.0-alpha…`) — a loadable extension binary is tied to
  exactly one of these, and a binary built for a different identifier is refused at `LOAD` time.
  `hf_status()` reports it as `duckdb_version`. A release of this extension is therefore one binary
  per DuckDB identifier per platform, and rebuilding for a newer DuckDB pre-release is a rebuild,
  not a version bump.

## 0.0.2-pre7 — every architecture DuckDB has a runner for, and an install path written from what clients actually do

* **The build matrix is now shaped by the event rather than by one fixed exclusion list.** A push builds
  the three cheap artifacts as before; a `workflow_dispatch` builds everything except wasm — `osx_amd64`,
  `osx_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `windows_amd64` on top of the three. Two details in
  extension-ci-tools turned out to matter. `linux_amd64_musl` and `osx_amd64` are marked `opt_in` in its own
  `config/distribution_matrix.json`, so *removing them from `exclude_archs` is not enough* — they need
  `opt_in_archs`. And `allow_failure_archs` exists, which is what lets an experimental cross-compile sit in
  the same run as the artifacts a release is made of: a job allowed to fail uploads nothing, so its platform
  directory is simply absent from the assembled tree instead of taking the run down.
* **A release run no longer cancels its own healthy jobs.** With `reduced_ci_mode: 'auto'` the shared
  workflow runs `fail-fast`, so the first architecture that fails to compile cancelled two architectures that
  were building next to it and had nothing wrong with them, and macOS jobs were not scheduled until the whole
  linux group finished. A dispatch now passes `reduced_ci_mode: 'disabled'`, which builds all eight in
  parallel and lets each answer for itself; `allow_failure_archs`, not fail-fast, decides the run's colour.
* **A malformed conditional exclusion string, written by me and invisible for six pushes' worth of builds.**
  GitHub's expression language binds `&&` tighter than `||`, so the natural-looking way to name two events —
  `event == 'push' || event == 'pull_request' && ';list' || ''` — parses as
  `(A || (B && ';list')) || ''`, short-circuits on `A`, and returns the **boolean** `true`, which pastes
  itself onto the string: `exclude_archs: windows_arm64;wasm_mvp;wasm_eh;wasm_threadstrue`. So `wasm_threads`
  stopped matching and built a browser artifact nothing consumes on every push, and the five expensive
  architectures — including macOS, at 10x billing — were not excluded either. Push run `39144427990` listed
  six architectures: `linux_amd64`, `linux_arm64`, `windows_amd64_mingw`, and then `osx_arm64`,
  `windows_amd64`, `wasm_threads`. A poisoned exclusion list cannot fail a build; it produces a green (or
  here, incidentally red) run that is twice the size it claims to be. The check is the `exclude_archs:` line
  echoed in the matrix job's log — that is where `…;wasm_threadstrue` was finally read — and a run's job
  list, read all the way down.
  * **A finding from earlier in this release is withdrawn**: "explicit opt-ins override exclusions, contrary
    to the input's own description". Its entire evidence was a push queueing musl builds that the exclusion
    string named — which this bug explains completely: on the dispatch that *did* build musl, the suffix
    evaluated to empty, so musl was never excluded and `opt_in_archs` did exactly what it says. Under the
    corrected strings the two lists do not overlap, so the precedence question is now genuinely untested
    rather than answered wrongly.
* **`windows_amd64` joins `windows_amd64_mingw`, and the difference is not cosmetic.** DuckDB derives the
  platform string it installs by — what `PRAGMA platform` answers — in `platform.hpp`, and the `_mingw`
  postfix is applied `#ifdef __MINGW32__` (plus a `DUCKDB_PLATFORM_RTOOLS` case for the R build). A client
  that is not itself mingw-built therefore asks for `…/<version>/windows_amd64/`, a directory 0.0.2-pre6
  published no file into: the mingw artifact can only ever serve a mingw-built DuckDB. Both are published now
  so neither kind of Windows client 404s.
* **A `## Installation` section at the top of the README**, written by running each step against the live
  repository instead of transcribing docs, which is how it picked up things the docs do not say: the
  repository registration is stored in `~/.duckdb/extension_repositories/<name>.json` and so is done once per
  machine rather than once per session — a later session `LOAD`s from it with `allow_extension_repositories`
  still at its default `undecided` — while the *gate itself* is per-session, and it gates installing as well
  as registering, so an update is `SET` + `INSTALL` in the same session and nothing else is needed to load; the public key needs no copy-paste because DuckDB reads it from the repository's
  `.well-known/duckdb-extension-repo.json` — and a prefix *without* that file cannot be registered at all,
  while a binary whose bytes were edited afterwards is still refused (re-checked here by flipping a byte in
  the local signed tree and watching `INSTALL` refuse it); a repository-installed extension must be loaded as
  `LOAD hf FROM hf_repo`, because bare `LOAD hf` looks at the flat path and calls the copy under
  `…/repositories/hf_repo/` *not found*; and nothing autoloads, so `LOAD` is a line per session — the same
  compiled-in-index rule that makes a third-party extension un-autoloadable is the one that keeps `INSTALL hf`
  with no prefix from working until this extension is in that index.
* **The new architectures found three bugs in our own code, which is the whole reason to build for them.**
  None was platform API — all three were things two toolchains had been letting us get away with:
  * `src/hf_http.cpp` passed values from `std::string::find()` to helpers taking `idx_t &`. `idx_t` is
    `uint64_t` (`typedefs.hpp`), and on Apple's libc++ that is `unsigned long long` while
    `string::size_type` is `unsigned long`: the same width, a different type, so the reference cannot bind.
    On Linux both spellings are `unsigned long`, so six call sites compiled for two years of builds and
    failed the first time macOS compiled them.
  * `src/CMakeLists.txt` picked BLAKE3's SIMD family from `CMAKE_SYSTEM_PROCESSOR`, which describes the
    **host** on an Apple cross-build. The `osx_amd64` job runs on an arm64 runner with
    `-DCMAKE_OSX_ARCHITECTURES=x86_64`, so the build believed it was compiling aarch64 and handed
    `blake3_neon.c` to an x86-64 compiler. Two `check_c_source_compiles` probes now ask the compiler what it
    is targeting. The Linux cross-compile version of this mistake would not have failed loudly — it would
    have shipped a binary with the wrong SIMD family in it.
  * `hf_xet_vectors.cpp` declared two zero-size arrays (a GNU extension MSVC refuses), and fourteen files
    used `memcpy`/`strlen`/`snprintf`/`getenv`/`uint64_t` without naming `<cstring>`/`<cstdio>`/`<cstdlib>`/
    `<cstdint>` — libstdc++ and libc++ hand those out through other headers, MSVC does not.
* **Published as 0.0.2-pre7**: CI run `39153506221` built all eight architectures green, including the
  reference-checks job. The assembled tree was signed and published to `lhoestq/duckdb-hf-extension` for
  `linux_amd64`, `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`,
  `windows_amd64` and `windows_amd64_mingw`. Every one of the sixteen served binaries (eight platforms, plain
  and gzipped, signed and unsigned trees) was re-downloaded and confirmed byte-identical to what CI built,
  with the footer reading `v2.0.0-alpha47600` + `v0.0.2-pre7`; `INSTALL`/`LOAD` then verified for real over
  https on this machine's own platform, signed and unsigned. The architectures that cannot run here — macOS,
  both Windows flavours, both musl builds — are verified by that byte comparison and nothing else, which is
  worth saying out loud rather than implying a green publish covers them.
* Version constant and the `scripts/smoke_write.sql` assertion that pins it move to `0.0.2-pre7`; the suites
  are unchanged at **200 passed / 12 expected failures / 0 failed**, selftest at 367 checks.

## 0.0.2-pre6 — a bad setting fails where it was set, and a release tree is checked against its own binaries

Published to `lhoestq/duckdb-hf-extension`: `v2.0.0-alpha47600` for **linux_amd64**, **linux_arm64** and
**windows_amd64_mingw** — the first release with more than one architecture — signed at the root and
unsigned under `unsigned/`, built by CI run `39133332083`.

* **`SET hf_xet_compression = 'bg4'` fails at the `SET`.** It used to be accepted, and then fail on the next
  `COPY` - an error a long way from its cause, and one that would have read as a compression bug. The
  accepted values are `lz4` (default) and `none`, case-insensitively. `bg4` is a real scheme in the format,
  which this build does not implement, and `'auto'` is not taken as a synonym for `lz4`: in the reference
  client it means a per-chunk decision this writer never takes, so accepting the word would promise a
  behaviour that does not happen. `scripts/smoke_xet.sql` pins the refusals, the normalisation of `'LZ4'`,
  and that `RESET` comes back to `lz4`.
* **A packaging bug that would have made the first windows release unusable.** The script that assembles a
  serveable repository read the DuckDB version identifier out of a built binary with `strings | grep
  '^v[0-9]'`, and in a mingw-built artifact the first such match is some fragment that reads `v0.1H` - so
  the binary was filed under `v0.1H/windows_amd64_mingw/`, a directory no client will ever ask for, while
  everything that could report on the run reported success. `scripts/ext_footer.py` now reads the footer as
  the structure DuckDB's `append_metadata.cmake` defines it (a 534-byte custom section: a
  `duckdb_signature` marker, eight NUL-padded 32-byte fields appended in reverse order, then the 256-byte
  signature slot), `make_extension_repo.sh` refuses a `DUCKDB_PLATFORM`/`DUCKDB_VERSION` that disagrees
  with the binary in front of it, and `publish_extension_repo.py` checks every binary's footer against the
  directory it is about to upload, before it touches anything remote.
* **The signature gate turned out to be untested, and it was passing.** `scripts/test_install.py`'s tamper
  check copies a repository, corrupts the served bytes, and requires a fresh DuckDB process to refuse them.
  Two ways it could report success without proving anything: it corrupted `os.listdir(...)[0]`, which on a
  three-platform tree is `linux_amd64` while the machine doing the install is `linux_arm64` - so the bytes
  it served were never touched - and its pass condition was "the child exited nonzero", which the child did
  also when it *succeeded*, because its `SystemExit(<message>)` exits 1. The line printed as the refusal was
  the child's own "LOADED DESPITE TAMPERING". Now every platform directory and both file forms are
  corrupted, the child signals acceptance by exiting 0, and a nonzero exit counts only when the error
  actually mentions the signature. On the real thing: 6 files corrupted, `INSTALL` refused with "an
  extension file that doesn't have a valid signature", and the same child run against untampered bytes exits
  0 - both directions of the check are exercised. The `signature -> verified against ...` line it printed
  was also an overclaim (it compared the key the *repository pins*, not a verification event), so it now
  says `trusted key -> repository pins ...`, and the refusal is where the proof lives.
* **Nothing was published under 0.0.2-pre5.** It was tagged and its CI run went green on all three
  platforms; these two changes landed before its artifacts went out, so the release that shipped is this
  one, and what a client downloads is built from the commit the tag names.

## 0.0.2-pre5 — xorb chunks go out LZ4-compressed

* **Chunk payloads inside an xorb are LZ4 frames now**, on by default, with `hf_xet_compression` as the
  switch (`lz4`, or `none` for the pre-M8 bytes). A Xet write waits on the wire, and this is the same
  upload with fewer bytes on it: a 2,103,723 byte CSV that serialized to 2,105,403 bytes uncompressed
  serializes to 889,917 compressed - 2.4x. The reference client's own encoder gets 881,602, 0.9% under
  ours, and that difference is free: two conforming LZ4 encoders are not required to agree on frame bytes,
  because a reader decodes each chunk from the scheme byte and the compressed length stored next to it.
* **What this cannot do is change what content *is***, and that is the property the whole change rests on.
  An xorb's hash is the merkle root of the *uncompressed* chunk hashes, so the four schemes produce the same
  `xorb_hex` for one file - measured, not assumed, with `tools/xet_probe` writing the same CSV four times.
  The live suite makes the CAS say it: `smoke_xet_upload.sql` writes a file with compression on, uploads it
  again with `hf_xet_compression = 'none'`, and requires every xorb to come back `inserted = false`.
  Meanwhile the metadata shard did not move at all - its byte ranges are cumulative *unpacked* lengths, so
  a reader asking for a range of a file asks in the space compression happens underneath.
* **Decodability replaced byte-exactness as the contract**, and is checked in four places: `AddChunk`
  decompresses the frame it just made and compares it with the bytes it hashed before any POST can happen
  (an uploaded xorb cannot be edited, and its hash looks fine while a broken payload makes it unreadable
  forever); the self-test rebuilds every pinned vector with LZ4 and pins hashes, chunk counts, unpacked
  totals and footer placement - frame bytes deliberately not among them; `xet-probe xorb-read`, which is
  xet-core's deserializer, reproduces the chunk hashes and the xorb hash of a file *our* writer dumped
  (`scripts/xet_bench.sh <file> lz4 <dir>`); and the live round-trip above. `hf_xet_selftest()` is at 367
  checks.
* It is not free and does not pretend to be: the CPU phase of a write goes from ~690 MB/s to ~370 MB/s on
  compressible data (from ~1% of a large upload's wall clock to ~2%), and incompressible chunks grow about
  16 bytes each - `1.001x` over 5 MB of random bytes - because reference clients compress *every* chunk and
  an incompressible one is stored raw inside a frame. There is no per-chunk "skip it" heuristic here, on
  purpose: the special case costs more than it saves and would make our xorbs a shape nobody else's are.
* **`XXH_NAMESPACE=LZ4_`, `-fvisibility=hidden`, and lz4's own visibility macros emptied out**, because
  `lz4frame` computes frame checksums with xxhash and this extension shares a process with DuckDB's own
  xxhash and its own namespaced LZ4. DuckDB's copies are inside C++ namespaces so nothing was colliding
  today; the extension now exports zero `LZ4_*`/`XXH*` symbols, which is what keeps it true tomorrow.

## 0.0.2-pre4 — writes go out over several streams, and CI's artifacts are the ones a client can load

* **A file's Xet xorbs are uploaded with several requests in flight**, which is the one thing a Xet write
  measurably waits on. `scripts/xet_bench.sh` times the CPU phase alone: 100 MB chunks in 31 ms and
  chunks+hashes+xorbs in 152 ms, against 12-70 s of wall clock for the same upload. Six interleaved 200 MB
  runs put `hf_write_max_parallel_uploads = 1` at 29.8/57.9/31.2 s and `3` at 17.6/20.9/18.0 s - about
  1.7x, not 3x, because a 200 MB file is four xorbs and the last one is 8 MiB and exposed either way, and
  because three streams on a link that sheds traffic are not three times one stream. `4` measured the same
  as `3`, so the default is the point where the curve flattened: `3`, which is also the smaller memory
  bill - an in-flight xorb is up to 64 MiB of serialized bytes, and the cap on those is simultaneously the
  whole backpressure story (the reader and chunker cannot get more than that far ahead of the network, so
  a 10 GB file costs what a 100 MB one does). Content addressing is what licenses the overlap: no xorb
  depends on another, and the metadata shard, which does, is built in file order by the thread reading the
  file and posted only after the last POST has landed.
* The concurrency's *identity* is what the suite checks, since "upload fast" is not an assertion:
  `smoke_xet_upload.sql` writes a multi-xorb file at the default of 3 and then re-uploads it at 1,
  requiring every xorb to answer `inserted = false` and the shard to answer `exists`. Xorb urls are
  content hashes, so a bug that moved a chunk boundary or described segments against the wrong object would
  produce hashes the CAS had never seen - and it would answer `true`. It also costs no new storage, which
  is the only reason it can live in a suite that talks to a real service.
* BLAKE3 is compiled per ISA: NEON on aarch64, SSE2/SSE4.1/AVX2/AVX-512 with CPUID-time dispatch on
  x86-64, portable C on MSVC and anywhere else. That is worth little on its own (1.15-1.27x on a phase
  worth 1% of a write) and a great deal as a side effect: the amd64 artifact now runs all 325 reference
  checks in CI, which is the first time any x86 SIMD path of this extension's hashing has been executed
  somewhere other than someone's expectation.
* **CI artifacts are loadable now, which they were not.** The self-test job failed on its first run with
  *built specifically for DuckDB version `d4355d01c593…`*: three green matrix runs had produced four
  platforms of binaries no client can `LOAD`, because DuckDB's Makefile lets the `DUCKDB_VERSION`
  environment beat `OVERRIDE_GIT_DESCRIBE`, extension-ci-tools exports it as the sha passed in
  `duckdb_version`, and the footer's version field is written from that variable. `duckdb_tag` creates the
  git tag and nothing downstream reads it, and `v2.0.0-alpha*` identifiers are tagged nowhere upstream -
  DuckDB's release CI generates them. CI therefore clones `lhoestq/duckdb`, a one-commit mirror whose tree
  object *is* upstream's for `d4355d01c5`, tagged with the identifier. Run `39077156438`: four platforms
  green, and `325 reference checks, every one of them passing` off the artifact a client would download.

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
