import sys, time, duckdb
USE_HF_EXT = sys.argv[1] == "hf"
URL = sys.argv[2]
SQL = sys.argv[3]
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD httpfs")
if USE_HF_EXT:
    con.execute("LOAD '/home/quentin/hf/hf-duckdb-extension/dist/hf.duckdb_extension'")
t0 = time.time()
res = con.sql(SQL).fetchall()
dt = time.time() - t0
print(f"{'hf-ext  ' if USE_HF_EXT else 'httpfs  '} {dt:6.2f}s  {SQL[:60]}  -> {res}")
