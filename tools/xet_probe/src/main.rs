// Prints what xet-core computes for stdin, in every form the C++ port has to reproduce.
//
// The point of this probe is the renderings: `hex()` reads the digest as four little-endian words
// and prints each as 16 digits, so the reference's hex string is *not* the plain hex of the digest
// bytes. Getting that backwards produces plausible-looking hashes that nothing accepts, so the
// oracle records plain hex, xet hex and base64 for the same digest and the self-test checks all
// three against the port.
//
//   probe data      chunk (leaf) hash of stdin
//   probe internal  interior-node hash of stdin
//   probe pattern   the fixed 0x00..0x1f byte pattern, no stdin
//   probe xorb <scheme> <out>  chunk stdin with the reference chunker, write a xorb with the reference
//                    writer, and report the hashes the C++ writer has to reproduce
//   probe xorb-read <path>     read a xorb back with xet-core's footer-aware reader
//   probe shard <max_xorb_bytes> < chunk stdin as a file, describe it as an upload session would, and
//                    write the shard with xet-core's own writer. `max_xorb_bytes` splits the file into
//                    several xorbs so a small input still produces a multi-segment shard; 0 means the
//                    format's real limit. Prints the bytes a session would send to `POST /v1/shards`.
use xet_core_structures::metadata_shard::chunk_verification::range_hash_from_chunks;
use xet_core_structures::metadata_shard::file_structs::{
    FileDataSequenceEntry, FileDataSequenceHeader, FileMetadataExt, FileVerificationEntry, MDBFileInfo,
};
use xet_core_structures::metadata_shard::shard_format::MDBShardFileFooter;
use xet_core_structures::metadata_shard::shard_in_memory::MDBInMemoryShard;
use xet_core_structures::metadata_shard::xorb_structs::{
    MDBXorbInfo, XorbChunkSequenceEntry, XorbChunkSequenceHeader, MDB_CHUNK_WITH_GLOBAL_DEDUP_FLAG,
};
use std::fs::File;
use std::ops::Deref;
use std::io::{Cursor, Read, Seek, SeekFrom, Write};
use xet_core_structures::merklehash::{
    compute_data_hash, compute_internal_node_hash, file_hash, xorb_hash, DataHash, MerkleHash,
};
use xet_core_structures::xorb_object::xorb_format_test_utils::serialize_xorb_to_stream_reference;
use xet_core_structures::xorb_object::{deserialize_chunks, CompressionScheme, XorbObject};

fn xorb_sizes(sizes: &[usize], start: usize, end: usize) -> String {
    let mut out = String::new();
    let mut offset = 0u32;
    for size in &sizes[start..end] {
        if !out.is_empty() {
            out.push(',');
        }
        out.push_str(&format!("{offset}/{size}"));
        offset += *size as u32;
    }
    out
}

fn plain_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn report(hash: &MerkleHash) {
    println!("hex={}", hash.hex());
    println!("base64={}", hash.base64());
    println!("plain_hex={}", plain_hex(hash.as_bytes()));
}

fn main() {
    let mode = std::env::args().nth(1).unwrap_or_else(|| "data".to_string());
    if mode == "pattern" {
        let mut bytes = [0u8; 32];
        for (i, b) in bytes.iter_mut().enumerate() {
            *b = i as u8;
        }
        // from_slice is how raw digest bytes enter the type, so this pins hex/base64 *and* parsing
        let hash = MerkleHash::from_slice(&bytes).unwrap();
        report(&hash);
        println!(
            "from_hex_roundtrip={}",
            MerkleHash::from_hex(&hash.hex()).unwrap().hex()
        );
        println!(
            "from_base64_roundtrip={}",
            MerkleHash::from_base64(&hash.base64()).unwrap().hex()
        );
        return;
    }
    let mut data = Vec::new();
    std::io::stdin().read_to_end(&mut data).unwrap();
    if mode == "xorb" {
        // The reference xorb, from the reference chunker and the reference writer, for the same bytes the
        // C++ writer will be given. Byte-exactness is the whole test here: a xorb the hub accepts is not
        // evidence that our footer layout matches, and `xorb-check` will happily read a file whose bytes
        // differ from what xet-core would have written.
        let scheme = match std::env::args().nth(2).as_deref() {
            None | Some("none") => CompressionScheme::None,
            Some("lz4") => CompressionScheme::LZ4,
            Some("bg4-lz4") => CompressionScheme::ByteGrouping4LZ4,
            Some("auto") => CompressionScheme::Auto,
            Some(other) => panic!("unknown compression scheme '{other}'"),
        };
        let out_path = std::env::args().nth(3).expect("probe xorb <scheme> <out_path>");
        let mut chunker = xet_data::deduplication::Chunker::new(64 * 1024);
        let mut chunks = chunker.next_block(&data, false);
        if let Some(last) = chunker.finish() {
            chunks.push(last);
        }
        let mut boundaries: Vec<(MerkleHash, u32)> = Vec::with_capacity(chunks.len());
        let mut sized: Vec<(MerkleHash, u64)> = Vec::with_capacity(chunks.len());
        let mut position = 0u32;
        for chunk in &chunks {
            position += chunk.data.len() as u32;
            boundaries.push((chunk.hash, position));
            sized.push((chunk.hash, chunk.data.len() as u64));
        }
        let hash = xorb_hash(&sized);
        let mut cursor = Cursor::new(Vec::<u8>::new());
        // xet-core's own correctness-testing serializer - the one its tests pin the format with
        let (_, written, footer_start) =
            serialize_xorb_to_stream_reference(&mut cursor, &hash, &data, &boundaries, scheme)
                .expect("reference serialization failed");
        std::fs::write(&out_path, cursor.into_inner()).expect("could not write the reference xorb");
        println!("xorb_hex={}", hash.hex());
        println!("xorb_base64={}", hash.base64());
        println!("num_chunks={}", chunks.len());
        println!("length={written}");
        println!("footer_start={footer_start}");
        for chunk in &chunks {
            println!("chunk {} {}", chunk.hash.hex(), chunk.data.len());
        }
        return;
    }
    if mode == "xorb-read" {
        // Read back a xorb the C++ writer produced, with xet-core's own reader. The footer is the part
        // this exists to check: `xorb-check` reads chunk bodies only, so a footer could be misaligned and
        // still yield the right content hash. Here the footer is parsed, its offsets followed, and the
        // chunk hashes it claims are recomputed from the bytes it points at.
        let path = std::env::args().nth(2).expect("probe xorb-read <path>");
        let mut file = File::open(&path).expect("could not open the xorb");
        let object = XorbObject::deserialize(&mut file).expect("xet-core rejected the footer");
        let info = object.info;
        let end = file.seek(SeekFrom::End(0)).expect("seek failed");
        let footer_start = end - 4 - info.serialized_length() as u64;
        let mut body = Vec::new();
        file.seek(SeekFrom::Start(0)).expect("seek failed");
        (&mut file).take(footer_start).read_to_end(&mut body).expect("read failed");
        let (data, boundaries) = deserialize_chunks(&mut Cursor::new(body)).expect("xet-core rejected the chunks");
        println!("num_chunks={}", info.num_chunks);
        println!("footer_xorb_hex={}", info.xorb_hash.hex());
        println!("footer_start={footer_start}");
        println!("footer_length={}", object.info_length);
        println!("uncompressed={}", data.len());
        let mut recomputed: Vec<(MerkleHash, u64)> = Vec::new();
        let mut footer_matches_body = true;
        for ((hash, physical), window) in info
            .chunk_hashes
            .iter()
            .zip(info.chunk_boundary_offsets.iter())
            .zip(boundaries.windows(2))
        {
            let chunk = &data[window[0] as usize..window[1] as usize];
            let hash_of_bytes = compute_data_hash(chunk);
            if &hash_of_bytes != hash {
                footer_matches_body = false;
            }
            println!("chunk {} {} {}", hash.hex(), chunk.len(), physical);
            recomputed.push((hash_of_bytes, chunk.len() as u64));
        }
        let recomputed_hash = xorb_hash(&recomputed);
        println!("recomputed_xorb_hex={recomputed_hash}");
        let last_physical = info.chunk_boundary_offsets.last().copied().unwrap_or(0) as u64;
        println!("physical_offsets_end_at_footer={}", last_physical == footer_start);
        println!("unpacked_offsets_end_at_data={}",
                 info.unpacked_chunk_offsets.last().copied().unwrap_or(0) as usize == data.len());
        println!("hashes_match={}", footer_matches_body && recomputed_hash == info.xorb_hash);
        return;
    }
    if mode == "shard" {
        // An upload session's shard, written by the reference writer. The parts are assembled with the
        // reference's own entry structs and their serialize() methods, then handed to MDBShardInfo, so
        // the framing under test - header, section order, bookends, lookup tables, footer - is all
        // xet-core's. What the probe chooses is only the grouping of chunks into xorbs, which is the
        // same choice the C++ makes and which the live CAS checks independently.
        let max_xorb_bytes: u64 = std::env::args()
            .nth(2)
            .unwrap_or_else(|| "0".to_string())
            .parse()
            .expect("probe shard <max_xorb_bytes>");
        // A third argument turns the file entry's metadata extension on, which is where a git-backed
        // repo's file records the sha256 its commit names it by. Empty means no extension, which is
        // what a bucket upload sends (huggingface_hub passes SKIP_SHA256 there).
        let sha256_hex: Option<String> = std::env::args().nth(3).filter(|s| !s.is_empty());
        let metadata_ext = sha256_hex
            .as_ref()
            .map(|hex| FileMetadataExt::new(DataHash::from_hex(hex).expect("probe shard <sha256> must be 64 hex digits")));
        use xet_data::deduplication::constants::{MAX_XORB_BYTES, MAX_XORB_CHUNKS};
        // config-overridable at runtime in the reference, hence the derefs; the defaults are what a
        // plain session uses and what the C++ constants hard-code
        let (limit_bytes, limit_chunks) = (*MAX_XORB_BYTES, *MAX_XORB_CHUNKS);
        let max_xorb_bytes = if max_xorb_bytes == 0 { limit_bytes as u64 } else { max_xorb_bytes };

        let mut chunker = xet_data::deduplication::Chunker::new(64 * 1024);
        let mut chunks = chunker.next_block(&data, false);
        if let Some(last) = chunker.finish() {
            chunks.push(last);
        }
        let chunk_hashes: Vec<MerkleHash> = chunks.iter().map(|c| c.hash).collect();
        let chunk_sizes: Vec<usize> = chunks.iter().map(|c| c.data.len()).collect();

        let mut groups: Vec<(usize, usize)> = Vec::new();
        let mut start = 0usize;
        while start < chunk_sizes.len() {
            let (mut end, mut bytes) = (start, 0u64);
            while end < chunk_sizes.len()
                && (end - start) < limit_chunks
                && bytes + chunk_sizes[end] as u64 <= max_xorb_bytes
            {
                bytes += chunk_sizes[end] as u64;
                end += 1;
            }
            if end == start {
                end = start + 1; // a single oversized chunk still needs a xorb of its own
            }
            groups.push((start, end));
            start = end;
        }

        let mut xorb_blocks = Vec::new();
        let mut xorb_hashes = Vec::new();
        let mut segments = Vec::new();
        let mut stored_bytes = 0u64;
        for (start, end) in &groups {
            let sized: Vec<(MerkleHash, u64)> =
                chunk_hashes[*start..*end].iter().copied().zip(chunk_sizes[*start..*end].iter().copied().map(|s| s as u64)).collect();
            let xorb_hash = xorb_hash(&sized);
            let group_bytes: u64 = chunk_sizes[*start..*end].iter().sum::<usize>() as u64;
            stored_bytes += group_bytes;
            let mut chunks_entries = Vec::new();
            let mut offset = 0u32;
            for (index, size) in chunk_sizes[*start..*end].iter().enumerate() {
                let mut entry = XorbChunkSequenceEntry::new(chunk_hashes[*start + index], *size, offset);
                // the reference flags a chunk that begins a file; with one file per shard that is only
                // the very first chunk, and it lands in the first xorb
                if *start == 0 && index == 0 {
                    entry.flags = MDB_CHUNK_WITH_GLOBAL_DEDUP_FLAG;
                }
                chunks_entries.push(entry);
                offset += *size as u32;
            }
            xorb_blocks.push(MDBXorbInfo {
                metadata: XorbChunkSequenceHeader::new(xorb_hash, end - start, group_bytes),
                chunks: chunks_entries,
            });
            xorb_hashes.push(xorb_hash);
            // Chunk indices in a file segment are *xorb local*: a reader resolves the xorb by hash and
            // then slices its own chunk list, so `start..end` here is `0..group.len()`. (The CAS checks
            // this on ingest: global file indices produce "invalid shard failed to verify file".)
            segments.push(FileDataSequenceEntry::new(xorb_hash, group_bytes, 0u64, (end - start) as u64));
        }

        let all_sized: Vec<(MerkleHash, u64)> = chunk_hashes
            .iter()
            .copied()
            .zip(chunk_sizes.iter().copied().map(|s| s as u64))
            .collect();
        let file_hash = file_hash(&all_sized);
        let verification: Vec<MerkleHash> =
            groups.iter().map(|(s, e)| range_hash_from_chunks(&chunk_hashes[*s..*e])).collect();

        let mut mdb = MDBInMemoryShard::default();
        for info in xorb_blocks {
            mdb.add_xorb_block(info).expect("reference rejected the xorb block");
        }
        mdb.add_file_reconstruction_info(MDBFileInfo {
            metadata: FileDataSequenceHeader::new(file_hash, segments.len(), true, metadata_ext.is_some()),
            segments,
            verification: verification.iter().map(|hash| FileVerificationEntry::new(*hash)).collect(),
            metadata_ext: metadata_ext.clone(),
        })
        .expect("reference rejected the file info");

        let full = mdb.to_bytes().expect("reference could not write the shard");
        // A session uploads the local shard file cut short at file_lookup_offset, with the header's
        // footer_size rewritten to 0; that is the byte stream the CAS sees and hashes.
        let footer = MDBShardFileFooter::deserialize(&mut Cursor::new(&full[full.len() - 200..]))
            .expect("the reference's own footer did not parse");
        let mut uploaded = full[..footer.file_lookup_offset as usize].to_vec();
        uploaded[40..48].copy_from_slice(&0u64.to_le_bytes());

        // xet-core ordering: ascending by the hash's first word, which is what the lookups key on
        let mut ordered_xorb_hashes = xorb_hashes.clone();
        ordered_xorb_hashes.sort_by_key(|hash| hash.deref()[0]);

        println!("shard_fingerprint={}", compute_data_hash(&uploaded).hex());
        println!("shard_length={}", uploaded.len());
        println!("shard_full_length={}", full.len());
        println!("shard_file_lookup_offset={}", footer.file_lookup_offset);
        println!(
            "sha256_ext={}",
            metadata_ext
                .map(|ext| ext.sha256.hex())
                .unwrap_or_default()
        );
        println!("file_hex={}", file_hash.hex());
        println!("stored_bytes={stored_bytes}");
        println!("num_xorbs={}", xorb_hashes.len());
        for (index, (start, end)) in groups.iter().enumerate() {
            // printed as the shard stores them: chunk indices are xorb local, so a segment that covers
            // a whole xorb reads 0..len regardless of where in the file that xorb sits
            println!("segment 0-{} {}", end - start, xorb_hashes[index].hex());
        }
        for hash in &verification {
            println!("verification {}", hash.hex());
        }
        for hash in &ordered_xorb_hashes {
            println!("xorb_stored {}", hash.hex());
        }
        println!("limits {limit_bytes} {limit_chunks}");
        for index in 0..groups.len() {
            println!(
                "xorb_chunks {} {} {}",
                groups[index].0,
                groups[index].1,
                xorb_sizes(&chunk_sizes, groups[index].0, groups[index].1)
            );
        }
        return;
    }
    if mode == "internal" {
        report(&compute_internal_node_hash(&data));
    } else {
        report(&compute_data_hash(&data));
    }
}
