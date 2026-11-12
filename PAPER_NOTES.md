# SaneQL dialect: notes for the parser paper

Written 2026-10-21 after the 22 TPC-H queries passed. Companion to `NOTES.md` (findings as they were made) and
`MEASUREMENTS.md` (numbers, regenerate with `scripts/measure.py`). This file says what the case study shows, what it
does not show, and how to report it.

## What the case study is

SaneQL (Neumann & Leis, "A Critique of Modern SQL and a Proposal Towards a Simple and Expressive Query Language",
CIDR 2024) implemented as a loadable DuckDB dialect extension, built from `duckdb/extension-template`, against
DuckDB v2.0.0 (`d54cbbe3df`), with no changes to the DuckDB core. Activated with `SET current_dialect='saneql'`.
Emits DuckDB's parsed AST directly; no SQL text is generated. Verified: 23 sqllogictests (22 TPC-H queries at
SF 0.01 checked against DuckDB's shipped answers, plus an operator test), 2919 assertions, all passing.

Why this case study matters more than the others (DuckPGQ, Spark dialect): the language has a published
specification by a third party, a public reference implementation to compare size against, and a public test
corpus with known answers. Nobody chose it for convenience, and the surface syntax shares almost nothing with SQL.

## The claim it supports

Syntax is a thin, replaceable layer when the parser is runtime-extensible and the semantic layer is shared.

| Component | SaneQL reference translator (code lines) | DuckDB dialect |
|---|---:|---:|
| Grammar | 267 (bison) | 24 PEG rules |
| Lexer | 692 | 0 (DuckDB's tokenizer, unchanged) |
| AST + generator | 1,064 | 0 (DuckDB's AST) |
| Semantic analysis | 1,682 | 0 (DuckDB's binder, 13,678 code lines, shared) |
| Algebra | 1,021 | 0 |
| SQL writer, infra | 280 | 0 |
| Dialect-specific C++ | | 753 |
| **Total** | **5,028** | **753** |

- The dialect is 15% of the reference implementation. Everything removed is lexing, semantics and algebra.
- It adds 2.2% to DuckDB's grammar rule count (24 of 1,107) and 2.1% to the hand-written transformer.
- Both codebases, independently, have the parser as the small part: SaneQL's grammar is 5% of its translator;
  DuckDB's grammar is 1,263 code lines against 9,778 hand-written transformer lines and 13,678 binder lines.
- Fair comparison to quote: grammar + lexer + semantic analysis of the reference (2,641) vs 753 here. Do not
  quote the 5,028 total without saying the algebra and SQL writer have no counterpart in an AST-emitting dialect.
  Keep DuckDB's generated trampolines (23,702 lines) out of authoring-effort comparisons; they are generated.

Side finding, footnote only: across the 22 queries SaneQL text is 9,553 characters and SQL 9,696. The two
notations carry the same information. This says nothing about readability or regularity, which is what the
SaneQL critique is about, so do not use it as a counter-argument.

## Positive findings about the dialect API

- No tokenizer override was needed. SQL already tokenizes `:=`, `=>`, `{`, `}`, `&&`, `||`, `!`.
- The scalar expression language (casts, arithmetic, comparisons, aggregates) was reused by referencing the
  `Expression` rule. The dialect only added `&&`/`||`/`!` as logical operators, `case({...})`, brace lists in
  scalar position and `distinct:=true`.
- Splicing the entry rule into `SelectAtom` made a SaneQL program valid anywhere a SELECT body is, including
  inside SQL subqueries, and SQL keeps working under the dialect. A pipeline in expression position is a scalar
  subquery (`x > t.aggregate(max(y))`).
- API contact surface stayed small: `AddRule`, `ReplaceRule`, `AddChoice`, `PrependChoice`, `GetRule`,
  `FinalizeTransformProcess`, `Transform<T>`, `MakeResult`, `CreateMatcherFactory`, `OperatorMatcher`.

## Limits and caveats (report all of them)

1. **`defun` is not implemented.** User-defined table operators "indistinguishable from built-in ones" is
   SaneQL's headline feature. The dialect resolves UFCS by syntax, not type: table operators are a closed keyword
   set (`'FILTER' / 'JOIN' / 'GROUPBY' / ...`), which is what lets the grammar tell `orders.filter(...)` from
   `revenue.desc()` or `n1.n_name` without a type system. A user-defined operator therefore needs a grammar
   change. Candidate follow-up: have `defun` register a new grammar choice at runtime, which would be the most
   literal demonstration of a runtime-extensible grammar. Needs a statement-scoped grammar-change API that does
   not exist yet. State as future work unless built.
2. **Host SQL semantics leak into the dialect.** `LIMIT`/`JOIN` are reserved in SQL and broke `limit:=` and
   `.join(` until argument names went through `ColLabel`; a column called `order` still needs quotes although
   SaneQL has six keywords; `||` had to be redefined from concatenation to logical or; `l_returnflag.desc()`
   parses as function `desc` in schema `l_returnflag` because SQL defers that disambiguation to the binder, so
   every dialect-interpreted method handles two AST shapes. These are the keyword and irregularity critiques of the
   SaneQL paper surfacing inside the host. Report them as the price of sharing the semantic layer.
3. **Semantic mismatches.** `let` did not map onto macros (a statement cannot carry CREATE MACRO) and became three
   rewrites: table-valued let is a CTE, scalar let is substituted, parametrised let is substituted per call into a
   correlated scalar subquery. Parameter substitution is scope-blind; TPC-H never relies on shadowing.
   `aggregate` is scalar-typed by position (SaneQL decides by type). Methods that are SQL special forms (`like`,
   `between`, `in`, `extract`, `count(distinct:=true)`) needed a post-pass. The SELECT builder nests subqueries
   where hand-written SQL would not (no HAVING is emitted); the optimizer flattens them.
4. **API gaps found by building against it.** `ParsedGrammar::AddTerminalRuleOverride` throws for an existing
   terminal, so rejecting `&&`/`||`/`=>` from the generic operator matcher required overriding
   `CreateMatcherFactory` and subclassing `OperatorMatcher` (12 lines, hard to find). The parse-tree shape (one-child
   LIST per rule, CHOICE inside, macro expansions named `Parens`/`List`, OPTIONAL/CHOICE carrying the wrapped name)
   is undocumented and was discovered by printing; one crash before it was understood. The host branch moved under
   the extension between milestones (the "API still changes" risk, realised).
5. **Coverage.** Not implemented because TPC-H does not use them: `projectout`, `distinct`, `window`, set
   operations, `groupby(type:=, sets:=)`, null ordering, `gensym`, `foreigncall`, `symbol`/`lambda` parameter
   kinds. Window and set operations map onto existing AST nodes and would follow the same pattern; `gensym` and
   `foreigncall` have no counterpart in the shared layer.
6. **Corpus fixes.** Upstream q5 lacks `c_nationkey = s_nationkey` (and carries a "wrong result?" comment); the
   local copy adds it. q10's column order and q17's missing final division are wrapped in a SQL SELECT in the tests.
7. **Not measured.** Parse-time overhead of the dialect; LLM generation accuracy SQL vs SaneQL.

## How to use it in the paper

- Extensibility section: third case study, with the table above and the 2% figures. One paragraph on what the
  dialect reuses, one on the limits (items 1 and 2 verbatim in spirit). Framing: "syntax is a thin replaceable
  layer"; never "the language was never the problem". The SaneQL learnability critique is orthogonal and its authors
  may review the paper.
- Lessons section: items 2 and 4 are findings about dialect APIs in general: a shared semantic layer imports the
  host language's lexical decisions, and terminal-level overrides are a distinct extension point from rule-level ones.
- Related work: cite the CIDR 2024 paper for 409 keywords, the 38% / 39.8% error figures, and "implementing a SQL
  front end is a major undertaking", then show the 753-line number as the answer.

## Reproduce

```
cd ~/git/saneql && make && ./build/release/test/unittest "test/sql/*"
python3 scripts/measure.py      # regenerates MEASUREMENTS.md; expects upstream SaneQL at /tmp/saneql_upstream
```
