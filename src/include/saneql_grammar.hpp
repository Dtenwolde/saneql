#pragma once

namespace duckdb {

//! SaneQL grammar fragment, spliced into DuckDB's PEG grammar through DialectExtension::ApplyGrammarChanges.
//! Scalar expressions, identifiers, literals and the List/Parens macros are DuckDB's own rules; only the operator
//! pipeline, `let` bindings and the few SaneQL scalar forms that SQL lacks are new. Rules whose name already exists
//! in the SQL grammar (the logical operator tails and NotKeyword) replace the SQL rule, the others are added.
static constexpr const char *SANEQL_GRAMMAR = R"GRAMMAR(
SaneqlStatement <- SaneqlLet* SaneqlQuery
SaneqlLet <- 'LET' ColLabel SaneqlLetParameters? ':=' Expression ','
SaneqlLetParameters <- Parens(List(ColLabel))
SaneqlQuery <- SaneqlSource SaneqlOperator+
SaneqlSource <- Identifier
SaneqlOperator <- '.' SaneqlOperatorName Parens(List(SaneqlArgument)?)
SaneqlOperatorName <- 'FILTER' / 'JOIN' / 'GROUPBY' / 'AGGREGATE' / 'ORDERBY' / 'PROJECT' / 'MAP' / 'AS'
SaneqlArgument <- SaneqlNamedArgument / SaneqlValue
SaneqlNamedArgument <- ColLabel ':=' SaneqlValue
SaneqlValue <- SaneqlList / Expression
SaneqlList <- '{' List(SaneqlArgument)? '}'
SaneqlScalarQuery <- SaneqlQuery
SaneqlBraceList <- '{' List(SaneqlBraceElement)? '}'
SaneqlBraceElement <- Expression
SaneqlCaseExpression <- 'CASE' Parens(SaneqlCaseArms SaneqlCaseElse?)
SaneqlCaseArms <- '{' List(SaneqlCaseArm) '}'
SaneqlCaseArm <- Expression '=>' Expression
SaneqlCaseElse <- ',' 'ELSE' ':=' Expression
SaneqlDistinctArgument <- 'DISTINCT' ':=' Expression
SaneqlOr <- 'OR' / '||'
SaneqlAnd <- 'AND' / '&&'
LogicalOrExpressionTail <- SaneqlOr LogicalAndExpression
LogicalAndExpressionTail <- SaneqlAnd LogicalNotExpression
NotKeyword <- 'NOT' / '!'
)GRAMMAR";

//! Words the SaneQL grammar uses as literals that SQL does not know as keywords.
static constexpr const char *SANEQL_KEYWORDS[] = {"LET", "GROUPBY", "ORDERBY", "PROJECT"};

} // namespace duckdb
