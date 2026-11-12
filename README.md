# SaneQL as a DuckDB dialect

A loadable DuckDB extension that adds [SaneQL](https://github.com/neumannt/saneql) (Neumann & Leis, CIDR 2024)
as a query dialect, built on DuckDB's runtime-extensible PEG parser. It is a case study for a paper on that
parser: the extension supplies syntax only and reuses DuckDB's expression grammar, transformer, binder and
planner.

```sql
LOAD saneql;
SET current_dialect = 'saneql';

lineitem
.filter(l_shipdate <= '1998-12-01'::date - '90 days'::interval)
.groupby({l_returnflag, l_linestatus}, {sum_qty:=sum(l_quantity), count_order:=count()})
.orderby({l_returnflag, l_linestatus});
```

Plain SQL keeps working under the dialect, and a SaneQL pipeline is accepted anywhere a `SELECT` body is.

**Scope:** the operators and forms the 22 TPC-H queries use: `filter`, `join` (inner, leftouter, leftsemi,
leftanti, exists), `as`, `groupby`, `aggregate`, `orderby`, `project`, `map`, `let` (scalar, table-valued and
parametrised), `&&`/`||`/`!`, `case({c => v}, else:=e)`, `{...}` lists, `x.in()/.like()/.between()/.extract()`,
`count(x, distinct:=true)`. All 22 queries match DuckDB's shipped TPC-H answers (`test/sql/tpch/`).

- `src/include/saneql_grammar.hpp`: the grammar fragment (24 PEG rules).
- `src/saneql_transformer.cpp`: the transform into DuckDB's parsed AST.
- `NOTES.md`: where SaneQL and SQL's shared semantic layer disagree, and what the dialect API needed.
- `MEASUREMENTS.md`: size of this extension vs. SaneQL's own translator vs. DuckDB's SQL front end.
- `examples/tpch/`: the TPC-H queries in SaneQL (from the SaneQL repository, BSD-3).

## Building and testing

```sh
git submodule update --init
GEN=ninja make          # builds DuckDB, the tpch extension and this extension
make test               # runs test/sql/**/*.test with the sqllogictest runner
```

The CLI needs the extensions loaded explicitly for a development build:

```sh
./build/release/duckdb -unsigned
LOAD 'build/release/extension/tpch/tpch.duckdb_extension';
LOAD 'build/release/extension/saneql/saneql.duckdb_extension';
```

The shell treats a line starting with `.` as a dot-command, so paste multi-line SaneQL as one statement.
`SANEQL_DEBUG_PARSE=1` prints the parse tree of every SaneQL query.

The duckdb submodule is pinned to v2.0.0; the dialect API is new on that line and still changes, so bump it
deliberately.
