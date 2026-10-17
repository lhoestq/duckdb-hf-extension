# Xet: what 0.0.2 has to build

Source of truth is `xet-core` (`https://github.com/huggingface/xet-core`), cloned at
`../xet-core`. Everything below was read out of that tree at `e922f74`; line numbers are not quoted
because they move, symbol names are.

## Why bother

Xet uploads unlock exactly the two things 0.0.1 documents as impossible:

* `COPY (…) TO 'hf://buckets/…'` — the bucket batch API's `addFile` demands a `xetHash`, and a
  bucket has no other upload route (probed: `sha256`, no hash, inline `contents` all refused, and
  there is no plain `PUT …/resolve/…`). With client-side chunking DuckDB can produce that hash.
* Cheaper, deduplicated uploads generally: a re-upload of a chunk the Hub already has costs nothing.

It is *not* needed for git-LFS writes to model/dataset/space/kernel repos, which is what 0.0.1
already does — so this is additive, and the existing write path stays as the fallback.

## The hash chain (the part that must be byte-exact)

**BLAKE3 keyed hash**, not SHA-256 (`xet_core_structures/src/merklehash/data_hash.rs`):

```
chunk hash   = blake3_keyed(DATA_KEY,           chunk_bytes)
internal hash= blake3_keyed(INTERNAL_NODE_HASH, "hex(size-1) : size\n" lines concatenated)

DATA_KEY           = 102 151 245 119  91 149  80 222  49  53 203 172 165 151  24  28
                     157 228  33  16 155 235  43  88 180 208 176  75 147 173 242  41
INTERNAL_NODE_HASH =   1 126 197 199 165  71  41 150 253 148 102 102 180 138   2 230
                      93 221  83 111  55 199 109 210 248  99  82 230  74  83 113  63
```

A hash is 32 bytes, held as `[u64; 4]`. Two renderings, and the hex one has a trap:

* `hex()` prints `word[i].to_le()` as 16 lowercase hex digits, i.e. **each 8-byte group is
  byte-swapped** relative to the raw digest. Copy the unit test
  `test_hash_hex_string_endianness` as a vector, do not eyeball it.
* `base64()` is URL-safe base64, no padding, over the raw 32 bytes (this is what the bucket/CAS
  APIs put in `xetHash` query strings).

Everything else is built from those two functions:

* **file hash** = the merkle root of the file's `(chunk_hash, size)` list (`file_hash`).
* **xorb hash** = the merkle root of the chunks inside one xorb (`xorb_hash`) — this is what
  `addFile`/`copyFile` names for bucket content.
* **range hash** = `range_hash_from_chunks` over a subsequence, used by reconstruction.

### The merkle tree ("mean tree"), `aggregated_hashes.rs`

Branching factor 4, and the shape is derived from the hashes themselves:

* a node is a *natural cut* when `hash[3].to_le() % 4 == 0`;
* a group (parent's children) has **at least 2 and at most 9** entries — the scan for a cut starts
  at the 3rd entry and stops at the 9th (`next_merge_cut`);
* a parent = `compute_internal_node_hash` over its children rendered as
  `"{child_hex} : {child_size}\n"` (hex = the 4-word `to_le()` rendering above, size in decimal,
  one line per child, no separator beyond the newline).

That is the whole tree rule. It is small enough to port directly and small enough to unit test
against the reference.

## Chunking (`xet_data/src/deduplication/chunking.rs`)

Content-defined with a **Gear hash** (`gearhash` crate 0.1.2 — 12 KB, MIT/Apache, downloadable from
crates.io, so its 256-entry table can be ported exactly):

* `mask = (target - 1)`, then shifted left by `mask.leading_zeros()` — the comparison is on the
  **high** bits, because the Gear hash's low bits are weak;
* target `64 KiB`, min `target / 8 = 8 KiB`, max `target * 2 = 128 KiB`
  (`xet_core_structures/src/xorb_object/constants.rs`; those are
  `test_configurable_constants!`, i.e. the published defaults);
* a boundary is where `hash & mask == 0` (via `gearhash::Hasher::next_match`), never below min,
  forced at max; the last partial chunk is flushed with `finish()`.

Chunk sizes are a *dedup-efficiency* choice, not a correctness contract: the client's hashes are
authoritative, so DuckDB could pick its own sizes. But matching `hf_xet` byte-for-byte is what makes
DuckDB-written content dedup against content the Python client wrote, so the plan is to match.

Xorb limits: `MAX_XORB_BYTES = 64 MiB`, `MAX_XORB_CHUNKS = 8192`, `XORB_BLOCK_SIZE = 64 MiB`.

## Xorb object format (`xorb_object/`)

Magic `XETBLOB`, versioned footer with side sections `XBLBHSH` (chunk hashes) and `XBLBBND`
(chunk boundaries/unpacked lengths); the trailing 16-byte footer buffer holds a 4-byte random
**nonce** and is *excluded* from the xorb hash — which is what makes re-uploading identical content
produce a distinct object. Chunk payloads are compressed with `CompressionScheme`:
`None = 0`, `LZ4 = 1`, `ByteGrouping4LZ4 = 2` (LZ4 frame + a 4-byte-group regroup transform),
`Auto = 99`. DuckDB already vendors the LZ4 *block* codec
(`third_party/lz4/lz4.{hpp,cpp}`, namespace `duckdb_lz4`, `LZ4_compress_default` /
`LZ4_decompress_safe`) but **not** `lz4frame`, and `lz4_flex`'s frame encoder means Xet payloads are
LZ4 *frames* - so either vendor `lz4frame.c` (BSD, self-contained) or write the frame container by
hand around the block API (magic `0x6422150C`, FLG/HC header, block sizes, end mark: short and fully
specified). `bg4` is a small byte shuffle worth porting since Auto may pick it.
`xorb_chunk_format.rs` (`serialize_chunk`, `write_chunk_header`) and `xorb_object_format.rs`
(footer construction) are the two files to translate.

## Upload protocol (`xet_client/src/cas_client/remote_client.rs`)

The CAS is a separate HTTP service; the Hub tells the client where it is. Endpoints as built by the
reference client:

```
GET  {cas}/v1/chunks/{key}                      dedup query for a chunk prefix
GET  {cas}/{api}/reconstructions/{file_hash}    read path: which xorb ranges make the file
GET  {cas}/v1/reconstructions?…                 multi-file variant
PUT  {cas}/v1/xorbs/{key}                       upload a xorb (key is a namespace prefix)
POST {cas}/v1/shards  /  /v2/shards             upload session metadata (file -> xorb/chunk map)
GET  {cas}/v2/file-chunk-hashes/{file_hash}     ask for a file's chunk list
```

Auth: the Hub's `preupload` response carries Xet connection info — `refresh_route` plus tokens — and
the client exchanges the Hub token for a short-lived CAS token by POSTing the refresh route
(`DirectRefreshRouteTokenRefresher` in `cas_client/auth.rs`). In `huggingface_hub` the same fields are
`xet_file_data.refresh_route` / `file_hash`, which is a convenient cross-check that our `preupload`
JSON already contains what a Xet upload needs (0.0.1 ignores that part of the response).

An upload is **xorbs + a shard**: nothing is visible to the Hub until the session's shard is uploaded
(`file_upload_session.rs` — "no session shard data gets uploaded until all the xorbs have been
successfully uploaded"). Shard format is the second big port after the xorb format
(`metadata_shard/`, `shard_format.rs`), and it has its own compression + an optional `metadata_ext`
(SHA-256 policy).

## What is still unknown (resolve in this order)

1. Can `addFile` on a bucket succeed with only a `xetHash` for content the CAS does not know yet, or
   must the xorb + shard be uploaded first? (Almost certainly the latter — decide the upload scope
   early, it sizes everything else.)
2. Which shard version does the Hub accept for *new* writes today (`v1` vs `v2`), and is
   `metadata_ext` required for repo types other than buckets?
3. Does `Auto` compression have to match `hf_xet`'s choice for the resulting xorb hash to match the
   reference client byte-for-byte, or is per-chunk scheme free (it should be free: the hash covers
   *uncompressed* chunk data — verify against `xorb_hash`, which takes `(hash, size)` pairs).
4. Exact `preupload` response shape for a bucket vs a git repo (`xet_file_data` fields per repo type).
5. Dedup: is a global dedup query required, or can a first implementation skip it (correct but
   no sharing)? `MIN_SPACING_BETWEEN_GLOBAL_DEDUP_QUERIES` suggests it is opportunistic.

## Implementation options

**A. Port to C++ inside the extension** (recommended).
Vendored deps: BLAKE3's reference C implementation (public domain, no deps — note 0.0.1 already ships
its own SHA-256 for LFS, which stays), the `gearhash` table + matcher, LZ4 (already in DuckDB's
third_party, or the same frame format via a small writer). Then `hf_xet_chunker.{hpp,cpp}` (CDC),
`hf_xet_hash.{hpp,cpp}` (leaf/internal/file/xorb), `hf_xorb_writer.{hpp,cpp}`, `hf_cas.{hpp,cpp}`
(refresh route, xorb PUT, shard POST). Everything streams off the write handle's temp file, which the
write path already creates.

*For:* no new toolchain in CI, one binary, works on every platform DuckDB builds for, and the
extension's "no heavy deps" rule survives.
*Against:* ~2-3k lines of new C++ (xorb + shard format), and hash/format bugs are silent.

**B. Rust staticlib FFI to `xet_data`.**
*For:* the format comes free and is certainly correct; `cargo` exists here (1.99).
*Against:* the extension build now needs a Rust toolchain for all six CI platforms, static-linking
`xet_data` means inheriting tokio + reqwest + its TLS story, and DuckDB's extension loader has no
business carrying an async runtime.

**C. Hybrid: port chunking + hashing (A) and stop at the hash.** Enough for
`addFile`-style paths only if the content already lives in the CAS, which is the case
`hf_bucket_copy` already handles. Not a shortcut to real uploads, but it *is* a useful first
milestone for verification.

Recommendation: **A, milestone-gated, with the reference implementation as an oracle.** `cargo` is
available locally, and `xet-core` ships exactly the examples this needs — `xet_data/examples/chunk`
(chunk boundaries), `examples/hash` (`chunk` / `xorb` / `file` / `range` hashes from a
`<hash> <length>` list), `examples/xorb-check` (validate a xorb file). Every C++ stage gets compared
against those on random + real (parquet, csv, binary) inputs in CI-able scripts.

## Milestones

* **M1 — hashing.** BLAKE3 keyed hash, the two keys, hex/base64 renderings, internal-node line
  format, mean-tree (`next_merge_cut` + group limits), file/xorb/range hash. Tests: the endianness
  vector, plus `examples/hash` agreement on generated chunk lists.
* **M2 — chunking.** Gear hash + mask, min/target/max, `finish()`; chunk-boundary and per-chunk-hash
  lists identical to `examples/chunk` output on the same inputs.
* **M3 — xorb writer.** Chunk serialization, compression schemes (None + LZ4 at minimum, BG4 after),
  footer with `XBLBHSH`/`XBLBBND` and nonce; `examples/xorb-check` must accept the output, and
  `xorb_hash` over our chunk list must equal the hash the file implies.
* **M4 — CAS client.** refresh-route exchange, `PUT /v1/xorbs/{key}`, exists-check, retry semantics —
  behind a `hf_xet_upload_debug` switch, and tested against the in-tree simulation server
  (`xet_client/src/cas_client/simulation`) before touching the production Hub.
* **M5 — shards + commit.** Produce the session shard, upload it, then commit with the file hash;
  first target: `COPY … TO 'hf://buckets/…'` (which is the whole point), then optionally make
  repo writes prefer Xet when `preupload` offers it.

Each milestone lands behind a setting or an internal flag so 0.0.x behaviour never regresses, and the
git-LFS path stays the default until M5 is proven.

## Where M1 and M2 stand (verified, not assumed)

`src/hf_xet_hash.cpp` and `src/hf_xet_chunk.cpp` implement M1 and M2; `hf_xet_selftest()` (152 checks,
`scripts/smoke_xet.sh`) replays reference vectors inside the extension. What that caught, and what is
worth knowing before touching this code:

* **The keys are load-bearing and unvisibly so.** Transcribing one of the two key constants wrongly made
  every hash wrong and every hash *plausible* - nothing downstream notices until the Hub rejects an
  upload. The constants now live in one place, and `scripts/xet_oracle.py` regenerates the vectors from
  xet-core rather than from anything a human typed.
* **The mask uses the high bits of the gear hash.** `(target - 1) << (target - 1).leading_zeros()`, i.e.
  `0xFFFF000000000000` for the 64 KiB target. Derived from the low bits instead, the chunker cuts roughly
  every other byte - and *still reproduces the reference boundaries on constant and tiny inputs*, because
  the minimum-chunk floor dominates there. Only content with real gear triggers (the splitmix vectors)
  tells the two apart, which is why the vectors include chunked inputs rather than just small ones.
* **`hex()` is not the hex of the digest bytes.** Four little-endian words, each printed as 16 digits, so
  every 8-byte group is byte-reversed. `base64()` is over the raw bytes, URL-safe and unpadded. A port can
  get the hashes right and the rendering wrong and never see it locally.
* **An empty chunk list hashes to the zero hash with no salt applied** - `file_hash_with_salt` returns
  `MerkleHash::default()` before keying, so "empty file" is not "keyed hash of a zero root".
* Chunk boundaries must be independent of how the bytes arrive. The self-test feeds the same inputs
  through in deliberately awkward piece sizes (1, 4, 19, …) and requires identical chunk lists, because
  the write path will feed the chunker from a file cursor, not from a buffer.

The vectors are generated and committed (`src/hf_xet_vectors.cpp`, `scripts/xet_oracle.py`), so CI needs
no cargo, and `scripts/xet_oracle.py --check` is what notices when a xet-core upgrade changes the format
under the port. Inputs are reproducible specs - literal bytes, a repeated pattern, splitmix64 - so a
vector says the same thing on every machine and nothing has to be re-derived by hand.

## M3 checklist (next)

* Chunk serialization inside a xorb, and the compression schemes: `None = 0`, `LZ4 = 1`,
  `ByteGrouping4LZ4 = 2`, `Auto = 99`. DuckDB vendors LZ4 *block* (`third_party/lz4`) but not lz4frame,
  so decide between vendoring the frame layer and writing the header by hand.
* Footer sections `XETBLOB` / `XBLBHSH` / `XBLBBND`, with the nonce excluded from the hash.
* Limits that bound a single upload: `MAX_XORB_BYTES` 64 MiB, `MAX_XORB_CHUNKS` 8192,
  `XORB_BLOCK_SIZE` 64 MiB.
* `examples/xorb-check` has to accept what the port writes, and `xorb_hash` over the port's chunk list
  must equal the hash the resulting file implies.
