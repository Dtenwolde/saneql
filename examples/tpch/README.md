# TPC-H in SaneQL

The 22 TPC-H queries as written in SaneQL, copied verbatim from
[neumannt/saneql](https://github.com/neumannt/saneql/tree/HEAD/examples/tpch) (BSD 3-Clause,
Copyright (c) 2023 Thomas Neumann). Two edits: the stray `-- gives the wrong result?` comment in q5 was removed, and q5's supplier join gained the
`c_nationkey = s_nationkey` predicate that TPC-H specifies and the upstream example lacks (which is why it gave
the wrong result). Everything else is unchanged.

`scripts/generate_tpch_tests.py` turns these into the sqllogictests in `test/sql/tpch/`, which check each
query against DuckDB's shipped TPC-H answers at scale factor 0.01.
