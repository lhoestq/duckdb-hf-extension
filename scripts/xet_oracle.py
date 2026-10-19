#!/usr/bin/env python3
"""Regenerate src/hf_xet_vectors.cpp - the reference data the Xet hash port is checked against.

    scripts/xet_oracle.py            # rewrite src/hf_xet_vectors.cpp
    scripts/xet_oracle.py --check    # regenerate to a temp file and diff; nonzero exit on drift

Everything here comes out of xet-core, never out of a re-implementation, so this script cannot make
the port agree with itself. The vectors are committed because CI must be able to run the self-test
without cargo; --check is what notices when a xet-core upgrade changes the format under us.

Inputs are reproducible specs (literal bytes, a repeated pattern, splitmix64 - the PRNG xet-core's
own tests use), not blobs, so a vector says the same thing on every machine.

Needs cargo (~/.cargo/bin) and a checkout of https://github.com/huggingface/xet-core next to this
repo (XET_CORE overrides the location).
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XET_CORE = os.environ.get("XET_CORE", os.path.join(ROOT, "..", "xet-core"))
OUT = os.path.join(ROOT, "src", "hf_xet_vectors.cpp")
OUT_SHARD = os.path.join(ROOT, "src", "hf_xet_shard_vectors.cpp")

KIND_LITERAL = "HFXetTestInput::LITERAL"
KIND_SPLITMIX = "HFXetTestInput::SPLITMIX"
KIND_REPEAT = "HFXetTestInput::REPEAT"

# (name, kind, pattern, size, seed) - the chunker behaves very differently on each of these, and the
# tree only reaches interesting shapes once there are enough chunks
INPUT_VECTORS = [
    ('literal "hello world"', KIND_LITERAL, "hello world", 0, 0),
    ("empty input", KIND_LITERAL, "", 0, 0),
    ("one byte", KIND_LITERAL, "a", 0, 0),
    ("200000 zero bytes", KIND_REPEAT, "", 200000, 0),
    ("120000 bytes of repeated sentence", KIND_REPEAT, "the quick brown fox jumps over the lazy dog\n", 120000, 0),
    ("splitmix64 n=1000 seed=1", KIND_SPLITMIX, "", 1000, 1),
    ("splitmix64 n=300000 seed=7", KIND_SPLITMIX, "", 300000, 7),
    ("splitmix64 n=4000000 seed=11", KIND_SPLITMIX, "", 4000000, 11),
]

# inputs serialized into xorbs by the reference writer (scheme: none). The 4 MB vector is left out:
# a xorb is capped at 64 MiB / 8192 chunks, so splitting is the upload session's business (M5), and
# the self-test would spend its time rebuilding a file it already covers
XORB_VECTORS = [v for v in INPUT_VECTORS if v[3] < 1000000 or v[3] == 0]

# inputs for the interior-node hash, which hashes pre-formatted "hex : size\n" text
INTERNAL_VECTORS = [
    ("single node", "0" * 64 + " : 0\n"),
    ("two nodes", "0" * 64 + " : 100\n" + "f" * 64 + " : 4294967296\n"),
    ("nine nodes", "".join("%064x : %d\n" % (i, i * 7) for i in range(9))),
]


# (name, kind, pattern, size, seed, max_xorb_bytes, sha256) - shards, i.e. a file described by the
# xorbs its chunks landed in. max_xorb_bytes is a probe-side override: the real limit is 64 MiB, so
# without it a test shard would always be a single segment and the interesting part of the format -
# several segments, several xorbs, the ordering between them - would go unpinned. sha256, when set,
# turns on the file entry's metadata extension, the one place a git-backed repo's file records the
# sha256 its commit names it by; it is a whole extra 48-byte entry, so it shifts the verification
# section's position and is pinned one-segment and multi-segment both.
SHARD_VECTORS = [
    ('literal "hello shard world"', KIND_LITERAL, "hello shard world", 0, 0, 0, None),
    ("empty input", KIND_LITERAL, "", 0, 0, 0, None),
    ("one byte", KIND_LITERAL, "a", 0, 0, 0, None),
    ("splitmix64 n=300000 seed=7", KIND_SPLITMIX, "", 300000, 7, 0, None),
    ("splitmix64 n=300000 seed=7, xorbs of 40000", KIND_SPLITMIX, "", 300000, 7, 40000, None),
    ("200000 zero bytes, xorbs of 32768", KIND_REPEAT, "", 200000, 0, 32768, None),
    ("splitmix64 n=4000000 seed=11", KIND_SPLITMIX, "", 4000000, 11, 0, None),
    ('sha256 extension, "hello shard world"', KIND_LITERAL, "hello shard world", 0, 0, 0,
     "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"),
    ("sha256 extension, splitmix64 n=300000 seed=7, xorbs of 40000", KIND_SPLITMIX, "", 300000, 7,
     40000, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"),
]


def splitmix_data(n, seed):
    """xet_data's create_random_data: splitmix64, little-endian words, truncated to n bytes."""
    mask = (1 << 64) - 1
    state, out = seed, bytearray()
    while len(out) < n:
        state = (state + 0x9E3779B97F4A7C15) & mask
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
        z ^= z >> 31
        out += z.to_bytes(8, "little")
    return bytes(out[:n])


def build_input(kind, pattern, size, seed):
    if kind == KIND_LITERAL:
        return pattern.encode()
    if kind == KIND_REPEAT:
        pat = pattern.encode() or b"\x00"
        return (pat * (size // len(pat) + 1))[:size]
    return splitmix_data(size, seed)


def c_string(value):
    if value is None:
        return "nullptr"
    out = '"'
    for b in value.encode() if isinstance(value, str) else value:
        if b == 0x22:
            out += '\\"'
        elif b == 0x5C:
            out += "\\\\"
        elif b == 0x0A:
            out += "\\n"
        elif b == 0x09:
            out += "\\t"
        elif 0x20 <= b < 0x7F:
            out += chr(b)
        else:
            out += "\\x%02x" % b
    return out + '"'


def build_reference(bin_dir):
    """Chunk, xorb and file hashes for each input vector, from xet-core's examples."""
    probe = os.path.join(ROOT, "tools", "xet_probe", "target", "release", "xet-probe")
    if not os.path.exists(probe):
        subprocess.run(["cargo", "build", "--release"], cwd=os.path.dirname(probe), check=True)

    def probe_run(mode, data):
        return subprocess.run([probe, mode], input=data, check=True, capture_output=True).stdout.decode()

    def example(args, data):
        return subprocess.run([os.path.join(bin_dir, "hash")] + args, input=data, check=True,
                              capture_output=True).stdout.decode().strip()

    vectors = []
    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, "input.bin")
        for name, kind, pattern, size, seed in INPUT_VECTORS:
            data = build_input(kind, pattern, size, seed)
            with open(path, "wb") as f:
                f.write(data)
            fields = dict(line.split("=", 1) for line in probe_run("data", data).splitlines())
            chunks_path = path + ".chunks"
            if os.path.exists(chunks_path):
                os.remove(chunks_path)
            subprocess.run([os.path.join(bin_dir, "chunk"), "-i", path, "-o", chunks_path],
                           check=True, capture_output=True)
            # an empty file yields no chunks at all; the reference hashes an empty list to zeros
            chunk_list = open(chunks_path, "rb").read() if os.path.exists(chunks_path) else b""
            vectors.append((name, kind, pattern, size, seed, fields["hex"], fields["base64"],
                            fields["plain_hex"],
                            ",".join(line.split()[1] for line in chunk_list.decode().splitlines()),
                            example(["--hash-type", "file"], chunk_list),
                            example(["--hash-type", "xorb"], chunk_list),
                            chunk_list.count(b"\n")))
    return vectors


def build_xorbs():
    """Serialize each input with xet-core's writer, and pin the result with the reference's own hashes.

    The whole object is fingerprinted with `probe data` over the serialized bytes: the xorb hash is a
    merkle root over chunk *contents*, so a footer can be misaligned and still hash correctly. Only a
    digest of the byte stream tells a layout bug apart from a correct hash.
    """
    probe = os.path.join(ROOT, "tools", "xet_probe", "target", "release", "xet-probe")
    vectors = []
    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, "input.bin")
        for name, kind, pattern, size, seed in XORB_VECTORS:
            data = build_input(kind, pattern, size, seed)
            with open(path, "wb") as f:
                f.write(data)
            out = subprocess.run([probe, "xorb", "none", path + ".xorb"], input=data, check=True,
                                 capture_output=True).stdout.decode()
            fields = dict(line.split("=", 1) for line in out.splitlines() if not line.startswith("chunk "))
            chunk_lines = [line.split()[1:] for line in out.splitlines() if line.startswith("chunk ")]
            xorb = open(path + ".xorb", "rb").read()
            # the fingerprint is the same keyed hash the port already implements and tests
            fingerprint = dict(line.split("=", 1) for line in
                               subprocess.run([probe, "data"], input=xorb, check=True,
                                              capture_output=True).stdout.decode().splitlines())["hex"]
            # physical boundaries are cumulative, which is what the footer stores
            physical, running = [], 0
            for chunk_hex, chunk_size in chunk_lines:
                running += 8 + int(chunk_size)
                physical.append(running)
            vectors.append((name, kind, pattern, size, seed, "none",
                            int(fields["num_chunks"]), int(fields["length"]),
                            fields["xorb_hex"], fingerprint,
                            ",".join(s for _, s in chunk_lines),
                            ",".join(str(p) for p in physical)))
    return vectors


def build_shards():
    """Whole session shards written by xet-core's own shard writer, for the shard port to reproduce.

    The probe assembles the entries with xet-core's structs and hands them to its in-memory shard, so
    the framing under test - header, section order, bookends, lookups, footer - is the reference's.
    The fingerprint is taken over the truncated bytes a session actually uploads, which is the stream
    the CAS stores and hashes.
    """
    probe = os.path.join(ROOT, "tools", "xet_probe", "target", "release", "xet-probe")
    vectors = []
    for name, kind, pattern, size, seed, max_xorb_bytes, sha256 in SHARD_VECTORS:
        data = build_input(kind, pattern, size, seed)
        out = subprocess.run([probe, "shard", str(max_xorb_bytes), sha256 or ""], input=data,
                             check=True, capture_output=True).stdout.decode()
        fields = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        if fields.get("sha256_ext", "") != (sha256 or ""):
            raise SystemExit("shard probe reported sha256_ext=%r for %r" % (fields.get("sha256_ext"), sha256))
        segments = [line.split() for line in out.splitlines() if line.startswith("segment ")]
        verification = [line.split()[1] for line in out.splitlines() if line.startswith("verification ")]
        stored = [line.split()[1] for line in out.splitlines() if line.startswith("xorb_stored ")]
        if len(segments) != len(verification):
            raise SystemExit("shard probe returned %d segments but %d verification entries"
                             % (len(segments), len(verification)))
        vectors.append((name, kind, pattern, size, seed, max_xorb_bytes, fields["shard_fingerprint"],
                        fields["file_hex"], int(fields["shard_length"]), sha256,
                        ",".join(stored),
                        ",".join(seg[1] for seg in segments),
                        ",".join(verification)))
    return vectors


def build_internal():
    rows = []
    for name, text in INTERNAL_VECTORS:
        probe = os.path.join(ROOT, "tools", "xet_probe", "target", "release", "xet-probe")
        out = subprocess.run([probe, "internal"], input=text.encode(), check=True,
                             capture_output=True).stdout.decode()
        rows.append((name, text, dict(line.split("=", 1) for line in out.splitlines())["hex"]))
    return rows


def build_renderings():
    probe = os.path.join(ROOT, "tools", "xet_probe", "target", "release", "xet-probe")
    out = subprocess.run([probe, "pattern"], check=True, capture_output=True).stdout.decode()
    fields = dict(line.split("=", 1) for line in out.splitlines())
    return [("bytes 0x00..0x1f", fields["plain_hex"], fields["hex"], fields["base64"])]


def build_dedup_vectors():
    """Which chunks the CAS's cross-repository dedup index is expected to hold.

    `hash_is_global_dedup_eligible` reduces a DataHash modulo 1024, and the word and byte order that
    reduction picks is precisely the sort of detail that reads correctly and computes wrongly - a wrong
    word still yields a 1-in-1024 sample, just of the wrong chunks, so nothing downstream notices. So
    the C++ does not decide this from the Rust text: it replays the reference's own answers.

    The hashes are generated, not digests of anything - eligibility is a pure function of the hash
    value - and they come from a fixed seed, so the same 100 000 candidates go to the probe every run
    and the same rows land in the vector file.
    """
    probe = os.path.join(ROOT, "tools", "xet_probe", "target", "release", "xet-probe")
    state = 0x9E3779B97F4A7C15
    mask = (1 << 64) - 1
    hexes = []
    for _ in range(100000):
        words = []
        for _ in range(4):
            state = (state * 6364136223846793005 + 1442695040888963407) & mask
            z = state
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
            z ^= z >> 31
            words.append(z)
        hexes.append("".join("%016x" % w for w in words))
    out = subprocess.run([probe, "dedup"], input=("\n".join(hexes) + "\n").encode(),
                         check=True, capture_output=True).stdout.decode()
    answers = [(line.split()[0], line.split()[1] == "1") for line in out.splitlines() if line.strip()]
    eligible = [h for h, e in answers if e]
    if not eligible:
        raise SystemExit("no hash in 100 000 was dedup-eligible - the probe's answer, or the modulus, changed")
    rows = [(h, True) for h in eligible]
    # a handful of ineligible ones per eligible one: a rule that flagged everything would otherwise
    # pass a vectors file made only of positives
    step = max(1, (len(answers) - len(eligible)) // (4 * len(eligible)))
    ineligible = [h for h, e in answers if not e][::step][: 4 * len(eligible)]
    rows += [(h, False) for h in ineligible]
    rows.sort()
    return rows



def build_tree_vectors():
    """xet-core's own saved (chunk list -> xorb hash, file hash) table from its correctness test."""
    source = open(os.path.join(XET_CORE, "xet_core_structures", "src", "merklehash",
                               "aggregated_hashes.rs")).read()
    start = source.index("let reference = vec![")
    block = " ".join(source[start:].split())
    entries = re.findall(r'\(\s*vec!\[([^\]]*)\],\s*"([0-9a-f]{64})",\s*\[([^\]]*)\]', block)
    rows = []
    for index, (nodes, cas_hash, salts) in enumerate(entries):
        # rustfmt leaves a trailing comma inside every tuple: ("hash", 100,)
        chunks = re.findall(r'\(\s*"([0-9a-f]{64})",\s*(\d+)\s*,?\s*\)', nodes)
        salted = re.findall(r'\(\s*"([0-9a-f]{64})",\s*"([0-9a-f]{64})"\s*,?\s*\)', salts)
        # the zero salt proves the plain file hash, the other one proves the salt is really mixed in
        for salt_hex, file_hash in salted[:2]:
            rows.append(("tree vector %d (count %d)" % (index, len(chunks)), chunks, cas_hash,
                         salt_hex, file_hash))
    if not rows:
        raise SystemExit("could not parse xet-core's reference table - did the format change?")
    return rows


def main():
    env = dict(os.environ, PATH=os.path.expanduser("~/.cargo/bin") + ":" + os.environ.get("PATH", ""))
    bin_dir = os.path.join(XET_CORE, "target", "release", "examples")
    if not os.path.exists(os.path.join(bin_dir, "hash")):
        # the `hash` example panics on clap's debug asserts (-h is both --hash-type and --help), so
        # this has to be the release profile
        subprocess.run(["cargo", "build", "--release", "-p", "xet-data", "--examples"],
                       cwd=XET_CORE, check=True, env=env)

    revision = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=XET_CORE,
                              capture_output=True, text=True).stdout.strip()
    inputs = build_reference(bin_dir)
    xorbs = build_xorbs()
    internal = build_internal()
    renderings = build_renderings()
    trees = build_tree_vectors()
    dedup = build_dedup_vectors()
    shards = build_shards()

    out = []
    add = out.append
    add("// Generated by scripts/xet_oracle.py from xet-core@%s - do not edit by hand." % revision)
    add("// Regenerate with `scripts/xet_oracle.py`; `--check` fails when the port's stored vectors")
    add("// no longer match what the reference implementation produces.")
    add('#include "hf_xet_hash.hpp"')
    add('#include "hf_xet_xorb.hpp"')
    # named, not inherited: MSVC does not hand out uint64_t through the duckdb headers, and this file is
    # generated - an include added to the output by hand would vanish on the next regeneration (which is
    # exactly what happened once, in `39706a5`, and only the MSVC build would have noticed)
    add("#include <cstdint>")
    add("")
    add("namespace duckdb {")
    add("")
    add("const vector<HFXetHashVector> &HFXetHashVectors() {")
    add("\tstatic const vector<HFXetHashVector> vectors = {")
    for (name, kind, pattern, size, seed, chunk, b64, plain, chunk_sizes, file_hash, xorb_hash,
         n_chunks) in inputs:
        add("\t\t// %s: %d chunks" % (name, n_chunks))
        add("\t\t{%s, %s, %s, %dULL, %dULL," % (c_string(name), kind,
                                                c_string(pattern) if kind != KIND_SPLITMIX else "nullptr",
                                                0 if kind == KIND_LITERAL else size, seed))
        add("\t\t %s, %s, %s, %s," % (c_string(chunk), c_string(b64), c_string(plain),
                                      c_string(chunk_sizes)))
        add("\t\t %s, %s}," % (c_string(file_hash), c_string(xorb_hash)))
    add("\t};")
    add("\treturn vectors;")
    add("}")
    add("")
    scheme_id = {"none": "HFXetCompressionScheme::NONE", "lz4": "HFXetCompressionScheme::LZ4"}
    add("const vector<HFXetXorbVector> &HFXetXorbVectors() {")
    add("\tstatic const vector<HFXetXorbVector> vectors = {")
    for (name, kind, pattern, size, seed, scheme, num_chunks, length, xorb_hash, fingerprint,
         chunk_sizes, physical) in xorbs:
        add("\t	// %s: %d chunks, %d bytes serialized" % (name, num_chunks, length))
        add("\t	{%s, %s, %dULL, %dULL, %s, %s," % (c_string(name), kind,
                                                    0 if kind == KIND_LITERAL else size, seed,
                                                    c_string(pattern) if kind != KIND_SPLITMIX else "nullptr",
                                                    scheme_id[scheme]))
        add("\t	 %d, %d, %s, %s," % (num_chunks, length, c_string(xorb_hash), c_string(fingerprint)))
        add("\t	 %s, %s}," % (c_string(chunk_sizes), c_string(physical)))
    add("\t};")
    add("\treturn vectors;")
    add("}")
    add("")
    add("const vector<HFXetInternalHashVector> &HFXetInternalHashVectors() {")
    add("\tstatic const vector<HFXetInternalHashVector> vectors = {")
    for name, text, hash_hex in internal:
        add("\t\t{%s, %s, %s}," % (c_string(name), c_string(text), c_string(hash_hex)))
    add("\t};")
    add("\treturn vectors;")
    add("}")
    add("")
    add("const vector<HFXetRenderingVector> &HFXetRenderingVectors() {")
    add("\tstatic const vector<HFXetRenderingVector> vectors = {")
    for name, raw_hex, hex_form, b64 in renderings:
        add("\t\t{%s, %s, %s, %s}," % (c_string(name), c_string(raw_hex), c_string(hex_form),
                                       c_string(b64)))
    add("\t};")
    add("\treturn vectors;")
    add("}")
    add("")
    add("const vector<HFXetDedupVector> &HFXetDedupVectors() {")
    add("\tstatic const vector<HFXetDedupVector> vectors = {")
    for hex_hash, eligible in dedup:
        add("\t\t{\"%s\", %s}," % (hex_hash, "true" if eligible else "false"))
    add("\t};")
    add("\treturn vectors;")
    add("}")
    add("")
    for index, (name, chunks, cas_hash, salt_hex, file_hash) in enumerate(trees):

        add("static const char *const HF_XET_TREE_%d_HASHES[] = {" % index)
        for hex_hash, _ in chunks:
            add("\t\"%s\"," % hex_hash)
        add("};")
        add("static const uint64_t HF_XET_TREE_%d_SIZES[] = {" % index)
        add("\t" + " ".join("%sULL," % size for _, size in chunks) if chunks else "\t0ULL,")
        add("};")
    add("const vector<HFXetTreeVector> &HFXetTreeVectors() {")
    add("\tstatic const vector<HFXetTreeVector> vectors = {")
    for index, (name, chunks, cas_hash, salt_hex, file_hash) in enumerate(trees):
        add("\t\t{%s, %d, HF_XET_TREE_%d_HASHES, HF_XET_TREE_%d_SIZES, \"%s\", \"%s\", \"%s\"}," %
            (c_string(name), len(chunks), index, index, cas_hash, salt_hex, file_hash))
    add("\t};")
    add("\treturn vectors;")
    add("}")
    add("")
    add("} // namespace duckdb")
    add("")

    shard_out = []
    add_shard = shard_out.append
    add_shard("// Generated by scripts/xet_oracle.py from xet-core@%s - do not edit by hand." % revision)
    add_shard("// Regenerate with `scripts/xet_oracle.py`; `--check` fails when the port's stored vectors")
    add_shard("// no longer match what the reference implementation produces.")
    add_shard('#include "hf_xet_shard.hpp"')
    add_shard("")
    add_shard("namespace duckdb {")
    add_shard("")
    add_shard("const vector<HFXetShardVector> &HFXetShardVectors() {")
    add_shard("\tstatic const vector<HFXetShardVector> vectors = {")
    for (name, kind, pattern, size, seed, max_xorb, fingerprint, file_hash, length, sha256, stored,
         ranges, verification) in shards:
        add_shard("\t	// %s: %d bytes uploaded" % (name, length))
        add_shard("\t	{%s, %s, %dULL, %dULL, %s, %dULL," % (c_string(name), kind,
                                                            0 if kind == KIND_LITERAL else size, seed,
                                                            c_string(pattern) if kind != KIND_SPLITMIX else "nullptr",
                                                            max_xorb))
        add_shard("\t	 %s, %s," % (c_string(fingerprint), c_string(file_hash)))
        add_shard("\t	 %s, %s, %s, %dULL, %s}," % (c_string(stored), c_string(ranges),
                                                c_string(verification), length,
                                                c_string(sha256) if sha256 else "nullptr"))
    add_shard("\t};")
    add_shard("\treturn vectors;")
    add_shard("}")
    add_shard("")
    add_shard("} // namespace duckdb")
    add_shard("")

    text = "\n".join(out)
    shard_text = "\n".join(shard_out)
    def stale(path, generated):
        current = open(path).read() if os.path.exists(path) else ""
        return current != generated

    if "--check" in sys.argv:
        if stale(OUT, text) or stale(OUT_SHARD, shard_text):
            print("generated Xet vectors are stale against xet-core@%s - rerun scripts/xet_oracle.py"
                  % revision)
            return 1
        print("Xet vectors are up to date with xet-core@%s" % revision)
        return 0
    with open(OUT, "w") as f:
        f.write(text)
    with open(OUT_SHARD, "w") as f:
        f.write(shard_text)
    print("wrote src/hf_xet_vectors.cpp: %d input vectors, %d xorb vectors, %d tree vectors, "
          "%d internal, %d rendering, %d dedup"
          % (len(inputs), len(xorbs), len(trees), len(internal), len(renderings), len(dedup)))
    print("wrote src/hf_xet_shard_vectors.cpp: %d shard vectors" % len(shards))
    return 0


if __name__ == "__main__":
    sys.exit(main())
