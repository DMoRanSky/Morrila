#include "duckdb/planner/semantic_placeholder.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

static unique_ptr<ParsedExpression> BuildDesc(const vector<string> &qualified_cols) {
    vector<unique_ptr<ParsedExpression>> parts;
    for (idx_t idx = 0; idx < qualified_cols.size(); idx++) {
        auto &qc = qualified_cols[idx];
        auto dot = qc.find('.');
        vector<string> name_parts;
        if (dot == std::string::npos) {
            // unqualified column name: use as single-part ColumnRefExpression
            name_parts.push_back(qc);
        } else {
            string alias = qc.substr(0, dot);
            string col = qc.substr(dot + 1);
            name_parts = {alias, col};
        }
        auto colref = make_uniq<ColumnRefExpression>(std::move(name_parts));
        auto castv = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(colref));
        vector<unique_ptr<ParsedExpression>> coalesce_args;
        coalesce_args.push_back(std::move(castv));
        coalesce_args.push_back(make_uniq<ConstantExpression>(Value("")));
        parts.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(coalesce_args)));
        if (idx + 1 < qualified_cols.size()) {
            // add a single space between fields
            parts.push_back(make_uniq<ConstantExpression>(Value(" ")));
        }
    }
    if (parts.empty()) {
        return make_uniq<ConstantExpression>(Value(""));
    }
    if (parts.size() == 1) {
        return std::move(parts[0]);
    }
    return make_uniq<FunctionExpression>("concat", std::move(parts));
}

unique_ptr<ParsedExpression> BuildSemanticPromptFromTemplate(const std::string &tmpl) {
    vector<unique_ptr<ParsedExpression>> parts;
    idx_t i = 0, n = tmpl.size();
    while (i < n) {
        if (tmpl[i] == '{') {
            idx_t j = i + 1;
            while (j < n && tmpl[j] != '}') j++;
            if (j >= n){
                break;
            }
            string inside = tmpl.substr(i + 1, j - i - 1);
            string inside_trim = inside;
            StringUtil::Trim(inside_trim);
            vector<string> tokens = StringUtil::Split(inside_trim, ',');
            vector<string> cols;
            for (auto &tok : tokens) {
                string t = tok;
                StringUtil::Trim(t);
                cols.push_back(t);
            }
            parts.push_back(BuildDesc(cols));
            i = j + 1;
        } else {
            idx_t j = i;
            while (j < n && tmpl[j] != '{') j++;
            if (j > i) {
                parts.push_back(make_uniq<ConstantExpression>(Value(tmpl.substr(i, j - i))));
            }
            i = j;
        }
    }
    if (parts.empty()) {
        return make_uniq<ConstantExpression>(Value(""));
    }
    if (parts.size() == 1) {
        return std::move(parts[0]);
    }
    return make_uniq<FunctionExpression>("concat", std::move(parts));
}

} // namespace duckdb
