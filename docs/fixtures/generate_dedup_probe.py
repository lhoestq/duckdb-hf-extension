#!/usr/bin/env python3
"""Regenerate the two payloads whose chunk hashes sit next to this file.

    scripts/../docs/fixtures/generate_dedup_probe.py payload.csv /tmp/payload.csv
    scripts/../docs/fixtures/generate_dedup_probe.py big.txt      /tmp/big.txt

Both are seeded, so a rerun produces the same bytes and therefore the same chunk hashes - which is the
whole point: `docs/fixtures/*_chunks_*.txt` were produced by `xet-probe xorb` over this generator's output,
the same files were uploaded to `lhoestq/hf-smoke-staging` under `dedup_probe/`, and the CAS's answers for
those hashes are what the M9 section of docs/XET.md reports. If this script's output stops matching those
hash lists, the chunker moved under us and the measurement has to be redone.
"""
import random
import sys

WORDS = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo",
         "lima", "mike", "november"]


def payload_csv():
    rnd = random.Random(1234)
    rows = []
    for i in range(6000):
        rows.append("%d,%s,%d" % (i, "".join(rnd.choice("abcdefghij") for _ in range(24)),
                                  rnd.randint(0, 10 ** 9)))
    return ("\n".join(rows) + "\n").encode()


def big_txt():
    rnd = random.Random(99)
    out = bytearray()
    while len(out) < 12 * 1024 * 1024:
        line = " ".join(rnd.choice(WORDS) + str(rnd.randint(0, 10 ** 6)) for _ in range(12))
        out += (line + "\n").encode()
    return bytes(out)


GENS = {"payload.csv": payload_csv, "big.txt": big_txt}

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else ""
    if which not in GENS:
        raise SystemExit("usage: generate_dedup_probe.py <%s> <out_path>" % "|".join(sorted(GENS)))
    data = GENS[which]()
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print("%s: %d bytes" % (which, len(data)))
