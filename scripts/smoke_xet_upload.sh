#!/usr/bin/env bash
# Xet upload smoke test - the one suite that talks to the production content addressable storage.
#
# The xet suite proves the bytes are right against compiled-in reference vectors; this one proves the
# real service agrees: it accepts the xorbs (recomputing every hash itself) and answers the dedup
# question the same way twice. Requires a token with write access, and leaves its chunks in the CAS -
# content is only removed by the hub's own retention rules, and nothing written here is ever read back
# until shard and commit exist (docs/XET.md, M5).
#
#   scripts/smoke_xet_upload.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate

python3 -u scripts/try.py < scripts/smoke_xet_upload.sql
