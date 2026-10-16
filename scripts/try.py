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
        # a column named "assert_*" is a promise: the statement only counts as OK when it holds.
        # (DuckDB evaluates both branches of a CASE eagerly, so "throwing" assertions are not a thing.)
        columns = [d[0] for d in (rel.description or [])]
        broken = []
        if isinstance(rows, list):
            for row in rows:
                for name, value in zip(columns, row):
                    if name.startswith("assert") and value is not True:
                        broken.append(f"{name}={value!r}")
        if broken:
            print("FAIL", sql.strip()[:78], "->", ", ".join(broken)[:240])
            return
        print("OK  ", sql.strip()[:78], "->", (rows[:limit] if isinstance(rows, list) else rows))
    except Exception as e:
        print("FAIL", sql.strip()[:78], "->", type(e).__name__, str(e).replace("\n", " ")[:240])


for sql in sys.stdin.read().splitlines():
    if sql.strip() and not sql.strip().startswith("--"):
        q(sql)
