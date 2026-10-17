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

- [x] CI matrix mirroring duckdb's (`linux_amd64/arm64`, `osx_amd64/arm64`, `windows_amd64`, musl) via
      `extension-ci-tools`; release workflow that uploads to a HF **dataset** acting as an extension repo,
      i.e. self-hosting `hf://` on HF - live at `lhoestq/duckdb-hf-extension`, signed and unsigned, both
      verified by `scripts/test_install.py`. Deviations, each with a reason in `.github/workflows/build.yml`:
      musl / windows_arm64 / wasm excluded (no local toolchain to verify them with), macOS built on tags and
      dispatches only (private repo: 10x minutes, and every job compiles DuckDB from source).
- [x] A job that verifies the *artifact* rather than a rebuild of it: `selftest` downloads `linux_amd64`
      and runs `scripts/ci_selftest.sh` with the `duckdb` binary the artifact ships in its
      `test-support.tar.gz`. Needed because the matrix's own test step cannot load an extension it has not
      heard of (`require hf` skips, silently, and a skip looks like a pass).
- [ ] `duckdb_extensions()`-visible metadata: needs a PR to DuckDB's signed extension index (their signing
      key is what makes a bare `INSTALL hf` work); our repository side is already in place
      (`.well-known/duckdb-extension-repo.json` + signed trees).
- [ ] Optional: a thin `pip` wrapper that registers the repo path (not required for the SQL experience).
- [ ] Upstream/README notes: what needs DuckDB-core or Hub changes (e.g. signed dev builds).

## Phase 6 — Xet client (0.0.2), research in `docs/XET.md`

- [x] M1 hashing: BLAKE3 keyed hashes (two fixed keys), the byte-swapped-per-word hex rendering,
      the mean-tree rule (cut when `hash[3] % 4 == 0`, groups of 2..9, parent hashed over
      `"{hex} : {size}\n"` lines), then file / xorb hashes. `src/hf_xet_hash.cpp`, vendored blake3.
- [x] M2 chunking: Gear hash + high-bit mask, 64 KiB target / 8 KiB min / 128 KiB max, boundaries and
      chunk hashes identical to the reference `examples/chunk`. `src/hf_xet_chunk.cpp`.
- [x] The verification itself: `scripts/xet_oracle.py` generates `src/hf_xet_vectors.cpp` from
      xet-core (input/file/xorb hashes, chunk size lists, the mean-tree table from xet-core's own
      correctness test, hex/base64 renderings), `hf_xet_selftest()` replays it inside the extension,
      `scripts/smoke_xet.sh` is the 152-check suite. `--check` notices xet-core drifting.
- [x] M3 xorb writer: chunk serialization + V1 footer (`XETBLOB` / `XBLBHSH` / `XBLBBND` + nonce,
      nonce excluded from the hash), uncompressed chunks; `src/hf_xet_xorb.cpp`, vectors generated the
      same way, and the whole serialized object fingerprinted - the port's bytes equal the reference
      writer's. LZ4/BG4 compression is deferred (docs/XET.md), and is an upload-size matter only.
- [x] M4 CAS client: write-token exchange derived from the url (`api/{type}/{id}/xet-write-token`,
      `/{revision}` for git-backed types, none for buckets), CAS-scoped bearer with the HF token left
      off, `POST {cas}/v1/xorbs/{prefix}/{hash}`, the reference's retry policy and 30s-before-expiry
      refresh, xorb splitting at the uncompressed 64 MiB / 8192 chunk limits. `hf_xet_upload()` exposes
      it; `scripts/smoke_xet_upload.sh` runs it against the production CAS, which recomputes every hash.
      No exists-check: the url is the content, and `was_inserted` is the dedup answer.
- [x] M5 shard + publish: a session shard is **not protobuf** - `metadata_shard/` hand-rolls
      little-endian, so `src/hf_xet_shard.cpp` hand-rolls it too, byte-identical to `xet-core` over 11
      generated shards (one file across 7 xorbs, the dedup flags, the bookends). The uploaded form is the
      shard **truncated at `file_lookup_offset`** with `footer_size` rewritten to 0, which is why re-sending
      identical content reports `exists`: the timestamp is in the dropped footer. `POST {cas}/v1/shards`
      takes it; the bucket then registers the file with a batch `addFile` carrying `xetHash` - the
      word-swapped hex of the file hash, verified by reproducing a file already on the Hub.
      **`COPY … TO 'hf://buckets/…'` works**, `hf_upload` too, and `hf_xet_upload(…, publish := true)` is
      the route by hand; the suites read it back and compare bytes, including a file whose chunks span
      several xorbs - the case that caught the xorb-local vs file-global chunk index bug (docs/XET.md).
- [x] M6 git-backed repos through Xet: the bytes go to the CAS, the commit stays the `lfsFile` pointer
      commit, and the shard's file-level `metadata_ext` (sha256 + `WITH_METADATA_EXT`) is the link between
      the sha256 the commit names and the xet hash the shard stores. Two things this taught: that sha256 is
      hex-parsed as a `DataHash` (four word-swapped little-endian groups), *not* as 32 bytes; and that no
      server-side gate exists to preflight - the write-token route answers even for a repo created
      `xetEnabled: false`, and preupload says `uploadMode: "lfs"` for Xet-enabled repos, so the client
      chooses (as `hf_xet` does). `hf_xet_write` defaults to on with the LFS upload as its fallback.
      What is left in this area is judgement, not unknowns: the next box.
- [ ] The upload polish that is now a performance matter rather than a correctness one: xorb uploads
      through the reference's adaptive concurrency (today sequential, ~76 MB in ~6 s), an upload-side dedup
      query (today nothing is reused across files, `inserted` shows it), and LZ4 frames (an uncompressed
      xorb is valid, just bigger on the wire).
- [x] Every stage diff-tested against `xet-core`'s own tooling (`chunk`, `hash`, `xorb`, `xorb-read`,
      `shard`) - the reference implementation is the oracle, since hash/format bugs are silent failures.
      The one lesson worth keeping: **offline byte-equality is necessary and not sufficient**. The shard
      passed every generated vector and the production CAS still rejected the first multi-xorb file,
      because a vector with one xorb cannot distinguish xorb-local from file-global chunk indices.

## ## Known unknowns (resolve in order)

1. Exact 2.0 extension ABI/`extension_version` tag rules for **locally built** extensions vs the wheel.
2. Whether 2.0's `FileSystem` still allows registering arbitrary schemes from a loadable extension
   (`dbp->file_system->RegisterSubtree`) or whether 2.0 requires the new catalog/`Secret`-based path binding.
3. Whether writes need `Secret`/`CREATE SECRET (TYPE HF ...)` to match 2.0's secret manager (mirroring
   `CREATE SECRET (TYPE S3 …)`) — almost certainly yes for parity; token-in-secret vs setting.
4. Multi-file commit transactionality of the Hub commit API (`expected_revisions`, 412 conflicts).
5. LFS thresholds per repo type (server-side configurable) ⇒ read from API, never hardcode.
6. `duckdb 2.0` may still move: pin commit now, add a `make update-duckdb` that re-resolves the wheel's commit.
