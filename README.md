# Horrila

Horrila is a research prototype for cost-based placement of semantic operators in hybrid query plans, built on top of DuckDB.

Upstream base: https://github.com/duckdb/duckdb

## Build

```bash
cd Horrila
make
```

## Runtime configuration

Set your API key if semantic operators require an external LLM backend:

```bash
export API_KEY='your-api-key'
```

Semantic optimizer modes:

- `costmodel` (default)
- `pullup`
- `none`

```bash
export SEMANTIC_OPTIMIZER=true
export DUCKDB_SEMANTIC_MODE=costmodel
```

Optional debug output:

```bash
export DUCKDB_SEMANTIC_DEBUG=1
```

## Basic sanity run

```bash
./build/release/duckdb -unsigned
```

Inside the CLI:

```sql
SELECT 42;
```

## Public demo query

This repository keeps one public semantic demo query for open-source users:

- `examples/semantic/demo_query.sql`

Run it with:

```bash
./build/release/duckdb -unsigned < examples/semantic/demo_query.sql
```

## Notes

- By default, runtime LLM interaction logs may be written to local log files (for example `llm_calls.log`).
- Do not commit credentials, private prompts, or private benchmark workloads.
