//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/semantic.cpp
//
// Simplified semantic optimizer that handles JOIN and FILTER operations.
// Pushes filters to the top and handles basic projections.
//
//===----------------------------------------------------------------------===//

#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include <iostream>
#include <memory>
#include <string>
#include <set>
#include <sstream>
#include "duckdb/optimizer/semantic.hpp"
#include "duckdb/common/llm_query.hpp"
#include <cstdlib>

namespace duckdb {

static bool SemanticDebugEnabled() {
	const char *env = std::getenv("DUCKDB_SEMANTIC_DEBUG");
	return env && env[0] != '\0' && env[0] != '0';
}

// Enhanced ToString with optional per-node annotations injected inside each box.
static std::string EnhancedToStringWithAnnotations(
	LogicalOperator *plan,
	const std::unordered_map<LogicalOperator*, std::vector<std::string>> *annotations = nullptr) {
	if (!plan) return "";

	// Assign node ids (preorder)
	std::unordered_map<LogicalOperator*, idx_t> node_ids;
	idx_t next_id = 0;
	std::function<void(LogicalOperator*)> assign_ids = [&](LogicalOperator *n) {
		if (!n) return;
		node_ids[n] = next_id++;
		for (auto &c : n->children) assign_ids(c.get());
	};
	assign_ids(plan);

	// Preorder list to align annotation lookup with meta_lines
	std::vector<LogicalOperator*> preorder_nodes(next_id, nullptr);
	for (auto &p : node_ids) {
		preorder_nodes[p.second] = p.first;
	}

	// Subtree table indices
	std::unordered_map<LogicalOperator*, std::unordered_set<idx_t>> subtree_table_indices;
	std::function<const std::unordered_set<idx_t>&(LogicalOperator*)> collect_subtree_tables;
	collect_subtree_tables = [&](LogicalOperator *n) -> const std::unordered_set<idx_t>& {
		if (!n) return subtree_table_indices[n];
		auto it = subtree_table_indices.find(n);
		if (it != subtree_table_indices.end()) return it->second;
		std::unordered_set<idx_t> set;
		auto own_tbls = n->GetTableIndex();
		for (auto t : own_tbls) set.insert((idx_t)t);
		for (auto &c : n->children) {
			if (!c) continue;
			auto &child_set = collect_subtree_tables(c.get());
			set.insert(child_set.begin(), child_set.end());
		}
		auto &ref = subtree_table_indices[n];
		ref = std::move(set);
		return subtree_table_indices[n];
	};
	for (auto &p : node_ids) collect_subtree_tables(p.first);

	// Map leaf table -> owning projection index (if any)
	std::unordered_map<idx_t, std::unordered_set<idx_t>> proj_index_to_subtrees;
	for (auto &p : node_ids) {
		auto node = p.first;
		if (node && node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &proj = node->Cast<LogicalProjection>();
			idx_t pidx = proj.table_index;
			auto it = subtree_table_indices.find(node);
			if (it != subtree_table_indices.end()) {
				proj_index_to_subtrees[pidx] = it->second;
			}
		}
	}
	std::unordered_map<idx_t, idx_t> leaf_to_proj;
	for (auto &pp : proj_index_to_subtrees) {
		idx_t proj_idx = pp.first;
		for (auto leaf : pp.second) leaf_to_proj[leaf] = proj_idx;
	}

	// Collect metadata strings (preorder)
	std::vector<std::string> meta_lines;
	std::function<void(LogicalOperator*)> collect_meta = [&](LogicalOperator *node) {
		if (!node) return;
		idx_t nid = node_ids[node];
		std::string meta = "NODE[" + std::to_string(nid) + "] TYPE=" + std::to_string((int)node->type);
		if (!node->children.empty()) {
			meta += " CH=[";
			for (size_t i = 0; i < node->children.size(); ++i) {
				if (i) meta += ",";
				auto cit = node_ids.find(node->children[i].get());
				if (cit != node_ids.end()) meta += std::to_string(cit->second);
			}
			meta += "]";
		}
		auto sit = subtree_table_indices.find(node);
		if (sit != subtree_table_indices.end() && !sit->second.empty()) {
			meta += " TBLS={";
			std::set<idx_t> st(sit->second.begin(), sit->second.end());
			std::set<idx_t> mapped;
			for (auto t : st) {
				auto pit = leaf_to_proj.find(t);
				mapped.insert((pit != leaf_to_proj.end()) ? pit->second : t);
			}
			bool first = true;
			for (auto mt : mapped) {
				if (!first) meta += ",";
				meta += std::to_string(mt);
				first = false;
			}
			meta += "}";
		}
		meta_lines.push_back(meta);
		for (auto &c : node->children) collect_meta(c.get());
	};
	collect_meta(plan);

	// Inject metadata into default ToString output
	std::string orig = plan->ToString();
	std::istringstream iss(orig);
	std::string line;
	std::vector<std::string> all_lines;
	while (std::getline(iss, line)) all_lines.push_back(line);

	std::string result;
	size_t meta_idx = 0;
	for (size_t i = 0; i < all_lines.size(); ++i) {
		result += all_lines[i] + "\n";
		// Inject after the interior horizontal rule line inside a box:
		// pattern: line begins and ends with the vertical bar and contains dashes.
		if (i + 1 < all_lines.size()) {
			const std::string &cur = all_lines[i];
			bool is_box_rule = StringUtil::StartsWith(cur, "│") && StringUtil::EndsWith(cur, "│") && cur.find("─") != std::string::npos;
			if (is_box_rule && meta_idx < meta_lines.size()) {
				std::string meta = meta_lines[meta_idx];
				// Determine visible content width between the vertical bars by counting UTF-8 code points
				auto utf8_len = [](const std::string &s) {
					size_t cnt = 0;
					for (size_t i = 0; i < s.size(); ) {
						unsigned char c = (unsigned char)s[i];
						if (c < 0x80) {
							i += 1;
						} else if ((c >> 5) == 0x6) {
							i += 2;
						} else if ((c >> 4) == 0xE) {
							i += 3;
						} else if ((c >> 3) == 0x1E) {
							i += 4;
						} else {
							i += 1; // fallback
						}
						cnt++;
					}
					return cnt;
				};
				size_t total_cp = utf8_len(cur);
				size_t content_width = total_cp >= 2 ? (total_cp - 2) : 0; // exclude the two border bars
				auto pad_and_trunc = [&](const std::string &text) {
					std::string line = "│";
					size_t tlen = utf8_len(text);
					if (tlen <= content_width) {
						size_t pad_l = (content_width - tlen) / 2;
						size_t pad_r = content_width - tlen - pad_l;
						line += std::string(pad_l, ' ') + text + std::string(pad_r, ' ');
					} else {
						// truncate by code points
						std::string truncated;
						size_t cp = 0;
						for (size_t i = 0; i < text.size() && cp < content_width; ) {
							unsigned char c = (unsigned char)text[i];
							size_t step = 1;
							if (c < 0x80) step = 1;
							else if ((c >> 5) == 0x6) step = 2;
							else if ((c >> 4) == 0xE) step = 3;
							else if ((c >> 3) == 0x1E) step = 4;
							if (i + step > text.size()) break;
							truncated.append(text, i, step);
							i += step;
							cp++;
						}
						line += truncated;
						// if truncated shorter than width, pad right
						if (cp < content_width) line += std::string(content_width - cp, ' ');
					}
					line += "│";
					return line;
				};
				result += pad_and_trunc(meta) + "\n";
				// Optional annotations per node (same preorder index)
				if (annotations && meta_idx < preorder_nodes.size()) {
					LogicalOperator *node = preorder_nodes[meta_idx];
					auto it_anno = annotations->find(node);
					if (it_anno != annotations->end()) {
						for (auto &anno : it_anno->second) {
							result += pad_and_trunc(anno) + "\n";
						}
					}
				}
				meta_idx++;
			}
		}
	}

	return result;
}

static std::string EnhancedToString(LogicalOperator *plan) {
	return EnhancedToStringWithAnnotations(plan, nullptr);
}

static bool IsSemanticFunctionName(const std::string &func_name) {
	if (StringUtil::CIEquals(func_name, "semantic")) {
		return true;
	}
	auto lowered = StringUtil::Lower(func_name);
	return lowered.find("semantic") != std::string::npos;
}

// Helper function to detect semantic expressions recursively
static bool IsSemanticExprRecursive(Expression &expr) {
	// Check if it's a function expression with semantic in the name
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &bf = expr.Cast<BoundFunctionExpression>();
		std::string func_name = bf.function.name;
		// Check if function name contains 'semantic' (case insensitive)
		if (IsSemanticFunctionName(func_name)) {
			return true;
		}
	}
	// Recursively check children
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		if (!found && IsSemanticExprRecursive(child)) {
			found = true;
		}
	});
	return found;
}

static bool IsSwapParentAllowed(LogicalOperatorType type) {
	switch (type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_JOIN:
		return true;
	default:
		return false;
	}
}


static void ClearProjectionMaps(LogicalOperator &op) {
	if (!op.HasProjectionMap()){
		return;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &f = op.Cast<LogicalFilter>();
		f.projection_map.clear();
		break;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &f = op.Cast<LogicalComparisonJoin>();
		f.left_projection_map.clear();
		f.right_projection_map.clear();
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &f = op.Cast<LogicalAnyJoin>();
		f.left_projection_map.clear();
		f.right_projection_map.clear();
		break;
	}
	default: {
		if (SemanticDebugEnabled()) {
			std::cout << "[SwapFilterWithParent] WARNING: Parent has projection_map but is not a handled type: \n"
					  << op.ToString() << "\n";
		}
		break;
	}
	}
}
// Recursively collect referenced table indices from an expression
static void CollectReferencedTables(Expression &expr, std::unordered_set<idx_t> &tables) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		// Note: depth may be > 0 for correlated refs; we still count its table index here
		tables.insert(colref.binding.table_index);
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { CollectReferencedTables(child, tables); });
}
// copy from pushdown_projection.cpp
static void ReplaceExpressionBinding(vector<unique_ptr<Expression>> &proj_expressions, Expression &expr,
                                     idx_t proj_table_idx) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		bool found_proj_col = false;
		BoundColumnRefExpression &colref = expr.Cast<BoundColumnRefExpression>();
		// find the corresponding column index in the projection expressions
		for (idx_t proj_idx = 0; proj_idx < proj_expressions.size(); proj_idx++) {
			auto &proj_expr = *proj_expressions[proj_idx];
			if (proj_expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				if (colref.ToString() == proj_expr.ToString()) {
					colref.binding.table_index = proj_table_idx;
					colref.binding.column_index = proj_idx;
					found_proj_col = true;
				}
			}
		}
		if (!found_proj_col) {
			// Project a new column
			auto new_colref = colref.Copy();
			colref.binding.table_index = proj_table_idx;
			colref.binding.column_index = proj_expressions.size();
			proj_expressions.push_back(std::move(new_colref));
		}
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](Expression &child) { return ReplaceExpressionBinding(proj_expressions, child, proj_table_idx); });
}

static auto FetchPromptFromSemanticFilter(Expression &expr) -> std::string {
	// For simplicity, we assume the prompt is stored as a string literal argument to the semantic function
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &fexpr = expr.Cast<BoundFunctionExpression>();
		std::string func_name = fexpr.function.name;
		if (StringUtil::CIEquals(func_name, "semantic")){
			// std::cout <<"[FetchPromptFromSemanticFilter] Found semantic function: " <<func_name <<"\n" << fexpr.alias <<"\n" << fexpr.GetAlias() <<"\n";
			return fexpr.alias;
		}
	}
	return "";
}

// Helper: Add offset to column indices in an expression that reference a specific table
// Used when pulling projection up past a JOIN (column indices shift due to schema merge)
static void AddColumnOffset(Expression &expr, idx_t target_table_index, idx_t offset) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == target_table_index) {
			colref.binding.column_index += offset;
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		AddColumnOffset(child, target_table_index, offset);
	});
}

// Helper: Update table index references in an expression
static void UpdateTableIndex(Expression &expr, idx_t old_table_index, idx_t new_table_index) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == old_table_index) {
			colref.binding.table_index = new_table_index;
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		UpdateTableIndex(child, old_table_index, new_table_index);
	});
}

// Get the number of output columns from an operator
static idx_t GetOutputColumnCount(LogicalOperator &op) {
	// For most operators, we can get this from the types vector
	return op.types.size();
}

// Find the type of a column binding by traversing the operator tree to leaf nodes
// This is necessary because intermediate operators (like CROSS_PRODUCT after restructuring)
// may have stale types vectors that don't reflect the current tree structure
static LogicalType FindBindingType(LogicalOperator &op, const ColumnBinding &binding) {
	// For leaf operators (SEQ_SCAN, GET, TABLE_FUNCTION, etc.), 
	// the types vector is authoritative
	bool is_leaf = op.children.empty();
	
	if (is_leaf) {
		auto bindings = op.GetColumnBindings();
		for (size_t i = 0; i < bindings.size() && i < op.types.size(); i++) {
			if (bindings[i].table_index == binding.table_index &&
			    bindings[i].column_index == binding.column_index) {
				return op.types[i];
			}
		}
		return LogicalType::ANY;
	}
	
	// For PROJECTION, check if this projection produces the binding
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = op.Cast<LogicalProjection>();
		if (proj.table_index == binding.table_index && 
		    binding.column_index < proj.expressions.size()) {
			// Get the return type of the expression at this column index
			return proj.expressions[binding.column_index]->return_type;
		}
	}
	
	// For non-leaf operators, recurse into children to find the source
	// Check which child can produce this binding
	for (auto &child : op.children) {
		if (!child) continue;
		
		auto child_bindings = child->GetColumnBindings();
		for (const auto &cb : child_bindings) {
			if (cb.table_index == binding.table_index &&
			    cb.column_index == binding.column_index) {
				// This child can produce the binding, recurse into it
				return FindBindingType(*child, binding);
			}
		}
	}
	
	return LogicalType::ANY;
}

// Collect all column bindings referenced by an expression
static void CollectReferencedBindings(Expression &expr, std::unordered_set<idx_t> &table_indices,
                                      std::vector<ColumnBinding> &bindings) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		table_indices.insert(colref.binding.table_index);
		bindings.push_back(colref.binding);
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		CollectReferencedBindings(child, table_indices, bindings);
	});
}

// Add pass-through columns to a projection so it exposes columns that ancestors need
// This is necessary when pulling a projection up past a JOIN, because the ancestors
// may reference columns from the JOIN's other child
static void AddPassThroughColumns(LogicalProjection &proj, LogicalOperator &grandparent, 
                                  LogicalOperator &new_child, idx_t proj_table_index, bool debug) {
	// Collect all column bindings that grandparent references
	std::unordered_set<idx_t> referenced_tables;
	std::vector<ColumnBinding> referenced_bindings;
	
	for (auto &expr : grandparent.expressions) {
		if (expr) {
			CollectReferencedBindings(*expr, referenced_tables, referenced_bindings);
		}
	}
	
	if (debug) {
		std::cout << "[AddPassThroughColumns] Grandparent references tables: {";
		bool first = true;
		for (auto t : referenced_tables) {
			if (!first) std::cout << ", ";
			std::cout << t;
			first = false;
		}
		std::cout << "}\n";
	}
	
	// Get bindings that the new child (JOIN) can provide
	auto child_bindings = new_child.GetColumnBindings();
	
	// For each binding that grandparent references but is NOT from this projection,
	// we need to add it as a pass-through column
	std::unordered_set<idx_t> existing_proj_cols;
	for (size_t i = 0; i < proj.expressions.size(); i++) {
		// Track column indices we already expose
		existing_proj_cols.insert(i);
	}
	
	idx_t next_col_idx = proj.expressions.size();
	for (const auto &binding : referenced_bindings) {
		// Skip bindings that reference this projection itself
		if (binding.table_index == proj_table_index) {
			continue;
		}
		
		// Check if this binding is available from the child
		bool found_in_child = false;
		for (const auto &child_binding : child_bindings) {
			if (child_binding.table_index == binding.table_index && 
			    child_binding.column_index == binding.column_index) {
				found_in_child = true;
				break;
			}
		}
		
		if (found_in_child) {
			// Check if we already have a pass-through for this binding
			bool already_added = false;
			for (auto &expr : proj.expressions) {
				if (expr && expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					auto &colref = expr->Cast<BoundColumnRefExpression>();
					if (colref.binding.table_index == binding.table_index &&
					    colref.binding.column_index == binding.column_index) {
						already_added = true;
						break;
					}
				}
			}
			
			if (!already_added) {
				// Add a pass-through column reference
				auto new_colref = make_uniq<BoundColumnRefExpression>(
					LogicalType::ANY, // Type will be resolved later
					binding
				);
				
				if (debug) {
					std::cout << "[AddPassThroughColumns] Adding pass-through for #[" 
					          << binding.table_index << "." << binding.column_index << "]\n";
				}
				
				proj.expressions.push_back(std::move(new_colref));
				next_col_idx++;
			}
		}
	}
}

bool SemanticOptimizer::IsSemanticExpr(Expression &expr) const {
	return IsSemanticExprRecursive(expr);
}

bool SemanticOptimizer::IsPureSemanticExpr(Expression &expr) const {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &bf = expr.Cast<BoundFunctionExpression>();
	return StringUtil::CIEquals(bf.function.name, "semantic");
}

bool SemanticOptimizer::IsPureSemanticFilter(const LogicalFilter &filter) const {
	if (filter.expressions.empty()) {
		return false;
	}
	for (auto &expr : filter.expressions) {
		if (!expr || !IsPureSemanticExpr(*expr)) {
			return false;
		}
	}
	return true;
}

int SemanticOptimizer::CountTablesInSemanticExpr(Expression &expr) const {
	// Count distinct logical table indices referenced anywhere in the expression tree
	std::unordered_set<idx_t> table_indices;
	CollectReferencedTables(expr, table_indices);
	return static_cast<int>(table_indices.size());
}

bool SemanticOptimizer::NodeHasSemantic(const LogicalOperator &op) const {
	// check operator-level expressions
	for (auto &expr : op.expressions) {
		if (expr && IsSemanticExpr(*expr)) {
			return true;
		}
	}
	// Special handling for LOGICAL_ANY_JOIN (semantic joins)
	if (op.type == LogicalOperatorType::LOGICAL_ANY_JOIN) {
		auto &j = static_cast<const LogicalAnyJoin &>(op);
		// Check the join condition for semantic expressions
		if (j.condition && IsSemanticExpr(*j.condition)) {
			return true;
		}
	}
	// check children operators recursively
	for (auto &child : op.children) {
		if (child && NodeHasSemantic(*child)) {
			return true;
		}
	}
	return false;
}

unique_ptr<LogicalOperator> SemanticOptimizer::RewriteJoinIfSemantic(unique_ptr<LogicalOperator> op) {
	// Check if this operator has semantic expressions
	if (!NodeHasSemantic(*op)) {
		return op;
	}
	// Handle LOGICAL_ANY_JOIN (semantic joins) - type 53
	if (op->type == LogicalOperatorType::LOGICAL_ANY_JOIN) {
		auto &j = static_cast<LogicalAnyJoin &>(*op);
		// Extract the semantic condition from the join
		auto semantic_predicate = j.condition->Copy();
		// Build CROSS PRODUCT of original children (preserve order)
		D_ASSERT(op->children.size() == 2);
		auto left = std::move(op->children[0]);
		auto right = std::move(op->children[1]);
		auto cross = make_uniq<LogicalCrossProduct>(std::move(left), std::move(right));
		// Attach semantic filter above cross product
		auto filter = make_uniq<LogicalFilter>(std::move(semantic_predicate));
		filter->AddChild(std::move(cross));
		return std::move(filter);
	}

	// For non-semantic-join operators, return as-is
	return op;
}

// Public entry point
unique_ptr<LogicalOperator> SemanticOptimizer::Rewrite(unique_ptr<LogicalOperator> plan) {
	if (!plan){
		return plan;
	}
	auto t0 = std::chrono::high_resolution_clock::now();
	// Print detailed plans only when debugging is enabled via env var
	if (SemanticDebugEnabled()) {
		std::cout <<"Before Semantic Optimizations::\n" << EnhancedToString(plan.get()) <<"\n";
	}

	// SP pull-up (paper §2) before SJ decomposition and SF placement.
	plan = PullUpSemanticProjections(std::move(plan));
	plan = RewriteJoin(std::move(plan));
	plan = SplitSemanticConjunctions(std::move(plan));
	auto parse_mode = []() -> Mode {
		const char *m = std::getenv("DUCKDB_SEMANTIC_MODE");
		if (!m) return Mode::COSTMODEL; // default
		std::string s(m);
		for (auto &c : s) c = (char)std::tolower((unsigned char)c);
		if (s == "none") return Mode::NONE;
		if (s == "pullup" || s == "pull-up") return Mode::PULLUP;
		return Mode::COSTMODEL;
	};

	Mode mode = parse_mode();
	if (SemanticDebugEnabled()) {
		std::cout << "semantic_optimizer = " << (mode == Mode::NONE ? "off" : (mode == Mode::PULLUP ? "pullup" : "costmodel")) << "\n";
	}

	if (mode == Mode::NONE) {
		return plan;
	}
	if (mode == Mode::COSTMODEL) {
		if (SemanticDebugEnabled()) {
			std::cerr << "[DEBUG] Enter SemanticOptimizer::Rewrite" << std::endl;
		}
		// ====== Optimizer latency timing begin ======
		
		plan = RunDPPlacement(std::move(plan));
		auto t1 = std::chrono::high_resolution_clock::now();
		double ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
		if (SemanticDebugEnabled()) {
			std::cout << "[optimizer_latency_ms=" << ms << "]\n";
		}
		// ====== Optimizer latency timing end ======
		return plan;
	}
	// Pulling up semantic filters to the top
	while (true) {
		auto filter_location = FindSemanticFilter(plan, std::nullopt);
		if (!filter_location) {
			break;
		}
		SwapFilterWithParent(*filter_location);
	}
	if (SemanticDebugEnabled()) {
		std::cout <<"Rewritten by Semantic Optimizer::\n" << EnhancedToString(plan.get()) <<"\n";
	}
	return plan;
}


// 全局 ablation filter 系数，自动从环境变量 ABLA_FILTER 读取（默认0.2）
static double GetAblationFilter() {
	static double ablation_filter = []() {
		const char* f = std::getenv("ABLA_FILTER");
		if (SemanticDebugEnabled()) {
			std::cout << "[GetAblationFilter] ABLA_FILTER env var: " << (f ? f : "not set") << "\n";
		}
		if (f) return atof(f);
		return 0.2;
	}();
	return ablation_filter;
}

// 全局 ablation join 系数，自动从环境变量 ABLA_JOIN 读取（默认0.1）
static double GetAblationJoin() {
	static double ablation_join = []() {
		const char* j = std::getenv("ABLA_JOIN");
		if (SemanticDebugEnabled()) {
			std::cout << "[GetAblationJoin] ABLA_JOIN env var: " << (j ? j : "not set") << "\n";
		}
		if (j) return atof(j);
		return 0.1;
	}();
	return ablation_join;
}

// Simple type-based helpers
static double getUniqueCoef(LogicalOperatorType type) {
	if (type == LogicalOperatorType::LOGICAL_FILTER) return GetAblationFilter();
	if (type == LogicalOperatorType::LOGICAL_DELIM_JOIN || type == LogicalOperatorType::LOGICAL_ANY_JOIN || type == 
		LogicalOperatorType::LOGICAL_POSITIONAL_JOIN || type == LogicalOperatorType::LOGICAL_ASOF_JOIN || type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) return GetAblationJoin();
	if (type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) return 1.01;
	return 1;
}

// Helper to identify join-like operator types
static bool IsJoinType(LogicalOperatorType type) {
	switch (type) {
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
		return true;
	default:
		return false;
	}
}

unique_ptr<LogicalOperator> SemanticOptimizer::RunDPPlacement(unique_ptr<LogicalOperator> plan) {
	if (!plan) return plan;
	
	// Minimal DP input extraction directly on the plan tree (no separate filter extraction)
	struct DPNode { idx_t id; LogicalOperator *node; vector<idx_t> children; vector<idx_t> tables; double card; double cost; };

	std::unordered_map<LogicalOperator*, idx_t> node_ids;
	// 调试：输出unordered_map遍历顺序
	if (SemanticDebugEnabled()) {
		std::cout << "[DEBUG] RunDPPlacement node_ids order:";
		for (auto &p : node_ids) std::cout << " " << p.second;
		std::cout << std::endl;
	}
	idx_t next_id = 0;
	std::function<void(unique_ptr<LogicalOperator>&)> assign;
	assign = [&](unique_ptr<LogicalOperator> &n) {
		if (!n) return;
		node_ids[n.get()] = next_id++;
		for (auto &c : n->children) assign(c);
	};
	assign(plan);

	// We already computed node_ids above; print the textual plan followed by a compact
	// mapping from assigned node id -> single-line node description. This keeps the
	// printing inline (no separate helper) as requested.
	if (SemanticDebugEnabled()) {
		std::cout << "--- plan->ToString() (original) ---\n";
		std::cout << EnhancedToString(plan.get()) << "\n";
		std::cout << "--- plan node id mapping ---\n";
		for (auto &p : node_ids) {
			auto node = p.first;
			auto id = p.second;
			std::string nt = EnhancedToString(node);
			std::cout << "NODE " << id << ":::: \n " << nt << "\n";
		}
		std::cout << "--- end plan annotation ---\n";
	}

	vector<DPNode> nodes; nodes.reserve(node_ids.size());
	std::function<void(unique_ptr<LogicalOperator>&)> build;
	build = [&](unique_ptr<LogicalOperator> &n) {
		if (!n) return;
		LogicalOperator *ptr = n.get();
		DPNode dn; dn.id = node_ids[ptr]; dn.node = ptr;
		for (auto &c : n->children) if (c) dn.children.push_back(node_ids[c.get()]);
		auto tbls = n->GetTableIndex(); for (auto t : tbls) dn.tables.push_back((idx_t)t);
		try { dn.card = (double)ptr->EstimateCardinality(optimizer.GetContext()); } catch(...) { dn.card = 0.0; }
		dn.cost = dn.card; // naive cost = cardinality
		nodes.push_back(std::move(dn));
		for (auto &c : n->children) build(c);
	};
	build(plan);

	std::unordered_map<idx_t, string> table_names_map;
	try {
		auto &bindings = optimizer.binder.bind_context.GetBindingsList();
		for (auto &b : bindings) {
			if (!b) continue;
			// Binding::index is the table_index
			table_names_map[b->index] = b->GetAlias();
		}
	} catch (...) {
		// ignore if binder not available
	}

	auto table_name = [&](idx_t t) {
		auto it = table_names_map.find(t);
		if (it != table_names_map.end()) return it->second;
		return StringUtil::Format("tbl#%llu", (unsigned long long)t);
	};

	// Compute subtree table sets (union of table indices from leaves under each node).
	std::unordered_map<LogicalOperator*, std::unordered_set<idx_t>> subtree_table_indices;
	if (SemanticDebugEnabled()) {
		std::cout << "[DEBUG] RunDPPlacement subtree_table_indices keys:";
		for (auto &p : subtree_table_indices) std::cout << " " << p.first;
		std::cout << std::endl;
	}

	std::function<const std::unordered_set<idx_t>&(LogicalOperator*)> collect_subtree_tables;
	collect_subtree_tables = [&](LogicalOperator *n) -> const std::unordered_set<idx_t>& {
		if (!n) return subtree_table_indices[n];
		auto it = subtree_table_indices.find(n);
		if (it != subtree_table_indices.end()) return it->second;
		std::unordered_set<idx_t> set;
		// Own tables (some nodes may report table indices; include them)
		auto own_tbls = n->GetTableIndex();
		for (auto t : own_tbls) set.insert((idx_t)t);
		// Recurse children
		for (auto &c : n->children) {
			if (!c) continue;
			auto &child_set = collect_subtree_tables(c.get());
			set.insert(child_set.begin(), child_set.end());
		}
		auto &ref = subtree_table_indices[n];
		ref = std::move(set);
		return subtree_table_indices[n];
	};

	// Precompute for all nodes we discovered
	for (auto &p : node_ids) {
		auto node = p.first;
		if (node) {
			collect_subtree_tables(node);
		}
	}

	// Map projection-assigned table_index -> underlying original table set
	std::unordered_map<idx_t, std::unordered_set<idx_t>> proj_index_to_subtrees;
	if (SemanticDebugEnabled()) {
		std::cout << "[DEBUG] RunDPPlacement proj_index_to_subtrees keys:";
		for (auto &p : proj_index_to_subtrees) std::cout << " " << p.first;
		std::cout << std::endl;
	}
	for (auto &p : node_ids) {
		auto node = p.first;
		if (node && node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &proj = node->Cast<LogicalProjection>();
			idx_t pidx = proj.table_index;
			auto it = subtree_table_indices.find(node);
			if (it != subtree_table_indices.end()) {
				proj_index_to_subtrees[pidx] = it->second;
			}
		}
		// 调试：输出每个节点的children、tables
		if (SemanticDebugEnabled()) {
			std::cout << "[DEBUG] node " << p.second << " children:";
			for (auto &c : node->children) std::cout << " " << c.get();
			std::cout << " tables:";
			auto tbls = node->GetTableIndex();
			for (auto t : tbls) std::cout << " " << t;
			std::cout << std::endl;
		}
	}

	// Invert proj_index_to_subtrees: map each leaf table index -> owning projection index (if any)
	std::unordered_map<idx_t, idx_t> leaf_to_proj;
	if (SemanticDebugEnabled()) {
		std::cout << "[DEBUG] RunDPPlacement leaf_to_proj keys:";
		for (auto &p : leaf_to_proj) std::cout << " " << p.first;
		std::cout << std::endl;
	}
	for (auto &pp : proj_index_to_subtrees) {
		idx_t proj_idx = pp.first;
		for (auto leaf : pp.second) {
			leaf_to_proj[leaf] = proj_idx;
		}
	}


	// Print DP_TABLES as the top-level projection table indices (if available),
	// falling back to binder's bindings when projections are not detected.
	if (SemanticDebugEnabled()) {
		std::cout << "DP_TABLES:\n";
		if (!proj_index_to_subtrees.empty()) {
			vector<idx_t> proj_idxs;
			for (auto &pp : proj_index_to_subtrees) proj_idxs.push_back(pp.first);
			sort(proj_idxs.begin(), proj_idxs.end());
			for (auto pidx : proj_idxs) {
				auto it = table_names_map.find(pidx);
				if (it != table_names_map.end()) std::cout << "TABLE[" << pidx << "] NAME=\"" << it->second << "\"\n";
				else std::cout << "TABLE[" << pidx << "] NAME=\"tbl#" << pidx << "\"\n";
			}
		} else {
			try {
				auto &bindings = optimizer.binder.bind_context.GetBindingsList();
				for (auto &b : bindings) {
					if (!b) continue;
					std::cout << "TABLE[" << b->index << "] NAME=\"" << b->GetAlias() << "\"\n";
				}
			} catch (...) {
				std::cout << "DP_TABLES: (binder not available)\n";
			}
		}
	}
	map<int, int> table_size;
	for (auto &dn : nodes) {
		// include a compact textual summary of the node (single-line) for easier inspection
		
		// use precomputed subtree table indices if available (more stable and maps through projections)
		vector<idx_t> tblvec;
		auto itst = subtree_table_indices.find(dn.node);
		if (itst != subtree_table_indices.end()) {
			for (auto t : itst->second) {
				tblvec.push_back(t);
			}
		} else {
			// fallback to stored dn.tables
				for (auto t : dn.tables) {
					tblvec.push_back(t);
				}
		}
		// sort and dedupe for deterministic output
		sort(tblvec.begin(), tblvec.end());
		tblvec.erase(unique(tblvec.begin(), tblvec.end()), tblvec.end());

		dn.tables = tblvec;
		if (dn.children.size() == 0) {
			for (int v: tblvec)
				table_size[v] = dn.card;
		}
	}

	// Print semantic filters (scan filter nodes) -- 保证顺序唯一
	int n = node_ids.size();
	// 1. 收集所有semantic filter节点 (pair<node_id, LogicalOperator*>)
	std::vector<std::pair<idx_t, LogicalOperator*>> semantic_filters;
	for (auto &p : node_ids) {
		auto node = p.first;
		idx_t id = p.second;
		if (node->type != LogicalOperatorType::LOGICAL_FILTER) continue;
		auto &f = node->Cast<LogicalFilter>();
		if (f.expressions.empty() || !f.expressions[0] || !IsSemanticExpr(*f.expressions[0])) continue;
		semantic_filters.emplace_back(id, node);
	}
	// 2. 按node_id降序排序，reverse顺序
	std::sort(semantic_filters.begin(), semantic_filters.end(), std::greater<>());
	int m = semantic_filters.size();
	vector<vector<idx_t>> semantic_filter_tables;
	vector<std::string> semantic_filter_exprs;
	vector<int> semantic_filter_id(n, -1);
	for (int i = 0; i < m; ++i) {
		idx_t id = semantic_filters[i].first;
		LogicalOperator* node = semantic_filters[i].second;
		auto &f = node->Cast<LogicalFilter>();
		std::unordered_set<idx_t> raw_tables;
		CollectReferencedTables(*f.expressions[0], raw_tables);
		std::unordered_set<idx_t> expanded_tables;
		for (auto t : raw_tables) {
			auto itproj = proj_index_to_subtrees.find(t);
			if (itproj != proj_index_to_subtrees.end()) {
				expanded_tables.insert(itproj->second.begin(), itproj->second.end());
				continue;
			}
			expanded_tables.insert(t);
		}
		vector<idx_t> filt_tbls;
		for (auto t : expanded_tables) filt_tbls.push_back(t);
		sort(filt_tbls.begin(), filt_tbls.end());
		filt_tbls.erase(unique(filt_tbls.begin(), filt_tbls.end()), filt_tbls.end());
		semantic_filter_id[id] = i;
		semantic_filter_tables.push_back(filt_tbls);
		semantic_filter_exprs.push_back(f.expressions[0]->ToString());
		double filter_card = (double)node->EstimateCardinality(optimizer.GetContext());
		if (SemanticDebugEnabled()) {
			std::cout << "F" << i << " = FILTER_NODE[" << id << "] EXPR=(" << f.expressions[0]->ToString()
			          << ") TABLES={";
			for (size_t j = 0; j < filt_tbls.size(); ++j) {
				if (j) std::cout << ",";
				std::cout << table_name(filt_tbls[j]);
			}
			std::cout << "} CARD=" << filter_card << "\n";
		}
	}


	auto count = [&](const auto &A, int S) -> int {
		int num = 0;
		std::set<idx_t> st;
		for (auto v : A) st.insert(v);
		for (int i = 0; i < m; i++) {
			if (S >> i & 1) {
				bool ok = false;
				for (auto v : semantic_filter_tables[i])
					if (st.count(v)) { ok = true; break; }
				if (ok) num++;
			}
		}
		return num;
	};
	
	vector<vector<double> > dp(n);
	vector<int> filter_set(n, 0), ban_set(n, 0);
	vector<int> wt(n, 1);
	vector<vector<vector<int> > > state_son(n), ord(n);
	double INF = 1e100;

	std::function<double(int, int)> get_multiplies = [&](int u, int i) -> double {
		double ret = 1;
		for (int v: nodes[u].children) {
			auto val = get_multiplies(v, i);
			ret *= val;
		}
		bool ok = false;
		for (auto table_a : nodes[u].tables) {
			for (auto table_b : semantic_filter_tables[i]) {
				if (table_a == table_b) {
					ok = true;
				}
			}
		}
		if (ok && semantic_filter_id[u] == -1) {
			ret *= getUniqueCoef(nodes[u].node -> type);
			int v = getUniqueCoef(nodes[u].node -> type);
		}
		return ret;
	};

	auto format_tables = [&](const vector<idx_t> &tbls) {
		std::string out = "{";
		for (size_t i = 0; i < tbls.size(); ++i) {
			if (i) out += ",";
			out += table_name(tbls[i]);
		}
		out += "}";
		return out;
	};

	auto format_filter_mask = [&](int mask) {
		std::string out;
		for (int j = 0; j < m; j++) {
			if (mask >> j & 1) {
				if (!out.empty()) out += ",";
				out += "F" + std::to_string(j) + "~" + format_tables(semantic_filter_tables[j]);
			}
		}
		if (out.empty()) out = "{}";
		return out;
	};

	auto join_ints = [](const vector<int> &v) {
		std::string out;
		for (size_t i = 0; i < v.size(); i++) {
			if (i) out += ",";
			out += std::to_string(v[i]);
		}
		return out;
	};

	std::unordered_map<LogicalOperator*, std::vector<std::string>> node_annotations;
	
	for (int u = n - 1; u >= 0; --u) {
		DPNode &p = nodes[u];
		vector<double> w(1 << m, INF);
		vector<vector<int> > ws(1 << m);
		vector<vector<int> > od(1 << m);
		for (int i = 0; i < (1 << m); i++) ws[i] = {};
		w[0] = 0;
		dp[u] = w;
		if (semantic_filter_id[u] != -1) {
			filter_set[u] |= 1ll << semantic_filter_id[u];
		}
		for (int v: p.children) {
			wt[u] *= wt[v];
			ban_set[u] |= ban_set[v];
			vector<double> dp_ne(1 << m, INF);
			vector<vector<int> > ws_ne(1 << m);
			for (int i = 0; i < (1 << m); i++) ws_ne[i] = {};
			for (int i = filter_set[u]; ; i = (i - 1) & filter_set[u]) {
				for (int j = filter_set[v]; ; j = (j - 1) & filter_set[v]) {
					double val = dp[u][i] + dp[v][j];
					double &pre = dp_ne[i | j] ;
					if (val < pre) {
						pre = val;
						auto ne = ws[i];
						ne.push_back(j);
						ws_ne[i | j] = ne;
					}
					if (j == 0) break;
				}
				if (i == 0) break;
			}
			ws = ws_ne;
			dp[u] = dp_ne;
			filter_set[u] |= filter_set[v];
		}
		p.cost *= wt[u];
		p.card *= wt[u];
		if (IsJoinType(p.node->type)) {
			double abla_filter = GetAblationFilter();
			for (int i = filter_set[u]; ; i = (i - 1) & filter_set[u]) {
				dp[u][i] += p.cost * pow(abla_filter, count(p.tables, i));
				if (i == 0) break;
			}
		}
		vector<int> st;
		for (int i = filter_set[u]; ; i = (i - 1) & filter_set[u]) {
			st.push_back(i);
			if (i == 0) break;
		}
		reverse(st.begin(), st.end());
		double abla_filter = GetAblationFilter();
		for (int i: st) {
			for (int j = 0; j < m; j++) {
				if (ban_set[u] >> j & 1) continue;
				if (i >> j & 1) {
					if (dp[u][i ^ (1 << j)] == INF) continue;
					double sz = 1;
					for (auto table_id: semantic_filter_tables[j]) sz *= table_size[table_id];
					double cost = alpha * sz * pow(abla_filter, count(semantic_filter_tables[j], i ^ (1 << j))) * get_multiplies(u, j);
					double v = dp[u][i ^ (1 << j)] + cost;
					double &pre = dp[u][i];
					if (v < pre) {
						pre = v;
						ws[i] = ws[i ^ (1 << j)];
						od[i] = od[i ^ (1 << j)];
						od[i].push_back(j);
					}
				}
			}
		}
		state_son[u] = ws;
		ord[u] = od;
		if (!IsSwapParentAllowed(p.node -> type)) ban_set[u] |= filter_set[u];
		if (SemanticDebugEnabled()) {
			std::cout << "[DEBUG][DP] node " << u << ": filter_set=" << filter_set[u] << ", ban_set=" << ban_set[u] << ", wt=" << wt[u] << std::endl;
			std::cout << "[DEBUG][DP] dp[" << u << "] = ";
			for (int i = 0; i < (1 << m); ++i) {
				if (dp[u][i] < INF) std::cout << "(" << i << ":" << dp[u][i] << ") ";
			}
			std::cout << std::endl;
		}
		std::vector<std::string> annos;
		annos.push_back(StringUtil::Format("DP card=%.4g cost=%.4g", p.card, p.cost));
		annos.push_back("Filters=" + format_filter_mask(filter_set[u]));
		// Only show optimal state
		if (dp[u][filter_set[u]] < INF) {
			std::string line = StringUtil::Format("cost=%.4g", dp[u][filter_set[u]]);
			annos.push_back(std::move(line));
		}
		node_annotations[p.node] = std::move(annos);
	}

	vector<int > state(n, -1);
	vector<int > state_bottom(n, -1);

	state[0] = (1 << m) - 1;
	map<int, pair<int, int>> order;
	for (int u = 0; u < n; u++) {
		assert(state[u] != -1);
		DPNode &p = nodes[u];
		int st = state[u];
		int cnt = 0;
		for (int f_id: ord[u][st]) {
			order[f_id] = make_pair(u, cnt++);
		}
		for (int i = 0; i < p.children.size(); i++) {
			int v = p.children[i];
			int son_st = state_son[u][st][i];
			state[v] = son_st;
			st ^= son_st;
		}
		state_bottom[u] = st ^ state[u];
		// no verbose keep-set print
	}



	std::function<std::optional<SemanticOptimizer::FilterLocation>(
			unique_ptr<LogicalOperator> &, std::optional<std::reference_wrapper<unique_ptr<LogicalOperator>>>)> find;
	find = [&](
			unique_ptr<LogicalOperator> &op, 
			std::optional<std::reference_wrapper<unique_ptr<LogicalOperator>>> parent_opt) -> std::optional<SemanticOptimizer::FilterLocation> {
		
		if (IsSwapParentAllowed(op->type)) {
			bool parent_has_semantic = (!op->expressions.empty() && IsSemanticExpr(*op->expressions[0]));
			int parent_table_count = 0;
			if (parent_has_semantic && op->type == LogicalOperatorType::LOGICAL_FILTER) {
				parent_table_count = CountTablesInSemanticExpr(*op->expressions[0]);
			}
			for (size_t i = 0; i < op->children.size(); i++) {
				auto &child = op->children[i];
				if (!child) {
					continue;
				}
				if (child->type == LogicalOperatorType::LOGICAL_FILTER) {
					if (!child->expressions.empty() && child->expressions[0]) {
						if (IsSemanticExpr(*child->expressions[0])) {
							int child_table_count = CountTablesInSemanticExpr(*child->expressions[0]);
							
						
							if (parent_opt) {
								int u = node_ids[op.get()];
								int v = node_ids[child.get()];
								int fidx = -1;
								if (v >= 0 && v < (int)semantic_filter_id.size()) fidx = semantic_filter_id[v];
								if (fidx != -1 && (((!((state_bottom[u] >> fidx) & 1)) && semantic_filter_id[u] == -1) || (semantic_filter_id[u] != -1 
									&& order[semantic_filter_id[u]].first == order[fidx].first && order[semantic_filter_id[u]].second < order[fidx].second))) {
									FilterLocation loc{
										parent_opt.value(),  // grandparent
										std::ref(op),        // parent (the join)
										std::ref(child)      // the filter
									};
									return loc;
								}
							}
						}
					}
				}
			}
		}
		
		
		// Recursively search children
		for (auto &child : op->children) {
			if (child) {
				auto result = find(child, std::ref(op));
				if (result) {
					return result;
				}
			}
		}
		
		return std::nullopt;
	};

	while (true) {
		auto filter_location = find(plan, std::nullopt);
		if (!filter_location) {
			break;
		}
		SwapFilterWithParent(*filter_location);

	}
	if (SemanticDebugEnabled()) {
		std::cout << EnhancedToStringWithAnnotations(plan.get(), &node_annotations) <<"\n";
	}

	return plan; // no transformation yet
}

unique_ptr<LogicalOperator> SemanticOptimizer::RewriteJoin(unique_ptr<LogicalOperator> plan) {
	if (!plan){
		return plan;
	}
	// post-order: rewrite children first
	for (auto &child : plan->children) {
		child = RewriteJoin(std::move(child));
	}
	// then possibly rewrite this node
	plan = RewriteJoinIfSemantic(std::move(plan));
	return plan;
}

unique_ptr<LogicalOperator> SemanticOptimizer::SplitSemanticConjunctions(unique_ptr<LogicalOperator> op) {
	if (!op) {
		return op;
	}
	for (auto &child : op->children) {
		child = SplitSemanticConjunctions(std::move(child));
	}
	if (op->type != LogicalOperatorType::LOGICAL_FILTER) {
		return op;
	}
	auto &filter = op->Cast<LogicalFilter>();
	LogicalFilter::SplitPredicates(filter.expressions);
	if (SemanticDebugEnabled()) {
		std::cout << "[SplitSemanticConjunctions] Encountered filter. Pure semantic? "
		          << (IsPureSemanticFilter(filter) ? "yes" : "no") << "\n";
	}
	vector<unique_ptr<Expression>> semantic_exprs;
	vector<unique_ptr<Expression>> remaining_exprs;
	semantic_exprs.reserve(filter.expressions.size());
	remaining_exprs.reserve(filter.expressions.size());
	for (auto &expr : filter.expressions) {
		if (expr && IsSemanticExpr(*expr)) {
			semantic_exprs.push_back(std::move(expr));
		} else {
			remaining_exprs.push_back(std::move(expr));
		}
	}
	filter.expressions = std::move(remaining_exprs);
	if (semantic_exprs.empty()) {
		return op;
	}
	bool keep_current_filter = !filter.expressions.empty();
	unique_ptr<LogicalOperator> tail;
	if (keep_current_filter) {
		tail = std::move(op);
	} else if (filter.children.size() == 1) {
		tail = std::move(filter.children[0]);
		op.reset();
	} else {
		// Fallback: no child to keep, retain original filter to avoid null plan
		tail = std::move(op);
	}
	ClearProjectionMaps(*tail);
	for (auto it = semantic_exprs.rbegin(); it != semantic_exprs.rend(); ++it) {
		if (!(*it)) {
			continue;
		}
		auto new_filter = make_uniq<LogicalFilter>(std::move(*it));
		if (SemanticDebugEnabled()) {
			std::cout << "[SplitSemanticConjunctions] Created new semantic filter with expression: "
			          << new_filter->expressions[0]->ToString() << "\n";
		}
		if (tail) {
			new_filter->children.push_back(std::move(tail));
		}
		tail = std::move(new_filter);
		ClearProjectionMaps(*tail);
	}
	return tail;
}

bool SemanticOptimizer::IsSemanticProjectionExpr(Expression &expr) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &bf = expr.Cast<BoundFunctionExpression>();
		std::string func_name = StringUtil::Lower(bf.function.name);
		// semantic_int, semantic_string, semantic_double are projection functions
		// semantic/semantic_bool are filter functions (return boolean)
		if (func_name == "semantic_int" || func_name == "semantic_string" || 
		    func_name == "semantic_double") {
			return true;
		}
	}
	// Check children recursively
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		if (!found && IsSemanticProjectionExpr(child)) {
			found = true;
		}
	});
	return found;
}

// Check if a projection node contains semantic projection expressions
bool SemanticOptimizer::HasSemanticProjectionExprs(LogicalProjection &proj) const {
	for (auto &expr : proj.expressions) {
		if (expr && IsSemanticProjectionExpr(*expr)) {
			return true;
		}
	}
	return false;
}

// Check if an expression references a specific table index
bool SemanticOptimizer::ExpressionReferencesTable(Expression &expr, idx_t table_index) const {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == table_index) {
			return true;
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		if (!found && ExpressionReferencesTable(child, table_index)) {
			found = true;
		}
	});
	return found;
}

// Check if a parent operator references the projection's output IN A WAY THAT BLOCKS PULL-UP
bool SemanticOptimizer::ParentReferencesProjection(LogicalOperator &parent, idx_t proj_table_index) const {
	// Check all expressions in the parent
	for (auto &expr : parent.expressions) {
		if (expr && ExpressionReferencesTable(*expr, proj_table_index)) {
			return true;
		}
	}
	
	// Special handling for different parent types
	switch (parent.type) {
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &join = parent.Cast<LogicalAnyJoin>();
		if (join.condition && ExpressionReferencesTable(*join.condition, proj_table_index)) {
			return true;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = parent.Cast<LogicalComparisonJoin>();
		for (auto &cond : join.conditions) {
			if (cond.left && ExpressionReferencesTable(*cond.left, proj_table_index)) {
				return true;
			}
			if (cond.right && ExpressionReferencesTable(*cond.right, proj_table_index)) {
				return true;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		// Check if ORDER BY columns reference the projection
		auto &order = parent.Cast<LogicalOrder>();
		for (auto &bound_order : order.orders) {
			if (bound_order.expression && ExpressionReferencesTable(*bound_order.expression, proj_table_index)) {
				return true;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		// Check if DISTINCT columns reference the projection
		auto &distinct = parent.Cast<LogicalDistinct>();
		for (auto &target : distinct.distinct_targets) {
			if (target && ExpressionReferencesTable(*target, proj_table_index)) {
				return true;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_LIMIT: {
		// LIMIT doesn't reference columns, always safe
		break;
	}
	case LogicalOperatorType::LOGICAL_TOP_N: {
		// TOP_N (ORDER BY + LIMIT) - check if ORDER BY columns reference the projection
		auto &topn = parent.Cast<LogicalTopN>();
		for (auto &order : topn.orders) {
			if (order.expression && ExpressionReferencesTable(*order.expression, proj_table_index)) {
				return true;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		// For nested projections, check if any expression references the semantic projection's output
		// This is the default check done above via parent.expressions
		break;
	}
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
		// CROSS_PRODUCT doesn't have join conditions, only check expressions (done above)
		break;
	}
	default:
		break;
	}
	
	return false;
}

// Check if operator type is potentially allowed for projection pull-up
// These operators may allow pull-up IF they don't reference the semantic projection's output
static bool IsPullUpParentAllowed(LogicalOperatorType type) {
	switch (type) {
	// Safe operators that don't change schema:
	case LogicalOperatorType::LOGICAL_FILTER:      // Just filters rows
	case LogicalOperatorType::LOGICAL_LIMIT:       // Just limits row count
	case LogicalOperatorType::LOGICAL_ORDER_BY:    // Just orders rows (need to check column refs)
	case LogicalOperatorType::LOGICAL_DISTINCT:    // Just deduplicates (need to check column refs)
	case LogicalOperatorType::LOGICAL_TOP_N:       // ORDER BY + LIMIT combined
	// Operators that need column binding updates:
	case LogicalOperatorType::LOGICAL_PROJECTION:  // Need to remap column references
	// JOIN operators - bindings are passed through from children:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:    // Cartesian product, simple case
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:  // Equi-join with conditions
	case LogicalOperatorType::LOGICAL_ANY_JOIN:         // Non-equi join
	case LogicalOperatorType::LOGICAL_JOIN:             // Generic join
		return true;
	// NOT allowed:
	// - AGGREGATE: changes schema (group by + aggregates)
	// - UNION: requires schema compatibility across branches
	default:
		return false;
	}
}

// Find a semantic projection that can be pulled up
std::optional<SemanticOptimizer::ProjectionLocation> SemanticOptimizer::FindSemanticProjection(
    unique_ptr<LogicalOperator> &op,
    std::optional<std::reference_wrapper<unique_ptr<LogicalOperator>>> parent_opt) {
	
	if (!op) {
		return std::nullopt;
	}
	
	// Debug: print what we're looking at
	if (std::getenv("DUCKDB_SEMANTIC_DEBUG") && std::getenv("DUCKDB_SEMANTIC_DEBUG")[0] != '\0') {
		std::cout << "[FindSemanticProjection] Visiting node type=" << LogicalOperatorToString(op->type) 
		          << " IsPullUpAllowed=" << (IsPullUpParentAllowed(op->type) ? "yes" : "no") << "\n";
		for (size_t i = 0; i < op->children.size(); i++) {
			if (op->children[i]) {
				std::cout << "  Child[" << i << "]: " << LogicalOperatorToString(op->children[i]->type);
				if (op->children[i]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
					auto &proj = op->children[i]->Cast<LogicalProjection>();
					std::cout << " HasSemanticProj=" << (HasSemanticProjectionExprs(proj) ? "yes" : "no");
					std::cout << " exprs=[";
					for (size_t j = 0; j < proj.expressions.size(); j++) {
						if (j > 0) std::cout << ", ";
						std::cout << proj.expressions[j]->ToString();
					}
					std::cout << "]";
				}
				std::cout << "\n";
			}
		}
	}
	
	// Check if current operator allows pull-up and has a semantic projection child
	if (IsPullUpParentAllowed(op->type)) {
		for (size_t i = 0; i < op->children.size(); i++) {
			auto &child = op->children[i];
			if (!child) {
				continue;
			}
			
			// Check if child is a projection with semantic expressions
			if (child->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				auto &proj = child->Cast<LogicalProjection>();
				
				if (HasSemanticProjectionExprs(proj)) {
					// Check if parent references this projection's output - if so, can't pull up
					if (ParentReferencesProjection(*op, proj.table_index)) {
						if (std::getenv("DUCKDB_SEMANTIC_DEBUG") && std::getenv("DUCKDB_SEMANTIC_DEBUG")[0] != '\0') {
							std::cout << "[FindSemanticProjection] Parent references projection table_index=" 
							          << proj.table_index << ", cannot pull up\n";
						}
						continue;
					}
					
					// Note: We allow pull-up even if parent has semantic expressions
					// This enables optimization across multiple semantic operators
					
					// For JOIN parents, additional check: grandparent must not reference
					// columns from the JOIN's OTHER children that would become inaccessible
					// after pull-up. We handle this by adding pass-through columns later.
					// But first verify we have a grandparent to work with.
					if (!parent_opt) {
						if (std::getenv("DUCKDB_SEMANTIC_DEBUG") && std::getenv("DUCKDB_SEMANTIC_DEBUG")[0] != '\0') {
							std::cout << "[FindSemanticProjection] No grandparent available, cannot pull up\n";
						}
						continue;
					}
					
					// Found a valid candidate
					ProjectionLocation loc{
						parent_opt.value(),
						std::ref(op),
						std::ref(child)
					};
					return loc;
				}
			}
		}
	}
	
	// Recursively search children
	for (auto &child : op->children) {
		if (child) {
			auto result = FindSemanticProjection(child, std::ref(op));
			if (result) {
				return result;
			}
		}
	}
	
	return std::nullopt;
}

// Swap a semantic projection with its parent
// Handles different parent types with appropriate column binding adjustments
void SemanticOptimizer::SwapProjectionWithParent(ProjectionLocation &location) {
	auto &grandparent = location.grandparent_ref.get();
	auto &parent = location.parent_ref.get();
	auto &semantic_proj_ptr = location.projection_ref.get();
	
	if (!grandparent || !parent || !semantic_proj_ptr) {
		return;
	}
	
	auto &semantic_proj = semantic_proj_ptr->Cast<LogicalProjection>();
	D_ASSERT(semantic_proj_ptr->children.size() == 1);
	
	// Save parent type before any structural changes that might invalidate pointers
	LogicalOperatorType parent_type = parent->type;
	idx_t semantic_proj_table_index = semantic_proj.table_index;
	
	bool debug = std::getenv("DUCKDB_SEMANTIC_DEBUG") && std::getenv("DUCKDB_SEMANTIC_DEBUG")[0] != '\0';
	
	// CRITICAL: For JOIN/CROSS_PRODUCT parents, collect binding->type mapping BEFORE the swap
	// After the swap, the structure changes and types become unreliable
	std::unordered_map<uint64_t, LogicalType> binding_types_before_swap;
	auto make_binding_key = [](const ColumnBinding &b) -> uint64_t {
		return ((uint64_t)b.table_index << 32) | (uint64_t)b.column_index;
	};
	
	if (parent_type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT ||
	    parent_type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	    parent_type == LogicalOperatorType::LOGICAL_ANY_JOIN ||
	    parent_type == LogicalOperatorType::LOGICAL_JOIN) {
		// Get bindings and types from parent (JOIN/CROSS_PRODUCT)
		auto parent_bindings = parent->GetColumnBindings();
		auto &parent_types = parent->types;
		
		if (debug) {
			std::cout << "[SwapProjectionWithParent] Pre-collecting types from parent\n";
			std::cout << "  Parent has " << parent_bindings.size() << " bindings, " 
			          << parent_types.size() << " types\n";
		}
		
		for (size_t i = 0; i < parent_bindings.size() && i < parent_types.size(); i++) {
			auto key = make_binding_key(parent_bindings[i]);
			binding_types_before_swap[key] = parent_types[i];
			if (debug) {
				std::cout << "  Binding #[" << parent_bindings[i].table_index << "." 
				          << parent_bindings[i].column_index << "] -> " 
				          << parent_types[i].ToString() << "\n";
			}
		}
	}
	
	if (debug) {
		std::cout << "[SwapProjectionWithParent] Pulling up projection (table_index=" 
		          << semantic_proj.table_index << ") above parent type=" 
		          << LogicalOperatorToString(parent->type) << "\n";
		std::cout << "[SwapProjectionWithParent] Grandparent type=" 
		          << LogicalOperatorToString(grandparent->type) << " with "
		          << grandparent->expressions.size() << " expressions\n";
	}
	
	// Handle parent-type-specific adjustments BEFORE the swap
	switch (parent->type) {
	case LogicalOperatorType::LOGICAL_FILTER: {
		// FILTER: Clear projection maps, no column binding changes needed
		ClearProjectionMaps(*parent);
		break;
	}
	case LogicalOperatorType::LOGICAL_LIMIT: {
		// LIMIT: No column binding changes needed, LIMIT just counts rows
		if (debug) {
			std::cout << "[SwapProjectionWithParent] LIMIT parent - no binding changes needed\n";
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		// ORDER_BY: Check passed (order columns don't reference projection output)
		if (debug) {
			std::cout << "[SwapProjectionWithParent] ORDER_BY parent - order expressions preserved\n";
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		// DISTINCT: Check passed (distinct targets don't reference projection output)
		if (debug) {
			std::cout << "[SwapProjectionWithParent] DISTINCT parent - distinct targets preserved\n";
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_TOP_N: {
		// TOP_N: Check passed (order columns don't reference projection output)
		if (debug) {
			std::cout << "[SwapProjectionWithParent] TOP_N parent - order expressions preserved\n";
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		// PROJECTION parent: The outer projection's expressions reference the child's output.
		// Since we verified the outer projection doesn't reference the semantic projection's output,
		// its column references should point to columns that are "passed through" from below.
		// After swap, these references will still be valid because the semantic projection
		// still exposes the same non-semantic columns from its child.
		if (debug) {
			auto &outer_proj = parent->Cast<LogicalProjection>();
			std::cout << "[SwapProjectionWithParent] PROJECTION parent (table_index=" 
			          << outer_proj.table_index << ")\n";
			std::cout << "  Outer proj expressions: [";
			for (size_t i = 0; i < outer_proj.expressions.size(); i++) {
				if (i > 0) std::cout << ", ";
				std::cout << outer_proj.expressions[i]->ToString();
			}
			std::cout << "]\n";
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
		// JOIN parent: Need comprehensive column binding handling
		// After pull-up, semantic projection sits between grandparent and JOIN
		// We need to:
		// 1. Add pass-through columns to semantic projection for columns grandparent needs
		// 2. Update grandparent's expressions to reference the new column positions
		ClearProjectionMaps(*parent);
		if (debug) {
			std::cout << "[SwapProjectionWithParent] JOIN parent type=" 
			          << LogicalOperatorToString(parent->type) << "\n";
			std::cout << "  Will handle pass-through columns after structural swap\n";
		}
		break;
	}
	default:
		// Other types: just clear projection maps if present
		ClearProjectionMaps(*parent);
		break;
	}
	
	// Step 1: Detach the projection's child (what's below the projection)
	auto proj_child = std::move(semantic_proj_ptr->children[0]);
	semantic_proj_ptr->children.clear();
	
	// Step 2: Find which child index of parent has the semantic projection
	// and replace it with the projection's child
	unique_ptr<LogicalOperator> semantic_proj_node = nullptr;
	size_t proj_child_index = 0;
	for (size_t i = 0; i < parent->children.size(); i++) {
		if (parent->children[i].get() == semantic_proj_ptr.get()) {
			proj_child_index = i;
			semantic_proj_node = std::move(parent->children[i]);
			parent->children[i] = std::move(proj_child);
			break;
		}
	}
	
	D_ASSERT(semantic_proj_node != nullptr);
	
	// Step 2.5: For JOIN parents, update column bindings in semantic projection
	// The semantic projection's expressions reference its original child's columns.
	// After pull-up, it sits above the JOIN, so we need to remap references.
	// JOIN output = [left_child_cols..., right_child_cols...]
	if (parent->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	    parent->type == LogicalOperatorType::LOGICAL_ANY_JOIN ||
	    parent->type == LogicalOperatorType::LOGICAL_JOIN ||
	    parent->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		// If semantic projection was on the RIGHT child (index 1), 
		// its column references need offset by left child's column count
		if (proj_child_index == 1 && parent->children.size() >= 1) {
			// Get the left child's column count (now parent->children[0])
			idx_t left_col_count = 0;
			if (parent->children[0]) {
				left_col_count = GetOutputColumnCount(*parent->children[0]);
			}
			
			if (debug) {
				std::cout << "[SwapProjectionWithParent] Semantic proj was on RIGHT side of JOIN\n";
				std::cout << "  Left child has " << left_col_count << " columns\n";
				std::cout << "  Adding offset to semantic projection's column references\n";
			}
			
			// The semantic projection's expressions reference the original right child's table_index
			// After the swap, it needs to reference columns in the JOIN's output
			// JOIN output: [left_cols (0..left_col_count-1), right_cols (left_col_count..)]
			// So we need to add left_col_count to all column indices that reference the right child
			
			// Get the table_index that the semantic projection's expressions reference
			// This should be the table_index of what was the right child (now parent->children[1])
			idx_t original_child_table_idx = DConstants::INVALID_INDEX;
			if (parent->children[1]) {
				auto child_tables = parent->children[1]->GetTableIndex();
				if (!child_tables.empty()) {
					original_child_table_idx = child_tables[0];
				}
			}
			
			// Update column indices in the semantic projection's expressions
			auto &sem_proj = semantic_proj_node->Cast<LogicalProjection>();
			for (auto &expr : sem_proj.expressions) {
				if (expr && original_child_table_idx != DConstants::INVALID_INDEX) {
					AddColumnOffset(*expr, original_child_table_idx, left_col_count);
				}
			}
			
			if (debug) {
				std::cout << "  Updated expressions: [";
				for (size_t i = 0; i < sem_proj.expressions.size(); i++) {
					if (i > 0) std::cout << ", ";
					std::cout << sem_proj.expressions[i]->ToString();
				}
				std::cout << "]\n";
			}
		}
	}
	
	// Step 3: Insert projection above parent in grandparent
	// Make parent the child of the projection
	for (size_t i = 0; i < grandparent->children.size(); i++) {
		if (grandparent->children[i].get() == parent.get()) {
			auto parent_ptr = std::move(grandparent->children[i]);
			semantic_proj_node->children.push_back(std::move(parent_ptr));
			grandparent->children[i] = std::move(semantic_proj_node);
			break;
		}
	}
	
	if (debug) {
		std::cout << "[SwapProjectionWithParent] Structural swap completed\n";
		std::cout << "  Structure: grandparent -> semantic_proj -> parent -> child\n";
	}
	
	// Step 4: Handle pass-through columns for ALL parent types
	// After the swap, grandparent's child is now the semantic projection.
	// If grandparent references columns that aren't exposed by the semantic projection,
	// we need to add those as pass-through columns.
	// This is critical for:
	// - JOIN parents: columns from the other child become accessible through semantic projection
	// - All parents: ensure grandparent's references are satisfied
	{
		
		if (debug) {
			std::cout << "[SwapProjectionWithParent] Step 4: Handling pass-through columns\n";
		}
		
		// Find the semantic projection (now in grandparent's children)
		// It was just inserted in Step 3
		LogicalProjection *sem_proj = nullptr;
		LogicalOperator *child_op = nullptr;  // The operator now under semantic projection
		
		for (auto &gchild : grandparent->children) {
			if (gchild && gchild->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				auto &proj = gchild->Cast<LogicalProjection>();
				if (proj.table_index == semantic_proj_table_index) {
					sem_proj = &proj;
					// Get the child (the original parent, now under semantic projection)
					if (!gchild->children.empty()) {
						child_op = gchild->children[0].get();
					}
					break;
				}
			}
		}
		
		if (!sem_proj || !child_op) {
			if (debug) {
				std::cout << "[SwapProjectionWithParent] WARNING: Could not locate sem_proj or child_op!\n";
			}
		} else {
			if (debug) {
				std::cout << "[SwapProjectionWithParent] Found semantic projection (table_index=" 
				          << sem_proj->table_index << ") with " 
				          << sem_proj->expressions.size() << " expressions\n";
				std::cout << "[SwapProjectionWithParent] Child type=" 
				          << LogicalOperatorToString(child_op->type) << "\n";
			}
			
			// Collect bindings that grandparent references
			std::unordered_set<idx_t> referenced_tables;
			std::vector<ColumnBinding> referenced_bindings;
			
			for (auto &expr : grandparent->expressions) {
				if (expr) {
					CollectReferencedBindings(*expr, referenced_tables, referenced_bindings);
				}
			}
			
			if (debug) {
				std::cout << "[SwapProjectionWithParent] Grandparent references " 
				          << referenced_bindings.size() << " column bindings: ";
				for (const auto &b : referenced_bindings) {
					std::cout << "#[" << b.table_index << "." << b.column_index << "] ";
				}
				std::cout << "\n";
			}
			
			// Get bindings available from the child operator (which is now under semantic projection)
			auto child_bindings = child_op->GetColumnBindings();
			
			if (debug) {
				std::cout << "[SwapProjectionWithParent] Child provides " 
				          << child_bindings.size() << " bindings: ";
				for (const auto &b : child_bindings) {
					std::cout << "#[" << b.table_index << "." << b.column_index << "] ";
				}
				std::cout << "\n";
			}
			
			// Build a map from original binding to new column index in semantic projection
			// Start after the existing expressions
			idx_t next_col_idx = sem_proj->expressions.size();
			std::unordered_map<uint64_t, idx_t> binding_to_new_col;
			
			// Helper to create a key from binding
			auto make_key = [](const ColumnBinding &b) -> uint64_t {
				return ((uint64_t)b.table_index << 32) | (uint64_t)b.column_index;
			};
			
			// First, map existing semantic projection columns
			for (idx_t i = 0; i < sem_proj->expressions.size(); i++) {
				auto &expr = sem_proj->expressions[i];
				if (expr && expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					auto &colref = expr->Cast<BoundColumnRefExpression>();
					binding_to_new_col[make_key(colref.binding)] = i;
					if (debug) {
						std::cout << "[SwapProjectionWithParent] Existing mapping: #[" 
						          << colref.binding.table_index << "." << colref.binding.column_index 
						          << "] -> col " << i << "\n";
					}
				}
			}
			
			// Also map the semantic projection's own output columns
			for (idx_t i = 0; i < sem_proj->expressions.size(); i++) {
				binding_to_new_col[make_key(ColumnBinding(sem_proj->table_index, i))] = i;
			}
			
			// For JOIN/CROSS_PRODUCT parents: add ALL child bindings as pass-through
			// This ensures ancestors can access any column from the original child
			// For other parents: only add bindings that grandparent references
			bool add_all_child_bindings = (parent_type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT ||
			                               parent_type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
			                               parent_type == LogicalOperatorType::LOGICAL_ANY_JOIN ||
			                               parent_type == LogicalOperatorType::LOGICAL_JOIN);
			
			// Determine which bindings to add as pass-through
			std::vector<ColumnBinding> bindings_to_add;
			if (add_all_child_bindings) {
				// Add ALL child bindings (for JOIN parents)
				bindings_to_add = std::vector<ColumnBinding>(child_bindings.begin(), child_bindings.end());
				if (debug) {
					std::cout << "[SwapProjectionWithParent] Adding ALL " << bindings_to_add.size() 
					          << " child bindings as pass-through (JOIN parent)\n";
				}
			} else {
				// Only add bindings that grandparent references
				bindings_to_add = referenced_bindings;
			}
			
			// For each binding, add pass-through if not already mapped
			for (const auto &binding : bindings_to_add) {
				// Skip if this is the semantic projection's output
				if (binding.table_index == sem_proj->table_index) {
					if (debug) {
						std::cout << "[SwapProjectionWithParent] Skip binding #[" 
						          << binding.table_index << "." << binding.column_index 
						          << "] - from semantic proj output\n";
					}
					continue;
				}
				
				uint64_t key = make_key(binding);
				
				// Skip if already mapped
				if (binding_to_new_col.find(key) != binding_to_new_col.end()) {
					if (debug) {
						std::cout << "[SwapProjectionWithParent] Skip binding #[" 
						          << binding.table_index << "." << binding.column_index 
						          << "] - already mapped to col " << binding_to_new_col[key] << "\n";
					}
					continue;
				}
				
				// Check if this binding is available from the child operator
				// Use the pre-collected type information (collected before the swap)
				bool found = false;
				LogicalType col_type = LogicalType::ANY;
				for (size_t j = 0; j < child_bindings.size(); j++) {
					if (child_bindings[j].table_index == binding.table_index &&
					    child_bindings[j].column_index == binding.column_index) {
						found = true;
						// Look up type from pre-collected mapping first
						auto type_key = make_binding_key(binding);
						auto type_it = binding_types_before_swap.find(type_key);
						if (type_it != binding_types_before_swap.end()) {
							col_type = type_it->second;
						} else {
							// Fallback: recursively find the type from the source operator
							// This is more reliable than using child_op->types[j] which may be stale
							col_type = FindBindingType(*child_op, binding);
						}
						if (debug) {
							std::cout << "[SwapProjectionWithParent] Binding #[" 
							          << binding.table_index << "." << binding.column_index 
							          << "] found at j=" << j << ", type=" << col_type.ToString() << "\n";
						}
						break;
					}
				}
				
				if (found) {
					// Add pass-through column using LogicalType::ANY
					// The actual type will be resolved by DuckDB's type system later
					auto new_colref = make_uniq<BoundColumnRefExpression>(col_type, binding);
					
					if (debug) {
						std::cout << "[SwapProjectionWithParent] Adding pass-through: #[" 
						          << binding.table_index << "." << binding.column_index 
						          << "] -> col " << next_col_idx << "\n";
					}
					
					binding_to_new_col[key] = next_col_idx;
					sem_proj->expressions.push_back(std::move(new_colref));
					// Don't update sem_proj->types - let DuckDB's resolution handle it
					next_col_idx++;
				} else if (!add_all_child_bindings) {
					// Only warn if we were explicitly looking for this binding
					if (debug) {
						std::cout << "[SwapProjectionWithParent] WARNING: Binding #[" 
						          << binding.table_index << "." << binding.column_index 
						          << "] not found in child output!\n";
					}
				}
			}
			
			// Now update grandparent's expressions to reference the new column positions
			// in the semantic projection
			if (debug) {
				std::cout << "[SwapProjectionWithParent] Remapping step: binding_to_new_col has " 
				          << binding_to_new_col.size() << " entries\n";
				for (auto &kv : binding_to_new_col) {
					idx_t table_idx = kv.first >> 32;
					idx_t col_idx = kv.first & 0xFFFFFFFF;
					std::cout << "  #[" << table_idx << "." << col_idx << "] -> col " << kv.second << "\n";
				}
			}
			
			std::function<void(Expression &)> update_refs = [&](Expression &expr) {
				if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					auto &colref = expr.Cast<BoundColumnRefExpression>();
					uint64_t key = make_key(colref.binding);
					
					auto it = binding_to_new_col.find(key);
					if (it != binding_to_new_col.end()) {
						// Only remap if it's NOT already referencing the semantic projection
						if (colref.binding.table_index != sem_proj->table_index) {
							idx_t old_table = colref.binding.table_index;
							idx_t old_col = colref.binding.column_index;
							
							colref.binding.table_index = sem_proj->table_index;
							colref.binding.column_index = it->second;
							
							if (debug) {
								std::cout << "[SwapProjectionWithParent] Remapped grandparent ref: #[" 
								          << old_table << "." << old_col << "] -> #[" 
								          << colref.binding.table_index << "." 
								          << colref.binding.column_index << "]\n";
							}
						}
					}
				}
				ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
					update_refs(child);
				});
			};
			
			for (auto &expr : grandparent->expressions) {
				if (expr) {
					update_refs(*expr);
				}
			}
			
			if (debug) {
				std::cout << "[SwapProjectionWithParent] Updated semantic projection has " 
				          << sem_proj->expressions.size() << " expressions\n";
			}
		}
	}
	
	if (debug) {
		std::cout << "[SwapProjectionWithParent] All adjustments completed successfully\n";
	}
	
	// Note: After this swap, the projection now sits above the parent.
	// For non-JOIN parents, the projection's expressions still reference columns 
	// from its (now grandchild) subtree via table_index.
	// For JOIN parents, we've added pass-through columns and remapped grandparent's references.
	//
	// Structure change:
	// Before:  grandparent -> parent -> projection -> child
	// After:   grandparent -> projection -> parent -> child
}

// Main entry point for semantic projection pull-up
unique_ptr<LogicalOperator> SemanticOptimizer::PullUpSemanticProjections(unique_ptr<LogicalOperator> plan) {
	if (!plan) {
		return plan;
	}
	
	// Repeatedly find and pull up semantic projections
	while (true) {
		auto proj_location = FindSemanticProjection(plan, std::nullopt);
		if (!proj_location) {
			break;
		}
		SwapProjectionWithParent(*proj_location);
	}
	
	if (std::getenv("DUCKDB_SEMANTIC_DEBUG") && std::getenv("DUCKDB_SEMANTIC_DEBUG")[0] != '\0') {
		std::cout << "[PullUpSemanticProjections] Completed projection pull-up\n";
	}
	
	return plan;
}


// Find semantic filter in the tree
std::optional<SemanticOptimizer::FilterLocation> SemanticOptimizer::FindSemanticFilter(
    unique_ptr<LogicalOperator> &op, 
    std::optional<std::reference_wrapper<unique_ptr<LogicalOperator>>> parent_opt) {
	
	if (IsSwapParentAllowed(op->type)) {
		bool parent_has_semantic = (!op->expressions.empty() && IsSemanticExpr(*op->expressions[0]));
		int parent_table_count = 0;
		if (parent_has_semantic && op->type == LogicalOperatorType::LOGICAL_FILTER) {
			parent_table_count = CountTablesInSemanticExpr(*op->expressions[0]);
		}
		for (size_t i = 0; i < op->children.size(); i++) {
			auto &child = op->children[i];
			if (!child) {
				continue;
			}
			if (child->type == LogicalOperatorType::LOGICAL_FILTER) {
				if (!child->expressions.empty() && child->expressions[0]) {
					if (IsSemanticExpr(*child->expressions[0])) {
						int child_table_count = CountTablesInSemanticExpr(*child->expressions[0]);
						// if (parent_table_count && child_table_count) {
						// 	FilterLocation loc{
						// 		parent_opt.value(),  // grandparent
						// 		std::ref(op),        // parent (the join)
						// 		std::ref(child)      // the filter
						// 	};
						// 	FuseFilterWithParent(loc);
						// }
						if (parent_table_count >= child_table_count) {
							continue;
						}
						if (parent_has_semantic && SemanticDebugEnabled()) {
							std::cout << "[FindSemanticFilter] Both parent and child are semantic filters. "
									  << "Parent table count: " << parent_table_count
									  << ", Child table count: " << child_table_count << "\n";
							
						}
						if (parent_opt) {
							FilterLocation loc{
								parent_opt.value(),  // grandparent
								std::ref(op),        // parent (the join)
								std::ref(child)      // the filter
							};
							return loc;
						}
					}
				}
			}
		}
	}
	
	
	// Recursively search children
	for (auto &child : op->children) {
		if (child) {
			auto result = FindSemanticFilter(child, std::ref(op));
			if (result) {
				return result;
			}
		}
	}
	
	return std::nullopt;
}



void SemanticOptimizer::SwapFilterWithParent(FilterLocation &location) {
	auto &grandparent = location.grandparent_ref.get();
	auto &parent = location.parent_ref.get();
	auto &semantic_filter = location.filter_ref.get();

	if (!parent || !semantic_filter || !grandparent) {
		return;
	}

	assert(semantic_filter->children.size() == 1);

	// track original and new projection table indices (if we encounter a projection parent)
	idx_t orig_table_index = DConstants::INVALID_INDEX;
	idx_t new_proj_table_index = DConstants::INVALID_INDEX;

	if (parent->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = parent->Cast<LogicalProjection>();
		idx_t orig = proj.expressions.size();
		vector<unique_ptr<Expression>> copy_proj;
		for (auto &e : proj.expressions) {
			copy_proj.push_back(e->Copy());
		}
		vector<unique_ptr<Expression>> new_filters;

		orig_table_index = proj.table_index;
		for (auto &f : semantic_filter->expressions) {
			auto cf = f->Copy();
			ReplaceExpressionBinding(copy_proj, *cf, proj.table_index);
			new_filters.push_back(std::move(cf));
		}
		if (copy_proj.size() > orig) {
			for (idx_t i = orig; i < copy_proj.size(); ++i) {
				proj.expressions.push_back(std::move(copy_proj[i]));
			}
		}
		semantic_filter->expressions = std::move(new_filters);
	} else {
		if (parent) {
			// Clear projection maps in parent to avoid inconsistencies
			ClearProjectionMaps(*parent);
		}
	}

	auto &filter = semantic_filter->Cast<LogicalFilter>();
	// Clear projection map in the filter to avoid inconsistencies
	filter.projection_map.clear();
	
	
	// Replace the semantic filter in parent's children with its child
	duckdb::unique_ptr<LogicalOperator> semantic_filter_node = nullptr;

	for (size_t i = 0; i < parent->children.size(); i++) {
		if (parent->children[i].get() == semantic_filter.get()) {
			auto filter_child = std::move(semantic_filter->children[0]);
			semantic_filter->children.clear();
			semantic_filter_node = std::move(semantic_filter);
			parent->children[i] = std::move(filter_child);
			break;
		}
	}

	assert(semantic_filter_node != nullptr);

	for (size_t i = 0; i < grandparent->children.size(); i++) {
		if (grandparent->children[i].get() == parent.get()) {
			// Move parent out of grandparent before using it
			auto parent_ptr = std::move(grandparent->children[i]);
			// Make parent the child of the semantic filter
			semantic_filter_node->children.push_back(std::move(parent_ptr));
			// Replace parent with filter in grandparent
			grandparent->children[i] = std::move(semantic_filter_node);
			break;
		}
	}
	
}

void SemanticOptimizer::FuseFilterWithParent(FilterLocation &location, const std::string &model_name) {
	auto &grandparent = location.grandparent_ref.get();
	auto &parent = location.parent_ref.get();
	auto &child = location.filter_ref.get();

	if (!parent || !child || !grandparent) {
		return;
	}

	if(parent->type != LogicalOperatorType::LOGICAL_FILTER ||
	   child->type != LogicalOperatorType::LOGICAL_FILTER) {
		if (SemanticDebugEnabled()) {
			std::cout << "[FuseFilterWithParent] Either parent or child is not a filter. Parent type: "
					  << LogicalOperatorToString(parent->type)
					  << ", Child type: " << LogicalOperatorToString(child->type) << "\n";
		}
		return;
	}

	if(!IsPureSemanticFilter(parent->Cast<LogicalFilter>()) ||
	   !IsPureSemanticFilter(child->Cast<LogicalFilter>())) {
		if (SemanticDebugEnabled()) {
			std::cout << "[FuseFilterWithParent] Either parent or child filter is not pure semantic.\n";
		}
		return;
	}

	std::string prompt_child = FetchPromptFromSemanticFilter(*child->expressions[0]);
	std::string prompt_parent = FetchPromptFromSemanticFilter(*parent->expressions[0]);
	
	if (SemanticDebugEnabled()) {
		std::cout << "[FuseFilterWithParent] Child prompt: " << prompt_child << "\n";
		std::cout << "[FuseFilterWithParent] Parent prompt: " << prompt_parent << "\n";
	}

	std::string fused_prompt = fuse_semantic_filters(prompt_parent, prompt_child, model_name);

	if (SemanticDebugEnabled()) {
		std::cout << "[FuseFilterWithParent] Fused prompt: " << fused_prompt << "\n";
	}

	unique_ptr<Expression> fused_expr = make_uniq<BoundConstantExpression>(Value(fused_prompt));
	auto filter_expr = optimizer.BindScalarFunction(
		"semantic", std::move(fused_expr)
	);
	if (SemanticDebugEnabled()) {
		std::cout << "[FuseFilterWithParent] Created fused semantic expression: " << filter_expr->ToString() << "\n";
	}
	// replace parent's expression with fused expression
	parent->expressions[0] = std::move(filter_expr);
	// remove child filter from parent's children
	for (size_t i = 0; i < parent->children.size(); i++) {
		if (parent->children[i].get() == child.get()) {
			// Move child's child to parent
			auto child_child = std::move(child->children[0]);
			child->children.clear();
			parent->children[i] = std::move(child_child);
			break;
		}
	}
} 
}// namespace duckdb