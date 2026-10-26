# Content-defined Parquet pages — measured; the page cut is the third-largest source of rewritten bytes

**Verdict, in three parts.** (1) Content-defined pages are worth a lot on the edits that shift rows: at 2M rows /
150 MB, a mid-file row insertion leaves 50.7% of a DuckDB-written file's bytes reusable and 95.0% of a
content-defined-pages file's bytes reusable — a rewrite that uploads ~73 MB becomes ~7.5 MB. (2) Nothing in this
extension can produce them today. Page boundaries are decided by DuckDB's Parquet writer, in one predicate, and the
upload side cannot recover bytes the writer already re-flowed. It is a writer change, which makes it an upstream
DuckDB change, carried locally meanwhile (the `Makefile` builds against `../vendor/duckdb`).
(3) Even with it, on this
extension's own write path a *schema* question costs more than the page cut does: when a rewrite changes a
column's nullability, 2.7% of the new file's bytes are reusable and content-defined pages buy ~nothing. That part
is fixable here, and cheaply.

**Followed up, same month:** (2) has been acted on. The writer patch exists — `/home/quentin/hf/duckdb`, branch
`content-defined-pages`, `COPY ... (FORMAT PARQUET, CONTENT_DEFINED_CHUNKING true)` — and its chunker is now a port
of Arrow's rather than a DuckDB-shaped guess, checked page for page against pyarrow (`scripts/cdc_conformance.py`,
*It cuts where Arrow cuts*). What it is worth on a DuckDB-written file: on a mid-file row shift, 89.8% of the new
file's bytes are reusable against 50.7% today — 94.2% with the sizes tuned, against pyarrow's 95.0% for the same
edits — and on a single cell changed deep in the file, 99.6% of that column's page bytes survive against 93.9%.
"It is built now" below is what the port keeps, what it cannot keep, and what the design above did not predict.

Measured 2026-10. Tooling added with this document: `scripts/cdc_page_measure.py` (the grid),
`scripts/parquet_pages.py` (a page-header walker with a `--compare` mode that hashes page bodies),
`scripts/parquet_page_values.py` (per-page *value* counts, the only page property two writers can be compared on),
`scripts/cdc_conformance.py` (the arrow-cpp boundary check, with its answers recorded in `scripts/fixtures/`), and
`scripts/dedup_estimate.py` (the same question in one screenful, over a table you make, with nano-xet as the store).

## What cuts a page today

Parquet's unit of rewriting is the page: a run of one column's values inside one row group, separately compressed,
with a header carrying its own statistics. A file is footer, then row groups of column chunks, each a dictionary
page (optional) followed by data pages. Pages are concatenated, so a page's *file offset* is not stable across
versions — but a page whose *body* is unchanged is unchanged for dedup purposes, because the byte chunker that
follows does not care where the page starts.

DuckDB cuts a page on accumulated estimated size, in exactly one place —
`extension/parquet/writer/primitive_column_writer.cpp:118`, inside `PrimitiveColumnWriter::Prepare`:

```cpp
const idx_t page_size_limit = writer.DataPageSizeLimit();
...
    page_info.estimated_page_size += GetRowSize(vector, vector_index, state);
    if (page_info.estimated_page_size >= page_size_limit && (vector_can_span_multiple_pages || i == 0)) {
        PageInformation new_info;
        ...
```

and `data_page_size_limit` defaults to `PrimitiveColumnWriter::MAX_UNCOMPRESSED_PAGE_SIZE` = **100 MiB**
(`extension/parquet/parquet_extension.cpp:110`). Combined with the
default row group size (`DEFAULT_ROW_GROUP_SIZE`, 122 880 rows), that means a DuckDB-written file has **one data
page per column per row group** for everything but huge string columns — measured on a 2M-row file: 17 pages for a
125 MB `payload` column, 7.6 MB per page. Arrow's default is 1 MiB (`parquet::DEFAULT_DATA_PAGE_SIZE`), i.e. ~125
pages for the same column.

`Prepare` decides page *membership* only; bytes are encoded later, in `BeginWrite`/`PrepareWrite`, and the
dictionary page is materialised last and inserted at the head of the column's page list (`WriteDictionary`, same
file). Worth remembering: DuckDB's cut decision is made over *logical* values, with no encoder feedback. Arrow's
is not (below), and that difference shows up in the numbers.

## What Arrow's content-defined chunking does

`ContentDefinedChunker` (C++ PR #45360, issue #45750, milestone 21.0.0, merged 2025-05-13 — `enable_content_defined_chunking()`
on the writer builder, options in `CdcOptions`; Rust `parquet/src/column/chunker/`, `CdcOptions`, merged later;
pyarrow exposes it as `use_content_defined_chunking=True|{min_chunk_size, max_chunk_size, norm_level}`, and
Hugging Face wrote the blog for it, `huggingface.co/blog/parquet-cdc`, selling it precisely as dedup for the Xet
cas — so a DuckDB-side equivalent should borrow Arrow's names rather than invent a dialect). A Gear-hash rolling hash is
advanced over each column's `(definition level, repetition level, value)` triplets — over *values*, not over
serialized page bytes, though per byte within each value — and a page is cut where the hash hits the boundary
predicate, subject to
`min_chunk_size` (256 KiB default) and `max_chunk_size` (1 MiB default). The rolling state **persists across pages
and row groups**, one chunker per leaf column.

The reason this survives an insertion is that the boundary depends on content, not on accumulated size. Cut
positions after the edit shift by exactly the amount the data shifted, so a page downstream of the edit covers
precisely the same sequence of values it covered before and comes out **byte-identical**. A size-based cut does
the opposite: every later value moves by one position, so every later cut lands on a different row, so every
later page differs.

Two constraints from Arrow that a DuckDB version inherits. Nested/repeated data can only be cut at record
boundaries (`rep_level == 0`), which is why the chunker hashes levels alongside values. And the dictionary
defeats it partially: in Arrow, dictionary pressure forces page flushes of its own, so boundaries stop being
content-defined wherever the dictionary is the limiter — its own test matrix runs every CDC case with the
dictionary on and off.

## The measurement

`scripts/cdc_page_measure.py` builds one base dataset in DuckDB (four columns, `id` sorted, one 64-char
`payload` — an approximation of a text column, which is where the bytes are), derives five edited versions of it,
writes each version with each writer, chunks both files with **xet-core's own chunker and hasher** and intersects
the chunk hashes as multisets. The chunker is `tools/xet_probe` built from this checkout, so the numbers are what
the real uploader would find, not what a port of it finds. Shared bytes are an *upper bound on the upload
savings*, offline: nothing here touches the CAS, and real savings additionally need the server's global dedup
query to hand back the shards that hold the matching content (`docs/XET.md`, M9).

Edits are the ones a dataset actually sees: `append` (tail), `update` (one cell deep in the file), `insert` (one
row at the middle, ids re-sorted), `delete` (that row again), `widen` (a column added). Writers are DuckDB at four
page-size settings, pyarrow at its default fixed 1 MiB pages, and pyarrow with CDC at default and small bounds,
with and without the dictionary, and since this document was first written the same two CDC tunings on the patched
DuckDB writer. Percentages below are shared bytes as a fraction of the *new* file, i.e. what a dedup-aware uploader
would not have to send.

2M rows (~150 MB per file), schema declared `NOT NULL` on both sides. `limit` is `data_page_size_limit` (size-based
cuts); the last two columns are the patched writer's `CONTENT_DEFINED_CHUNKING` at Arrow's default sizes and at the
small ones, from the same materialised sources as the plain `duckdb` column:

| edit | duckdb | limit 1 MiB | limit 256 KiB | pyarrow fixed | pyarrow CDC | CDC 64 KiB | … no dict | **duckdb CDC** | **CDC 64 KiB** |
|---|---|---|---|---|---|---|---|---|---|
| append | 98.7% | 98.5% | 98.7% | 98.6% | 98.5% | 98.6% | 98.4% | 98.7% | 98.7% |
| update | 95.1% | 99.2% | 99.7% | 99.2% | 99.7% | 99.8% | 99.8% | **99.7%** | **99.8%** |
| insert | **50.7%** | 54.2% | 50.7% | 50.9% | 94.6% | **95.0%** | **95.9%** | **89.8%** | **94.2%** |
| delete | **50.8%** | 50.7% | 50.8% | 50.8% | 94.5% | **94.9%** | **95.9%** | **89.8%** | **94.2%** |
| widen | 66.7% | 66.7% | 66.6% | 70.6% | 70.7% | 70.7% | 69.2% | 66.7% | 66.7% |

DuckDB's CDC column is Arrow's within a point at the same tuning (`64 KiB`: 94.2 against 95.0 on a row shift, where
DuckDB without CDC is 50.7), and 5 points behind at the *default* sizes. That gap is row groups, not the chunker —
same file, same edit, same sizes, only `ROW_GROUP_SIZE` moving, insert at 2M rows:

| `ROW_GROUP_SIZE` | 122880 (DuckDB's default) | 1048576 (pyarrow's) | one row group |
|---|---|---|---|
| shared bytes of v2 | 89.8% | 95.7% | 96.4% |

Most of it is that a page cannot span row groups, so a small row group forces a page cut every 122880 rows whatever
the hash says; the state reset discussed below accounts for the rest, about 0.7 points once the row groups are the
same size. Read the other way: `ROW_GROUP_SIZE` is a dedup knob, and the writer default is a pessimistic one.

At 200k rows the shape is the same and the effects are harsher (the row-shift cases bottom out at 48–50%, and CDC
reaches 83–93% — pyarrow 85.1/89.5/92.9 at the three tunings, DuckDB 83.1/92.4), which is the right direction: the
*fraction* a shift damages is independent of file size, so the absolute saving grows with the file. Cost of
content-defined pages here is +0.8% file size (150.2 → 151.4 MB) and 10× the page count, which is a read-path cost
too: more headers, more page-lookup work, slightly worse compression per page.

`scripts/parquet_pages.py --compare` shows the mechanism directly — the same file pair, counting pages whose
*bodies* hash equal (payload column, 2M rows):

| writer | pages, v1 | identical after `update` | identical after `insert` |
|---|---|---|---|
| duckdb default (pages = row groups) | 17 | 16 (94%) | 8 (47%) |
| duckdb, 1 MiB pages | 131 | 130 (99%) | 70 (53%) |
| pyarrow fixed 1 MiB | 125 | 124 (99%) | 63 (50%) |
| pyarrow CDC 64 KiB | 1310 | 1309 (99.9%) | **1297 (99.0%)** |

Two readings. A *value-preserving* edit (`update`: one cell, same length) barely hurts size-based cuts at 1 MiB —
the deficit is a few dozen bytes in a megabyte of page, so the cut lands on the same row. What looks like a win for
small pages is narrower than it feels: at 64 KiB pages DuckDB's `update` drops back to 95.3% at 2M rows (99.2% at
1 MiB), because a shortened value shifts a page boundary across a row and then *every* subsequent fixed cut is
off by one. Content-defined cuts have no such trade-off — they are the only policy that survives `insert` at all.

## Three things that dominate the page cut

**1. A nullability flip rewrites every page of the column, and no page policy saves you.** A Parquet column carries
definition levels — bytes inside every data page — only when it is `optional`. Flip a column from `required` to
`optional` between two versions and the pages change without a single value changing. Same data, same edit, same
writer, only whether the schema was stable differing (200k rows, `--by-schema`; the `optional` column is the same
rows through a projection that asks for optionality, because declaring a column `NULL` on the table does not
survive to the file):

| edit | both `required` | both `optional` | as the queries typed it | what flipped |
|---|---|---|---|---|
| append | 97.2% | 97.5% | **2.7%** | all four columns → `optional` |
| update | 87.1% | 86.9% | **14.5%** | `payload` → `optional` |
| insert | 48.7% | 48.6% | 48.7% | nothing |
| delete | 49.9% | 49.3% | 49.9% | nothing |
| widen | 69.2% | 68.9% | 69.2% | only the new column |

Three things to take from this. It is *symmetric*: a schema that is `optional` on both sides costs within a point of
one that is `required` on both, so the enemy is instability, not optionality. Content-defined pages do not rescue a
flip — pyarrow CDC under the as-typed schema gets 13.7% on `append` versus 94.9% with it stable, because the level
bytes inside every page changed. And **this is the normal case on this extension's write path**: nullability
comes from DuckDB's inference over the query's expressions, so `COPY (SELECT ... UNION ALL SELECT <computed
columns>) TO 'hf://...'` yields `optional` columns where `COPY (SELECT * FROM t) TO 'hf://...'` yields `required`.
Two different queries over one logical table produce two incompatible files.

The fix is on this side of the boundary and costs almost nothing: normalise every column to `optional` at write
time. Values are untouched; measured cost is 0.1% of file size. DuckDB has no type-level syntax for it
(`CAST(x AS BIGINT NULL)` is a parse error) and no write option, but two expressions do it and survive binding:
`CASE WHEN col IS NOT NULL THEN col END` (what `optional_projection` in the script uses, because it preserves every
value) and `NULLIF(col, <a sentinel that cannot occur in the data>)`. `CASE WHEN TRUE THEN col END` does not work —
it is constant-folded and stays `required`. Wrapping the copy's select list this way would make every file this
extension writes carry one stable shape, whatever the user's query looked like.

**2. A dictionary page is one global structure per column chunk.** With dictionary encoding, all distinct values of
a column chunk live in one page at the head of the chunk, and the data pages hold indices into it. One new distinct
value anywhere in the chunk rewrites that page, and the fallback to PLAIN mid-chunk (past Arrow's 1 MiB dictionary
limit) is positioned by dictionary growth, not by content. Turning the dictionary off moved `insert` from 89.5% to
92.9% at 200k rows, and the page-size distribution shows why: with the dictionary on, CDC'd `payload` pages bottom
out at 1 905 bytes against 58 235 bytes with it off — the dictionary is forcing cuts of its own, at positions no
longer chosen by the hash. DuckDB is less exposed here than Arrow: it materialises the dictionary after page
membership is fixed (`WriteDictionary` inserts it at the head at flush time), so a cut chosen in `Prepare` is not
disturbed by dictionary pressure.

**3. Page-boundary policy**, the subject of everything above, worth roughly the 44 points between 50.7% and 95.0%
on a row shift. Note that it ranks *third*: fix the schema, then the page cut, then worry about dictionaries.

Also corrected here, against `docs/XET.md`: `widen` is not the disaster a CSV rewrite is. 66–71% of the new file is
reusable because Parquet keeps the old columns' chunks byte-identical and adds a new column chunk; only the new
column, the footer and the statistics are genuinely new. The `0 of 6889 chunks reused` figure in `docs/XET.md` is
measured on a CSV-shaped rewrite, where an added column shifts every downstream byte, and it does not carry over to
Parquet. (I did not re-run that experiment; I measured the Parquet case.)

## Where this could be implemented

*Written before the patch existed, kept because the reasoning is what produced the shape of it. Two of its calls
 did not survive: the state cannot live on the writer (see* What the chunker is*), and matching Arrow's hash turned
 out to be worth having (below).*

**Not on the upload side.** `src/hf_xet_chunk.cpp` already cuts the serialized file with a Gear hash at a 64 KiB
target, and its boundaries are content-defined in exactly the sense Arrow's are. But chunk boundaries only decide
what a *changed byte* costs; they cannot make a byte that DuckDB re-wrote stay the same. Page stability is a
property of the writer. Aligning upload chunk boundaries to page boundaries is also not the lever it looks like:
with 64 KiB chunks and ~100 KB–1 MB pages, a damaged page spoils the chunks that overlap it, which is the same
order of magnitude as the page, so alignment recovers at most the couple of chunks straddling each page edge. That
is the whole size of the prize there — the gap between 99.0% of pages identical and 95.0% of bytes shared in the
table above — and unlike the 44 points above, it does not grow with the file. The interop cost, though, is
permanent: chunk hashes no other Xet client can reproduce (`docs/XET.md`).

**Today, without any DuckDB change:** `COPY ... (FORMAT PARQUET, DATA_PAGE_SIZE_LIMIT 1048576)` is a real knob and
takes the `update` case from 95.1% to 99.2%. It does nothing for `insert`/`delete` (50.7% → 54.2% / 50.7%) and
below ~256 KiB it starts costing you the cascade described above. So it is a tuning knob, not the feature.

**The feature is a patch to DuckDB's Parquet writer**, in `PrimitiveColumnWriter::Prepare`: a per-column rolling
hash advanced per row next to `estimated_page_size`, cutting when the predicate fires between `min` and `max`
instead of when the size limit is crossed, with the state living in `PrimitiveColumnWriterState` and surviving row
groups. Favourable, from reading the code: the cut decision is one predicate shared by every primitive writer
(strings included, via `templated_column_writer.hpp`); it runs over logical values before encoding; there is no
dictionary feedback (above); and the byte source is at hand in `Prepare` (`GetRowSize` and the vector). Hard parts:
`list`/`struct` writers call `Prepare` with `vector_can_span_multiple_pages = false` and own the level bookkeeping
(`is_empty`, `HandleRepeatLevels`), so record-boundary cuts need work there and the honest first version is
flat columns only; and a `COPY`-level option (`content_defined_chunking`, plus the three Arrow parameters, with
the same names so the datasets ecosystem converges) has to be threaded through `parquet_extension.cpp`.

**Matching Arrow's hash was not required — and is done anyway.** The argument below was written for a fork with its
own chunker, and it held on its own terms: page boundaries only have to be stable *within a writer*, and the hash
that must stay byte-exact for cross-client dedup is the chunk hash in `src/hf_xet_chunk.cpp`, which this does not
touch. What it missed is that a user tuning a Parquet writer tunes *parameters*, and a parameter that means
different things in two writers — `min_chunk_size` meaning "floor and target" in one and "floor, with the target
halfway to `max`" in the other — is not tunable across them. Converging on Arrow's arithmetic makes the three
numbers mean one thing in DuckDB, arrow-cpp and arrow-rs, which is what *It cuts where Arrow cuts* then checks.
The hash tables being Arrow's generated ones is along for the ride; what matters is the size arithmetic.

*Superseded, for the record:* the case against it was that Arrow's tables are its own generated Gear tables keyed
to value hashing, and reimplementing them buys nothing but a shared boundary. What it buys is a shared meaning for
the knobs, plus a fixed point to test against.

## It is built now, in a DuckDB fork

`/home/quentin/hf/duckdb`, branch `content-defined-pages` (local, not pushed). The option is

```sql
COPY t TO 'f.parquet' (FORMAT PARQUET, CONTENT_DEFINED_CHUNKING true);
COPY t TO 'f.parquet' (FORMAT PARQUET,
     CONTENT_DEFINED_CHUNKING {min_chunk_size: 262144, max_chunk_size: 1048576, norm_level: 0});
```

a `WRITE_ONLY` copy option of type `ANY`, so it takes a boolean or a struct of optional fields, and inside the
struct literal the separators are `:` (a `:=` is a table-function named argument, not a struct field). It is a copy
option and not a setting precisely because a `COPY` gets prepared, stored and logged: a session setting would
silently change what an already-written script produces, while the option travels with the statement (there is a
test that prepares a `COPY`, executes it, and requires the file to be byte-identical to the same `COPY` run
directly). The three fields are Arrow's, with Arrow's defaults — 256 KiB / 1 MiB / 0 — and an unknown field is an
error rather than a silently ignored one; `false` writes bytes identical to omitting the option.

Write the sizes as plain numbers if anything has to match another writer. `min_chunk_size: '1MB'` is a megabyte to
DuckDB and 1,000,000 < 1048576 to Arrow, because DuckDB's size parser — the one shared with `SET memory_limit` —
reads `KB`/`MB`/`GB` as powers of 1000 and `KiB`/`MiB`/`GiB` as powers of 1024. A 4.6% difference in the ceiling
moves every boundary after the first page, which is exactly the thing this option is for; a bare number is a byte
count on both sides, so it is the form the conformance harness uses and now the form the parser documents.

Nothing is paid for when the option is off. The loop that decides page cuts gained two predictable branches, and
12 write shapes repeated 8 times each — 250 MB of uncompressed strings, fixed widths with a nullable column,
snappy, a LIST column — time the same out of the patched and the unpatched binary, inside the run-to-run noise of
either (a LIST column: 0.025 s and 0.026 s, which way round depends on the run). That measurement is the one a
reviewer asks for, so it is in the PR text too.

### What the chunker is

`extension/parquet/include/writer/parquet_cdc_chunker.hpp` is a port of `parquet::internal::ContentDefinedChunker`
kept close on purpose: the same 64-bit gear hash over the same eight tables — `parquet_cdc_gear_tables.hpp` is
byte-identical to the values Arrow checks in as `cpp/src/parquet/chunker_internal_generated.h` (produced by
`chunker_internal_codegen.py`, and the same numbers arrow-rs has in
`parquet/src/column/chunker/cdc_generated.rs`) — the same mask arithmetic (`target = ((min + max) / 2 - min) / 8`, the top
`floor(log2(target)) - norm_level` bits of 64, `norm_level` subtracted), the same gate that leaves the hash alone
until `min_chunk_size` bytes have been counted, and the same eight-consecutive-hits rule (`nth_run` over eight
gear tables) that turns a hit into a boundary. A `static_assert` pins the number Arrow's own unit test pins: its
default sizes imply a 15-bit mask. The table is checked into the fork with no generator beside it: the recipe, and
the four known-first entries that tell a right regeneration from a wrong one, are stated in its own header comment,
and the script that runs it lives here with the rest of the tooling
(`scripts/generate_cdc_gear_tables.py`). The four earlier notes in this document about "the fork's own scheme" — a
32-bit avalanched hash, one table, a per-value probability, `min` as target, no `norm_level` — described the first
version of this file and are now wrong: they were the port's guess at the algorithm, and the algorithm turned out
to be reachable, so it is used instead.

It is fed the same bytes: a value's own bytes in machine order — strings with no length prefix — and, for a
nullable column, the two bytes of its definition level before them, so a null is content there and here. Which
types can take part is a decision about bytes, not a TODO: DECIMAL, UUID, INTERVAL, TIME, ENUM, HUGEINT and BIT
keep size-based cuts because DuckDB's bytes for them are not the bytes an Arrow writer would hash (`DECIMAL(9,2)`
is four bytes here and a decimal128 there, an ENUM hashes its DuckDB-assigned dictionary code, and so on), and
list and struct leaves keep them because Arrow cuts those at repetition level 0 rather than per value.

Two departures are left, and both are structural rather than arithmetic.

**State lifetime.** Arrow holds one chunker per leaf column for the whole file and says so in a comment ("not
reset between pages and row groups"). DuckDB's lives in `PrimitiveColumnWriterState` and starts over at each row
group, because `ParquetWriter::Flush` calls `Prepare` with no lock held and several row groups are flushed
concurrently against one shared `ColumnWriter` — a chunker on the writer is a data race, and it showed up first as
page counts that changed from run to run rather than as a crash. So boundaries agree with arrow-cpp inside the
first row group and are, after that, a DuckDB run of the same algorithm with the same statistics. The cost is
measured above: 89.8% against 95.7% of a shifted file's bytes reusable at the same chunk sizes, by way of
`ROW_GROUP_SIZE`.

**What closes a page besides a boundary.** Arrow's chunker answers "where does this chunk end" and its writer then
closes pages for reasons of its own — `max_rows_per_page` (20,000 by default) and `data_page_size` (1 MiB of
*encoded* bytes, which for a string includes its 4-byte length prefix). DuckDB has one equivalent knob,
`data_page_size_limit`, on 100 MiB by default and measured on its own estimate of the page's bytes. That is not
the chunker disagreeing; it is two writers with different page policies sitting on top of the same boundaries, and
the conformance harness reports it as such.

What the built thing is worth, measured with the reference chunker (`tools/xet_probe chunks`) over finished files —
2M rows, writer defaults on both sides (so 17 row groups), one 128-byte string changed in the middle, `payload`
column:

| | pages of the changed column that survive | those pages' bytes | file chunks the store already has | chunk bytes |
|---|---|---|---|---|
| pages cut by size (DuckDB today) | 16 of 17 | 93.9% | 2192 of 2302 (95.2%) | 95.1% |
| pages cut by content | 253 of 254 | 99.6% | 2328 of 2335 (99.7%) | 99.7% |

An append is 98.7% reusable either way (the grid above), size-based pages or not: **CDC pages do nothing for an
append**, which the byte chunker at upload time already covers almost completely, and which is why the mid-file
edit is the case to argue from. Everything else the earlier measurement said still holds: this is a flat-column
feature (a list or struct leaf keeps size-based cuts, since cutting there means cutting at repetition level 0), and
the file is ordinary Parquet — pyarrow and stock DuckDB 2.0 read these files, and `hash()` of the file and of a
re-write of it both agree.

Page count is not free in file size, but it is cheap and the sign is not fixed: on the single 128-byte-string
column above, cutting into ~512 KiB pages made the file 3.6% *smaller* (21.6 MB against 22.5), because each page is
compressed on its own and small pages happen to suit this data; on a mixed-type table with dictionaries and
megabyte strings the same setting cost 3.7%, and `min_chunk_size: 4096` cost 15.2%. So the floor in the option is
not just a dedup argument, it is the thing keeping page headers and narrow compression windows from adding up —
which is also why the default is 256 KiB and not something smaller. The pair above pays +0.7% (149.0 → 150.0 MB) 
for 17 pages becoming 254.

`test/sql/copy/parquet/writer/parquet_write_content_defined_pages.test` covers the round trip, `false`-equals-
nothing, byte-for-byte repeatability, page counts and mean page size following `min`/`max`, five pinned page-count
vectors, the nested fallback, nulls, empty and oversized values, prepared-statement serialization, and the binder
errors. `test/sql/copy/*` passes with the patch in — 660 cases, 197674 assertions; the 49 skips are `require
httpfs`/`tpch`/`tpcds`/`encodings`/`windows` and one `mode skip flaky`.

## It cuts where Arrow cuts — and where it does not, it cuts less

Boundary agreement is the whole point of porting the algorithm rather than inventing one, so it is checked rather
than argued: `scripts/cdc_conformance.py` writes one 400k-row, 9-column table twice — once with pyarrow
(`use_content_defined_chunking`, dictionary off, no compression, one row group) and once with the patched writer
over those same values (`ROW_GROUP_SIZE` large enough for one row group too) — and compares, per leaf column, how
many values land on each data page. `scripts/parquet_page_values.py` reads those counts out of the page headers,
because nothing in a footer's column-chunk metadata answers the question: `parquet_metadata`'s
`encoding_stats.count` is pages per column chunk, not values per page, and byte offsets depend on headers, stats
and codecs, which the two writers do not share.

Min 256 KiB, 400k rows, once per (max, nullability) — `matches arrow` means the two lists of per-page value counts
are equal:

| column | max 512 KiB, nulls | max 1 MiB, nulls | max 512 KiB, no nulls | max 1 MiB, no nulls |
|---|---|---|---|---|
| `i` BIGINT, `d` DOUBLE, `f` DOUBLE, `b` BOOLEAN, `u` TIMESTAMP | matches | matches | matches | matches |
| `s` VARCHAR(128) | matches | matches | matches | matches |
| `short` VARCHAR(5), 6 distinct | matches | refined | refined | refined |
| `uuid`, `dec` (deliberately unsupported) | one page | one page | one page | one page |

The Arrow side of every one of those cells is frozen in `scripts/fixtures/arrow_<max>_<nulls>_400k.json`, so the
check runs without pyarrow (`--check-fixture`) and against a recorded answer rather than against whatever version
is installed. Exit code 0 means every column met its expectation; `--strict` also fails the refined cases.

`short` is the one column that is not page-for-page identical, and it is Arrow's page rules, not the boundaries:
DuckDB cuts `[178163, 178162, 43675]` and Arrow cuts `[133120, 45043, 133133, 45029, 43675]` — Arrow's cuts are a
strict superset, its `data_page_size` closing each of DuckDB's CDC pages at 1 MiB of encoded bytes, which for a
5-byte string means 4 extra prefix bytes per value that the chunker never counts. A Python model of the chunker
(feeding the same generated tables the same bytes) reproduces DuckDB's list exactly, so the chunker agrees and
Arrow's surroundings cut again. That is a writer-policy difference to know about, not to chase: replicating
parquet-cpp's encoded-byte accounting inside the chunker would make the *chunker* wrong.

Three things had to be pinned for the comparison to mean anything, and all three will move every boundary in a
file if they are left implicit:

* **units**, as above: byte counts on both sides;
* **nullability**, which is a fixture condition and not incidental data. DuckDB infers a column's nullability from
  statistics, so a source column with `null_count == 0` reads back NOT NULL and is written REQUIRED, while pyarrow
  writes the same values OPTIONAL. That is 0 versus 2 bytes per value going into the hash, so the harness asks for
  both explicitly (`--nulls mixed` puts a null in every column every 977 rows, `--nulls none` casts the Arrow
  schema to non-nullable) and reports the two separately;
* **Arrow's own page rules**, `max_rows_per_page` (20,000, so it is set to a billion here) and `data_page_size`.

Row groups are pinned to one on both sides so the state-scope difference stays out of the boundary comparison —
and that it is a difference and not a rounding error is measured above, not assumed.

What to say about all of this to DuckDB and to Arrow — a PR description and the two questions that are not ours to
answer — is drafted in `docs/CDC_UPSTREAM.md`.

## What I did not measure

Why fixed-width columns under pyarrow CDC keep only ~92% of their pages after an insert where the binary column
keeps 99% — the resync argument in this document says they should keep ~100%, so something about the dictionary
fallback or the `max_chunk_size` forced cuts is eating 8 points, and I have not pinned it down. The last few points
between page identity (99.0% of payload pages identical) and chunk sharing (95.0%): the byte chunker's boundaries
straddle pages, so the handful of changed pages spoil the ~64 KiB chunks around them too — a loss bounded by a
couple of chunks rather than by the file. Nested and list columns,
which is where the Arrow design is actually subtle. More than two versions of a file, and a long-lived file with a
history of edits rather than one edit. Whether the CAS's global query actually finds the shards in practice — that
is the `docs/XET.md` question, orthogonal to this one. And whether ~100 KB pages hurt read throughput enough to
matter: I would want `norm_level` and a 256 KiB floor there, as Arrow ships by default.

Two things the conformance section leaves open by construction. arrow-rs is assumed to agree with arrow-cpp — the
two implementations have matched before and the harness compares against C++ only, because that is what pyarrow
binds to and what the Hugging Face writer uses; a Rust side to the same fixture would be the next check. And what
is compared is *which rows land on which page*, never the page bytes: two writers with the same boundaries still
write different page headers, different statistics, a different codec and a different `created_by`, so a
DuckDB-CDC page and a pyarrow-CDC page of the same rows are not the same bytes. Cross-client dedup therefore still
runs on the uploader's chunker; what boundary agreement buys is that the *page* a client rewrites stays small, and
that `min`/`max`/`norm_level` mean the same thing wherever they are set.

## Reproducing

```sh
cd /home/quentin/hf/hf-duckdb-extension-bis # this repo
cargo build --release --manifest-path tools/xet_probe/Cargo.toml # the reference chunker
python scripts/cdc_page_measure.py 2000000 # the grid; add --by-schema for the nullability table
python scripts/parquet_pages.py $CDC_WORK/duckdb_required_v1.parquet # page sizes per column
python scripts/parquet_pages.py --compare $CDC_WORK/duckdb_required_v1.parquet $CDC_WORK/duckdb_required_insert.parquet
```

`python` here means a venv with `pyarrow` (21 or newer for `use_content_defined_chunking`; 26.0.0 for these
numbers) and `duckdb` for Python. The `duckdb-cdc` rows of the grid are written with the *patched* shell rather
than through the python module — the installed module is a stock DuckDB, which does not know the option — so point
the script at a build before running it: `DUCKDB=/home/quentin/hf/duckdb/build/release/duckdb` (that is also the
default). Without that build the two rows stop the run with a message naming the variable — the grid keeps every
file it wrote, so re-running against a real build costs only the rows that are missing. The grid writes ~2 GB of
Parquet to `$CDC_WORK` (`CDC_WORK=/tmp/cdc_grid` to move it) and does not talk to the network.

The same question, asked about a table you already have, with [nano-xet](https://pypi.org/project/nano-xet/) as
the store instead of the reference chunker:

```sh
pip install 'nano-xet>=0.1.2' pyarrow duckdb
python scripts/dedup_estimate.py --rows 500000 # one table, three edits, size-based pages next to CDC
python scripts/dedup_estimate.py --rows 2000000 --edit insert --row-group 2000000
```

It writes the file twice out of one DuckDB table - once as it is, once after an edit - stores the first in a
throwaway nano-xet store, and reports the share of the second's bytes that store did not have to take. Those
percentages are directional rather than quoted: nano-xet's chunker cuts at 8 / 64 / 128 KiB, so a file wants a
few dozen chunks before a number means anything, and the script says so when it has fewer - `cdc_page_measure.py`
above is the one whose numbers this document quotes. The writes shell out to `$DUCKDB` for the same reason the
grid does - the option is in the patched build, and that build has no python binding - and the `duckdb` import is
used on the other end: importing nano-xet registers `nxet://` with fsspec, and `con.register_filesystem(fs)` lets
DuckDB open the stored file back out, which is the check that the store holds what the writer wrote. That read
wants nano-xet 0.1.2 or newer - before it, `modified` answered with an epoch float, fsspec promises a datetime,
and DuckDB's bridge calls `.timestamp()` on whatever it is handed; the script checks the version up front rather
than letting that land as an error from inside `read_parquet`.

The boundary check against arrow-cpp, and the four recorded answers it compares against:

```sh
python scripts/cdc_conformance.py --check-fixture scripts/fixtures/arrow_524288_mixed_400k.json        # exit 0
python scripts/cdc_conformance.py --check-fixture scripts/fixtures/arrow_524288_none_400k.json --strict # exit 1
python scripts/cdc_conformance.py --max 524288 --nulls mixed --save-fixture scripts/fixtures/new.json  # re-record
```

A fixture carries its own conditions (`--min/--max/--norm/--rows/--nulls` default to what it says), so checking one
is one argument and no pyarrow — the recorded Arrow answer is in the file. The *values* are regenerated from the
row number when the recorded `values_file` is gone, which is why the fixtures survive `/tmp` being cleared; they
were recorded with pyarrow 26.0.0. A re-record is only honest against a pinned Arrow — parquet-cpp's
`ContentDefinedChunker` is unchanged since #45360, but `data_page_size` refinement and the defaults around it are
not guaranteed frozen.

With the patch built, the DuckDB side reproduces without pyarrow:

```sh
cd /home/quentin/hf/duckdb && git checkout content-defined-pages
export PATH=/home/quentin/hf/hf-duckdb-extension/.venv/bin:$PATH # cmake + ninja
cmake -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_SHELL=ON && cmake --build build/release --target duckdb unittest -j 10
build/release/test/unittest test/sql/copy/parquet/writer/parquet_write_content_defined_pages.test

# a smaller version of the same measurement: write a pair, then ask the reference chunker what it already has
D=/home/quentin/hf/duckdb/build/release/duckdb
$D -c "CREATE TABLE base AS SELECT i id, repeat(md5(i::VARCHAR),4) payload FROM range(1000000) t(i);
       CREATE TABLE edit AS SELECT id, CASE WHEN id=500000 THEN 'X'||payload ELSE payload END payload FROM base;
       COPY base TO 'v1.parquet' (FORMAT PARQUET, COMPRESSION zstd, ROW_GROUP_SIZE 1000000);
       COPY edit TO 'v2.parquet' (FORMAT PARQUET, COMPRESSION zstd, ROW_GROUP_SIZE 1000000);
       COPY base TO 'c1.parquet' (FORMAT PARQUET, COMPRESSION zstd, ROW_GROUP_SIZE 1000000, CONTENT_DEFINED_CHUNKING true);
       COPY edit TO 'c2.parquet' (FORMAT PARQUET, COMPRESSION zstd, ROW_GROUP_SIZE 1000000, CONTENT_DEFINED_CHUNKING true);"
/home/quentin/hf/hf-duckdb-extension-bis/tools/xet_probe/target/release/xet-probe \
    chunks < v1.parquet > v1.chunks   # and v2, c1, c2
python scripts/parquet_pages.py --compare v1.parquet v2.parquet              # and c1 / c2
```

`--compare` counts page bodies that hash the same; for the chunk numbers, intersect the two `chunks` files on the
hash column and divide by the new file's chunk count and bytes. Both are offline.

## References

- `/home/quentin/hf/vendor/duckdb` at `v2.0.0`: `extension/parquet/writer/primitive_column_writer.cpp`
  (`Prepare`, `WriteDictionary`, `MAX_UNCOMPRESSED_PAGE_SIZE`), `extension/parquet/include/parquet_writer.hpp`
  (`data_page_size_limit`, `DataPageSizeLimit()`),
  `extension/parquet/include/writer/templated_column_writer.hpp` (where the dictionary
  limits are actually consulted), `extension/parquet/parquet_extension.cpp` (the write options that reach users).
- Arrow C++ PR #45360 ("Support Content-Defined Chunking of Parquet files", issue #45750), merged 2025-05-13:
  `cpp/src/parquet/chunker_internal.{h,cc}`,
  `chunker_internal_codegen.py`, `properties.h`, `column_writer.cc`. CDC is wired only through `WriteArrow()`;
  `WriteBatch`/`WriteBatchSpaced` throw if it is enabled.
- Arrow Rust `apache/arrow-rs` parquet `chunker/cdc.rs`, merged 2026-03-20, same parameters and same defaults
  (256 KiB / 1 MiB / `norm_level` 0), one chunker per leaf column persisted across row groups.
- `parquet::arrow::write_properties` / pyarrow `use_content_defined_chunking`, and
  `huggingface.co/blog/parquet-cdc` for why the feature exists in the shape it has.
- `docs/XET.md` for the hash chain that must stay byte-exact, and for what a dedup hit costs to *discover*;
  `docs/PARQUET_METADATA_EDIT.md` for why the metadata can't be edited around these problems either.
- `scripts/cdc_page_measure.py`, `scripts/parquet_pages.py`, added with this document.
- The patch itself, `/home/quentin/hf/duckdb` branch `content-defined-pages` (off `c15a232a57`, not pushed):
  `extension/parquet/include/writer/parquet_cdc_chunker.hpp` (the chunker, its table and why the predicate is a
  probability), `writer/primitive_column_writer.cpp` (`Prepare`, and which types get cut by content),
  `include/writer/primitive_column_writer.hpp` (where the state lives), `parquet_extension.cpp` (the option, its
  parsing and its serialization tags 122–124), `include/parquet_writer.hpp` (the writer options).
  `test/sql/copy/parquet/writer/parquet_write_content_defined_pages.test` is the contract.
