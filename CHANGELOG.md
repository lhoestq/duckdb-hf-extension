# Changelog

Two independent version numbers: **this project's** (what the extension can do, reported as
`hf_version`) and the **DuckDB version identifier** (`v2.0.0-alpha…`). A loadable binary is tied to exactly
one DuckDB identifier, so a release is one binary per identifier per platform, and rebuilding for a newer
pre-release is a rebuild rather than a version bump. `hf_status()` reports both.

## 0.0.2 — DuckDB 2.0.0 is out: the first release built against a released DuckDB, and the first published to a bucket

The target moved from `v2.0.0-alpha47600` to **`v2.0.0`** (upstream `d54cbbe3df17e8e5ce68312f483b4155836112aa`, 736
commits past the alpha this project spent eight releases on). Nothing in the extension's own source changed for it:
`scripts/check.sh` reported `syntax OK` against the new headers on the first try, and the suites ran unchanged
against the *released* python package instead of a dev wheel — **212 passed / 13 expected failures / 0 failed, and
`hf_xet_selftest()` 1090 checks / 0 failed**, on `v2.0.0 (Cyanoptera) d54cbbe3df`.

* **Why a release was forced rather than optional**: a loadable binary is tied to exactly one DuckDB identifier,
  and `LOAD` compares the footer's against the running client's. Every client on released 2.0.0 is refused by the
  pre8 artifacts, so a `v2.0.0/` tree had to be built and published; alpha clients keep what they have, because
  publishing only ever adds.
* **The number is `0.0.2`, not `0.0.2-pre9`.** `v0.0.2-pre9` exists — it is the cutover commit, tagged, and CI
  built seven of its eight platforms before the run was cancelled — but a DuckDB that shipped, a suite with no
  unexpected failures, and nine releases of history behind it is as close to "no longer pre" as this project can
  honestly claim. What is *not* reason for the number: nothing in the README's Limitations section moved. The
  dedup flag, `read_blob` of a big file and the uncovered failure branches are still described there, and still
  true.
* **Publishing moved from a dataset repository to a bucket**: `duckdb-extensions/builds`, served at
  `https://hf.co/buckets/duckdb-extensions/builds/resolve`. What differs is the plumbing, not the client. A
  dataset is a git repo, so it serves under `/resolve/main/` (a revision), every upload is a commit, and files are
  stored as pointers; a bucket is object storage, serves under `/resolve/` with no revision at all, and
  `hf buckets sync` writes bytes with no history behind them — the right shape for a tree a client walks by url.
  Measured with a probe file before anything real was published: an anonymous `GET` answers 200 after redirecting
  to a signed CDN url carrying `user_id=public`, a path beginning `.well-known/` is served like any other (which is
  what a key-pinned `INSTALL` has to fetch), and an absent file answers 404 instead of the website's html. Both
  `publish_extension_repo.py` and `verify_published.py` take `--bucket` / `HF_BUCKET` now, and the tree arguments
  are unchanged — the target is the only difference between the two backends, which is what keeps the "same command
  twice, one with `verify_` in front of it" property intact.
* **Both CI workarounds for an untagged alpha retire here.** Eight releases of `override_duckdb_repository` →
  `https://github.com/lhoestq/duckdb`, a one-commit mirror whose only purpose was to carry a tag upstream never
  published, plus a refusal to set `duckdb_tag` (the phase `git tag`s inside the clone and nothing reads the
  result, because extension-ci-tools exports `DUCKDB_VERSION` and DuckDB's Makefile prefers it over
  `OVERRIDE_GIT_DESCRIBE`). 2.0.0 is tagged upstream, so the default clone checks it out and
  `duckdb_version: v2.0.0` is the entire configuration. The Makefile keeps its local pin — that line exists for
  the case of a checkout at a sha, which still stamps one.
* **A released DuckDB is also cheaper to build against**, which matters on a private repository where macOS
  minutes meter at 10×: extension-ci-tools asks `duckdb-staging.duckdb.org/<short sha>/latest_alpha_version.txt`
  for a prebuilt static-libs archive and only compiles DuckDB when that answers 404. It 404s for this project's
  old alpha commit and answers `v2.0.0-alpha49307` for `d54cbbe3df` (and the archive behind it is really there —
  `…/d54cbbe3df/v2.0.0-alpha49307/duckdb/duckdb/github_release/duckdb-static-libs-osx-arm64.tar.gz` answers 200).
  Measured on the pre9 run: `Using prebuilt DuckDB archive: …/duckdb-static-libs-linux-arm64.tar.gz` appears ~50
  seconds into the job, where every job of every release for eight versions spent ~7 minutes compiling DuckDB
  before a line of this extension was built.
* **`HF_EXTENSION_VERSION` and the `scripts/smoke_write.sql` assertion move to `0.0.2`.** Hand-moved, as
  always, and still the same trap: pre8's release proved what happens when only the tag moves — an artifact that
  reports the previous release's number. This one is a *different binary for a different DuckDB*, which makes the
  number worth moving on its own merits.

## 0.0.2-pre8 — cross-repository dedup (M9): an upload can now point at content it did not store

Published from CI run `39325221245` to `lhoestq/duckdb-hf-extension` for `v2.0.0-alpha47600`:
`linux_amd64`, `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`,
`windows_amd64`, `windows_amd64_mingw` — signed at the root, unsigned under `unsigned/`. **Thirty-three served
files** — eight platforms × plain and gzipped, both trees, plus the signed tree's `.well-known` key — re-downloaded
and confirmed byte-identical to CI's bytes, every served extension's footer checked against the directory serving
it, and `INSTALL`/`LOAD` verified over https in all three client forms (key-pinned repository, plain
`INSTALL hf FROM '<prefix>'`, and `unsigned/`). That is `scripts/verify_published.py`, new here: pre7's comparison
was ad hoc and had no view of the served tree, only of files I had remembered to list. The architectures that
cannot run on this machine are covered by that byte comparison and nothing else.

`hf_xet_upload()` asks the CAS whether it already holds each sampled chunk, and for the chunks it does, writes a
pointer instead of the bytes. `SET hf_xet_upload_dedup = false` turns the asking off; it is on by default, and the
byte-identity suites run with it off, because a deduplicated shard is deliberately not the same shard. Four new
result columns report the file rather than the xorb, so they repeat on every row: `deduped_chunks`, `dedup_queries`,
`dedup_hits`, `dedup_shard_failures`.

* **Proven live in both shapes** (`xet_upload` p7/p8, suite now 42 statements). An identical file re-uploaded from a
  *different repository* costs zero xorbs — one result row, `xorb IS NULL`, a shard of pointers, and
  `read_blob('hf://…')` returning the same md5 as the local file the Hub had never seen in that repo. The edited
  file next to it dedups partially: `deduped_chunks >= 1`, uploaded uncompressed bytes strictly below the file size,
  and a shard mixing pointers with new xorbs reads back byte for byte.
* **The upload is two passes now.** Pass A chunks and hashes the whole file; only then can the question "which
  chunks does somebody already store?" be answered, because the answers are keyed to chunk boundaries that pass B
  must not move. Pass B re-reads the unmatched ranges by (offset, length) and re-checks each hash against pass A
  before uploading it. The no-dedup path produces the same shard bytes as before, one segment per xorb, which is
  what lets p1–p6 keep asserting shard identity.
* **Stored shard chunk hashes are keyed; xorb hashes are not.** A dedup query's answer is another client's shard:
  header advertising a 200-byte footer (an *uploaded* shard advertises none), the HMAC key at offset 72 of that
  footer, the file section a lone bookend, and all three lookup tables present with zero entries. Matching therefore
  keys the *query*, never the stored hash, and the table keeps one collection per distinct key — like the
  reference's `KeyedShardCollection`s, where the first collection that resolves the keyed start hash is the answer.
* **`footer bytes ≠ footer hex`, and that distinction is the reason the first attempt shipped at zero.** A key read
  out of a footer is those 32 bytes; the same key parsed from a hex string is those bytes reversed inside every
  8-byte word. `xet-probe hmac` plus 64 generated `HFXetHmacVectors()` pairs pin which of the two
  `HFXetChunkHmac` consumes. The first consumer of this feature hit every property above correctly except that one:
  it queried, got 200, parsed, registered — and matched nothing, reporting `deduped_chunks = 0` while the CAS
  answered every question. Every offline check it was tested against passed. Two things fixed that, in order: the
  measurement (a live CAS response, taken apart field by field), and then `HFXetStoredShardVectors()` — a keyed
  shard exported by the reference's own `export_as_keyed_shard_streaming`, compiled in, so the path from a CAS
  answer to a matched run is now covered with no network in the loop.
* **A generated fixture that is not byte-stable is not a fixture**: the keyed export stamps two validity timestamps
  off the clock, so `xet-probe` zeroes those 16 bytes and `scripts/xet_oracle.py --check` stops reporting drift on
  an unchanged tree. It also exports from the *full* local shard rather than the truncated upload, because the
  export copies the input header verbatim and an upload has `footer_size` zeroed — that would have shipped a shard
  carrying a footer it does not advertise.
* `hf_xet_selftest()` is **1090 checks / 0 failed** (from 972): 64 HMAC pairs, the keyed-collection rules, and 20
  rows over the stored-shard fixture — 8 in the shard reader (parse, key, key-not-the-zero-sentinel, no file
  entries, one xorb, every chunk, stored hash is not the plain one, stored hash is `keyed(key, chunk hash)`) and 12
  in the matcher (register with no file section, know the file's own hashes, whole-xorb run, run starting mid-xorb,
  run breaking at a changed hash, one collection per key, and a shard keyed differently than its own hashes
  answering nothing).
* **Measured, with `scripts/dedup_measure.py`** (237 MB of hex digests, random per run, LZ4 on, one run): an
  identical re-upload costs **0 bytes on the wire and 1.4 s** where the `dedup = false` baseline cost **235 MB and
  21.5 s**; appending 20 000 lines to the file costs **0.2 MB** (18 chunks sent, 3616 pointed); adding a per-row
  column dedups **nothing at all** — 0 of 6889 chunks, because changing every line moves every boundary after it.
  Asking is not the expense: seven queries and zero hits moved the wire time per MB less than run-to-run noise.
  One answered query buys more than a chunk because the answer is *the shard of the file holding that chunk*, which
  names every xorb that file posted — two answers covered all four xorbs of the 237 MB file.
* **Which is also the measurement trap**: content-addressed storage survives `hf_delete`. The first 300 MB run
  reported 1053 chunks pointed in the column case — that was the *previous run's* widened file, same generator, same
  prefix, deleted from the bucket and still in the CAS. A dedup benchmark over deterministic fixtures measures reuse
  of your own last run, so the added column in `dedup_measure.py` carries per-run noise.
* **`windows_amd64` broke on generated output, and only there.** `cl.exe` refuses a zero-length array (`error
  C2466`) where gcc and clang accept it, so the two count-0 tree vectors' empty leaf-hash arrays failed the MSVC
  build while linux, both musl cross-builds, macOS and mingw were green. `v0.0.2-pre7` had carried a placeholder
  string in exactly those spots — typed into the generated file by hand, never into `xet_oracle.py`, so this
  release's regeneration deleted it and `--check` called the result up to date. The generator emits the placeholder
  now and refuses outright to write a file declaring an empty array.
* **Correcting pre7's entry**: it claimed a dispatch passes `reduced_ci_mode: 'disabled'`. Nothing passed it —
  `build.yml` had the sentence in a comment and no such line, so every dispatch through pre7 ran with `fail-fast`
  on. The input is forwarded from the same event test as `opt_in_archs` as of this release.
* **Version constant and the `scripts/smoke_write.sql` assertion move to `0.0.2-pre8`; suites are unchanged at 212
  passed / 13 expected failures / 0 failed, with `hf_xet_selftest()` at 1090.** The need for the bump is the
  lesson: the first pre8 dispatch's artifact loaded here answered `hf_version` `0.0.2-pre7` while its footer said
  `v0.0.2-pre8`. The footer comes from `git describe` at the built commit and decides whether a client accepts the
  binary; `hf_version` is a constant only a release commit moves. Artifacts from a run before the bump are not
  that release's artifacts, whatever their footers claim.
* **The release run could not start for three hours, and nothing in this repo was wrong.** Jobs concluded `failure`
  in 3–6 s with `steps: 0`, no runner name and no log blob (`BlobNotFound`) — including
  `.github/workflows/diagnostic.yml`, two steps added to answer precisely "can Actions schedule a runner at all"
  and kept because it answers it in seconds. The cause was the account's billing. After that was fixed the
  diagnostic went green (`39318023750`) and the release run worked on the first try.
* **Dispatch a release on its tag**: `gh workflow run "Build extension" --ref refs/tags/v0.0.2-pre8`. HEAD carries
  commits after the tag, and `git describe` there stamps `v0.0.2-pre8-1-g45c6090` into every footer — the
  lagging-tag trap, with the tag and the build merely differing rather than one lagging the other.
* **A job can hang rather than fail.** `linux_arm64` sat `in_progress` for 50 minutes where that build takes 6½, and
  the matrix's second wave (`osx_*`, `windows_*`) was never created because it waits on the linux jobs — a five-job
  list forty minutes in is a hung job in wave one, not a scheduler that is broken again. Cancelled through the API
  (`gh run cancel` has no `--yes`, it just prints a usage line) and re-dispatched: green in 24 minutes.
  `verify_published.py` found its own first false red on the way — it stat'ed a temp file before closing it and
  reported the 499-byte key file as 0 bytes, a truncated upload that was never one. A check is code, and code gets
  tested or lies.

## 0.0.2-pre7 — every architecture DuckDB has a runner for

Published from CI run `39153506221` to `lhoestq/duckdb-hf-extension` for `v2.0.0-alpha47600`:
`linux_amd64`, `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`,
`windows_amd64`, `windows_amd64_mingw` — signed at the root, unsigned under `unsigned/`. All sixteen served
binaries (eight platforms × plain/gzipped, both trees) were re-downloaded and confirmed byte-identical to
CI's bytes with the footer reading `v2.0.0-alpha47600` + `v0.0.2-pre7`, and `INSTALL`/`LOAD` verified over
https on this machine's own platform. The architectures that cannot run here — macOS, both Windows flavours,
both musl builds — are covered by that byte comparison and nothing else.

* **The matrix is shaped by the event**: a push builds the three cheap artifacts, a `workflow_dispatch`
  builds everything except wasm. Two extension-ci-tools inputs matter: `opt_in_archs` (musl and `osx_amd64`
  are `opt_in` in its own matrix, so removing them from `exclude_archs` is not enough) and
  `allow_failure_archs` (an experimental cross-compile can ride along; a failing job uploads nothing, so its
  platform directory is simply absent rather than sinking the run).
* **A release run no longer cancels its own healthy jobs.** With `reduced_ci_mode: 'auto'` the shared
  workflow uses `fail-fast`, so one architecture that failed to compile cancelled two healthy siblings. A
  dispatch now passes `reduced_ci_mode: 'disabled'`: eight builds in parallel, each answering for itself.
* **`windows_amd64` joins `windows_amd64_mingw`, and the difference was not cosmetic.** DuckDB composes the
  platform string in `platform.hpp`, and the `_mingw` postfix is applied `#ifdef __MINGW32__` (plus an
  R-tools case). A non-mingw client asks for `…/windows_amd64/`, a directory pre6 published nothing into —
  its mingw artifact could only ever serve a mingw-built DuckDB.
* **A malformed conditional exclusion string, invisible for six pushes.** GitHub binds `&&` tighter than
  `||`, so `event == 'push' || event == 'pull_request' && ';list' || ''` parses as
  `(A || (B && ';list')) || ''`, short-circuits on `A`, and pastes the boolean `true` onto the string:
  `exclude_archs: windows_arm64;wasm_mvp;wasm_eh;wasm_threadstrue`. `wasm_threads` stopped matching, and the
  five expensive architectures — macOS at 10× billing — were not excluded either. A poisoned exclusion list
  cannot fail a build; the check is the `exclude_archs:` line echoed in the matrix job's log, plus the run's
  job list read all the way down.
  * **A finding from earlier in this release is withdrawn**: "explicit opt-ins override exclusions". Its
    whole evidence was musl jobs queueing on a push, which this bug explains — on the dispatch that did build
    musl the suffix was empty, so musl was never excluded and `opt_in_archs` did what it says. The two lists
    no longer overlap, so precedence is genuinely untested rather than answered wrongly.
* **The new architectures found three bugs in our own code** — none of them platform API, all three things
  one toolchain had been excusing: `std::string::find()` results passed to helpers taking `idx_t &`
  (`idx_t` is `uint64_t`, which on Apple's libc++ is `unsigned long long` and so not `string::size_type`;
  six sites, fine on Linux where both spellings are `unsigned long`); BLAKE3's SIMD family chosen from
  `CMAKE_SYSTEM_PROCESSOR`, which is the **host** on an Apple cross-build, so an `osx_amd64` job on an arm64
  runner handed `blake3_neon.c` to an x86-64 compiler (the Linux version of this mistake would have shipped a
  wrong-SIMD binary instead of failing) — two `check_c_source_compiles` probes now ask the compiler; and two
  zero-size arrays plus fourteen files leaning on transitive `<cstring>`/`<cstdio>`/`<cstdlib>`/`<cstdint>`,
  which MSVC does not hand out.
* **A `## Installation` section** written by running each step against the live repository, which is how it
  noticed that the registration persists in `~/.duckdb/extension_repositories/<name>.json` while the
  `allow_extension_repositories` *gate* is per-session and gates installing as well as registering; that the
  key is read from the repository's `.well-known/duckdb-extension-repo.json`; that a repository-installed
  extension must be loaded as `LOAD hf FROM hf_repo`; and that nothing autoloads — the compiled-in-index rule
  that also keeps a bare `INSTALL hf` from working.
* Version constant and the `scripts/smoke_write.sql` assertion move to `0.0.2-pre7`. Suites unchanged:
  **200 passed / 12 expected failures / 0 failed**, selftest 367 checks.

## 0.0.2-pre6 — a bad setting fails where it was set; a release tree is checked against its own binaries

Published for `v2.0.0-alpha47600` on `linux_amd64`, `linux_arm64`, `windows_amd64_mingw` (first release with
more than one architecture), CI run `39133332083`. Nothing shipped under pre5: it was tagged and green, but
these two changes landed before its artifacts went out.

* **`SET hf_xet_compression = 'bg4'` fails at the `SET`** instead of on the next `COPY` — an error a long way
  from its cause. `lz4` (default) and `none`, case-insensitively; `bg4` is a real scheme this build does not
  implement, and `auto` is not accepted as a synonym for `lz4` because in the reference client it means a
  per-chunk decision this writer never takes.
* **A packaging bug that would have made the first Windows release unusable.** The repository script read the
  DuckDB version out of a binary with `strings | grep '^v[0-9]'`, and in a mingw artifact the first match is
  `v0.1H` — so the binary was filed under a directory no client asks for while everything that could report
  reported success. `scripts/ext_footer.py` now reads the footer as the structure `append_metadata.cmake`
  defines (a 534-byte section: signature marker, eight NUL-padded 32-byte fields in reverse order, the
  256-byte signature slot); `make_extension_repo.sh` refuses a version/platform that disagrees with the
  binary; `publish_extension_repo.py` checks every binary against its directory before any network call.
* **The signature gate was passing without testing anything.** `test_install.py`'s tamper check corrupted
  `os.listdir(...)[0]` — `linux_amd64` on a machine installing `linux_arm64`, so the served bytes were never
  touched — and passed on "child exited nonzero", which the child also did when it *succeeded*
  (`SystemExit(<message>)` exits 1). The line printed as the refusal was the child's own "LOADED DESPITE
  TAMPERING". Now every platform directory and both file forms are corrupted, acceptance exits 0, and a
  nonzero exit counts only when it names the signature; verified in both directions, and from the keyless
  repository form too (the README recommends it).

## 0.0.2-pre5 — xorb chunks go out LZ4-compressed

* Chunk payloads are LZ4 frames, on by default, `hf_xet_compression` as the switch: a 2,103,723 byte CSV that
  serialized to 2,105,403 bytes uncompressed now serializes to 889,917 (2.4×). The reference encoder gets
  881,602; the 0.9% gap is free, because two conforming LZ4 encoders need not agree on frame bytes when each
  chunk is decoded from the scheme byte and length stored beside it.
* **It cannot change what content *is***, and that is the property the change rests on: an xorb's hash is the
  merkle root of the *uncompressed* chunk hashes, so all four schemes give the same `xorb_hex` (measured with
  `tools/xet_probe` writing one file four ways). The live suite makes the CAS say it — write with lz4,
  re-upload with `hf_xet_compression = 'none'`, every xorb must answer `inserted = false`. The shard did not
  move: its byte ranges are cumulative *unpacked* lengths.
* **Decodability replaced byte-exactness as the contract**, checked in four places: `AddChunk` decompresses
  what it just compressed before any POST (an uploaded xorb cannot be edited, and its hash looks fine while a
  broken payload makes it unreadable forever); the selftest rebuilds every pinned vector with LZ4 and pins
  hashes, counts, unpacked totals and footer placement — frame bytes deliberately not among them;
  `xet-probe xorb-read` (xet-core's deserializer) reproduces the hashes of a file *our* writer dumped; and the
  live re-upload. 325 checks became 367.
* Not free: the CPU phase goes ~690 → ~370 MB/s on compressible data (~1% of a big upload's wall clock → ~2%),
  and incompressible chunks grow ~16 B each (1.001× over 5 MB of random) because reference clients compress
  *every* chunk and store incompressible ones raw inside a frame. No per-chunk "skip" heuristic on purpose: it
  would make our xorbs a shape nobody else's are, for a rounding error.
* `XXH_NAMESPACE=LZ4_`, `-fvisibility=hidden` and emptying lz4's own visibility macros: the extension exports
  **zero** `LZ4_*`/`XXH*` symbols, which is what keeps DuckDB's own namespaced copies collision-free tomorrow.

## 0.0.2-pre4 — writes go out over several streams; CI's artifacts are loadable

* **A file's xorbs upload with several requests in flight.** `scripts/xet_bench.sh` times the CPU phase alone:
  100 MB chunks in 31 ms, chunk+hash+xorb in 152 ms, against 12–70 s of upload. Six interleaved 200 MB runs:
  concurrency 1 → 29.8/57.9/31.2 s, 3 → 17.6/20.9/18.0 s (~1.7×, not 3×: four xorbs of 64/64/64/8 MiB leave
  the last one exposed either way, and three streams on a shedding link are not three times one stream). 4
  measured the same as 3, so 3 is the default — the flattening point and the smaller memory bill, since an
  in-flight xorb is up to 64 MiB and that cap is simultaneously all the backpressure there is. Content
  addressing licenses the overlap; the shard, which *is* order-sensitive, is built in file order and posted
  after the last POST lands.
* The suite asserts the *identity* of the concurrency, not the speed: write a multi-xorb file at 3, re-upload
  at 1, require every xorb `inserted = false` and the shard `exists`. A bug that moved a boundary or booked a
  segment against the wrong object would produce hashes the CAS had never seen, and it would answer `true`.
* BLAKE3 compiled per ISA (NEON; SSE2/SSE4.1/AVX2/AVX-512 with CPUID dispatch; portable C on MSVC). Worth
  little alone, worth a lot as a side effect: the amd64 artifact now runs all reference checks in CI, so the
  x86 SIMD paths are executed somewhere rather than assumed.
* **CI artifacts were not loadable, and three green runs had proved it.** The selftest job failed with *built
  specifically for DuckDB version `d4355d01c593…`*: DuckDB's Makefile lets the `DUCKDB_VERSION` environment
  beat `OVERRIDE_GIT_DESCRIBE`, extension-ci-tools exports it as the sha passed in `duckdb_version`, and the
  footer's version comes from that variable; `duckdb_tag` creates a git tag nothing downstream reads, and
  `v2.0.0-alpha*` identifiers are tagged nowhere upstream. CI now clones `lhoestq/duckdb` — one commit, tree
  identical to upstream, carrying the tag.

## 0.0.2-pre3 — installable by someone else; a git repo written through Xet

* **Git-backed writes now put content in Xet.** `COPY … TO 'hf://datasets/…'` chunks the file, stores its
  xorbs and shard, and sends *the same* `lfsFile` commit as always — the shard's file entry carries the sha256
  as its metadata extension, the one place the two hash systems are stated together. Proven live on 6 MB of
  `/dev/urandom` (a blob no LFS store had seen): committed, readable, byte-identical.
* `hf_xet_write` (default on) falls back to the plain LFS upload when the content store is unreachable, so a
  dead CAS costs a slow write, not a failed one; if the write fails anyway the error names the earlier Xet
  failure.
* That sha256 is parsed as a `DataHash` — four little-endian word-swapped groups, like every other hash in the
  shard, *not* 32 plain bytes. The wrong reading still yields 32 bytes and a shard the CAS accepts, and leaves
  a file that resolves to nothing; only reference vectors notice, so `xet_oracle.py` emits two and the
  selftest also asserts the flags word.
* `INSTALL hf FROM '…/resolve/main/unsigned'` works, and so does the signed form with a pinned key.
  `publish_extension_repo.py` builds the tree into a Hub dataset and prints the client recipes;
  `test_install.py` takes a url as readily as a directory, so what is verified is what a client fetches.
* The CI matrix builds on push. It had been failing from the start, and an earlier note here that "Actions is
  unavailable" came from reading one endpoint's 403 as a verdict instead of opening a job log. Two real causes,
  both visible only there: DuckDB's CI images configure with a vcpkg toolchain at the extension root and die at
  `project()` without a manifest (hence the empty `vcpkg.json`), and nothing tagged the DuckDB checkout, so
  binaries carried the right code and the wrong version identifier.
* **A skipped sqllogictest reports success**, which is how a suite of reference vectors sat green while testing
  nothing: `hf_selftest.test` opens with `require hf`, and only extensions on DuckDB's compiled-in autoload
  list satisfy that — a third-party name cannot join. `scripts/ci_selftest.sh` now loads a built artifact into
  the `duckdb` binary that artifact ships, compares answers and counts itself, and a `selftest` job runs it on
  the bytes a client downloads. Opening the file paid for itself: its last line counted functions with
  `LIKE 'hf\_%'`, and DuckDB's `LIKE` takes the backslash literally — it asserted `>= 12` about a query
  returning 0, in a file no run had executed.
* `scripts/xet_bench.sh` measures the CPU phase without the network, which reorders what to optimize: a 100 MB
  write spends ~1% of its wall clock on CPU, so *sequential xorb uploads* were next. End-to-end timings on this
  link vary 10×, which is why the project quotes repeatable offline figures and treats single-shot timings as
  anecdotes. macOS builds run on tags and dispatches: private repo, macOS minutes bill 10×, and every job
  compiles DuckDB from source.

## 0.0.2-pre2 — a file at the end of the shard

* **`COPY … TO 'hf://buckets/me/x.parquet'` works, through Xet.** A bucket file *is* Xet content, so this is
  the honest bucket write: chunk, store xorbs, store the shard, register by hash (`addFile` + `xetHash`).
  Readable at the url, verified `md5`-for-`md5`, including a 230 MB uncompressed file whose chunks span several
  xorbs (the Hub rebuilds read ranges from the shard). `hf_upload(local, 'hf://buckets/…')` takes the same route.
* `hf_xet_shard.cpp` writes the session shard — hand-rolled little-endian, not protobuf — byte-identical to
  `xet-core`; its uploaded form is the shard cut at `file_lookup_offset` with `footer_size` rewritten to 0,
  which is what makes re-uploads dedup (the timestamp is in the dropped part). Two coordinate systems cost a
  debugging cycle: a segment indexes chunks **inside its xorb** while its verification hash covers a **file-wide**
  range — invisible until a file spans xorbs, and the production CAS caught it (`400 (19) invalid shard failed
  to verify file`).
* `hf_xet_upload` gained `publish := true` and `xet_hash, shard_bytes, shard_hash, shard_result, published`.
  `xetHash` is the word-swapped hex of the file hash — confirmed by reproducing a real Hub file's hash.
* A trailing slash on a write target is refused rather than obeyed: `COPY … TO '…/dir/'` used to create a file
  *named* `dir`, for buckets and git-backed repos alike. Local DuckDB refuses the same statement.
* Content past the 64 MiB xorb limit splits into several xorbs; two bugs hid there. `Finish()` cleared the chunk
  list but not the byte counter, so the second xorb of a big file was born full; flushing on "is it full yet" let
  an object end one chunk over the limit. The builder is now asked `WouldOverflow(len)` before a chunk is added,
  and the limits apply to *uncompressed* bytes.
* `scripts/smoke_xet_upload.sh` runs against the production CAS — the only suite that can fail on a wrong byte,
  since the CAS recomputes every xorb hash. Fresh content each run, so `inserted` is answered for the right reason.
* `hf_xet_selftest()` 226 → 296 checks (whole serialized byte streams of reference xorbs; chunks still
  uncompressed, which is a scheme of the format rather than a stub).

## 0.0.2-pre1 — the start of Xet

* `hf_xet_selftest()` — 152 checks that the extension's Xet hashing and chunking agree byte-for-byte with
  `xet-core`: leaf/chunk hashes, file and xorb merkle roots, boundary lists, mean-tree shapes from xet-core's own
  correctness table, interior-node hashes, and the `hex()`/`base64()` renderings. Pure computation: no token, no
  network.
* Reference vectors are generated (`scripts/xet_oracle.py` → `src/hf_xet_vectors.cpp`), never hand-written, and
  committed so CI needs no cargo; `--check` fails when a newer xet-core disagrees with what is stored.
* Two silent bugs, both caught only by generated vectors: a key constant transcribed wrongly (every hash wrong,
  all of them plausible), and the chunker's mask built from the gear hash's low bits instead of its high ones —
  which cuts at almost every byte *and still reproduces the reference boundaries on constant input*.
* `hf_bucket_copy(…, hash_wait := n)`: how long to wait for the Hub to finish hashing a source file committed
  moments earlier (default 30s, `0` to refuse). Copies of already-hashed files no longer pause at all.

## 0.0.1 — first release

`hf://` behaves like `s3://` in both directions, for every repo type the Hub serves.

* **Reading**: `SELECT * FROM 'hf://…'`, `read_parquet`/`csv`/`json`/`blob`/`text`, `glob`, `**`; `datasets`,
  `models`, `spaces`, `kernels`, `buckets`, with `@revision` and the short `hf://user/repo/file` form; range reads
  on `/resolve/` with a shared per-file window cache (grid-aligned read-ahead, coalesced windows, parallel range
  parts, retention beyond the handle under a process budget). A full scan of a 287 MB parquet file fetches
  **1.000×** the file and matches or beats `httpfs`. ETag/`version_tag` + mtime so DuckDB's external file cache
  can validate entries.
* **Writing**: `COPY … TO 'hf://…'` for parquet (incl. zstd), csv, json; overwrite; empty results; `PARTITION_BY`;
  unicode and spaces in paths; nested paths. The Hub's own protocol — `preupload` → git-LFS *basic* or *basic
  multipart* → ndjson commit. `datasets`, `models`, `spaces`, `kernels`; `hf_upload`, `hf_delete`, per-call and
  session commit messages; create-on-write (private, spaces as `sdk = static`), with a 404 that says so; commits
  invalidate the read cache for the repository they changed.
* **Buckets without a Xet client**: reads, listings, globs, deletes; `hf_bucket_copy` moves content already on the
  Hub with a server-side `copyFile` (files, folders, globs, bucket-to-bucket), git blobs coming back `skipped`.
  `COPY … TO 'hf://buckets/…'` was refused here, with a message naming the two routes that did work — the write
  path that genuinely needs Xet chunk hashes, which 0.0.2 exists to build.
* **Observability**: `hf_status()`, `hf_read_stats(reset :=)`, `hf_read_cache_clear()`, `hf_list_revisions`,
  `hf_list_refs`, `hf_create_repo`; `HF_READ_DEBUG=1` / `HF_WRITE_DEBUG=1`; 10 settings; the `huggingface` secret
  type shared with httpfs.
* **Distribution**: `scripts/make_extension_repo.sh` (serveable `<version>/<platform>/` tree, optionally signed),
  `INSTALL hf FROM '<prefix>'` verified against a local repository unsigned and signed with tampering refused, and
  `.github/workflows/build.yml` building the matrix through DuckDB's reusable workflow.
* **Tested**: `scripts/run_all.sh` against the live Hub — 140 passing checks plus 7 asserting a refusal says what
  it should.
* **Known gaps then**: no Xet uploads; `read_blob` of a large file still behind httpfs; no repository delete from
  SQL (the Hub wants a JSON body on a `DELETE`, DuckDB's transport sends none).
