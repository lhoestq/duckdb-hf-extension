#!/usr/bin/env bash
# Bucket round-trip smoke test.
#
# DuckDB cannot *write* a bucket (bucket uploads need an Xet `xetHash`, which only the `hf_xet` client
# computes), so the fixtures are put there with the `hf` CLI and then read back through hf://.
#
#   scripts/smoke_buckets.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate

SCRATCH_BUCKET="${SCRATCH_BUCKET:-hf://buckets/lhoestq/playground}"
BUCKET_ID="${SCRATCH_BUCKET#hf://buckets/}"

# 1. fixtures
build/release/duckdb -c "COPY (SELECT 42 AS answer UNION ALL SELECT 43) TO '/tmp/hf_bucket_probe.parquet' (FORMAT PARQUET);
           COPY (SELECT 1 AS a, 'x' AS b) TO '/tmp/hf_bucket_probe.csv' (FORMAT CSV);
           COPY (SELECT 7 AS answer) TO '/tmp/hf_bucket_second.parquet' (FORMAT PARQUET);" >/dev/null

hf buckets cp /tmp/hf_bucket_probe.parquet "hf://buckets/$BUCKET_ID/duckdb/probe.parquet" --format quiet
hf buckets cp /tmp/hf_bucket_second.parquet "hf://buckets/$BUCKET_ID/duckdb/second.parquet" --format quiet
hf buckets cp /tmp/hf_bucket_probe.csv "hf://buckets/$BUCKET_ID/duckdb/probe.csv" --format quiet

# 2. read them back through the extension (the two expected failures are the write paths)
python3 -u scripts/try.py < scripts/smoke_buckets.sql
