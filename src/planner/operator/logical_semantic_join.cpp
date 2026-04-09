#pragma once
#include "duckdb/planner/logical_operator.hpp"
#include <string>

namespace duckdb {

class LogicalSemanticJoin : public LogicalOperator {
public:
    std::string semantic_description; // 存储SEMANTIC表达式内容

    LogicalSemanticJoin(unique_ptr<LogicalOperator> left, unique_ptr<LogicalOperator> right, std::string description)
        : LogicalOperator(LogicalOperatorType::SEMANTIC_JOIN, std::move(left), std::move(right)),
          semantic_description(std::move(description)) {}

    // 按需补充GetColumnBindings/ResolveTypes等方法
};

} // namespace duckdb