#include "saneql_dialect.hpp"
#include "saneql_grammar.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/peg/matcher/operator_matcher.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Parse tree access
//===--------------------------------------------------------------------===//

//! Collects, in source order, the parse results produced by rule `rule_name` below `pr`. Matches are not
//! descended into, so nested occurrences (lists inside lists) are left to the caller. Optional and choice nodes
//! carry the name of what they wrap, so they are looked through rather than matched.
void CollectRule(ParseResult &pr, const char *rule_name, vector<reference<ParseResult>> &out) {
	switch (pr.type) {
	case ParseResultType::OPTIONAL: {
		auto &opt = pr.Cast<OptionalParseResult>();
		if (opt.HasResult()) {
			CollectRule(opt.GetResult(), rule_name, out);
		}
		return;
	}
	case ParseResultType::CHOICE:
		CollectRule(pr.Cast<ChoiceParseResult>().GetResult(), rule_name, out);
		return;
	default:
		break;
	}
	if (pr.Name() == rule_name) {
		out.push_back(pr);
		return;
	}
	switch (pr.type) {
	case ParseResultType::LIST:
		for (auto &child : pr.Cast<ListParseResult>().GetChildren()) {
			CollectRule(child.get(), rule_name, out);
		}
		break;
	case ParseResultType::REPEAT:
		for (auto &child : pr.Cast<RepeatParseResult>().GetChildren()) {
			CollectRule(child.get(), rule_name, out);
		}
		break;
	default:
		break;
	}
}

//! Returns the first leaf of the given type below `pr`.
optional_ptr<ParseResult> FindLeaf(ParseResult &pr, ParseResultType leaf_type) {
	if (pr.type == leaf_type) {
		return &pr;
	}
	switch (pr.type) {
	case ParseResultType::LIST:
		for (auto &child : pr.Cast<ListParseResult>().GetChildren()) {
			auto found = FindLeaf(child.get(), leaf_type);
			if (found) {
				return found;
			}
		}
		return nullptr;
	case ParseResultType::OPTIONAL: {
		auto &opt = pr.Cast<OptionalParseResult>();
		return opt.HasResult() ? FindLeaf(opt.GetResult(), leaf_type) : nullptr;
	}
	case ParseResultType::CHOICE:
		return FindLeaf(pr.Cast<ChoiceParseResult>().GetResult(), leaf_type);
	default:
		return nullptr;
	}
}

Identifier GetIdentifier(ParseResult &pr) {
	auto leaf = FindLeaf(pr, ParseResultType::IDENTIFIER);
	if (!leaf) {
		throw InternalException("SaneQL: expected an identifier in rule '%s'", pr.Name());
	}
	return leaf->Cast<IdentifierParseResult>().identifier;
}

//! The (lower-cased) keyword literal matched below `pr`, e.g. the operator name.
string GetKeyword(ParseResult &pr) {
	auto leaf = FindLeaf(pr, ParseResultType::KEYWORD);
	if (!leaf) {
		throw InternalException("SaneQL: expected a keyword in rule '%s'", pr.Name());
	}
	return StringUtil::Lower(leaf->Cast<KeywordParseResult>().keyword);
}

//! The alternative a choice rule (`R <- A / B`) selected: the matcher wraps a rule match in a one-child LIST
//! named after the rule, with the CHOICE node inside it.
ParseResult &Alternative(ParseResult &rule_pr) {
	auto &choice = rule_pr.Cast<ListParseResult>().GetChild(0);
	if (choice.type != ParseResultType::CHOICE) {
		throw InternalException("SaneQL: expected a choice in rule '%s'", rule_pr.Name());
	}
	return choice.Cast<ChoiceParseResult>().GetResult();
}

//! Transforms the ColLabel below `pr` (an identifier or any keyword) into an Identifier.
Identifier GetLabel(PEGTransformer &transformer, ParseResult &pr) {
	vector<reference<ParseResult>> labels;
	CollectRule(pr, "ColLabel", labels);
	if (labels.size() != 1) {
		throw InternalException("SaneQL: expected a label in rule '%s'", pr.Name());
	}
	return transformer.Transform<Identifier>(labels[0].get());
}

unique_ptr<ParsedExpression> TransformExpression(PEGTransformer &transformer, ParseResult &pr) {
	return transformer.Transform<unique_ptr<ParsedExpression>>(pr);
}

unique_ptr<SelectStatement> TransformStatement(PEGTransformer &transformer, ParseResult &pr) {
	return transformer.Transform<unique_ptr<SelectStatement>>(pr);
}

//===--------------------------------------------------------------------===//
// Expression helpers
//===--------------------------------------------------------------------===//

//! A bare, unqualified name: SaneQL symbols (join types, aliases, table references) arrive as these.
bool IsBareName(const ParsedExpression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::COLUMN_REF && !expr.Cast<ColumnRefExpression>().IsQualified();
}

const Identifier &BareName(const ParsedExpression &expr) {
	return expr.Cast<ColumnRefExpression>().ColumnNames()[0];
}

//! A SELECT * over a single table reference without any clause: the shape every pipeline starts with.
bool IsPlainScan(const SelectNode &node) {
	if (node.select_list.size() != 1 || node.select_list[0]->GetExpressionClass() != ExpressionClass::STAR ||
	    node.select_list[0]->ToString() != "*") {
		return false;
	}
	return node.from_table && !node.where_clause && node.groups.group_expressions.empty() &&
	       node.groups.grouping_sets.empty() && !node.having && !node.qualify && !node.sample &&
	       node.modifiers.empty() && node.cte_map.map.empty();
}

//! A pipeline used as a table: a plain scan contributes its table reference, anything else becomes a subquery.
unique_ptr<TableRef> StatementToTableRef(unique_ptr<SelectStatement> statement) {
	if (statement->node->type == QueryNodeType::SELECT_NODE) {
		auto &node = statement->node->Cast<SelectNode>();
		if (IsPlainScan(node)) {
			return std::move(node.from_table);
		}
	}
	return make_uniq<SubqueryRef>(std::move(statement));
}

unique_ptr<TableRef> ToTableRef(const string &op, unique_ptr<ParsedExpression> expr) {
	if (IsBareName(*expr)) {
		auto table_ref = make_uniq<BaseTableRef>();
		table_ref->SetTable(BareName(*expr));
		return std::move(table_ref);
	}
	if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return StatementToTableRef(std::move(expr->Cast<SubqueryExpression>().SubqueryMutable()));
	}
	throw ParserException("SaneQL: %s expects a table, got %s", op, expr->ToString());
}

JoinType ToJoinType(const string &op, const string &symbol) {
	auto lowered = StringUtil::Lower(symbol);
	if (lowered == "inner") {
		return JoinType::INNER;
	} else if (lowered == "leftouter") {
		return JoinType::LEFT;
	} else if (lowered == "rightouter") {
		return JoinType::RIGHT;
	} else if (lowered == "fullouter") {
		return JoinType::OUTER;
	} else if (lowered == "leftsemi" || lowered == "exists") {
		return JoinType::SEMI;
	} else if (lowered == "leftanti" || lowered == "notexists") {
		return JoinType::ANTI;
	}
	throw ParserException("SaneQL: unsupported %s type '%s'", op, symbol);
}

//===--------------------------------------------------------------------===//
// Arguments
//===--------------------------------------------------------------------===//

//! One operator argument or list element: `name:=` binding (optional), then either a `{...}` list or an expression.
struct SaneqlArg {
	string name;
	bool is_list = false;
	vector<SaneqlArg> elements;
	unique_ptr<ParsedExpression> expr;
};

SaneqlArg ParseArgument(PEGTransformer &transformer, ParseResult &argument_pr);

SaneqlArg ParseValue(PEGTransformer &transformer, ParseResult &value_pr) {
	SaneqlArg arg;
	auto &value = Alternative(value_pr);
	if (value.Name() == "SaneqlList") {
		arg.is_list = true;
		vector<reference<ParseResult>> elements;
		CollectRule(value, "SaneqlArgument", elements);
		for (auto &element : elements) {
			arg.elements.push_back(ParseArgument(transformer, element.get()));
		}
	} else {
		arg.expr = TransformExpression(transformer, value);
	}
	return arg;
}

SaneqlArg ParseArgument(PEGTransformer &transformer, ParseResult &argument_pr) {
	auto &argument = Alternative(argument_pr);
	if (argument.Name() == "SaneqlNamedArgument") {
		// ColLabel ':=' SaneqlValue
		auto &list = argument.Cast<ListParseResult>();
		auto arg = ParseValue(transformer, list.GetChild(2));
		arg.name = GetLabel(transformer, list.GetChild(0)).GetIdentifierName();
		return arg;
	}
	return ParseValue(transformer, argument);
}

vector<SaneqlArg> ParseArguments(PEGTransformer &transformer, ParseResult &arguments_pr) {
	vector<reference<ParseResult>> argument_prs;
	CollectRule(arguments_pr, "SaneqlArgument", argument_prs);
	vector<SaneqlArg> args;
	for (auto &argument_pr : argument_prs) {
		args.push_back(ParseArgument(transformer, argument_pr.get()));
	}
	return args;
}

//! Resolves a parameter by name, else by position among the positional (unnamed) arguments.
optional_ptr<SaneqlArg> FindArg(vector<SaneqlArg> &args, const char *name, idx_t position) {
	for (auto &arg : args) {
		if (StringUtil::CIEquals(arg.name, name)) {
			return &arg;
		}
	}
	idx_t positional = 0;
	for (auto &arg : args) {
		if (!arg.name.empty()) {
			continue;
		}
		if (positional == position) {
			return &arg;
		}
		positional++;
	}
	return nullptr;
}

SaneqlArg &RequireArg(const string &op, vector<SaneqlArg> &args, const char *name, idx_t position) {
	auto arg = FindArg(args, name, position);
	if (!arg) {
		throw ParserException("SaneQL: %s requires a '%s' argument", op, name);
	}
	return *arg;
}

//! A list argument `{a, b:=c}` as expressions, bindings applied as aliases. A bare expression counts as a list of one.
vector<unique_ptr<ParsedExpression>> TakeExpressionList(const string &op, SaneqlArg &arg) {
	vector<unique_ptr<ParsedExpression>> result;
	if (!arg.is_list) {
		result.push_back(std::move(arg.expr));
		return result;
	}
	for (auto &element : arg.elements) {
		if (element.is_list) {
			throw ParserException("SaneQL: %s does not accept nested lists", op);
		}
		if (!element.name.empty()) {
			element.expr->SetAlias(Identifier(element.name));
		}
		result.push_back(std::move(element.expr));
	}
	return result;
}

unique_ptr<ParsedExpression> TakeExpression(const string &op, SaneqlArg &arg) {
	if (arg.is_list) {
		throw ParserException("SaneQL: %s expects an expression, not a list", op);
	}
	return std::move(arg.expr);
}

//! A symbol argument (`type:=leftouter`, `as(n1)`) is parsed as a bare column reference.
string TakeSymbol(const string &op, SaneqlArg &arg) {
	if (arg.is_list || !IsBareName(*arg.expr)) {
		throw ParserException("SaneQL: %s expects a symbol", op);
	}
	return BareName(*arg.expr).GetIdentifierName();
}

//===--------------------------------------------------------------------===//
// Query builder
//===--------------------------------------------------------------------===//

//! Builds a SelectNode incrementally while walking the operator chain. Each SaneQL operator either extends the
//! current SELECT (when SQL's clause order allows it) or wraps it as a subquery and starts a fresh one.
class SaneqlQueryBuilder {
public:
	void Source(Identifier table) {
		auto table_ref = make_uniq<BaseTableRef>();
		table_ref->SetTable(std::move(table));
		Start(std::move(table_ref));
	}

	void Apply(const string &op, vector<SaneqlArg> args) {
		if (op == "filter") {
			Filter(TakeExpression(op, RequireArg(op, args, "condition", 0)));
		} else if (op == "join") {
			auto right = ToTableRef(op, TakeExpression(op, RequireArg(op, args, "other", 0)));
			auto condition = TakeExpression(op, RequireArg(op, args, "condition", 1));
			auto type = JoinType::INNER;
			if (auto arg = FindArg(args, "type", 2)) {
				type = ToJoinType(op, TakeSymbol(op, *arg));
			}
			Join(std::move(right), std::move(condition), type);
		} else if (op == "as") {
			Alias(Identifier(TakeSymbol(op, RequireArg(op, args, "binding", 0))));
		} else if (op == "groupby") {
			auto groups = TakeExpressionList(op, RequireArg(op, args, "groups", 0));
			vector<unique_ptr<ParsedExpression>> aggregates;
			if (auto arg = FindArg(args, "aggregates", 1)) {
				aggregates = TakeExpressionList(op, *arg);
			}
			GroupBy(std::move(groups), std::move(aggregates));
		} else if (op == "aggregate") {
			GroupBy({}, TakeExpressionList(op, RequireArg(op, args, "aggregates", 0)));
		} else if (op == "orderby") {
			auto orders = TakeExpressionList(op, RequireArg(op, args, "expressions", 0));
			unique_ptr<ParsedExpression> limit;
			unique_ptr<ParsedExpression> offset;
			if (auto arg = FindArg(args, "limit", 1)) {
				limit = TakeExpression(op, *arg);
			}
			if (auto arg = FindArg(args, "offset", 2)) {
				offset = TakeExpression(op, *arg);
			}
			OrderBy(std::move(orders), std::move(limit), std::move(offset));
		} else if (op == "project") {
			Project(TakeExpressionList(op, RequireArg(op, args, "expressions", 0)), false);
		} else if (op == "map") {
			Project(TakeExpressionList(op, RequireArg(op, args, "expressions", 0)), true);
		} else {
			throw ParserException("SaneQL: unsupported operator '%s'", op);
		}
	}

	unique_ptr<SelectStatement> Finish() {
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(node);
		return statement;
	}

private:
	void Start(unique_ptr<TableRef> from) {
		node = make_uniq<SelectNode>();
		node->from_table = std::move(from);
		node->select_list.push_back(make_uniq<StarExpression>());
	}

	//! Turns the current SELECT into a subquery and starts a new SELECT * over it.
	void Wrap() {
		Start(make_uniq<SubqueryRef>(Finish()));
	}

	bool IsPlain() const {
		return IsPlainScan(*node);
	}

	bool HasProjection() const {
		return node->select_list.size() != 1 || node->select_list[0]->GetExpressionClass() != ExpressionClass::STAR;
	}

	void Filter(unique_ptr<ParsedExpression> condition) {
		if (HasProjection() || !node->modifiers.empty()) {
			Wrap();
		}
		if (node->where_clause) {
			node->where_clause = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
			                                                      std::move(node->where_clause), std::move(condition));
		} else {
			node->where_clause = std::move(condition);
		}
	}

	void Join(unique_ptr<TableRef> right, unique_ptr<ParsedExpression> condition, JoinType type) {
		if (!IsPlain()) {
			Wrap();
		}
		auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
		join->type = type;
		join->left = std::move(node->from_table);
		join->right = std::move(right);
		join->condition = std::move(condition);
		node->from_table = std::move(join);
	}

	void Alias(Identifier alias) {
		if (!IsPlain() || node->from_table->type == TableReferenceType::JOIN) {
			Wrap();
		}
		node->from_table->alias = std::move(alias);
	}

	void GroupBy(vector<unique_ptr<ParsedExpression>> groups, vector<unique_ptr<ParsedExpression>> aggregates) {
		if (HasProjection() || !node->modifiers.empty()) {
			Wrap();
		}
		node->select_list.clear();
		GroupingSet grouping_set;
		for (idx_t i = 0; i < groups.size(); i++) {
			auto group = groups[i]->Copy();
			group->SetAlias(Identifier());
			node->groups.group_expressions.push_back(std::move(group));
			grouping_set.insert(ProjectionIndex(i));
			node->select_list.push_back(std::move(groups[i]));
		}
		if (!grouping_set.empty()) {
			node->groups.grouping_sets.push_back(std::move(grouping_set));
		}
		for (auto &aggregate : aggregates) {
			node->select_list.push_back(std::move(aggregate));
		}
	}

	//! `expr.desc()` / `expr.asc()` set the order direction. DuckDB parses `a.desc()` as a function `desc` in
	//! schema `a` (it resolves schema-vs-column in the binder) and `(a + 1).desc()` as a method call `desc(a + 1)`,
	//! so both shapes are unwrapped here.
	static OrderByNode ToOrderByNode(unique_ptr<ParsedExpression> expr) {
		auto type = OrderType::ORDER_DEFAULT;
		if (expr->GetExpressionClass() == ExpressionClass::FUNCTION) {
			auto &function = expr->Cast<FunctionExpression>();
			auto &qualified_name = function.GetQualifiedName();
			auto name = StringUtil::Lower(qualified_name.Name().GetIdentifierName());
			auto &arguments = function.GetArgumentsMutable();
			if (name == "desc" || name == "asc") {
				unique_ptr<ParsedExpression> receiver;
				if (arguments.size() == 1 && !arguments[0].HasName() && qualified_name.Path().size() == 1) {
					receiver = std::move(arguments[0].GetExpressionMutable());
				} else if (arguments.empty() && qualified_name.Path().size() > 1) {
					vector<Identifier> column(qualified_name.Path().begin(), qualified_name.Path().end() - 1);
					receiver = make_uniq<ColumnRefExpression>(std::move(column));
				}
				if (receiver) {
					type = name == "desc" ? OrderType::DESCENDING : OrderType::ASCENDING;
					expr = std::move(receiver);
				}
			}
		}
		return OrderByNode(type, OrderByNullType::ORDER_DEFAULT, std::move(expr));
	}

	void OrderBy(vector<unique_ptr<ParsedExpression>> orders, unique_ptr<ParsedExpression> limit,
	             unique_ptr<ParsedExpression> offset) {
		if (!node->modifiers.empty()) {
			Wrap();
		}
		auto order_modifier = make_uniq<OrderModifier>();
		for (auto &order : orders) {
			order_modifier->orders.push_back(ToOrderByNode(std::move(order)));
		}
		node->modifiers.push_back(std::move(order_modifier));
		if (limit || offset) {
			auto limit_modifier = make_uniq<LimitModifier>();
			limit_modifier->limit = std::move(limit);
			limit_modifier->offset = std::move(offset);
			node->modifiers.push_back(std::move(limit_modifier));
		}
	}

	void Project(vector<unique_ptr<ParsedExpression>> expressions, bool keep_existing) {
		if (HasProjection() || !node->modifiers.empty()) {
			Wrap();
		}
		if (!keep_existing) {
			node->select_list.clear();
		}
		for (auto &expression : expressions) {
			node->select_list.push_back(std::move(expression));
		}
	}

private:
	unique_ptr<SelectNode> node;
};

//===--------------------------------------------------------------------===//
// Rewriting: let substitution and SaneQL methods that SQL spells differently
//===--------------------------------------------------------------------===//

//! Walks a finished statement. Replaces references to `let` bindings (scalar values by their expression, calls of
//! parametrised bindings by their body with the arguments substituted) and rewrites SaneQL method calls that have
//! no function counterpart in DuckDB: like, between, in, extract, and `distinct:=true` aggregate arguments.
class SaneqlRewriter {
public:
	struct FunctionBinding {
		vector<string> parameters;
		unique_ptr<ParsedExpression> body;
	};

	case_insensitive_map_t<unique_ptr<ParsedExpression>> scalars;
	case_insensitive_map_t<FunctionBinding> functions;

public:
	void RewriteStatement(SelectStatement &statement) {
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *statement.node, [&](unique_ptr<ParsedExpression> &expr) { RewriteExpression(expr); });
	}

	void RewriteExpression(unique_ptr<ParsedExpression> &expr) {
		if (!expr) {
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expr, [&](unique_ptr<ParsedExpression> &child) { RewriteExpression(child); });
		switch (expr->GetExpressionClass()) {
		case ExpressionClass::SUBQUERY:
			RewriteStatement(*expr->Cast<SubqueryExpression>().SubqueryMutable());
			break;
		case ExpressionClass::COLUMN_REF:
			if (IsBareName(*expr)) {
				auto entry = scalars.find(BareName(*expr).GetIdentifierName());
				if (entry != scalars.end()) {
					Replace(expr, entry->second->Copy());
				}
			}
			break;
		case ExpressionClass::FUNCTION:
			RewriteFunction(expr);
			break;
		default:
			break;
		}
	}

private:
	static void Replace(unique_ptr<ParsedExpression> &expr, unique_ptr<ParsedExpression> replacement) {
		if (expr->HasAlias()) {
			replacement->SetAlias(expr->GetAlias());
		}
		expr = std::move(replacement);
	}

	void RewriteFunction(unique_ptr<ParsedExpression> &expr) {
		auto &function = expr->Cast<FunctionExpression>();
		auto &qualified_name = function.GetQualifiedName();
		auto name = StringUtil::Lower(qualified_name.Name().GetIdentifierName());
		auto &arguments = function.GetArgumentsMutable();

		// a call of a parametrised let binding: substitute the arguments into a copy of the body
		if (qualified_name.Path().size() == 1) {
			auto entry = functions.find(name);
			if (entry != functions.end()) {
				auto &binding = entry->second;
				if (arguments.size() != binding.parameters.size()) {
					throw ParserException("SaneQL: %s expects %llu arguments, got %llu", name,
					                      binding.parameters.size(), arguments.size());
				}
				SaneqlRewriter substitution;
				for (idx_t i = 0; i < arguments.size(); i++) {
					substitution.scalars[binding.parameters[i]] = std::move(arguments[i].GetExpressionMutable());
				}
				auto body = binding.body->Copy();
				substitution.RewriteExpression(body);
				Replace(expr, std::move(body));
				return;
			}
		}

		// `count(x, distinct:=true)`
		for (idx_t i = 0; i < arguments.size(); i++) {
			if (arguments[i].HasName() &&
			    StringUtil::CIEquals(arguments[i].GetName().GetIdentifierName(), "distinct")) {
				function.DistinctMutable() = true;
				arguments.erase(arguments.begin() + i);
				break;
			}
		}

		// SaneQL methods without a DuckDB function of the same name. `a.like(p)` arrives as function `like` in
		// schema `a` with one argument, `(a).like(p)` as `like(a, p)`: both are normalised to receiver + arguments.
		static const case_insensitive_map_t<idx_t> METHOD_ARITY {
		    {"like", 2}, {"between", 3}, {"in", 2}, {"extract", 2}};
		auto method = METHOD_ARITY.find(name);
		if (method == METHOD_ARITY.end()) {
			return;
		}
		bool qualified = qualified_name.Path().size() > 1;
		if (arguments.size() + (qualified ? 1 : 0) != method->second) {
			return;
		}
		for (auto &argument : arguments) {
			if (argument.HasName()) {
				return;
			}
		}
		vector<unique_ptr<ParsedExpression>> args;
		if (qualified) {
			vector<Identifier> column(qualified_name.Path().begin(), qualified_name.Path().end() - 1);
			args.push_back(make_uniq<ColumnRefExpression>(std::move(column)));
		}
		for (auto &argument : arguments) {
			args.push_back(std::move(argument.GetExpressionMutable()));
		}

		unique_ptr<ParsedExpression> replacement;
		if (name == "like") {
			replacement =
			    make_uniq<FunctionExpression>(Identifier("~~"), std::move(args), nullptr, nullptr, false, true);
		} else if (name == "between") {
			replacement = make_uniq<BetweenExpression>(std::move(args[0]), std::move(args[1]), std::move(args[2]));
		} else if (name == "in") {
			auto &candidates = *args[1];
			if (candidates.GetExpressionClass() == ExpressionClass::FUNCTION &&
			    StringUtil::CIEquals(candidates.Cast<FunctionExpression>().FunctionName().GetIdentifierName(),
			                         "list_value")) {
				// x.in({a, b, c}) is x IN (a, b, c)
				vector<unique_ptr<ParsedExpression>> children;
				children.push_back(std::move(args[0]));
				for (auto &candidate : candidates.Cast<FunctionExpression>().GetArgumentsMutable()) {
					children.push_back(std::move(candidate.GetExpressionMutable()));
				}
				replacement = make_uniq<OperatorExpression>(ExpressionType::COMPARE_IN, std::move(children));
			} else {
				vector<unique_ptr<ParsedExpression>> children;
				children.push_back(std::move(args[1]));
				children.push_back(std::move(args[0]));
				replacement = make_uniq<FunctionExpression>(Identifier("list_contains"), std::move(children));
			}
		} else if (name == "extract") {
			if (!IsBareName(*args[1])) {
				throw ParserException("SaneQL: extract expects a date part symbol");
			}
			vector<unique_ptr<ParsedExpression>> children;
			children.push_back(ConstantExpression::String(BareName(*args[1]).GetIdentifierName()));
			children.push_back(std::move(args[0]));
			replacement = make_uniq<FunctionExpression>(Identifier("date_part"), std::move(children));
		}
		Replace(expr, std::move(replacement));
	}
};

//===--------------------------------------------------------------------===//
// Transforms
//===--------------------------------------------------------------------===//

// SaneqlQuery <- SaneqlSource SaneqlOperator+
arena_ptr<TransformResultValue> TransformSaneqlQuery(PEGTransformer &transformer, ParseResult &parse_result) {
	if (getenv("SANEQL_DEBUG_PARSE")) {
		Printer::Print(parse_result.ToString());
	}
	auto &list = parse_result.Cast<ListParseResult>();
	SaneqlQueryBuilder builder;
	builder.Source(GetIdentifier(list.GetChild(0)));

	vector<reference<ParseResult>> operators;
	CollectRule(list.GetChild(1), "SaneqlOperator", operators);
	for (auto &operator_pr : operators) {
		// '.' SaneqlOperatorName Parens(List(SaneqlArgument)?)
		auto &operator_list = operator_pr.get().Cast<ListParseResult>();
		auto name = GetKeyword(operator_list.GetChild(1));
		builder.Apply(name, ParseArguments(transformer, operator_list.GetChild(2)));
	}
	return transformer.MakeResult<unique_ptr<SelectStatement>>(builder.Finish());
}

//! Whether a let body `t.op(...).aggregate(...)` ends in aggregate. SaneQL types such a body as a scalar (semantic
//! analysis decides by type); here the decision is syntactic.
bool EndsWithAggregate(ParseResult &body_pr) {
	vector<reference<ParseResult>> operators;
	CollectRule(body_pr, "SaneqlOperator", operators);
	return !operators.empty() &&
	       GetKeyword(operators.back().get().Cast<ListParseResult>().GetChild(1)) == "aggregate";
}

// SaneqlStatement <- SaneqlLet* SaneqlQuery
arena_ptr<TransformResultValue> TransformSaneqlStatement(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	SaneqlRewriter rewriter;
	vector<pair<Identifier, unique_ptr<SelectStatement>>> tables;

	vector<reference<ParseResult>> lets;
	CollectRule(list.GetChild(0), "SaneqlLet", lets);
	for (auto &let_pr : lets) {
		// 'LET' ColLabel SaneqlLetParameters? ':=' Expression ','
		auto &let = let_pr.get().Cast<ListParseResult>();
		auto name = GetLabel(transformer, let.GetChild(1)).GetIdentifierName();
		vector<string> parameters;
		auto &parameters_pr = let.Child<OptionalParseResult>(2);
		if (parameters_pr.HasResult()) {
			vector<reference<ParseResult>> labels;
			CollectRule(parameters_pr.GetResult(), "ColLabel", labels);
			for (auto &label : labels) {
				parameters.push_back(transformer.Transform<Identifier>(label.get()).GetIdentifierName());
			}
		}
		auto body = TransformExpression(transformer, let.GetChild(4));
		// earlier bindings are visible in the body
		rewriter.RewriteExpression(body);
		if (!parameters.empty()) {
			rewriter.functions[name] = SaneqlRewriter::FunctionBinding {std::move(parameters), std::move(body)};
		} else if (body->GetExpressionClass() == ExpressionClass::SUBQUERY && !EndsWithAggregate(let.GetChild(4))) {
			// a table-valued binding becomes a CTE
			tables.emplace_back(Identifier(name), std::move(body->Cast<SubqueryExpression>().SubqueryMutable()));
		} else {
			rewriter.scalars[name] = std::move(body);
		}
	}

	auto statement = TransformStatement(transformer, list.GetChild(1));
	rewriter.RewriteStatement(*statement);
	for (auto &table : tables) {
		auto info = make_uniq<CommonTableExpressionInfo>();
		info->query_node = std::move(table.second->node);
		statement->node->cte_map.map.insert(table.first, std::move(info));
	}
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(statement));
}

// SaneqlScalarQuery <- SaneqlQuery
arena_ptr<TransformResultValue> TransformSaneqlScalarQuery(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto subquery = make_uniq<SubqueryExpression>();
	subquery->SubqueryMutable() = TransformStatement(transformer, list.GetChild(0));
	subquery->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(subquery));
}

// SaneqlBraceList <- '{' List(SaneqlBraceElement)? '}'
arena_ptr<TransformResultValue> TransformSaneqlBraceList(PEGTransformer &transformer, ParseResult &parse_result) {
	vector<reference<ParseResult>> elements;
	CollectRule(parse_result, "SaneqlBraceElement", elements);
	vector<unique_ptr<ParsedExpression>> values;
	for (auto &element : elements) {
		values.push_back(TransformExpression(transformer, element.get().Cast<ListParseResult>().GetChild(0)));
	}
	auto result = make_uniq<FunctionExpression>(Identifier("list_value"), std::move(values));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

// SaneqlCaseExpression <- 'CASE' Parens(SaneqlCaseArms SaneqlCaseElse?)
arena_ptr<TransformResultValue> TransformSaneqlCaseExpression(PEGTransformer &transformer,
                                                              ParseResult &parse_result) {
	auto result = make_uniq<CaseExpression>();
	vector<reference<ParseResult>> arms;
	CollectRule(parse_result, "SaneqlCaseArm", arms);
	for (auto &arm_pr : arms) {
		// Expression '=>' Expression
		auto &arm = arm_pr.get().Cast<ListParseResult>();
		CaseCheck check;
		check.when_expr = TransformExpression(transformer, arm.GetChild(0));
		check.then_expr = TransformExpression(transformer, arm.GetChild(2));
		result->CaseChecksMutable().push_back(std::move(check));
	}
	vector<reference<ParseResult>> else_prs;
	CollectRule(parse_result, "SaneqlCaseElse", else_prs);
	if (!else_prs.empty()) {
		// ',' 'ELSE' ':=' Expression
		result->ElseMutable() =
		    TransformExpression(transformer, else_prs[0].get().Cast<ListParseResult>().GetChild(3));
	} else {
		result->ElseMutable() = ConstantExpression::Null();
	}
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

// SaneqlDistinctArgument <- 'DISTINCT' ':=' Expression
arena_ptr<TransformResultValue> TransformSaneqlDistinctArgument(PEGTransformer &transformer,
                                                                ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	FunctionArgument argument(Identifier("distinct"), TransformExpression(transformer, list.GetChild(2)));
	return transformer.MakeResult<FunctionArgument>(std::move(argument));
}

// LogicalOrExpressionTail <- SaneqlOr LogicalAndExpression
// LogicalAndExpressionTail <- SaneqlAnd LogicalNotExpression
arena_ptr<TransformResultValue> TransformLogicalTail(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(TransformExpression(transformer, list.GetChild(1)));
}

// NotKeyword <- 'NOT' / '!'
arena_ptr<TransformResultValue> TransformNotKeyword(PEGTransformer &transformer, ParseResult &) {
	return transformer.MakeResult<bool>(true);
}

using finalize_function_t = arena_ptr<TransformResultValue> (*)(PEGTransformer &, ParseResult &);

template <finalize_function_t FINALIZE>
arena_ptr<TransformProcess> Start(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FINALIZE);
}

//! The SQL operator matcher minus the tokens SaneQL gives a meaning of its own: `&&`/`||` are logical and/or (SQL
//! would read them as list overlap and string concatenation), `=>` separates case arms.
class SaneqlOperatorMatcher final : public OperatorMatcher {
public:
	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (token && (token->text == "&&" || token->text == "||" || token->text == "=>")) {
			return MatcherResult::Failure();
		}
		return OperatorMatcher::MatchAtomic(state);
	}
};

} // namespace

void SaneqlDialect::ApplyGrammarChanges(GrammarChangesInput &input) {
	auto &grammar = input.parsed_grammar;
	static const case_insensitive_map_t<grammar_transform_process_function_t> TRANSFORMS {
	    {"SaneqlStatement", Start<TransformSaneqlStatement>},
	    {"SaneqlQuery", Start<TransformSaneqlQuery>},
	    {"SaneqlScalarQuery", Start<TransformSaneqlScalarQuery>},
	    {"SaneqlBraceList", Start<TransformSaneqlBraceList>},
	    {"SaneqlCaseExpression", Start<TransformSaneqlCaseExpression>},
	    {"SaneqlDistinctArgument", Start<TransformSaneqlDistinctArgument>},
	    {"LogicalOrExpressionTail", Start<TransformLogicalTail>},
	    {"LogicalAndExpressionTail", Start<TransformLogicalTail>},
	    {"NotKeyword", Start<TransformNotKeyword>},
	};

	for (auto &keyword : SANEQL_KEYWORDS) {
		grammar.AddChoice("UnreservedKeyword", StringUtil::Format("'%s'", keyword));
	}
	for (auto &rule : StringUtil::Split(SANEQL_GRAMMAR, '\n')) {
		if (rule.empty()) {
			continue;
		}
		auto name = rule.substr(0, rule.find(' '));
		auto transform = TRANSFORMS.find(name);
		grammar_transform_process_function_t process;
		if (transform != TRANSFORMS.end()) {
			process = transform->second;
		}
		if (grammar.GetRule(name)) {
			grammar.ReplaceRule(rule, process);
		} else {
			grammar.AddRule(rule, process);
		}
	}
	// A SaneQL program is accepted wherever a SELECT body is
	grammar.PrependChoice("SelectAtom", "SaneqlStatement");
	// SaneQL forms inside scalar expressions: a pipeline as scalar subquery, `case({c => v}, else:=e)`, `{a, b}` lists
	grammar.PrependChoice("SingleExpression", "SaneqlScalarQuery");
	grammar.PrependChoice("SingleExpression", "SaneqlCaseExpression");
	grammar.PrependChoice("SingleExpression", "SaneqlBraceList");
	// `count(x, distinct:=true)`
	grammar.PrependChoice("FunctionArgument", "SaneqlDistinctArgument");
}

unique_ptr<MatcherFactory> SaneqlDialect::CreateMatcherFactory(CreateMatcherFactoryInput &input) {
	// replace the SQL operator terminal: the grammar-level override API only adds new terminals
	input.terminal_rules["OperatorLiteral"] = make_uniq<SaneqlOperatorMatcher>();
	return DialectExtension::CreateMatcherFactory(input);
}

} // namespace duckdb
