//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/postgres_type_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/postgres_catalog_set.hpp"
#include "storage/postgres_type_entry.hpp"

namespace duckdb {
struct CreateTableInfo;
class PostgresResult;
class PostgresSchemaEntry;
struct PGTypeInfo;

class PostgresTypeSet : public PostgresInSchemaSet {
public:
	explicit PostgresTypeSet(PostgresSchemaEntry &schema, unique_ptr<PostgresResultSlice> enum_result = nullptr,
	                         unique_ptr<PostgresResultSlice> composite_type_result = nullptr);

public:
	optional_ptr<CatalogEntry> CreateType(PostgresTransaction &transaction, CreateTypeInfo &info);

	static string GetInitializeEnumsQuery(PostgresVersion version, const string &schema = string(),
	                                      const string &type_name = string());
	static string GetInitializeCompositesQuery(const string &schema = string(), const string &type_name = string());

	optional_ptr<CatalogEntry> ReloadEntry(PostgresTransaction &transaction, const string &type_name) override;

protected:
	void LoadEntries(ClientContext &context, PostgresTransaction &transaction) override;
	bool SupportReload() const override {
		return true;
	}

	optional_ptr<CatalogEntry> CreateEnum(PostgresTransaction &transaction, PostgresResult &result, idx_t start_row,
	                                      idx_t end_row);
	optional_ptr<CatalogEntry> CreateCompositeType(PostgresTransaction &transaction, PostgresResult &result,
	                                               idx_t start_row, idx_t end_row);

protected:
	unique_ptr<PostgresResultSlice> enum_result;
	unique_ptr<PostgresResultSlice> composite_type_result;
};

} // namespace duckdb
