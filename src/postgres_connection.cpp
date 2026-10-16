#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parser.hpp"
#include "postgres_connection.hpp"
#include "postgres_logging.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

OwnedPostgresConnection::OwnedPostgresConnection(PGconn *conn) : connection(conn) {
}

OwnedPostgresConnection::~OwnedPostgresConnection() {
	if (!connection) {
		return;
	}
	PQfinish(connection);
	connection = nullptr;
}

PostgresConnection::PostgresConnection(shared_ptr<OwnedPostgresConnection> connection_p)
    : connection(std::move(connection_p)) {
}

PostgresConnection::~PostgresConnection() {
	Close();
}

PostgresConnection::PostgresConnection(PostgresConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
}

PostgresConnection &PostgresConnection::operator=(PostgresConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
	return *this;
}

PostgresConnection PostgresConnection::Open(const string &dsn, const string &attach_path) {
	PostgresConnection result;
	result.connection = make_shared_ptr<OwnedPostgresConnection>(PostgresUtils::PGConnect(dsn, attach_path));
	result.dsn = dsn;
	return result;
}

static bool ResultHasError(PGresult *result) {
	if (!result) {
		return true;
	}
	switch (PQresultStatus(result)) {
	case PGRES_COMMAND_OK:
	case PGRES_TUPLES_OK:
		return false;
	default:
		return true;
	}
}

void PostgresConnection::LogQuery(optional_ptr<ClientContext> context, const string &query,
                                  std::chrono::steady_clock::time_point start) {
	if (!context) {
		return;
	}
	auto duration =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	DUCKDB_LOG(*context, PostgresQueryLogType, query, duration);
}

PGresult *PostgresConnection::PQExecute(optional_ptr<ClientContext> context, const string &query,
                                        const PostgresParameters &params, int result_format) {
	TaskScheduler::BlockingScope blocking;
	auto start = std::chrono::steady_clock::now();
	PGconn *conn = GetConn();
	PGresult *res = nullptr;

	if (params.Empty()) {
		res = PQexec(GetConn(), query.c_str());
	} else {
		// Unlike PQexec, PQexecParams allows at most one SQL command in the given string.
		res = PQexecParams(conn, query.c_str(), params.Count(), params.Types(), params.Values(), params.Lengths(),
		                   params.Formats(), result_format);
	}
	LogQuery(context, query, start);
	return res;
}

unique_ptr<PostgresResult> PostgresConnection::TryQuery(optional_ptr<ClientContext> context, const string &query,
                                                        optional_ptr<string> error_message,
                                                        const PostgresParameters &params, int result_format) {
	TaskScheduler::BlockingScope blocking;
	lock_guard<mutex> guard(connection->connection_lock);
	auto result = PQExecute(context, query.c_str(), params, result_format);

	if (ResultHasError(result)) {
		if (error_message) {
			auto error = result ? PQresultErrorMessage(result) : PQerrorMessage(GetConn());
			*error_message = StringUtil::Format("Failed to execute query \"" + query + "\": " + string(error));
		}
		PQclear(result);
		return nullptr;
	}
	return make_uniq<PostgresResult>(result);
}

unique_ptr<PostgresResult> PostgresConnection::Query(optional_ptr<ClientContext> context, const string &query,
                                                     const PostgresParameters &params, int result_format) {
	string error_msg;
	auto result = TryQuery(context, query, &error_msg, params, result_format);
	if (!result) {
		throw std::runtime_error(error_msg);
	}
	return result;
}

void PostgresConnection::Execute(optional_ptr<ClientContext> context, const string &query,
                                 const PostgresParameters &params) {
	Query(context, query, params);
}

vector<unique_ptr<PostgresResult>> PostgresConnection::ExecuteQueries(ClientContext &context, const string &queries) {
	TaskScheduler::BlockingScope blocking;
	auto start = std::chrono::steady_clock::now();
	auto res = PQsendQuery(GetConn(), queries.c_str());
	if (res == 0) {
		LogQuery(context, queries, start);
		throw std::runtime_error("Failed to execute query \"" + queries + "\": " + string(PQerrorMessage(GetConn())));
	}
	vector<unique_ptr<PostgresResult>> results;
	string error_message;
	while (true) {
		auto res = PQgetResult(GetConn());
		if (!res) {
			break;
		}
		auto result = make_uniq<PostgresResult>(res);
		if (ResultHasError(res)) {
			if (error_message.empty()) {
				error_message = "Failed to execute query \"" + queries + "\": " + string(PQresultErrorMessage(res));
			}
			continue;
		}
		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			continue;
		}
		results.push_back(std::move(result));
	}
	LogQuery(context, queries, start);
	if (!error_message.empty()) {
		throw std::runtime_error(error_message);
	}
	return results;
}

PostgresVersion PostgresConnection::GetPostgresVersion(ClientContext &context) {
	auto result = TryQuery(context, "SELECT version(), (SELECT COUNT(*) FROM pg_settings WHERE name LIKE 'rds%')");
	if (!result) {
		PostgresVersion version;
		version.type_v = PostgresInstanceType::UNKNOWN;
		return version;
	}
	auto pg_version_string = result->GetString(0, 0);
	auto version = PostgresUtils::ExtractPostgresVersion(pg_version_string);
	if (result->GetInt64(0, 1) > 0) {
		version.type_v = PostgresInstanceType::AURORA;
	}
	if (StringUtil::Contains(pg_version_string, "Redshift")) {
		version.type_v = PostgresInstanceType::REDSHIFT;
	}
	return version;
}

bool PostgresConnection::IsOpen() {
	return connection.get();
}

void PostgresConnection::Close() {
	if (!IsOpen()) {
		return;
	}
	connection = nullptr;
}

bool PostgresConnection::PingServer(const std::string &health_check_query) {
	if (!IsOpen()) {
		return false;
	}
	PGconn *conn = GetConn();
	if (PQstatus(conn) != CONNECTION_OK) {
		return false;
	}
	if (health_check_query.empty()) {
		return true;
	}
	TaskScheduler::BlockingScope blocking;
	PGresult *res = PQexec(conn, health_check_query.c_str());
	PostgresResult res_holder(res);
	return PQresultStatus(res) == PGRES_TUPLES_OK;
}

void PostgresConnection::Reset(const std::string &health_check_query) {
	if (!IsOpen()) {
		throw InternalException("Cannot reset a connection that is not open");
	}
	TaskScheduler::BlockingScope blocking;
	PGconn *conn = GetConn();
	auto tx_status = PQtransactionStatus(conn);
	if (tx_status == PQTRANS_INTRANS || tx_status == PQTRANS_INERROR) {
		PGresult *res = PQexec(conn, "ROLLBACK");
		PostgresResult res_holder(res);
	}
	{
		PGresult *res = PQexec(conn, "DISCARD ALL");
		PostgresResult res_holder(res);
		if (PQresultStatus(res) == PGRES_COMMAND_OK) {
			return;
		}
	}
	PQreset(conn);
	if (!PingServer(health_check_query)) {
		throw InternalException("Connection reset failure");
	}
}

vector<IndexInfo> PostgresConnection::GetIndexInfo(const string &table_name) {
	return vector<IndexInfo>();
}

} // namespace duckdb
