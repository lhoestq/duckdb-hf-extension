#!/usr/bin/env bash
# The hf_dataset() / hf_dataset_files() suite.
#
# Most of it talks to the production datasets-server and reads real converted datasets. But whether a
# dataset is half converted, whether its conversion is queued, and whether the route refuses with a 200 and
# a reason in the body, are facts about a moment on the Hub that cannot be provoked on demand. Those
# assertions are made against tools/hf_datasets_server_fixture.py, which answers a handful of made up
# dataset names from fixed bodies and hands everything else to the production service - so the parquet files
# the fixture lists are the real ones, and reading them still goes over hf:// with the token.
set -uo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate

# a port the kernel picked, so that two runs - or anything else on this machine - cannot collide with it
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')

python3 tools/hf_datasets_server_fixture.py --port "$PORT" > /tmp/hf_datasets_fixture.log 2>&1 &
FIXTURE=$!
trap 'kill "$FIXTURE" 2>/dev/null' EXIT

# binding takes a moment, and a suite started before it fails on a connection rather than on an assert
ready=""
for _ in $(seq 30); do
  if curl -sf "http://127.0.0.1:$PORT/health" > /dev/null 2>&1; then ready=1; break; fi
  sleep 0.2
done
if [ -z "$ready" ]; then
  echo "the datasets-server fixture never answered on port $PORT, see /tmp/hf_datasets_fixture.log"
  exit 1
fi

# Does this machine have a token to send? The last assertion asks a route that refuses an unauthenticated
# listing, which is only a meaningful question when there is a token to put in the header - a job running
# without secrets must not fail for the absence of a token it was never given.
TOKEN_SOURCE=$(python3 -c "
import duckdb
con = duckdb.connect(config={'allow_unsigned_extensions': True})
con.execute(\"LOAD 'dist/hf.duckdb_extension'\")
print(con.execute('SELECT token_source FROM hf_status()').fetchone()[0])
" 2>/dev/null)
if [ -z "$TOKEN_SOURCE" ] || [ "$TOKEN_SOURCE" = "none" ]; then
  AUTH="SELECT 'nothing to check: this machine resolves no token to send' AS auth_needs_a_token"
else
  AUTH="SELECT count(*) = 2 AS assert_the_route_got_a_bearer_token FROM hf_dataset_files('fixture/private')"
fi

sed -e "s|@PORT@|$PORT|g" -e "s|@AUTH@|$AUTH|g" scripts/smoke_datasets.sql | python3 -u scripts/try.py
