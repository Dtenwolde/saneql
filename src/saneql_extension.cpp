#define DUCKDB_EXTENSION_MAIN

#include "saneql_extension.hpp"
#include "saneql_dialect.hpp"

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	DialectExtension::Register(config, make_shared_ptr<SaneqlDialect>());
}

void SaneqlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string SaneqlExtension::Name() {
	return "saneql";
}

std::string SaneqlExtension::Version() const {
#ifdef EXT_VERSION_SANEQL
	return EXT_VERSION_SANEQL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(saneql, loader) {
	duckdb::LoadInternal(loader);
}
}
