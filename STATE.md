# Working state

Resume notes for whoever picks this up next (usually me, after a context compaction). The *user-facing*
story is `README.md`, the per-release story is `CHANGELOG.md`, the protocol story is `docs/XET.md`, and the
long form of every decision — the failed attempts and the measurements behind it — is in `git log` bodies.
This file only holds what is true right now and what to do next.

## Position — 2026-10-19, ~15:10 UTC

**0.0.2-pre7 is published, live, for all eight DuckDB client platforms.** HEAD carries M9 (cross-repository dedup,
unreleased); tag `v0.0.2-pre7` is at `39706a5` — move the tag to HEAD before the next dispatch, because the footer's
version is `git describe` at HEAD and a lagging tag ships a commit sha.

* Live at `https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/` (signed) and
  `…/resolve/main/unsigned/`, from CI run `39153506221`, for `v2.0.0-alpha47600`: `linux_amd64`,
  `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`, `windows_amd64`,
  `windows_amd64_mingw`. Sixteen served binaries byte-compared against CI's; `INSTALL`/`LOAD` verified over
  https here. **Everything not `linux_arm64` is verified by byte comparison only** — this machine cannot
  execute it, and a footer plus a sha256 is not a load test.
* Suites **212 passed / 13 expected failures / 0 failed** (xet 12, xet_upload 42, read 55, write 57, models 24,
  buckets 22); `hf_xet_selftest()` **1090 checks / 0 failed** — 471 dedup-eligibility rows, 64 HMAC rows, the shard
  rows, 24 rows over the keyed stored-shard fixture, and one non-vacuous `sampled-eligible chunks` row
  (`splitmix64 n=4000000 seed=11`, 1 of 1) which is what keeps the persisted chunk flag file-start-only. The expected
  count lives in `test/sql/hf_selftest.test`; `scripts/ci_selftest.sh` reads it from there, so a check that silently
  disappears is a failed run rather than a quieter green one.
* **M9 is shipped, and proven live in both shapes.** p7: content stored against a datasets repo, never published,
  then the same bytes published from a bucket repo — one row, no xorbs, `deduped_chunks > 0`, shard of pointers, and
  `read_blob` over `hf://` returns identical bytes. p8: the same file with two rows appended — partial dedup, the
  uploaded `uncompressed_bytes` strictly below the file size, mixed shard reads back byte-identical.
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

1. **Release 0.0.2-pre8** with M9 in it: annotate HEAD as `v0.0.2-pre8`, dispatch build.yml with
   `reduced_ci_mode: 'disabled'`, publish from the run, and re-do the byte-compare plus https `INSTALL`/`LOAD`
   verification pre7 got. Nothing in M9 changes a byte for an upload that runs with `hf_xet_upload_dedup = false`,
   which is a claim worth re-testing on a pre-release rather than asserting.
2. **Measure what dedup saves on the copy case**: a dataset copied with one extra column, xorb bytes and wall clock
   with `hf_xet_upload_dedup` on and off. The 1-in-1024 sample is thin by construction; the number to know is how
   thin, and whether segment count rather than xorb count is what a big partitioned rewrite pays for.
3. **Shard-driven read prefetch** — walk a shard's chunk table instead of one range request per window; this
   is the fix for the `read_blob`-of-a-big-file gap that is still behind httpfs.
4. Optional: decide whether CI should `strip` the mingw artifact (73 MB unstripped vs 31 MB for the MSVC
   build), and add macOS install notes if anyone ever reports from one.
5. Failure-path coverage only where a safe test seam exists — see the *Limitations* note in the README about
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
