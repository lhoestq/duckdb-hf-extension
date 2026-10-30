# Development — running the suites, building, shipping a repository

The protocol behind the Xet path is [docs/XET.md](docs/XET.md); the current release is
[CHANGELOG.md](CHANGELOG.md).

## Why not just httpfs

`httpfs` ships a *read-only* `hf://` filesystem, and rejects `models`. This adds **writes** (`COPY TO`,
`INSERT`, `hf_upload`, `hf_delete`, `hf_bucket_copy`), the table functions below, all five repo types,
`@revision` and the short `hf://user/repo/path` form, and registers with `IsManuallySet()` so it wins over
httpfs whoever loads first. The `huggingface` **secret type** is shared, so one `CREATE SECRET` serves both.
It does not link httpfs' C++ classes (no stable ABI, extensions are `dlopen`'d `RTLD_LOCAL`); it drives
DuckDB's own `HTTPUtil`/`HTTPParams`/`HTTPHeaders` and lets DuckDB autoload httpfs purely as the transport.

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
`scripts/ci_selftest.sh` runs them against **the built artifact**, comparing numbers instead of trusting a
verdict. The client it loads the artifact into comes from `scripts/pick_test_client.sh`, which reads the
artifact's footer and fetches that DuckDB release's own `duckdb-cli-<platform>.tar.gz` — the binary users run.
It used to use the `duckdb` inside the artifact's `test-support.tar.gz`, and for a *released* DuckDB that
binary refuses the artifact: it is built from the static-libs archive DuckDB staged for the commit, which
carries the pre-release label the staging pipeline chose (`v2.0.0-alpha49307` for the commit tagged `v2.0.0`),
while the footer the loader compares says `v2.0.0`. The fallback to `test-support.tar.gz` is kept for
pre-release identifiers, which have no GitHub release asset to fetch — and for those, the shipped binary does
match a DuckDB compiled from source.

## Building

A loadable artifact needs three things to say the same version, and the build checks only one of them: the
duckdb sources that get *compiled*, the version string the footer *carries*, and the client that later *loads*
it. `LOAD` compares the footer against its own version and platform as exact strings
(`src/main/extension.cpp`), so a mismatch is either a refusal you can read or a mismatch nobody reads at all.

### Point the checkout at the version, then say it out loud

`OVERRIDE_GIT_DESCRIBE` sets a string, and a string is all it sets: duckdb's `CMakeLists.txt` turns it into
`DUCKDB_EXPLICIT_VERSION` → `DUCKDB_VERSION` → `DUCKDB_NORMALIZED_VERSION`, and `DUCKDB_NORMALIZED_VERSION` is
what the footer records. Nothing in that chain looks at the sources being compiled. A checkout that has drifted
past the tag therefore builds an artifact every released 2.0.0 client will *accept* — and then run as another
commit's code, since a `CPP`-ABI extension is compiled against those headers. `make` succeeds. So:

```bash
git -C ../vendor/duckdb fetch --tags
git -C ../vendor/duckdb checkout v2.0.0
git -C ../vendor/duckdb describe --tags --exact-match   # v2.0.0 — a -3-gabc1234 suffix IS the drift
git -C ../vendor/duckdb status --short                  # empty; edits to duckdb are invisible to the footer too
```

`v2.0.0` is `d54cbbe3df17e8e5ce68312f483b4155836112aa` and has no submodules, so a checkout is the whole thing.
The two inputs are found by layout: `DUCKDB_SRCDIR` is `./duckdb` when one exists beside the Makefile (what CI
gets) and `../vendor/duckdb` otherwise, `extension-ci-tools` likewise beside or `../vendor/extension-ci-tools`,
pinned to `4a5752ce7689b547344a91067bddb2b2b5395fac` — the same sha `build.yml` dispatches. Either path can be
overridden by hand: `DUCKDB_SRCDIR=/path/to/duckdb make release`. CI cannot drift, because extension-ci-tools
checks duckdb *out* at the `duckdb_version` input it is given; a local checkout is the one place where the pin
and the content can disagree.

Omit the pin and you get the loud failure instead: with no `DUCKDB_EXPLICIT_VERSION`, duckdb builds its version
from `scripts/ci/release_version.txt` plus a commit count — `v2.0.0-dev49307` — which matches `-dev[0-9]+$`, and
the `if` in `CMakeLists.txt` then stamps the footer with the **sha**. The refusal is unreadable in proportion:

```
The file was built specifically for DuckDB version 'd4355d01c593...' and can only be loaded with that
version of DuckDB. (this version of DuckDB is 'vd4355d01c593...')
```

which is exactly what the first release run of this repository shipped — a green matrix, and no client that
could load it. The Makefile sets `DUCKDB_VERSION=v2.0.0` when neither variable is passed, so a bare
`make release` is stamped correctly.

### On a machine that has never built it — nothing the build reads comes from this repository

```bash
# the two trees the build reads, in the layout local development expects beside this repository
git clone --branch v2.0.0 https://github.com/duckdb/duckdb ../vendor/duckdb
git clone https://github.com/duckdb/extension-ci-tools ../vendor/extension-ci-tools
git -C ../vendor/extension-ci-tools checkout 4a5752ce7689b547344a91067bddb2b2b5395fac

python3 -m venv .venv && . .venv/bin/activate
pip install cmake ninja duckdb==2.0.0     # build + suites; add huggingface_hub for the tools/ that call the Hub
```

`--branch v2.0.0` is what makes the first rule hold by construction: HEAD lands on the tag, so the sources and
the pin say the same thing. Not `--depth 1`: the version duckdb derives *without* a pin counts commits with
`git rev-list --count HEAD`, and a shallow clone counts wrong — a mistake that surfaces as a footer carrying a
sha. A missing `../vendor/duckdb` fails early and legibly — `CMake Error: The source directory "…/vendor/duckdb"
does not exist.` — the one to fear is the checkout that exists and sits elsewhere.

The suites load `dist/` into the venv's duckdb, so that package has to agree with the pin too. `python -c
'import duckdb; print(duckdb.sql("PRAGMA version").fetchall())'` answers `('v2.0.0', 'd54cbbe3df',
'Cyanoptera')` — the same commit the checkout sits on. Out of step, a suite fails as a crash instead of as a
version error, which is the least useful way to be told.

`make` is not in the venv (the system supplies it); `cmake` and `ninja` are, so a shell that skipped the
`activate` line stops at `cmake: command not found`, which reads like a broken build system and is a PATH.

### Build

```bash
. .venv/bin/activate
make release OVERRIDE_GIT_DESCRIBE="$(python -c 'import duckdb;print(duckdb.sql("PRAGMA version").fetchall()[0][0])')"
# -> build/release/extension/hf/hf.duckdb_extension
cp build/release/extension/hf/hf.duckdb_extension dist/hf.duckdb_extension   # make release does not copy
```

Taking the pin from the running duckdb keeps artifact and client agreeing by construction; `scripts/run_all.sh
--build` takes it from `DUCKDB_VERSION` (default `v2.0.0`) instead, copies to `dist/` itself, and refuses to
test a `dist/` older than the build tree — `make release` does not copy, and a stale `dist/` is how a suite
reports a verdict that belongs to a binary nobody built.

When you move the checkout to a different duckdb, wipe `build/release`: the `release` target is `mkdir -p` plus
a configure, never a clean.

### Use it

A local build is unsigned (`ext_footer.py` says `signed False`), so the client has to be told — `-unsigned` on
the CLI (`duckdb -h | grep unsigned`; `scripts/pick_test_client.sh` fetches that same release binary into
`.ci-cache/client-<arch>/duckdb`, `client-osx-arm64` on a Mac), the config key in Python:

```bash
duckdb -unsigned -c "LOAD 'dist/hf.duckdb_extension'; FROM hf_status()"
```

```python
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD 'dist/hf.duckdb_extension'")
```

And it is built for one platform — the machine that ran the compiler — which the same footer check enforces:

```
Invalid Input Error: Failed to load 'hf.duckdb_extension', The file was built for the platform 'osx_arm64',
but we can only load extensions built for platform 'linux_arm64'.
```

So an artifact built on a linux box will not load in a Mac client, and copying it over is not a workaround:
build it on the Mac, or install the published `osx_arm64` one. Before debugging anything else, read the footer,
which is a structure and has a reader that knows it:

```bash
.venv/bin/python3 scripts/ext_footer.py --all dist/hf.duckdb_extension
# linux_arm64	v2.0.0	90f03e2e94
#    ^ LOAD checks this	^ LOAD checks this	^ git describe of *this* repo, which LOAD ignores
```

That third field comes from `git describe --tags --always` run over this repository by duckdb's
`extension_build_tools.cmake`, and it survives as a version only when the build sits *exactly on* a tag:
`v0.0.3` yes, `v0.0.3-8-g6e661b4` no — the regex wants `vX.Y.Z(-rc)?` and falls back to the sha. CI dispatches
on a tag ref, which is why released artifacts say `v0.0.2` and local builds say a sha. No client reads it, and
`hf_status()` answers from `HF_EXTENSION_VERSION` instead, so a sha there is not a bug.

`scripts/check.sh` is a compile-only syntax check of every source file — with one caveat worth knowing when a
build seems to have swallowed a feature: it globs `src/*.cpp`, while the build compiles `HF_SOURCES` in
`src/CMakeLists.txt`, so a new file not added there passes `check.sh` and is simply absent from the extension.

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

A binary is tied to exactly one version identifier, so the alpha this project was built against up to and
including 0.0.2-pre8 (`v2.0.0-alpha47600`, what a pre-2.0 client reports) is a different artifact, not the
same one relabelled. [Installation](#installation) says which client to pin against which identifier.

Built for **DuckDB 2.0.0** (`v2.0.0`) on all eight DuckDB client platforms, and the
first release published to a **bucket**:
`https://hf.co/buckets/duckdb-extensions/builds/resolve`. It carries cross-repository dedup (`COPY TO` an
upload can point at content another repository already stored: 0 bytes and 1.4 s to re-upload 237 MB you
already have). The last build for the pre-2.0 identifier is **0.0.2-pre8** (`v2.0.0-alpha47600`), which keeps
serving every `v2.0.0-alpha…` client from the dataset tree. HEAD may carry fixes beyond either.
Suites: 212 passed / 13 expected failures / 0 failed, plus 1090 offline reference checks. Release notes in
[CHANGELOG.md](CHANGELOG.md); the Xet protocol port and its measurements in [docs/XET.md](docs/XET.md).

All eight platforms are published under each identifier, signed at the root and unsigned under `unsigned/`.
`v2.0.0` — from 0.0.2 — lives in a bucket, `https://hf.co/buckets/duckdb-extensions/builds/resolve`: object
storage, no revision in the url. Every `v2.0.0-alpha…` identifier before it lives in a dataset repo,
`https://huggingface.co/datasets/lhoestq/duckdb-hf-extension/resolve/main`, which is where pre1–pre8 went and
what those clients still fetch. The bucket tree was checked by fetching every served path and re-reading each
binary's footer against the directory serving it (33 files, `scripts/verify_published.py`), and installed over
https from the long and the short host and from the unsigned tree (`scripts/test_install.py`); the dataset tree
went through the same two checks when pre8 was published. Take the signed
one — `INSTALL` then checks bytes against a key instead of trusting the CDN.
`windows_amd64` vs `windows_amd64_mingw` is not about which Windows you run: DuckDB composes that name
itself, and `_mingw` appears only when DuckDB was *compiled* with mingw (`#ifdef __MINGW32__`).

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
  repository billing macOS minutes at 10×, that is the line in a build log worth reading. It is also the line that
  decides what `hf_status().duckdb_version` says: the archive is a *prebuilt DuckDB under a different label*, and
  `DuckDB::LibraryVersion()` is statically linked from it, so a downloaded `v2.0.0` archive labels the extension
  `v2.0.0-alpha49307` (`nm`: local symbol `t _ZN6duckdb6DuckDB14LibraryVersionEv`). Nothing about loading depends
  on it — the footer is written from `DUCKDB_VERSION`, not from that string — but the choice between the two lines
  in a build log is visible from SQL, so it is a distribution decision and not a build-speed one.
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
cannot be verified against a different tree than it was published from by accident. Then
`scripts/test_install.py '<prefix>'` (and `…/unsigned`) proves the install path over https from a clean home
directory, and dispatching `.github/workflows/selftest_artifact.yml` with a run id re-runs every offline check
against *that* run's artifact with a released client — 19 seconds, which is what makes it possible to re-check a
published release without rebuilding eight platforms to test one file.

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
                      pick_test_client.sh (the released duckdb that can load a given artifact)
tools/xet_probe       Rust probe over xet-core — the oracle (cargo, --release)
.github/workflows/build.yml   DuckDB's reusable distribution workflow, for the platform matrix
.github/workflows/selftest_artifact.yml  offline checks against a *past* run's artifact, released client
.github/workflows/diagnostic.yml  two steps: can Actions schedule a runner at all
```

The long form of any claim in these files — the failed attempts, the measurements behind a decision, the bug
each caveat came from — is in `git log`, whose commit bodies carry ~9 000 words of it, and in the
CHANGELOG's per-release entries.
