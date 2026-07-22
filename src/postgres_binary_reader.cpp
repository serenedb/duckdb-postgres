#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "postgres_binary_reader.hpp"
#include "postgres_scanner.hpp"

namespace duckdb {

PostgresBinaryReader::PostgresBinaryReader(PostgresConnection &con_p, const vector<column_t> &column_ids,
                                           const PostgresBindData &bind_data)
    : PostgresResultReader(con_p, column_ids, bind_data), parser(bind_data.types, bind_data.postgres_types) {
}

PostgresBinaryReader::~PostgresBinaryReader() {
	FreeBuffer();
}

void PostgresBinaryReader::BeginCopy(ClientContext &context, const string &sql) {
	con.BeginCopyFrom(context, sql, PGRES_COPY_OUT);
	if (!FetchNextBuffer()) {
		throw IOException("Failed to fetch header for COPY \"%s\"", sql);
	}
	parser.CheckHeader();
}

PostgresReadResult PostgresBinaryReader::Read(DataChunk &output) {
	while (output.size() < STANDARD_VECTOR_SIZE) {
		if (parser.ReadChunk(output, column_ids)) {
			return PostgresReadResult::HAVE_MORE_TUPLES;
		}
		FreeBuffer();
		if (!FetchNextBuffer()) {
			return PostgresReadResult::FINISHED;
		}
	}
	return PostgresReadResult::HAVE_MORE_TUPLES;
}

bool PostgresBinaryReader::FetchNextBuffer() {
	char *out_buffer;
	int len = PQgetCopyData(con.GetConn(), &out_buffer, 0);
	auto new_buffer = data_ptr_cast(out_buffer);

	// len -1 signals end
	if (len == -1) {
		// consume all available results
		while (true) {
			PostgresResult pg_res(PQgetResult(con.GetConn()));
			auto final_result = pg_res.res;
			if (!final_result) {
				break;
			}
			if (PQresultStatus(final_result) != PGRES_COMMAND_OK) {
				throw IOException("Failed to fetch header for COPY: %s", string(PQresultErrorMessage(final_result)));
			}
		}
		return false;
	}

	// len -2 is error
	// we expect at least 2 bytes in each message for the tuple count
	if (!new_buffer || len < sizeof(int16_t)) {
		throw IOException("Unable to read binary COPY data from Postgres: %s", string(PQerrorMessage(con.GetConn())));
	}
	buffer = new_buffer;
	parser.SetBuffer(buffer, len);
	return true;
}

void PostgresBinaryReader::FreeBuffer() {
	if (buffer) {
		PQfreemem(buffer);
	}
	buffer = nullptr;
}

PostgresParamBinaryReader::PostgresParamBinaryReader(PostgresConnection &con_p, const vector<column_t> &column_ids,
                                                     const PostgresBindData &bind_data)
    : PostgresResultReader(con_p, column_ids, bind_data), parser(bind_data.types, bind_data.postgres_types) {
}

void PostgresParamBinaryReader::BeginCopy(ClientContext &context, const string &sql) {
	auto result = con.Query(context, sql, bind_data.params, /*result_format=*/1);
	auto res = result->res;
	const int rows = PQntuples(res);
	const int fields = PQnfields(res);
	auto put_be16 = [&](uint16_t v) {
		buffer.push_back(static_cast<char>(v >> 8));
		buffer.push_back(static_cast<char>(v));
	};
	auto put_be32 = [&](uint32_t v) {
		buffer.push_back(static_cast<char>(v >> 24));
		buffer.push_back(static_cast<char>(v >> 16));
		buffer.push_back(static_cast<char>(v >> 8));
		buffer.push_back(static_cast<char>(v));
	};
	buffer.clear();
	for (int r = 0; r < rows; r++) {
		put_be16(static_cast<uint16_t>(fields));
		for (int c = 0; c < fields; c++) {
			if (PQgetisnull(res, r, c)) {
				put_be32(static_cast<uint32_t>(-1));
				continue;
			}
			const int len = PQgetlength(res, r, c);
			put_be32(static_cast<uint32_t>(len));
			const char *cell = PQgetvalue(res, r, c);
			buffer.insert(buffer.end(), cell, cell + len);
		}
	}
	parser.SetBuffer(data_ptr_cast(buffer.data()), buffer.size());
}

PostgresReadResult PostgresParamBinaryReader::Read(DataChunk &output) {
	if (parser.ReadChunk(output, column_ids)) {
		return PostgresReadResult::HAVE_MORE_TUPLES;
	}
	return PostgresReadResult::FINISHED;
}

} // namespace duckdb
