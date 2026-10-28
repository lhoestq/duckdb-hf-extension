# The PR description, paste-ready

Branch `content-defined-pages` at `4c62eb544f7` on `lhoestq/duckdb`, six commits, eight files, +1397/−13 (563 of
those lines are the generated gear table). The first commit is the patch; the five after it are Krisztian Szucs's
review pass — two parameter-validation fixes, one 32-bit-overflow fix, a comment fix, and the state-scope change
that question 1 below is about. Everything below the rule is the description; `docs/CDC_UPSTREAM.md`
holds the state-scope write-up and the two Arrow questions that are *not* part of it.

---

### Add `CONTENT_DEFINED_CHUNKING` to the Parquet writer

```sql
COPY t TO 'f.parquet' (FORMAT PARQUET, CONTENT_DEFINED_CHUNKING true);
COPY t TO 'f.parquet' (FORMAT PARQUET,
     CONTENT_DEFINED_CHUNKING {min_chunk_size: 262144, max_chunk_size: 1048576, norm_level: 0});
```

Data pages are cut where the values say, not only where a size limit says. The files stay ordinary Parquet — same
page types, same encodings, readable by any reader — the only difference is which row each page starts at.

#### Why

Datasets are stored by content now. With size-based pages, editing one value moves every later page boundary of
that column chunk, so the whole tail of the chunk is new bytes to a deduplicating uploader. `data_page_size_limit`
does not fix this, it relocates it: a smaller limit makes each damaged region smaller and the number of damaged
regions larger. Nothing else in the writer makes a page boundary survive an edit.

Measured on 2M-row files (~150 MB, four columns, one 64-char text column), with the **reference Xet chunker and
hasher** run over the finished files — i.e. what the real uploader would find, not a port of it — as the share of
the rewritten file's bytes that a store already holds:

| edit to the file | default pages | `DATA_PAGE_SIZE_LIMIT 1MiB` | `CONTENT_DEFINED_CHUNKING` |
|---|---|---|---|
| append a row | 98.7% | 98.5% | 98.6% |
| one cell changed mid-file | 95.1% | 99.2% | **99.8%** |
| one row inserted mid-file | 50.7% | 54.2% | **92.3%** (94.9% at 64 KiB sizes) |
| that row deleted again | 50.8% | 50.7% | **92.3%** (94.9% at 64 KiB sizes) |
| a column added | 66.7% | 66.7% | 66.6% |

The insert row is the whole argument: one shifted row invalidates every size-based cut after it, while a content
boundary re-establishes itself within a page or two. The append row is the reassurance: content-defined pages do
not make the easy case worse. Direct mechanism, same file pairs, counting pages whose *bodies* hash equal — 259
content pages of the payload column, 258 identical after a mid-file edit.

What it costs: **+0.8% file size** (150.2 → 151.4 MB) and an order of magnitude more pages, which is a read-path
cost too — more headers, more page-lookup work, slightly worse compression per page. Arrow's defaults have the same
cost; this is what the policy buys, not an artefact of the port.

#### The chunker is Apache Arrow's

A port of `parquet::internal::ContentDefinedChunker`, deliberately, rather than a new scheme:

* the same eight gear tables — Arrow's numbers, derived the way Arrow derives them (`md5(bytes([seed] * 64 +
  [byte] * 64))`, first 16 hex digits), table 0 starting `0xf09f35a563783945, 0x0dcc5b3bc5ae410a,
  0x63f1ea8d22554270, 0xfbe5ee7bd05a7b61`, which is what arrow-cpp generates in
  `cpp/src/parquet/chunker_internal_codegen.py` and what arrow-rs checks in as
  `parquet/src/column/chunker/cdc_generated.rs`. The recipe and those four anchors are stated in the table's own
  header comment so a regeneration that is not Arrow's gets caught rather than shipped;
* the same mask — the top N bits of a 64-bit hash, `N` from `((min + max) / 2 - min) / 8` — the same gate that
  leaves the hash alone below `min_chunk_size`, and the same rule that a boundary needs **eight consecutive hits,
  one per table**;
* the same convention that the value *completing* a chunk opens the next page (cut-before), unlike the size limit,
  which is a cut-after overflow rule;
* fed the same bytes: a value's bytes with no length prefix, and for a nullable column the two bytes of its
  definition level ahead of them, so a null is content there and here.

The point is not to agree with Arrow for its own sake. It is that `min_chunk_size`, `max_chunk_size` and
`norm_level` then *mean the same thing* in DuckDB, arrow-cpp and arrow-rs, so someone tuning a writer to talk to a
particular reader has one knob per property instead of one dialect per client. A second dialect under the same
parameter names would not have been tunable across writers at all.

#### Parity, checked rather than claimed

The same 400k rows go through pyarrow with `use_content_defined_chunking` and through this writer, and the harness
compares **values per data page**, row ranges equal per column. Six of seven columns are equal page for page
(`smallint`, `int`, `bigint`, `float`, `double`, `varchar` — the last one refined, below). The harness and four
frozen expected answers are in a companion branch of the extension repo, `parquet-page-cdc`
(`scripts/cdc_conformance.py`, `scripts/fixtures/`), because they need pyarrow and the reference chunker, neither
of which belongs in DuckDB's test suite.

The one column that is not equal is a `VARCHAR` column, and it is equal in the only sense the chunkers control:
Arrow's cuts are a strict superset of these, at a superset of the same offsets. Arrow's writer additionally closes
a CDC chunk at its own `data_page_size` (1 MiB by default), measured on *encoded* bytes — for a string, including
the 4-byte length prefix the chunker never counts. On 5-byte strings, Arrow cuts `[133120, 45043, 133133, 45029,
43675]` where this writer cuts `[178163, 178162, 43675]`: the chunker agrees, Arrow's surrounding page policy cuts
twice. `data_page_size_limit` here stays a size cut of DuckDB's own, on top of the content boundaries, and both
behaviours are tested.

Three conditions have to be pinned for the comparison to mean anything, and the harness pins them: nullability
declared explicitly (DuckDB infers `NOT NULL` from statistics, pyarrow keeps `OPTIONAL`, and definition levels are
fed to the hash); `max_rows_per_page` lifted, or Arrow's default 20 000 rows/page closes pages the chunker did not;
one row group per side — not because the two implementations disagree about row groups (they do not, see question
1) but because pyarrow 26.0.0, the version the recorded answers came from, restarts its chunker at every row group
and does not match its own `column_writer.cc` comment until #51685 ships.

#### Scope, deliberately narrow

* **Top-level columns only.** A list or struct leaf keeps size-based cuts: Arrow cuts those at repetition level 0
  and `Prepare` does not track repetition levels.
* **Only types whose physical bytes in DuckDB are the bytes an Arrow writer would hash.** `DECIMAL`, `UUID`,
  `TIME`, `INTERVAL`, `ENUM`, `HUGEINT`, `BIT` keep size-based cuts rather than be matched approximately.
* `false` writes bytes identical to omitting the option.
* It is a **write-only copy option, not a setting**, so a prepared or stored `COPY` keeps its behaviour and there is
  no session state to trip over; `read_parquet` rejects it as unknown, which is right.
* Sizes accept plain integers and DuckDB's usual suffixes. Plain integers are the ones to use for parity work:
  DuckDB reads `MB` as 10⁶ where Arrow's parameter is 1048576, and 4.6% off the ceiling moves every boundary after
  the first page. Errors name the option and say what was accepted.

#### Nothing is paid for when the option is off

The loop that decides page cuts gains two predictable branches, and the row-group lock is taken only when the
option is on. Measured by building the writer with and without
this commit and running 12 write shapes 8 times each — 250 MB of uncompressed strings, fixed widths with a
nullable column, snappy on and off, a LIST column: min-of-8 times are equal within run-to-run noise (a LIST
column: 0.025 s against 0.026 s, and which way round that falls depends on the run).

#### Two things worth arguing about in review

1. **The chunker state lives per leaf column per file, which makes a CDC copy's writer stage serial.** Arrow's is
   the same shape (per leaf column per file; `column_writer.cc` says "not reset between pages and row groups",
   arrow-rs matches, and arrow-cpp made it true as well as documented in #51685, merged 2026-10-12 — after 26.0.0,
   so no released pyarrow has it). Per-file state needs serialising in DuckDB in a way it does not in Arrow:
   `ParquetWriter::Flush` calls `Prepare` with no lock held and prepares row groups concurrently over one shared
   `ColumnWriter`. So with the option on, `Flush` takes a mutex across `PrepareRowGroup` and `FlushRowGroup`, and
   batch mode defers `PrepareBatch` into `FlushBatch`, which already runs in batch order; row groups then reach the
   chunker one at a time and in file order. The lock is taken only when the option is on.

   What that buys: boundaries stop depending on how the write was scheduled. Writing the same 400k-row table as one
   row group and as four gives identical content cuts, with and without nulls — the only extra cuts are the forced
   ones at the row-group ends — where the earlier per-row-group state moved six of eleven cuts in one column. That
   is arrow's `IndependentOfRowGroupBoundaries` property. And it is worth 2.5 points of dedup on a shifted file at
   DuckDB's default row-group size: insert-one-row-mid-file at 2M rows, 256 KiB / 1 MiB, with only `ROW_GROUP_SIZE`
   moving, 89.8% of the new file's bytes held before against 92.3% now at 122880, 95.7% against 96.3% at pyarrow's
   1048576, and 96.4% unchanged at a single row group.

   What it costs, in the same units (5M rows, one 128-byte string column, 8 row groups, best of three): 16 threads
   0.92 s without the option against 1.56 s with, and single-threaded 1.36 s against 1.75 s. Without the option 16
   threads still beats one; with it, barely. A single-row-group file pays only the hashing (2.06 s to 2.35 s), and
   the ratio depends on how cheap the rest of the write is — 20M `BIGINT` rows go 0.25 s to 0.39 s. So the trade is
   write parallelism for boundaries independent of the write's scheduling, and I think it is the right way round,
   since the option exists precisely to make boundaries predictable. The alternative is per-row-group state, which
   is a smaller patch and buys a serial writer back, at 2.5 points of dedup and no boundary agreement with another
   client's file past the first row group.
2. **`data_page_size_limit` still cuts on top of content cuts.** Kept deliberately: it is an overflow rule, not a
   boundary, and dropping it would let one huge value produce one huge page. It no longer disturbs the chunker — a
   size cut used to reset the count of bytes since the last boundary, which put `min_chunk_size` back to zero and,
   below that threshold, turned the option into a no-op; Arrow's `data_pagesize` does not reset its chunker either.
   If the consensus is that CDC should own page sizes entirely, that is a one-line change and a documented
   behaviour difference from Arrow.

#### Tests

`test/sql/copy/parquet/writer/parquet_write_content_defined_pages.test` (61 assertions) covers the option in both
spellings, defaults, serialization through a prepared `COPY`, `false` being byte-identical to no option, size and
norm bounds, unknown fields, and page counts against `parquet_metadata`'s `encoding_stats` for cut and uncut
columns — including that a `DATA_PAGE_SIZE_LIMIT` cut leaves the content boundaries where they were, and that
splitting the same data over five row groups adds four forced cuts and moves none of the content ones.
`test/sql/copy/*` on this branch: 660 cases, 197683 assertions, all passed,
49 skipped (31 `tpch`, 13 `httpfs`, 2 `tpcds`, 2 `windows`, 1 `encodings`, 1 `mode skip flaky`).

Measurements are from this branch on `linux-arm64`, pyarrow 26.0.0, the reference Xet chunker as built from
`xet-core` at `e922f74`.
