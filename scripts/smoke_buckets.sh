#!/usr/bin/env bash
# Bucket round-trip smoke test.
#
# DuckDB cannot *write* a bucket directly (bucket uploads need an Xet `xetHash`, which only the `hf_xet`
# client computes), but it can move files that already live on the Hub into one with `hf_bucket_copy`. The
# fixtures are therefore written by DuckDB itself into a staging dataset repository, copied into the bucket,
# read back through hf://, and deleted again.
#
#   scripts/smoke_buckets.sh            # keeps the staging repo
#   CLEANUP=1 scripts/smoke_buckets.sh  # deletes it afterwards
set -euo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate

# 1. fixtures, written by the extension into a staging dataset repository
python3 -u scripts/try.py < scripts/smoke_buckets_setup.sql

# the refusal test for hf_upload needs a local file to point at
build/release/duckdb -c "COPY (SELECT 1 AS a, 'x' AS b) TO '/tmp/hf_bucket_probe.csv' (FORMAT CSV);" >/dev/null

# 2. the suite itself (the two expected failures are the direct write paths: COPY and hf_upload into a bucket)
python3 -u scripts/try.py < scripts/smoke_buckets.sql

# 3. the staging repository goes with the fixtures
if [ "${CLEANUP:-}" = "1" ]; then
  token="$(cat "$HOME/.cache/huggingface/token")"
  curl -sS -o /dev/null -X DELETE "https://huggingface.co/api/datasets/lhoestq/hf-smoke-staging" \
    -H "Authorization: Bearer $token" -w 'staging repo deleted: HTTP %{http_code}\n'
fi
