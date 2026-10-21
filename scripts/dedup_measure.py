#!/usr/bin/env python3
"""What cross-repository dedup actually saves, measured against the live CAS.

    .venv/bin/python scripts/dedup_measure.py [size_mb]

Four uploads of one file's worth of content, in the order the numbers below depend on them:

* **dedup off** — the baseline, and the cost of asking by contrast.
* **identical bytes** — everything is already stored, so this should cost one small shard.
* **a tail appended** — the bytes before the tail are byte-identical, so no chunk boundary in the
  shared prefix moves. This is the case content-defined chunking exists for.
* **a column added to every row** — the first line changes, so nearly every boundary after it moves.
  Included because it is the case people hope for ("I only changed one column") and it is the case
  the format cannot help with.

Content is random per run on purpose: "is this already stored?" has to be answered about bytes the
CAS has not seen for the wrong reason.
"""
import os
import subprocess
import sys
import time

import duckdb

EXT = os.path.abspath("dist/hf.duckdb_extension")
SIZE_MB = int(sys.argv[1]) if len(sys.argv) > 1 else 60
BUCKET = "hf://buckets/lhoestq/playground/duckdb/dedup_measure"
SRC = "/tmp/dm_src.csv"
APPENDED = "/tmp/dm_appended.csv"
WIDENED = "/tmp/dm_widened.csv"

con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute(f"LOAD '{EXT}'")
con.execute("SET hf_create_repo = true")


def upload(local, name, dedup):
    con.execute(f"SET hf_xet_upload_dedup = {'true' if dedup else 'false'}")
    started = time.time()
    rows = con.execute(
        "SELECT xorb, num_chunks, bytes, uncompressed_bytes, deduped_chunks, dedup_queries, dedup_hits, "
        "dedup_shard_failures FROM hf_xet_upload(?, scope := ?, publish := true)", [local, f"{BUCKET}/{name}"]
    ).fetchall()
    return dict(
        seconds=time.time() - started,
        xorbs=sum(1 for r in rows if r[0] is not None),
        wire=sum(r[2] for r in rows),
        plain=sum(r[3] for r in rows),
        sent=sum(r[1] for r in rows),
        deduped=max((r[4] for r in rows), default=0),
        queries=max((r[5] for r in rows), default=0),
        hits=max((r[6] for r in rows), default=0),
        failures=max((r[7] for r in rows), default=0),
    )


def line(label, size, res):
    # `sent` counts chunks inside xorbs we posted; `deduped` counts chunks the shard points at
    # instead. They are not two views of one total: a pointed run and a sent run can share a xorb.
    print(f"{label:<22} file {size / 1e6:6.1f} MB  {res['seconds']:6.1f} s  posted {res['xorbs']:3d} xorb(s), "
          f"{res['wire'] / 1e6:7.1f} MB wire  sent {res['sent']:6d} chunks  pointed {res['deduped']:6d}  "
          f"queries {res['queries']:3d} hits {res['hits']:3d} shard failures {res['failures']}", flush=True)


# hex digests only: the widened file is rebuilt by reading the source back, and text that does not
# round-trip through read_csv would make the comparison about quoting instead of about chunking
con.execute(f"""COPY (SELECT md5((i * 1.2345)::VARCHAR) AS a, md5((i * 6.789)::VARCHAR) AS b
                     FROM range({SIZE_MB * 12000}) t(i)) TO '{SRC}' (FORMAT CSV)""")

# the append is done with file writes, not SQL, because the whole point is that the bytes in front of
# the new lines do not move
with open(SRC, "rb") as src, open(APPENDED, "wb") as out:
    content = src.read()
    out.write(content)
    for i in range(20000):
        out.write(f"{i:032x},{'f' * 32}\n".encode())

# the added column carries per-run noise on purpose. Chunking is content-defined over a byte stream,
# and this file is generated from a fixed formula, so a rerun's widened file *is* the previous run's
# widened file with more rows after it - which dedups beautifully and measures nothing about the
# column change. Random suffix per line makes it a clean slate, the way a re-exported dataset with a
# new derived column actually is not.
con.execute(f"""COPY (SELECT a, b,
                        'a column nobody had before ' || md5(((row_number() OVER ()) * 0.7)::VARCHAR) AS c
                     FROM read_csv('{SRC}'))
               TO '{WIDENED}' (FORMAT CSV)""")


def size(path):
    return os.path.getsize(path)


print(f"-- artifact: {EXT} ({os.path.getsize(EXT)} bytes), source {size(SRC) / 1e6:.1f} MB")
baseline = upload(SRC, "baseline.csv", dedup=False)
line("baseline, dedup off", size(SRC), baseline)

again = upload(SRC, "identical.csv", dedup=True)
line("identical, dedup on", size(SRC), again)

appended = upload(APPENDED, "appended.csv", dedup=True)
line("tail appended, dedup on", size(APPENDED), appended)

widened = upload(WIDENED, "widened.csv", dedup=True)
line("column added, dedup on", size(WIDENED), widened)

print(f"\n-- identical: {again['wire'] / 1e6:.1f} MB on the wire where the baseline cost "
      f"{baseline['wire'] / 1e6:.1f} MB; {again['queries']} queries, {again['hits']} answered, "
      f"{again['seconds']:.1f} s vs {baseline['seconds']:.1f} s")
print(f"-- append-only: {appended['deduped']} chunks pointed at, {appended['sent']} sent, "
      f"{appended['wire'] / 1e6:.1f} MB wire for a {size(APPENDED) / 1e6:.1f} MB file "
      f"({appended['seconds']:.1f} s)")
print(f"-- widened: {widened['deduped']} chunks pointed at, {widened['sent']} sent, "
      f"{widened['wire'] / 1e6:.1f} MB wire for a {size(WIDENED) / 1e6:.1f} MB file "
      f"({widened['seconds']:.1f} s)")

# read everything back: a shard of pointers is only worth having if a reader assembles the same file
for name, path in (("baseline.csv", SRC), ("identical.csv", SRC), ("appended.csv", APPENDED),
                   ("widened.csv", WIDENED)):
    local_md5 = con.execute(f"SELECT md5(content) FROM read_blob('{path}')").fetchone()[0]
    served_md5 = con.execute(f"SELECT md5(content) FROM read_blob('{BUCKET}/{name}')").fetchone()[0]
    print(f"-- read back {name}: {'same bytes' if local_md5 == served_md5 else 'DIFFERENT BYTES'}")

for name in ("baseline.csv", "identical.csv", "appended.csv", "widened.csv"):
    con.execute(f"SELECT count(*) FROM hf_delete('{BUCKET}/{name}')")
subprocess.run(["rm", "-f", SRC, APPENDED, WIDENED], check=False)
