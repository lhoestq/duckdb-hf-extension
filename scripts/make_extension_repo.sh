#!/usr/bin/env bash
# Assemble a serveable DuckDB extension repository from a build directory.
#
#   scripts/make_extension_repo.sh [out_dir]              # default: dist/repo
#   DUCKDB_VERSION=v2.0.0-alpha47600 scripts/make_extension_repo.sh
#   DUCKDB_EXTENSION_SIGNING_PK="$(cat key.pem)" scripts/make_extension_repo.sh   # signed + metadata
#
# The resulting directory is a plain static file tree, so any web server (or an HF dataset/bucket,
# see README "Distributing") works as the repository:
#
#   <prefix>/<duckdb_version>/<platform>/hf.duckdb_extension.gz
#   <prefix>/.well-known/duckdb-extension-repo.json        (only when signing)
#
set -euo pipefail

OUT_DIR="${1:-dist/repo}"
BUILD_DIR="${BUILD_DIR:-build/release}"
EXT="${EXT:-$BUILD_DIR/extension/hf/hf.duckdb_extension}"

if [[ ! -f "$EXT" ]]; then
  echo "no extension binary at $EXT (run 'make release' first)" >&2
  exit 1
fi

# The version identifier and the platform are baked into the extension's footer, and the footer is the
# only copy of the truth: a client asks for <duckdb_version>/<platform>/hf.duckdb_extension.gz and then
# refuses a binary whose footer disagrees. So read it as a structure (scripts/ext_footer.py, which knows
# the layout) rather than the way this file used to - `strings | grep '^v[0-9]'` - because the version
# string is not the only thing in a binary that starts like a version. The first windows artifact to come
# through here matched a stray `v0.1H` from somewhere in mingw's metadata and was filed under a directory
# no client will ever ask for, which a green run and a 222 MB artifact did not show.
FOOTER="$(python3 scripts/ext_footer.py --all "$EXT")"
FOOTER_PLATFORM="${FOOTER%%	*}"
FOOTER_REST="${FOOTER#*	}"
FOOTER_VERSION="${FOOTER_REST%%	*}"
if [[ -z "$FOOTER_PLATFORM" || -z "$FOOTER_VERSION" ]]; then
  echo "could not read the footer out of $EXT" >&2
  exit 1
fi

# An override is a cross-check here, not a fallback. make_extension_repo_from_ci.sh passes the platform
# taken from the artifact's *name*, because the alternative - letting a local duckdb_platform_out decide -
# files every platform's binary under the architecture of whichever machine ran the script. If that name
# and the binary's own footer ever disagree, that is the misfiling mistake, so stop instead of choosing.
if [[ -n "${DUCKDB_PLATFORM:-}" && "$DUCKDB_PLATFORM" != "$FOOTER_PLATFORM" ]]; then
  echo "refusing: DUCKDB_PLATFORM=$DUCKDB_PLATFORM but $EXT says $FOOTER_PLATFORM" >&2
  exit 1
fi
if [[ -n "${DUCKDB_VERSION:-}" && "$DUCKDB_VERSION" != "$FOOTER_VERSION" ]]; then
  echo "refusing: DUCKDB_VERSION=$DUCKDB_VERSION but $EXT says $FOOTER_VERSION" >&2
  exit 1
fi
PLATFORM="${DUCKDB_PLATFORM:-$FOOTER_PLATFORM}"
VERSION="$FOOTER_VERSION"

DEST="$OUT_DIR/$VERSION/$PLATFORM"
mkdir -p "$DEST"
cp "$EXT" "$DEST/hf.duckdb_extension"

# Sign (optional). DuckDB reserves the last 256 bytes of the binary for the signature: drop them,
# sign the two-level sha256 of the rest (sha256 of the concatenation of per-1MiB-chunk digests), and
# append the 256-byte RSA signature - exactly what duckdb/scripts/extension-upload-single.sh does.
PUBKEY_FILE="$OUT_DIR/.well-known/duckdb-extension-repo.json"
if [[ -n "${DUCKDB_EXTENSION_SIGNING_PK:-}" ]]; then
  key_file="$(mktemp)"
  trap 'rm -f "$key_file"' EXIT
  printf '%s\n' "$DUCKDB_EXTENSION_SIGNING_PK" >"$key_file"

  work="$(mktemp -d)"
  cp "$DEST/hf.duckdb_extension" "$work/ext"
  truncate -s -256 "$work/ext"
  : >"$work/concats"
  split -b 1M "$work/ext" "$work/chunk_"
  for f in "$work"/chunk_*; do openssl dgst -binary -sha256 "$f" >>"$work/concats"; done
  openssl dgst -binary -sha256 "$work/concats" >"$work/hash"
  openssl pkeyutl -sign -in "$work/hash" -inkey "$key_file" -pkeyopt digest:sha256 -out "$work/sign"
  cat "$work/sign" >>"$work/ext"
  # keep the binary the same size it was before (the reserved area is reused, not extended)
  truncate -s "$(stat -c%s "$EXT")" "$work/ext"
  mv "$work/ext" "$DEST/hf.duckdb_extension"
  rm -rf "$work"

  mkdir -p "$OUT_DIR/.well-known"
  openssl pkey -in "$key_file" -pubout |
    python3 -c 'import json,sys; print(json.dumps({"version": 1, "signature_keys": [sys.stdin.read()]}))' \
      >"$PUBKEY_FILE"
  echo "signed with $(openssl pkey -in "$key_file" -pubout -outform DER | openssl dgst -sha256 -hex | awk '{print $2}')"
else
  rm -f "$PUBKEY_FILE"
  echo "note: built UNSIGNED - clients need allow_unsigned_extensions=true (set"
  echo "      DUCKDB_EXTENSION_SIGNING_PK to sign, see README 'Signing')"
fi

# DuckDB 2.0 sniffs gzip instead of trusting a .gz suffix, so ship both forms: file:// consumers
# read the plain file, http(s) consumers the compressed one (which is 3-4x smaller).
gzip -kf "$DEST/hf.duckdb_extension"

echo
echo "repository: $OUT_DIR"
echo "  $VERSION/$PLATFORM/hf.duckdb_extension     ($(stat -c%s "$DEST/hf.duckdb_extension") bytes)"
echo "  $VERSION/$PLATFORM/hf.duckdb_extension.gz  ($(stat -c%s "$DEST/hf.duckdb_extension.gz") bytes)"
echo "  footer says: platform=$FOOTER_PLATFORM duckdb_version=$FOOTER_VERSION ext_version=$(python3 scripts/ext_footer.py --extension-version "$DEST/hf.duckdb_extension") signed=$(python3 scripts/ext_footer.py --signed "$DEST/hf.duckdb_extension" && echo yes || echo no)"
echo
cat <<EOF
try it:
  python3 scripts/test_install.py "$(cd "$OUT_DIR" && pwd)"
EOF
