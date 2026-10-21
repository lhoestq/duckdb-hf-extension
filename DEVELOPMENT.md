# Development — running the suites, building, shipping a repository

The protocol behind the Xet path is [docs/XET.md](docs/XET.md); the current release is
[CHANGELOG.md](CHANGELOG.md).

## Testing

The suites run against the **live Hub** — they write to scratch repositories and clean up, so they need a
write-capable token (`~/.cache/huggingface/token` or `HF_TOKEN`).

```bash
scripts/run_all.sh --build     # build once, then every suite
bash scripts/smoke_read.sh     # or one suite on its own; each .sh makes and removes its own fixtures
```

```
xet         12 passed,  3 expected failures   # offline: 367 reference checks, no token needed
xet_upload  30 passed,  1 expected failures   # against the production CAS
read        55 passed,  0 expected failures
write       57 passed,  6 expected failures
models      24 passed,  0 expected failures
buckets     22 passed,  2 expected failures
```

`xet_upload` is the suite that cannot pass on a wrong byte: the CAS re-hashes every xorb and re-derives the
verification hash of every shard segment it is sent — which is how a shard that indexed chunks by file
position instead of xorb position was caught.

`scripts/try.py` executes one statement per line-group and grades it: a column named `assert*` must be
`TRUE`, and a `-- expect-fail: <substring>` comment above a statement means it must raise with that
substring — so a refusal that stops refusing starts failing. Scratch state: `lhoestq/playground` is kept and
reused; `hf-smoke-staging` and `hf-ext-write-probe` are created per run and removed by
`scripts/delete_repo.sh`.

The offline checks exist three times because the CI matrix can run neither of the first two: a sqllogictest
opening with `require hf` is only satisfiable by extensions on DuckDB's compiled-in autoload list, so the
matrix's test step *skips* `test/sql/hf_selftest.test` — and a skipped file counts as a pass. So
`scripts/ci_selftest.sh` runs them against **the built artifact**, loaded into the `duckdb` shell that
artifact ships in its own `test-support.tar.gz`, comparing numbers instead of trusting a verdict.

## Building

```bash
# duckdb + extension-ci-tools are either checked out beside this Makefile (./duckdb, ./extension-ci-tools,
# as in CI) or vendored one level up in ../vendor
. .venv/bin/activate                      # the version to build against comes from the running duckdb
make release OVERRIDE_GIT_DESCRIBE="$(python -c 'import duckdb;print(duckdb.sql("PRAGMA version").fetchall()[0][0])')"
# -> build/release/extension/hf/hf.duckdb_extension (copied to dist/)
```

A locally built extension is unsigned, so loading it needs the flag:

```python
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD '/path/to/hf.duckdb_extension'")
```

`scripts/check.sh` is a compile-only syntax check of every source file.

### Generated files, and the toolchain that checks them

`src/hf_xet_vectors.cpp` and `src/hf_xet_shard_vectors.cpp` are output of `scripts/xet_oracle.py`, which
replays `../xet-core` and fails (`--check`) when the port's stored vectors drift from the reference.
Two rules follow from how that file has broken twice:

* Nothing in a generated file is fixed by editing it. A hand-typed workaround is a pending failure: the
  next regeneration deletes it, and `--check` then reports the file as up to date, because it is.
* gcc and clang accept constructs ISO C++ does not, and `windows_amd64` is built with `cl.exe`, which
  does not. A zero-length array compiles here, on linux and under mingw, and stops MSVC with `error
  C2466`. So portability checks that only run locally are not portability checks; the generator now
  refuses to write an empty array (and once did catch it: it names the same two lines cl.exe did).


## Distributing

A repository is a static file tree, `<prefix>/<duckdb_version>/<platform>/hf.duckdb_extension[.gz]` — any
web server, `file://` directory, or Hub repo. `scripts/make_extension_repo.sh` assembles one from a build;
`scripts/make_extension_repo_from_ci.sh` assembles one from a CI run's artifacts (how the published tree is
made, since one machine cannot build eight architectures); `scripts/publish_extension_repo.py` publishes a
tree to a Hub **bucket** (`--bucket` / `HF_BUCKET`, what 0.0.2 and later use) or to a Hub **dataset**
(`--repo` / `HF_REPO`, what pre1–pre8 went into) and prints the client recipes. The layout is identical — a
bucket just has no revision in its url (`/resolve/`, not `/resolve/main/`) and no commit behind each upload —
and neither target ever deletes: `hf buckets sync` runs without `--delete`, because a tree for one duckdb
version must not remove the binaries clients on another identifier are still fetching.

Three rules the scripts enforce, each because breaking it once failed silently:

* **A directory must equal the footer of what it serves.** `scripts/ext_footer.py` reads the footer as the
  structure DuckDB's `append_metadata.cmake` defines, `make_extension_repo.sh` refuses a
  `DUCKDB_VERSION`/`DUCKDB_PLATFORM` that disagrees with the binary, and the publish script checks every
  binary against its directory before touching the remote. (An earlier version read the version with
  `strings | grep '^v[0-9]'`; in a mingw artifact the first match is `v0.1H`, which filed a 73 MB binary
  where no client would ever look — green run, happy script.)
* **The version identifier must match the running DuckDB** or `LOAD` refuses the binary. For a released
  DuckDB, build against its tag — which since 2.0.0 is the whole story: `duckdb_version: v2.0.0`, the default
  clone of `duckdb/duckdb`, nothing else. For a dev pre-release it was not: the identifier is generated by
  DuckDB's release CI and tagged nowhere upstream, while extension-ci-tools exports `DUCKDB_VERSION` into the
  build (the Makefile prefers it over `OVERRIDE_GIT_DESCRIBE`, and `duckdb_tag` does not help — nothing
  downstream reads the tag it creates). So CI cloned <https://github.com/lhoestq/duckdb>, a one-commit mirror
  whose tree equals upstream's for the target sha and which *carries the tag*. 2.0.0 retired both inputs; the
  mirror stays because artifacts and CHANGELOG entries name what it built.
* **Whether a CI job compiles DuckDB or downloads it is worth checking in the log**, and it depends on the
  version being released: extension-ci-tools asks
  `https://duckdb-staging.duckdb.org/<short sha>/latest_alpha_version.txt` for a prebuilt static-libs archive and
  builds from source when it 404s (`Using prebuilt DuckDB archive:` versus `building DuckDB from source instead`).
  Every alpha job of this repository paid ~7 minutes of compiling, per architecture, per run. On a private
  repository billing macOS minutes at 10×, that is the line in a build log worth reading.
* **Signing is what makes `INSTALL` safe.** `make_extension_repo.sh` signs when
  `DUCKDB_EXTENSION_SIGNING_PK` is set (RSA-2048 over a two-level sha256, in the binary's last 256 bytes) and
  publishes the key at `<prefix>/.well-known/duckdb-extension-repo.json`. `scripts/test_install.py` verifies a
  tree or a url end to end — install, load, a real query, throwaway home — and on the signed run flips a byte
  in *every* platform directory and both file forms (DuckDB sniffs gzip), demanding a fresh process refuse it
  from a repository created with the key pinned *and* keyless.

```bash
RUN=<ci-run-id> DUCKDB_EXTENSION_SIGNING_PK="$(cat ~/.config/duckdb-hf/signing.pem)" \
  scripts/make_extension_repo_from_ci.sh dist/repo-signed
RUN=<ci-run-id> scripts/make_extension_repo_from_ci.sh dist/repo         # same tree, unsigned
HF_BUCKET=duckdb-extensions/builds scripts/publish_extension_repo.py dist/repo-signed --unsigned-tree dist/repo
HF_BUCKET=duckdb-extensions/builds scripts/verify_published.py   dist/repo-signed --unsigned-tree dist/repo
```

The last line is the one that cannot be skipped: it re-fetches every file the publish uploaded, from the url a
client will use, and fails unless the sha256 matches the tree it was built from *and* each served binary's footer
names the directory it is served from. Its arguments are the publish script's arguments on purpose — a release
cannot be verified against a different tree than it was published from by accident.

Clients then say `INSTALL hf FROM '<prefix>'; LOAD hf;`. Use `FROM '<prefix>'` rather than
`SET custom_extension_repository`, which redirects *every* install — including the `httpfs` this extension
needs as its https transport — to your own tree.

**Cutting a release:** bump `HF_EXTENSION_VERSION` in `src/include/hf_common.hpp` and the assertion in
`scripts/smoke_write.sql` together, then

```bash
git commit -am ...; git tag -a v0.0.2-pre8 -m "..."; git push origin main
git push origin refs/tags/v0.0.2-pre8       # `git push --follow-tags` silently skips a lightweight tag
gh workflow run "Build extension" --ref refs/tags/v0.0.2-pre8
```

A push to `main` is not what builds a release tree (`build.yml` has no `tags:` trigger), and the dispatch has to
build the commit the tag names: the footer's version is `git describe` at the built commit, so a dispatch on a
`main` that has moved past the tag stamps `v0.0.2-pre8-1-g45c6090` and every client refuses the binary. That is the
lagging-tag trap wearing a different hat — the tag does not have to be behind, it only has to *differ* from what was
built. Dispatching the tag ref removes the whole class: the run's `headBranch` is the tag and its `headSha` is the
tagged commit, which is what `gh run list` is there to confirm before anyone waits 25 minutes.

If a job hangs instead of failing — `in_progress` far past the ~7 minutes that build takes, with the matrix's second
wave never created because it waits on the first — cancel through the API (`gh api -X POST
repos/lhoestq/duckdb-hf-extension/actions/runs/<run>/cancel`; `gh run cancel` has no `--yes`) and re-dispatch the
tag. If *every* job dies in seconds with `steps: 0` and no log, that is not this repo: `gh workflow run 389744541`
(`.github/workflows/diagnostic.yml`, two steps) answers whether Actions can schedule a runner at all, in seconds
instead of a metered matrix run's ~25 minutes.


## Layout

```
src/hf_url            hf:// parsing, resolve/tree urls        src/hf_secrets   secret type, settings, env, token file
src/hf_crypto         self-contained sha256/base64/JSON       src/hf_http      HTTPUtil: auth, retries, errors
src/hf_fs             FileSystem: dispatch, list, glob, rm    src/hf_read      the per-file window cache
src/hf_commit         preupload -> lfs batch -> commit        src/hf_write     temp buffering, upload on Close
src/hf_xet_hash       keyed BLAKE3 + mean tree (M1)           src/hf_xet_chunk gear-hash chunking (M2)
src/hf_xet_xorb       xorb + footer + LZ4 (M3, M8)            src/hf_xet_shard metadata shard (M5)
src/hf_cas            token exchange, uploads, dedup (M4,M7)  src/hf_xet_dedup  CAS queries, keyed shards, runs (M9)
src/hf_xet_vectors    generated reference vectors             src/hf_functions the hf_* table functions
test/sql/hf_selftest.test   the offline checks as a sqllogictest (see Testing)
scripts/              try.py run_all.sh check.sh ci_selftest.sh bench.py delete_repo.sh
                      smoke_{read,write,models,buckets,xet,xet_upload}.{sh,sql}
                      xet_oracle.py (--check = xet-core drift) xet_bench.sh  ext_footer.py
                      cas_dedup_probe.py (is this chunk indexed?) dedup_measure.py (what dedup saves)
                      make_extension_repo{,_from_ci}.sh publish_extension_repo.py
                      test_install.py (installs a tree or a url) verify_published.py (served bytes == CI's)
tools/xet_probe       Rust probe over xet-core — the oracle (cargo, --release)
.github/workflows/build.yml   DuckDB's reusable distribution workflow, for the platform matrix
.github/workflows/diagnostic.yml  two steps: can Actions schedule a runner at all
```

The long form of any claim in these files — the failed attempts, the measurements behind a decision, the bug
each caveat came from — is in `git log`, whose commit bodies carry ~9 000 words of it, and in the
CHANGELOG's per-release entries.
