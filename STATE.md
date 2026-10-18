# Working state

Resume notes for whoever picks this up next (usually me, after a context compaction). The *user-facing*
story is `README.md`, the per-release story is `CHANGELOG.md`, the protocol story is `docs/XET.md`, and the
long form of every decision — the failed attempts and the measurements behind it — is in `git log` bodies.
This file only holds what is true right now and what to do next.

## Position — 2026-10-18, ~19:40 UTC

**0.0.2-pre7 is published, live, for all eight DuckDB client platforms.** HEAD = `b125b2f` (docs since the
release); tag `v0.0.2-pre7` is at `39706a5`, i.e. 8 commits behind HEAD — those are docs-only, but *move the
tag forward before the next dispatch*, because the footer's version is `git describe` at HEAD and a lagging
tag ships a commit sha.

* Live at `https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/` (signed) and
  `…/resolve/main/unsigned/`, from CI run `39153506221`, for `v2.0.0-alpha47600`: `linux_amd64`,
  `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`, `windows_amd64`,
  `windows_amd64_mingw`. Sixteen served binaries byte-compared against CI's; `INSTALL`/`LOAD` verified over
  https here. **Everything not `linux_arm64` is verified by byte comparison only** — this machine cannot
  execute it, and a footer plus a sha256 is not a load test.
* Suites **200 passed / 12 expected failures / 0 failed** (xet 12, xet_upload 30, read 55, write 57,
  models 24, buckets 22); `hf_xet_selftest()` **367 checks / 0 failed**.
* The CI matrix fix (`e5bb74d`) was verified against the live matrix: push run `39157627780` built exactly
  `linux_amd64`, `linux_arm64`, `windows_amd64_mingw` + the reference-checks job, green.

## Next, in order

1. **M9 (A) then (B)** — the gate experiment, fully designed and costed in `docs/XET.md` → *M9*. Add
   `HF_XET_CHUNK_FLAG_GLOBAL_DEDUP` where `word[3].to_le() % 1024 == 0` in `src/hf_xet_shard.cpp`, upload a
   many-chunk file, re-sweep the 196 committed fixture hashes. If the CAS ignores the flag on non-file-start
   chunks, cross-repo dedup is identical-files-only and step (C) shrinks accordingly. Then (C) query during
   chunking (the returned body is a shard, `hf_xet_shard.cpp` already parses it) and (D) emit pointer entries.
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
make release OVERRIDE_GIT_DESCRIBE="$(python -c 'import duckdb;print(duckdb.sql("PRAGMA version").fetchall()[0][0])')"
duckdb -unsigned -c "LOAD './dist/hf.duckdb_extension'; SELECT * FROM hf_status();"   # local build is unsigned
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
* **Tools**: `numpy` is not in the venv — use `cur.description` / `fetchall()`, never `fetchdf()`.
  `hf_xet_write` is not connect-time config (SET it after LOAD). `hf_xet_upload`'s `scope` must be a full
  `hf://` URL carrying the path, and `publish` is bucket-only (git-backed repos throw). Read a built
  extension's footer with `scripts/ext_footer.py`, never `strings | grep`.

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
