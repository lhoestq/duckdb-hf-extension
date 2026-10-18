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
*(M8 vendored `lz4.c`/`lz4hc.c`/`lz4frame.c`/`xxhash.c` at v1.10.0 and shipped LZ4 frames; bg4 did not
ship, and the reasoning about byte-for-byte equality turned out to be the load-bearing part - see M8.)*
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
   **Answered before it was built**: free, and verified - `xet-probe xorb` writes the same file's xorb
   under four schemes and prints the same `xorb_hex` for all four (M8).
4. Exact `preupload` response shape for a bucket vs a git repo (`xet_file_data` fields per repo type).
5. Dedup: is a global dedup query required, or can a first implementation skip it (correct but
   no sharing)? `MIN_SPACING_BETWEEN_GLOBAL_DEDUP_QUERIES` suggests it is opportunistic.
   **Answered by measurement, not inference** (`GET /v1/chunks/…` against the live CAS, two uploads,
   196 + 5 chunks swept one query per chunk): optional, opportunistic, and the index is sparser than the
   name suggests — see *M9 (design)* at the end of this file for what the CAS returns and which chunks it
   answers for.

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
* **M5 — shards + publish.** Produce the session shard, upload it, then register the file by hash;
  first target: `COPY … TO 'hf://buckets/…'` (which is the whole point). **Done** - see *M5* below.
* **M6 — git-backed repos through Xet.** Datasets, models, spaces and kernels: the `metadata_ext` link
  between the LFS pointer's sha256 and the Xet file hash, then deciding when a repo write should prefer
  Xet over git-LFS (which is a cost question, not a correctness one: LFS works today for every type).
  **Done** - see *M6* at the bottom.

Each milestone lands behind a setting or an internal flag so 0.0.x behaviour never regresses. Git-backed
writes stayed on git-LFS through M5 deliberately: the Xet path earned its bytes on buckets first, where a
file *is* its Xet content and nothing else can go wrong.

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

## M3 xorb writer (done, for the uncompressed scheme)

`src/hf_xet_xorb.cpp` writes a complete serialized xorb: a chunk section of
`<8-byte header><chunk bytes>` per chunk, then the V1 footer - `XETBLOB` + version + xorb hash, the
`XBLBHSH` hash section, the `XBLBBND` section carrying both boundary lists (physical offsets with
headers included, uncompressed offsets without), `num_chunks` repeated three times in total, the two
section offsets, a 16-byte reserved buffer whose first 4 bytes carry the per-upload nonce, and the
footer's own length. Everything little-endian, hashes as raw bytes, the 3-byte lengths of the chunk
header as-is.

What the format demands that is easy to get quietly wrong:

* **The two section offsets count back from the end of the footer *struct*, which excludes the
  trailing length u32.** A reader takes the last 4 bytes, seeks back `4 + info_length`, and finds
  the first section; the second and third are found by those offsets. Off-by-four is a xorb that
  serializes fine and cannot be read back.
* **The nonce is excluded from the xorb hash on purpose.** The hash covers chunk contents only, so
  the reserved buffer is the one field a client can randomize to make two identical uploads
  distinguishable on the wire without changing what they are. The vectors pin the all-zero nonce;
  uploads will fill it in.
* **Physical and unpacked boundaries are different lists.** One is offsets in the serialized object
  (header included), the other is cumulative uncompressed lengths; both are `u32` per chunk and both
  end at the total of their own space. Swapping them produces a xorb that reads back with the wrong
  chunk sizes while hashing correctly.
* **The size limits are on uncompressed bytes (64 MiB) and chunk count (8192)**, not on serialized
  size, because the CAS addresses a xorb by the hash of its chunks and does not know our layout.
* The self-test follows the footer's own offsets and checks the section identifiers are where the
  footer claims, rather than re-deriving them - trusting the arithmetic that produced them would
  test nothing.

Compression is `None` for now, which is a legal scheme rather than a stub: the hash covers
uncompressed content, so an uncompressed xorb is as valid as a compressed one, and `CompressionScheme`
is refused in the constructor with a pointer here rather than silently writing bytes the Hub cannot
decode. LZ4 (scheme 1) needs the *frame* format - DuckDB vendors LZ4 block only - and byte-for-byte
equality with the reference's `lz4_flex` output is not required, since any valid frame decodes to the
same chunk and the hash is over the chunk; what is required is that the reference reader gets the
original bytes back. BG4 (scheme 2) is a byte-regrouping permutation on top of that. Both are
upload-size optimizations and can land after the write path works.

*(LZ4 landed in M8 and is the default; bg4 did not, and `Auto` is still refused rather than guessed at.
Everything above is what M8 was built on: the "what is required is that the reader gets the original
bytes back" sentence is the test that replaced byte-exactness as the contract.)*

## M4 the CAS client (done)

`src/hf_cas.cpp`. The CAS URL is not a constant and the token is not the HF token: every upload starts
with a write-token exchange against the repository, which answers with `{casUrl, exp, accessToken}`.
The route is derived from the url the user typed -
`{endpoint}/api/{repo_type}/{repo_id}/xet-write-token`, plus a `/{revision}` path segment for the
git-backed repo types and none for buckets. Buckets and datasets get the same CAS and the same upload
call; the token just says which repository the content belongs to.

* **The CAS request carries the CAS token and nothing else.** Attaching the HF `Authorization` header
  alongside it is refused, exactly as with presigned LFS PUTs, so `HFClient::Post` grew the same
  `with_auth` switch `Put` has.
* **`{cas}/v1/xorbs/{prefix}/{hash}` with `POST`** (not PUT - the reference posts), prefix `default`,
  hash the swizzled hex that Rust's `{:x}` prints for a `Key`. The answer is `{"was_inserted": bool}`.
* **There is no exists-check to call.** Content addressing *is* the deduplication: the url is the
  content, so re-posting is idempotent and `was_inserted: false` is how the CAS says "already had it".
  The reference's session-level chunk/xorb filtering exists to skip work, not to preserve correctness.
* **Retries**: 5 attempts, 3s base exponential, 6 minute total budget, retrying 5xx/408/429/timeout/
  connection errors and stopping on everything else - the reference's policy, transcribed. Uploads are
  sequential for now; concurrency is a throughput feature, not a correctness one.
* **Tokens are cached per client and refreshed 30s before expiry**, the reference's buffer. An `exp` we
  cannot read is treated as already expired - refreshing too often is dull, uploading against a dead
  token is not.

`hf_xet_upload(path [, scope := 'hf://...'])` exposes all of it as a table function, one row per xorb:
`xorb, num_chunks, bytes, uncompressed_bytes, inserted`. The `scope` names the repository whose token
the content is uploaded against, needed when the bytes are not in a repository at all - which is what a
`COPY TO` will look like: DuckDB fills a local temp file, the target repo supplies the scope. M5 builds
on exactly that shape.

What the production CAS proved about M3, unprompted: it recomputes the xorb hash from the bytes it
receives. A deliberately wrong hash came back as `400 BadRequest: xorb hash mismatch: expected fc79…,
got 4bf3…`. So every accepted upload is an independent confirmation that the serialization parses, from
the other side of the wire, on a machine running code we did not write.

Two bugs the first 70 MiB file found, both invisible below the xorb size limit:

* `Finish()` cleared the chunk list but not the byte counter, so the *second* xorb of a large file was
  born "full" and threw. A builder that has handed over its bytes now reports empty in every respect.
* Flushing on `IsFull()` - asking after the fact - let an object end up one chunk over the format's
  64 MiB. The builder now answers `WouldOverflow(len)` first, and the loop closes a xorb before the
  chunk that does not fit. The limits are on *uncompressed* bytes, which is why the function reports
  both byte counts: 1026 chunks / 67,076,687 uncompressed bytes is legal at a serialized 67,126,031.

## M5 the metadata shard, and a file at the end of it (done)

The shard is **not protobuf**. `xet_core_structures/src/metadata_shard/` hand-rolls little-endian
binary, which is a gift: the writer is 200 lines of appends and no dependency. What a session uploads is
*not* what `MDBShardInfo::serialize` writes either - it is that same byte stream **cut short at
`file_lookup_offset`, with the header's `footer_size` rewritten to 0**
(`read_shard_to_bytes_remove_footer`). Header, file section, file bookend, xorb section, xorb bookend.
No lookup tables, no footer: the CAS rebuilds the lookups, and the footer carries the timestamp - which
is why re-uploading identical content dedups instead of creating a new shard per upload.

The layout, in the order it is written (all little-endian, 32-byte hashes):

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

Three things worth writing down, each of which cost a debugging cycle:

* **Chunk indices in a file segment are xorb-local.** A reader resolves the xorb by hash and then slices
  *its* chunk list, so a segment that covers a whole xorb is `0..len` - not the chunk's position in the
  file. The CAS checks this on ingest: `400 (19) invalid shard failed to verify file <hash>`. A
  single-xorb file cannot tell the difference, which is why the first small uploads all worked.
* **The verification hash is a keyed blake3 over the concatenated *chunk hashes* of the segment**
  (`chunk_verification.rs`), i.e. a range over the *file's* chunk list. Same key, different
  coordinate system from the one above: the entry stores xorb-local indices, the hash covers file
  ranges, and the shard does not store the file range - so a writer has to carry both.
* **`xetHash` in the Hub's API is `MerkleHash::hex()`, the word-swapped hex** - the same rendering as
  the xorb hash. This was checked against the live Hub rather than reasoned out: the reference chunker
  and hasher reproduce `xet_probe.parquet` as
  `1b14d820168795c0a60ca8daa1b3a2c7161375f62883b6767fe9c5f10d444d9f`, which is exactly what the tree API
  reports as its `xetHash`.

`POST {cas}/v1/shards` answers `{"result": 0|1, "shard_hash": "..."}` (`0` = already present, `1` =
merged), with `shard_hash` in plain hex - and it is the hash of the shard *as the CAS re-serialized it*,
so it is a name the service chose, not something to compare a local hash against.

A bucket then takes the file by hash:

```
POST {endpoint}/api/buckets/{repo}/batch   Content-Type: application/x-ndjson
{"type":"addFile","path":"duckdb/x.parquet","xetHash":"<word-swapped file hash>","mtime":<ms>}
```

which is `COPY (SELECT ...) TO 'hf://buckets/...'` end to end: temp file → chunk → xorbs → shard →
`addFile` → readable at the url, verified byte-for-byte by `scripts/smoke_xet_upload.sql` including a
230 MB uncompressed file whose chunks land in several xorbs (the Hub assembles the read ranges from the
shard, so a segment bug shows up as a corrupt read rather than an error).

**M6 — git-backed repositories through Xet (done).** A dataset/model/space file is committed as an LFS
pointer: `{"key":"lfsFile","value":{"path":…, "algo":"sha256","oid":…, "size":…}}`. That pointer names the
content by sha256, and a Xet file is named by a BLAKE3 merkle root, so the two halves only describe the
same bytes if something states the relation. That something is the shard's file-level metadata extension:
`flags | WITH_METADATA_EXT` plus a 32-byte `sha256` in the `FileSequence` entry. `huggingface_hub` omits
it for buckets (`SKIP_SHA256` - a bucket file is addressed by its xet hash, so nothing has to be joined)
and sends it for everything else. That is the entire protocol difference: the commit this extension sends
for `hf://datasets/...` is *the same commit it always sent*, the bytes merely stop visiting the LFS
endpoint.

What the reference reading of it is worth, in three notes:

* **the sha256 in that extension is hex-parsed as a `DataHash`, which means four 16-hex-digit groups read
  as little-endian words and then word-swapped** - `Sha256Policy::from_hex` → `DataHash::from_hex`, the
  same convention as `MerkleHash::from_hex`. Parsing it as 32 ordinary byte pairs still writes 32 bytes,
  still produces a shard the CAS accepts, and still leaves a file that resolves to nothing. Only a
  reference vector notices (`scripts/xet_oracle.py` now emits two, one single-segment and one
  multi-segment, and `hf_xet_selftest()` additionally asserts the stored flags word, since flagging an
  extension that is not there — or writing one without flagging it — is invisible to a byte diff of a
  *correct* shard).
* **nothing on the read side is asked for**: `GET /api/{type}/{repo}/xet-write-token/{revision}` answers
  with a CAS url and token for datasets, models and spaces, including for a repository created with
  `"xetEnabled": false` (a probe repo, `lhoestq/hf-ext-nonxet-probe`, whose API info reports
  `xetEnabled: None`, still handed a token for `cas-server.xethub.hf.co`). The preupload route likewise
  answers `uploadMode: "lfs"` for a Xet-enabled staging repo. Neither is a gate: the *client* picks the
  route, exactly as `hf_xet` does, so there is no repo capability to preflight and no reason to wait for
  one.
* **fallback is the honest default.** `hf_xet_write` is on; if the CAS upload throws, the same file goes
  out through `UploadLFSBlob` and the commit is unchanged, so an unreachable content store makes a write
  slow rather than impossible. If the commit then fails anyway the message names the earlier Xet failure,
  because "the server rejected the commit" and "the CAS was already down and we sent LFS bytes instead"
  need different responses from a user. What is *not* covered by a test is the fallback firing: nothing in
  SQL can make a CAS upload fail, and a test-only seam in the upload path is not something a shipping
  extension should carry — so the deterministic coverage is the `hf_xet_write = false` route, and the
  fallback is covered by reading and by the fact that the LFS path is the one every earlier release used.

The live proof is deliberately made of bytes that could not have been served from anywhere else: 6 MB of
`/dev/urandom`, never uploaded as an LFS blob, written to a Xet-enabled staging repo (`hf_xet_write = true`),
committed, then read back with `read_blob` and compared by sha256 and size. The tree API does not expose a
usable `xetHash` for git-backed files, so "a commit accepted for content that exists in no LFS store, and
reads back identical" is the strongest signal the surface gives.

**Next**: the polish that stops being excusable — parallel xorb uploads, an upload-side dedup query
(`POST /v1/chunks/…` before hashing work we already paid for elsewhere), LZ4 frames, and a prefetch that
walks a shard's chunk table on read instead of issuing one range per window.

## M7 a file's xorbs on several streams (done)

M4-M6 made a write correct; how fast it was is a separate question, and the answer came from measuring
rather than from guessing which of the three remaining optimizations to take first. With
`scripts/xet_bench.sh` (the CPU phase alone, no network, 100 MB of incompressible bytes):

| phase | time |
|---|---|
| gear chunking | 31 ms (3.4 GB/s) |
| chunk + keyed blake3 per chunk + xorb serialize, NEON | 152 ms (690 MB/s) |
| same, portable BLAKE3 (all SIMD off) | 177-193 ms |
| the same upload, end to end | 12,000-70,000 ms, link-dependent |

about 1% of a big write's wall clock is CPU. So SIMD dispatch was worth committing (it is free, and it
makes the x86 paths' correctness a *tested* thing rather than an assumption - the CI self-test runs the 367
checks on an amd64 artifact, which nothing local can) and was not worth waiting for. What a user of
`hf_xet_write` actually waits on is a sequence of xorb POSTs, one per 64 MiB, each on its own connection
getting what one connection on this link gets. Measured on 200 MB of fresh random content, six runs,
concurrency alternated so link drift hits both arms:

| `hf_write_max_parallel_uploads` | seconds |
|---|---|
| 1 | 29.8, 57.9, 31.2 |
| 3 | 17.6, 20.9, 18.0 |
| 4 | 22.2, 19.4 |

median 31 s to 18 s, ~1.7x. Not 3x, because a 200 MB file is four xorbs of 64/64/64/8 MiB - the small
last one is exposed either way - and because three streams on a link that sheds are not three times a
stream. Concurrency 4 measured the same as 3, so 3 is the default: it is the point where the curve has
flattened, and it is the cheaper memory bill.

**What the parallelism is allowed to touch.** Content addressing is the whole license: an xorb's url is
its content hash, no xorb depends on another, and the order they land in is not observable. The metadata
shard *is* order-sensitive - it names segments and chunks in file order - so the split is: the thread
reading the file builds each xorb and writes down everything the shard will say about it, in file order,
and hands the bytes over; only the CAS's `inserted` answer comes back the other way, and it is collected
once every POST has landed, before the shard is serialized. A shard that described xorbs in the order the
network finished them would be describing a different file.

**In flight is the memory bound, and the backpressure bound, with the same number.** A xorb waiting to go
is up to 64 MiB of serialized bytes held in memory, so `Submit` blocks while three are running. That is
also the entire reason the reader and chunker cannot run arbitrarily far ahead of the network: a 10 GB
file costs the same memory as a 100 MB one.

**The thread-affine part.** A worker gets a CAS client without a `FileOpener` (`ForWorkerThread`). A
`ClientContext`-backed opener reads settings through the client it belongs to, so two threads using one is
not a thing to discover through a bug report; the read path already made this choice for its parallel range
fetches (`FetchWindow` in `hf_read.cpp`), including the fallback of running the work inline when a thread
fails to start. What that costs is transport parameters (retries, user agent, headers) resolved from
`DBConfig` instead of the session - for one POST per xorb, to a service that inspects none of them.
Authentication is unaffected: the CAS token comes from the resolved `HFConnection`, and the exchange that
gets it is deliberately done on the main thread, before any worker exists.

**The shared token.** Several threads share one client, so the token is refreshed under a mutex and
*copied out* (`Authorize(cas_url, access_token)`) rather than returned by reference - the old `CasUrl()`
handed back a `const string&` into state that another thread's refresh could be rewriting mid-header. A
token that expires during an upload is refreshed by whichever worker notices, from the connection alone,
which is the one thing the dropped opener was not needed for.

**Failure**: the first error any upload had is kept and thrown from `Finish()`; `Submit` stops feeding
once one exists, since the shard that would reference this file is not going to be posted anyway; and the
destructor joins unconditionally, because the producer can throw mid-file (a read error, an allocation) and
a worker holding a reference to the uploader has to be gone before it dies. Whatever such a worker thought
is not the interesting error - the caller is already reporting something better.

**How it is tested** is worth stating, because the obvious test (a big upload, timed) is not a test:
`smoke_xet_upload.sql` p6 re-uploads the suite's multi-xorb file with concurrency forced to 1, after the
default of 3 wrote it, and requires every xorb to answer `inserted = false` and the shard to answer
`exists`. Xorb identities are content hashes, so a concurrency bug that moved a boundary, reordered a chunk
into a different xorb, or booked a segment against the wrong object would produce hashes the CAS has never
seen - and it would answer `true`. It costs no new storage, which is why it can live in a live suite. What
stays uncovered is a failure *during* a concurrent upload: as with the fallback, nothing in SQL makes one
happen, and the seam required to try is worse than the gap.

**Next**: LZ4 frames (fewer bytes on the wire, which is the thing that measured slow, and provable offline
as a byte count), an upload-side dedup query (`POST /v1/chunks/…` before hashing work another repository
already paid for), and a read prefetch that walks a shard's chunk table instead of issuing one range per
window.

## M8 LZ4 chunk compression (done)

M7 put three xorbs on the wire at once. The other lever on the same measurement is making each xorb
smaller, and it turned out to be the bigger one - so it was measured offline before a line of the write
path changed. `tools/xet_probe` (`xet-probe xorb <scheme> <out>`, xet-core's own writer) over the same
2,103,723 byte CSV the smoke suites write:

| scheme | xorb bytes | |
|---|---|---|
| `none` | 2,105,403 | 8 bytes of chunk header per chunk, nothing else |
| `lz4` | 881,602 | 0.42x the content |
| `bg4` | 1,148,898 | *worse* than plain lz4 on this data |
| `auto` | 881,602 | auto resolved to lz4, which is what it does for tabular data |

`xorb_hex` was `9f7738bc…b16489` in all four rows. That single fact is what the milestone rests on: an
xorb's hash is the merkle root of the **uncompressed** chunk hashes, so the codec is a decision about
bytes on the wire and not about identity. Compressing cannot make content that a repository already holds
look like new content, and it cannot make our objects unreadable to a client that picked a different
codec. Everything below follows from that being true rather than from hoping it is.

**What the reference does, mirrored rather than improved on.** `Auto` never resolves to `None`: reference
clients pick lz4 (or bg4-lz4 with `bg4_split` + the K-Lipschitz predictor) and compress *every* chunk,
including the ones that come out bigger than they went in. There is no per-chunk "skip it if it doesn't
pay" here, because the cost of not skipping is small and measured - an incompressible chunk is stored as a
raw LZ4 block inside a frame and costs about 16 bytes of framing, **1.001x** over 5 MB of `/dev/urandom` -
and a clever-looking divergence would make our xorbs a shape nobody else's are, for a rounding error. bg4
is not implemented: it needs `bg4_split` and the predictor, and it lost to plain lz4 on the data that made
it here. `Auto` remains refused in the constructor rather than guessed at.

**Frames, and what is *not* pinned.** A chunk payload is an LZ4 frame (`lz4_flex::frame` on the reference
side), so `lz4frame.c` is vendored (v1.10.0, BSD-2) rather than hand-rolled. Two conforming LZ4 encoders do
not produce the same frame bytes - descriptors, block sizes and checksum choices differ - and ours is not
the reference's: 889,917 bytes where `lz4_flex` wrote 881,602 (+0.9%) for the CSV, and 5,247,715 where it
wrote 5,246,480 (+0.02%) for random bytes. Those differences are not the contract, and pinning them in a
self-test would have been pinning lz4 version details that are free to change. The contract is that a
chunk header's scheme byte plus its compressed length let a reader decode that chunk on its own, and that
the hash covers the bytes before compression.

**So decodability is what gets checked**, in four places, each answering a different doubt:

* `AddChunk` decompresses what it just compressed and compares it, byte for byte at the exact length, with
  the bytes it hashed - before the xorb can be posted. An uploaded xorb cannot be edited, and it is
  addressed by a hash of content that looks perfectly fine while a broken payload makes it unreadable
  forever; the check costs milliseconds against the network round trip that follows it.
* `hf_xet_selftest()` (325 checks before this, **367** after) rebuilds every pinned input vector with LZ4
  and requires the *chunk and xorb hashes* to equal the reference-pinned ones for that vector, the chunk
  count to be unchanged, the unpacked lengths to still sum to the input size, the chunk section to end
  exactly where the footer starts, and the footer to still be findable by its trailing length. Frame bytes
  are deliberately absent from that list.
* the reference's own reader gets our files: `scripts/xet_bench.sh <file> lz4 <dir>` leaves serialized
  xorbs on disk, and `xet-probe xorb-read` over one of them - xet-core deserializing, recomputing every
  chunk hash - reports `num_chunks=33`, `uncompressed=2103723` and footer hash `9f7738bc…b16489`, which is
  the hash the reference writer's own xorb for that file carries.
* live, in `smoke_xet_upload.sql`: a file is written with the default (lz4) and then uploaded again with
  `hf_xet_compression='none'`, and every xorb of the second pass must come back `inserted = false`. The
  CAS holding content it has never seen by url but recognizing it by hash is the strongest available
  statement that compression is not part of identity - and it is the service making it, not us. The same
  block keeps the old "serialized is larger than content, by the framing" assertion alive, where it is
  true; with lz4 on, the suite asserts the opposite direction (`sum(bytes) < sum(uncompressed_bytes)`).

**The shard did not move**, which is worth saying because it looked like it might.
`chunk_byte_range_start` is cumulative **unpacked** length: xet-core advances segment bookkeeping by
`unpacked_segment_bytes` and `memory_client` accumulates uncompressed sizes. A reader asking for a byte
range of a file asks in the space compression happens underneath, so LZ4 changed xorb serialization and
nothing else.

**The build**, because two C libraries in one process is where this nearly went wrong: `lz4frame` computes
frame checksums with xxhash, and DuckDB links xxhash and its own namespaced LZ4 into the same process as
this extension. `XXH_NAMESPACE=LZ4_` renames ours (`LZ4_XXH32`), and `-fvisibility=hidden` plus empty
`LZ4LIB_VISIBILITY`/`LZ4FLIB_VISIBILITY` - lz4's headers stamp `visibility("default")` on their own
declarations unless those macros are predefined - leave **zero** `LZ4_*`/`XXH*` symbols exported from the
extension. DuckDB's copies are inside C++ namespaces, so there was no live collision to fix; this is the
hygiene that keeps it that way for the next extension loaded into the same process. `hf_library` keeps its
own default visibility: that is the target holding the entry points.

**Cost**, measured with the same bench that justified M7: the CPU phase of a write was ~690 MB/s
uncompressed and is ~370 MB/s on compressible CSV / ~520 MB/s on random bytes. It was about 1% of a large
upload's wall clock and is now about 2%, buying 0.42x on the bytes the other 98% waits for. `hf_xet_compression`
(default `lz4`, accepts `none`) is the switch; `none` reproduces pre-M8 bytes exactly.

The setting is validated when it is set, not when a write finally reads it: `SET hf_xet_compression = 'bg4'`,
a scheme the format defines and this build does not implement, fails there, and `scripts/smoke_xet.sql`
pins those refusals next to the case-insensitive `'LZ4'` and the `RESET`. `'auto'` is deliberately not
taken as a synonym for `lz4`: in the reference client it means a per-chunk decision, which this writer
does not take, so accepting it would promise a behaviour that never happens. `ChunkCompression` still
checks the value where it is consumed - the setting can also arrive from a config file, and the last place
a value is read is the last place it can be wrong.

**Next**: cross-repository dedup on the upload side (M9 below — and note that the endpoint is a per-chunk
`GET`, not the `POST /v1/chunks/…` batch this section guessed before anyone had read the CAS spec), and a
read prefetch that walks a shard's chunk table instead of issuing one range per window.

## M9 (design, measured first) cross-repository dedup

Everything in this section was established by asking the live CAS, on 2026-10-18, rather than by reading
field names out of xet-core. Two files were written through the extension into `lhoestq/hf-smoke-staging`
(`dedup_probe/payload.csv`, 5 chunks; `dedup_probe/big.txt`, 196 chunks), every one of their chunk hashes
was computed with `xet-probe xorb`, and each hash was then queried against the CAS.

**The endpoint.** `GET {cas}/v1/chunks/{prefix}/{chunk_hash_hex}`, bearer-authenticated with the same
write token an upload uses. The token comes from the usual exchange — and the revision is a *path segment*,
which matters: `api/datasets/<repo>/xet-write-token` answers 404 while
`api/datasets/<repo>/xet-write-token/main` answers with `casUrl` / `accessToken` / `exp`. `prefix` is the
same `default` used for xorb uploads, and the hash is the same word-swapped hex the xorb URL uses.

**What a hit is.** 200 with a body that is a **complete metadata shard** — same `HFRepoMetaData` header,
same 48-byte-entry layout, the format `hf_xet_shard.cpp` already parses. For the 5-chunk file our uploaded
shard was 576 bytes and the answer was 632; for the 196-chunk file, 9744 against 9800. Both exactly +56, so
the CAS hands back the shard of the file that contained the chunk, re-serialised with a little more than the
stripped upload we sent. A miss is a 404 with a 53-byte body.

**Which chunks the index holds — the part that was genuinely surprising.** 1 hit in 196, and 1 in 5, and in
both files the hit was **chunk 1**. Not a sampling by hash, not a range index: the CAS indexes the chunks a
shard *flags*, and our writer flags exactly one chunk per file. `hf_xet_shard.hpp` has

```cpp
//! XorbChunkSequenceEntry.flags, set on a chunk that starts a file. Advisory, and cheap to honour:
//! the CAS uses it to populate a cross-repository dedup table.
constexpr uint32_t HF_XET_CHUNK_FLAG_GLOBAL_DEDUP = 0x80000000u;
```

which is `MDB_CHUNK_WITH_GLOBAL_DEDUP_FLAG` (`1 << 31`), applied at `hf_xet_shard.cpp:176` to
`chunk.starts_a_file`. The producer half of global dedup is therefore already in this extension, added in
M5 on the strength of a comment in `xorb_structs.rs`; what is missing is that a *reference* client flags far
more than the file's first chunk, and that nothing here ever reads the table back.

**The reference's own eligibility rule** (`metadata_shard/constants.rs`):

```rust
pub fn hash_is_global_dedup_eligible(h: &MerkleHash) -> bool { (*h) % 1024 == 0 }   // MDB_SHARD_GLOBAL_DEDUP_CHUNK_MODULUS
```

and `Rem<u64> for DataHash` is `self[3].to_le() % rhs` — the fourth `u64` of the digest, little-endian, mod
1024. So eligibility is a 1-in-1024 sample by hash, *or* whatever a client explicitly flags. Checked against
both files: none of the 201 chunk hashes is eligible under that rule, which is why 196 queries produced one
answer and why that one answer was the flagged chunk. (Worth stating because the arithmetic is easy to get
wrong: with the digest read in the *printed* word order the residues of chunk 1 are 158 and 936, neither of
them zero, and it is the flagged-ness, not the modulus, that put it in the index.)

**How the reference client uses it** (`xet_data/src/processing/`): `global_dedup_query_enabled` defaults to
`true`, queries are issued at most once per `MIN_SPACING_BETWEEN_GLOBAL_DEDUP_QUERIES = 256` chunks, the
returned shard is registered in a cache shard manager, and `chunk_hash_dedup_query` then matches *runs* of
chunk hashes against three tiers in order — resumed session, current session, cached global shards — and
answers with a `FileDataSequenceEntry`, which is a pointer: N chunks live in xorb X at offset Y. A chunk
matched that way is never uploaded; the new shard simply points at an xorb somebody else already paid for.

**What that buys here, ranked by how sure the win is.**
1. **Re-uploading an identical file costs nothing.** Its chunk 1 is in the index (we flag it), so one query
   returns the whole file's shard, the whole file matches as one run, and the upload is a shard of pointers
   with zero xorbs. This is the case that works *today*, with no change to the producer half.
2. **Copying a dataset between repositories** is (1) repeated per file, and partitioned writes are file
   granular, so a re-run of a partitioned `COPY` over unchanged partitions uploads nothing.
3. **Edited files** need the modulus sampling to be worth anything: flag eligible chunks on the way out and
   the index stops being one entry per file, which is what makes a 90%-similar file discoverable at all.
   This is a change to the writer, and it changes what *other* clients can dedup against us as well.

**Order of work.** (A) Flag chunks that satisfy `word[3] % 1024 == 0` in the shard writer — small, pure, and
unit-testable against `xet-probe`'s hashes with the self-test the way M1/M2 vectors are. (B) Re-run the
sweep over the same 196 chunks and confirm the CAS now answers for the eligible ones; that is the experiment
that says the flag is honoured for chunks other than file starts, and it costs one upload and 196 GETs.
(C) Query while chunking (a sample, spaced like the reference), parse the returned shards with the existing
parser, match the longest runs. (D) Emit pointer entries for matched runs, and measure: the number that
matters is `bytes` in `hf_xet_upload` against `uncompressed_bytes` for a dataset copied with one column
appended.

**(A) and (B) are the gate.** If the CAS ignores the flag on a chunk that does not satisfy its own modulus
rule, then the honest summary is that cross-repository dedup is a first-chunk-per-file affair for now, and
step (C) shrinks to "query the first chunk, and dedup whole identical files" — which is still step 1 above,
and still worth having.

**Reproducing it.** The payloads are seeded generators, not blobs: `docs/fixtures/generate_dedup_probe.py`
rebuilds both byte-for-byte (`sha256` `b9ac43b3…` for `payload.csv` at 238,180 bytes, `6ca2476f…` for
`big.txt` at 12,582,987), and `docs/fixtures/{payload_chunks_5,big_chunks_196}.txt` hold the chunk hashes in
file order exactly as `xet-probe xorb` printed them. The files themselves are still in
`lhoestq/hf-smoke-staging` under `dedup_probe/`, so the sweep is: exchange a write token with the revision
appended, then loop `GET {cas}/v1/chunks/default/$(cat big_chunks_196.txt)`. Chunk 1
(`bae174d08d38265783468cb3068408dab5ad6d087d939c0f643806936c3c2d34`) answers 9800 bytes; the other 195
answer 404.
