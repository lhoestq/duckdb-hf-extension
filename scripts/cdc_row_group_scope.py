#!/usr/bin/env python3
"""How much of a content-defined page's behaviour is still the row group, offline.

`scripts/cdc_page_measure.py` measures the grid; one column of that grid is worth isolating, because it is the
one that looks like a difference between two chunkers and mostly is not. A data page cannot span a row group,
so a row group is a forced cut whatever the hash says, and DuckDB's default (122880 rows) is small next to
pyarrow's (1048576). This script writes the same edited pair at Arrow's CDC sizes under three row-group
settings and asks the reference chunker what it already holds, so the row-group lever has a number of its own
and the rest is what is left over.

    python scripts/cdc_row_group_scope.py                 # 2M rows, the grid's `insert`
    python scripts/cdc_row_group_scope.py 200000 --edit update
    python scripts/cdc_row_group_scope.py 2000000 --plain  # the same pairs without CDC, as a control

The dataset, the edit and the seed are the grid's, copied rather than imported so the two scripts stay
readable on their own; percentages from here and from the grid are therefore comparable. Both versions are
written `NOT NULL` on both sides - a nullability flip rewrites more pages than any page policy changes, see
`docs/CDC_PARQUET_PAGES.md`. The chunker is xet-core's own, so a number here is what the real uploader would
find. The writes shell out to a patched `$DUCKDB`; the python module is a stock build and does not know
`CONTENT_DEFINED_CHUNKING`.
"""

import os
import subprocess
import sys

import duckdb

DUCKDB = os.environ.get("DUCKDB", "/home/quentin/hf/duckdb/build/release/duckdb")
PROBE = os.environ.get("XET_PROBE", "tools/xet_probe/target/release/xet-probe")
WORK = os.environ.get("CDC_WORK", "/tmp/cdc_rowgroup")
SEED = os.environ.get("CDC_SEED", "17")  # the grid's seed, so both scripts describe one dataset
CDC = "{min_chunk_size: 262144, max_chunk_size: 1048576}"  # arrow's defaults, as in the grid's duckdb-cdc row
EDITS = ("insert", "update", "append", "delete")


def die(message: str) -> None:
    sys.exit(f"cdc_row_group_scope: {message}")


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    edit = sys.argv[sys.argv.index("--edit") + 1] if "--edit" in sys.argv else "insert"
    if edit not in EDITS:
        die(f"--edit is one of {', '.join(EDITS)}, not {edit}")
    if not os.path.exists(DUCKDB):
        die(f"$DUCKDB points at {DUCKDB}, which is not a file")
    if not os.path.exists(PROBE):
        die(f"xet-probe not found at {PROBE}; `cargo build --release --manifest-path tools/xet_probe/Cargo.toml`")
    rows = int(args[0]) if args else 2_000_000
    mid = rows // 2 * 2
    os.makedirs(WORK, exist_ok=True)

    con = duckdb.connect()
    con.execute(f"""
        CREATE TABLE base AS
        SELECT (i * 2)::BIGINT AS id,
               hash(i)::DOUBLE / 1e18 AS x,
               (i % 997)::INTEGER AS kind,
               md5(hash(i + {SEED})::VARCHAR) || md5(hash(i + 7919 + {SEED})::VARCHAR) AS payload
        FROM range(1, {rows + 1}) t(i)
    """)
    queries = {
        "insert": f"""SELECT * FROM base UNION ALL
                      SELECT {mid + 1}::BIGINT, 0.5, 42, concat('ff', {mid}::VARCHAR, 'ee') ORDER BY id""",
        "update": f"""SELECT * REPLACE (CASE WHEN id = {mid} THEN concat('zz', payload[4:32]) ELSE payload END
                                       AS payload) FROM base ORDER BY id""",
        "append": f"""SELECT * FROM base UNION ALL
                      SELECT (i * 2 + 2 * {rows})::BIGINT, hash(i + 424242)::DOUBLE / 1e18, (i % 997)::INTEGER,
                             md5(hash(i + 31 + {SEED})::VARCHAR) || md5(hash(i + 8191 + {SEED})::VARCHAR)
                      FROM range(1, {max(rows // 100, 100)} + 1) t(i) ORDER BY id""",
        "delete": f"SELECT * FROM base WHERE id <> {mid} ORDER BY id",
    }
    # Declared NOT NULL on both sides: the writer's nullability comes from the projection, and an inferred
    # flip between the two versions would rewrite every page of every column and bury the thing under test.
    for name, path in (("v1", f"{WORK}/v1.parquet"), ("v2", f"{WORK}/v2.parquet")):
        query = "SELECT * FROM base ORDER BY id" if name == "v1" else queries[edit]
        con.execute(f"COPY ({query}) TO '{path}' (FORMAT PARQUET)")

    settings = [("default", None), ("1048576", 1048576), (f"one ({rows})", rows)]
    print(f"rows={rows}  edit={edit}  CDC {CDC}  written by {DUCKDB}\n")
    print(f"{'ROW_GROUP_SIZE':>16}  {'v2 (MB)':>9}  {'held':>7}")
    for label, row_group in settings:
        held, total, size = measure_pair(f"{WORK}/cdc_{label.replace(' ', '_')}", row_group)
        print(f"{label:>16}  {size / 1024 / 1024:8.1f}    {held / total:6.1%}")

    if "--plain" in sys.argv:  # the same pairs with no chunker at all: what the row group alone is worth
        print("\nsame pairs without CONTENT_DEFINED_CHUNKING:")
        for label, row_group in settings:
            held, total, size = measure_pair(f"{WORK}/plain_{label.replace(' ', '_')}", row_group, cdc=False)
            print(f"{label:>16}  {size / 1024 / 1024:8.1f}    {held / total:6.1%}")


def measure_pair(stem: str, row_group: int | None, cdc: bool = True) -> tuple[int, int, int]:
    """Write (v1, v2) at one row-group size with the patched writer, and ask what the store already holds."""
    extra = f", ROW_GROUP_SIZE {row_group}" if row_group else ""
    for source in ("v1", "v2"):
        path = f"{stem}_{source}.parquet"
        options = f", CONTENT_DEFINED_CHUNKING {CDC}" if cdc else ""
        out = subprocess.run([DUCKDB, "-c",
                              f"COPY (FROM read_parquet('{WORK}/{source}.parquet')) TO '{path}'"
                              f" (FORMAT PARQUET{options}{extra});"], capture_output=True, text=True)
        if out.returncode != 0:
            die(f"writing {path}: {out.stderr.strip()}")
    held, total = 0, 0
    seen = counts(chunks(f"{stem}_v1.parquet"))
    for digest, size in chunks(f"{stem}_v2.parquet"):
        if seen.get(digest, 0) > 0:
            seen[digest] -= 1
            held += size
        total += size
    return held, total, os.path.getsize(f"{stem}_v2.parquet")


def chunks(path: str) -> list[tuple[str, int]]:
    """(hash, size) per chunk, as xet-core's own chunker and hasher see this file's bytes."""
    with open(path, "rb") as handle:
        out = subprocess.run([PROBE, "chunks"], stdin=handle, capture_output=True, text=True)
    if out.returncode != 0:
        die(f"{PROBE} chunks failed on {path}: {out.stderr.strip()}")
    # one line per chunk: sequence, byte count, hash
    return [(parts[2], int(parts[1])) for parts in (line.split() for line in out.stdout.splitlines())]


def counts(chunks_of_file: list[tuple[str, int]]) -> dict[str, int]:
    """How many chunks of each hash a file holds - a multiset, so a repeated chunk cannot be matched twice."""
    pool: dict[str, int] = {}
    for digest, _size in chunks_of_file:
        pool[digest] = pool.get(digest, 0) + 1
    return pool


if __name__ == "__main__":
    main()
