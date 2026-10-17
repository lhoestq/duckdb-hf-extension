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
use std::fs::File;
use std::io::{Cursor, Read, Seek, SeekFrom, Write};
use xet_core_structures::merklehash::{compute_data_hash, compute_internal_node_hash, xorb_hash, MerkleHash};
use xet_core_structures::xorb_object::xorb_format_test_utils::serialize_xorb_to_stream_reference;
use xet_core_structures::xorb_object::{deserialize_chunks, CompressionScheme, XorbObject};

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
    if mode == "internal" {
        report(&compute_internal_node_hash(&data));
    } else {
        report(&compute_data_hash(&data));
    }
}
