#!/usr/bin/env bash
# Every smoke suite, in one command. Each suite writes its full transcript to logs/ and contributes one
# line here; the exit code is 0 only when every suite behaved as advertised (refusals included).
#
#   scripts/run_all.sh                    # uses dist/hf.duckdb_extension as built
#   scripts/run_all.sh --build            # build it first
#   SCRATCH=... scripts/smoke_write.sh    # the suites themselves are individually runnable
set -uo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate

# the loadable extension has to be built for the running duckdb, so both come from the same identifier
DUCKDB_VERSION="${DUCKDB_VERSION:-v2.0.0-alpha47600}"
mkdir -p logs
status=0

if [ "${1:-}" = "--build" ]; then
  echo "building for $DUCKDB_VERSION ..."
  if ! make release OVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION" > logs/build.log 2>&1; then
    echo "BUILD FAILED, see logs/build.log"
    tail -20 logs/build.log
    exit 1
  fi
  cp build/release/extension/hf/hf.duckdb_extension dist/hf.duckdb_extension
fi
ls -l dist/hf.duckdb_extension | awk '{print "extension:", $5, "bytes"}'

run() { # name, command...
  local name="$1"; shift
  echo "--- $name"
  if "$@" > "logs/smoke_$name.log" 2>&1; then
    tail -1 "logs/smoke_$name.log" | sed "s/^/    $name: /"
  else
    echo "    $name: FAILED - $(tail -1 "logs/smoke_$name.log" | cut -c1-100)"
    grep '^FAIL' "logs/smoke_$name.log" | head -5 | sed 's/^/      /'
    status=1
  fi
}

run read bash -c 'python3 -u scripts/try.py < scripts/smoke_read.sql'
run write bash scripts/smoke_write.sh
run models bash scripts/smoke_models.sh
run buckets bash scripts/smoke_buckets.sh

echo
[ $status -eq 0 ] && echo "all suites passed" || echo "SOME SUITES FAILED"
exit $status
