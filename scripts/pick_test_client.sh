#!/usr/bin/env bash
# Choose a duckdb to load an extension artifact with, and say which one was chosen.
#
#   scripts/pick_test_client.sh path/to/hf.duckdb_extension [dir-to-search-for-test-support]
#
# Prints the chosen client's version and, inside GitHub Actions, writes DUCKDB (and LD_LIBRARY_PATH for
# the fallback client) to $GITHUB_ENV so the next step can use it. Locally it just prints, which is what
# makes the two CI callers - build.yml's own selftest job and selftest_artifact.yml, which re-runs the
# checks against some past run's artifact - share one implementation instead of two copies that drift.
#
# Why a released client rather than the one the build shipped. A build artifact carries a duckdb in
# test-support.tar.gz, and until DuckDB 2.0 that binary always matched, because the matrix compiled
# DuckDB from source. For a released version extension-ci-tools downloads the static-libs archive DuckDB
# staged for the commit instead - and the staged archive is labelled with the alpha it was staged under,
# `v2.0.0-alpha49307` for the commit tagged `v2.0.0`. A binary built from it refuses an extension whose
# footer says `v2.0.0`, which is the footer DuckDB's own version script wrote from DUCKDB_VERSION. So the
# thing that "ships a matching duckdb" is not the thing a client runs, and testing with it turns a
# labelling difference in DuckDB's pipeline into a red job on a green artifact (measured on the first
# 2.0.0 release run).
#
# Hence: fetch `duckdb-cli-<platform>.tar.gz` from the DuckDB release named by the artifact's own footer.
# That is the binary users run, the version check is against what a real client reports, and the
# identifier comes from the artifact rather than from a second copy of a version string in a yaml file.
# A pre-release identifier has no GitHub release - which is how DuckDB shipped before 2.0 - so the asset
# 404s, and only then does this fall back to test-support.tar.gz, which does match a DuckDB compiled from
# source for that identifier.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT="${1:?usage: pick_test_client.sh path/to/hf.duckdb_extension [dir-with-test-support.tar.gz]}"
SEARCH="${2:-$(dirname "$EXT")}"

case "$(uname -m)" in
  x86_64) ARCH=linux-amd64 ;;
  aarch64 | arm64) ARCH=linux-arm64 ;;
  *) echo "no duckdb-cli asset name for $(uname -m)" >&2; exit 1 ;;
esac

footer=$("$root/scripts/ext_footer.py" --version "$EXT")
echo "artifact asks to be loaded by DuckDB: $footer (platform $(uname -m) -> $ARCH)"
mkdir -p "$root/.ci-cache"

url="https://github.com/duckdb/duckdb/releases/download/${footer}/duckdb-cli-${ARCH}.tar.gz"
client=""
if curl -sfL "$url" -o "$root/.ci-cache/cli-${ARCH}.tar.gz"; then
  rm -rf "$root/.ci-cache/client-${ARCH}" && mkdir -p "$root/.ci-cache/client-${ARCH}"
  tar -xzf "$root/.ci-cache/cli-${ARCH}.tar.gz" -C "$root/.ci-cache/client-${ARCH}"
  client="$root/.ci-cache/client-${ARCH}/duckdb"
  echo "using the released client a user would run: $url"
else
  echo "no DuckDB release asset for $footer (a pre-release identifier has none): using the binary the build shipped"
  support=$(find "$SEARCH" -name test-support.tar.gz 2>/dev/null | head -1)
  if [[ -z "$support" ]]; then
    echo "neither a DuckDB release asset nor a test-support.tar.gz under $SEARCH - nothing to load the artifact with" >&2
    find "$SEARCH" -type f 2>/dev/null | head -20 >&2
    exit 1
  fi
  mkdir -p "$root/build" && tar -xzf "$support" -C "$root/build"
  # that duckdb links libduckdb.so rather than carrying it
  export LD_LIBRARY_PATH="$root/build/release/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  client="$root/build/release/duckdb"
fi

[[ -x "$client" ]] || { echo "no executable duckdb at $client" >&2; exit 1; }
echo "duckdb under test: $($client --version)"
if [[ -n "${GITHUB_ENV:-}" ]]; then
  echo "DUCKDB=$client" >> "$GITHUB_ENV"
  [[ -n "${LD_LIBRARY_PATH:-}" ]] && echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" >> "$GITHUB_ENV"
else
  # locally, print it the way Actions will hand it to the next step, so the caller can copy it
  echo "DUCKDB=$client"
fi
