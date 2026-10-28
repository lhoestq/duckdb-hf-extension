#!/usr/bin/env python3
"""How much of an edited Parquet file would a Xet-like store already hold?

One DuckDB table, written twice - once as it is, once after an edit - into a store that chunks by content. The
answer is the share of the second file's bytes the store did not have to take. Two DuckDBs are in play: the
writes go through the shell at $DUCKDB, because `CONTENT_DEFINED_CHUNKING` only exists in the writer patch
(duckdb fork, branch `content-defined-pages`) and that build has no python binding; everything after a write
goes through the `duckdb` import and the `nxet://` protocol importing nano-xet registers with fsspec, where
`con.register_filesystem(fs)` lets DuckDB open the file out of the store - the check that what it measured is
the file the writer wrote - and `fs.store.stats()` is what the ratio comes from.

    scripts/dedup_estimate.py                                    # 500k rows, three edits, CDC vs size cuts
    scripts/dedup_estimate.py --rows 2000000 --edit insert --row-group 2000000
    scripts/dedup_estimate.py --cdc-min 32768 --cdc-max 262144
    scripts/dedup_estimate.py --size-limit default,1048576       # baselines only

Needs `pip install 'nano-xet>=0.1.2' pyarrow duckdb` (numpy makes nano-xet's chunker much faster; 0.1.2 is the
release that answers `modified` with a datetime, which is what DuckDB's fsspec bridge asks of it). The chunker
is nano-xet's own - gear hash, 8 / 64 / 128 KiB - not xet-core's, so these percentages are directional: a file
wants a few dozen chunks before a number means anything, and `scripts/cdc_page_measure.py` is the one whose
numbers `docs/CDC_PARQUET_PAGES.md` quotes.
"""

import argparse
import os
import subprocess
import sys
import tempfile

import duckdb
import nano_xet

DUCKDB = os.environ.get("DUCKDB", "/home/quentin/hf/duckdb/build/release/duckdb")
EDITS = ("append", "update", "insert")


def die(message: str) -> None:
    sys.exit(f"dedup_estimate: {message}")


def run_duckdb(statements: list) -> None:
    """One process, one session: the table the `COPY`s read has to exist in the session they run in.

    `COPY (SELECT ... FROM range(0, n)) TO ...` would do it in one statement and no table, but the columns of a
    bare expression come out nullable, every page of every column then carries definition levels, and the same
    one-cell edit then reuses 57% of the file instead of 93%. Nullability is the biggest lever in this whole
    measurement (docs/CDC_PARQUET_PAGES.md), so it is stated here rather than left to inference.
    """
    run = subprocess.run([DUCKDB, "-c", "\n".join(statements)], capture_output=True, text=True)
    if run.returncode != 0:
        die(f"{DUCKDB} failed\n{(run.stderr or run.stdout).strip()}\n"
            f"CONTENT_DEFINED_CHUNKING only exists in the fork build (branch content-defined-pages): build it "
            f"and point DUCKDB at build/release/duckdb, or run with --no-cdc")


def copy(query: str, path: str, options: dict) -> str:
    extra = "".join(f", {name.upper()} {value}" for name, value in options.items())
    return f"COPY ({query}) TO '{path}' (FORMAT PARQUET{extra});"


def load(rows: int) -> list:
    """The one table. Ids even, so an inserted row can land mid-file on an odd id and still sort into place."""
    return ["CREATE OR REPLACE TABLE base(id BIGINT NOT NULL, author VARCHAR NOT NULL, payload VARCHAR NOT NULL);",
            "INSERT INTO base SELECT i * 2, concat('user-', lpad(CAST(i % 997 AS VARCHAR), 3, '0')), "
            "concat('record ', CAST(i AS VARCHAR), ' :: ', repeat('x', 48)) "
            f"FROM range(0, {rows}) t(i);"]


def edited(edit: str, rows: int) -> str:
    """The same rows, one dataset-shaped edit."""
    middle = (rows // 2) * 2
    if edit == "append":
        return f"SELECT * FROM base UNION ALL SELECT {rows * 2}, 'user-tail', " \
               "concat('record tail :: ', repeat('x', 48))"
    if edit == "update":
        return f"SELECT id, author, CASE WHEN id = {middle} THEN concat('record {middle} :: ', repeat('y', 48)) " \
               "ELSE payload END AS payload FROM base"
    return f"SELECT * FROM base WHERE id < {middle} UNION ALL SELECT {middle} + 1, 'user-new', " \
           "concat('record new :: ', repeat('x', 48)) UNION ALL SELECT * FROM base WHERE id >= " \
           f"{middle} ORDER BY id"


def held_share(store_root: str, v1: str, v2: str):
    """Share of v2 a store holding v1 already has, what it would add, its size, its chunks, and the row count
    DuckDB sees when it opens the file out of the store - that read-back is the check that what the store holds
    is the file the writer wrote, not something that happens to be the same size."""
    os.makedirs(store_root, exist_ok=True)
    fs = nano_xet.NXetFileSystem(store_uri=store_root)
    con = duckdb.connect()
    con.register_filesystem(fs)  # from here on, 'nxet://...' is a path DuckDB can open
    with open(v1, "rb") as handle:
        fs.pipe_file("v1.parquet", handle.read())
    before = fs.store.stats()
    with open(v2, "rb") as handle:
        fs.pipe_file("v2.parquet", handle.read())
    after = fs.store.stats()
    rows = con.execute("SELECT count(*) FROM read_parquet('nxet://v2.parquet')").fetchone()[0]
    # a store keeps every chunk once across all the files that reference it, so the difference between the two
    # stats is exactly what storing v2 on top of v1 cost it
    size = after.logical_bytes - before.logical_bytes
    held = after.dedup_bytes - before.dedup_bytes
    return held / size, size - held, size, after.chunks - before.chunks, rows


def main() -> None:
    ap = argparse.ArgumentParser(description="estimate the dedup ratio of an edited Parquet file")
    ap.add_argument("--rows", type=int, default=500_000)
    ap.add_argument("--edit", default=",".join(EDITS), help="comma list of append,update,insert")
    ap.add_argument("--no-cdc", action="store_true", help="skip CONTENT_DEFINED_CHUNKING")
    ap.add_argument("--cdc-min", type=int, default=262_144, help="min_chunk_size, plain bytes (Arrow's default)")
    ap.add_argument("--cdc-max", type=int, default=1_048_576, help="max_chunk_size, plain bytes")
    ap.add_argument("--size-limit", default="1048576",
                    help="DATA_PAGE_SIZE_LIMIT values, comma separated; 'default' for the writer's own, which "
                         "is one row-group-sized page and so a pessimistic baseline rather than a fair one")
    ap.add_argument("--row-group", type=int, help="ROW_GROUP_SIZE, also a dedup knob: pages cannot span groups")
    args = ap.parse_args()

    edits = [item.strip() for item in args.edit.split(",") if item.strip()]
    if not edits or any(item not in EDITS for item in edits):
        die(f"--edit must be some of {', '.join(EDITS)}")
    writers = {}
    for value in [item.strip() for item in args.size_limit.split(",") if item.strip()]:
        key = "pages by writer default" if value in ("default", "none") else f"DATA_PAGE_SIZE_LIMIT {value}"
        writers[key] = {} if value in ("default", "none") else {"data_page_size_limit": value}
    if not args.no_cdc:
        sizes = f"min_chunk_size: {args.cdc_min}, max_chunk_size: {args.cdc_max}"
        writers["CONTENT_DEFINED_CHUNKING"] = {"content_defined_chunking": "{" + sizes + "}"}
    if not writers:
        die("--size-limit is empty and --no-cdc asked for no content-defined row, which leaves no writer to run")
    for options in writers.values():
        if args.row_group:
            options["row_group_size"] = str(args.row_group)
    if not os.path.exists(DUCKDB):
        die(f"$DUCKDB points at {DUCKDB}, which is not a file")
    # 0.1.1 answered `modified` with an epoch float where fsspec promises a datetime, and DuckDB's bridge calls
    # .timestamp() on it - which surfaces as a throwback from inside read_parquet, so ask for the fix instead.
    if tuple(int(part) for part in nano_xet.__version__.split(".")[:3]) < (0, 1, 2):
        die(f"nano-xet {nano_xet.__version__} cannot serve reads to DuckDB; pip install -U 'nano-xet>=0.1.2'")

    with tempfile.TemporaryDirectory(prefix="dedup-estimate-") as work:
        print(f"rows={args.rows}   written by {DUCKDB}   stored as nxet:// and read back by duckdb "
              f"{duckdb.__version__}\n")
        print(f"{'writer':<30}{'v2':>8}  " + "  ".join(f"{edit:>20}" for edit in edits))
        sparse = False
        for name, options in writers.items():
            slug = name.split(" ")[0].lower()
            v1 = os.path.join(work, f"{slug}-v1.parquet")
            v2 = {edit: os.path.join(work, f"{slug}-{edit}.parquet") for edit in edits}
            run_duckdb(load(args.rows) + [copy("SELECT * FROM base", v1, options)]
                   + [copy(edited(edit, args.rows), v2[edit], options) for edit in edits])
            cells, size_mb = [], 0.0
            for edit in edits:
                expected = args.rows + (edit != "update")
                held, added, size, chunks, rows = held_share(os.path.join(work, f"{slug}-{edit}"), v1, v2[edit])
                if rows != expected:
                    die(f"the store's copy of the {edit} file has {rows} rows, not {expected}")
                sparse = sparse or chunks < 25
                size_mb = size / 1024 / 1024
                cells.append(f"{100 * held:>10.1f}% ({added / 1024:>6.0f}K/{chunks}ch)")
            print(f"{name:<30}{size_mb:>6.1f}MB  " + "  ".join(cells))
        print("\n% = the share of the edited file's bytes a store holding the original already has; the rest is "
              "what it would\ntake, in KiB and chunks. Under ~25 chunks a percentage is mostly granularity: "
              "raise --rows. Directional\nnumbers - nano-xet's chunker, not xet-core's; rows differ only in how "
              "the writer cuts pages.")


if __name__ == "__main__":
    main()
