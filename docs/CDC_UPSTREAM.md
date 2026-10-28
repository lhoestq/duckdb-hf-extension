# Content-defined pages: what goes upstream, in the words to send

Two audiences, one patch. The DuckDB half is a PR from the fork (`lhoestq/duckdb`, branch
`content-defined-pages`, pushed at `4c62eb544f7` — open as duckdb/duckdb#27911, draft). The Arrow half is two
questions, not a patch — the behaviour we would have to copy is Arrow's, and whether it *should* be copied is
Arrow's call, though one of the two has since been answered by Arrow itself (see *The state scope*, below).
`docs/CDC_PARQUET_PAGES.md` has the measurements behind every number quoted here;
`scripts/cdc_conformance.py` reproduces the boundary claims.

## Before opening it — what is left now that it is open

The branch tip is `4c62eb544f7`: six commits, on top of `c15a232a57`, which is the fork's `main` and 292 commits
behind `duckdb/duckdb`'s as of late October. Five of the six are Krisztian Szucs's, stacked exactly on the first,
and they are what the PR now is: the original patch plus a parameter-validation pass and the state-scope change
below. The branch is still not rebased. The gear table is checked in with no generator beside it — the recipe
and the four known-first entries that prove a regeneration is Arrow's are in the table's own header, and the script
stays here with the rest of the tooling (`scripts/generate_cdc_gear_tables.py`), because a codegen script inside the
Parquet extension only invites the question of why it is not wired into a build step, when the answer is that it ran
once, on purpose, and its output is Arrow's numbers.
That is fine for opening the PR — GitHub diffs a PR against the merge base, so it shows seven files and nothing else —
but it wants rebasing before anyone takes it seriously, and the rebase has only been *tested for
conflicts*, not built: `git cherry-pick --no-commit` onto `d71c926f431` in a throwaway worktree applies cleanly,
and the reason it does is that upstream's 34 changed lines in `primitive_column_writer.cpp` are an allocator-aware
thrift-container refactor (`PageWriteInformation` now takes `row_group.columns.get_allocator()`) that touches
`BeginWrite`, `RegisterToRowGroup` and the stats writer, while `Prepare`'s body is byte-identical to the one this
patch was written against. No `ccache` here, so a rebased build is a full rebuild; after one, re-run
`test/sql/copy/parquet/*` *and* the four fixtures, because a rebase moves the writer under the boundaries and a
fixture is the only thing that notices. `parquet_writer.cpp` is now in the diff too, so the same question arises
there: upstream has been busy around `Flush`.

Two repo gates were checked with the tools DuckDB's own `scripts/format.py` uses, since a PR that fails them gets
a bot comment: clang-format (11.0.1 is pinned, unavailable on aarch64, so 15.0.7 was used after confirming it
leaves 60 untouched parquet files byte-identical) and `typos -c scripts/typos.toml`, both clean. The generated
gear table is wrapped in `// clang-format off` — with 4 spaces, not a tab, which is what clang-format leaves the
pragma itself at — because formatting re-packs the rows and the layout exists so a diff against a re-run of the
generator is a diff of numbers and nothing else.

## The PR description

It lives in `docs/CDC_PR.md`, paste-ready and current with the branch. Keeping a second copy here would mean two
texts to keep honest, and one of them would be the stale one.

## The state scope: argued, lost, and settled by Arrow

This section used to argue that DuckDB's chunker could not live on the writer. It was right about the reason and
wrong about the conclusion, and Arrow answered the same question in Arrow without consulting any of us.

The reason was real: `ParquetWriter::Flush` calls `Prepare` with no lock held, and several row groups are prepared
concurrently against one shared `ColumnWriter`. Putting the chunker on the writer without changing that is a data
race — it first appeared as page counts that changed from run to run, not as a crash. So the first patch held the
state on `PrimitiveColumnWriterState`, which restarts per row group, and that is what arrow-cpp *ships* (pyarrow 26
included) even though `column_writer.cc` says otherwise in a comment.

Krisztian Szucs moved it onto `PrimitiveColumnWriter` anyway — one chunker per leaf column per file — and paid for
it properly: `ParquetWriter::Flush` takes a `cdc_lock` across `PrepareRowGroup` and `FlushRowGroup` when the option
is on, and batch mode defers `PrepareBatch` into `FlushBatch`, which already runs in batch order. Row groups reach
the chunker one at a time, in file order, and the page boundaries stop depending on how many threads showed up.
Arrow did the same thing in #51685 ("Keep the CDC chunking state across row groups") on 2026-10-12 — three days
after 26.0.0 was cut, so it is in no released pyarrow.

What it is worth, insert-one-row-mid-file into a 2M-row file, 256 KiB / 1 MiB, only `ROW_GROUP_SIZE` moving
(`scripts/cdc_row_group_scope.py`):

| `ROW_GROUP_SIZE` | 122880 (default) | 1048576 (pyarrow's default) | one row group |
|---|---|---|---|
| bytes of the new file already held, before | 89.8% | 95.7% | 96.4% |
| bytes of the new file already held, after | 92.3% | 96.3% | 96.4% |

Two-and-a-half points at DuckDB's default row-group size, nothing at one row group — which is the shape you would
want: the gain is exactly the boundaries the restart used to lose, and a file with one row group never had them to
lose. The residual to pyarrow's 94.6% is not a chunker difference any more either; it is that a page cannot span a
row group, and DuckDB's default row group is eight times smaller than pyarrow's. The same pair without the option
moves 50.7% → 69.8% between the default and a single row group, so the row group was always a dedup lever that had
nothing to do with the hash.

The cost is write parallelism, and the PR should say so in the same breath: with the option on, a copy's writer
stage is serial. Measured here (5M rows, one 128-byte string column, 8 row groups, `read_parquet` source, best of
three): 0.92 s on 16 threads without CDC against 1.56 s with, where without CDC 16 threads still beats one
(1.36 s) and with CDC it barely does (1.75 s). One-row-group files pay only the hashing (2.06 s to 2.35 s).

What the *restart* used to cost in the only currency that matters for cross-client dedup — boundary agreement with
another writer's file — is now zero against arrow's `main` and still real against pyarrow 26: two clients writing
the same rows produce matching pages only inside the first row group, unless the other client is built from arrow
after #51685. That is worth a line in the PR description, because a reviewer with a wheel will otherwise test it
and see a mismatch.

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
