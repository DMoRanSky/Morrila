# Morrila: Cost-Based Placement of Semantic Operators in Hybrid Query Plans

Morrila is a plan-level optimizer for hybrid semantic-relational SQL execution.
It optimizes where semantic operators are placed in query plans to reduce total cost while preserving exact operator semantics.

This repository is a research fork based on DuckDB:

- Upstream: https://github.com/duckdb/duckdb

## Overview

Recent database systems support semantic operators (LLM-powered predicates/projections/joins) together with relational operators.
The key optimization problem is semantic operator placement:

- Push down semantic operators: fewer relational rows, but more LLM calls.
- Pull up semantic operators: fewer LLM calls via caching, but potentially larger relational intermediates.

Morrila addresses this with:

1. Equivalence-preserving rewrites that reduce optimization to semantic filter placement.
2. A pull-up strategy that minimizes LLM invocations under function caching.
3. A DP-based cost model that balances LLM and relational costs.

## Results (from paper evaluation)

Across 44 semantic SQL queries over five schemas and two benchmarks, Morrila reports:

- Up to 1.5x speedup.
- Up to 4.3x cost reduction.
- Strong quality preservation (around 0.85 F1 in reported experiments).

## Reproduction Guide

### Prerequisites

- CMake
- C++ toolchain (clang/gcc)
- OpenAI API key for semantic function calls

### Step 1: Build

From repository root:

    cmake .
    make

### Step 2: Configure environment

Set API key:

    export API_KEY="your-openai-api-key"

Enable semantic optimizer:

    export SEMANTIC_OPTIMIZER=true

Optional debug info:

    export DUCKDB_SEMANTIC_DEBUG=1

Optional worker count:

    export NUM_LLM_WORKERS=32

### Step 3: Select optimization mode

Morrila supports three modes:

- costmodel: DP-based cost model (default)
- pullup: aggressive filter pull-up
- none: disable semantic optimizer

Use:

    export DUCKDB_SEMANTIC_MODE=costmodel

or

    export DUCKDB_SEMANTIC_MODE=pullup

or

    export DUCKDB_SEMANTIC_MODE=none

### Step 4: Run semantic SQL queries

Use your own runner script to execute queries under [test/semantic/demo_queries](test/semantic/demo_queries) with `./duckdb -unsigned < <query.sql>`.

### Step 5: Reproduce the main benchmark

The main hybrid benchmark queries live under [test/semantic/agent_bench](test/semantic/agent_bench).

Use your own script to run [test/semantic/agent_bench/Q1.sql](test/semantic/agent_bench/Q1.sql) through [test/semantic/agent_bench/Q30.sql](test/semantic/agent_bench/Q30.sql).

Execution is separate from metric aggregation. LLM call count, token usage, and the values needed for cost accounting are recorded in the generated log at [llm_calls_gpt-5-mini.log](llm_calls_gpt-5-mini.log). For our paper numbers, derive cost from the logged GPT-5 mini token usage and the provider pricing you used in the run.

Fetch all LLM metrics from [llm_calls_gpt-5-mini.log](llm_calls_gpt-5-mini.log) and write your own aggregation script for call count, tokens, and cost.

To reproduce the paper numbers, keep the same environment variables, run the same query set, and aggregate runtime from the terminal output with LLM calls/tokens from [llm_calls_gpt-5-mini.log](llm_calls_gpt-5-mini.log).

## Query Inventory

Current public semantic query files in this repo:

- [test/semantic/demo_queries/semantic_filter_1.sql](test/semantic/demo_queries/semantic_filter_1.sql)
- [test/semantic/agent_bench/](test/semantic/agent_bench/) includes 30 main queries and 14 queries from ecomm in DataAgentBench.
The 30-query hybrid benchmark described in the paper is stored in [test/semantic/agent_bench](test/semantic/agent_bench). It follows the same execution pattern as the demo query, but is kept in a separate directory so the reproduction steps stay explicit.

The 14-query SemBench E-Commerce evaluation is a public external benchmark; if you reproduce that part, download the benchmark data from the SemBench release and run those queries with the same driver.

If you add more synthetic queries, place them in [test/semantic/demo_queries](test/semantic/demo_queries) and reuse the same commands.

## Scope

- This open-source repo documents reproduction for our method (Morrila) only.
- Private DB-agent query assets are not included.
- Public benchmark queries should be either checked in here or downloaded from their original release, depending on the benchmark license.
- Reproducing other semantic systems is outside this README.

## Troubleshooting

- Missing/invalid API key causes semantic calls to fail with HTTP 401.
- LLM logs are written to [llm_calls_gpt-5-mini.log](llm_calls_gpt-5-mini.log).
- The log records total LLM calls and token usage. It does not print a separate cost field, so derive cost from the token totals and the provider's pricing or billing dashboard.
- If optimizer behavior seems unchanged, check SEMANTIC_OPTIMIZER and DUCKDB_SEMANTIC_MODE.

## Modified Files (Artifact Map)

This repo is a DuckDB fork. Morrila-specific code is concentrated in a **small set of new files** plus a few hook points in the existing planner/optimizer pipeline. To see the full delta from upstream DuckDB, start from commit `f1be2944fd` (`Restore semantic source changes from backup branch`) and later Morrila commits (`git log --oneline -- src/optimizer/semantic.cpp src/common/llm_query.cpp`).

### Core (read these first)

| Path | Role |
|------|------|
| [`src/optimizer/semantic.cpp`](src/optimizer/semantic.cpp) | **Semantic optimizer pass**: SJ→SF rewrites, SP pull-up, filter splitting, **`RunDPPlacement()`** (DP cost model + traceback), pull-up mode. Entry: `SemanticOptimizer::Rewrite()`. |
| [`src/include/duckdb/optimizer/semantic.hpp`](src/include/duckdb/optimizer/semantic.hpp) | Optimizer API / mode enum (`none`, `pullup`, `costmodel`). |
| [`src/common/llm_query.cpp`](src/common/llm_query.cpp) | **LLM runtime + function cache**: `LLMCallManager`, batched OpenAI calls, **`batch_check_semantic*()`**, per-predicate result cache keyed by `(prompt, tuple)`. |
| [`src/include/duckdb/common/llm_query.hpp`](src/include/duckdb/common/llm_query.hpp) | LLM/cache headers and cache API. |
| [`src/function/scalar/semantic_functions.cpp`](src/function/scalar/semantic_functions.cpp) | SQL UDFs `semantic()`, `semantic_int()`, `semantic_string()`, `semantic_double()`; invoke cache + LLM layer. |

### Optimizer hook (1 call site)

| Path | Role |
|------|------|
| [`src/optimizer/optimizer.cpp`](src/optimizer/optimizer.cpp) | Invokes `SemanticOptimizer` when `SEMANTIC_OPTIMIZER=true` / `DUCKDB_SEMANTIC_MODE` is set (after join-filter pushdown). |

### Planner / binder glue (small edits)

| Path | Role |
|------|------|
| [`src/planner/semantic_placeholder.cpp`](src/planner/semantic_placeholder.cpp) | `{column}` placeholders in semantic prompts. |
| [`src/planner/operator/logical_semantic_join.cpp`](src/planner/operator/logical_semantic_join.cpp) | Logical `SEMANTIC JOIN` operator (rewritten away by optimizer). |
| [`src/planner/binder/tableref/bind_joinref.cpp`](src/planner/binder/tableref/bind_joinref.cpp) | Parses `SEMANTIC JOIN` in SQL. |
| [`src/planner/expression_binder/where_binder.cpp`](src/planner/expression_binder/where_binder.cpp), [`select_binder.cpp`](src/planner/expression_binder/select_binder.cpp) | Bind `semantic(...)` in WHERE / SELECT. |
| [`src/function/function.cpp`](src/function/function.cpp) | Registers semantic scalar functions. |

### Build integration

| Path | Role |
|------|------|
| [`src/optimizer/CMakeLists.txt`](src/optimizer/CMakeLists.txt), [`src/common/CMakeLists.txt`](src/common/CMakeLists.txt), [`src/function/scalar/CMakeLists.txt`](src/function/scalar/CMakeLists.txt), [`src/planner/CMakeLists.txt`](src/planner/CMakeLists.txt) | Add the new `.cpp` files to the DuckDB build. |

### Benchmarks & logs (not engine code)

| Path | Role |
|------|------|
| [`test/semantic/agent_bench/`](test/semantic/agent_bench/) | 30 hybrid benchmark SQL files (paper eval). |
| [`test/semantic/demo_queries/`](test/semantic/demo_queries/) | Minimal demo queries. |
| [`llm_calls_gpt-5-mini.log`](llm_calls_gpt-5-mini.log) | Per-run LLM call / token log (generated at runtime). |

**Where is predicate placement implemented?**  
`SemanticOptimizer::RunDPPlacement()` in [`src/optimizer/semantic.cpp`](src/optimizer/semantic.cpp) (DP over filter positions on a fixed join tree); realized by `FindSemanticFilter()` + `SwapFilterWithParent()`. Pull-up-only mode skips DP and always pulls semantic filters up. Environment knob: `DUCKDB_SEMANTIC_MODE=costmodel|pullup|none`.

## License

MIT.
