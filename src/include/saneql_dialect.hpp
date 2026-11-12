#pragma once

#include "duckdb/parser/peg/dialect_extension.hpp"

namespace duckdb {

//! The SaneQL dialect: selected with SET current_dialect = 'saneql'.
class SaneqlDialect final : public DialectExtension {
public:
	SaneqlDialect() : DialectExtension("saneql") {
	}

	void ApplyGrammarChanges(GrammarChangesInput &input) override;
	unique_ptr<MatcherFactory> CreateMatcherFactory(CreateMatcherFactoryInput &input) override;
};

} // namespace duckdb
