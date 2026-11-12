#!/usr/bin/env python3
"""Generate test/sql/tpch/qNN.test from examples/tpch/qNN.sane.

Each test runs the SaneQL query under `SET current_dialect='saneql'` and compares the result with DuckDB's
shipped TPC-H answer file. Two queries need a SQL wrapper because the SaneQL version does not produce the
reference result shape: q10 lists its columns in a different order and q17 stops before the final aggregation.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = """# name: test/sql/tpch/q{n}.test
# description: TPC-H q{n} in SaneQL (examples/tpch/q{n}.sane, from github.com/neumannt/saneql, BSD-3)
# group: [tpch]

require saneql

require tpch

statement ok
CALL dbgen(sf=0.01);

statement ok
SET current_dialect = 'saneql';

"""

# SQL wrappers around the verbatim SaneQL query, where the SaneQL output differs from the TPC-H result shape
WRAPPERS = {
    10: ("SaneQL's groupby lists the keys first; reorder the columns to the TPC-H layout",
         "SELECT c_custkey, c_name, revenue, c_acctbal, n_name, c_address, c_phone, c_comment FROM (\n{q}\n) "
         "ORDER BY revenue DESC"),
    17: ("The SaneQL example stops at the filtered lineitems; add TPC-H's final aggregation",
         "SELECT sum(l_extendedprice) / 7.0 AS avg_yearly FROM (\n{q}\n)"),
}

for i in range(1, 23):
    n = f"{i:02d}"
    query = (ROOT / "examples" / "tpch" / f"q{n}.sane").read_text().strip()
    comment, body = "", query
    if i in WRAPPERS:
        comment = f"# {WRAPPERS[i][0]}\n"
        body = WRAPPERS[i][1].format(q=query)
    test = HEADER.format(n=n) + f"{comment}query I\n{body}\n----\n<FILE>:duckdb/extension/tpch/dbgen/answers/sf0.01/q{n}.csv\n"
    (ROOT / "test" / "sql" / "tpch" / f"q{n}.test").write_text(test)
print("generated 22 tests")
