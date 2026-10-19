# Working state

Resume notes for whoever picks this up next (usually me, after a context compaction). The *user-facing*
story is `README.md`, the per-release story is `CHANGELOG.md`, the protocol story is `docs/XET.md`, and the
long form of every decision — the failed attempts and the measurements behind it — is in `git log` bodies.
This file only holds what is true right now and what to do next.

## Position — 2026-10-19, ~12:55 UTC

**0.0.2-pre7 is published, live, for all eight DuckDB client platforms.** HEAD = `e605dde` (M9's rules, shard
reader and instruments, unreleased); tag `v0.0.2-pre7` is at `39706a5`, 10 commits behind HEAD — the last nine are
docs-only, but *move the tag forward before the next dispatch*, because the footer's version is `git describe` at
HEAD and a lagging tag ships a commit sha.

* Live at `https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main/` (signed) and
  `…/resolve/main/unsigned/`, from CI run `39153506221`, for `v2.0.0-alpha47600`: `linux_amd64`,
  `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`, `windows_amd64`,
  `windows_amd64_mingw`. Sixteen served binaries byte-compared against CI's; `INSTALL`/`LOAD` verified over
  https here. **Everything not `linux_arm64` is verified by byte comparison only** — this machine cannot
  execute it, and a footer plus a sha256 is not a load test.
* Suites **200 passed / 12 expected failures / 0 failed** (xet 12, xet_upload 30, read 55, write 57,
  models 24, buckets 22), re-run against `e605dde`; `hf_xet_selftest()` **972 checks / 0 failed** — 471
  dedup-eligibility rows, 131 shard rows, and one non-vacuous `sampled-eligible chunks` row
  (`splitmix64 n=4000000 seed=11`, 1 of 1) which is what keeps the persisted chunk flag file-start-only.
* **M9's producer half is settled by measurement, not inference.** The CAS indexes a file's first chunk *and*
  one hash in 1024, computing the second itself: 4 hits out of 4 eligible chunks (3 of them with the persisted
  flag clear), 13 controls answered 404. `scripts/cas_dedup_probe.py` reproduces it and prints its own verdict;
  the rule is replayed from `xet-probe` vectors, never re-derived in C++.
* The CI matrix fix (`e5bb74d`) was verified against the live matrix: push run `39157627780` built exactly
  `linux_amd64`, `linux_arm64`, `windows_amd64_mingw` + the reference-checks job, green.

## Next, in order

1. **M9 (C) then (D)** — query, match, pointer segments. `docs/XET.md` → *M9* has the measured rules; the shape of
   the work:
   * `HFClient` needs a GET that reports status. `Get(url, headers, throw_on_error = false)` cannot be reused for
     this: it returns `""` for a 404 *and* for a refused token, and those are the two answers that must not be
     confused — a miss means upload, a 401 means stop.
   * `GET {cas}/v1/chunks/default/{hex}` with the CAS bearer: 200 is a shard (`HFXetShardView::Parse` reads it),
     404 is a miss. Ask about chunk 0 and about `HFXetGlobalDedupEligible` chunks, at least 256 chunks apart like
     the reference; other chunks are not indexed, so asking about them is wasted network (measured: 13/13 were 404).
   * Match as the reference does: look the first hash up in the registered xorb chunk tables, extend forward while
     hashes agree, and build the segment with the *referenced xorb's* `xorb_flags` and the summed
     `unpacked_segment_bytes` (`FileDataSequenceEntry::from_xorb_entries`). A pointer segment is an ordinary
     segment naming an xorb the uploaded shard does not carry.
   * Pass A hashes chunks before uploading them, so pass B re-reads each unmatched range by (offset, length) and
     re-checks the hash — boundaries come from pass A, and re-chunking a subrange would move them.
   * Not copied from the reference: its defragment / `allow_dedup_on_next_range` heuristics. Taking every match is
     correct and costs only segments.
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
