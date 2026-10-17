#!/usr/bin/env bash
# Run the offline reference checks against *the binary a client downloads*.
#
#   scripts/ci_selftest.sh                                   # locally built tree
#   scripts/ci_selftest.sh path/to/hf.duckdb_extension       # an extracted build artifact
#   DUCKDB=path/to/duckdb scripts/ci_selftest.sh ext.bin     # any duckdb whose version identifier matches
#
# Why this exists next to test/sql/hf_selftest.test: that file needs `require hf`, and DuckDB treats an
# extension as autoloadable only if it is named in its own compiled-in AUTOLOADABLE_EXTENSIONS list
# (src/include/duckdb/main/extension_entries.hpp) - a third-party name can never get on it, so the runner
# answers "require hf: 1" and skips the file. A skipped test file reports success, which is the worst
# possible failure mode for a test suite. So the same checks are run here through the plain shell: load
# the file, call the functions, compare the answers. Nothing is trusted to a verdict.
#
# Nothing here touches the network: hf_xet_selftest() replays reference vectors (hashes, chunk
# boundaries, whole xorb and metadata shard bytes) that are compiled into the extension.
set -euo pipefail

EXT="${1:-build/release/extension/hf/hf.duckdb_extension}"
DUCKDB="${DUCKDB:-build/release/duckdb}"

[[ -f "$EXT" ]] || { echo "no extension binary at $EXT (run 'make release', or extract one from an artifact)" >&2; exit 1; }
[[ -x "$DUCKDB" ]] || { echo "no executable duckdb at $DUCKDB (it ships inside test-support.tar.gz in the build artifact)" >&2; exit 1; }

# One source of truth for each number: the version the extension reports, and the number of reference
# checks the sqllogictest file asserts. Both fail loudly when they drift, instead of one of them quietly
# becoming the definition of green.
WANT_VERSION=$(sed -n 's/.*HF_EXTENSION_VERSION = "\([^"]*\)".*/\1/p' src/include/hf_common.hpp)
WANT_CHECKS=$(grep -A2 'SELECT count(\*) FROM hf_xet_selftest()' test/sql/hf_selftest.test | tail -1)

EXT_ABS=$(readlink -f "$EXT")
# every statement runs against the loaded file, so this script tests the artifact rather than whatever a
# build may have linked into the binary it is running
run() { "$DUCKDB" -unsigned -batch -noheader -list -c "LOAD '$EXT_ABS'; $1"; }

echo "== $EXT_ABS on $($DUCKDB --version 2>/dev/null || echo 'unknown duckdb')"
status=$(run "SELECT hf_version, duckdb_version, current_setting('hf_xet_write') FROM hf_status();")
echo "   hf_version | duckdb_version | hf_xet_write = $status"

got_version=$(run "SELECT hf_version FROM hf_status();")
[[ "$got_version" == "$WANT_VERSION" ]] || { echo "FAIL: hf_version reports '$got_version', the source says '$WANT_VERSION'" >&2; exit 1; }

# the write route, on by default: content goes to Xet, with the LFS upload as its fallback
got_xet_write=$(run "SELECT current_setting('hf_xet_write');")
[[ "$got_xet_write" == "true" ]] || { echo "FAIL: hf_xet_write defaults to '$got_xet_write', it is meant to default to true" >&2; exit 1; }

checks=$(run "SELECT count(*) FROM hf_xet_selftest();")
[[ "$checks" == "$WANT_CHECKS" ]] || { echo "FAIL: hf_xet_selftest() ran $checks checks, expected $WANT_CHECKS (regenerate with scripts/xet_oracle.py, and update test/sql/hf_selftest.test to match)" >&2; exit 1; }

all_ok=$(run "SELECT bool_and(assert_passed) FROM hf_xet_selftest();")
[[ "$all_ok" == "true" ]] || {
  echo "FAIL: hf_xet_selftest() has failing checks - the reference vectors do not match this build:" >&2
  run "SELECT name, detail FROM hf_xet_selftest() WHERE NOT assert_passed;" >&2
  exit 1
}
echo "   $checks reference checks, every one of them passing"

# a refusal that happens before any request, so it is testable with no token and no network
if err=$(run "SELECT * FROM hf_ls('hf://datasets/lhoestq');" 2>&1); then
  echo "FAIL: a url with no path was accepted: $err" >&2; exit 1
elif [[ "$err" != *"expected 'hf://<repo_type>/<namespace>/<name>/<path>'"* ]]; then
  echo "FAIL: a pathless url failed with something else than the parser's message: $err" >&2; exit 1
fi

# A write target naming a directory is refused before anything is sent - but reaching that check means
# going through COPY, and COPY needs a format, and in DuckDB 2.0 a format is an extension: without one
# loaded, TO 'some/path' stops at "no extension capable of reading the file" long before any filesystem
# is consulted. Take it when the tree has one; say so out loud when it does not.
PARQUET="${PARQUET:-$(dirname "$DUCKDB")/extension/parquet/parquet.duckdb_extension}"
if [[ -f "$PARQUET" ]]; then
  if err=$(run "LOAD '$(readlink -f "$PARQUET")'; COPY (SELECT 1 AS i) TO 'hf://datasets/lhoestq/demo/somedir/';" 2>&1); then
    echo "FAIL: a write target ending in '/' was accepted: $err" >&2; exit 1
  elif [[ "$err" != *"does not contain a file name"* ]]; then
    echo "FAIL: a directory write target failed with something else than the refusal: $err" >&2; exit 1
  fi
  echo "   a write target that names a directory is refused"
else
  echo "   (no parquet beside $DUCKDB: skipping the COPY refusal check - COPY needs a format, and a format"
  echo "    is an extension in DuckDB 2.0. scripts/smoke_write.sql covers it where one exists.)"
fi

# starts_with rather than LIKE 'hf\_%': DuckDB's LIKE takes the backslash literally, so the LIKE form
# of this assertion is 0 - and it sat in the skipped .test file saying ">= 12" for weeks
functions=$(run "SELECT count(*) FROM duckdb_functions() WHERE starts_with(function_name, 'hf_');")
[[ "$functions" -ge 12 ]] || { echo "FAIL: only $functions hf_* table functions registered, one has stopped registering" >&2; exit 1; }
echo "   $functions hf_* table functions registered"
echo "OK: $EXT_ABS loads, and every offline check says so by number"
