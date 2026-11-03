#pragma once

#ifndef DUCKDB_BUILD_LOADABLE_EXTENSION
#define DUCKDB_BUILD_LOADABLE_EXTENSION
#endif
#include "duckdb.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

class PostgresScannerExtension : public duckdb::Extension {
public:
	std::string Name() override {
		return "postgres_scanner";
	}
	void Load(duckdb::ExtensionLoader &loader) override;
	std::string Version() const override;

	static void OnConnectionOpened(duckdb::ClientContext &context);
};

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(postgres_scanner, loader);
}
