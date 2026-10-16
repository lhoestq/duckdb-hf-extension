#!/usr/bin/env bash
# Model repository smoke test. The two fixtures decide which transfer the hub uses: a .bin of a few MB is
# LFS-tracked by the model gitattributes, a small .txt goes into git as a blob.
#
#   scripts/smoke_models.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. .venv/bin/activate

head -c 2000000 /dev/urandom > /tmp/hf_model_blob.bin
python3 -c "open('/tmp/hf_model_text.txt','w').write('hf extension model fixture\n' * 200)"
ls -l /tmp/hf_model_blob.bin /tmp/hf_model_text.txt | awk '{print "fixture:", $5, $9}'

python3 -u scripts/try.py < scripts/smoke_models.sql
