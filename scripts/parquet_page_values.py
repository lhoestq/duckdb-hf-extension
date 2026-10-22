#!/usr/bin/env python3
"""Where a Parquet file's data pages start and end, in value indices.

`min_chunk_size` and `max_chunk_size` are about *values*: a content-defined boundary falls between two
values, and which value that is depends only on the values before it. Page byte offsets, which is what
parquet_metadata reports, also depend on the page header, on the statistics written into it, on whether the
column got a dictionary page and on the codec - so two writers that agree perfectly on boundaries still
disagree on offsets. This script therefore reads the per-page value counts straight out of the page headers
(a DATA_PAGE header carries num_values) and reports each leaf column as the list of values each of its data
pages holds. Two writers that agree on boundaries agree on those lists, and nothing else has to be equal for
the comparison to mean something.

The page headers are found by walking from the first page of each column chunk - offsets that come from
parquet_metadata, which DuckDB computes - and reading one thrift compact struct per page. Sums are checked
against the column chunk's num_values, so a parser that misreads a header says so instead of quietly
reporting different boundaries.

    python3 scripts/parquet_page_values.py file.parquet [more.parquet ...]
    python3 scripts/parquet_page_values.py --json file.parquet > pages.json

The DuckDB shell used for the metadata walk is found in $DUCKDB, or the patched build, or PATH.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

DEFAULT_DUCKDB = "/home/quentin/hf/duckdb/build/release/duckdb"

# thrift compact protocol field types. The numbers are the ones parquet's thrift uses - note that STRUCT is
# 12, not 11 (apache/thrift's TCompactProtocol.tcc has CT_STRUCT = 0x0C, which is what both arrow and duckdb
# write), and that there is no uuid type at all.
COMPACT_STOP = 0
COMPACT_TRUE = 1
COMPACT_FALSE = 2
COMPACT_BYTE = 3
COMPACT_I16 = 4
COMPACT_I32 = 5
COMPACT_I64 = 6
COMPACT_DOUBLE = 7
COMPACT_BINARY = 8
COMPACT_LIST = 9
COMPACT_SET = 10
COMPACT_MAP = 11
COMPACT_STRUCT = 12

# parquet PageHeader
PAGE_TYPE = 1
PAGE_COMPRESSED_SIZE = 3
PAGE_DATA_HEADER = 5
PAGE_DICTIONARY_HEADER = 7
PAGE_DATA_HEADER_V2 = 8
# and the num_values field inside each of those page headers
NUM_VALUES_FIELD = 1

DATA_PAGE = 0
DICTIONARY_PAGE = 2


class Cursor:
    def __init__(self, buffer: bytes, offset: int = 0):
        self.buffer = buffer
        self.pos = offset

    def byte(self) -> int:
        value = self.buffer[self.pos]
        self.pos += 1
        return value

    def varint(self) -> int:
        result = 0
        shift = 0
        while True:
            value = self.byte()
            result |= (value & 0x7F) << shift
            if not value & 0x80:
                return result
            shift += 7

    def zigzag(self) -> int:
        value = self.varint()
        return (value >> 1) ^ -(value & 1)

    def binary(self) -> bytes:
        length = self.varint()
        value = self.buffer[self.pos : self.pos + length]
        self.pos += length
        return value

    def skip(self, field_type: int) -> None:
        if field_type in (COMPACT_TRUE, COMPACT_FALSE):
            return
        if field_type == COMPACT_BYTE:
            self.byte()
        elif field_type in (COMPACT_I16, COMPACT_I32, COMPACT_I64):
            self.varint()
        elif field_type == COMPACT_DOUBLE:
            self.pos += 8
        elif field_type == COMPACT_BINARY:
            self.binary()
        elif field_type in (COMPACT_LIST, COMPACT_SET):
            header = self.byte()
            size = header >> 4
            element_type = header & 0x0F
            if size == 15:
                size = self.varint()
            for _ in range(size):
                self.skip(element_type)
        elif field_type == COMPACT_MAP:
            size = self.varint()
            if size:
                kinds = self.byte()
                for _ in range(size):
                    self.skip(kinds >> 4)
                    self.skip(kinds & 0x0F)
        elif field_type == COMPACT_STRUCT:
            Struct(self).skip()
        else:
            raise ValueError(f"unknown compact field type {field_type}")


class Struct:
    """One thrift compact struct, read field by field. Field ids are deltas, and a struct ends on a
    zero byte; that is the whole protocol for our purposes."""

    def __init__(self, cursor: Cursor):
        self.cursor = cursor

    def fields(self):
        last_id = 0
        while True:
            header = self.cursor.byte()
            if header == COMPACT_STOP:
                return
            delta = header >> 4
            field_type = header & 0x0F
            field_id = self.cursor.zigzag() if delta == 15 else last_id + delta
            last_id = field_id
            yield field_id, field_type

    def skip(self) -> None:
        for _, field_type in self.fields():
            self.cursor.skip(field_type)


def read_page_header(buffer: bytes, offset: int):
    """Returns (page_type, num_values_or_None, compressed_page_size) for the page starting at offset."""
    cursor = Cursor(buffer, offset)
    page_type = None
    compressed_size = None
    num_values = None
    for field_id, field_type in Struct(cursor).fields():
        if field_id == PAGE_TYPE and field_type in (COMPACT_I32, COMPACT_I16):
            page_type = cursor.zigzag()
        elif field_id == PAGE_COMPRESSED_SIZE and field_type in (COMPACT_I32, COMPACT_I16):
            compressed_size = cursor.zigzag()
        elif field_id in (PAGE_DATA_HEADER, PAGE_DATA_HEADER_V2, PAGE_DICTIONARY_HEADER) and field_type == COMPACT_STRUCT:
            num_values = read_page_header_values(cursor)
        else:
            cursor.skip(field_type)
    return page_type, num_values, compressed_size


def read_page_header_values(cursor: Cursor) -> int:
    """The num_values field of a DataPageHeader / DataPageHeaderV2 / DictionaryPageHeader."""
    num_values = None
    for field_id, field_type in Struct(cursor).fields():
        if field_id == NUM_VALUES_FIELD and field_type in (COMPACT_I32, COMPACT_I16):
            num_values = cursor.zigzag()
        else:
            cursor.skip(field_type)
    if num_values is None:
        raise ValueError("page header without a num_values field")
    return num_values


def column_chunks(duckdb_shell: str, path: str):
    """One entry per (row group, leaf column), with the byte range its pages live in."""
    query = (
        "select path_in_schema, row_group_id, data_page_offset, total_compressed_size, num_values, "
        "coalesce(dictionary_page_offset, -1) as dict_offset "
        f"from parquet_metadata('{path}') order by data_page_offset"
    )
    out = subprocess.run([duckdb_shell, "-csv", "-noheader", "-c", query], capture_output=True, text=True, check=True)
    chunks = []
    for line in out.stdout.strip().splitlines():
        fields = line.split(",")
        chunks.append(
            {
                "path": fields[0],
                "row_group": int(fields[1]),
                "offset": int(fields[2]),
                "size": int(fields[3]),
                "num_values": int(fields[4]),
            }
        )
    return chunks


def page_values(duckdb_shell: str, path: str):
    """[(column path, row group, [values per data page])], in file order."""
    buffer = Path(path).read_bytes()
    results = []
    for chunk in column_chunks(duckdb_shell, path):
        per_page = []
        pos = chunk["offset"]
        # the end is reached by counting values, not bytes: total_compressed_size is not a byte range over the
        # pages alone - duckdb, for one, writes a bloom filter after them and counts it in. Stop when the
        # pages hold what the column chunk says they hold.
        seen = 0
        while seen < chunk["num_values"]:
            page_type, num_values, compressed_size = read_page_header(buffer, pos)
            if page_type == DATA_PAGE:
                per_page.append(num_values)
                seen += num_values
            pos += compressed_size + header_length(buffer, pos)
        total = sum(per_page)
        if total != chunk["num_values"]:
            raise ValueError(
                f"{path}: {chunk['path']} row group {chunk['row_group']}: pages hold {total} values, "
                f"the column chunk says {chunk['num_values']}"
            )
        results.append({"path": chunk["path"], "row_group": chunk["row_group"], "pages": per_page})
    return results


def header_length(buffer: bytes, offset: int) -> int:
    """How many bytes the page header at offset occupies - needed to step over it to the body."""
    cursor = Cursor(buffer, offset)
    Struct(cursor).skip()
    return cursor.pos - offset


def main():
    paths = []
    as_json = False
    for arg in sys.argv[1:]:
        if arg == "--json":
            as_json = True
        else:
            paths.append(arg)
    duckdb_shell = os.environ.get("DUCKDB", DEFAULT_DUCKDB)
    if not paths:
        print(__doc__)
        return 1
    for path in paths:
        results = page_values(duckdb_shell, path)
        if as_json:
            print(json.dumps({path: results}, indent=1))
            continue
        print(path)
        for entry in results:
            pages = entry["pages"]
            print(f"  {entry['path']} rg{entry['row_group']}: {len(pages)} pages, "
                  f"{pages[0]}..{pages[-1]} values, {sum(pages)} total")
    return 0


if __name__ == "__main__":
    sys.exit(main())
