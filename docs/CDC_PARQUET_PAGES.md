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
`content-defined-pages`, `COPY ... (FORMAT PARQUET, CONTENT_DEFINED_CHUNKING true)` — and it delivers the predicted
dedup on a DuckDB-written file (99.6% of a changed column's page bytes, 98.9% of the reference chunker's chunks,
against 20.6% and 72.3% without it). "It is built now" below is about the two places where the design above turned
out to be wrong, and what the built thing does that this document did not predict.

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

## It is built now, in a DuckDB fork

`/home/quentin/hf/duckdb`, branch `content-defined-pages` (local, not pushed). The option is

```sql
COPY t TO 'f.parquet' (FORMAT PARQUET, CONTENT_DEFINED_CHUNKING true);
COPY t TO 'f.parquet' (FORMAT PARQUET, CONTENT_DEFINED_CHUNKING {min_chunk_size: '256KB', max_chunk_size: '1MB'});
```

a `WRITE_ONLY` copy option of type `ANY`, so it takes a boolean or a struct of optional sizes, both accepted as a
byte count or as a size string. It is a copy option and not a setting precisely because a `COPY` gets prepared,
stored and logged: a session setting would silently change what an already-written script produces, while the
option travels with the statement (there is a test that prepares a `COPY`, executes it, and requires the file to be
byte-identical to the same `COPY` run directly). Sizes are uncompressed value bytes, as in Arrow; the defaults are
Arrow's 256 KiB / 1 MiB; `norm_level` is not implemented, and an unknown struct field is an error rather than a
silently ignored one — which is why there is no alias for `use_content_defined_chunking` either, since that name
comes with parameters this does not have. `false` writes bytes identical to omitting the option.

Two design notes from this document did not survive contact with the code.

**The chunker state cannot live on the writer.** "the state living in `PrimitiveColumnWriterState` and surviving row
groups" above has the right instinct and the wrong conclusion — it lives in the state *and does not* survive row
groups, because it has to. `ParquetWriter::Flush` calls `Prepare` with no lock held, and several row groups are
flushed concurrently against one shared `ColumnWriter`, so a chunker on the writer is a data race (it showed up
first as page counts that changed run to run, not as a crash). Seeding per row group costs almost nothing: a
boundary is decided by the values between the row group start and itself, so a row group nobody touched still
comes out byte-identical, which is the whole property.

**A bit-mask predicate is the wrong predicate.** The plan was the classic one — roll a hash over the value bytes,
cut when the low `log2(target)` bits are zero, which is what a *byte* chunker does to hit a target size. But a page
can only be cut between two values, so the predicate is sampled once per value, not once per byte: tested once per
128-byte value, a mask sized for 256 KiB wants 2^18 samples — 34 MB — to fire once, and every page then falls
through to `max_chunk_size`. Pages came out at 1 MiB whether the minimum was 64 KiB or 256 KiB. The fix is to make
the boundary a per-value probability proportional to that value's own size — cut with probability
`value_bytes / min_chunk_size`, which is the per-value form of a byte chunker's `1 / target` per byte, and which
makes the expected page the target whatever the column looks like. The rolling hash is avalanched (`lowbias32`)
before it is compared, because in a `(hash << 1) + table[byte]` update the low bits are mostly the last few bytes.
`min_chunk_size` is therefore a floor *and* the target, and pages average a little over twice it — measured over
38.4 MB of uncompressed string data in one row group: minimum 128 KiB → 152 pages (mean 253 KiB), 256 KiB → 73
pages (526 KiB), 512 KiB → 40 pages (958 KiB), option off → 1 page. Not the "distribution between min and max" a
byte-stream chunker produces, and worth knowing before setting it: DuckDB's average page is `2 × min`, Arrow's is
nearer `min`.

One more, smaller: the chunker counts its own bytes rather than reusing the writer's `estimated_page_size`, because
that one counts a *dictionary key's bit width* for a dictionary-encoded value. Left shared, a page-size limit and a
content-defined minimum would drift apart by orders of magnitude depending on an encoding decision made per row
group.

What the built thing is worth, measured with the reference chunker (`tools/xet_probe chunks`) over finished files —
1M rows, one row group, zstd, one 128-byte string changed in the middle:

| | pages of the changed column that survive | those pages' bytes | file chunks the store already has | chunk bytes |
|---|---|---|---|---|
| pages cut by size (DuckDB today) | 1 of 2 | 20.6% | 263 of 364 (72.3%) | 71.1% |
| pages cut by content | 248 of 249 | 99.6% | 354 of 358 (98.9%) | 98.0% |

The append case (1M rows → 1.1M rows, same measurement, chunk level) is 91.4% reusable with size-based pages and
91.0% with content-defined ones: **CDC pages do nothing for an append**, which the byte chunker at upload time
already covers almost completely, and which is why the mid-file edit is the case to argue from. Everything else the
earlier measurement said still holds: this is a flat-column feature (a list or struct leaf keeps size-based cuts,
since cutting there means cutting at repetition level 0), and the file is ordinary Parquet — pyarrow and stock
DuckDB 2.0 read these files, and `hash()` of the file and of a re-write of it both agree.

Page count is not free in file size, but it is cheap and the sign is not fixed: on the single 128-byte-string
column above, cutting into ~512 KiB pages made the file 3.6% *smaller* (21.6 MB against 22.5), because each page is
compressed on its own and small pages happen to suit this data; on a mixed-type table with dictionaries and
megabyte strings the same setting cost 3.7%, and `min_chunk_size: 4096` cost 15.2%. So the floor in the option is
not just a dedup argument, it is the thing keeping page headers and narrow compression windows from adding up —
which is also why the default is 256 KiB and not something smaller.

`test/sql/copy/parquet/writer/parquet_write_content_defined_pages.test` covers the round trip, `false`-equals-
nothing, byte-for-byte repeatability, page counts and mean page size following `min`/`max`, the nested fallback,
nulls, empty and oversized values, prepared-statement serialization, and the binder errors. `test/sql/copy/*`
passes with the patch in.

## The same question, answered twice: how the fork differs from Arrow's chunker

Read against the current `apache/arrow` main (`cpp/src/parquet/chunker_internal.{h,cc}`, the MD5-generated
`chunker_internal_generated.h`, `column_writer.cc`, `properties.h`) rather than against notes about it. Same family
— a Gear rolling hash, a size gate, cuts only between values — and eight real differences.

1. **Where the predicate is sampled.** Arrow rolls per *byte*, including inside a string value, and tests
   `(hash & mask) == 0` per byte, latching `has_matched_` so the cut lands after the value ends. Its per-value
   probability is therefore already proportional to value bytes — the property the fork derives and applies
   explicitly, one hash test per value instead of one per byte.
2. **Which bits are tested.** Arrow masks the **top** N bits of a 64-bit state: in `(h << 1) + table[b]` the high
   bits hold a mixture of the last 64 bytes, the low bits hold the last byte. The fork's state is 32 bits and it
   tests the whole word, so it avalanches first (`lowbias32`) — that is the bug the naive mask version hit.
3. **Distribution shape.** Arrow needs **8 hits in a row, one per gear table** (`nth_run_`,
   `kNumGearhashTables == 8`), the normalized-chunking trick from "Git is for Data" §6.2.1, which makes chunk
   sizes approximately normal. The fork has no normalization: sizes are geometric, mean ≈ 2×min with an
   exponential tail out to max.
4. **What `min` and `max` mean.** Arrow's mask width comes from `target = ((min+max)/2 - min)/8`, so its mean sits
   near `(min+max)/2` and `min` is only a floor; in the fork `min` is floor *and* target and `max` is only a cap.
   At the shared defaults these coincide — Arrow's own arithmetic lands on ≈512 KiB at min 256 KiB / max 1 MiB
   (its 48 KiB target is rounded down to a 32 KiB power of two), which is the fork's 2×min and matches the 526 KiB
   measured here — and they part elsewhere: at min 64 KiB / max 1 MiB Arrow gives ≈320 KiB, the fork ≈128 KiB.
   Arrow additionally has `norm_level` to sharpen the distribution; the fork has no equivalent.
5. **State lifetime.** Arrow holds one chunker per leaf column for the whole file, explicitly "not reset between
   pages and row groups", and leaves the last chunk open so the next `WriteArrow()` continues the same page. The
   fork's state is per row group, because DuckDB flushes row groups concurrently — and because a Parquet page
   cannot span row groups in any case.
6. **Nested data.** Arrow hashes the definition and repetition levels into the rolling hash (2 bytes per `int16`,
   counted into the chunk size) and cuts only at `rep_level == 0`, i.e. at record boundaries. The fork hashes a
   one-byte null marker, counts value bytes only, and leaves list and struct columns on size-based cuts.
7. **Dictionaries.** Arrow chunks an Arrow dictionary array by its **indices** (`DictionaryArray::indices()`) and
   dictionary-encodes within each chunk, so its boundaries follow the coding; DuckDB decides page membership before
   the dictionary decision and hashes the raw value bytes.
8. **Plumbing.** Arrow's is `enable_content_defined_chunking()` plus `CdcOptions{min, max, norm_level}` on the
   writer builder, and only through `WriteArrow()` — `WriteBatch()` and `WriteBatchSpaced()` throw. The fork's is
   a COPY option. And the hash tables are the fork's own (one 32-bit table from SplitMix32 against Arrow's eight
   64-bit tables from MD5), so boundaries do not line up: a DuckDB-CDC file and a pyarrow-CDC file of the same data
   share nothing beyond what the uploader's own chunking finds.

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

With the patch built, the DuckDB side reproduces without pyarrow:

```sh
cd /home/quentin/hf/duckdb && git checkout content-defined-pages
export PATH=/home/quentin/hf/hf-duckdb-extension/.venv/bin:$PATH # cmake + ninja
cmake -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_SHELL=ON && cmake --build build/release --target duckdb unittest -j 10
build/release/test/unittest test/sql/copy/parquet/writer/parquet_write_content_defined_pages.test

# the dedup table above: write a pair, then ask the reference chunker what it already has
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
