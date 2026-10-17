# WORKING STATE — huggingface duckdb extension (`hf://`)

> Living scratchpad. Updated continuously. **Read `PLAN.md` first, then this file, then resume.**

## Current position (2026-10-17, ~11:55 UTC)

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
