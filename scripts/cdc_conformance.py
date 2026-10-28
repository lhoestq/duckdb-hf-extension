#!/usr/bin/env python3
"""Do DuckDB and arrow cut this column in the same places?

The chunker in the DuckDB fork is a port of arrow's, and "port" is a claim that either holds for every column
and every size or is worth nothing. This checks it where it can be checked: the same values, the same
min_chunk_size / max_chunk_size / norm_level, comparing the *sequence of values per data page* each writer
produces. That is the boundary question, phrased so that page headers, statistics, dictionary pages and
codecs - which the writers genuinely do not agree on - stay out of it. See scripts/parquet_page_values.py.

Three files are written from one set of values:

  arrow_cpp.parquet  pyarrow, content defined            the expectation
  duckdb.parquet     duckdb, content defined             compared against the expectation
  duckdb_plain.parquet duckdb, no content defined        what a column that is not cut by content must match

The columns cover the shapes that matter, and the last two are ones the fork deliberately refuses to cut (see
ParquetCdcSupportedType) - those have to come out exactly as a plain write leaves them, because a column that
is *almost* cut by content, with boundaries that look comparable and are not, is worse than one that is not.

    python3 scripts/cdc_conformance.py                          # defaults: 256KB, 1MB, norm 0
    python3 scripts/cdc_conformance.py --min 64KB --max 1MB --norm 1
    python3 scripts/cdc_conformance.py --save-fixture f.json    # also freeze arrow's answer
    python3 scripts/cdc_conformance.py --check-fixture f.json   # duckdb against a frozen answer

arrow-rs is not driven from here - it has no python binding - but that is what the fixture is for: freeze
arrow-cpp's page counts, then run the same column through parquet's ArrowWriter with
set_content_defined_chunking and diff. If the two Arrows ever drift apart, the fixture says so.
"""

import argparse
import decimal
import json
import os
import subprocess
import sys
import tempfile
import uuid as uuid_module
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from parquet_page_values import page_values  # noqa: E402

DEFAULT_DUCKDB = "/home/quentin/hf/duckdb/build/release/duckdb"
ROWS = 60_000
NULLABLE = True

# arrow's defaults (parquet::WriterProperties), and the row count these fixtures were recorded at
DEFAULT_MIN = 262144
DEFAULT_MAX = 1048576
DEFAULT_ROWS = 400_000

try:
    import pyarrow  # noqa: F401
    HAVE_PYARROW = True
except ImportError:
    HAVE_PYARROW = False

# name, duckdb type, arrow type, generator, cut_by_content. Values follow from the row number alone, because
# two clients build them independently and have to end up with the same bytes.
#
# NULLABLE decides whether every column carries a null every 977th row. It is one switch for all columns and
# part of the fixture, not an accident of the data: a nullable column feeds the chunker two definition-level
# bytes per row that a non-nullable column does not feed, so the two cases cut at different values and both
# have to be checked. It also has to *agree* between the writers, which is subtler than it sounds - duckdb
# reads a column's nullability from the parquet statistics, so a null-free column comes back non-nullable and
# is written back REQUIRED whatever the input schema said, and pyarrow will only write a REQUIRED field if the
# arrow schema says so. Both writers end up in the same place, but by different routes.
COLUMNS = [
    ("i", "BIGINT", "int64", lambda r: r, True, False),
    ("s", "VARCHAR", "string", lambda r: f"{(r * 7919) % (10 ** 32):032x}" * 4, True, True),
    ("short", "VARCHAR", "string", lambda r: f"v{r % 997}", True, True),
    ("d", "DOUBLE", "float64", lambda r: (r % 10007) * 1.0000000001, True, False),
    ("f", "FLOAT", "float32", lambda r: (r % 511) * 0.5, True, True),
    ("b", "BOOLEAN", "bool_", lambda r: r % 3 == 0, True, False),
    ("u", "UBIGINT", "uint64", lambda r: r * 2654435761, True, False),
    # left on size-based cuts, see ParquetCdcSupportedType: arrow's chunker reads a decimal128 array, sixteen
    # bytes per value, where duckdb keeps a DECIMAL(18,3) in eight; uuid is a fixed size binary there and a
    # hugeint here
    ("uuid", "UUID", "uuid", lambda r: f"{r:032x}", False, True),
    ("dec", "DECIMAL(18,3)", "decimal128", lambda r: (r % 100000) / 1000.0, False, True),
]


def build_data(work: Path) -> Path:
    """The values, once, in a form both clients can read: plain parquet, no dictionary, no compression, so
    that whatever differs afterwards differs because of the chunker and nothing else."""
    import pyarrow as pa
    import pyarrow.compute as pc
    import pyarrow.parquet as pq

    arrays = {}
    for name, duck_type, arrow_type, gen, cut, _ in COLUMNS:
        values = [None if (NULLABLE and r % 977 == 0) else gen(r) for r in range(ROWS)]
        if arrow_type == "decimal128":
            # pyarrow wants exact decimals, and the .3f holds the values to the column's scale
            array = pa.array([None if v is None else decimal.Decimal(f"{v:.3f}") for v in values],
                             type=pa.decimal128(18, 3))
        elif arrow_type == "uuid":
            array = pa.array([None if v is None else uuid_module.UUID(v) for v in values], type=pa.uuid())
        else:
            array = pa.array(values, type=getattr(pa, arrow_type)())
        arrays[name] = array
    table = pa.table(arrays)
    path = work / "values.parquet"
    pq.write_table(table, path, use_dictionary=False, compression=None, data_page_version="1.0")
    read_back = pq.read_table(path)
    assert read_back.num_rows == ROWS
    # the values really are the ones both writers were meant to see
    assert sum(c.null_count for c in read_back.columns) == (
        len(COLUMNS) * sum(1 for r in range(ROWS) if r % 977 == 0) if NULLABLE else 0)
    assert pc.sum(read_back["i"]).as_py() == sum(r for r in range(ROWS) if not (NULLABLE and r % 977 == 0))
    return path


def write_arrow(values_path: Path, out_path: Path, options: dict, rows: int):
    import pyarrow as pa
    import pyarrow.parquet as pq

    # No dictionary, because arrow-cpp chunks a dictionary array by its *indices* and what is being compared
    # here is the value feed. duckdb hashes values whichever encoding it settles on, so values-vs-values is
    # the comparison that means something - see the dictionary row of the comparison in docs.
    #
    # max_rows_per_page is raised out of the way: arrow also closes a page every 20000 rows whatever the
    # chunker says, which is a page rule duckdb has no equivalent of, and leaving it at its default would
    # compare arrow's row counting against duckdb's content for columns of narrow values. arrow closes pages
    # at 1 MB uncompressed as well (not exposed through python); keep max_chunk_size at or below that and the
    # two rules cannot disagree.
    table = pq.read_table(values_path)
    if not NULLABLE:
        # the arrow side has to be told, where duckdb reads it from the statistics on its own
        schema = pa.schema([pa.field(f.name, f.type, nullable=False) for f in table.schema])
        table = table.cast(schema)
    pq.write_table(table, out_path, use_dictionary=False, compression=None, max_rows_per_page=10 ** 9,
                   row_group_size=rows,
                   use_content_defined_chunking={"min_chunk_size": size_bytes(options["min_chunk_size"]),
                                                 "max_chunk_size": size_bytes(options["max_chunk_size"]),
                                                 "norm_level": options["norm_level"]})


def write_duckdb(duckdb_shell: str, values_path: Path, out_path: Path, options: dict, rows: int):
    if options:
        # quoted only when it is a size with a unit - duckdb reads a bare number in a string as bytes as well,
        # but there is no reason to make it look like a size when it is not one
        fields = ", ".join(f"{name}: '{value}'" if isinstance(value, str) and not value.isdigit() else f"{name}: {value}"
                           for name, value in options.items())
        cdc = f", CONTENT_DEFINED_CHUNKING {{ {fields} }}"
    else:
        cdc = ""
    # row_group_size is pinned above the row count so the whole table is one row group, on both sides (arrow
    # gets it from pyarrow's own 1M-row default). Row groups are held out of this comparison for a version
    # reason rather than a design one: this build carries the chunker state across row groups, which is what
    # arrow's column_writer.cc comment promises and what arrow-cpp actually does only since #51685 (merged
    # 2026-10-12, three days after 26.0.0 was cut), while pyarrow 26.0.0 - the arrow these fixtures were
    # recorded against - restarts the chunker at every row group. Leave two row groups in and the harness is
    # measuring that release gap and reporting it as a chunker disagreement. Once a release carries #51685 the
    # pin can come out, and the multi-row-group fixture becomes the interesting one.
    query = (f"COPY (FROM read_parquet('{values_path}')) TO '{out_path}' "
             f"(FORMAT PARQUET, ROW_GROUP_SIZE {rows * 2}{cdc})")
    subprocess.run([duckdb_shell, "-c", query], check=True, capture_output=True, text=True)


def pages_by_column(pages):
    result = {}
    for entry in pages:
        result.setdefault(entry["path"], []).extend(entry["pages"])
    return result


def cut_offsets(pages):
    """The value index each page starts at - the shape of a file in one list, and the only form in which two
    writers' page lists can be compared meaningfully, since a list of lengths hides where the cuts are."""
    total, offsets = 0, []
    for size in pages:
        offsets.append(total)
        total += size
    return offsets


def only_refined(expected, actual):
    """True when the reference cut everywhere duckdb cut and somewhere more. That is what arrow does to a
    column whose pages fill to the ceiling: its writer closes a page at 1 MiB of encoded bytes as well, and
    counts a string's 4-byte length prefix there, while the chunker counts values and 2-byte levels. So the
    chunker agrees and arrow's surroundings cut again - a difference about page rules, not about boundaries.

    The totals have to be equal for that reading to mean anything: a shorter file also looks like a column of
    extra cuts, and that is a broken file, not a refined one."""
    if sum(expected) != sum(actual):
        return False
    wanted, got = cut_offsets(expected), cut_offsets(actual)
    return set(got) <= set(wanted) and len(wanted) > len(got)


def report(name, expected, actual, expectation, strict=False):
    """One line per column, and True when the column failed. `expectation` is what this column is supposed to
    do: 'content' means match arrow, 'size' means stay where a plain write leaves it."""
    match = expected == actual
    mean = sum(expected) // max(len(expected), 1)
    refined = not match and expectation == "content" and only_refined(expected, actual)
    if expectation == "content":
        verdict = "matches arrow" if match else (
            "arrow cut again inside duckdb's pages (its own page rules)" if refined else "DIFFERS FROM ARROW")
    else:
        verdict = "not cut, as plain" if match else "CHANGED WITHOUT BEING CUT"
    print(f"  {name:8} {len(expected):4} {len(actual):4}  {mean:8}  {expectation:8}  {verdict}")
    if expectation == "content" and not match:
        for index, (want, got) in enumerate(zip(expected, actual)):
            if want != got:
                print(f"           first difference at page {index}: expected {want}, duckdb {got}")
                break
    if refined and strict:
        return True
    # a content column that disagrees is a failure, and so is a size column that moved; the one exception is
    # arrow cutting again inside a page duckdb filled, which its own page rules account for
    if expectation == "content":
        return not match and not (refined and not strict)
    return not match


def main():
    parser = argparse.ArgumentParser()
    # byte counts, not sizes with units: see size_bytes. arrow has no notion of a size at all beyond a number
    # every one of these defaults to "whatever the fixture says" when a fixture is being checked, and to arrow's
    # own defaults otherwise - the five of them are the fixture's conditions, so retyping them to check a fixture
    # is an invitation to check one thing against another's answer
    parser.add_argument("--min", default=None, help="bytes, or a size in duckdb's reading (KB=10^3, KiB=2^10)")
    parser.add_argument("--max", default=None)
    parser.add_argument("--norm", type=int, default=None)
    parser.add_argument("--nulls", choices=["mixed", "none"], default=None,
                        help="whether every column carries nulls (default) or none of them do")
    parser.add_argument("--rows", type=int, default=None)
    parser.add_argument("--duckdb", default=os.environ.get("DUCKDB", DEFAULT_DUCKDB))
    parser.add_argument("--save-fixture", default=None)
    parser.add_argument("--check-fixture", default=None)
    parser.add_argument("--keep", default=None, help="directory to leave the parquet files in")
    parser.add_argument("--values", default=None,
                        help="an existing values.parquet to write from, instead of building one")
    parser.add_argument("--strict", action="store_true",
                        help="fail on pages arrow cut again inside, which its own page rules explain")
    args = parser.parse_args()

    fixture = json.loads(Path(args.check_fixture).read_text()) if args.check_fixture else None
    # the fixture carries its own conditions, so anything not typed on the command line is taken from it
    args.rows = args.rows or (fixture.get("rows") if fixture else DEFAULT_ROWS)
    args.nulls = args.nulls or (fixture.get("nulls") if fixture else "mixed")
    given = {"min_chunk_size": args.min, "max_chunk_size": args.max, "norm_level": args.norm}
    if fixture:
        for name, value in fixture["options"].items():
            given[name] = given[name] if given[name] is not None else value
    for name, fallback in (("min_chunk_size", DEFAULT_MIN), ("max_chunk_size", DEFAULT_MAX), ("norm_level", 0)):
        given[name] = given[name] if given[name] is not None else fallback
    options = normalize_options(given)

    global NULLABLE, ROWS
    NULLABLE = args.nulls == "mixed"
    ROWS = args.rows
    work = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="cdc_conformance_"))
    work.mkdir(parents=True, exist_ok=True)

    print(f"options: {options}, nulls in every column: {NULLABLE}")
    print(f"files in {work}")
    print(f"  {'column':8} {'pages':>6} {'pages':>6}  {'mean':>6}  {'expected':8}  verdict")

    if fixture:
        if normalize_options(fixture["options"]) != options or fixture.get("rows") != args.rows \
                or fixture.get("nulls") != args.nulls:
            print(f"fixture was written for {fixture['options']} over {fixture.get('rows')} rows with "
                  f"nulls={fixture.get('nulls')}, not {options} over {args.rows} rows with nulls={args.nulls} - "
                  f"pass --min/--max/--norm/--rows/--nulls to check a different configuration", file=sys.stderr)
            return 2
        values_path = Path(args.values or fixture["values_file"])
        if not values_path.exists():
            # the values follow from the row number alone, so a fixture without its values file is still a
            # checkable fixture - it just takes a few seconds longer and needs pyarrow to rebuild them
            print(f"  ({values_path} is gone, rebuilding the values from rows={args.rows}, nulls={args.nulls})")
            if not HAVE_PYARROW:
                print("  and pyarrow is not installed to rebuild them with", file=sys.stderr)
                return 2
            values_path = build_data(work)
        write_duckdb(args.duckdb, values_path, work / "duckdb.parquet", options, args.rows)
        write_duckdb(args.duckdb, values_path, work / "duckdb_plain.parquet", None, args.rows)
        expected_pages = pages_by_column(fixture["pages"])
        plain_pages = pages_by_column(page_values(args.duckdb, str(work / "duckdb_plain.parquet")))
        duckdb_pages = pages_by_column(page_values(args.duckdb, str(work / "duckdb.parquet")))
        print("  (arrow-cpp side comes from the frozen fixture)")
        return 1 if any_failure(COLUMNS, expected_pages, plain_pages, duckdb_pages, "arrow-cpp (fixture)",
                                args.strict) else 0

    if not HAVE_PYARROW:
        print("pyarrow is not installed - use --check-fixture with a frozen answer instead", file=sys.stderr)
        return 2

    values_path = Path(args.values) if args.values else build_data(work)
    write_arrow(values_path, work / "arrow_cpp.parquet", options, args.rows)
    write_duckdb(args.duckdb, values_path, work / "duckdb.parquet", options, args.rows)
    write_duckdb(args.duckdb, values_path, work / "duckdb_plain.parquet", None, args.rows)

    arrow_pages = pages_by_column(page_values(args.duckdb, str(work / "arrow_cpp.parquet")))
    duckdb_pages = pages_by_column(page_values(args.duckdb, str(work / "duckdb.parquet")))
    plain_pages = pages_by_column(page_values(args.duckdb, str(work / "duckdb_plain.parquet")))
    failed = any_failure(COLUMNS, arrow_pages, plain_pages, duckdb_pages, "arrow-cpp", args.strict)

    if args.save_fixture:
        Path(args.save_fixture).write_text(json.dumps({"options": options, "rows": args.rows, "nulls": args.nulls,
                                                       "values_file": str(values_path),
                                                       "pages": page_values(args.duckdb,
                                                                             str(work / "arrow_cpp.parquet"))},
                                                      indent=1))
        print(f"fixture written to {args.save_fixture} - it records its own conditions and the path of the values "
              f"it was built from ({values_path}); that path is a convenience, since the same values follow from "
              f"rows={args.rows} and nulls={args.nulls} alone and are rebuilt if it is gone")
    return 1 if failed else 0


def any_failure(columns, arrow_pages, plain_pages, duckdb_pages, arrow_label, strict=False):
    """Every column is compared against whichever of the two references applies to it. The header line is
    printed by the caller, so this stays a plain loop over columns."""
    failed = False
    for name, duck_type, arrow_type, gen, cut, nullable in columns:
        expectation = "content" if cut else "size"
        reference = arrow_pages if cut else plain_pages
        reference_name = arrow_label if cut else "duckdb plain"
        if name not in reference:
            print(f"  {name:8} reference pages missing from {reference_name}")
            failed = True
            continue
        failed |= report(name, reference[name], duckdb_pages.get(name, []), expectation, strict)
    return failed


UNITS = {"K": (1000, 1024), "M": (1000 ** 2, 1024 ** 2), "G": (1000 ** 3, 1024 ** 3), "T": (1000 ** 4, 1024 ** 4)}


def normalize_options(options):
    """Sizes as byte counts, so that 262144, '262144' and '256KiB' are the same option and a fixture recorded
    with one spelling compares equal to another. norm_level is a level, not a size."""
    return {name: (int(value) if name == "norm_level" else size_bytes(value)) for name, value in options.items()}


def size_bytes(size):
    """A size read the way duckdb reads it, because the same number has to reach arrow, which only takes byte
    counts. duckdb parses these with DBConfig::ParseMemoryLimit, where KB is 10^3 and KiB is 2^10 - '1MB' is
    1000000, '1MiB' is 1048576 - and reading one name as two different sizes would show up as a chunker
    disagreement that has nothing to do with the chunker. Plain byte counts carry no such trap, which is why
    they are what this script passes by default."""
    if isinstance(size, int):
        return size
    text = size.strip().upper().rstrip("B")
    if text[-1].isdigit():
        return int(float(text))
    binary = text.endswith("I")
    if binary:
        text = text[:-1]
    decimal, binary_size = UNITS[text[-1]]
    return int(float(text[:-1]) * (binary_size if binary else decimal))


if __name__ == "__main__":
    sys.exit(main())
