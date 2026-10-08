#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "postgres_parameters.hpp"
#include "postgres_scanner.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/postgres_catalog.hpp"
#include "storage/postgres_table_entry.hpp"
#include "storage/postgres_transaction.hpp"

namespace duckdb {

static bool ExtractFlag(TableFunctionBindInput &input, const string &name, bool default_val) {
	auto it = input.named_parameters.find(Identifier(name));
	if (it != input.named_parameters.end()) {
		Value &bool_val = it->second;
		if (!bool_val.IsNull()) {
			return BooleanValue::Get(bool_val);
		}
	}
	return default_val;
}

static unique_ptr<FunctionData> BindDML(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<Identifier> &names,
                                        PostgresCatalog &pg_catalog, PostgresConnection &con, std::string sql,
                                        bool use_transaction, PostgresParameters params = PostgresParameters()) {
	// The statement returns no result columns: it's a command (DDL, or DML without RETURNING).
	// Instead of failing, run it as a command and return a single-row Success result. We reuse
	// the prepare/describe just done — no extra round-trip — and defer execution to
	// InitGlobalState (execution time, not bind, so EXPLAIN does not run it).
	auto result = make_uniq<PostgresBindData>(context);
	result->command_only = true;
	if (ExtractFlag(input, "suppress_dml_output", false)) {
		// This invocation wraps a command with no result set (DDL, or DML without RETURNING). Tell the
		// binder via the return-type modifier so that when this is routed through CONNECT the outer
		// statement is reported as NOTHING and displays like a native command (no spurious result table).
		input.table_function.call_return_type = StatementReturnType::NOTHING;
	}
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back(Identifier("rowcount"));
	result->catalog_name = pg_catalog.GetName();
	result->dsn = con.GetDSN();
	result->types = return_types;
	result->names.emplace_back(names[0].GetIdentifierName());
	result->read_only = false;
	result->sql = std::move(sql);
	result->params = std::move(params);
	result->use_transaction = use_transaction;
	PostgresScanFunction::PrepareBind(pg_catalog.GetPostgresVersion(), context, *result, 0, pg_catalog);
	return std::move(result);
}

static void TypeColumnsFromTable(ClientContext &context, const Identifier &catalog, TableFunctionBindInput &input,
                                 vector<LogicalType> &return_types, PostgresBindData &bind_data) {
	auto schema = input.named_parameters.find("schema");
	auto table = input.named_parameters.find("table");
	if (schema == input.named_parameters.end() || table == input.named_parameters.end()) {
		return;
	}
	bind_data.table_name = table->second.GetValue<string>();
	auto attached =
	    PostgresTableEntry::Lookup(context, QualifiedName(catalog, Identifier(schema->second.GetValue<string>()),
	                                                      Identifier(bind_data.table_name)));
	auto &entry = attached.Get<PostgresTableEntry>();
	for (idx_t c = 0; c < bind_data.names.size(); c++) {
		Identifier name(bind_data.names[c]);
		const auto index = entry.GetColumnIndex(name, true);
		if (!index.IsValid()) {
			continue;
		}
		return_types[c] = entry.GetColumn(index).GetType();
		bind_data.types[c] = return_types[c];
		bind_data.postgres_types[c] = entry.postgres_types[index.index];
	}
}

static void CastLookupColumnsToVarchar(PostgresBindData &bind_data) {
	string columns;
	string projection;
	for (idx_t c = 0; c < bind_data.types.size(); c++) {
		const string column = (c == 0 ? "c" : ", c") + std::to_string(c);
		columns += column;
		projection += column + PostgresUtils::VarcharCast(bind_data.types[c], bind_data.postgres_types[c],
		                                                  bind_data.table_name, bind_data.names[c]);
	}
	if (projection != columns) {
		bind_data.sql = "SELECT " + projection + " FROM (" + bind_data.sql + ") AS t(" + columns + ")";
	}
}

static unique_ptr<FunctionData> PGQueryBindInternal(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names,
                                                    bool lookup) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to postgres_query cannot be NULL");
	}

	// look up the database to query
	auto db_name = input.inputs[0].GetValue<string>();
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, Identifier(db_name));
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" referenced in postgres_query", db_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "postgres") {
		throw BinderException("Attached database \"%s\" does not refer to a Postgres database", db_name);
	}
	auto &pg_catalog = catalog.Cast<PostgresCatalog>();
	auto &transaction = Transaction::Get(context, catalog).Cast<PostgresTransaction>();
	auto sql = input.inputs[1].GetValue<string>();
	// strip any trailing semicolons
	StringUtil::RTrim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql = sql.substr(0, sql.size() - 1);
		StringUtil::RTrim(sql);
	}

	bool use_transaction = ExtractFlag(input, "use_transaction", true);

	vector<Value> param_values;
	auto params_it = input.named_parameters.find("params");
	if (params_it != input.named_parameters.end()) {
		Value &struct_val = params_it->second;
		if (struct_val.IsNull()) {
			throw BinderException("Parameters to postgres_query cannot be NULL");
		}
		if (struct_val.type().id() != LogicalTypeId::STRUCT && struct_val.type().id() != LogicalTypeId::TUPLE) {
			throw BinderException("Query parameters must be specified in a STRUCT");
		}
		param_values = StructValue::GetChildren(struct_val);
	}
	if (lookup && !param_values.empty()) {
		throw BinderException("postgres_lookup supplies parameters per call; params := cannot be used");
	}

	// schema_query: an optional cheaper query with the SAME result schema, used
	// only for the prepare/describe round trips. A caller re-issuing a
	// large-literal query per batch (e.g. the external-lookup index source)
	// passes a tiny constant variant whose describe result caches in the
	// catalog, so repeated binds cost no round trip at all. Ignored when
	// `params` are given: parameter-count validation needs the real statement.
	auto schema_sql = sql;
	auto schema_it = input.named_parameters.find("schema_query");
	if (!lookup && param_values.empty() && schema_it != input.named_parameters.end() && !schema_it->second.IsNull()) {
		schema_sql = schema_it->second.GetValue<string>();
		StringUtil::RTrim(schema_sql);
		while (!schema_sql.empty() && schema_sql.back() == ';') {
			schema_sql = schema_sql.substr(0, schema_sql.size() - 1);
			StringUtil::RTrim(schema_sql);
		}
	}

	auto &con = use_transaction ? transaction.GetConnection() : transaction.GetConnectionWithoutTransaction();

	if (!ExtractFlag(input, "prepare", true)) {
		if (param_values.size() > 0) {
			throw BinderException("query parameters cannot be used with 'prepare=FALSE'");
		}
		return BindDML(context, input, return_types, names, pg_catalog, con, std::move(sql), use_transaction);
	}

	auto result = make_uniq<PostgresBindData>(context);
	auto type_config = PostgresTypeConfig::FromContext(context);
	vector<Oid> param_types;
	// Parameterized statements have constant text, so the statement itself is
	// the describe-cache key (param types included); plain statements key on
	// the schema_query.
	const string &cache_key = (param_values.empty() && !lookup) ? schema_sql : sql;
	PostgresCatalog::DescribeCacheEntry cached;
	if (pg_catalog.TryGetDescribe(cache_key, cached)) {
		if (!lookup && cached.param_types.size() != param_values.size()) {
			throw BinderException("Incorrect number of parameters specified, expected: %zu, actual: %zu, query: \"%s\"",
			                      cached.param_types.size(), param_values.size(), sql);
		}
		names = std::move(cached.names);
		return_types = std::move(cached.types);
		result->postgres_types = std::move(cached.postgres_types);
		param_types = std::move(cached.param_types);
	} else {
		auto conn = con.GetConn();
		// prepare execution of the query to figure out the result types and names
		auto prepared = PQprepare(conn, "", schema_sql.c_str(), 0, nullptr);
		PostgresResult prepared_wrapper(prepared);
		if (!prepared) {
			throw BinderException("Failed to prepare query \"%s\" (no result returned): %s", schema_sql,
			                      PQerrorMessage(conn));
		}
		if (PQresultStatus(prepared) != PGRES_COMMAND_OK) {
			throw BinderException("Failed to prepare query \"%s\": %s", schema_sql, PQresultErrorMessage(prepared));
		}
		// use describe_prepared
		auto describe_prepared = PQdescribePrepared(conn, "");
		PostgresResult describe_wrapper(describe_prepared);
		if (!describe_prepared || PQresultStatus(describe_prepared) != PGRES_COMMAND_OK) {
			auto extended_err = describe_prepared ? PQresultErrorMessage(describe_prepared) : PQerrorMessage(conn);
			throw BinderException("Failed to describe prepared statement: %s", extended_err);
		}
		int nfields = PQnfields(describe_prepared);
		int nparams = PQnparams(describe_prepared);
		if (nfields <= 0) {
			if (nparams != static_cast<int>(param_values.size())) {
				throw BinderException(
				    "Incorrect number of parameters specified, expected: %d, actual: %zu, query: \"%s\"", nparams,
				    param_values.size(), sql);
			}
			PostgresParameters command_params;
			if (!param_values.empty()) {
				vector<Oid> command_param_types;
				for (int p = 0; p < nparams; p++) {
					command_param_types.emplace_back(PQparamtype(describe_prepared, p));
				}
				command_params = PostgresParameters(std::move(command_param_types), std::move(param_values));
			}
			return BindDML(context, input, return_types, names, pg_catalog, con, std::move(sql), use_transaction,
			               std::move(command_params));
		}
		for (idx_t c = 0; c < nfields; c++) {
			PostgresType postgres_type;
			postgres_type.oid = PQftype(describe_prepared, c);
			PostgresTypeData type_data;
			type_data.type_name = PostgresUtils::PostgresOidToName(postgres_type.oid);
			type_data.type_modifier = PQfmod(describe_prepared, c);
			auto converted_type =
			    PostgresUtils::TypeToLogicalType(nullptr, nullptr, type_config, type_data, postgres_type);
			result->postgres_types.push_back(postgres_type);
			return_types.emplace_back(converted_type);
			names.emplace_back(PQfname(describe_prepared, c));
		}
		if (!lookup && nparams != param_values.size()) {
			throw BinderException("Incorrect number of parameters specified, expected: %d, actual: %zu, query: \"%s\"",
			                      nparams, param_values.size(), sql);
		}
		for (idx_t p = 0; p < nparams; p++) {
			Oid ptype = PQparamtype(describe_prepared, p);
			param_types.emplace_back(ptype);
		}
		pg_catalog.StoreDescribe(
		    cache_key, PostgresCatalog::DescribeCacheEntry {names, return_types, result->postgres_types, param_types});
	}

	// set up the bind data
	result->type_config = type_config;
	result->catalog_name = pg_catalog.GetName();
	result->dsn = con.GetDSN();
	result->types = return_types;
	for (auto &nm : names) {
		result->names.emplace_back(nm.GetIdentifierName());
	}
	result->read_only = false;
	result->sql = std::move(sql);
	if (lookup) {
		result->lookup = true;
		result->lookup_param_types = std::move(param_types);
		TypeColumnsFromTable(context, pg_catalog.GetName(), input, return_types, *result);
		CastLookupColumnsToVarchar(*result);
	} else {
		result->params = PostgresParameters(std::move(param_types), std::move(param_values));
	}
	result->use_transaction = use_transaction;
	PostgresScanFunction::PrepareBind(pg_catalog.GetPostgresVersion(), context, *result, 0, pg_catalog);
	return std::move(result);
}

static unique_ptr<FunctionData> PGQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	return PGQueryBindInternal(context, input, return_types, names, /*lookup=*/false);
}

static unique_ptr<FunctionData> PGLookupBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	return PGQueryBindInternal(context, input, return_types, names, /*lookup=*/true);
}

static FunctionSignature PostgresQuerySignature(bool suppress_dml_output) {
	FunctionSignature signature;
	signature.AddParameter("database", LogicalType::VARCHAR)
	    .AddParameter("sql", LogicalType::VARCHAR)
	    .WithTypedKwargs("options", [&](TypedKwargs &options) {
		    options.Add("use_transaction", LogicalType::BOOLEAN).Add("params", LogicalType::ANY);
		    if (suppress_dml_output) {
			    options.Add("suppress_dml_output", LogicalType::BOOLEAN);
		    }
		    options.Add("prepare", LogicalType::BOOLEAN);
		    options.Add("schema_query", LogicalType::VARCHAR);
	    });
	return signature;
}

static FunctionSignature PostgresLookupSignature() {
	FunctionSignature signature;
	signature.AddParameter("database", LogicalType::VARCHAR)
	    .AddParameter("sql", LogicalType::VARCHAR)
	    .WithTypedKwargs("options", [&](TypedKwargs &options) {
		    options.Add("use_transaction", LogicalType::BOOLEAN)
		        .Add("schema_query", LogicalType::VARCHAR)
		        .Add("schema", LogicalType::VARCHAR)
		        .Add("table", LogicalType::VARCHAR);
	    });
	return signature;
}

PostgresQueryFunction::PostgresQueryFunction()
    : TableFunction("postgres_query", PostgresQuerySignature(true), nullptr, PGQueryBind) {
	PostgresScanFunction scan_function;
	init_global = scan_function.init_global;
	init_local = scan_function.init_local;
	function = scan_function.function;
	projection_pushdown = true;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
}

PostgresExecuteFunction::PostgresExecuteFunction()
    : TableFunction("postgres_execute", PostgresQuerySignature(false), nullptr, PGQueryBind) {
	PostgresScanFunction scan_function;
	init_global = scan_function.init_global;
	init_local = scan_function.init_local;
	function = scan_function.function;
	projection_pushdown = true;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
}

PostgresLookupFunction::PostgresLookupFunction()
    : TableFunction("postgres_lookup", PostgresLookupSignature(), nullptr, PGLookupBind) {
	PostgresScanFunction scan_function;
	init_global = scan_function.init_global;
	in_out_function = PostgresLookupScan;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
}
} // namespace duckdb
