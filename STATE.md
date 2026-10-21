# Working state

Resume notes for whoever picks this up next (usually me, after a context compaction). The *user-facing*
story is `README.md`, the per-release story is `CHANGELOG.md`, the protocol story is `docs/XET.md`, and the
long form of every decision — the failed attempts and the measurements behind it — is in `git log` bodies.
This file only holds what is true right now and what to do next.

## Position — 2026-10-21, ~20:00 UTC

**`0.0.2` is published and live for DuckDB `v2.0.0`.** CI run `39624699278`, dispatched on the tag ref
(`gh workflow run "Build extension" --ref refs/tags/v0.0.2`), built all eight platforms in 26m48s with footer
`duckdb_version=v2.0.0 ext_version=v0.0.2 signed=yes`; the tree went to the bucket, `scripts/verify_published.py`
fetched **33 served files / 0 problems**, and `scripts/test_install.py` installed + loaded over https from the long
host, the short `hf.co` host and the unsigned tree. Tag `v0.0.2` (`ac12778`) points at `d177cfb`, the commit that
moves `HF_EXTENSION_VERSION`, which is what a release tag has to be. Built locally too, same numbers:
**212 passed / 13 expected failures / 0 failed** and `hf_xet_selftest()` **1090 / 0** on
`v2.0.0 (Cyanoptera) d54cbbe3df`, with no change to the extension's source — the 736 commits between
`v2.0.0-alpha47600` and the release broke nothing here. `v0.0.2-pre9` is also tagged: it is the cutover commit
(2.0.0 target, mirror retired) and CI built seven of its eight platforms before the run was cancelled in favour of
this one. It is not published and nothing references it as a release.

**Publishing moved to a bucket**: `duckdb-extensions/builds`, so the client prefix is
`https://hf.co/buckets/duckdb-extensions/builds/resolve` — no `/main`, because a bucket has no revision. Probed
before publishing anything real: anonymous `GET` of a bucket path → 200 after a redirect to a signed CDN url with
`user_id=public`; a `.well-known/` key path is served like any other path; an absent file is a real 404. Both
`publish_extension_repo.py` and `verify_published.py` now take `--bucket` / `HF_BUCKET` (`--repo` / `HF_REPO` still
works, and the dataset keeps every file pre1–pre8 put there — nothing deletes on either backend).

What moved for 2.0.0: `Makefile`/`scripts/run_all.sh` default to `DUCKDB_VERSION=v2.0.0`, `../vendor/duckdb` is a
checkout of the `v2.0.0` tag, `.github/workflows/build.yml` passes `duckdb_version: v2.0.0` **and no longer passes
`override_duckdb_repository`** — the one-commit mirror at `https://github.com/lhoestq/duckdb` existed only to carry
a tag upstream never published, and upstream publishes 2.0.0. It stays for the artifacts it built. The other thing
that changed is cost, and it is measured now, not predicted: on the pre9 run a job printed
`Using prebuilt DuckDB archive: https://duckdb-staging.duckdb.org/d54cbbe3df/v2.0.0-alpha49307/…/duckdb-static-libs-linux-arm64.tar.gz`
~50 seconds in, where every pre-2.0 job compiled DuckDB for ~7 minutes first. extension-ci-tools asks
`duckdb-staging.duckdb.org/<short sha>/latest_alpha_version.txt` and builds from source only when that 404s — it
404s for the old alpha's commit and answers for `d54cbbe3df`.

**That archive is also why `hf_status().duckdb_version` says `v2.0.0-alpha49307` on the released binaries, and
they were published anyway.** The commit tagged `v2.0.0` was staged for packaging as `v2.0.0-alpha49307`, and
`DuckDB::LibraryVersion()` — `src/hf_functions.cpp:437`, the value in that column — is compiled from the archive's
`version.cpp`: `nm` shows `_ZN6duckdb6DuckDB14LibraryVersionEv` as a local (`t`) symbol inside `hf.duckdb_extension`,
and `strings` finds both labels in a CI artifact where a source build of the tag carries only `v2.0.0`. Asking the
host is not an option: the released static CLI exports zero `duckdb_*` dynamic symbols and `DatabaseManager` has no
version member, so no extension can read the version of the process that loaded it. The load never consults that
string — the footer `INSTALL`/`LOAD` compare says `v2.0.0` — and CI run `39662681195` proves the released bytes pass
every offline check (1090/0, 14 functions) in a released 2.0.0 client. Decision, with Quentin: publish and document
(README Limitations leads with it) rather than fork `duckdb/extension-ci-tools` to set `CI_FETCH_PREBUILT_DUCKDB`
`false` — hard-coded in `_extension_distribution.yml`, not an input, and a caller cannot set a called reusable
workflow's `env` — or shadow `DuckDB::LibraryVersion()` (ODR violation). Un-pin path if it ever matters: upstream PR
adding `fetch_prebuilt_duckdb` as an input, then `ci_tools_version` moves off `4a5752ce…`.

**CI now tests artifacts with a client that exists.** The 0.0.2 selftest job was red because the `duckdb` inside
`test-support.tar.gz` carries the staged label and refuses a footer that says `v2.0.0` — a labelling difference in
DuckDB's pipeline rendered as a red job on a green artifact. `scripts/pick_test_client.sh` reads the footer, fetches
`duckdb-cli-<platform>.tar.gz` from that DuckDB release (the binary users run), and falls back to
`test-support.tar.gz` only when there is no release asset, i.e. for pre-release identifiers. The workflow
`.github/workflows/selftest_artifact.yml` runs the same checks against *a finished run's* artifact from a workflow
dispatch (input: `run_id`) — 19 s, and it is how a published release gets re-checked without rebuilding eight
platforms. Note the
artifact-scoping rule found on the way: `actions/download-artifact@v4` cannot see another workflow file's artifacts
and answers "Artifact not found", so that job lists and downloads through the REST API.

**The one thing that failed on the pre9 run was not this repo.** `distribution / linux_arm64` died inside
`docker build` of the toolchain image: `yum groupinstall "Development Tools"` could not fetch EPEL8 metadata
(`Curl error (60): Peer certificate cannot be authenticated` for `mirrors.wcupa.edu/epel/8/…/repomd.xml`, then
*All mirrors were tried*). Steps ran, a runner existed, the failure is upstream third-party infra — so the response
is `gh run rerun <run> --failed` (one matrix job, minutes now that DuckDB is downloaded), not a code change. Note
that this is a different signature from the `steps: 0` billing failure below: that one never reached a runner.

### 0.0.2-pre8, still live for `v2.0.0-alpha47600`

**0.0.2-pre8 stays published and live, for all eight DuckDB client platforms.** It is M9 (cross-repository dedup) plus
the MSVC generated-file fix and the `reduced_ci_mode` fix, built by CI run `39325221245` — dispatched **on the tag
ref**, `gh workflow run "Build extension" --ref refs/tags/v0.0.2-pre8`, which is the only way to get the footer to
say `v0.0.2-pre8`: HEAD is docs ahead of `bcbe681`, and a dispatch there would have stamped
`v0.0.2-pre8-1-g45c6090`, which every client refuses. The tag does not have to lag HEAD to fall into the
lagging-tag trap; it only has to differ from what was built.

* Live at `https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/` (signed) and
  `…/resolve/main/unsigned/`, for `v2.0.0-alpha47600`: `linux_amd64`, `linux_arm64`, `linux_amd64_musl`,
  `linux_arm64_musl`, `osx_amd64`, `osx_arm64`, `windows_amd64`, `windows_amd64_mingw`. **33 served files** —
  8 platforms × plain and gzipped, both trees, plus the signed tree's `.well-known` key — fetched over https and
  sha256-compared against CI's artifacts, and each served extension's footer checked against the directory it is
  served from. That is `scripts/verify_published.py`, new this release, and it is now the step between publish and
  "live": what it replaces was a one-off list of urls I had remembered to check, not a walk of the served tree.
* Verified over https three ways with `scripts/test_install.py`: the key-pinned repository form (which asserts the
  pinned fingerprint is ours), the README's `INSTALL hf FROM '<prefix>'`, and `unsigned/` — each followed by a real
  `hf_ls`. The binary a client installs today reports `hf_version = 0.0.2-pre8` and 1090/1090 selftest. CI's own
  `reference checks on the built artifact (linux_amd64)` job ran on this run and said the same 1090.
  **Everything not `linux_arm64` is verified by byte comparison and footer only** — this machine cannot execute it,
  and a footer plus a sha256 is not a load test.
* **That run cost five attempts, and three of them were never this repo's fault.** Three failed with jobs
  concluding `failure` in 3–6 s, `steps: 0`, no runner and `BlobNotFound` logs — the account's billing, cleared by
  paying it, not by changing anything here. Keep workflow `389744541` (`.github/workflows/diagnostic.yml`) for that
  question: it costs seconds where a matrix run costs ~25 minutes of metered time. One more attempt hung: `linux_arm64`
  sat `in_progress` for 50 minutes where the same build takes 6½, and the second matrix wave was never created because
  it waits on the linux jobs — a hung job is cancelled with
  `gh api -X POST repos/<owner>/<repo>/actions/runs/<run>/cancel` (`gh run cancel <run> --yes` is a usage error; there
  is no `--yes`) and the tag re-dispatched.
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

1. **The release recipe, proven end to end twice (pre8, 0.0.2) and unchanged for the next identifier.** Bump
   `HF_EXTENSION_VERSION` (`src/include/hf_common.hpp`) and the `scripts/smoke_write.sql` assertion *together* —
   hand-moved, and pre8 proved what happens when only the tag moves — commit with `[skip ci]`, annotate the tag on
   *that* commit, push both, then `gh workflow run "Build extension" --ref refs/tags/<tag>`: dispatch on the tag,
   never on HEAD, because the footer version is `git describe` at the built commit. Once green —
   `RUN=<run> DUCKDB_EXTENSION_SIGNING_PK="$(cat ~/.config/duckdb-hf/signing.pem)" scripts/make_extension_repo_from_ci.sh dist/repo-signed`,
   the same tree unsigned into `dist/repo`, then
   `HF_BUCKET=duckdb-extensions/builds scripts/publish_extension_repo.py dist/repo-signed --unsigned-tree dist/repo`,
   then `scripts/verify_published.py` with the same two arguments, then `scripts/test_install.py` on the served
   urls. A hung job is cancelled through the API and the tag re-dispatched; a job that dies inside DuckDB's own
   docker/toolchain steps (an EPEL cert flake) gets `gh run rerun <run> --failed`. Two traps this run added: the
   tree you publish must hold one directory per identifier (`publish_extension_repo.py` refuses stale ones, which
   is what caught `v2.0.0-alpha47600` leftovers sitting in `dist/` here), and `dist/` is not cleared between runs
   for that reason.
2. **The `v2.0.0-alpha49307` label, if it ever has to go.** Documented in README Limitations and accepted for
   0.0.2; it changes no byte and no load. To make `hf_status()` match: get `duckdb/extension-ci-tools` to accept
   `fetch_prebuilt_duckdb` as an input to `_extension_distribution.yml` (today `CI_FETCH_PREBUILT_DUCKDB: true` is
   hard-coded at line 247 of the pinned sha *and* of `main`, and a caller cannot set a called workflow's `env`),
   then point `ci_tools_version` at it and the artifacts come out labelled like official ones. Re-measure the
   wall clock when doing it — the prebuilt archive did not make a run faster (26m48s vs pre8's 23m39s from source).
3. **A live A/B for the dedup flag.** What is covered today: with `hf_xet_upload_dedup = false` the *offline*
   byte-identity vectors still pass (that is how `docs/XET.md` describes the suite, and the oracle generator has no
   dedup path, so it is a real no-dedup baseline), and live the flag is covered only by counters
   (`assert_nothing_was_pointed_at`, `assert_dedup_was_asked`). What is not covered: one run uploading the *same*
   file twice, flag on then off, comparing what each reads back. A deduplicated shard is deliberately a different
   shard, so the assertion is about the bytes at the end of a read, not the bytes written — which is exactly the
   promise a user of the flag relies on.
4. **Shard-driven read prefetch** — walk a shard's chunk table instead of one range request per window; this
   is the fix for the `read_blob`-of-a-big-file gap that is still behind httpfs.
5. Optional: decide whether CI should `strip` the mingw artifact (73 MB unstripped vs 31 MB for the MSVC
   build), and add macOS install notes if anyone ever reports from one.
6. Failure-path coverage only where a safe test seam exists — see the *Limitations* note in the README about
   why the CAS-failure and LFS-fallback branches are uncovered by choice.

## Environment

| item | value |
|---|---|
| host | DGX Spark, `aarch64` / `linux_arm64`, Debian 12, gcc 12.2, 20 cores |
| repo | `/home/quentin/hf/hf-duckdb-extension` (private remote `lhoestq/duckdb-hf-extension`) |
| venv | `. .venv/bin/activate` — has `cmake`, `ninja`, `huggingface_hub`, `duckdb` 2.0.0 released (reports `v2.0.0`, no `numpy`) |
| duckdb | `v2.0.0`, source `d54cbbe3df17e8e5ce68312f483b4155836112aa`, *Cyanoptera*, vendored at `/home/quentin/hf/vendor/duckdb` |
| xet-core | `/home/quentin/hf/xet-core` at `e922f74`; `cargo` is at `~/.cargo/bin` and **not on PATH**; the probe binary is `tools/xet_probe/target/release/xet-probe` |
| token | `~/.cache/huggingface/token` (user `lhoestq`); `gh` authenticated as `lhoestq` |
| signing key | `~/.config/duckdb-hf/signing.pem`, fingerprint `sha256:f007c8016bc2f3a2…dbfce` |

```bash
make release                                    # defaults DUCKDB_VERSION=v2.0.0 (see the traps below)
cp build/release/extension/hf/hf.duckdb_extension dist/    # run_all.sh does this; a smoke_*.sh on its own does not
python3 scripts/try.py < scripts/smoke_xet.sql  # prints the artifact it loads; there is no `duckdb` CLI in the venv
bash scripts/run_all.sh --build            # every live suite; or one smoke_*.sh
bash scripts/smoke_xet.sh                  # the offline reference checks, no token needed
python3 scripts/xet_oracle.py --check      # drift check against xet-core (needs cargo on PATH)
scripts/ci_selftest.sh build/release/extension/hf/hf.duckdb_extension   # what CI runs on an artifact
scripts/pick_test_client.sh dist/repo-signed/v2.0.0/linux_arm64/hf.duckdb_extension dist/repo-signed
                                                # fetches the released duckdb that artifact's footer names,
                                                # prints DUCKDB=<path>; the same script CI's selftest uses
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
  the experiment to run. This one was billing; it is fixed, and the runner-side symptom disappears the moment the
  account can pay a runner again.
* **The matrix runs in waves, and a wave that has not been created yet is not a failure.** The four linux jobs are
  scheduled immediately; `osx_*` and `windows_*` appear only when the linux wave is done (`needs:`, and the second
  wave waits on the first even with `reduced_ci_mode: disabled`). So a `jobs` list of five, forty minutes into an
  eleven-job run, means a *hung job in wave one* — check `started_at` against history (linux ≈ 7–9 min, the second
  wave ≈ 7 min) rather than concluding the scheduler is broken again.
* **`gh run cancel` has no `--yes`.** It prints a usage line and changes nothing; the non-interactive way is
  `gh api -X POST repos/<owner>/<repo>/actions/runs/<id>/cancel`. Confirm with the run's `status`, not with the
  exit code of the command you thought cancelled it.

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
