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
# independent of which implementation path computes it, so the portable C is the same hash the Rust
# client's SIMD build produces - which scripts/xet_oracle.py checks rather than assumes.
#
# All SIMD is switched off so all six CI platforms compile one way from the same sources. The
# portable C is a few hundred MB/s, and a first Xet upload path is network-bound long before it is
# hash-bound; per-ISA dispatch is a later optimization, not a correctness question.
enable_language(C)
add_library(hf_blake3_library OBJECT
    ${HF_THIRD_PARTY}/blake3/blake3.c
    ${HF_THIRD_PARTY}/blake3/blake3_dispatch.c
    ${HF_THIRD_PARTY}/blake3/blake3_portable.c)
target_include_directories(hf_blake3_library PUBLIC ${HF_THIRD_PARTY}/blake3)
target_compile_definitions(hf_blake3_library PRIVATE
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
    BLAKE3_USE_NEON=0)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:hf_library> $<TARGET_OBJECTS:hf_blake3_library>
    PARENT_SCOPE)
