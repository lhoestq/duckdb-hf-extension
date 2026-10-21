#!/usr/bin/env python3
"""What content-defined *page* boundaries buy a Xet upload, measured offline.

`docs/XET.md` measures the upload side of this extension against a live repo: an identical re-upload costs
0 bytes, an appended tail costs 0.2 MB, a column added to every row costs everything. Those numbers are
about a byte-level content-defined chunker (`src/hf_xet_chunk.cpp`, Gear hash, 64 KiB target) finding the
bytes of a file it has seen before. This script asks the *writer's* half of the same question - given an
edit, how much of the new file's bytes are still the old file's bytes - because a chunker can only match
bytes the writer kept.

Apache Arrow's answer is `ContentDefinedChunker` (apache/arrow#45360 for C++, in 21.0.0; `parquet/src/column/
chunker/` in arrow-rs): cut a column's data pages at rolling-hash boundaries, so a page's row range comes from the
values instead of from a size threshold, and an edit changes one page instead of re-flowing every later page of
that column.

Method, per writer and per edit:

  1. write the same logical table twice - `v1`, and `v2` with one deliberate edit - with that writer's
     settings on *both* sides, so the comparison isolates the edit and the one writer flag;
  2. chunk both files with the *reference* Xet chunker (`xet-probe chunks`, xet-core's own, which is what
     `hf_xet_upload` reproduces). Reference rather than ours so a bug in our port cannot flatter a result;
  3. report how much of `v2`'s chunk bytes have a hash that also occurs in `v1`.

Shared bytes are what a dedup-enabled upload could point at instead of posting: the reference's upload loop
consults its local dedup table at every chunk (`xet_data/src/deduplication/file_deduplication.rs`), so once
a file's shard has been discovered matching resumes at the first identical chunk downstream of the damage.

Two things to hold in mind while reading the output. The numbers are a ceiling for the file pair, not a
quote for a live upload - a live run must also *find* the other file's shard, and `docs/XET.md` covers how
thin that index is (a file's first chunk, plus one hash in 1024). And the schema caveat below is not a
footnote: it moved these numbers more than the feature under test did.

**The schema caveat.** A Parquet column carries definition levels - bytes inside every data page - only
when the column is `optional` in the file schema. DuckDB marks a column optional when its output type is
nullable, and its output type depends on the *query shape*: a column reference keeps `NOT NULL`, a
`CASE`/arithmetic/`md5` expression does not, and a `UNION ALL` of the two yields nullable. So a query that
logically appends rows to a non-null column can turn every page of every column of the file into different
bytes without changing a single existing byte of data. `--by-schema` runs every edit three times - declared
`NOT NULL`, declared `NULL`, and as the query inferred it - and prints what each file's schema ended up being.
The two declared runs come out identical to each other, which is the point: optionality is cheap, *instability*
is what costs, and the as-inferred run is what a write from this extension actually gets.

    python scripts/cdc_page_measure.py [rows]        # 2M by default
    python scripts/cdc_page_measure.py 200000 --by-schema
    XET_PROBE=/path/to/xet-probe CDC_WORK=/tmp/cdc scripts/cdc_page_measure.py 500000
"""
import os
import subprocess
import sys
import tempfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

PROBE = os.environ.get("XET_PROBE", "tools/xet_probe/target/release/xet-probe")

# `duckdb` at its defaults is what every write this extension makes today. Its `data_page_size_limit`
# defaults to PrimitiveColumnWriter::MAX_UNCOMPRESSED_PAGE_SIZE = 100 MiB, which in practice means one
# page per row group, so the `-page` rows below are what happens as that limit comes down - the cheap
# half of the question, needing no content-defined chunking at all. The pyarrow entries are the reference
# implementation of content-defined pages, off and on: same writer, same data, one flag. `-64k` retunes
# arrow's CDC (256 KiB - 1 MiB by default) to sit near xet's 64 KiB chunk target.
WRITERS = [
    ("duckdb", "duckdb", {}),
    ("duckdb-page1m", "duckdb", {"data_page_size_limit": 1048576}),
    ("duckdb-page256k", "duckdb", {"data_page_size_limit": 262144}),
    ("duckdb-page64k", "duckdb", {"data_page_size_limit": 65536}),
    ("pyarrow", "pyarrow", {}),
    ("pyarrow-cdc", "pyarrow", dict(use_content_defined_chunking=True)),
    ("pyarrow-cdc-64k", "pyarrow", dict(use_content_defined_chunking={"min_chunk_size": 32768,
                                                                    "max_chunk_size": 262144})),
    # dictionary encoding is worth its own row: a dictionary page is one structure covering a whole row
    # group, so one new distinct value rewrites it and shifts every data page that follows
    ("pyarrow-cdc-64k-nodict", "pyarrow", dict(use_content_defined_chunking={
        "min_chunk_size": 32768, "max_chunk_size": 262144}, use_dictionary=False)),
]

# an append (the case byte-level CDC already handles well), one cell dead centre, one late row that sorts
# into the middle, one deleted row, one column added to every row
EDITS = ["append", "update", "insert", "delete", "widen"]

BY_SCHEMA = "--by-schema" in sys.argv
args = [a for a in sys.argv[1:] if not a.startswith("--")]

# What the file's nullability should be on both sides of an edit. This is less a control variable than a
# confound that has to be pinned down or it swamps the effect being measured: a Parquet column carries
# definition levels - bytes inside every data page - only when it is `optional`, so a column flipping
# between `required` and `optional` between two versions rewrites every page of that column without a
# single value changing. DuckDB chooses nullability from the query's expressions, so the `astyped` scheme
# is what a write from this extension actually gets.
SCHEMES = {"required": False, "optional": True, "astyped": None}

def die(message):
    sys.stderr.write(f"cdc_page_measure: {message}\n")
    sys.exit(1)


if not os.path.exists(PROBE):
    die(f"xet-probe not found at {PROBE}; `cargo build --release` in tools/xet_probe, or set XET_PROBE")

rows = int(args[0]) if args else 2_000_000
seed = os.environ.get("CDC_SEED", "17")
work = os.environ.get("CDC_WORK") or tempfile.mkdtemp(prefix="cdc_page_")
os.makedirs(work, exist_ok=True)
con = duckdb.connect()
mid = rows // 2 * 2  # an id that exists in base, so update/delete hit exactly one row


def sql(statement):
    return con.execute(statement).fetchall()


# The shapes a real dataset has: a sorted serial key with a hole at every odd id (so a backfilled row lands
# mid-file), two fixed-width columns, a low-cardinality column, and a high-entropy payload that resists
# compression the way an already-compressed or hex-encoded column does. High entropy is the honest worst
# case for page-level dedup: there is no slack for a shifted boundary to hide in.
con.execute(f"""
CREATE TABLE base AS
SELECT (i * 2)::BIGINT AS id,
       hash(i)::DOUBLE / 1e18 AS x,
       (i % 997)::INTEGER AS kind,
       md5(hash(i + {seed})::VARCHAR) || md5(hash(i + 7919 + {seed})::VARCHAR) AS payload
FROM range(1, {rows + 1}) t(i)
""")

VARIANTS = {
    "append": f"""SELECT * FROM base
                  UNION ALL
                  SELECT (i * 2 + 2 * {rows})::BIGINT, hash(i + 424242)::DOUBLE / 1e18,
                         (i % 997)::INTEGER,
                         md5(hash(i + 31 + {seed})::VARCHAR) || md5(hash(i + 8191 + {seed})::VARCHAR)
                  FROM range(1, {max(rows // 100, 100)} + 1) t(i) ORDER BY id""",
    "update": f"""SELECT * REPLACE
                    (CASE WHEN id = {mid} THEN concat('zz', payload[4:32]) ELSE payload END AS payload)
                  FROM base ORDER BY id""",
    "insert": f"""SELECT * FROM base
                  UNION ALL
                  SELECT {mid + 1}::BIGINT, 0.5, 42, concat('ff', {mid}::VARCHAR, 'ee')
                  ORDER BY id""",
    "delete": f"""SELECT * FROM base WHERE id <> {mid} ORDER BY id""",
    "widen": f"""SELECT *, md5(hash(id + 6151 + {seed})::VARCHAR) AS extra
                 FROM base ORDER BY id""",
}


def declared_ddl(edit, nullable):
    """A declared schema, so the file's nullability comes from the dataset and not from query inference.

    Only worth building for `NOT NULL`: declaring a column `NULL` does not survive to the file, because the
    writer's nullability comes from the projection's inferred type and `SELECT * FROM t` over a nullable
    column of a table whose rows are all non-null is not how DuckDB decides it. The `optional` scheme below
    therefore asks for optionality in the projection itself.
    """
    types = [("id", "BIGINT"), ("x", "DOUBLE"), ("kind", "INTEGER"), ("payload", "VARCHAR")]
    if edit == "widen":
        types.append(("extra", "VARCHAR"))
    suffix = "" if nullable is None else (" NULL" if nullable else " NOT NULL")
    return "(" + ", ".join(f"{name} {sql_type}{suffix}" for name, sql_type in types) + ")"


def optional_projection(query, edit):
    """Same rows, same values, `optional` in the file.

    `CASE WHEN col IS NOT NULL THEN col END` stays nullable through binding and is value-preserving for any
    value - a `NULLIF(col, sentinel)` needs a sentinel that cannot occur in the data, and `CASE WHEN TRUE
    THEN col END` is constant-folded back to `required`.
    """
    names = ["id", "x", "kind", "payload"] + (["extra"] if edit == "widen" else [])
    projection = ", ".join(f"CASE WHEN {c} IS NOT NULL THEN {c} END AS {c}" for c in names)
    return f"SELECT {projection} FROM ({query})"


def materialise(query, edit, scheme, path, options=None):
    """One version of the table as a file, under one schema scheme.

    `astyped` writes the query straight out, which is how this extension writes: the file's nullability is
    then whatever DuckDB inferred from the expressions. The other two ask for it explicitly.
    """
    if scheme == "astyped":
        write_duckdb(options or {}, f"({query})", path, select=True)
        return path
    if scheme == "optional":
        write_duckdb(options or {}, f"({optional_projection(query, edit)})", path, select=True)
        return path
    con.execute("DROP TABLE IF EXISTS m_v")
    con.execute(f"CREATE TABLE m_v {declared_ddl(edit, False)}")
    con.execute(f"INSERT INTO m_v {query}")
    write_duckdb(options or {}, "m_v", path)
    return path


def arrow_version(path, scheme):
    """The same rows as an Arrow table, with the scheme's nullability.

    pyarrow decides whether to write definition levels from the Arrow field's nullability, so this has to
    be set explicitly for the comparison to hold nullability constant across a writer's v1 and v2.
    """
    table = pq.read_table(path)
    nullable = SCHEMES[scheme]
    if nullable is None:
        return table
    schema = pa.schema([pa.field(f.name, f.type, nullable=nullable) for f in table.schema])
    return table.cast(schema)


def write_parquet(options, table, path):
    pq.write_table(table, path, **(options or {}))


def write_duckdb(options, source, path, select=False):
    query = f"COPY ({source if select else f'SELECT * FROM {source}'}) TO '{path}' (FORMAT PARQUET"
    for key, value in options.items():
        query += f", {key.upper()} {value}"
    sql(query + ")")


CACHE = {}


def chunks_of(path):
    """(hash, size) per chunk, as xet-core's own chunker and hasher see this file's bytes."""
    if path in CACHE:
        return CACHE[path]
    with open(path, "rb") as handle:
        out = subprocess.run([PROBE, "chunks"], stdin=handle, capture_output=True, text=True)
    if out.returncode != 0:
        die(f"{PROBE} chunks failed on {path}: {out.stderr.strip()}")
    parsed = []
    for line in out.stdout.splitlines():
        _index, size, digest = line.split()
        parsed.append((digest, int(size)))
    if not parsed:
        die(f"the reference chunker returned no chunks for {path}")
    CACHE[path] = parsed
    return parsed


def overlap(held, seen):
    """How much of `seen` something holding `held` could point at: multiset intersection by chunk hash."""
    pool = {}
    for digest, size in held:
        entry = pool.setdefault(digest, [0, 0])
        entry[0] += 1
        entry[1] += size
    shared_chunks = shared_bytes = 0
    for digest, size in seen:
        entry = pool.get(digest)
        if entry and entry[0] > 0:
            entry[0] -= 1
            shared_chunks += 1
            shared_bytes += size
    return shared_chunks, shared_bytes, sum(size for _d, size in seen)


def mb(size):
    return f"{size / 1024 / 1024:.1f} MB"


def nullability(path):
    schema = pq.read_schema(path)
    return "required" if all(not field.nullable for field in schema) else \
        "+".join(field.name for field in schema if field.nullable)


print(f"rows={rows} mid={mid} seed={seed} work={work}")
print(f"chunker: {PROBE} chunks (xet-core's Gear chunker, 64 KiB target)\n")

header = (f"{'writer':<22} {'schema':<8} {'edit':<8} {'v1':>9} {'v2':>9} {'chunks':>7} "
          f"{'shared':>7} {'shared bytes':>13} {'of v2':>7}")
print(header)
print("-" * len(header))

schemes = list(SCHEMES) if BY_SCHEMA else ["required"]
for scheme in schemes:
    base_file = materialise("SELECT * FROM base ORDER BY id", "base", scheme, f"{work}/base_{scheme}.parquet")
    base_arrow = arrow_version(base_file, scheme)

    for name, kind, options in WRITERS:
        v1 = f"{work}/{name}_{scheme}_v1.parquet"
        if not os.path.exists(v1):
            if kind == "duckdb":
                materialise("SELECT * FROM base ORDER BY id", "base", scheme, v1, options)
            else:
                write_parquet(options, base_arrow, v1)
        for edit in EDITS:
            v2 = f"{work}/{name}_{scheme}_{edit}.parquet"
            if not os.path.exists(v2):
                if kind == "duckdb":
                    materialise(VARIANTS[edit], edit, scheme, v2, options)
                else:
                    source = materialise(VARIANTS[edit], edit, scheme, f"{work}/src_{scheme}_{edit}.parquet")
                    write_parquet(options, arrow_version(source, scheme), v2)
            shared, shared_bytes, total = overlap(chunks_of(v1), chunks_of(v2))
            print(f"{name:<22} {scheme:<8} {edit:<8} {mb(os.path.getsize(v1)):>9} {mb(os.path.getsize(v2)):>9} "
                  f"{len(chunks_of(v2)):>7} {shared:>7} {mb(shared_bytes):>13} {100.0 * shared_bytes / total:>6.1f}%")
    print()

print("nullability each scheme's file actually has, and what the bare query inferred:")
for edit in EDITS + ["base"]:
    query = "SELECT * FROM base ORDER BY id" if edit == "base" else VARIANTS[edit]
    as_file = materialise(query, edit, "astyped", f"{work}/probe_{edit}.parquet")
    declared = {s: nullability(materialise(query, edit, s, f"{work}/probe_{s}_{edit}.parquet"))
                for s in ("required", "optional")}
    print(f"  {edit:<8} as-typed: {nullability(as_file):<28} required: {declared['required']:<12} "
          f"optional: {declared['optional']}")
print(f"\nfiles are in {work}; nothing here touched the CAS")
