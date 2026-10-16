import sys, duckdb
EXT = "/home/quentin/hf/hf-duckdb-extension/dist/hf.duckdb_extension"
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute(f"LOAD '{EXT}'")

def q(sql, limit=4):
    try:
        rel = con.execute(sql)
        try:
            rows = rel.fetchall()
        except Exception:
            rows = None
        print("OK  ", sql.strip()[:78], "->", (rows[:limit] if isinstance(rows, list) else rows))
    except Exception as e:
        print("FAIL", sql.strip()[:78], "->", type(e).__name__, str(e).replace("\n", " ")[:240])

for sql in sys.stdin.read().splitlines():
    if sql.strip():
        q(sql)
