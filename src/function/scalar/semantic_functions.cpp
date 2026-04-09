#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/llm_query.hpp"

namespace duckdb {

static void SemanticFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// semantic(prompt) -> BOOLEAN
	const std::string suffix_filter_prompt = "\n Return a single yes or no, do not contain any other words.";
    D_ASSERT(args.ColumnCount() == 1);
    auto &prompt_vec = args.data[0];

    idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
    auto out = FlatVector::GetData<bool>(result);
    auto &out_valid = FlatVector::Validity(result);

    std::vector<std::string> prompts;
    prompts.reserve(count);

	UnifiedVectorFormat puf;
	prompt_vec.ToUnifiedFormat(count, puf);
	auto pdata = reinterpret_cast<const string_t *>(puf.data);
	for (idx_t i = 0; i < count; i++) {
        auto idx = puf.sel->get_index(i);
        if (!puf.validity.RowIsValid(idx)) {
            prompts.emplace_back();
            continue;
        }
        prompts.emplace_back(pdata[idx].GetString() + suffix_filter_prompt);
    }

    auto results = batch_check_semantic(prompts);
    for (idx_t i = 0; i < count; i++) {
        auto idx = puf.sel->get_index(i);
        if (!puf.validity.RowIsValid(idx)) {
            out_valid.SetInvalid(i);
            continue;
        }
        out[i] = results[i];
    }
}

static void SemanticFunctionInt(DataChunk &args, ExpressionState &state, Vector &result) {
	// semantic_int(prompt) -> BIGINT
	const std::string suffix_filter_prompt = "\n Return a single integer, do not contain any other words.";
	D_ASSERT(args.ColumnCount() == 1);
	auto &prompt_vec = args.data[0];

	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<int64_t>(result);
	auto &out_valid = FlatVector::Validity(result);

	std::vector<std::string> prompts;
	prompts.reserve(count);

	UnifiedVectorFormat puf;
	prompt_vec.ToUnifiedFormat(count, puf);
	auto pdata = reinterpret_cast<const string_t *>(puf.data);
	for (idx_t i = 0; i < count; i++) {
		auto idx = puf.sel->get_index(i);
		if (!puf.validity.RowIsValid(idx)) {
			prompts.emplace_back();
			continue;
		}
		prompts.emplace_back(pdata[idx].GetString() + suffix_filter_prompt);
	}

	auto results = batch_check_semantic_int(prompts);
	for (idx_t i = 0; i < count; i++) {
		auto idx = puf.sel->get_index(i);
		if (!puf.validity.RowIsValid(idx)) {
			out_valid.SetInvalid(i);
			continue;
		}
		out[i] = results[i];
	}
}

static void SemanticFunctionString(DataChunk &args, ExpressionState &state, Vector &result) {
	// semantic_string(prompt) -> VARCHAR
	const std::string suffix_filter_prompt = "\n Return a single word or phrase, do not contain any other words.";
	D_ASSERT(args.ColumnCount() == 1);
	auto &prompt_vec = args.data[0];

	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<string_t>(result);
	auto &out_valid = FlatVector::Validity(result);

	std::vector<std::string> prompts;
	prompts.reserve(count);

	UnifiedVectorFormat puf;
	prompt_vec.ToUnifiedFormat(count, puf);
	auto pdata = reinterpret_cast<const string_t *>(puf.data);
	for (idx_t i = 0; i < count; i++) {
		auto idx = puf.sel->get_index(i);
		if (!puf.validity.RowIsValid(idx)) {
			prompts.emplace_back();
			continue;
		}
		prompts.emplace_back(pdata[idx].GetString() + suffix_filter_prompt);
	}

	auto results = batch_check_semantic_string(prompts);
	for (idx_t i = 0; i < count; i++) {
		auto idx = puf.sel->get_index(i);
		if (!puf.validity.RowIsValid(idx)) {
			out_valid.SetInvalid(i);
			continue;
		}
		out[i] = StringVector::AddString(result, results[i]);
	}
}

static void SemanticFunctionDouble(DataChunk &args, ExpressionState &state, Vector &result) {
	// semantic_double(prompt) -> DOUBLE
	const std::string suffix_filter_prompt = "\n Return a single floating point number, do not contain any other words.";
	D_ASSERT(args.ColumnCount() == 1);
	auto &prompt_vec = args.data[0];

	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<double>(result);
	auto &out_valid = FlatVector::Validity(result);

	std::vector<std::string> prompts;
	prompts.reserve(count);

	UnifiedVectorFormat puf;
	prompt_vec.ToUnifiedFormat(count, puf);
	auto pdata = reinterpret_cast<const string_t *>(puf.data);
	for (idx_t i = 0; i < count; i++) {
		auto idx = puf.sel->get_index(i);
		if (!puf.validity.RowIsValid(idx)) {
			prompts.emplace_back();
			continue;
		}
		prompts.emplace_back(pdata[idx].GetString() + suffix_filter_prompt);
	}

	auto results = batch_check_semantic_double(prompts);
	for (idx_t i = 0; i < count; i++) {
		auto idx = puf.sel->get_index(i);
		if (!puf.validity.RowIsValid(idx)) {
			out_valid.SetInvalid(i);
			continue;
		}
		out[i] = results[i];
	}
}

void RegisterSemanticFunctions(BuiltinFunctions &set) {
    ScalarFunction fun_semantic("semantic", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, SemanticFunction);
    fun_semantic.stability = FunctionStability::VOLATILE;
    set.AddFunction(fun_semantic);

	// SEMANTIC_BOOLEAN = SEMANTIC
	ScalarFunction fun_semantic_bool("semantic_bool", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, SemanticFunction);
	fun_semantic_bool.stability = FunctionStability::VOLATILE;
	set.AddFunction(fun_semantic_bool);

	// SEMANTIC_INT
	ScalarFunction fun_semantic_int("semantic_int", {LogicalType::VARCHAR}, LogicalType::BIGINT, SemanticFunctionInt);
	fun_semantic_int.stability = FunctionStability::VOLATILE;
	set.AddFunction(fun_semantic_int);

	// SEMANTIC_STRING
	ScalarFunction fun_semantic_string("semantic_string", {LogicalType::VARCHAR}, LogicalType::VARCHAR, SemanticFunctionString);
	fun_semantic_string.stability = FunctionStability::VOLATILE;
	set.AddFunction(fun_semantic_string);

	// SEMANTIC_DOUBLE
	ScalarFunction fun_semantic_double("semantic_double", {LogicalType::VARCHAR}, LogicalType::DOUBLE, SemanticFunctionDouble);
	fun_semantic_double.stability = FunctionStability::VOLATILE;
	set.AddFunction(fun_semantic_double);

}

} // namespace duckdb
