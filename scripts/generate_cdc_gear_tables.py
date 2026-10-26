#!/usr/bin/env python3
"""Regenerate the gear hash tables used for content-defined Parquet page boundaries.

The tables are Apache Arrow's, byte for byte: arrow-cpp generates them in
cpp/src/parquet/chunker_internal_codegen.py and arrow-rs checks in the same numbers as
`parquet/src/column/chunker/cdc_generated.rs`. DuckDB deliberately reproduces them rather
than inventing its own so that a column written by DuckDB, by arrow-cpp and by arrow-rs with
the same (min_chunk_size, max_chunk_size, norm_level) puts its page boundaries in the same
places. If this script's recipe drifts from arrow's, every file written with
content_defined_chunking moves its boundaries and the parity the chunker exists to provide is
gone - so the check below against the values arrow checks in is part of this script, not an
afterthought.

This script deliberately lives in the extension repo, not in the DuckDB fork: the fork carries
the generated table and nothing else, so the recipe and its self-check are kept where the rest of
the content-defined-page tooling is (scripts/cdc_conformance.py and friends). Run it from here.

Usage: python3 scripts/generate_cdc_gear_tables.py > ../duckdb/extension/parquet/include/writer/parquet_cdc_gear_tables.hpp
"""

import hashlib
import sys

NUM_TABLES = 8
ENTRIES = 256

# The first entries of table 0, as arrow-cpp and arrow-rs both check them in. Any recipe that
# does not reproduce these numbers is not Arrow's recipe.
KNOWN_FIRST = [0xF09F35A563783945, 0x0DCC5B3BC5AE410A, 0x63F1EA8D22554270, 0xFBE5EE7BD05A7B61]


def generate_table(seed: int):
    """Arrow's generate_hashtable: md5(bytes([seed]*64 + [entry]*64))[:16] read as hex."""
    table = []
    for entry in range(ENTRIES):
        digest = hashlib.md5(bytes([seed] * 64 + [entry] * 64)).hexdigest()
        table.append(int(digest[:16], 16))
    return table


def main():
    tables = [generate_table(seed) for seed in range(NUM_TABLES)]
    if tables[0][:4] != KNOWN_FIRST:
        sys.exit(f"table 0 starts with {[hex(x) for x in tables[0][:4]]}, "
                 f"expected {[hex(x) for x in KNOWN_FIRST]} - this is not arrow's recipe")

    print("//===----------------------------------------------------------------------===//")
    print("//                         DuckDB")
    print("//")
    print("// writer/parquet_cdc_gear_tables.hpp")
    print("//")
    print("//")
    print("//===----------------------------------------------------------------------===//")
    print()
    print("// Apache Arrow's gear hash tables, in Arrow's own numbers - do not edit by hand.")
    print("//")
    print("// Each table is generated the way Arrow generates them: the first 16 hex digits of")
    print("// md5(bytes([seed] * 64 + [byte] * 64)), read as a number, for seed 0..7 and byte 0..255.")
    print("// That is arrow-cpp's recipe (cpp/src/parquet/chunker_internal_codegen.py) and the same")
    print("// numbers arrow-rs checks in (parquet/src/column/chunker/cdc_generated.rs), which is why")
    print("// DuckDB, arrow-cpp and arrow-rs put content-defined page boundaries in the same places.")
    print("// Table 0 therefore starts with 0xf09f35a563783945, 0x0dcc5b3bc5ae410a,")
    print("// 0x63f1ea8d22554270, 0xfbe5ee7bd05a7b61: a regeneration that does not reproduce those")
    print("// four is not Arrow's recipe, and boundaries would silently stop matching the other two.")
    print()
    print("#pragma once")
    print()
    print("#include \"duckdb/common/common.hpp\"")
    print()
    print("namespace duckdb {")
    print()
    print("//! Number of gear hash tables. A page boundary needs one mask hit per table, in order; see")
    print("//! ParquetContentDefinedChunker.")
    print(f"static constexpr idx_t PARQUET_CDC_NUM_GEAR_TABLES = {NUM_TABLES};")
    print()
    print("static constexpr uint64_t PARQUET_CDC_GEAR_TABLES[PARQUET_CDC_NUM_GEAR_TABLES][256] = {")
    # One table per brace, four entries per row: the layout is fixed so that a diff against a re-run of
    # this script is a diff of numbers and nothing else. clang-format would re-pack the rows to 120 columns.
    print("    // clang-format off")
    for seed, table in enumerate(tables):
        print(f"\t{{// seed = {seed}")
        for i in range(0, ENTRIES, 4):
            row = ", ".join(f"0x{v:016x}" for v in table[i:i + 4])
            end = "," if i + 4 < ENTRIES else ""
            print(f"\t {row}{end}")
        print("\t}" + ("," if seed + 1 < NUM_TABLES else ""))
    print("    // clang-format on")
    print("};")
    print()
    print("} // namespace duckdb")


if __name__ == "__main__":
    main()
