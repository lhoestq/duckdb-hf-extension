#!/usr/bin/env bash
# Where does a Xet write spend its time? This measures the CPU phase; the answer, on a DGX Spark
# (aarch64) with a 100 MB file of /dev/urandom, was:
#
#   chunking                     31 ms   (3.4 GB/s)
#   chunk + keyed hash + xorb   152 ms   (690 MB/s, NEON BLAKE3)  /  177-193 ms portable BLAKE3
#   the same upload, end to end 12,000 - 70,000 ms, depending on the link
#
# so the write path is wire-bound by two orders of magnitude. BLAKE3 dispatch is a 1.15-1.27x on this
# phase (the spread is run-to-run, both binaries measured with XET_BLAKE3=portable below), which is not
# what a large upload waits on. Two consequences worth knowing before optimizing: 100 MB costs ~0.15 s
# of CPU, and the *first* thing to parallelize is the sequence of xorb POSTs, not the hashing loop.
#
#   scripts/xet_bench.sh [file-to-measure]
#   XET_BLAKE3=portable scripts/xet_bench.sh /tmp/100mb.bin   # rebuild BLAKE3 with all SIMD off
#
# Needs a `make release` tree (it links the objects that build, so what it times is what ships).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
objects="$root/build/release/extension/hf/src/CMakeFiles"
duckdb_include="$(cd "$root/.." && pwd)/vendor/duckdb/src/include"
input="${1:-/tmp/xet_bench_100mb.bin}"

if [[ ! -d "$objects/hf_library.dir" ]]; then
  echo "no build tree at build/release - run: . .venv/bin/activate && make release OVERRIDE_GIT_DESCRIBE=v2.0.0-alpha47600" >&2
  exit 1
fi
if [[ ! -e "$input" ]]; then
  echo "generating an incompressible $input (compressible input would understate the hash cost and" >&2
  echo "overstate what LZ4 would save; both numbers are worth collecting)" >&2
  head -c 104857600 /dev/urandom > "$input"
fi

blake3_objs=$(find "$objects/hf_blake3_library.dir" -name '*.o' | tr '\n' ' ')
if [[ "${XET_BLAKE3:-built}" == "portable" ]]; then
  # the same comparison the build comment claims: switch every SIMD path off and rebuild just BLAKE3
  scratch=$(mktemp -d)
  echo "rebuilding BLAKE3 portable into $scratch"
  gcc -O2 -c -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 \
    -I "$root/third_party/blake3" \
    "$root/third_party/blake3/blake3.c" "$root/third_party/blake3/blake3_portable.c" \
    "$root/third_party/blake3/blake3_dispatch.c"
  mv blake3*.o "$scratch/"
  blake3_objs=$(find "$scratch" -name '*.o' | tr '\n' ' ')
fi

g++ -std=c++17 -O2 -o /tmp/hf_xet_bench "$root/scripts/xet_bench.cpp" \
  "$objects/hf_library.dir"/*.o $blake3_objs \
  -I "$root/src/include" -I "$root/third_party/blake3" -I "$root/third_party/gearhash" \
  -I "$duckdb_include" \
  -L "$root/build/release/src" -lduckdb -Wl,-rpath,"$root/build/release/src"

/tmp/hf_xet_bench "$input"
