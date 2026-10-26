# Content-defined pages: what goes upstream, in the words to send

Two audiences, one patch. The DuckDB half is a PR from the fork (`lhoestq/duckdb`, branch
`content-defined-pages`, pushed at `39f85de128a`). The Arrow half is two questions, not a patch — the
behaviour we would have to copy is Arrow's, and whether it *should* be copied is Arrow's call. `docs/CDC_PARQUET_PAGES.md`
has the measurements behind every number quoted here; `scripts/cdc_conformance.py` reproduces the boundary claims.

## Before opening it

The branch is `39f85de128a`: one commit, seven files, on top of `c15a232a57`, which is the fork's `main` and 292
commits behind `duckdb/duckdb`'s as of today. The gear table is checked in with no generator beside it — the recipe
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
fixture is the only thing that notices.

Two repo gates were checked with the tools DuckDB's own `scripts/format.py` uses, since a PR that fails them gets
a bot comment: clang-format (11.0.1 is pinned, unavailable on aarch64, so 15.0.7 was used after confirming it
leaves 60 untouched parquet files byte-identical) and `typos -c scripts/typos.toml`, both clean. The generated
gear table is wrapped in `// clang-format off` — with 4 spaces, not a tab, which is what clang-format leaves the
pragma itself at — because formatting re-packs the rows and the layout exists so a diff against a re-run of the
generator is a diff of numbers and nothing else.

## The PR description

It lives in `docs/CDC_PR.md`, paste-ready and current with the branch. Keeping a second copy here would mean two
texts to keep honest, and one of them would be the stale one.

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
