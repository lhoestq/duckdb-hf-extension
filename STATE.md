# WORKING STATE — huggingface duckdb extension (`hf://`)

> Living scratchpad. Updated continuously. **Read `PLAN.md` first, then this file, then resume.**

## Current position (2026-10-16, ~15:15 UTC)

**Milestone: the extension is feature-complete for v1 and green on both live smoke suites, and reads are
now within ~1.3x of `httpfs` on large files (was ~15x).**

* Built `dist/hf.duckdb_extension` for `v2.0.0-alpha47600` (`OVERRIDE_GIT_DESCRIBE=v2.0.0-alpha47600`),
  loads in the Python pre-release with `allow_unsigned_extensions`.
* `scripts/smoke_read.sql` (22 checks) and `scripts/smoke_write.sql` (parquet/csv/json/overwrite/empty/
  partitioned/unicode/model/zstd + the three expected error paths) pass.
* Read perf work of this session, in order: thread-safety fix for a `SIGSEGV` (DuckDB 2.0 shares one file
  handle between tasks), transport-session reuse, per-file shared byte-range cache with coalesced
  in-flight windows and doubling aligned read-ahead. See "Reading performance" in `README.md` for numbers.
* Remaining known gaps (deliberate, documented in `README.md`): Xet uploads, multipart LFS (>5 GiB
  objects), `hf://buckets/*` writes.

Immediate next actions: (1) push to a private `lhoestq` GitHub repo (doing this now), (2) package/ship -
`make release` + `scripts/create_local_extension_repo.py` so `INSTALL hf` works instead of `LOAD '<path>'`,
(3) optional read tuning (async prefetch of the next window).

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
