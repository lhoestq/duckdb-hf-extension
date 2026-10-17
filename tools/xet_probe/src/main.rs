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
use std::io::Read;
use xet_core_structures::merklehash::{compute_data_hash, compute_internal_node_hash, MerkleHash};

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
    if mode == "internal" {
        report(&compute_internal_node_hash(&data));
    } else {
        report(&compute_data_hash(&data));
    }
}
