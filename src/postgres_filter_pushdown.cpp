#include "postgres_filter_pushdown.hpp"

#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/function/scalar/struct_utils.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/common/enum_util.hpp"

#include "dbconnector/table_scan/filter_pushdown.hpp"
#include "dbconnector/table_scan/filter_util.hpp"

#include "postgres_utils.hpp"

namespace duckdb {

namespace {

// The postgres ctid literal for a duckdb rowid: the scan encodes a row's ctid
// as (page << 16) | tuple (see postgres_text_reader ConvertCTID), so decode it
// back to the '(page,tuple)' tid postgres understands.
string CtidLiteral(int64_t rowid) {
	int64_t page = rowid >> 16;
	int64_t tuple = rowid & 0xFFFF;
	return "'(" + std::to_string(page) + "," + std::to_string(tuple) + ")'::tid";
}

const char *CtidComparisonOp(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return " = ";
	case ExpressionType::COMPARE_NOTEQUAL:
		return " <> ";
	case ExpressionType::COMPARE_LESSTHAN:
		return " < ";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return " <= ";
	case ExpressionType::COMPARE_GREATERTHAN:
		return " > ";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return " >= ";
	default:
		return nullptr;
	}
}

ExpressionType FlipCtidComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	default:
		return type;
	}
}

bool ConstRowId(const Expression &expr, int64_t &out) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || !value.type().IsIntegral()) {
		return false;
	}
	out = value.GetValue<int64_t>();
	return true;
}

// Render a filter on the rowid (ctid) virtual column as a postgres ctid
// predicate. The shared dbconnector renderer refuses virtual columns (it has no
// ctid vocabulary and would compare a tid to an int), so a view-backed inverted
// index keyed on ctid renders its lookup here instead. Returns "" if the filter
// shape is not renderable (caller then re-applies it locally / errors).
string RenderCtidFilter(const Expression &expr) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &cmp = expr.Cast<BoundFunctionExpression>();
		auto type = cmp.GetExpressionType();
		auto &left = BoundComparisonExpression::Left(cmp);
		auto &right = BoundComparisonExpression::Right(cmp);
		int64_t rowid = 0;
		if (ConstRowId(right, rowid)) {
			// ctid <op> const
		} else if (ConstRowId(left, rowid)) {
			type = FlipCtidComparison(type);
		} else {
			return string();
		}
		const char *op = CtidComparisonOp(type);
		if (!op) {
			return string();
		}
		return "ctid" + string(op) + CtidLiteral(rowid);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		const char *join = conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND   ? " AND "
		                   : conj.GetExpressionType() == ExpressionType::CONJUNCTION_OR ? " OR "
		                                                                                : nullptr;
		if (!join) {
			return string();
		}
		string out;
		for (auto &child : conj.GetChildren()) {
			auto rendered = RenderCtidFilter(*child);
			if (rendered.empty()) {
				return string();
			}
			if (!out.empty()) {
				out += join;
			}
			out += rendered;
		}
		return out.empty() ? string() : "(" + out + ")";
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.GetExpressionType() != ExpressionType::COMPARE_IN) {
			return string();
		}
		string in_list;
		for (idx_t i = 1; i < op.GetChildren().size(); i++) {
			int64_t rowid = 0;
			if (!ConstRowId(*op.GetChildren()[i], rowid)) {
				return string();
			}
			if (!in_list.empty()) {
				in_list += ", ";
			}
			in_list += CtidLiteral(rowid);
		}
		return in_list.empty() ? string() : "ctid IN (" + in_list + ")";
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto &name = func.Function().GetName();
		optional_ptr<const Expression> child;
		if (func.BindInfo() && name == OptionalFilterScalarFun::NAME) {
			child = func.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr.get();
		} else if (func.BindInfo() && name == SelectivityOptionalFilterScalarFun::NAME) {
			child = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr.get();
		}
		if (!child) {
			return string();
		}
		return RenderCtidFilter(*child);
	}
	default:
		return string();
	}
}

} // namespace

string PostgresFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                                optional_ptr<TableFilterSet> filters, const vector<string> &names) {
	using namespace dbconnector;
	if (!filters || !filters->HasFilters()) {
		// no filters
		return string();
	}
	string result;
	for (auto &entry : *filters) {
		auto column_id = column_ids[entry.GetIndex()];
		auto &filter = entry.Filter();

		string filter_text;
		if (IsVirtualColumn(column_id)) {
			// rowid == the postgres ctid; render the ctid predicate ourselves.
			filter_text = RenderCtidFilter(table_scan::FilterUtil::GetExpression(filter, "PostgresFilterPushdown ctid"));
		} else {
			auto config = table_scan::FilterPushdown::CreateConfig('"', '\'', query::QuoteEscapeStyle::DOUBLE_QUOTE,
			                                                       query::Dialect::Postgres, "'\\x", "::BYTEA");
			filter_text =
			    table_scan::FilterPushdown::TransformFilter(config, names[column_id], filter, column_id);
		}

		if (filter_text.empty()) {
			if (table_scan::FilterUtil::IsInternalFilter(filter)) {
				continue;
			}
			throw NotImplementedException(
			    "Unsupported filter pushdown, use 'pg_experimental_filter_pushdown=FALSE' to disable pushdowns."
			    " Problematic filter: \"%s\"",
			    table_scan::FilterUtil::ToString(filter));
		}
		if (!result.empty()) {
			result += " AND ";
		}
		result += filter_text;
	}
	return result;
}

} // namespace duckdb
