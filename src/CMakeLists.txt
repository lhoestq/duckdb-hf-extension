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
    ${HF_THIRD_PARTY}/gearhash)

# Vendored BLAKE3 (third_party/blake3, blake3-1.8.3, C only - no C++ runtime needed).
#
# Xet hashes are keyed BLAKE3, so this has to be bit-exact. It is: BLAKE3's output is defined to be
# independent of which implementation path computes it, and scripts/xet_oracle.py checks that rather
# than assuming it. The same is true of the per-ISA paths compiled below - which is exactly what the
# CI `selftest` job is for: it runs the 325 reference checks against the *built* linux_amd64 artifact,
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

if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
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
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64|aarch64_be)$")
    list(APPEND HF_BLAKE3_SOURCES ${HF_THIRD_PARTY}/blake3/blake3_neon.c)
    # NEON by autodetection, so only the x86 switches stay switched off
    set(HF_BLAKE3_DEFINITIONS BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif ()

add_library(hf_blake3_library OBJECT ${HF_BLAKE3_SOURCES})
target_include_directories(hf_blake3_library PUBLIC ${HF_THIRD_PARTY}/blake3)
target_compile_definitions(hf_blake3_library PRIVATE ${HF_BLAKE3_DEFINITIONS})

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:hf_library> $<TARGET_OBJECTS:hf_blake3_library>
    PARENT_SCOPE)
