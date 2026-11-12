# Notes: SaneQL as a DuckDB dialect extension

Findings while mapping SaneQL (Neumann & Leis, CIDR 2024) onto DuckDB's runtime-extensible PEG parser.
Each entry records a mismatch between SaneQL and the shared SQL semantic layer (binder/planner), or a property
of the dialect API, that is relevant to the paper. Nothing listed here is worked around silently in the code.

Status (2026-10-21): all 22 TPC-H queries from the SaneQL repository run under `SET current_dialect='saneql'`
and match DuckDB's shipped answers at scale factor 0.01 (`test/sql/tpch/`). The extension is 24 grammar rules
(21 added, 3 replaced) and 753 code lines of C++; exact counts, the comparison with SaneQL's own translator and
with DuckDB's SQL front end, and per-query text sizes are in `MEASUREMENTS.md` (regenerate with
`scripts/measure.py`). Scalar expressions, identifiers, literals and the `List()`/`Parens()` macros are
DuckDB's own rules.

Not measured: parse-time overhead of the dialect (parsing is a negligible share of query time at any scale
where TPC-H is interesting) and the optional LLM generation-accuracy comparison from the plan.

## Dialect API observations

- **No tokenizer override was needed.** DuckDB's base tokenizer already emits `:=` as one compound token (SQL
  named arguments), `=>` as one token, `{` / `}` as single-byte operators (struct literals) and `&&`, `||`, `!` as
  operators. The `CreateTokenizer` hook stayed untouched.
- **Scalar expressions are reused wholesale.** The SaneQL grammar references DuckDB's `Expression` rule for
  every scalar, so casts (`'90 days'::interval`), arithmetic, comparisons and aggregate calls come for free.
  SaneQL-specific scalar syntax had to be added explicitly: `&&`/`||`/`!` for and/or/not (three rules replaced:
  the OR and AND tails and `NotKeyword`), `case({c => v, ...}, else:=e)`, brace lists `{a, b}` in scalar
  position, and `distinct:=true` as an aggregate argument.
- **Replacing an existing terminal needs the matcher-factory hook.** `&&`, `||` and `=>` are generic SQL
  operators (list overlap, string concatenation, named argument), so DuckDB's operator matcher consumed them
  before the logical-operator rules could see them. `ParsedGrammar::AddTerminalRuleOverride` only adds new
  terminals (it throws on `OperatorLiteral`, which already has one). The dialect instead overrides
  `CreateMatcherFactory` and swaps `input.terminal_rules["OperatorLiteral"]` for a subclass of DuckDB's
  `OperatorMatcher` that rejects those three tokens. Twelve lines, but it took a while to find the right hook.
- **The entry rule is spliced into `SelectAtom`.** That makes a SaneQL program valid anywhere a SELECT body
  is, including inside SQL subqueries and set operations, while plain SQL keeps working under the dialect. The
  transform returns `unique_ptr<SelectStatement>`, the same result type as `SelectParens`. A SaneQL pipeline is
  also prepended to `SingleExpression` as a scalar subquery, which is how `x > t.aggregate(max(y))` works.
- **Operator names are a closed keyword set.** `SaneqlOperatorName <- 'FILTER' / 'JOIN' / 'GROUPBY' / ...`
  rather than `Identifier`. This is what lets the grammar tell a table pipeline `orders.filter(...)` from a
  scalar method call `revenue.desc()` or a qualified column `n1.n_name` at the syntax level (see the UFCS note
  below). Words SQL does not know (`LET`, `GROUPBY`, `ORDERBY`, `PROJECT`) are registered as unreserved
  keywords with `AddChoice("UnreservedKeyword", ...)`.
- **SQL's reserved words leak into SaneQL positions.** `limit:=10` and `.join(...)` failed at first because
  `LIMIT` and `JOIN` are reserved in SQL and DuckDB's `Identifier` rule rejects them. Argument names use
  `ColLabel` (identifier or any keyword). Column and table names inside expressions still go through DuckDB's
  identifier rules, so a SaneQL column called `order` needs double quotes here while SaneQL itself has only six
  keywords. This is the keyword problem from the SaneQL paper, surfacing in the host.
- **Rule-name based tree walking.** Most rules (`SaneqlOperator`, `SaneqlArgument`, `SaneqlLet`, ...) have no
  transform of their own. The transforms for `SaneqlStatement`/`SaneqlQuery` walk the parse tree by rule name
  and call `PEGTransformer::Transform<T>` only for DuckDB rules (`Expression`, `ColLabel`). Parse-tree shape
  that had to be discovered by printing it: a rule match is a one-child LIST named after the rule; a choice rule
  has a CHOICE node inside that; macro expansions are LIST nodes named `Parens`/`List`; OPTIONAL and CHOICE
  nodes carry the name of what they wrap (this one caused a crash before it was understood). API contact
  surface: `AddRule`, `ReplaceRule`, `AddChoice`, `PrependChoice`, `GetRule`, `FinalizeTransformProcess`,
  `Transform<T>`, `MakeResult`, `CreateMatcherFactory`, `OperatorMatcher`, and the parse-result node types.
- **The host checkout moved under us.** Between milestones the `~/git/duckdb` working copy advanced and the
  dialect header was gone from its path; the pinned submodule is the only stable reference. Expected on a
  branch under active development, but it is the "API still changes" risk from the handoff, realised.

## Semantic mismatches

- **Pipeline order vs. SQL clause order.** SaneQL operators compose in any order; a SQL SELECT has a fixed
  clause order. The builder extends the current SELECT when the next operator fits (filter before groupby
  becomes WHERE, orderby after groupby becomes ORDER BY, joins extend FROM while the SELECT is still a plain
  scan) and otherwise wraps the current SELECT as a subquery and starts a new one (filter after groupby,
  project after orderby, join after filter). No HAVING is emitted; a post-aggregation filter is a WHERE over a
  subquery, which the optimizer treats identically. The resulting ASTs nest more subqueries than hand-written
  SQL would; the binder and optimizer flatten them, so this costs nothing at runtime.
- **`let` has three meanings and SQL has three different features for them.** A statement cannot carry a
  `CREATE MACRO`, so the plan to map `let` onto DuckDB macros does not work for statement-scoped bindings.
  Instead: a table-valued `let` becomes a CTE; a scalar `let` (`let base := '1993-10-01'::date`) is
  substituted into the expression tree; a parametrised `let f(x) := ...` is substituted at each call with the
  arguments replacing the parameters, producing a correlated scalar subquery (q2, q17, q20). SaneQL's own
  semantics for expression parameters are substitution, so this matches, but the substitution is scope-blind
  (any bare column reference with the binding's name is replaced). TPC-H never relies on shadowing.
- **`aggregate` is scalar-typed in expression context.** `partsupp_germany.aggregate(sum(x))` inside a filter
  (q11), `revenue.aggregate(max(total_revenue))` (q15) and a `let` whose body ends in `aggregate` (q22) are
  scalars in SaneQL because its semantic analysis knows the result has one row and one column. Here that
  decision is syntactic: a pipeline in expression position becomes a scalar subquery, and a parameterless `let`
  is a CTE unless its body's last operator is `aggregate`, in which case it is a scalar binding.
- **UFCS is type-directed in SaneQL, syntax-directed here.** `x.desc()`, `orders.filter(...)`, `n1.n_name`
  and `p_type.like('%BRASS')` are all `a.b(...)` or `a.b` to the parser. SaneQL resolves them by the type of
  `a`. The grammar resolves them by the method name: a closed set of table operators makes a pipeline;
  everything else is a SQL expression and goes to the binder.
- **`a.desc()` is a schema-qualified function call in SQL.** DuckDB parses `l_returnflag.desc()` as function
  `desc` in schema `l_returnflag` with zero arguments, and only `(expr).desc()` as the method call `desc(expr)`.
  SQL defers schema-vs-column disambiguation to the binder, so the parsed AST has two shapes for the same SaneQL
  construct. Every method the dialect must interpret itself (`desc`, `asc`, `like`, `between`, `in`, `extract`)
  handles both shapes; methods DuckDB knows as functions (`substr`) are left to the binder's own method
  resolution.
- **Methods without a function.** `like`, `between`, `in` and `extract(year)` are operators or special syntax
  in SQL, not functions, so the binder cannot resolve the method form. A post-pass over the finished statement
  rewrites them to the LIKE operator, a BetweenExpression, an IN list (when the argument is a brace list) or
  `date_part`. `count(x, distinct:=true)` becomes `count(DISTINCT x)` the same way. These are the places where
  SaneQL's "everything is a function" meets SQL's special forms.
- **`||` means or.** In SaneQL `||` is logical or; in SQL it is string concatenation. The dialect takes the
  SaneQL meaning everywhere, so concatenation needs `concat()` under this dialect. TPC-H uses neither.
- **Table-valued arguments are expressions until the operator decides.** A join operand parses as an
  expression (a bare name is a column reference, a pipeline a scalar subquery); the `join` transform reinterprets
  a bare name as a table and unwraps a scalar subquery into a table reference, keeping it a plain table when the
  pipeline was only `t.as(alias)`. SaneQL does this by type; here the operator's parameter kind does it.
- **Output shape differences.** SaneQL's `groupby` lists keys before aggregates, so q10's columns come out in a
  different order than the TPC-H reference, and the SaneQL q17 example stops before TPC-H's final
  `sum(...) / 7.0`. The tests wrap those two in a SQL SELECT (which works because a SaneQL program is a SELECT
  body). The upstream q5 example is missing the `c_nationkey = s_nationkey` predicate and carries a "gives the
  wrong result?" comment; the copy in `examples/tpch/q05.sane` adds the predicate and then matches.
- **Shell dot-commands.** The DuckDB CLI treats a line starting with `.` as a shell command, so the idiomatic
  multi-line SaneQL layout (one `.operator(...)` per line) cannot be typed into the shell line by line. Queries
  work when pasted as one statement, in files, and in sqllogictests. Not a parser issue.

## Not covered (not used by TPC-H)

`projectout`, `distinct`, `window`, the set operations, `defun`, `gensym`, `foreigncall`, `groupby(type:=,
sets:=)`, `orderby` null ordering, and SaneQL's `table`/`symbol`/`lambda` parameter kinds beyond what `let`
needs. Window functions and set operations map onto existing DuckDB AST nodes and would follow the same
pattern; `gensym` and `foreigncall` have no counterpart in the shared semantic layer.
