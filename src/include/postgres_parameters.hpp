//===----------------------------------------------------------------------===//
//                         DuckDB
//
// postgres_parameters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include <libpq-fe.h>
#include "postgres_version.hpp"

namespace duckdb {

//! One encoded statement parameter; `ptr` points into the caller's buffer.
struct PostgresParamSlot {
	const char *ptr = nullptr;
	int length = 0;
	int format = 0;
};

//! Encode `count` rows of `vec` as one array parameter of the DESCRIBEd array
//! oid, straight from the vector data (no per-row Value). Types without a
//! binary writer fall back to the pg array-literal text form.
PostgresParamSlot CreateVectorArrayParam(Oid array_oid, Vector &vec, idx_t count, vector<char> &buf);

class PostgresParameters {
	vector<Oid> types;
	vector<Value> values;
	vector<vector<char>> copied_values;
	vector<const char *> value_ptrs;
	vector<int> lengths;
	vector<int> formats;

public:
	PostgresParameters() {
	}

	PostgresParameters(vector<Oid> types_p, vector<Value> values_p);

	bool Empty() const {
		return types.empty();
	}

	int Count() const {
		return static_cast<int>(types.size());
	}

	const Oid *Types() const {
		return types.data();
	}

	const char *const *Values() const {
		return value_ptrs.data();
	}

	const int *Lengths() const {
		return lengths.data();
	}

	const int *Formats() const {
		return formats.data();
	}
};

} // namespace duckdb
