#!/usr/bin/env bash
# Read-path suite. Needs nothing but a token: it only reads public repos and the scratch repo.
set -euo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate
python3 -u scripts/try.py < scripts/smoke_read.sql
