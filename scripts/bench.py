import sys, time, duckdb
USE_HF_EXT = sys.argv[1] == "hf"
URL = sys.argv[2]
SQL = sys.argv[3]
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD httpfs")
if USE_HF_EXT:
    con.execute("LOAD '/home/quentin/hf/hf-duckdb-extension/dist/hf.duckdb_extension'")
for kv in (sys.argv[4].split(";") if len(sys.argv) > 4 else []):
    if kv.strip():
        k, v = kv.split("=", 1)
        con.execute(f"SET {k} = {v}")

t0 = time.time()
res = con.sql(SQL).fetchall()
dt = time.time() - t0
print(f"{'hf-ext  ' if USE_HF_EXT else 'httpfs  '} {dt:6.2f}s  {SQL[:60]}  -> {res}")
if USE_HF_EXT:
    print("        read stats " + str(con.sql("FROM hf_read_stats()").fetchall()))
