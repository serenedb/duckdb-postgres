//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/postgres_index_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/postgres_catalog_set.hpp"
#include "storage/postgres_index_entry.hpp"

namespace duckdb {
class PostgresSchemaEntry;
class TableCatalogEntry;

class PostgresIndexSet : public PostgresInSchemaSet {
public:
	PostgresIndexSet(PostgresSchemaEntry &schema, unique_ptr<PostgresResultSlice> index_result = nullptr);

public:
	static string GetInitializeQuery(const string &schema = string(), const string &index_name = string());

	optional_ptr<CatalogEntry> CreateIndex(PostgresTransaction &transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table);

	optional_ptr<CatalogEntry> ReloadEntry(PostgresTransaction &transaction, const string &index_name) override;

protected:
	void LoadEntries(ClientContext &context, PostgresTransaction &transaction) override;
	bool SupportReload() const override {
		return true;
	}

	optional_ptr<CatalogEntry> CreateIndexEntry(PostgresTransaction &transaction, PostgresResult &result, idx_t row);

protected:
	unique_ptr<PostgresResultSlice> index_result;
};

} // namespace duckdb
