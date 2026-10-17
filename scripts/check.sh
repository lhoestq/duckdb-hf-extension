#!/usr/bin/env bash
# Fast syntax check of the extension sources against the vendored duckdb headers.
# (The real build goes through cmake/make, this is just a quick feedback loop.)
set -u
cd "$(dirname "$0")/.."
D=../vendor/duckdb
INCLUDES="-Isrc/include -I$D/src/include -I$D/third_party/pcg -I$D/third_party/re2/src \
-I$D/third_party/utf8proc/include -I$D/third_party/fmt/include -I$D/third_party/concurrentqueue \
-I$D/third_party/zstd/include -I$D/third_party/yyjson/include -I$D/third_party/fsst \
-Ithird_party/blake3 -Ithird_party/gearhash"
status=0
for src in src/*.cpp; do
  if ! output=$(g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter $INCLUDES "$src" 2>&1); then
    echo "--- $src"
    echo "$output" | head -${LINES:-40}
    status=1
  fi
done
[ $status -eq 0 ] && echo "syntax OK"
exit $status
