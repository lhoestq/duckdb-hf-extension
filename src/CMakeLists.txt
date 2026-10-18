set(HF_SOURCES
    hf_extension.cpp
    hf_settings.cpp
    hf_secrets.cpp
    hf_url.cpp
    hf_crypto.cpp
    hf_http.cpp
    hf_fs.cpp
    hf_read.cpp
    hf_commit.cpp
    hf_write.cpp
    hf_functions.cpp
    hf_xet_chunk.cpp
    hf_xet_hash.cpp
    hf_cas.cpp
    hf_xet_xorb.cpp
    hf_xet_shard.cpp
    hf_xet_shard_vectors.cpp
    hf_xet_vectors.cpp)

set(HF_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

add_library(hf_library OBJECT ${HF_SOURCES})
target_include_directories(hf_library PUBLIC
    ${HF_THIRD_PARTY}/blake3
    ${HF_THIRD_PARTY}/gearhash
    ${HF_THIRD_PARTY}/lz4)

# Vendored BLAKE3 (third_party/blake3, blake3-1.8.3, C only - no C++ runtime needed).
#
# Xet hashes are keyed BLAKE3, so this has to be bit-exact. It is: BLAKE3's output is defined to be
# independent of which implementation path computes it, and scripts/xet_oracle.py checks that rather
# than assuming it. The same is true of the per-ISA paths compiled below - which is exactly what the
# CI `selftest` job is for: it runs the 367 reference checks against the *built* linux_amd64 artifact,
# so an AVX2 path that disagreed with the reference would fail there instead of shipping.
#
# Upstream selects SIMD two ways, and the asymmetry is the reason this block is longer than it looks:
#
#   * x86-64 runtime dispatch (`-DBLAKE3_DISPATCH`): each ISA is its own translation unit compiled
#     with its own -m flag, and blake3_dispatch.c picks one after CPUID. One binary, no requirement
#     that the machine it lands on has AVX-512.
#   * aarch64 compile-time NEON: NEON is mandatory in ARMv8, so autodetection just turns it on and no
#     dispatch is involved - but the intrinsics file has to be compiled in for the symbol to exist.
#
# What is left portable: MSVC on x86-64 (upstream uses hand-written .asm there, which is not vendored)
# and everything that is neither x86-64 nor aarch64. Portable is ~300 MB/s, which was fine when the
# path it served was network-bound and is not fine now that a git-backed write goes through here.
enable_language(C)

set(HF_BLAKE3_SOURCES
    ${HF_THIRD_PARTY}/blake3/blake3.c
    ${HF_THIRD_PARTY}/blake3/blake3_dispatch.c
    ${HF_THIRD_PARTY}/blake3/blake3_portable.c)
set(HF_BLAKE3_DEFINITIONS BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)

# The target architecture is asked of the compiler rather than read out of CMAKE_SYSTEM_PROCESSOR. That
# variable describes the host on an Apple cross-build - the osx_amd64 job runs on an arm64 runner and passes
# -DCMAKE_OSX_ARCHITECTURES=x86_64 - so the first osx_amd64 build concluded it was compiling for aarch64 and
# handed blake3_neon.c to a compiler that answered "NEON intrinsics not available with the soft-float ABI".
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_QUIET TRUE)
check_c_source_compiles("#if !(defined(__x86_64__) || defined(_M_X64))\n#error not x86-64\n#endif\nint main(void) { return 0; }" HF_TARGET_X86_64)
check_c_source_compiles("#include <arm_neon.h>\nint main(void) { uint32x4_t v = vld1q_u8((const unsigned char *)\"0123456789abcdef\"); return (int)v[0]; }" HF_TARGET_NEON)

if (HF_TARGET_X86_64 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # the upstream c/CMakeLists.txt flags, per file
    set_source_files_properties(${HF_THIRD_PARTY}/blake3/blake3_sse2.c PROPERTIES COMPILE_FLAGS "-msse2")
    set_source_files_properties(${HF_THIRD_PARTY}/blake3/blake3_sse41.c PROPERTIES COMPILE_FLAGS "-msse4.1")
    set_source_files_properties(${HF_THIRD_PARTY}/blake3/blake3_avx2.c PROPERTIES COMPILE_FLAGS "-mavx2")
    set_source_files_properties(${HF_THIRD_PARTY}/blake3/blake3_avx512.c PROPERTIES
        COMPILE_FLAGS "-mavx512f -mavx512vl")
    list(APPEND HF_BLAKE3_SOURCES
        ${HF_THIRD_PARTY}/blake3/blake3_sse2.c
        ${HF_THIRD_PARTY}/blake3/blake3_sse41.c
        ${HF_THIRD_PARTY}/blake3/blake3_avx2.c
        ${HF_THIRD_PARTY}/blake3/blake3_avx512.c)
    set(HF_BLAKE3_DEFINITIONS BLAKE3_DISPATCH)
elseif (HF_TARGET_NEON)
    list(APPEND HF_BLAKE3_SOURCES ${HF_THIRD_PARTY}/blake3/blake3_neon.c)
    # NEON by autodetection, so only the x86 switches stay switched off
    set(HF_BLAKE3_DEFINITIONS BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif ()

add_library(hf_blake3_library OBJECT ${HF_BLAKE3_SOURCES})
target_include_directories(hf_blake3_library PUBLIC ${HF_THIRD_PARTY}/blake3)
target_compile_definitions(hf_blake3_library PRIVATE ${HF_BLAKE3_DEFINITIONS})

# Vendored LZ4 (third_party/lz4, v1.10.0, BSD-2-Clause) for the Xet xorb chunk-compression schemes.
#
# Unlike BLAKE3 this one does not have to agree byte-for-byte with anyone: an xorb's hash covers
# *uncompressed* chunk contents, and a reader decodes each chunk with the scheme byte and the stored
# compressed length, so any conforming LZ4 frame decodes. What it does have to be is decodable - which
# is checked two ways, by decompressing every xorb we just serialized before posting it, and by handing
# dumped xorbs to xet-core's own reader (`scripts/xet_bench.sh <file> lz4 <dir>` then `xet-probe
# xorb-read`, the compression part of docs/XET.md's "Xorb format" section).
#
# XXH_NAMESPACE=LZ4_ is not decoration: lz4frame computes frame checksums with xxhash, DuckDB links its
# own xxhash into the same process, and an extension's default-visibility symbols are exported. With
# the namespace on, the only XXH32 in the binary is DuckDB's, and lz4's is called LZ4_XXH32.
add_library(hf_lz4_library OBJECT
    ${HF_THIRD_PARTY}/lz4/lz4.c
    ${HF_THIRD_PARTY}/lz4/lz4hc.c
    ${HF_THIRD_PARTY}/lz4/lz4frame.c
    ${HF_THIRD_PARTY}/lz4/xxhash.c)
target_include_directories(hf_lz4_library PUBLIC ${HF_THIRD_PARTY}/lz4)
target_compile_definitions(hf_lz4_library PRIVATE XXH_NAMESPACE=LZ4_)
if (NOT MSVC)
    # Keep the LZ4/xxhash C symbols out of the process's global namespace. hf_library calls them from
    # the same shared object, so nothing outside needs to resolve them, and an extension that exports a
    # plain `LZ4_compress_default` is one more candidate in the global namespace for whatever else in
    # the process asks for that name (DuckDB's own copy of LZ4 is namespaced into C++, so it is not the
    # collision - some other extension's unnamespaced copy would be).
    #
    # -fvisibility=hidden alone is not enough: lz4.h and lz4frame.h stamp their public declarations with
    # __attribute__((visibility("default"))) unless LZ4*_VISIBILITY is predefined, so the macro has to be
    # emptied out too - and emptied out for the *caller* as well, or hf_library would be declaring with
    # wider visibility than the definition it links against.
    set(HF_LZ4_HIDDEN -fvisibility=hidden)
    target_compile_definitions(hf_lz4_library PRIVATE LZ4LIB_VISIBILITY= LZ4FLIB_VISIBILITY=)
    target_compile_options(hf_lz4_library PRIVATE ${HF_LZ4_HIDDEN})
    # hf_library's *declarations* are matched to that definition (an attribute saying "default" on a
    # symbol defined hidden is at best a warning), but hf_library itself keeps its own visibility: this
    # is the target holding the extension's entry points, and those have to stay exported.
    target_compile_definitions(hf_library PRIVATE LZ4LIB_VISIBILITY= LZ4FLIB_VISIBILITY=)
endif()

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:hf_library> $<TARGET_OBJECTS:hf_blake3_library>
    $<TARGET_OBJECTS:hf_lz4_library>
    PARENT_SCOPE)
