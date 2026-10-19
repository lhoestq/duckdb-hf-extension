# Xet — the protocol behind `hf_xet_write`, and what it was measured against

Source of truth is `xet-core` (<https://github.com/huggingface/xet-core>), cloned at `../xet-core`; everything
here was read out of it at `e922f74` and then checked against the production CAS. Symbol names rather than line
numbers, because lines move. Milestones M1–M9 are named the same way here, in `src/hf_xet_*.cpp` and in the
CHANGELOG.

Xet buys two things: `COPY … TO 'hf://buckets/…'` (the bucket batch API's `addFile` demands a `xetHash`, and a
bucket has no other upload route — `sha256`, no hash and inline `contents` were all probed and refused), and
cheaper uploads in general. It is *not* needed for git-LFS writes to dataset/model/space/kernel repos, which is
why the LFS path stayed as the fallback.

## The hash chain (the part that must be byte-exact)

**BLAKE3 keyed hash**, not SHA-256 (`merklehash/data_hash.rs`):

```
chunk hash    = blake3_keyed(DATA_KEY,           chunk_bytes)
internal hash = blake3_keyed(INTERNAL_NODE_HASH, "hex(size) : size\n" lines concatenated)

DATA_KEY           = 102 151 245 119  91 149  80 222  49  53 203 172 165 151  24  28
                     157 228  33  16 155 235  43  88 180 208 176  75 147 173 242  41
INTERNAL_NODE_HASH =   1 126 197 199 165  71  41 150 253 148 102 102 180 138   2 230
                      93 221  83 111  55 199 109 210 248  99  82 230  74  83 113  63
```

A hash is 32 bytes held as `[u64; 4]`, with two renderings and one trap: `hex()` prints `word[i].to_le()` as 16
lowercase digits, i.e. **each 8-byte group is byte-swapped** relative to the raw digest; `base64()` is URL-safe,
unpadded, over the raw bytes (that is what goes in `xetHash` query strings). A port can get every hash right and
the rendering wrong, and never see it locally. Built from those: **file hash** = merkle root of the file's
`(chunk_hash, size)` list, **xorb hash** = merkle root of the chunks in one xorb (what `addFile` names),
**range hash** = the same over a subsequence, used by reconstruction. An empty chunk list hashes to the zero
hash with no salt applied — "empty file" is not "keyed hash of a zero root".

**The mean tree** (`aggregated_hashes.rs`), branching factor 4, shape derived from the hashes themselves: a node
is a natural cut when `hash[3].to_le() % 4 == 0`; a group has **2..9** entries (the scan for a cut starts at the
3rd entry, stops at the 9th); a parent is the internal hash over its children rendered as
`"{child_hex} : {child_size}\n"`, one line per child.

## Chunking (`deduplication/chunking.rs`)

Content-defined with a **Gear hash** (`gearhash` crate, whose 256-entry table is ported exactly):
`mask = (target - 1) << (target - 1).leading_zeros()`, so the comparison is on the **high** bits (the low bits of
a Gear hash are weak); target 64 KiB, min `target/8`, max `target*2`; a boundary is `hash & mask == 0`, never
below min, forced at max, last partial chunk flushed with `finish()`. Getting the mask from the low bits chunks at
almost every byte *and still reproduces the reference boundaries on constant and tiny inputs*, because the minimum
floor dominates there — only content with real triggers tells them apart.

Chunk sizes are a dedup-efficiency choice, not a correctness contract (the client's hashes are authoritative), but
matching `hf_xet` byte-for-byte is what makes DuckDB-written content dedup against Python-written content. Xorb
limits: `MAX_XORB_BYTES = 64 MiB`, `MAX_XORB_CHUNKS = 8192` — measured on **uncompressed** bytes, since the CAS
addresses a xorb by the hash of its chunks and does not know our layout.

## Xorb format (`xorb_object/`)

Magic `XETBLOB`; a chunk section of `<8-byte header><chunk bytes>` per chunk; then a V1 footer carrying the xorb
hash, a `XBLBHSH` hash section, a `XBLBBND` section with **both** boundary lists (physical offsets with headers
included, and cumulative uncompressed lengths — swapping them yields correct hashes and wrong chunk sizes),
`num_chunks` three times, two section offsets, a 16-byte reserved buffer whose first 4 bytes are a per-upload
**nonce**, and the footer's own length. All little-endian.

Two traps: the section offsets count back from the end of the footer *struct*, which **excludes** the trailing
length u32 (off-by-four serializes fine and cannot be read back); and the nonce is excluded from the xorb hash on
purpose — it is the one field a client can randomize so two identical uploads are distinguishable on the wire
without changing what they are.

Chunk payloads are `CompressionScheme`: `None = 0`, `LZ4 = 1`, `ByteGrouping4LZ4 = 2`, `Auto = 99`. This build
ships `lz4` (default) and `none`. `lz4frame.c` v1.10.0 is vendored rather than hand-rolled, and our frames are
*not* byte-identical to the reference's `lz4_flex` output (+0.9% on a CSV, +0.02% on random bytes) — which is not
the contract. The contract is that a chunk header's scheme byte plus its compressed length let a reader decode that
chunk alone, and that the hash covers the bytes before compression. Two consequences worth stating: an xorb's hash
is the merkle root of the **uncompressed** chunk hashes, so all four schemes give the same `xorb_hex` for one file
(measured, one file written four ways with `tools/xet_probe`) — compression is a wire decision, not identity; and
reference clients compress **every** chunk, so an incompressible chunk is stored raw inside a frame (~16 B, 1.001×)
rather than skipped. `bg4` is not implemented (it needs `bg4_split` and the K-Lipschitz predictor, and it lost to
plain lz4 on the data that got here); `Auto` is refused in the constructor rather than guessed at.

## Shard format (`metadata_shard/`)

Not protobuf — hand-rolled little-endian, which is a gift: the writer is a few hundred lines of appends and no
dependency. What a session uploads is *not* `MDBShardInfo::serialize` either, but that same stream **cut short at
`file_lookup_offset` with the header's `footer_size` rewritten to 0** (`read_shard_to_bytes_remove_footer`):
header, file section, file bookend, xorb section, xorb bookend — no lookups (the CAS rebuilds them) and no footer
(the timestamp lives there, which is why re-sending identical content dedups instead of making a new shard).

```
header  "HFRepoMetaData" + 18 tag bytes, u64 version 2, u64 footer_size = 0
file    hash, u32 flags, u32 num_segments, u64 0
  seg   xorb_hash, u32 xorb_flags, u32 unpacked_bytes, u32 chunk_index_start, u32 chunk_index_end
  verif range_hash + 16 zero bytes            (one per segment)
  ext   sha256 + 16 zero bytes                (only when WITH_METADATA_EXT)
  bookend all-ones hash, zeros
xorb    xorb_hash, u32 flags, u32 num_chunks, u32 num_bytes_in_xorb, u32 num_bytes_on_disk = 0
  chunk hash, u32 byte_range_start, u32 unpacked_bytes, u32 flags, u32 0
  bookend all-ones hash, zeros
```

Three coordinate systems, each of which cost something to learn:

* **A file segment's chunk indices are xorb-local.** A reader resolves the xorb by hash and slices *its* chunk
  list, so a segment covering a whole xorb is `0..len`. The CAS checks this (`400 (19) invalid shard failed to
  verify file`), and a single-xorb file cannot tell the difference — which is why the first uploads all worked.
* **The verification hash is a keyed blake3 over the concatenated chunk hashes of a *file-wide* range**
  (`chunk_verification.rs`). Same key, different coordinate system from the line above; the shard stores neither
  range, so a writer has to carry both.
* **`chunk_byte_range_start` is cumulative *unpacked* length.** Compression happens underneath the space a reader
  asks in, so LZ4 changed xorb serialization and left the shard untouched.

## Upload protocol (`cas_client/remote_client.rs`)

The CAS is a separate HTTP service and the Hub tells the client where it is:

```
GET  {cas}/v1/chunks/default/{chunk_hash_hex}     cross-repository dedup query (see M9 below)
POST {cas}/v1/xorbs/default/{xorb_hash_hex}        upload a xorb -> {"was_inserted": bool}
POST {cas}/v1/shards                               upload a session shard -> {"result": 0|1, "shard_hash": …}
GET  {cas}/{api}/reconstructions/{file_hash}       read path: which xorb ranges make a file
POST {endpoint}/api/{type}/{repo}/xet-write-token/{revision}   exchange the HF token for a CAS token
```

Details that are not guesses: the revision is a **path segment** on the write-token route
(`…/xet-write-token` answers 404, `…/xet-write-token/main` answers `{casUrl, accessToken, exp}`); `POST` not `PUT`
for xorbs; the hash in the url is the word-swapped hex Rust's `{:x}` prints for a `Key`; the CAS request carries
the **CAS token and nothing else** (attaching the HF `Authorization` alongside is refused, exactly as with
presigned LFS PUTs); buckets and git repos get the same CAS and the same call, the token just says which
repository the content belongs to.

**There is no exists-check.** The url *is* the content, so re-posting is idempotent and `was_inserted: false` is
the CAS saying "already had it"; the reference's session-level filtering exists to skip work, not to preserve
correctness. Retries: 5 attempts, 3 s base exponential, 6 min budget, retrying 5xx/408/429/timeout/connection
errors (the reference's policy, transcribed). Tokens are cached per client and refreshed 30 s before expiry, under
a mutex and **copied out** — an earlier `CasUrl()` returned a `const string&` into state another thread's refresh
could rewrite mid-header. An `exp` that cannot be parsed is treated as already expired.

`POST /v1/shards` answers `result` 0 = already present, 1 = merged, with `shard_hash` in plain hex — and it is the
hash of the shard *as the CAS re-serialized it*, a name the service chose, not something to compare locally. A
bucket then takes the file:

```
POST {endpoint}/api/buckets/{repo}/batch   Content-Type: application/x-ndjson
{"type":"addFile","path":"duckdb/x.parquet","xetHash":"<word-swapped file hash>","mtime":<ms>}
```

`xetHash` is `MerkleHash::hex()` — the word-swapped hex, checked against the live Hub rather than reasoned out:
the reference chunker and hasher reproduce `xet_probe.parquet` as exactly what the tree API reports for it.

For a **git-backed** repo the commit stays the ordinary `lfsFile {path, algo: sha256, oid, size}` pointer commit;
the only difference is the shard's file-level metadata extension (`flags | WITH_METADATA_EXT` plus the 32-byte
sha256), which is what links the hash the commit names to the hash the shard stores. `huggingface_hub` omits it
for buckets (`SKIP_SHA256` — a bucket file is addressed by its xet hash, nothing to join) and sends it for
everything else. Note that **that sha256 is hex-parsed as a `DataHash`** — four 16-digit groups read as
little-endian words then word-swapped (`Sha256Policy::from_hex` → `DataHash::from_hex`) — not as 32 byte pairs. The
wrong reading still writes 32 bytes, still produces a shard the CAS accepts, and still leaves a file that resolves
to nothing; the selftest pins two vectors plus the flags word, because an extension written without being flagged
(or flagged without being written) is invisible to a byte diff of a *correct* shard.

Nothing on the read side gates any of this: the write-token route answers for datasets, models and spaces —
including a repo created `xetEnabled: false` (`lhoestq/hf-ext-nonxet-probe`, whose API info reports
`xetEnabled: None`, was handed a token for `cas-server.xethub.hf.co`) — and `preupload` answers `uploadMode: "lfs"`
for Xet-enabled repos. There is no capability to preflight; the *client* picks the route, as `hf_xet` does. Hence
`hf_xet_write` on by default with the LFS upload as its fallback, and an error that names the earlier Xet failure
if the commit fails too, because "the server rejected the commit" and "the CAS was down and we sent LFS bytes"
need different responses from a user.

## Concurrency

Content addressing is what licenses parallelism: an xorb's url is its content, no xorb depends on another, and the
order they land in is unobservable. The shard *is* order-sensitive, so the thread reading the file builds each xorb
and records everything the shard will say about it, in file order, and hands the bytes over; only the CAS's
`inserted` answer comes back, collected after every POST lands. In flight is the memory bound and the backpressure
bound with the same number: a queued xorb is up to 64 MiB, so `Submit` blocks at `hf_write_max_parallel_uploads`,
which is also why a 10 GB file costs the same memory as a 100 MB one.

Workers get a CAS client **without a `FileOpener`** (`ForWorkerThread`): a `ClientContext`-backed opener reads
settings through the session it belongs to, so two threads sharing one is not something to discover through a bug
report (the read path made the same choice in `FetchWindow`, including running the work inline if a thread fails to
start). The cost is transport parameters resolved from `DBConfig` rather than the session — for one POST per xorb,
to a service that inspects none of them. Auth is unaffected: the token comes from the resolved `HFConnection`,
exchanged on the main thread before any worker exists. Failure: the first error is kept and thrown from `Finish()`,
`Submit` stops feeding once one exists, and the destructor joins unconditionally because the producer can throw
mid-file while a worker holds a reference to it.

## Verification

Every stage is diff-tested against `xet-core`'s own tooling through `tools/xet_probe` (`chunk`, `hash`, `xorb
<scheme>`, `xorb-read`, `shard`) — the reference implementation is the oracle, because hash and format bugs are
silent. Vectors are **generated** by `scripts/xet_oracle.py` into `src/hf_xet_vectors.cpp`, never hand-written, and
committed so CI needs no cargo; `--check` is what notices xet-core drifting. Inputs are reproducible specs (literal
bytes, a repeated pattern, splitmix64) and are fed to the chunker in deliberately awkward piece sizes (1, 4, 19, …)
requiring identical chunk lists, because the write path feeds it from a file cursor. `hf_xet_selftest()` replays
them inside the extension: **1090 checks**.

What that caught, worth knowing before touching this code: the two key constants are load-bearing invisibly (one
transcribed wrongly made every hash wrong and every hash *plausible*); the chunk mask's high bits, above; `hex()`
rendering, above; and the fact that **offline byte-equality is necessary and not sufficient** — the shard passed
every generated vector and the production CAS still rejected the first multi-xorb file, because a vector with one
xorb cannot distinguish xorb-local from file-global chunk indices. That is also why the live suites matter: the CAS
recomputes every xorb hash and re-derives every shard segment's verification hash, so an accepted upload is a
confirmation from code we did not write (a deliberately wrong hash came back
`400 BadRequest: xorb hash mismatch: expected fc79…, got 4bf3…`).

It happened twice over, in the same shape. The first was a shard that passed every byte vector and was rejected by
the real CAS. The second was dedup: a matcher that queried correctly, parsed the shard the CAS returned, registered
it, and matched nothing — `deduped_chunks = 0` with every query answered 200 — because stored chunk hashes are keyed
and it compared them with unkeyed ones. Both were invisible to tests that only exercise *this port's own* bytes.
The remedies are the same two: ask the real service a question whose answer it composes itself, and compile the
answer's shape in as a fixture (`HFXetStoredShardVectors()`) so the offline suite covers bytes no upload of ours
produces.

Two tests are worth copying as patterns, because "upload it and time it" is not an assertion:

* **concurrency**: write a multi-xorb file at the default of 3, re-upload it at 1, require every xorb
  `inserted = false` and the shard `exists`. Xorb urls are content hashes, so a bug that moved a boundary,
  reordered a chunk between xorbs, or booked a segment against the wrong object produces hashes the CAS has never
  seen — and it would answer `true`. Costs no new storage, so it can live in a live suite.
* **compression**: write with lz4, re-upload with `hf_xet_compression = 'none'`, require the same
  `inserted = false`. The service recognizing content by hash that it has never seen by url is the strongest
  available statement that compression is not part of identity.

What stays uncovered: a failure *during* a concurrent upload, and the LFS fallback firing. Nothing in SQL makes a
CAS upload fail, and a test-only seam in the upload path is worse than the gap.

## M9 — cross-repository dedup (shipped; the producer's rules were measured first, the reader's key came last)

Established against the live CAS rather than by reading field names: 2026-10-18 for what a query *returns*,
2026-10-19 for *which chunks it answers about at all* — the gate that decides how much of the feature is available.
The instrument is `scripts/cas_dedup_probe.py`: it chunks and hashes a file through `xet-probe` (`chunks` and `dedup`
modes) so that no part of the answer depends on our own chunker or hash rule being right, exchanges a repo-scoped
write token, and sweeps `GET {cas}/v1/chunks/default/{chunk_hash_hex}` one hash at a time.

* **A hit is a complete metadata shard** — same header, same 48-byte entries, the format `hf_xet_shard.cpp` now
  parses in `HFXetShardView::Parse`. The answers are the uploaded shard plus a 56-byte envelope: 576 → 632,
  9 744 → 9 800, and a 64 MiB file's 49 536-byte shard → 49 592. A miss is a 404 with a 53-byte body.
* **The index covers *file-start* ∪ *one hash in 1024*, and the CAS works the second half out for itself.**
  The gate experiment: a 67 108 864-byte random payload (`/dev/urandom`, not published — `publish := false`, so the
  CAS got content and a shard and no file was named) chunked into 1 025 chunks in one xorb
  (`72fe5a3663b5b47eb45e8076283c32d605067d862ff70a45e7abaad4d04bd5b1`), shard 49 536 bytes, CAS answer
  *sync performed*. Of 17 hashes asked, the 4 that the rule marks eligible answered 200 — chunk 0 (file-start) and
  chunks 445, 903 and 938, which are eligible *only* by hashing — and all 13 controls answered 404. Every hit carried
  the same 49 592 bytes: the shard that holds them, ours.
* **So the persisted chunk flag is not the lever, and writing it more widely would be wrong.** Chunks 445/903/938
  were uploaded with `HF_XET_CHUNK_FLAG_GLOBAL_DEDUP` (`0x80000000`) clear and were indexed anyway; the earlier note
  here that "the index holds only flagged chunks" misread a first-chunk hit as a flag hit. The reference agrees:
  `MDB_CHUNK_WITH_GLOBAL_DEDUP_FLAG` is set on a file's first chunk by the writer, and
  `is_global_dedup_eligible()` is a *read-time* predicate — the persisted bit **or** `hash_is_global_dedup_eligible()`.
  Compiling the rule into the writer instead of the reader breaks the reference byte vectors: `splitmix64 n=4000000
  seed=11` stops matching the moment the OR appears, and the selftest row `<vector> sampled-eligible chunks` (one
  such chunk, stored with the bit clear) is the check that keeps it that way.
* **The rule itself is `Word(3) % 1024 == 0`**, from `impl Rem<u64> for DataHash` = `self[3].to_le() % rhs` with
  `MDB_SHARD_GLOBAL_DEDUP_CHUNK_MODULUS = 1024`. Replayed, not reasoned: `xet-probe dedup` answered for 100 000
  hashes and `HFXetGlobalDedupEligible` agreed 100 000 times; 470 of those pairs (94 positives) are compiled into
  `HFXetDedupVectors()` and graded by `hf_xet_selftest`. In print the rule is visible as hashes ending in `00` — all
  three mid-file hits do.
* **What the reference matches** (`ShardInMemory::chunk_hash_dedup_query`, `xet_core_structures`): look the *first*
  query hash up in the xorb chunk table, extend forward while hashes keep agreeing, and answer with
  `(run_length, FileDataSequenceEntry::from_xorb_entries(...))` — which copies the referenced xorb header's
  `xorb_flags`, sums the run's `unpacked_segment_bytes`, and carries xorb-*local* chunk indices. Queries are issued
  for chunk 0 and for eligible chunks, no more than once per `min_spacing_between_global_dedup_queries = 256`
  chunks, and the returned shard is registered so later positions match without another request. A matched chunk is
  never uploaded: the new shard points at an xorb somebody else already paid for.
* **Deliberately not copied:** the reference's `Defragmenter`/`allow_dedup_on_next_range` heuristics, which refuse
  matches that would fragment a file into too many segments. That is a read-performance tuning layer; taking every
  match is correct, and costs segments.
* **A returned shard is *keyed*, and the key is in its footer.** The first consumer of the above passed every
  offline check and reported `deduped_chunks = 0` against the live CAS while the CAS answered 200 to every query:
  the shard came back, parsed, registered — and matched nothing. Stored chunk hashes are not the file's chunk
  hashes. They are `blake3_keyed_hash(footer_key, hash)`, and xorb hashes are not (`72fe5a36…` in a CAS answer is
  the hash the uploader computed for that xorb). Read from two live responses, then reproduced offline:
  * the answer's header advertises `footer_size = 200` (an *uploaded* shard advertises 0 — a session rewrites that
    field to zero and appends nothing); footer version 1; `chunk_hash_hmac_key` is the footer's own 32 bytes at
    offset **72** of its 200, and the footer's last field is its own start offset, which has to add up.
  * a CAS answer holds xorbs and nothing else: `file_info_offset` 48 and `xorb_info_offset` 96 — the file section is
    one bookend — and all three lookup tables are present with **0 entries**. So the answer cannot be looked up by
    file, and a reader that needed a file entry to register a shard would register nothing at all.
  * **the byte-order trap**: a key *read out of the footer* means those 32 bytes; a key *parsed from a hex string*
    means the same bytes reversed inside every 8-byte word (`DataHash::from_hex` reads four LE words). The two
    agree only for a key that is palindromic per word, and `HFXetChunkHmac` takes `.bytes` — so a key that arrives
    by the wrong route produces a hash that is confidently wrong. `HFXetHmacVectors()` (64 pairs, generated from
    `DataHash::hmac`, first key all-zero, second all-`f`) pins the construction; `xet-probe hmac` produces them.
  * `MDBShardInfo::keyed_chunk_hash` applies the key only when it is non-zero, and `ShardFileManager` keeps one
    `KeyedShardCollection` **per distinct key**, answering from the first that resolves. `HFXetDedupTable` copies
    that: stored hashes are registered exactly as the shard spells them, only *queries* are keyed, and a zero key is
    the sentinel for "stored as given" (`HFXetKeyedChunkHash` owns that case, vector-tested both ways).
* **The fixture that makes this testable offline** is compiled in as `HFXetStoredShardVectors()`: one 300 000-byte
  file, six chunks, one xorb, exported by the reference's own `export_as_keyed_shard_streaming` (680 bytes, key
  `22…22`) through `xet-probe shard <max_xorb_bytes> <sha256> <key>`. It is the same shape as the live answers
  above — footer 200, bookend-only file section, empty lookup tables, keyed chunks — and it is fed to the parse,
  register and match checks that a session's own round trip can never reach, because a session uploads an unkeyed
  shard and reads back only what it wrote. Two details of the probe matter for reproducibility: it exports from the
  *full* local shard rather than the truncated upload (the export copies the input header verbatim, and an upload has
  `footer_size` zeroed — that would ship a shard carrying a footer it does not advertise), and it zeroes the
  footer's two validity timestamps, which come off the clock and would make every regeneration drift.

**What this buys, ranked by certainty.** (1) Re-uploading an *identical* file costs nothing — chunk 0 is indexed, one
query returns the whole file's shard, everything matches as one run, and the upload is a shard of pointers with zero
xorbs. (2) Copying a dataset between repositories is (1) per file, and partitioned writes are file-granular, so
re-running a partitioned `COPY` over unchanged partitions uploads nothing. (3) *Edited* files are reachable through
the 1-in-1024 sample: a 100 MB rewrite shares ~1 in 1024 of its chunks by luck of the hash — thin, but it is what the
reference has too, and it is what makes a shifted boundary discoverable at all.

**Order of work.** (A) ~~Flag eligible chunks in the shard writer~~ — *replaced*: the rule belongs to the reader
(`HFXetShardChunkView::GlobalDedup()` = persisted bit **or** rule), pinned by the byte vectors above. (B) the gate
above — **passed**. (C) the query and the match — **passed**: `HFXetDedupQuery`/`HFXetDedupTable` in
`src/hf_xet_dedup.cpp`, spaced 256 like the reference, keyed collections as above. (D) pointer segments — **passed**:
the upload is two passes now (pass A chunks and hashes the whole file, pass B re-reads only the ranges that still
need uploading), a matched run becomes an ordinary segment naming an xorb the shard does not carry, and
`hf_xet_upload()` reports `deduped_chunks`, `dedup_queries`, `dedup_hits` and `dedup_shard_failures` per file.
`hf_xet_upload_dedup` (BOOLEAN, default `true`) turns the queries off; the byte-identity suites run with it off,
because a deduplicated shard is deliberately not the same shard.

**Live proof, both shapes** (`scripts/smoke_xet_upload.sql`, p7 and p8). p7: 60 000 rows chunked and uploaded against
`hf://datasets/lhoestq/hf-smoke-staging` and never published, then the same bytes uploaded from
`hf://buckets/lhoestq/playground/duckdb` — one result row, `xorb IS NULL` for all of them, `deduped_chunks > 0`,
`dedup_shard_failures = 0`, a shard hash named, and `read_blob` over `hf://` returns the same md5 as the local file.
The Hub served a file whose content is entirely pointers into xorbs another repository posted, which is the claim the
format exists for. p8: that file with two rows appended — `deduped_chunks >= 1`, the sum of `uncompressed_bytes`
over the xorbs that *were* uploaded strictly less than the file's size, and the mixed shard reads back byte-identical.

**Reproducing the format facts.** `docs/fixtures/generate_dedup_probe.py` rebuilds the two 2026-10-18 payloads
byte-for-byte (sha256 `b9ac43b3…`, 238 180 B for `payload.csv`; `6ca2476f…`, 12 582 987 B for `big.txt`), and
`docs/fixtures/{payload_chunks_5,big_chunks_196}.txt` hold their chunk hashes in file order. They are still in
`lhoestq/hf-smoke-staging` under `dedup_probe/`. The gate payload is *not* pinned — it was random bytes, kept only as
the CAS's xorb above — so re-running means generating a new one and asking the same question:

```sh
head -c 67108864 /dev/urandom > /tmp/gate.bin     # ~1025 chunks; ~1000 gives one eligible chunk per file on average
python3 scripts/cas_dedup_probe.py --sample 12 datasets/lhoestq/hf-smoke-staging /tmp/gate.bin   # before uploading:
                                                                                               # all 404, verdict inconclusive
# then upload, and ask again:
#   SELECT * FROM hf_xet_upload('/tmp/gate.bin',
#     scope := 'hf://datasets/lhoestq/hf-smoke-staging/gate.bin');
```

## Next

1. **Measure what dedup saves on the copy case** — a dataset copied with one extra column, xorb bytes and wall
   clock with `hf_xet_upload_dedup` on and off. The 1-in-1024 sample is thin by construction; the number to know is
   how thin.
2. **Shard-driven read prefetch**: walk a shard's chunk table to decide what to fetch, instead of one range request
   per window — the fix for the sequential `read_blob` gap in the README's *Limitations*.
