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

## License

MIT.
