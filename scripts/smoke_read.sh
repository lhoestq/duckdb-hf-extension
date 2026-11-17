#!/usr/bin/env bash
# Read-path suite. Needs nothing but a token: it only reads public repos and the scratch repo.
set -euo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate
# Poison freshly allocated and freshly freed memory, so a use-after-free on the read path is fatal here rather
# than silent: p21-p23 of the suite drive the cache trim into the state of issue #2, and without the poison an
# allocator answers a read of a freed map node with `ok` and the numbers come out identical either way. Malloc
# bookkeeping checks stay on as well. Set MALLOC_PERTURB_='' to run the suite without it.
export MALLOC_PERTURB_=${MALLOC_PERTURB_-42}
python3 -u scripts/try.py < scripts/smoke_read.sql
