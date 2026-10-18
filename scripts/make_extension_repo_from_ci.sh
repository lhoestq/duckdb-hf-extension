#!/usr/bin/env bash
# Build a serveable extension repository out of the artifacts a CI run produced, rather than out of a
# local build - which is the difference between `INSTALL hf` working on this machine's architecture and
# working on everyone's.
#
#   RUN=39122775811 scripts/make_extension_repo_from_ci.sh [out_dir]          # default dist/repo-ci
#   RUN=... DUCKDB_EXTENSION_SIGNING_PK="$(cat ~/.config/duckdb-hf/signing.pem)" \
#       scripts/make_extension_repo_from_ci.sh dist/repo-ci-signed
#
# Needs `gh` authenticated against the repository the run belongs to, because artifacts of a private
# repository are not public downloads. Then it is the same tree make_extension_repo.sh produces, one
# <duckdb_version>/<platform>/ directory per platform the matrix built, and publish_extension_repo.py
# uploads it like any other.
#
# Two things here are deliberate:
#   * the platform directory is taken from the *artifact name* and passed to make_extension_repo.sh as
#     DUCKDB_PLATFORM, never inferred from a footer: a local build directory also carries a
#     duckdb_platform_out file, and letting that decide would file every platform's binary under the
#     architecture of the machine that happened to run this script. The footer is not ignored:
#     make_extension_repo.sh cross-checks it against that name and refuses the binary when they disagree,
#     which is the guard against the other direction of the same mistake (and the one that caught a
#     windows binary filed under `v0.1H`, see scripts/ext_footer.py).
#   * artifacts are cached (CACHE_DIR below, default dist/ci-artifacts/<run>/), because the unsigned and
#     the signed tree come from the same bytes and re-downloading a few hundred MB to sign them is a tax
#     on nothing.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

RUN="${RUN:?set RUN to a GitHub Actions run id - see one with: gh run list --limit 1}"
OUT_DIR="${1:-dist/repo-ci}"
CACHE_DIR="${CACHE_DIR:-dist/ci-artifacts/$RUN}"
REPO="${REPO:-lhoestq/duckdb-hf-extension}"

mkdir -p "$CACHE_DIR"
if [[ -z "$(ls -A "$CACHE_DIR" 2>/dev/null)" ]]; then
  echo "downloading the artifacts of run $RUN into $CACHE_DIR"
  # the matrix uploads one artifact per platform, named <ext>-<duckdb_version>-extension-<arch>; the
  # selftest job's artifact (if any) shares that shape only for the platform it tested, so a repeat
  # binary for one platform is not an error below, the first one seen wins
  gh run download "$RUN" -R "$REPO" -D "$CACHE_DIR"
else
  echo "reusing the cached artifacts in $CACHE_DIR"
fi

built=0
# gh run download unpacks each artifact into a directory named after it
for dir in "$CACHE_DIR"/hf-*-extension-*; do
  [[ -d "$dir" ]] || continue
  name="$(basename "$dir")"
  platform="${name##*-extension-}"
  ext="$(find "$dir" -name hf.duckdb_extension -print -quit)"
  if [[ -z "$ext" ]]; then
    echo "  skipping $name (no hf.duckdb_extension inside)" >&2
    continue
  fi
  echo "--- $platform"
  # BUILD_DIR points at a directory with no duckdb_platform_out in it, so the explicit
  # DUCKDB_PLATFORM above is what decides the destination directory
  EXT="$ext" BUILD_DIR="$dir" DUCKDB_PLATFORM="$platform" scripts/make_extension_repo.sh "$OUT_DIR"
  built=$((built + 1))
done

if [[ "$built" == "0" ]]; then
  echo "run $RUN produced no hf extension artifacts - check it with: gh run view $RUN" >&2
  exit 1
fi

echo
echo "$built platform(s) assembled into $OUT_DIR"
cat <<EOF
publish it (see scripts/publish_extension_repo.py), then verify one platform for real:
  python3 scripts/test_install.py "$(cd "$OUT_DIR" && pwd)"
EOF
