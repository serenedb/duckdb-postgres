#include "postgres_parameters.hpp"

#include "duckdb.hpp"

#include "duckdb/common/vector/struct_vector.hpp"

#include <libpq-fe.h>

#include <cstdio>

#include "postgres_conversion.hpp"
#include "postgres_type_oids.hpp"

namespace duckdb {

static const int FORMAT_TEXT = 0;
static const int FORMAT_BINARY = 1;

struct Param {
	const char *ptr = nullptr;
	int length = 0;
	int format = FORMAT_TEXT;

	Param() {
	}

	Param(const char *ptr_in, int length_in, int format_in) : ptr(ptr_in), length(length_in), format(format_in) {
	}
};

static Param CreateVarcharParam(Value &value) {
	const string &str = StringValue::Get(value);
	return Param(str.c_str(), static_cast<int>(str.length()), FORMAT_TEXT);
}

template <typename INT_TYPE>
static Param CreateIntParam(INT_TYPE num, vector<char> &copy_holder) {
	copy_holder.resize(sizeof(INT_TYPE));
	memcpy(copy_holder.data(), &num, sizeof(INT_TYPE));
	return Param(copy_holder.data(), sizeof(INT_TYPE), FORMAT_BINARY);
}

static uint32_t FloatHtonl(float num) {
	std::array<char, sizeof(float)> arr;
	memcpy(arr.data(), &num, sizeof(float));
	uint32_t int_num = *reinterpret_cast<uint32_t *>(arr.data());
	return htonl(int_num);
}

static uint64_t DoubleHtonll(double num) {
	std::array<char, sizeof(double)> arr;
	memcpy(arr.data(), &num, sizeof(double));
	uint64_t int_num = *reinterpret_cast<uint64_t *>(arr.data());
	return htonll(int_num);
}

// The element type of a supported array parameter, from the DESCRIBEd
// parameter oid -- postgres' own type inference for $n decides what we ship.
static Oid ArrayElemOid(Oid array_oid) {
	switch (array_oid) {
	case BOOLARRAYOID:
		return BOOLOID;
	case INT2ARRAYOID:
		return INT2OID;
	case INT4ARRAYOID:
		return INT4OID;
	case INT8ARRAYOID:
		return INT8OID;
	case FLOAT4ARRAYOID:
		return FLOAT4OID;
	case FLOAT8ARRAYOID:
		return FLOAT8OID;
	case TEXTARRAYOID:
		return TEXTOID;
	case VARCHARARRAYOID:
		return VARCHAROID;
	case BPCHARARRAYOID:
		return BPCHAROID;
	case TIDARRAYOID:
		return TIDOID;
	default:
		return 0;
	}
}

static void PutBe16(vector<char> &buf, uint16_t v) {
	v = htons(v);
	const char *p = reinterpret_cast<const char *>(&v);
	buf.insert(buf.end(), p, p + sizeof(v));
}
static void PutBe32(vector<char> &buf, uint32_t v) {
	v = htonl(v);
	const char *p = reinterpret_cast<const char *>(&v);
	buf.insert(buf.end(), p, p + sizeof(v));
}
static void PutBe64(vector<char> &buf, uint64_t v) {
	v = htonll(v);
	const char *p = reinterpret_cast<const char *>(&v);
	buf.insert(buf.end(), p, p + sizeof(v));
}

// One element in the array's binary payload: int32 byte length, then the
// element's binary wire encoding. The child value is cast to the element
// type first, so any duckdb list child type postgres can hold round-trips.
static void EncodeArrayElem(vector<char> &buf, Oid elem_oid, const Value &v) {
	switch (elem_oid) {
	case BOOLOID:
		PutBe32(buf, 1);
		buf.push_back(BooleanValue::Get(v.DefaultCastAs(LogicalType::BOOLEAN)) ? 1 : 0);
		return;
	case INT2OID:
		PutBe32(buf, 2);
		PutBe16(buf, static_cast<uint16_t>(SmallIntValue::Get(v.DefaultCastAs(LogicalType::SMALLINT))));
		return;
	case INT4OID:
		PutBe32(buf, 4);
		PutBe32(buf, static_cast<uint32_t>(IntegerValue::Get(v.DefaultCastAs(LogicalType::INTEGER))));
		return;
	case INT8OID:
		PutBe32(buf, 8);
		PutBe64(buf, static_cast<uint64_t>(BigIntValue::Get(v.DefaultCastAs(LogicalType::BIGINT))));
		return;
	case FLOAT4OID: {
		PutBe32(buf, 4);
		const float f = FloatValue::Get(v.DefaultCastAs(LogicalType::FLOAT));
		uint32_t u;
		memcpy(&u, &f, sizeof(u));
		PutBe32(buf, u);
		return;
	}
	case FLOAT8OID: {
		PutBe32(buf, 8);
		const double d = DoubleValue::Get(v.DefaultCastAs(LogicalType::DOUBLE));
		uint64_t u;
		memcpy(&u, &d, sizeof(u));
		PutBe64(buf, u);
		return;
	}
	case TEXTOID:
	case VARCHAROID:
	case BPCHAROID: {
		const string s = StringValue::Get(v.DefaultCastAs(LogicalType::VARCHAR));
		PutBe32(buf, static_cast<uint32_t>(s.size()));
		buf.insert(buf.end(), s.begin(), s.end());
		return;
	}
	case TIDOID: {
		// tid has no duckdb twin -- elements arrive as STRUCT{block, offset}
		// (any integer child types) or as their "(block,offset)" text form.
		uint32_t block = 0;
		uint16_t offset = 0;
		if (v.type().id() == LogicalTypeId::STRUCT) {
			auto &children = StructValue::GetChildren(v);
			if (children.size() != 2) {
				throw BinderException("A tid struct value must have exactly two children");
			}
			block = static_cast<uint32_t>(BigIntValue::Get(children[0].DefaultCastAs(LogicalType::BIGINT)));
			offset = static_cast<uint16_t>(BigIntValue::Get(children[1].DefaultCastAs(LogicalType::BIGINT)));
		} else {
			const string s = StringValue::Get(v.DefaultCastAs(LogicalType::VARCHAR));
			unsigned long parsed_block = 0;
			unsigned int parsed_offset = 0;
			if (sscanf(s.c_str(), "(%lu,%u)", &parsed_block, &parsed_offset) != 2) {
				throw BinderException("Invalid tid array element: %s", s.c_str());
			}
			block = static_cast<uint32_t>(parsed_block);
			offset = static_cast<uint16_t>(parsed_offset);
		}
		PutBe32(buf, 6);
		PutBe32(buf, block);
		PutBe16(buf, offset);
		return;
	}
	default:
		throw BinderException("Unsupported array element oid: %u", elem_oid);
	}
}

static Param CreateTextParam(const string &text, vector<char> &buf) {
	buf.assign(text.begin(), text.end());
	buf.push_back('\0');
	return Param(buf.data(), static_cast<int>(text.size()), FORMAT_TEXT);
}

// A LIST parameter whose element type has no binary writer here: the pg
// array-literal text form, which postgres parses into ANY array type.
static Param CreateTextArrayParam(Value &value, vector<char> &buf) {
	string text = "{";
	const auto &children = ListValue::GetChildren(value);
	for (idx_t i = 0; i < children.size(); i++) {
		if (i > 0) {
			text += ",";
		}
		if (children[i].IsNull()) {
			text += "NULL";
			continue;
		}
		text += '"';
		for (const char c : children[i].DefaultCastAs(LogicalType::VARCHAR).ToString()) {
			if (c == '"' || c == '\\') {
				text += '\\';
			}
			text += c;
		}
		text += '"';
	}
	text += "}";
	return CreateTextParam(text, buf);
}

// A LIST parameter as a postgres binary one-dimensional array.
static Param CreateArrayParam(Oid array_oid, Value &value, vector<char> &buf) {
	const Oid elem_oid = ArrayElemOid(array_oid);
	if (elem_oid == 0) {
		return CreateTextArrayParam(value, buf);
	}
	const auto &children = ListValue::GetChildren(value);
	bool has_null = false;
	for (const auto &child : children) {
		has_null |= child.IsNull();
	}
	if (children.empty()) {
		PutBe32(buf, 0); // ndim: empty arrays carry no dimensions
		PutBe32(buf, 0); // hasnull
		PutBe32(buf, elem_oid);
	} else {
		PutBe32(buf, 1); // ndim
		PutBe32(buf, has_null ? 1 : 0);
		PutBe32(buf, elem_oid);
		PutBe32(buf, static_cast<uint32_t>(children.size()));
		PutBe32(buf, 1); // lower bound
		for (const auto &child : children) {
			if (child.IsNull()) {
				PutBe32(buf, static_cast<uint32_t>(-1));
			} else {
				EncodeArrayElem(buf, elem_oid, child);
			}
		}
	}
	return Param(buf.data(), static_cast<int>(buf.size()), FORMAT_BINARY);
}

static bool ReadIntElem(const UnifiedVectorFormat &fmt, PhysicalType physical, idx_t idx, int64_t &out) {
	switch (physical) {
	case PhysicalType::INT8:
		out = UnifiedVectorFormat::GetData<int8_t>(fmt)[idx];
		return true;
	case PhysicalType::INT16:
		out = UnifiedVectorFormat::GetData<int16_t>(fmt)[idx];
		return true;
	case PhysicalType::INT32:
		out = UnifiedVectorFormat::GetData<int32_t>(fmt)[idx];
		return true;
	case PhysicalType::INT64:
		out = UnifiedVectorFormat::GetData<int64_t>(fmt)[idx];
		return true;
	case PhysicalType::UINT8:
		out = UnifiedVectorFormat::GetData<uint8_t>(fmt)[idx];
		return true;
	case PhysicalType::UINT16:
		out = UnifiedVectorFormat::GetData<uint16_t>(fmt)[idx];
		return true;
	case PhysicalType::UINT32:
		out = UnifiedVectorFormat::GetData<uint32_t>(fmt)[idx];
		return true;
	default:
		return false;
	}
}

// Binary elements straight from the vector data. Returns false when this
// vector/oid pairing has no direct writer -- the caller falls back to text.
static bool EncodeVectorArrayElems(vector<char> &buf, Oid elem_oid, Vector &vec, idx_t count, bool &has_null) {
	const auto physical = vec.GetType().InternalType();
	if (elem_oid == TIDOID) {
		if (physical != PhysicalType::STRUCT) {
			return false;
		}
		auto &children = StructVector::GetEntries(vec);
		if (children.size() != 2) {
			return false;
		}
		UnifiedVectorFormat fmt;
		UnifiedVectorFormat block_fmt;
		UnifiedVectorFormat offset_fmt;
		vec.ToUnifiedFormat(count, fmt);
		children[0].ToUnifiedFormat(count, block_fmt);
		children[1].ToUnifiedFormat(count, offset_fmt);
		const auto block_physical = children[0].GetType().InternalType();
		const auto offset_physical = children[1].GetType().InternalType();
		for (idx_t i = 0; i < count; i++) {
			const auto idx = fmt.sel->get_index(i);
			if (!fmt.validity.RowIsValid(idx)) {
				has_null = true;
				PutBe32(buf, static_cast<uint32_t>(-1));
				continue;
			}
			int64_t block;
			int64_t offset;
			if (!ReadIntElem(block_fmt, block_physical, block_fmt.sel->get_index(i), block) ||
			    !ReadIntElem(offset_fmt, offset_physical, offset_fmt.sel->get_index(i), offset)) {
				return false;
			}
			PutBe32(buf, 6);
			PutBe32(buf, static_cast<uint32_t>(block));
			PutBe16(buf, static_cast<uint16_t>(offset));
		}
		return true;
	}
	UnifiedVectorFormat fmt;
	vec.ToUnifiedFormat(count, fmt);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = fmt.sel->get_index(i);
		if (!fmt.validity.RowIsValid(idx)) {
			has_null = true;
			PutBe32(buf, static_cast<uint32_t>(-1));
			continue;
		}
		switch (elem_oid) {
		case BOOLOID: {
			if (physical != PhysicalType::BOOL) {
				return false;
			}
			PutBe32(buf, 1);
			buf.push_back(UnifiedVectorFormat::GetData<bool>(fmt)[idx] ? 1 : 0);
			break;
		}
		case INT2OID: {
			int64_t v;
			if (!ReadIntElem(fmt, physical, idx, v)) {
				return false;
			}
			PutBe32(buf, 2);
			PutBe16(buf, static_cast<uint16_t>(v));
			break;
		}
		case INT4OID: {
			int64_t v;
			if (!ReadIntElem(fmt, physical, idx, v)) {
				return false;
			}
			PutBe32(buf, 4);
			PutBe32(buf, static_cast<uint32_t>(v));
			break;
		}
		case INT8OID: {
			int64_t v;
			if (!ReadIntElem(fmt, physical, idx, v)) {
				return false;
			}
			PutBe32(buf, 8);
			PutBe64(buf, static_cast<uint64_t>(v));
			break;
		}
		case FLOAT4OID: {
			if (physical != PhysicalType::FLOAT) {
				return false;
			}
			const float f = UnifiedVectorFormat::GetData<float>(fmt)[idx];
			uint32_t u;
			memcpy(&u, &f, sizeof(u));
			PutBe32(buf, 4);
			PutBe32(buf, u);
			break;
		}
		case FLOAT8OID: {
			if (physical != PhysicalType::DOUBLE) {
				return false;
			}
			const double d = UnifiedVectorFormat::GetData<double>(fmt)[idx];
			uint64_t u;
			memcpy(&u, &d, sizeof(u));
			PutBe32(buf, 8);
			PutBe64(buf, u);
			break;
		}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID: {
			if (physical != PhysicalType::VARCHAR) {
				return false;
			}
			const auto str = UnifiedVectorFormat::GetData<string_t>(fmt)[idx];
			PutBe32(buf, static_cast<uint32_t>(str.GetSize()));
			buf.insert(buf.end(), str.GetData(), str.GetData() + str.GetSize());
			break;
		}
		default:
			return false;
		}
	}
	return true;
}

static PostgresParamSlot CreateVectorTextArrayParam(Vector &vec, idx_t count, vector<char> &buf) {
	string text = "{";
	for (idx_t i = 0; i < count; i++) {
		if (i > 0) {
			text += ",";
		}
		auto v = vec.GetValue(i);
		if (v.IsNull()) {
			text += "NULL";
			continue;
		}
		text += '"';
		for (const char c : v.DefaultCastAs(LogicalType::VARCHAR).ToString()) {
			if (c == '"' || c == '\\') {
				text += '\\';
			}
			text += c;
		}
		text += '"';
	}
	text += "}";
	buf.assign(text.begin(), text.end());
	buf.push_back('\0');
	PostgresParamSlot slot;
	slot.ptr = buf.data();
	slot.length = static_cast<int>(text.size());
	slot.format = FORMAT_TEXT;
	return slot;
}

PostgresParamSlot CreateVectorArrayParam(Oid array_oid, Vector &vec, idx_t count, vector<char> &buf) {
	buf.clear();
	const Oid elem_oid = ArrayElemOid(array_oid);
	if (elem_oid != 0) {
		if (count == 0) {
			PutBe32(buf, 0);
			PutBe32(buf, 0);
			PutBe32(buf, elem_oid);
		} else {
			PutBe32(buf, 1);
			PutBe32(buf, 0);
			PutBe32(buf, elem_oid);
			PutBe32(buf, static_cast<uint32_t>(count));
			PutBe32(buf, 1);
			bool has_null = false;
			if (!EncodeVectorArrayElems(buf, elem_oid, vec, count, has_null)) {
				buf.clear();
				return CreateVectorTextArrayParam(vec, count, buf);
			}
			if (has_null) {
				const uint32_t one = htonl(1);
				memcpy(buf.data() + 4, &one, sizeof(one));
			}
		}
		PostgresParamSlot slot;
		slot.ptr = buf.data();
		slot.length = static_cast<int>(buf.size());
		slot.format = FORMAT_BINARY;
		return slot;
	}
	return CreateVectorTextArrayParam(vec, count, buf);
}

// Encode one parameter by the DESCRIBEd $n type -- postgres fixed the
// parameter's type at prepare time and interprets binary bytes as exactly
// that, so the duckdb value is cast to it first. Types without a binary
// writer here go as text, which postgres parses into anything.
static Param CreateParam(Oid type_oid, Value &value, vector<char> &copy_holder) {
	if (value.IsNull()) {
		return Param(nullptr, 0, FORMAT_BINARY);
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		return CreateArrayParam(type_oid, value, copy_holder);
	}

	switch (type_oid) {
	case BOOLOID: {
		copy_holder.assign(1, BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN)) ? 1 : 0);
		return Param(copy_holder.data(), 1, FORMAT_BINARY);
	}
	case INT2OID: {
		uint16_t num = static_cast<uint16_t>(SmallIntValue::Get(value.DefaultCastAs(LogicalType::SMALLINT)));
		return CreateIntParam(htons(num), copy_holder);
	}
	case INT4OID: {
		uint32_t num = static_cast<uint32_t>(IntegerValue::Get(value.DefaultCastAs(LogicalType::INTEGER)));
		return CreateIntParam(htonl(num), copy_holder);
	}
	case INT8OID: {
		uint64_t num = static_cast<uint64_t>(BigIntValue::Get(value.DefaultCastAs(LogicalType::BIGINT)));
		return CreateIntParam(htonll(num), copy_holder);
	}
	case FLOAT4OID: {
		float num = FloatValue::Get(value.DefaultCastAs(LogicalType::FLOAT));
		return CreateIntParam(FloatHtonl(num), copy_holder);
	}
	case FLOAT8OID: {
		double num = DoubleValue::Get(value.DefaultCastAs(LogicalType::DOUBLE));
		return CreateIntParam(DoubleHtonll(num), copy_holder);
	}
	default:
		if (value.type().id() == LogicalTypeId::VARCHAR) {
			return CreateVarcharParam(value);
		}
		return CreateTextParam(StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR)), copy_holder);
	}
}

PostgresParameters::PostgresParameters(vector<Oid> types_p, vector<Value> values_p)
    : types(std::move(types_p)), values(std::move(values_p)) {
	idx_t count = types.size();
	if (values.size() != count) {
		throw BinderException("Parameters count mismatch, types count: %zu, values count: %zu", count, values.size());
	}

	copied_values.resize(count);
	value_ptrs.resize(count);
	lengths.resize(count);
	formats.resize(count);

	for (idx_t i = 0; i < types.size(); i++) {
		Value &val = values[i];
		vector<char> &copy_holder = copied_values[i];

		Param param = CreateParam(types[i], val, copy_holder);

		value_ptrs[i] = param.ptr;
		lengths[i] = param.length;
		formats[i] = param.format;
	}
}

} // namespace duckdb
