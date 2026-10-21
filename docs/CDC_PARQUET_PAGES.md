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

Measured 2026-10. Tooling added with this document: `scripts/cdc_page_measure.py` (the grid) and
`scripts/parquet_pages.py` (a page-header walker with a `--compare` mode that hashes page bodies).

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
serialized page bytes — and a page is cut where the hash hits the boundary predicate, subject to
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
with and without the dictionary. Percentages below are shared bytes as a fraction of the *new* file, i.e. what a
dedup-aware uploader would not have to send.

2M rows (~150 MB per file), schema declared `NOT NULL` on both sides:

| edit | duckdb default | `data_page_size_limit` 1 MiB | 256 KiB | pyarrow fixed | pyarrow CDC | pyarrow CDC 64 KiB | … and no dict |
|---|---|---|---|---|---|---|---|
| append | 98.7% | 98.5% | 98.7% | 98.6% | 98.5% | 98.6% | 98.4% |
| update | 95.1% | 99.2% | 99.7% | 99.2% | 99.7% | 99.8% | 99.8% |
| insert | **50.7%** | 54.2% | 50.7% | 50.9% | 94.6% | **95.0%** | **95.9%** |
| delete | **50.8%** | 50.7% | 50.8% | 50.8% | 94.5% | **94.9%** | **95.9%** |
| widen | 66.7% | 66.7% | 66.6% | 70.6% | 70.7% | 70.7% | 69.2% |

At 200k rows the shape is the same and the effects are harsher (the row-shift cases bottom out at 48–50% and CDC
reaches 89–93%), which is the right direction: the *fraction* a shift damages is independent of file size, so the
absolute saving grows with the file. Cost of content-defined pages here is +0.8% file size (150.2 → 151.4 MB) and
10× the page count, which is a read-path cost too: more headers, more page-lookup work, slightly worse compression
per page.

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

**Matching Arrow's hash is not required, and should not be attempted.** Arrow's tables are its own generated
Gear tables keyed to *value* hashing, and page boundaries only have to be stable *within a writer*, in the file
bytes. The hash that must stay byte-exact for cross-client dedup is the chunk hash and chunker in
`src/hf_xet_chunk.cpp` — leave it alone. That is what makes this patch acceptable to upstream in a way that a
second chunking dialect here would not be.

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

## Reproducing

```sh
cd /home/quentin/hf/hf-duckdb-extension-bis # this repo
cargo build --release --manifest-path tools/xet_probe/Cargo.toml # the reference chunker
python scripts/cdc_page_measure.py 2000000 # the grid; add --by-schema for the nullability table
python scripts/parquet_pages.py $CDC_WORK/duckdb_required_v1.parquet # page sizes per column
python scripts/parquet_pages.py --compare $CDC_WORK/duckdb_required_v1.parquet $CDC_WORK/duckdb_required_insert.parquet
```

`python` here means a venv with `pyarrow` (21 or newer for `use_content_defined_chunking`; 26.0.0 for these
numbers) and `duckdb` for Python — DuckDB has no content-defined pages, so the CDC rows can only be produced by
the reference writer. The grid writes ~2 GB of Parquet to `$CDC_WORK` and does not talk to the network.

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
