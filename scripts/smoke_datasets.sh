#!/usr/bin/env bash
# The hf_dataset() / hf_dataset_files() suite.
#
# Most of it talks to the production datasets-server and reads real converted datasets. But whether a
# dataset is half converted, whether its conversion is queued, and whether the route refuses with a 200 and
# a reason in the body, are facts about a moment on the Hub that cannot be provoked on demand. Those
# assertions are made against tools/hf_datasets_server_fixture.py, which answers three made up dataset
# names from fixed bodies and hands everything else to the production service - so the parquet files the
# fixture lists are the real ones, and reading them still goes over hf:// with the token.
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

sed "s|@PORT@|$PORT|g" scripts/smoke_datasets.sql | python3 -u scripts/try.py
