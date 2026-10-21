#!/usr/bin/env python3
"""Walk a Parquet file's pages: count, sizes and offsets per column chunk.

DuckDB's `parquet_metadata()` stops at the column chunk, and pyarrow's metadata object stops there too, so
the one level of the file that content-defined page boundaries actually changes - the page - is invisible
from either. `docs/CDC_PARQUET_PAGES.md` is a claim about page boundaries, so this is the instrument that
checks it: for each column chunk it walks the page headers from the chunk's first page and prints how many
pages the writer made and how big they came out.

A page is `<Thrift-compact PageHeader><page bytes>`, the header's field 2 is `uncompressed_page_size` and
field 3 is `compressed_page_size`, and the next page starts `compressed_page_size` after the header ends.
That is all this script needs, so it is a header walker rather than a Thrift reader: it decodes field
headers, skips nested structs by counting their `0x00` terminator, and jumps over page bodies.

    scripts/parquet_pages.py file.parquet                    # one line per column chunk
    scripts/parquet_pages.py file.parquet --hist payload     # page size histogram for one column
    scripts/parquet_pages.py --compare v1.parquet v2.parquet # per column: pages whose *body* hashes match

`--compare` is the mode the document leans on: it hashes each page body (never the header, whose statistics
and row counts move when a neighbouring page changes) and reports, per column, how many pages of the second
file are byte-for-byte pages of the first. That is the mechanism behind the byte-level dedup numbers - a
page that survives is a page the uploader does not send.

The offset every page gets reported at is its *file* offset, which is what makes a page map comparable
against a chunk map from `xet-probe chunks` - the question being which upload chunks contain more than one
page, and which pages are too small to reach a chunk boundary of their own.
"""
import struct
import sys

# Thrift compact protocol type ids (arrow/parquet-cpp's TCompactProtocol), only the ones a PageHeader uses
CT_BOOLEAN_TRUE, CT_BOOLEAN_FALSE, CT_BYTE, CT_I16, CT_I32, CT_I64, CT_DOUBLE, CT_BINARY, CT_MAP, CT_LIST, \
    CT_SET, CT_STRUCT = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12

VARINT_TYPES = {CT_BYTE, CT_I16, CT_I32, CT_I64}


class Cursor:
    def __init__(self, data, offset=0):
        self.data = data
        self.pos = offset

    def byte(self):
        value = self.data[self.pos]
        self.pos += 1
        return value

    def varint(self):
        shift = 0
        result = 0
        while True:
            chunk = self.byte()
            result |= (chunk & 0x7F) << shift
            if not chunk & 0x80:
                return result
            shift += 7

    def zigzag(self):
        raw = self.varint()
        return (raw >> 1) ^ -(raw & 1)

    def skip_bytes(self):
        # not `self.pos += self.varint()`: augmented assignment reads self.pos before calling, so the
        # varint's own bytes would not be counted
        length = self.varint()
        self.pos += length

    def skip_struct(self):
        """A compact struct is a run of field headers, terminated by a 0x00 byte."""
        last_field = 0
        while True:
            header = self.byte()
            if header == 0:
                return
            delta = header >> 4
            type_id = header & 0x0F
            field_id = last_field + delta if delta else self.zigzag()
            last_field = field_id
            self.skip_value(type_id)

    def skip_value(self, type_id):
        if type_id == CT_BOOLEAN_TRUE or type_id == CT_BOOLEAN_FALSE:
            return  # the value is in the field header itself
        if type_id in VARINT_TYPES:
            self.varint()
        elif type_id == CT_DOUBLE:
            self.pos += 8
        elif type_id == CT_BINARY:
            self.skip_bytes()
        elif type_id in (CT_MAP, CT_SET):
            size = self.varint()
            if size:
                self.byte()  # <key><value> type pair, one byte
                for _ in range(size):
                    self.skip_struct()
        elif type_id == CT_LIST:
            header = self.byte()
            size = header >> 4
            element_type = header & 0x0F
            if size == 15:
                size = self.varint()
            for _ in range(size):
                self.skip_value(element_type)
        elif type_id == CT_STRUCT:
            self.skip_struct()
        else:
            raise ValueError(f"unknown thrift compact type id {type_id} at offset {self.pos}")


def read_page_header(data, offset):
    """Returns (uncompressed_size, compressed_size, is_dictionary, offset_after_header).

    A page header is small and flat, so it is walked inline rather than decoded: the sizes are fields 2
    and 3, and the page's kind is which of fields 5, 6, 7 is set (data page, data page v2, dictionary
    page). Nested structs are skipped on their *type* id, not their field id - `Statistics` also has
    fields 5 and 6, and reading them as structs walks off into the page body.
    """
    cur = Cursor(data, offset)
    uncompressed = compressed = None
    is_dictionary = False
    last_field = 0
    while True:
        header = cur.byte()
        if header == 0:
            break
        delta = header >> 4
        type_id = header & 0x0F
        field_id = last_field + delta if delta else cur.zigzag()
        last_field = field_id
        if field_id in (2, 3) and type_id in VARINT_TYPES:
            value = cur.zigzag()
            if field_id == 2:
                uncompressed = value
            else:
                compressed = value
        elif type_id == CT_STRUCT:
            is_dictionary = field_id == 7
            cur.skip_struct()
        else:
            cur.skip_value(type_id)
    if uncompressed is None or compressed is None:
        raise ValueError(f"page header at offset {offset} has no size fields")
    return uncompressed, compressed, is_dictionary, cur.pos


def column_chunks(metadata):
    """(path, first page offset, total compressed bytes) per column chunk, in file order."""
    out = []
    for row_group in range(metadata.num_row_groups):
        for column in range(metadata.num_columns):
            chunk = metadata.row_group(row_group).column(column)
            start = chunk.data_page_offset
            if chunk.dictionary_page_offset is not None and chunk.dictionary_page_offset > 0:
                start = min(start, chunk.dictionary_page_offset)
            out.append((row_group, chunk.path_in_schema, start, chunk.total_compressed_size))
    out.sort(key=lambda entry: entry[2])
    return out


def walk(data, start, total_size, limit=None):
    """Every page of one column chunk: (offset, uncompressed, compressed, is_dictionary)."""
    pages = []
    end = start + total_size
    pos = start
    while pos < end:
        uncompressed, compressed, is_dictionary, header_end = read_page_header(data, pos)
        pages.append((pos, uncompressed, compressed, is_dictionary))
        pos = header_end + compressed
        if limit and len(pages) >= limit:
            break
    return pages


def summarize(path, histogram_for=None, peer=None):
    import pyarrow.parquet as pq

    with open(path, "rb") as handle:
        data = handle.read()
    metadata = pq.ParquetFile(path).metadata
    rows = []
    for row_group, column, start, total in column_chunks(metadata):
        pages = walk(data, start, total)
        compressed = [p[2] for p in pages]
        uncompressed = [p[1] for p in pages if not p[3]]
        data_pages = [p for p in pages if not p[3]]
        rows.append(dict(
            row_group=row_group, column=column, pages=len(pages), data_pages=len(data_pages),
            dict_pages=len(pages) - len(data_pages), chunk_bytes=total,
            mean=sum(compressed) // len(compressed), minimum=min(compressed), maximum=max(compressed),
            max_uncompressed=max(uncompressed) if uncompressed else 0,
            first=pages[0][0], last=pages[-1][0],
            # how many pages are big enough that a byte-level chunker at a 64 KiB target would cut
            # *inside* one, which is the thing that makes an edit damage a neighbour page's chunk
            under_64k=sum(1 for size in data_pages if size[2] < 64 * 1024),
        ))
        if histogram_for and column == histogram_for:
            print(f"# page sizes for '{column}' in {path}")
            for offset, uncompressed, compressed, is_dictionary in pages:
                kind = "dict " if is_dictionary else "data "
                print(f"  {offset:>10} {kind} uncompressed={uncompressed:>9} compressed={compressed:>9}")
    return rows


def page_hashes(data, path):
    """(column, offset, compressed, sha256 of the page bytes) for every page in the file.

    The digest covers the page *body* only, not its header: a header carries per-page statistics, and a
    column's stats can move because a row elsewhere changed while the page's own bytes are untouched. What
    a byte-level chunker sees - and what a Xet chunk hash can match - is the body.
    """
    import hashlib
    import pyarrow.parquet as pq

    metadata = pq.ParquetFile(path).metadata
    out = []
    for _row_group, column, start, total in column_chunks(metadata):
        for offset, _uncompressed, compressed, is_dictionary in walk(data, start, total):
            _u, _c, _d, header_end = read_page_header(data, offset)
            body = data[header_end:header_end + compressed]
            out.append((column, offset, compressed, is_dictionary, hashlib.sha256(body).digest()))
    return out


def compare(path_a, path_b):
    """How much of b's pages something holding a already has, per column. The mechanism, not the upload."""
    with open(path_a, "rb") as handle:
        data_a = handle.read()
    with open(path_b, "rb") as handle:
        data_b = handle.read()
    pages_a = page_hashes(data_a, path_a)
    pages_b = page_hashes(data_b, path_b)
    pool = {}
    for column, _offset, _size, _dict, digest in pages_a:
        key = (column, digest)
        pool[key] = pool.get(key, 0) + 1
    per_column = {}
    for column, _offset, size, is_dictionary, digest in pages_b:
        entry = per_column.setdefault(column, [0, 0, 0, 0])
        entry[0] += 1
        entry[1] += size
        key = (column, digest)
        if pool.get(key, 0) > 0:
            pool[key] -= 1
            entry[2] += 1
            entry[3] += size
    print(f"\n=== pages shared: {path_a} -> {path_b} ===")
    header = (f"{'column':<24} {'pages':>6} {'identical':>10} {'of them':>8} "
              f"{'page bytes':>11} {'identical':>10} {'of them':>8}")
    print(header)
    print("-" * len(header))
    for column, (pages, total, shared, shared_bytes) in sorted(per_column.items()):
        print(f"{column:<24} {pages:>6} {shared:>10} {100.0 * shared / pages:>7.1f}% "
              f"{total / 1024 / 1024:>10.2f} MB {shared_bytes / 1024 / 1024:>9.2f} MB "
              f"{100.0 * shared_bytes / total:>7.1f}%")


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if "--compare" in argv:
        if len(args) != 2:
            print("--compare takes exactly two files")
            return 1
        compare(args[0], args[1])
        return 0
    histogram_for = None
    for arg in argv[1:]:
        if arg.startswith("--hist"):
            parts = arg.split("=", 1)
            histogram_for = parts[1] if len(parts) == 2 else None
    if not args:
        print(__doc__)
        return 1
    for path in args:
        print(f"\n=== {path} ===")
        rows = summarize(path, histogram_for)
        if histogram_for:
            continue
        header = (f"{'rg':>3} {'column':<24} {'pages':>6} {'dict':>5} {'chunk bytes':>12} "
                  f"{'mean':>8} {'min':>7} {'max':>8} {'<64KiB':>7}")
        print(header)
        print("-" * len(header))
        for row in rows:
            print(f"{row['row_group']:>3} {row['column']:<24} {row['data_pages']:>6} {row['dict_pages']:>5} "
                  f"{row['chunk_bytes']:>12} {row['mean']:>8} {row['minimum']:>7} {row['maximum']:>8} "
                  f"{row['under_64k']:>7}")
        totals = {}
        for row in rows:
            entry = totals.setdefault(row["column"], [0, 0, 0])
            entry[0] += row["data_pages"]
            entry[1] += row["chunk_bytes"]
            entry[2] += row["under_64k"]
        print(f"{'':>3} {'TOTAL':<24} {'':>6} {'':>5} {'':>12}")
        for column, (pages, size, under) in sorted(totals.items()):
            print(f"    {column:<22} {pages:>6} pages, {size / 1024 / 1024:6.1f} MB, {under} pages under 64 KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
