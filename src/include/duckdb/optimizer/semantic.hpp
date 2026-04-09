//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/semantic.hpp
//
// Rewrites semantic joins into CROSS PRODUCT + FILTER with semantic(...) predicate.
// Keeps order (no join order changes). Minimal, opt-in pass.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include <optional>
#include <functional>

namespace duckdb {

class LogicalFilter;
class LogicalProjection;

class SemanticOptimizer {
public:
	explicit SemanticOptimizer(Optimizer &optimizer) : optimizer(optimizer) {}
	// Entry: returns a possibly rewritten plan
	unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> plan);
	unique_ptr<LogicalOperator> RewriteJoin(unique_ptr<LogicalOperator> plan);
	enum class Mode { NONE, PULLUP, COSTMODEL };
	unique_ptr<LogicalOperator> RunDPPlacement(unique_ptr<LogicalOperator> plan);

private:
	Optimizer &optimizer;
	unique_ptr<LogicalOperator> SplitSemanticConjunctions(unique_ptr<LogicalOperator> plan);
	unique_ptr<LogicalOperator> PullUpSemanticProjections(unique_ptr<LogicalOperator> plan);

	struct FilterLocation {
		std::reference_wrapper<unique_ptr<LogicalOperator>> grandparent_ref;
		std::reference_wrapper<unique_ptr<LogicalOperator>> parent_ref;
		std::reference_wrapper<unique_ptr<LogicalOperator>> filter_ref;
	};

	struct ProjectionLocation {
		std::reference_wrapper<unique_ptr<LogicalOperator>> grandparent_ref;
		std::reference_wrapper<unique_ptr<LogicalOperator>> parent_ref;
		std::reference_wrapper<unique_ptr<LogicalOperator>> projection_ref;
	};

	// Semantic projection helpers
	bool IsSemanticProjectionExpr(Expression &expr) const;
	bool HasSemanticProjectionExprs(LogicalProjection &proj) const;
	bool ParentReferencesProjection(LogicalOperator &parent, idx_t proj_table_index) const;
	bool ExpressionReferencesTable(Expression &expr, idx_t table_index) const;
	
	// Find semantic projection that can be pulled up
	std::optional<ProjectionLocation> FindSemanticProjection(
		unique_ptr<LogicalOperator> &op,
		std::optional<std::reference_wrapper<unique_ptr<LogicalOperator>>> parent);
	
	// Perform the projection swap
	void SwapProjectionWithParent(ProjectionLocation &location);

	bool IsSemanticExpr(Expression &expr) const;
	bool IsPureSemanticExpr(Expression &expr) const;
	bool IsPureSemanticFilter(const LogicalFilter &filter) const;
	bool NodeHasSemantic(const LogicalOperator &op) const;
	int CountTablesInSemanticExpr(Expression &expr) const;
	
	unique_ptr<LogicalOperator> RewriteJoinIfSemantic(unique_ptr<LogicalOperator> op);

	// Find semantic filter in the tree
	std::optional<FilterLocation> FindSemanticFilter(unique_ptr<LogicalOperator> &op, 
	                                            std::optional<std::reference_wrapper<unique_ptr<LogicalOperator>>> parent);

	// Filter-parent swapping for semantic filters - pullup chain approach
	void SwapFilterWithParent(FilterLocation &location);
	void FuseFilterWithParent(FilterLocation &location, const std::string &model_name = "gpt-5");
};

} // namespace duckdb;
