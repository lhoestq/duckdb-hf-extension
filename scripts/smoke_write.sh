#!/usr/bin/env bash
# Write-path smoke test: formats, overwrite, partitioning, unicode, LFS multipart, create-on-write,
# delete semantics, and the read cache noticing a commit. Run it against the scratch repositories with:
#
#   scripts/smoke_write.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate

# a 20 MB blob: big enough that the hub switches git LFS to basic multipart (~10 MB is where it starts)
head -c 20000000 /dev/urandom > /tmp/hf_write_fixture.bin
ls -l /tmp/hf_write_fixture.bin | awk '{print "fixture: " $5 " bytes"}'

python3 -u scripts/try.py < scripts/smoke_write.sql

# the create-on-write test made three throwaway repositories; the hub has no trash, so they go now
scripts/delete_repo.sh dataset lhoestq/hf-ext-write-probe
scripts/delete_repo.sh model lhoestq/hf-ext-write-probe
scripts/delete_repo.sh space lhoestq/hf-ext-write-probe
