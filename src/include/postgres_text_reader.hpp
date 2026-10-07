//===----------------------------------------------------------------------===//
//                         DuckDB
//
// postgres_text_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "postgres_result_reader.hpp"
#include "duckdb/main/client_context.hpp"
#include "postgres_connection.hpp"
#include "postgres_result.hpp"

namespace duckdb {

struct PostgresTextReader : public PostgresResultReader {
	explicit PostgresTextReader(ClientContext &context, PostgresConnection &con, const vector<column_t> &column_ids,
	                            const PostgresBindData &bind_data);
	~PostgresTextReader() override;

public:
	void BeginCopy(ClientContext &context, const string &sql) override;
	PostgresReadResult Read(DataChunk &result) override;

	static void ConvertVector(ClientContext &context, Vector &source, Vector &target, const PostgresType &postgres_type,
	                          idx_t count);

private:
	void Reset();
	static void ConvertList(ClientContext &context, Vector &source, Vector &target, const PostgresType &postgres_type,
	                        idx_t count);
	static void ConvertStruct(ClientContext &context, Vector &source, Vector &target, const PostgresType &postgres_type,
	                          idx_t count);
	static void ConvertCTID(Vector &source, Vector &target, idx_t count);
	static void ConvertBlob(Vector &source, Vector &target, idx_t count);

private:
	ClientContext &context;
	DataChunk scan_chunk;
	unique_ptr<PostgresResult> result;
	idx_t row_offset = 0;
};

} // namespace duckdb
