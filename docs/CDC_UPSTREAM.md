# Content-defined pages: what goes upstream, in the words to send

Two audiences, one patch. The DuckDB half is a PR from the fork (`lhoestq/DuckDB`, branch
`content-defined-pages`, pushed at `6db6f00f1a0`). The Arrow half is two questions, not a patch — the
behaviour we would have to copy is Arrow's, and whether it *should* be copied is Arrow's call. `docs/CDC_PARQUET_PAGES.md`
has the measurements behind every number quoted here; `scripts/cdc_conformance.py` reproduces the boundary claims.

## Before opening it

The branch sits on `c15a232a57`, which is the fork's `main` and 292 commits behind `duckdb/DuckDB`'s as of today.
That is fine for opening the PR — GitHub diffs a PR against the merge base, so it shows eight files and nothing
else — but it wants rebasing before anyone takes it seriously, and the rebase has only been *tested for
conflicts*, not built: `git cherry-pick --no-commit` onto `d71c926f431` in a throwaway worktree applies cleanly,
and the reason it does is that upstream's 34 changed lines in `primitive_column_writer.cpp` are an allocator-aware
thrift-container refactor (`PageWriteInformation` now takes `row_group.columns.get_allocator()`) that touches
`BeginWrite`, `RegisterToRowGroup` and the stats writer, while `Prepare`'s body is byte-identical to the one this
patch was written against. No `ccache` here, so a rebased build is a full rebuild; after one, re-run
`test/sql/copy/parquet/*` *and* the four fixtures, because a rebase moves the writer under the boundaries and a
fixture is the only thing that notices.

Two repo gates were checked with the tools DuckDB's own `scripts/format.py` uses, since a PR that fails them gets
a bot comment: clang-format (11.0.1 is pinned, unavailable on aarch64, so 15.0.7 was used after confirming it
leaves 60 untouched parquet files byte-identical) and `typos -c scripts/typos.toml`, both clean. The generated
gear table is wrapped in `// clang-format off` — with 4 spaces, not a tab, which is what clang-format leaves the
pragma itself at — because formatting re-packs the rows and the layout exists so a diff against a re-run of the
generator is a diff of numbers and nothing else.

## To DuckDB (PR description)

> **Add `CONTENT_DEFINED_CHUNKING` to the Parquet writer**
>
> ```sql
> COPY t TO 'f.parquet' (FORMAT PARQUET, CONTENT_DEFINED_CHUNKING true);
> COPY t TO 'f.parquet' (FORMAT PARQUET,
>      CONTENT_DEFINED_CHUNKING {min_chunk_size: 262144, max_chunk_size: 1048576, norm_level: 0});
> ```
>
> Data pages are cut where the values say, not only where a size limit says. The files are ordinary Parquet —
> same page types, same encodings, readable by any reader — the only difference is which row each page starts at.
>
> That matters once the file is stored by content. With size-based pages, editing one value moves every later
> page boundary of that column chunk, so the whole tail of the chunk is new bytes to a deduplicating uploader.
> `data_page_size_limit` does not fix this, only relocates it: a smaller limit makes each damaged region smaller
> and the number of damaged regions larger. Measured with the reference Xet chunker over finished 2M-row files,
> as a share of the rewritten file's bytes that a store already holds:
>
> | edit to the file | today | `DATA_PAGE_SIZE_LIMIT 1MiB` | `CONTENT_DEFINED_CHUNKING` |
> |---|---|---|---|
> | one cell changed mid-file | 95.1% | 99.2% | 99.7% |
> | one row inserted mid-file | 50.7% | 54.2% | 89.8% (94.2% at 64 KiB sizes) |
> | append | 98.7% | 98.5% | 98.7% |
>
> The chunker is a port of `parquet::internal::ContentDefinedChunker` rather than a new scheme, on purpose: same
> gear tables (generated with Arrow's MD5 recipe, checked against Arrow's own known-first entries), same mask over
> `((min + max) / 2 - min) / 8`, same eight-consecutive-hits rule, same cut-before convention, and the same bytes
> fed to the hash — value bytes with no length prefix, plus two bytes of definition level per value when the column
> is nullable. So `min_chunk_size`, `max_chunk_size` and `norm_level` mean the same thing in DuckDB, arrow-cpp and
> arrow-rs, and a user tuning a writer to talk to a particular reader has one knob per property. Boundaries are
> checked against pyarrow's output row for row, values per data page, by a fixture-based harness.
>
> Scope, deliberately narrow: top-level columns only (a list or struct leaf keeps size-based cuts, since Arrow
> cuts those at repetition level 0 and `Prepare` does not track repetition levels), and only types whose physical
> bytes in DuckDB are the bytes Arrow would hash — DECIMAL, UUID, TIME, INTERVAL, ENUM, HUGEINT and BIT are left
> on size-based cuts rather than matched approximately. `false` writes bytes identical to omitting the option. It
> is a write-only copy option, not a setting, so a prepared or stored `COPY` keeps its behaviour; there is no
> session state to trip over, and `read_parquet` rejects it as unknown, which is right.

> Nothing here is paid for when the option is off. The loop that decides page cuts gains two predictable
> branches, and 12 write shapes repeated 8 times each — 250 MB of uncompressed strings, fixed widths with a
> nullable column, snappy, a LIST column — time the same out of the patched and the unpatched binary, within
> the run-to-run noise of either.
>
> One known divergence from Arrow, above: Arrow keeps one chunker per leaf column for a whole file, this keeps one
> per row group. Numbers and the reason below — it is the one thing in here someone may want to change, and it is
> not free to change.

## The state scope, if anyone wants to argue it (DuckDB issue, or a note in the PR)

Arrow's chunker is per leaf column, per file: `column_writer.cc` says "not reset between pages and row groups",
and arrow-rs matches. DuckDB's cannot be, as the code stands: `ParquetWriter::Flush` calls `Prepare` with no lock
held, and several row groups are prepared concurrently against one shared `ColumnWriter`. Putting the chunker on
the writer is a data race — it first appeared as page counts that changed from run to run, not as a crash.

The cost, insert-one-row-mid-file into a 2M-row file, 256 KiB / 1 MiB, only `ROW_GROUP_SIZE` moving:

| `ROW_GROUP_SIZE` | 122880 (default) | 1048576 (pyarrow's default) | one row group |
|---|---|---|---|
| bytes of the new file already held | 89.8% | 95.7% | 96.4% |

So of the ~5 points between DuckDB and pyarrow at default settings, about 4 are that a page cannot span row
groups — a smaller row group forces a cut every 122880 rows whatever the hash thinks — and about 0.7 points is the
state restart itself. Worth stating plainly because the easy reading ("Arrow keeps state longer, so it dedups
better") overstates it: at Arrow's row-group size, DuckDB is already ahead of pyarrow's own number (95.7% against
94.6% for the same edit). What the reset does cost is *boundary agreement with another writer's file*: two clients
writing the same rows produce matching pages only inside the first row group.

If per-file state is ever wanted it needs a lock or a per-column mutex around the boundary decision, or row groups
prepared in order — all three cost more than 0.7 points of dedup. The honest recommendation in the PR is to leave
it and say why.

## To Arrow (two questions, no patch)

1. **`data_page_size` refines CDC chunks, and a second implementation cannot see it.** With CDC on, parquet-cpp
   still applies the writer's `data_page_size` (1 MiB by default) to what the chunker produced, and that limit is
   measured on *encoded* bytes — for a string, including the 4-byte length prefix the chunker never counts. On a
   column of 5-byte strings, Arrow cuts `[133120, 45043, 133133, 45029, 43675]` where DuckDB cuts
   `[178163, 178162, 43675]`: a strict superset, at a superset of the same offsets, so the chunkers agree and
   Arrow's page policy cuts twice. Is that a contract ("CDC gives candidate boundaries, the writer still owns
   page size") or an accident of ordering? It is the one thing in a conformance run between the two writers that
   is not explained by configuration — every other difference traced to units (`KB` is 10^3 in DuckDB, 2^10 as
   `KiB`) or to nullability.
2. **Is there a documented statement of what the chunker consumes?** The values, in which encoding, and the level
   bytes: Arrow hashes `int16` definition and repetition levels, 2 bytes each, and no bytes for a non-nullable
   column, which is reproducible. But for a string it hashes the bytes with no length prefix while the *page size*
   accounting includes one, and for a dictionary array it hashes the *indices* and for DECIMAL the unscaled
   bytes in the Arrow decimal128 layout. None of that is in `CdcOptions` or the blog; it is in the code, so a
   second implementation reproduces it by reading `column_writer.cc` line by line. A paragraph in
   `properties.h` — "the chunker hashes: value bytes as Arrow sees them, levels as int16, dictionaries by index"
   — is the difference between an interoperable setting and a coincidence.

Neither question asks Arrow to change code. Both are about turning behaviour that exists into a promise.
