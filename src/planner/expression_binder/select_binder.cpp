#include "duckdb/planner/expression_binder/select_binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/semantic_placeholder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include <iostream>

namespace duckdb {

SelectBinder::SelectBinder(Binder &binder, ClientContext &context, BoundSelectNode &node, BoundGroupInformation &info)
    : BaseSelectBinder(binder, context, node, info) {
}

BindResult SelectBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &fexpr = expr.Cast<FunctionExpression>();
		if (StringUtil::CIEquals(fexpr.function_name, "semantic") || 
		    StringUtil::CIEquals(fexpr.function_name, "semantic_bool")) {
			// std::cout << "Found semantic BOOLEAN function in SELECT: " << fexpr.ToString() << "\n";
			if (!fexpr.children.empty() && fexpr.children[0]->GetExpressionClass() == ExpressionClass::CONSTANT) {
				auto &tmpl_const = fexpr.children[0]->Cast<ConstantExpression>();
				auto tid = tmpl_const.value.type().id();
				if (tid == LogicalTypeId::VARCHAR || tid == LogicalTypeId::STRING_LITERAL) {
					string tmpl = tmpl_const.value.ToString();
					auto prompt_expr = BuildSemanticPromptFromTemplate(tmpl);
					fexpr.children[0] = std::move(prompt_expr);
				}
			}
		}
		// semantic_int
		else if (StringUtil::CIEquals(fexpr.function_name, "semantic_int")) {
			// std::cout << "Found semantic INT function in SELECT: " << fexpr.ToString() << "\n";
			if (!fexpr.children.empty() && fexpr.children[0]->GetExpressionClass() == ExpressionClass::CONSTANT) {
				auto &tmpl_const = fexpr.children[0]->Cast<ConstantExpression>();
				auto tid = tmpl_const.value.type().id();
				if (tid == LogicalTypeId::VARCHAR || tid == LogicalTypeId::STRING_LITERAL) {
					string tmpl = tmpl_const.value.ToString();
					auto prompt_expr = BuildSemanticPromptFromTemplate(tmpl);
					fexpr.children[0] = std::move(prompt_expr);
				}
			}
		}
		// semantic_string
		else if (StringUtil::CIEquals(fexpr.function_name, "semantic_string")) {
			// std::cout << "Found semantic STRING function in SELECT: " << fexpr.ToString() << "\n";
			if (!fexpr.children.empty() && fexpr.children[0]->GetExpressionClass() == ExpressionClass::CONSTANT) {
				auto &tmpl_const = fexpr.children[0]->Cast<ConstantExpression>();
				auto tid = tmpl_const.value.type().id();
				if (tid == LogicalTypeId::VARCHAR || tid == LogicalTypeId::STRING_LITERAL) {
					string tmpl = tmpl_const.value.ToString();
					auto prompt_expr = BuildSemanticPromptFromTemplate(tmpl);
					fexpr.children[0] = std::move(prompt_expr);
				}
			}
		}
		// semantic_double
		else if (StringUtil::CIEquals(fexpr.function_name, "semantic_double")) {
			// std::cout << "Found semantic DOUBLE function in SELECT: " << fexpr.ToString() << "\n";
			if (!fexpr.children.empty() && fexpr.children[0]->GetExpressionClass() == ExpressionClass::CONSTANT) {
				auto &tmpl_const = fexpr.children[0]->Cast<ConstantExpression>();
				auto tid = tmpl_const.value.type().id();
				if (tid == LogicalTypeId::VARCHAR || tid == LogicalTypeId::STRING_LITERAL) {
					string tmpl = tmpl_const.value.ToString();
					auto prompt_expr = BuildSemanticPromptFromTemplate(tmpl);
					fexpr.children[0] = std::move(prompt_expr);
				}
			}
		}
	}
	return BaseSelectBinder::BindExpression(expr_ptr, depth, root_expression);
}

unique_ptr<ParsedExpression> SelectBinder::GetSQLValueFunction(const string &column_name) {
	auto alias_entry = node.bind_state.alias_map.find(column_name);
	if (alias_entry != node.bind_state.alias_map.end()) {
		// don't replace SQL value functions if they are in the alias map
		return nullptr;
	}
	return ExpressionBinder::GetSQLValueFunction(column_name);
}

BindResult SelectBinder::BindColumnRef(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	// first try to bind the column reference regularly
	auto result = BaseSelectBinder::BindColumnRef(expr_ptr, depth, root_expression);
	if (!result.HasError()) {
		return result;
	}
	// binding failed
	// check in the alias map
	auto &colref = (expr_ptr.get())->Cast<ColumnRefExpression>();
	if (!colref.IsQualified()) {
		auto &bind_state = node.bind_state;
		auto alias_entry = node.bind_state.alias_map.find(colref.column_names[0]);
		if (alias_entry != node.bind_state.alias_map.end()) {
			// found entry!
			auto index = alias_entry->second;
			if (index >= node.bound_column_count) {
				throw BinderException("Column \"%s\" referenced that exists in the SELECT clause - but this column "
				                      "cannot be referenced before it is defined",
				                      colref.column_names[0]);
			}
			if (bind_state.AliasHasSubquery(index)) {
				throw BinderException("Alias \"%s\" referenced in a SELECT clause - but the expression has a subquery."
				                      " This is not yet supported.",
				                      colref.column_names[0]);
			}
			auto copied_expression = node.bind_state.BindAlias(index);
			result = BindExpression(copied_expression, depth, false);
			return result;
		}
	}
	// entry was not found in the alias map: return the original error
	return result;
}

bool SelectBinder::QualifyColumnAlias(const ColumnRefExpression &colref) {
	if (!colref.IsQualified()) {
		return node.bind_state.alias_map.find(colref.column_names[0]) != node.bind_state.alias_map.end();
	}
	return false;
}

} // namespace duckdb
