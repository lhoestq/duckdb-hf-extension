#!/usr/bin/env bash
# Delete a hub repository for good - used by the smoke suites to clean up what they created.
#
#   scripts/delete_repo.sh dataset lhoestq/my-scratch-repo
#   scripts/delete_repo.sh model   lhoestq/my-scratch-repo
#
# This is a shell script rather than an extension function because the hub wants the target as a json body on a
# DELETE request, and DuckDB's http transport sends DELETE without a body: from SQL the extension can delete the
# files inside a repository (hf_delete), not the repository itself. `hf repos delete` and
# `huggingface_hub.delete_repo()` do the same thing as this script.
set -euo pipefail
kind="${1:?usage: delete_repo.sh <dataset|model|space|kernel> <namespace/name>}"
repo="${2:?usage: delete_repo.sh <dataset|model|space|kernel> <namespace/name>}"
endpoint="${HF_ENDPOINT:-https://huggingface.co}"
token_file="${HF_TOKEN_FILE:-$HOME/.cache/huggingface/token}"
if [ -n "${HF_TOKEN:-}" ]; then token="$HF_TOKEN"; else token="$(cat "$token_file")"; fi
name="${repo#*/}"
org="${repo%/*}"
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$endpoint/api/repos/delete" \
  -H "Authorization: Bearer $token" -H "Content-Type: application/json" \
  -d "{\"name\":\"$name\",\"organization\":\"$org\",\"type\":\"$kind\"}")
# 404 here means the repository was already gone, which for cleanup is a success
if [ "$code" = "200" ] || [ "$code" = "204" ]; then
  echo "deleted $kind $repo"
elif [ "$code" = "404" ]; then
  echo "no $kind $repo to delete"
else
  echo "could not delete $kind $repo: HTTP $code" >&2
  exit 1
fi
