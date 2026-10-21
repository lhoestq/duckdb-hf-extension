# Editing a Parquet footer in place on Xet — investigated, not implemented

**Verdict: nothing was built.** A Parquet key-value-metadata edit is a byte edit of the last few KB of a file, and
the idea was to pay only for that: range-read the footer, patch the Thrift `FileMetaData`, upload the new footer
chunk, write a new shard that points at the untouched prefix, repoint the bucket path. The blocker is not the
footer and not the Merkle root — it is the shard's **verification section**, which the CAS will only vouch for
*whole segments*. So a client that edits anywhere inside a segment must hold the bytes of that segment, re-chunk
them and re-hash them. A segment is a run of a file's chunks inside one xorb and is bounded by the xorb limits
(64 MiB uncompressed), and for a file that fits in one xorb the segment **is** the file. On the cases where a
footer edit is most attractive — a normal tens-of-MB Parquet file — the cheapest correct edit costs a full download
and a full re-upload, which is what a plain rewrite already costs, so the feature would buy nothing.

Reference implementation checked: `/home/quentin/hf/xet-core` at `e922f742` ("Add xorb upload grant types", #998).
Paths below are relative to that checkout.

## What a new file hash actually costs

Two independent things have to be reproduced for the edited file, and they have very different costs.

**1. The file hash (Merkle root over chunk hashes) — solved server-side, cheap.**
`GET {cas}/v2/file-chunk-hashes/{file_hash}` with the nonstandard header `X-Range-Dirty: bytes=A-B,C-D`
(`xet_client/src/cas_client/remote_client.rs::get_file_chunk_hashes`) answers with a
`FileChunkHashesResponse` (`xet_client/src/cas_types/mod.rs`):

```rust
pub windows: Vec<ChunkWindow>,                 // what must be re-chunked, chunk-aligned
pub hash_ranges: Vec<Option<MerkleHashSubtree>>,  // N+1 pre-aggregated subtrees for the gaps
pub gap_verification: Vec<HexMerkleHash>,      // one range hash per *stable* segment
```

`hash_ranges` are ready-made `MerkleHashSubtree`s (`xet_core_structures/src/merklehash/merkle_hash_subtree.rs`)
for every run of chunks the edit does not touch. The client builds a subtree for its new chunks with
`MerkleHashSubtree::from_chunks`, merges `[gap0, w0, gap1, …]`, and takes `final_hash().hmac(zero_salt)` — see
`xet_data/src/processing/range_upload.rs`, the `merge_seq` block. **The untouched bytes are never needed for
hashing.** This part of the design worked as hoped.

**2. The shard's verification entries — this is what forces the download.**
Each segment of a file's metadata shard carries a `FileVerificationEntry`, computed by
`xet_core_structures/src/metadata_shard/chunk_verification.rs`:

```rust
pub fn range_hash_from_chunks(chunks: &[MerkleHash]) -> MerkleHash {
    let combined = chunks.iter().flat_map(|h| h.as_bytes().to_vec());
    blake3::keyed_hash(&VERIFICATION_KEY, combined)   // the *unkeyed* chunk hashes, concatenated
}
```

so a segment's verification hash needs the unkeyed hash of every chunk in that segment's file-wide range. The CAS
will hand those hashes back for the segments that stay out of the way — `gap_verification` is documented as "one
range hash per **stable original segment** (= a segment that lies in a gap between dirty windows)", and
`build_file_chunk_hashes_response` implements it as "Emit one range hash per stable segment (= no overlap with any
window)" (`xet_client/src/cas_client/chunk_window_builder.rs`). A segment that is **partially** dirty therefore has
no server-supplied verification hash, and the client cannot derive one: the dedup index answers with *keyed* chunk
hashes (`docs/XET.md`, M9), and `range_hash_from_chunks` consumes unkeyed ones. The only source of the unkeyed
hashes of the kept chunks is the kept bytes themselves.

## What xet-core does about it, which is the answer to the question

`upload_ranges` (`xet_data/src/processing/range_upload.rs`) is the primitive for "edit a file in the CAS without
reading it". It snaps every dirty range **out to the enclosing segment boundaries** before it asks the server for
anything:

```rust
// Snapping to *segment* boundaries (rather than chunk boundaries) lets us swap whole
// segments during composition, so the client never has to truncate a segment mid-chunk.
// Safe to send to the server because segment edges are chunk edges, so the server's
// chunk-aligned windows come back identical to our snapped ranges.
```

and then per window it re-reads the original bytes and feeds them to the cleaner:

```rust
if cursor < edit_start {
    stream_cas_range(&ctx, &cas_client, original_hash, cursor, edit_start, &mut cleaner).await?;
}
// … the new bytes …
if cursor < w_end {
    stream_cas_range(&ctx, &cas_client, original_hash, cursor, w_end, &mut cleaner).await?;
}
```

where `stream_cas_range` is `FileReconstructor::new(…).with_byte_range(…).reconstruct_to_stream()` — a real
download of the window's original bytes. Composition refuses anything else:

```
"server returned a window starting at {w.start} that straddles segment {seg_idx} \
 ({}..{}); composition requires segment-aligned windows"
```

(`compose_mdb`, same file). So **confirmed: an edit inside a xorb costs the affected segment, downloaded and
re-chunked and re-uploaded**, whatever the file size. Two notes on the wording of the question:

* the unit is the **segment**, not the xorb — usually the same thing for a single-file xorb, but a segment can be a
  slice when part of a file deduped into someone else's xorb, and one xorb can back several segments;
* the *unchanged prefix* of the segment is not merely hashed, it is **re-uploaded**: the cleaner re-chunks from the
  window start, and the dedup it can consult is the session's own plus a sampled global index (see below), so the
  re-chunked prefix chunks are not matched against the xorb they came from.

## The numbers, and when it pays

`xet_core_structures/src/xorb_object/constants.rs`: chunks target 64 KiB (min 8 KiB, max 128 KiB), a xorb caps at
`MAX_XORB_BYTES = 64 MiB` uncompressed and `MAX_XORB_CHUNKS = 8192` — so **one segment ≤ 64 MiB**, and the window
extension past a mid-file edit is small (it runs to the next stable CDC boundary, two chunks in
`[2·min, max − min)` = `[16 KiB, 120 KiB)`, `next_stable_chunk_boundary`).

| file | dirty range (footer) | what the edit must move | vs. plain rewrite |
| --- | --- | --- | --- |
| ≤ 64 MiB (one xorb ⇒ one segment) | KB–MB | the whole file down, the whole file up | nothing saved |
| 300 MB, tail segment full | KB–MB | ≤ 64 MiB down, ≤ 64 MiB up | ~5× |
| 5 GB | KB–MB | ≤ 64 MiB down, ≤ 64 MiB up | ~80× |

Wire sizes are LZ4 per chunk, which buys little on Parquet (the pages are already compressed), so read the table as
uncompressed. Everything in the first row is the case the feature was for: a DuckDB-written Parquet of 10–50 MB is
one segment, so "fetch the footer only" would become "fetch the file".

## A second, independent blocker: git-backed repositories

Composed files carry no SHA-256 — `range_upload.rs`'s own limitations section:

> The composed file has no SHA-256 metadata (`metadata_ext = None`), since recomputing it would require reading the
> full file. This means `upload_ranges` is only suitable for contexts that don't require SHA-256 verification
> (e.g. HF buckets, xet-native repos), not for Git LFS-backed repos that verify SHA-256 on download.

The window cleaners are started with `Sha256Policy::Skip`, and `FileDataSequenceHeader::new(hash, n, true, false)` in
`compose_mdb` says the same in the flags. A git-backed commit needs an `lfsFile` entry with the new full-file
`sha256` (`src/hf_commit.cpp`), which is a full read by definition. So even if the segment cost were acceptable, the
path only ever exists for **buckets** (`addFile` with a `xetHash`), not for the LFS repositories most datasets live
in — which is also where a metadata-edit feature would have been used.

## What would make it a KB-scale operation (needs server work, not client work)

The client half is already available and cheap: `GET {cas}/v1/reconstructions/{hash}` returns the whole segment
table — per term `xorb hash`, xorb-local chunk index range, `unpacked_length` (`remote_client.rs::
get_file_reconstruction_info`) — in a few KB, and it is enough to name a pointer segment. What is missing is a way
to *prove* a segment that the original shard does not contain verbatim:

1. **chunk-granular verification.** Let `gap_verification` (or a variant of it) return a range hash over an
   arbitrary chunk-index range instead of only over whole stable segments. Then a client could cut the tail segment
   at the chunk boundary the server already reports, point the kept prefix at the existing xorb (needs only xorb
   hash + chunk index range + byte length, all in the reconstruction answer), and upload nothing but the new footer
   chunk. Cost: footer + ≤ 1 chunk + shard. The Merkle side needs no change — `hash_ranges` is already chunk-based.
2. **or expose the unkeyed chunk hashes per xorb.** The xorb footer already stores them, `XorbObjectInfoV1.chunk_hashes`
   plus the chunk boundary offsets, ~40 B per chunk, at the end of the object
   (`xet_core_structures/src/xorb_object/xorb_object_format.rs`). A range read of a xorb tail would let a client
   re-tile and compute range hashes without touching chunk data. Needs a byte-range read of a xorb object, which the
   reconstruction APIs do not offer today (they hand out URLs for ranges the *file* needs).
3. plus, for anything but buckets, a commit path that accepts a `xetHash` instead of a full-file `sha256`.

That is an upstream conversation (xet-core / the CAS), and the shape of the ask is small: option 1 is a finer-grained
version of an endpoint that already exists.

## What to tell users instead

* **Reading** Parquet metadata is already cheap and needs no new code: the read path is range-based, so the footer
  (last bytes) costs one range read — `SELECT * FROM parquet_kv_metadata('hf://…')` / `parquet_schema` behave.
* **Writing** metadata means rewriting the tail segment, i.e. re-uploading. There is no dedup rescue: the global
  index only samples `MDB_SHARD_GLOBAL_DEDUP_CHUNK_MODULUS = 1024` of chunks and the stored hashes are keyed
  (`docs/XET.md`, M9), which is exactly the measurement in `scripts/dedup_measure.py` — reuse is thin by construction.
* Volatile key-value metadata therefore should not live in a large Parquet file that is edited in place. A sidecar
  (`metadata.json` next to it, or a `.parquet` plus a small `_meta.parquet`) costs one small upload per edit and
  stays consistent with the data as long as both are written together.

## Map of the reference code, for whoever revisits this

* `xet_data/src/processing/range_upload.rs` — `upload_ranges`, `DirtyInput`, segment snapping, `compute_windows`,
  `stream_cas_range`, `compose_mdb`, the merge sequence and `final_hash().hmac(0)`.
* `xet_data/src/processing/file_upload_session.rs:706` — `register_composed_file`, how a composed file's MDB reaches
  the session shard (no per-file sha256).
* `xet_client/src/cas_client/remote_client.rs` — `get_file_chunk_hashes` (`/v2/file-chunk-hashes/{hash}` +
  `X-Range-Dirty`), `get_file_reconstruction_info` (`/v1/reconstructions/{hash}`), `get_reconstruction_v1/v2`
  (presigned range URLs), `upload_shard`, `upload_xorb`.
* `xet_client/src/cas_client/chunk_window_builder.rs` — server-side window/stability logic and the per-segment
  `gap_verification` emission.
* `xet_core_structures/src/metadata_shard/chunk_verification.rs` — `range_hash_from_chunks`, `VERIFICATION_KEY`.
* `xet_core_structures/src/merklehash/merkle_hash_subtree.rs` — the hump representation, `from_chunks`, `merge`,
  `final_hash`; the part of the port that would have been the real work, and which is now moot.
* `xet_core_structures/src/xorb_object/constants.rs`, `xorb_object/xorb_object_format.rs` — chunk/xorb limits,
  stability condition, xorb footer layout.
