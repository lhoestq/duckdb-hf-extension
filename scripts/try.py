#!/usr/bin/env python3
"""Run a smoke suite from stdin and grade it.

Each statement is executed on its own; a column whose name starts with "assert" is a promise, and the
statement only counts as passing while every one of them comes back True. (DuckDB evaluates both branches
of a CASE eagerly, so "throwing" assertions are not a thing - the grading has to happen out here.)

A statement that is expected to fail is written as

    -- expect-fail: <a substring of the error>
    COPY (SELECT 1 i) TO 'hf://buckets/me/playground/x.parquet';

which is how the suites assert that a refusal happens *and* says the right thing. The exit code is 1 as
soon as one statement behaves other than advertised.
"""
import os
import sys
import time
import duckdb

EXT = "/home/quentin/hf/hf-duckdb-extension/dist/hf.duckdb_extension"
if len(sys.argv) > 1:
    EXT = sys.argv[1]

# Say which bytes are under test before anything else. A suite that grades an artifact built yesterday reads
# exactly like one that graded this morning's change, and `smoke_*.sh` load dist/, which is only refreshed by
# run_all.sh or a deliberate copy - so "all green" here has once meant "the previous release still works".
stat = os.stat(EXT)
print(f"-- testing {EXT}\n--   {stat.st_size} bytes, modified "
      f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(stat.st_mtime))}",
      flush=True)

con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute(f"LOAD '{EXT}'")


def statements(text):
    """Split into (sql, expected_error_or_None) pairs.

    Comment lines are dropped, except an `expect-fail:` marker, which is attached to the statement that
    follows it. Semicolons inside quoted strings do not end a statement.
    """
    out, current, quoted, expect = [], [], False, None
    for line in text.splitlines():
        stripped = line.strip()
        if not quoted and stripped.startswith("--"):
            if stripped.lower().startswith("-- expect-fail:"):
                if current and "".join(current).strip():
                    raise ValueError("expect-fail marker in the middle of a statement")
                expect = stripped.split(":", 1)[1].strip()
            continue
        for char in line:
            if char == "'":
                quoted = not quoted
            elif char == ";" and not quoted:
                sql = "".join(current).strip()
                if sql:
                    out.append((sql, expect))
                current, expect = [], None
                continue
            current.append(char)
        current.append("\n")
    tail = "".join(current).strip()
    if tail:
        out.append((tail, expect))
    return out


def run(sql, expect):
    """One statement -> ("ok" | "expected" | "fail", detail)."""
    try:
        rel = con.execute(sql)
        try:
            rows = rel.fetchall()
        except Exception:
            rows = None
        broken = []
        if isinstance(rows, list):
            columns = [d[0] for d in (rel.description or [])]
            for row in rows:
                for name, value in zip(columns, row):
                    if name.startswith("assert") and value is not True:
                        broken.append(f"{name}={value!r}")
        if broken:
            return "fail", ", ".join(broken)[:240]
        if expect is not None:
            return "fail", f"expected a failure mentioning '{expect}', the statement succeeded"
        shown = rows[:4] if isinstance(rows, list) else rows
        return "ok", str(shown)[:200] if shown else ""
    except Exception as error:
        text = f"{type(error).__name__}: {error}".replace("\n", " ")
        if expect is None:
            return "fail", text[:240]
        if expect.lower() not in text.lower():
            return "fail", f"expected '{expect}', got {text[:200]}"
        return "expected", text[:120]


tally = {"ok": 0, "expected": 0, "fail": 0}
label = {"ok": "OK  ", "expected": "XFAIL", "fail": "FAIL"}
for sql, expect in statements(sys.stdin.read()):
    verdict, detail = run(sql, expect)
    tally[verdict] += 1
    print(f"{label[verdict]} {sql.strip()[:70].replace(chr(10), ' '):<70} -> {detail}")

print(f"\n{tally['ok']} passed, {tally['expected']} expected failures, {tally['fail']} failed")
sys.exit(1 if tally["fail"] else 0)
