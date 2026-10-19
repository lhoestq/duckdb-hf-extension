# Working state

Resume notes for whoever picks this up next (usually me, after a context compaction). The *user-facing*
story is `README.md`, the per-release story is `CHANGELOG.md`, the protocol story is `docs/XET.md`, and the
long form of every decision — the failed attempts and the measurements behind it — is in `git log` bodies.
This file only holds what is true right now and what to do next.

## Position — 2026-10-19, ~19:05 UTC

**0.0.2-pre7 is published, live, for all eight DuckDB client platforms.** **0.0.2-pre8 (M9, cross-repository
dedup) is committed and tagged, and its release run cannot start** — Actions is currently failing this repo's
jobs before they reach a runner, see *The release is blocked on Actions, not on code* below.

Tag `v0.0.2-pre8` is at `bcbe681`. Dispatch **on the tag**, `gh workflow run "Build extension" --ref refs/tags/v0.0.2-pre8`,
and not on `main`: the footer version is `git describe` at the built commit, so a dispatch on `main` (now ahead of
the tag by the diagnostic-workflow commit) would stamp `v0.0.2-pre8-1-g45c6090` and every client would refuse the
binary. That is the lagging-tag trap wearing a cap — the tag does not have to lag HEAD to fall into it, it only has
to differ from what was built.

* **The release is blocked on Actions, not on code.** Since ~18:20 UTC jobs in this repo conclude `failure` in
  3–6 seconds with `steps: 0`, no runner name, and no log blob (`BlobNotFound`). A build error compiles and
  writes a log; a test error has steps; neither description fits. The evidence, in order: run `39299553138`
  built all eight platforms green and lost only the reference-checks job that way; run `39304739130` lost even
  `Generate matrix`; and `.github/workflows/diagnostic.yml` — two steps, `uname -a`, `ubuntu-24.04` — failed the
  same way in 5 s (`39305064645`). Nothing in a repo can make `uname` fail to reach a machine, so this is
  account-level (an Actions spending limit is the likely one: a private repo, every job compiles DuckDB) or
  GitHub-side. The annotations and check-suites endpoints 404 with this token, so the reason string is not
  readable from here — read it in the Actions UI.
  **Before dispatching another matrix run, dispatch `389744541` (`gh workflow run 389744541`) and wait for it to
  go green.** It costs seconds; a matrix run costs ~25 minutes of metered runner time and fails the same way.
* What is *done* for pre8, so a resume does not redo it: `HF_EXTENSION_VERSION` = `0.0.2-pre8` and the
  `smoke_write.sql` assertion with it; suites re-run green on a fresh build; `scripts/xet_oracle.py` fixed so MSVC
  can compile the generated vectors again; `reduced_ci_mode` actually forwarded on a dispatch; tag created and
  pushed. What is *not* done: any artifact published, `README.md`'s release line verified live, CHANGELOG heading
  renamed from *Unreleased* — all three wait for a green run off the tag.

* Live at `https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/` (signed) and
  `…/resolve/main/unsigned/`, from CI run `39153506221`, for `v2.0.0-alpha47600`: `linux_amd64`,
  `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`, `windows_amd64`,
  `windows_amd64_mingw`. Sixteen served binaries byte-compared against CI's; `INSTALL`/`LOAD` verified over
  https here. **Everything not `linux_arm64` is verified by byte comparison only** — this machine cannot
  execute it, and a footer plus a sha256 is not a load test.
* Suites **212 passed / 13 expected failures / 0 failed** (xet 12, xet_upload 42, read 55, write 57, models 24,
  buckets 22); `hf_xet_selftest()` **1090 checks / 0 failed** — 471 dedup-eligibility rows, 64 HMAC rows, the shard
  rows, 8 rows over the keyed stored-shard fixture in the shard reader plus 12 in the dedup matcher, and one
  non-vacuous `sampled-eligible chunks` row
  (`splitmix64 n=4000000 seed=11`, 1 of 1) which is what keeps the persisted chunk flag file-start-only. The expected
  count lives in `test/sql/hf_selftest.test`; `scripts/ci_selftest.sh` reads it from there, so a check that silently
  disappears is a failed run rather than a quieter green one.
* **M9 is shipped, and proven live in both shapes.** p7: content stored against a datasets repo, never published,
  then the same bytes published from a bucket repo — one row, no xorbs, `deduped_chunks > 0`, shard of pointers, and
  `read_blob` over `hf://` returns identical bytes. p8: the same file with two rows appended — partial dedup, the
  uploaded `uncompressed_bytes` strictly below the file size, mixed shard reads back byte-identical.
* **M9 is measured** (`scripts/dedup_measure.py 300`, table in `docs/XET.md`): a 237 MB re-upload of identical
  bytes costs **0 bytes and 1.4 s** against the baseline's **235 MB and 21.5 s**; appending 20 000 lines costs
  **0.2 MB**; adding a per-row column dedups **0 of 6889 chunks**. One answered query is not one chunk but a whole
  file's shard — two answers named all four xorbs of the 237 MB file.
* **The reason it took two attempts: stored shard chunk hashes are keyed, xorb hashes are not.** A CAS dedup answer
  is another client's shard — footer 200 bytes, `chunk_hash_hmac_key` at offset 72 of it, file section a lone
  bookend, all three lookup tables empty — and every chunk hash inside it is `keyed_hash(footer_key, hash)`. The
  first consumer asked, parsed, registered and matched nothing: offline-green, live-zero. `docs/XET.md` → *M9* has
  the measurements, the footer-bytes-vs-hex trap, and the compiled-in keyed fixture (`HFXetStoredShardVectors()`,
  exported by `export_as_keyed_shard_streaming` through `xet-probe shard 0 "" <key>`) that covers it offline.
* **M9's producer half is settled by measurement, not inference.** The CAS indexes a file's first chunk *and*
  one hash in 1024, computing the second itself: 4 hits out of 4 eligible chunks (3 of them with the persisted
  flag clear), 13 controls answered 404. `scripts/cas_dedup_probe.py` reproduces it and prints its own verdict;
  the rule is replayed from `xet-probe` vectors, never re-derived in C++.
* The CI matrix fix (`e5bb74d`) was verified against the live matrix: push run `39157627780` built exactly
  `linux_amd64`, `linux_arm64`, `windows_amd64_mingw` + the reference-checks job, green.

## Next, in order

1. **Release 0.0.2-pre8.** First `gh workflow run 389744541` (the diagnostic) until it is green; then
   `gh workflow run "Build extension" --ref refs/tags/v0.0.2-pre8`, and once it is green:
   `RUN=<run> DUCKDB_EXTENSION_SIGNING_PK="$(cat ~/.config/duckdb-hf/signing.pem)" scripts/make_extension_repo_from_ci.sh dist/repo-signed`,
   the same tree unsigned into `dist/repo`, then
   `HF_REPO=lhoestq/duckdb-hf-extension scripts/publish_extension_repo.py dist/repo-signed --unsigned-tree dist/repo`.
   Then byte-compare all sixteen served binaries and re-do the https `INSTALL`/`LOAD` check pre7 got, rename the
   CHANGELOG's *Unreleased* heading to `0.0.2-pre8`, and note the run id in STATE. The check job that runs
   `scripts/ci_selftest.sh` may still refuse to schedule; the equivalent check by hand is to download the
   `linux_arm64` artifact (the one platform this machine executes) and `LOAD` it into the venv duckdb.
   Nothing in M9 changes a byte for an upload that runs with `hf_xet_upload_dedup = false`, which is a claim worth
   re-testing on a pre-release rather than asserting.
2. **Shard-driven read prefetch** — walk a shard's chunk table instead of one range request per window; this
   is the fix for the `read_blob`-of-a-big-file gap that is still behind httpfs.
3. Optional: decide whether CI should `strip` the mingw artifact (73 MB unstripped vs 31 MB for the MSVC
   build), and add macOS install notes if anyone ever reports from one.
4. Failure-path coverage only where a safe test seam exists — see the *Limitations* note in the README about
   why the CAS-failure and LFS-fallback branches are uncovered by choice.

## Environment

| item | value |
|---|---|
| host | DGX Spark, `aarch64` / `linux_arm64`, Debian 12, gcc 12.2, 20 cores |
| repo | `/home/quentin/hf/hf-duckdb-extension` (private remote `lhoestq/duckdb-hf-extension`) |
| venv | `. .venv/bin/activate` — has `cmake`, `ninja`, `huggingface_hub`, `duckdb` 2.0.0.dev (no `numpy`) |
| duckdb | `v2.0.0-alpha47600`, source `d4355d01c59362d57036ad377735dd5340276a5b`, *Cyanoptera*, vendored at `/home/quentin/hf/vendor/duckdb` |
| xet-core | `/home/quentin/hf/xet-core` at `e922f74`; `cargo` is at `~/.cargo/bin` and **not on PATH**; the probe binary is `tools/xet_probe/target/release/xet-probe` |
| token | `~/.cache/huggingface/token` (user `lhoestq`); `gh` authenticated as `lhoestq` |
| signing key | `~/.config/duckdb-hf/signing.pem`, fingerprint `sha256:f007c8016bc2f3a2…dbfce` |

```bash
make release                                    # defaults DUCKDB_VERSION=v2.0.0-alpha47600 (see the traps below)
cp build/release/extension/hf/hf.duckdb_extension dist/    # run_all.sh does this; a smoke_*.sh on its own does not
python3 scripts/try.py < scripts/smoke_xet.sql  # prints the artifact it loads; there is no `duckdb` CLI in the venv
bash scripts/run_all.sh --build            # every live suite; or one smoke_*.sh
bash scripts/smoke_xet.sh                  # the offline reference checks, no token needed
python3 scripts/xet_oracle.py --check      # drift check against xet-core (needs cargo on PATH)
scripts/ci_selftest.sh build/release/extension/hf/hf.duckdb_extension   # what CI runs on an artifact
```

## Facts that bind the next change

* **DuckDB, not this extension**: nothing autoloads (`LOAD hf FROM hf_repo` is a line per session, and the
  `allow_extension_repositories` gate — not the registration — is what expires); a skipped sqllogictest
  (`require hf`) reports success; `duckdb_extensions().extension_version` is `git describe` at build time;
  `LIKE` takes backslashes literally; `string::find()` returns `size_type`, which on Apple's libc++ is not
  `idx_t` (`uint64_t` = `unsigned long long` there).
* **CI**: builds from `main` HEAD, not from a tag. `github.event_name != 'workflow_dispatch' && ';list' || ''`
  is the only safe shape for an event-conditional `exclude_archs` — GitHub binds `&&` tighter than `||`, so
  naming two events pastes the boolean `true` onto the string and excludes nothing, green. Pass
  `reduced_ci_mode: 'disabled'` on a dispatch or `fail-fast` cancels healthy siblings. Read the matrix job's
  echoed inputs and a run's full job list, never just its conclusion. `gh run cancel` prompts — use
  `gh api -X POST .../actions/runs/<id>/cancel`.
* **Portability**: MSVC has no `unistd.h`/`getpid()` (guard with `_WIN32` + `process.h`/`_getpid()`), refuses
  zero-size arrays, and does not hand out `memcpy`/`snprintf`/`getenv`/`uint64_t` transitively.
  `CMAKE_SYSTEM_PROCESSOR` is the *host* on Apple cross-builds — probe the compiler instead.
* **Xet**: see `docs/XET.md`. The short version — hashes are keyed BLAKE3 with word-swapped hex; xorb limits
  are on uncompressed bytes; segment chunk indices are xorb-local while verification hashes are file-wide;
  `chunk_byte_range_start` is unpacked space; the sha256 ext is a `DataHash`, not 32 bytes; the CAS takes its
  own bearer and nothing else, refreshed 30 s early; FileOpeners are thread-affine, so workers get one without.
  For dedup one more, and it cost a cycle: **footer bytes are not footer hex**. A key read out of a footer means
  those 32 bytes; a key parsed from a hex string means them reversed inside every 8-byte word; `HFXetChunkHmac`
  consumes `.bytes`. Stored chunk hashes are keyed and xorb hashes are plain, and only the *query* side is keyed
  (`HFXetKeyedChunkHash` owns the zero-key "stored as given" sentinel).
* **Local build, the two traps that produce a confident wrong answer.** (1) An artifact built against the vendored
  shallow DuckDB (HEAD `d4355d01c5`, no tags) is stamped with that sha unless `DUCKDB_VERSION` is set at configure
  time, and the pre-release Python build then refuses to LOAD it; the Makefile now defaults the version, and a CI
  pin still wins. (2) `scripts/try.py` loads `dist/hf.duckdb_extension`, and only `run_all.sh` copies a fresh build
  there — a `smoke_*.sh` run on its own grades whatever `dist/` happens to hold. `try.py` prints path, bytes and
  mtime for exactly this reason.
* **Tools**: `numpy` is not in the venv — use `cur.description` / `fetchall()`, never `fetchdf()`.
  `hf_xet_write` is not connect-time config (SET it after LOAD). `hf_xet_upload`'s `scope` must be a full
  `hf://` URL carrying the path, and `publish` is bucket-only (git-backed repos throw). Read a built
  extension's footer with `scripts/ext_footer.py`, never `strings | grep`.
* **A fresh shell has neither `cargo` nor `hf` nor `cmake` on PATH.** `export PATH="$HOME/.cargo/bin:$PWD/.venv/bin:$PATH"`
  covers all three — the venv carries `cmake`, `ninja` and `hf` — after which
  `cmake --build build/release --target hf_loadable_extension -j 8` is the fast rebuild, and it keeps the
  `DUCKDB_VERSION` the cache was configured with.
* **A graded SQL suite parses line by line**, so a section header must be a `--` comment: `//===…===//` is SQL to
  the runner and swallows the statement under it — which is how a `SET hf_xet_upload_dedup = true` once vanished
  from a suite while every assertion still reported OK.
* **Deleting a file does not delete its chunks.** The CAS is content-addressed and outlives the file that referenced
  it, so a dedup benchmark built from a deterministic generator measures reuse of your own previous run — which is
  how a "column added to every row" case once came out at 1053 chunks shared instead of the 0 it really has.
  Noise per run in anything the benchmark expects *not* to find.
* **Two version numbers, one of them hand-moved.** The footer's is `git describe` at the built commit and decides
  whether a client accepts the binary; `hf_version` in `hf_status()` comes from `HF_EXTENSION_VERSION` in
  `src/include/hf_common.hpp` and only moves when a release commit moves it, with the `smoke_write.sql` assertion
  beside it. Bump both or ship a release that reports the previous one — an artifact from the first pre8 dispatch
  loaded here and answered `0.0.2-pre7` while its footer said `v0.0.2-pre8`.
* **The suites load `dist/`, not `build/`.** `scripts/run_all.sh` copies a newer build artifact over it now; before
  that the copy happened only under `--build`, so a direct `cmake --build` left the suites testing the previous
  binary — one changed string in the version constant, and `assert_version` failed against a file that no build had
  produced. Same reason `try.py` prints path, size and mtime before it loads anything.
* **A CI job that fails with `steps: 0`, no runner and no log blob is not a build failure.** Nothing in a commit can
  make a job never reach a machine; `gh workflow run 389744541` (`.github/workflows/diagnostic.yml`, two steps,
  seconds to run) says whether Actions can schedule anything, and until it goes green a 25-minute matrix run is not
  the experiment to run.

## Rules

* Everything under `/home/quentin/hf/hf-duckdb-extension`, vendored deps in `../vendor`. Never commit tokens.
* Long builds under `nohup … &` into `build/logs/` so they survive a compaction.
* A milestone is not done until the suites run and `CHANGELOG.md` says what it costs. Quote repeatable
  offline measurements; treat single-shot network timings as anecdotes (this link varies 10× run to run).
* Doc-only changes commit with `[skip ci]`. Tags are annotated and pushed explicitly
  (`git push --follow-tags` silently skips a lightweight one).
* Scratch repos: `lhoestq/playground` is kept and reused; `hf-smoke-staging` and `hf-ext-write-probe` are
  created per run and deleted by `scripts/delete_repo.sh`. The `dedup_probe/` fixtures stay put — the M9
  sweep is run against them.
