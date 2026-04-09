#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include <memory>
#include <string>

namespace duckdb {

// Build a ParsedExpression from a template string with placeholders like
//   "... {alias.col, alias2.col2} ..."
// Each column placeholder is turned into COALESCE(CAST(alias.col AS VARCHAR), '')
// and the full prompt is concatenated with concat(...).
std::unique_ptr<ParsedExpression> BuildSemanticPromptFromTemplate(const std::string &tmpl);

} // namespace duckdb
