# PLAN — `hf` DuckDB extension (native `hf://` support, DuckDB 2.0)

Goal: `INSTALL hf; LOAD hf;` then `hf://` behaves exactly like `s3://` for reading **and writing**:

```sql
SELECT * FROM 'hf://datasets/microsoft/falcon-refwildc/dataset/en.parquet';
SELECT * FROM read_parquet('hf://datasets/foo/**/*.parquet', hive_partitioning=true);
COPY (SELECT * FROM tbl) TO 'hf://datasets/me/out/data.parquet';
COPY (SELECT * FROM tbl) TO 'hf://datasets/me/out/' (FORMAT parquet, PARTITION_BY region);
CREATE TABLE x AS SELECT * FROM 'hf://datasets/foo/data.parquet';
```

> **Status 2026-10-16:** Phases 0-3 are done and verified against the live Hub (see `README.md`
> "Status" and `STATE.md`). Phase 4 is partly done: read performance now matches `httpfs` within ~1.3x on
> large files, the live smoke suites (`scripts/smoke_read.sql`, `scripts/smoke_write.sql`) are green, but
> `sqllogictest` coverage and CI are still open. Phase 5 (distribution) has not started - today you build
> with `make release` and `LOAD` the file with `allow_unsigned_extensions`.

## Ground rules

* Native C++ loadable extension (`HFFileSystem : FileSystem`) — no Python at runtime.
* Build against the **exact** core commit of the pre-release: `d4355d01c59362d57036ad377735dd5340276a5b`
  (already vendored at `/home/quentin/hf/vendor/duckdb`).
* Oracle for semantics: `huggingface_hub` (Python) and `opendal` `services/huggingface`.
* Every phase ends with: tests green, `PLAN.md` checkboxes flipped, `STATE.md` log line appended.

---

## Phase 0 — Ground truth (✅ done, see STATE.md)

- [x] DuckDB 2.0 pre-release installed, version/commit identified.
- [x] `httpfs` confirmed loadable ⇒ reference S3 experience is available for A/B testing.
- [x] DuckDB source at exact commit vendored.
- [ ] Read `AGENTS.md`, `.github/config/*.cmake` (locate httpfs repo), `extension/README.md` (rest).
- [ ] Understand 2.0 extension ABI tag (`abdd053708`) via `scripts/check_extension_abi.py`,
      `scripts/compute-extension-hash.sh`, `src/include/duckdb/main/extension_helper.hpp`.
- [ ] Clone `duckdb-httpfs` (+ `extension-ci-tools`, `extension_template`) and read `S3FileSystem`.
- [ ] Check for an HF token; else ask user.

## Phase 1 — Skeleton that loads (highest risk, do first)

- [x] Extension repo layout (`CMakeLists.txt`, `hf.config`, `Makefile`, `hf_extension.cpp`, `hfs extension` entrypoints,
      `duckdb_extension_config.cmake`, `extension_manifest`?, CI-ready naming).
- [x] `hf_extension.cpp`: `DUCKDB_EXTENSION_MAIN`/`DUCKDB_EXTENSION_ENTRYPOINT` (2.0 form), register
      `HFFileSystem` on the instance file system + settings + `ParserExtension`-style path handling if needed.
- [x] `make loadable_extension` against vendored duckdb ⇒ `hf.duckdb_extension`.
- [x] `LOAD '/abs/path/hf.duckdb_extension'` from the Python pre-release; prove it loads
      (fallbacks: `allow_unsigned_extensions=true`, custom repo dir, exact-ABI build).
- [x] **Gate:** if the wheel refuses to load a locally built extension, stop and design the distribution
      strategy (repo layout + CI matrix + `SET extension_repo`) before writing features.

## Phase 2 — URL layer + read path (the `s3://` experience)

- [x] `HFUrl` parser: `hf://{datasets|models|spaces|codes}/<namespace>/<name>/<path>@<rev>` (+
      defaults, `%2F`/`#` revision quoting rules from `huggingface_hub.parse_hf_url`).
- [x] Settings: `hf_token`, `hf_endpoint`, `hf_retry_st backoff`, `hf_timeout`, `hf_max_parallel`,
      `hf_offline`, `hf_cache_path`(optional), env fallbacks (`HF_TOKEN`, `HF_TOKEN_PATH`, `HF_ENDPOINT`, `HF_HUB_OFFLINE`).
- [x] `HFFileSystem : FileSystem`: `OpenFile`, `GetFileSize`, `Read`, `Seek`, `CanSeek`, `CanHandleFile`,
      `ListFiles`, `IsDirectory`, `Stat`, `FileExists`, `Glob`, `RemoveFile`, `CreateDirectory`, `MoveFile`,
      `CopyFile`(unsupported→error), `GetHomeDirectory`(n/a) + `TryDropFileCache`.
      Implementation: HTTP ranged GET on `/resolve/{revision}/{path}` with `Range`, 302→cdn-lfs,
      `x-error-code` handling, retry/backoff on 429/5xx, ETag/`If-None-Match` on listings.
- [x] Wire listing: `GET {endpoint}/api/{type}s/{id}/tree/{rev}?recursive=1&cursor=` (paged) → cache per dir.
- [x] Errors mapped to DuckDB exceptions with HF `x-error-code`/`x-request-id` in the message.
- [x] SQL tests: read csv/parquet/json, metadata, `glob`, `partitioned_write` round-trip on public repos.
- [x] `.test` files (sqllogictest) + a `scripts/smoke.py` that runs the same via the Python API.

## Phase 3 — Write path (Hub commit API)

- [x] `HFWriteFile : FileHandle` buffering to temp file (hub uploads are not streamable) + spill policy.
- [x] `Close()` protocol: `HEAD /api/paths/{type}s/{id}/{path}`? → `POST /api/blobs/{sha256}`
      (`x-etags`, `x-size`) → `POST /api/{type}s/{id}/commit/{rev}` ndjson
      (`{"key":"header","gitCommitMessage":…}` / `{"key":"file","path","algo":"sha256","oid","size","operation":"write"|"delete"}`).
- [x] LFS detection (size/kind thresholds per repo type from `/api/{type}s/{id}/lfs-files` or
      `.gitattributes`) ⇒ `path` vs `oid` payload key, `content` upload for small files.
- [x] `CREATE TABLE`, `INSERT`, `COPY TO`, `PARTITION_BY`, `OVERWRITE_PARTITIONED_WRITE`, `APPEND`,
      `RemoveFile`→commit with `operation: "delete"`, `MoveFile`→copy+delete commit.
- [x] Concurrency: batch multiple finished files of one `COPY` into **one** commit (like `huggingface_hub`
      `upload_folder`) with `expected_revisions`/`hub-...` conflict handling; retries on 412.
- [x] Large-file multipart/large-upload path if needed (`/api/lfs/...` PUT with `x-transfer-ids`, chunked).
- [x] Tests: write→read round-trip on a scratch repo, all formats, plus `huggingface_hub` cross-check.

## Phase 4 — Parity polish & robustness

- [ ] `hf_ls` / `hf_meta` / `hf_list_revisions` / `hf_create_repo` / `hf_delete_path` table functions,
      `FROM hf_datasets('...')` convenience TVF.
- [ ] Optional local cache (read-through, honoring `HF_HUB_CACHE` semantics) — nice-to-have, gated.
- [ ] `PRAGMA hf_status()` showing endpoint, auth source, resolved url template (no token leakage).
- [ ] Negative tests: 401/403/404/429/502, offline, bad revision, binary path chars, big (>5 GB) file read.
- [ ] Perf: parallel listing + prefetch ranges; measure vs `s3://` on the same public parquet dataset.
- [ ] Documentation: README usage, settings table, limitations, examples with real public repos.

## Phase 5 — Distribution (make it actually usable by others)

- [ ] CI matrix mirroring duckdb's (`linux_amd64/arm64`, `osx_amd64/arm64`, `windows_amd64`, musl) via
      `extension-ci-tools`; release workflow that uploads to a HF **dataset** acting as an extension repo
      (`SET custom_extension_repoBase`/`hf_repo`), i.e. self-hosting `hf://` on HF.
- [ ] `duckdb_extensions()`-visible metadata (`extension_config.cmake`/`extension_packages.json` or `repo.json`).
- [ ] Optional: a thin `pip` wrapper that registers the repo path (not required for the SQL experience).
- [ ] Upstream/README notes: what needs DuckDB-core or Hub changes (e.g. signed dev builds).

## Phase 6 — Xet client (0.0.2), research in `docs/XET.md`

- [ ] M1 hashing: BLAKE3 keyed hashes (two fixed keys), the byte-swapped-per-word hex rendering,
      the mean-tree rule (cut when `hash[3] % 4 == 0`, groups of 2..9, parent hashed over
      `"{hex} : {size}\n"` lines), then file / xorb / range hashes.
- [ ] M2 chunking: Gear hash + high-bit mask, 64 KiB target / 8 KiB min / 128 KiB max, boundaries and
      chunk hashes identical to the reference `examples/chunk`.
- [ ] M3 xorb writer: chunk serialization, compression (none, lz4 frame, bg4+lz4), footer with
      `XBLBHSH` / `XBLBBND` + nonce (nonce excluded from the hash); `examples/xorb-check` accepts it.
- [ ] M4 CAS client: refresh-route token exchange, `PUT {cas}/v1/xorbs/{key}`, exists checks, retried
      against the in-tree simulation server before the real Hub.
- [ ] M5 shard + commit: upload the session shard, commit with the file hash. First target is
      `COPY … TO 'hf://buckets/…'`, the one write 0.0.1 refuses; git-LFS stays the default elsewhere.
- [ ] Every stage diff-tested against `xet-core`'s own examples (`chunk`, `hash`, `xorb-check`) - the
      reference implementation is the oracle, since hash/format bugs are silent failures.

## ## Known unknowns (resolve in order)

1. Exact 2.0 extension ABI/`extension_version` tag rules for **locally built** extensions vs the wheel.
2. Whether 2.0's `FileSystem` still allows registering arbitrary schemes from a loadable extension
   (`dbp->file_system->RegisterSubtree`) or whether 2.0 requires the new catalog/`Secret`-based path binding.
3. Whether writes need `Secret`/`CREATE SECRET (TYPE HF ...)` to match 2.0's secret manager (mirroring
   `CREATE SECRET (TYPE S3 …)`) — almost certainly yes for parity; token-in-secret vs setting.
4. Multi-file commit transactionality of the Hub commit API (`expected_revisions`, 412 conflicts).
5. LFS thresholds per repo type (server-side configurable) ⇒ read from API, never hardcode.
6. `duckdb 2.0` may still move: pin commit now, add a `make update-duckdb` that re-resolves the wheel's commit.
