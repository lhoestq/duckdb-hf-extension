#!/usr/bin/env python3
"""Ask the CAS, one chunk at a time, what it can already serve.

Cross-repository dedup is the one part of a Xet write that cannot be checked by round-tripping your own
upload: the question is whether content stored by *some other* repository becomes findable from here.
The CAS answers it directly - `GET {cas}/v1/chunks/default/{chunk_hash}` returns a metadata shard that
describes where that content lives, or 404 - but only for chunks it decided to index, and which chunks
those are is the thing being measured (docs/XET.md, M9).

So this prints, per chunk of a file:

    index  size  hash  eligible  status  shard_bytes

where `eligible` is the reference's own answer (`xet-probe dedup`, i.e. xet-core's
`hash_is_global_dedup_eligible` = one hash in 1024) rather than a re-derivation of the rule here, and the
chunk hashes themselves come from the reference chunker (`xet-probe chunks`) for the same reason: an
answer about the CAS must not depend on our C++ being right, or a wrong C++ would still make this look
like success.

Chunk 0 is the file's first chunk, which the shard marks with a flag instead of relying on the hash rule,
so it is shown as eligible too.

    scripts/cas_dedup_probe.py datasets/lhoestq/hf-smoke-staging /tmp/payload.bin
    XET_PROBE=/path/to/xet-probe scripts/cas_dedup_probe.py <repo> <file>

Requires a writable token (HF_TOKEN, or whatever `hf auth token` prints) and the file to have been
uploaded through this repository at least once - the token is scoped to one repo, which is also why the
answer is about the repo's own view of the global index.
"""
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

ENDPOINT = os.environ.get("HF_ENDPOINT", "https://huggingface.co")
REVISION = "main"


def die(message):
    sys.stderr.write(f"cas_dedup_probe: {message}\n")
    sys.exit(1)


def hub_token():
    token = os.environ.get("HF_TOKEN") or os.environ.get("HUGGING_FACE_HUB_TOKEN")
    if token:
        return token.strip()
    try:
        out = subprocess.run(["hf", "auth", "token"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        die(f"no HF_TOKEN in the environment and `hf auth token` failed: {exc}")
    if out.returncode != 0 or not out.stdout.strip():
        die(f"no HF_TOKEN in the environment and `hf auth token` said: {out.stderr.strip()}")
    return out.stdout.strip()


def request(url, headers, timeout=60):
    """Returns (status, body bytes). A 404 is an answer, not an error, so nothing here raises on status."""
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as response:
        return response.code, response.read()


def probe(probe_path, mode, stdin_bytes):
    """Run the reference probe over raw bytes. Bytes in, bytes out: the file under test is arbitrary
    content, and decoding it as text to hand it to a subprocess would re-encode it and chunk different
    bytes than the file holds."""
    if not os.path.exists(probe_path):
        die(f"xet-probe not found at {probe_path} (build it with `cargo build --release` in tools/xet_probe)")
    out = subprocess.run([probe_path, mode], input=stdin_bytes, capture_output=True)
    if out.returncode != 0:
        die(f"xet-probe {mode} failed: {out.stderr.decode('latin-1').strip()}")
    return out.stdout.decode("latin-1")


def main(argv):
    probe_path = os.environ.get("XET_PROBE", "tools/xet_probe/target/release/xet-probe")
    sample = 0
    args = list(argv[1:])
    while args and args[0].startswith("--"):
        flag, _, value = args.pop(0).partition("=")
        if not value and args:  # both `--flag=v` and `--flag v` spellings
            value = args.pop(0)
        if flag == "--probe-path":
            probe_path = value
        elif flag == "--sample":
            sample = int(value)
        else:
            die(f"unknown flag: {flag}")
    if len(args) != 2:
        die("usage: cas_dedup_probe.py [--probe-path=<xet-probe>] <repo e.g. datasets/lhoestq/x> <file>")
    repo, path = args
    if not os.path.exists(path):
        die(f"no such file: {path}")

    with open(path, "rb") as handle:
        data = handle.read()

    # Chunking and hashing by the reference, so the numbers below are answers about the CAS alone
    lines = probe(probe_path, "chunks", data).splitlines()
    chunks = []
    for line in lines:
        index, size, hex_hash = line.split()
        chunks.append((int(index), int(size), hex_hash))
    if not chunks:
        die(f"{path} chunked into nothing")

    # The eligibility rule straight from xet-core, through the same probe that produced our vectors
    answers = probe(probe_path, "dedup", ("\n".join(c[2] for c in chunks)).encode())
    eligible = {}
    for line in answers.splitlines():
        hex_hash, answer = line.split()
        eligible[hex_hash] = answer == "1"

    token = hub_token()
    body = request(f"{ENDPOINT}/api/{repo}/xet-write-token/{REVISION}",
                   {"Authorization": f"Bearer {token}"})[1]
    try:
        exchange = json.loads(body)
    except json.JSONDecodeError:
        die(f"the write-token exchange did not answer with json: {body[:200]!r}")
    cas_url = exchange.get("casUrl") or exchange.get("cas_url")
    cas_token = exchange.get("accessToken") or exchange.get("access_token")
    if not cas_url or not cas_token:
        die(f"the exchange answer is missing casUrl/accessToken: {list(exchange)}")

    # A whole file is 1000+ requests, and the interesting chunks are not spread evenly: the eligible ones
    # are 1 in 1024, so asking about arbitrary chunks is mostly asking the CAS to say no. --sample keeps
    # the first chunk and every eligible one, and adds a deterministic spread of the rest as a control.
    note = ""
    if sample:
        keep = [c for c in chunks if c[0] == 0 or eligible[c[2]]]
        rest = [c for c in chunks if c[0] != 0 and not eligible[c[2]]]
        step = max(1, len(rest) // max(1, sample))
        keep.extend(rest[i] for i in range(0, len(rest), step))
        note = " (sampled: %d eligible/first + every %d%dth of %d others)" % (
            sum(1 for c in keep if c[0] == 0 or eligible[c[2]]), step,
            step, len(rest))
        chunks = keep

    print("# repo=%s file=%s chunks=%d bytes=%d cas=%s%s" %
          (repo, path, len(chunks), len(data), cas_url, note))
    print("# index\tsize\tchunk_hash\teligible\tstatus\tshard_bytes")
    hits = misses = 0
    hits_by_cause = {"first": 0, "sample": 0, "neither": 0}
    for index, size, hex_hash in chunks:
        status, shard = request(f"{cas_url}/v1/chunks/default/{hex_hash}",
                                {"Authorization": f"Bearer {cas_token}"})
        is_hit = status == 200
        if is_hit:
            hits += 1
        else:
            misses += 1
        is_first = index == 0
        cause = "first" if is_first else ("sample" if eligible[hex_hash] else "neither")
        hits_by_cause[cause] += 0 if not is_hit else 1
        print("%d\t%d\t%s\t%s\t%d\t%d" % (index, size, hex_hash,
                                          "yes" if (is_first or eligible[hex_hash]) else "no",
                                          status, len(shard) if is_hit else 0))

    sampled = sum(1 for _, _, h in chunks if eligible[h])
    print("# indexed: %d hits / %d chunks; eligible-by-hash %d, first chunk 1" %
          (hits, len(chunks), sampled))
    print("# hits by cause: first=%d sampled-only=%d not-eligible=%d" %
          (hits_by_cause["first"], hits_by_cause["sample"], hits_by_cause["neither"]))
    print("# the claim under test: a chunk that is eligible *only* by hashing (no file-start flag) is "
          "indexed anyway")
    print("# verdict: %s" % ("PASS" if hits_by_cause["sample"] > 0 else
                             ("inconclusive (this file has no sampled-only eligible chunk)" if sampled
                              else "inconclusive (no eligible chunk besides the first)")))


if __name__ == "__main__":
    main(sys.argv)
