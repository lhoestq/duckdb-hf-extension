#!/usr/bin/env python3
"""The dedup estimate with nothing between the writer and the store: DuckDB COPYs into `nxet://`.

Same question as `scripts/dedup_estimate.py` - what share of an edited Parquet file would a store holding the
original already hold - and no local file in the answer. Registering nano-xet's fsspec filesystem turns
`nxet://` into a path DuckDB can write to, so the patched writer's bytes go into the store as they are made,
the store's own numbers say what storing the second file cost it, and `read_parquet('nxet://v2.parquet')` reads
the file back out of the store, which is the check that what got measured is the file the writer wrote. No
subprocess, no temp directory, no upload step: the statements below are the whole measurement.

    DUCKDB_PY=/home/quentin/hf/duckdb-python/.venv/lib/python3.11/site-packages scripts/dedup_estimate_dev.py
    scripts/dedup_estimate_dev.py --rows 2000000 --edit insert --row-group 2000000
    scripts/dedup_estimate_dev.py --no-cdc                    # any duckdb, size-based cuts only
    scripts/dedup_estimate_dev.py --store file:///scratch/nx  # a store that outlives the run

It wants the dev build's python module, since `CONTENT_DEFINED_CHUNKING` is a change to the writer, and the
python client left the duckdb repo, so no fork checkout is pip-installable on its own. The module used here is
duckdb-python with its `external/duckdb` submodule holding a clone of the fork branch:

    git clone https://github.com/duckdb/duckdb-python && cd duckdb-python
    git clone /home/quentin/hf/duckdb external/duckdb          # a local clone, so a minute of copying at most
    git -C external/duckdb checkout content-defined-pages
    git config submodule.external/duckdb.url /home/quentin/hf/duckdb && git submodule init external/duckdb
    python3 -m venv .venv && source .venv/bin/activate         # needs python3-dev for Python.h
    pip install 'cmake>=3.29' 'ninja>=1.10' 'nanobind>=3.0' 'scikit-build-core>=0.11.4' 'numpy>=2.0'
    pip install . --no-build-isolation                         # four minutes on 20 cores: unity builds

`DUCKDB_PY` is then the directory holding the built `duckdb` package - that venv's `site-packages`. A CDC file
written through it is byte-identical to the same `COPY` written by the patched shell to a local file except for
the twelve bytes of `created_by`, so both entry points drive one writer and their rows are interchangeable.
`--no-cdc` runs the same code on any duckdb with the size-based rows only, which is how the machinery is
exercised without the build - and its rows are then not comparable with `scripts/dedup_estimate.py`'s, because a
released writer and a patched one do not write the same bytes.

The chunker is nano-xet's own - gear hash, 8 / 64 / 128 KiB - not xet-core's, so the percentages are
directional; a file wants a few dozen chunks before one means anything. `scripts/cdc_page_measure.py` is the
tool whose numbers `docs/CDC_PARQUET_PAGES.md` quotes, and `scripts/dedup_estimate.py` is the version that runs
against the patched shell binary on any machine with one.
"""

import argparse
import os
import sys

import nano_xet

EDITS = ("append", "update", "insert")
V1, V2 = "nxet://v1.parquet", "nxet://v2.parquet"
PROBE = ("COPY (SELECT 1 AS a) TO 'nxet://probe.parquet' (FORMAT PARQUET, "
         "CONTENT_DEFINED_CHUNKING {min_chunk_size: 65536, max_chunk_size: 262144});")


def die(message: str) -> None:
    sys.exit(f"dedup_estimate_dev: {message}")


def import_duckdb():
    """`import duckdb`, with $DUCKDB_PY in front of the released one if it is set.

    The dev module and the released nano-xet can share an interpreter - both are for this same python - so the
    one path entry is the only seam between them. Which build answered is printed in the header, because a
    version number is easier to read than a patch level.
    """
    path = os.environ.get("DUCKDB_PY")
    if path:
        if not os.path.isdir(path):
            die(f"$DUCKDB_PY is {path}, which is not a directory")
        sys.path.insert(0, path)
    try:
        import duckdb
    except ImportError as exc:
        die(f"nothing importable as duckdb ({exc}); set DUCKDB_PY at the dev build's package directory")
    return duckdb


def connect(duckdb, store_root: str):
    """A store and a connection that can see it. `register_filesystem` takes the fsspec filesystem itself - a
    `pyarrow.fs` wrapper round it gets refused as a "Bad filesystem instance" - and each pair gets its own
    store, so a measurement never inherits chunks from the one before it."""
    fs = nano_xet.NXetFileSystem(store_uri=store_root)
    con = duckdb.connect()
    con.register_filesystem(fs)
    return fs, con


def under(prefix: str, tail: str) -> str:
    """One directory per measurement, under whatever `--store` named. `memory://` is a URI with nothing after the
    scheme, and a slash there would move the path into a host, so it is the one prefix not to join with one."""
    return prefix + tail if prefix.endswith("://") else f"{prefix.rstrip('/')}/{tail}"


def load(rows: int) -> list:
    """The one table. Ids even, so an inserted row can land mid-file on an odd id and still sort into place,
    and the columns declared NOT NULL: an expression's nullability is a page's definition levels, which is the
    biggest single number in docs/CDC_PARQUET_PAGES.md."""
    return ["CREATE OR REPLACE TABLE base(id BIGINT NOT NULL, author VARCHAR NOT NULL, payload VARCHAR NOT NULL);",
            "INSERT INTO base SELECT i * 2, concat('user-', lpad(CAST(i % 997 AS VARCHAR), 3, '0')), "
            "concat('record ', CAST(i AS VARCHAR), ' :: ', repeat('x', 48)) "
            f"FROM range(0, {rows}) t(i);"]


def copy(query: str, path: str, options: dict) -> str:
    extra = "".join(f", {name.upper()} {value}" for name, value in options.items())
    return f"COPY ({query}) TO '{path}' (FORMAT PARQUET{extra});"


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


def estimate(duckdb, store_root: str, rows: int, edit: str, options: dict):
    """v1 into the store, then the edit, and the difference between what the store says before and after.

    A store keeps every chunk once across the files that reference it, so that difference is exactly the cost of
    the second file - and `before` has to be read after v1 is in, not after the store was opened.
    """
    fs, con = connect(duckdb, store_root)
    for statement in load(rows):
        con.execute(statement)
    con.execute(copy("SELECT * FROM base", V1, options))
    before = fs.store.stats()
    con.execute(copy(edited(edit, rows), V2, options))
    after = fs.store.stats()
    read = con.execute(f"SELECT count(*) FROM read_parquet('{V2}')").fetchone()[0]
    size = after.logical_bytes - before.logical_bytes
    if not size:
        die(f"the store already held every chunk of the {edit} file, so storing it cost it nothing - which is "
            f"what a --store left over from an earlier run looks like. memory:// is empty each process; give "
            f"--store an empty place")
    held = after.dedup_bytes - before.dedup_bytes
    return held / size, size - held, size, after.chunks - before.chunks, read


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", type=int, default=500_000, help="rows in the table (default 500000)")
    parser.add_argument("--edit", default=",".join(EDITS), help="comma list of append, update, insert")
    parser.add_argument("--no-cdc", action="store_true", help="skip CONTENT_DEFINED_CHUNKING; runs on any duckdb")
    parser.add_argument("--cdc-min", type=int, default=262_144, help="min_chunk_size in bytes (default 262144)")
    parser.add_argument("--cdc-max", type=int, default=1_048_576, help="max_chunk_size in bytes (default 1048576)")
    parser.add_argument("--size-limit", default="1048576",
                        help="comma list of DATA_PAGE_SIZE_LIMIT baselines; 'default' for the writer's own")
    parser.add_argument("--row-group", type=int, help="ROW_GROUP_SIZE to write with; pages cannot cross one")
    parser.add_argument("--store", default="memory://",
                        help="URI prefix for one store per measurement; empty each run, as memory:// is")
    args = parser.parse_args()

    duckdb = import_duckdb()
    if tuple(int(part) for part in nano_xet.__version__.split(".")[:3]) < (0, 1, 2):
        die(f"nano-xet {nano_xet.__version__} cannot serve reads to DuckDB; pip install -U 'nano-xet>=0.1.2'")

    edits = [edit.strip() for edit in args.edit.split(",") if edit.strip()]
    for edit in edits:
        if edit not in EDITS:
            die(f"unknown edit {edit!r}; there is append, update and insert")
    writers = {}
    for limit in [item.strip() for item in args.size_limit.split(",") if item.strip()]:
        if limit in ("default", "none"):
            writers["pages by writer default"] = {}
        else:
            writers[f"DATA_PAGE_SIZE_LIMIT {limit}"] = {"data_page_size_limit": limit}
    if not args.no_cdc:
        sizes = f"min_chunk_size: {args.cdc_min}, max_chunk_size: {args.cdc_max}"
        writers["CONTENT_DEFINED_CHUNKING"] = {"content_defined_chunking": "{" + sizes + "}"}
    if not writers:
        die("--size-limit is empty and --no-cdc asked for no content-defined row, which leaves no writer to run")
    for options in writers.values():
        if args.row_group:
            options["row_group_size"] = str(args.row_group)

    if not args.no_cdc:
        _, probee = connect(duckdb, under(args.store, "probe"))
        try:
            probee.execute(PROBE)
        except Exception as exc:
            die(f"this duckdb cannot write content-defined pages: {str(exc).splitlines()[0]}\n"
                f"{duckdb.__version__} from {os.path.dirname(duckdb.__file__)}\n"
                "CONTENT_DEFINED_CHUNKING is in the fork build (branch content-defined-pages); see the header "
                "for how to get a python module off it, or run --no-cdc")

    print(f"rows={args.rows}   duckdb {duckdb.__version__} in-process   stored as nxet://, "
          f"read back through it   chunked by nano-xet {nano_xet.__version__} (8 / 64 / 128 KiB)\n")
    print(f"{'writer':<30}{'v2':>8}  " + "  ".join(f"{edit:>20}" for edit in edits))
    sparse = False
    for name, options in writers.items():
        slug = name.split(" ")[0].lower()
        cells, size_mb = [], 0.0
        for edit in edits:
            expected = args.rows + (edit != "update")
            root = under(args.store, f"{slug}-{edit}")
            held, added, size, chunks, read = estimate(duckdb, root, args.rows, edit, options)
            if read != expected:
                die(f"the store's copy of the {edit} file has {read} rows, not {expected}")
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
