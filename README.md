Horrila: Cost-Based Placement of Semantic Operators in Hybrid Query Plans

This repository is a research fork of DuckDB focused on Horrila.

Source base: https://github.com/duckdb/duckdb

## Scope of this open-source release

- This repo supports reproducing our method (Horrila optimizer) and running semantic SQL queries in this codebase.
- We only document how to run our queries and our optimizer modes.
- We do not include private DB-agent query assets in this repository.
- Reproducing other external systems is out of scope for this README.

## Build

From repository root:

	cmake .
	make

## Environment setup

Set OpenAI key used by semantic operators:

	export API_KEY="your-openai-api-key"

Enable semantic optimizer (enabled by default, but set explicitly for clarity):

	export SEMANTIC_OPTIMIZER=true

Optional debug output (prints semantic plan rewrite info):

	export DUCKDB_SEMANTIC_DEBUG=1

Optional concurrency for LLM workers (default is 100):

	export NUM_LLM_WORKERS=32

## Optimizer modes

Horrila exposes three semantic optimization modes:

- costmodel: DP-based cost model (default)
- pullup: greedy semantic filter pull-up
- none: disable semantic optimization

Switch mode with:

	export DUCKDB_SEMANTIC_MODE=costmodel
	export DUCKDB_SEMANTIC_MODE=pullup
	export DUCKDB_SEMANTIC_MODE=none

## Run our queries

### 1) Single query demo

Current public demo query:

- [test/semantic/demo_queries/semantic_filter_1.sql](test/semantic/demo_queries/semantic_filter_1.sql)

Run it:

	./duckdb -unsigned < test/semantic/demo_queries/semantic_filter_1.sql

If your environment still uses the older output path, you can also use:

	./build/release/duckdb -unsigned < test/semantic/demo_queries/semantic_filter_1.sql

### 2) Batch run all query files in demo folder

	for f in test/semantic/demo_queries/*.sql; do
	  echo "Running ${f}"
	  ./duckdb -unsigned < "${f}"
	done

## Add your own synthetic queries

To run additional queries for this release:

1. Put SQL files into [test/semantic/demo_queries](test/semantic/demo_queries).
2. Use SEMANTIC(...) predicates in SQL.
3. Run each file with the same command shown above.

Example pattern:

	SELECT *
	FROM your_table t
	WHERE SEMANTIC('Question about tuple {t.col1}, {t.col2} ...');

## Query inventory in this repo

Public semantic query files currently included:

- [test/semantic/demo_queries/semantic_filter_1.sql](test/semantic/demo_queries/semantic_filter_1.sql)

As more query files are added into [test/semantic/demo_queries](test/semantic/demo_queries), the same run commands apply.

## Logs and troubleshooting

- LLM call logs are written to files like llm_calls_gpt-5-mini.log in repository root.
- If API_KEY is missing or invalid, semantic calls can fail with HTTP 401.
- If you see no semantic optimization effect, check SEMANTIC_OPTIMIZER and DUCKDB_SEMANTIC_MODE.
