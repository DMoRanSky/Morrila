# Horrila

Fork from https://github.com/duckdb/duckdb.

## Build

```bash
cmake .
make
```



### Change the semantic optimizer mode

**Important:** First, ensure the semantic optimizer is enabled (it's enabled by default):

```bash
# Make sure SEMANTIC_OPTIMIZER is not set to false
unset SEMANTIC_OPTIMIZER
# Or explicitly enable it:
export SEMANTIC_OPTIMIZER=true
```

The semantic optimizer has three modes:

- `costmodel` (default): DP-based optimization with cost model
- `pullup`: Aggressive filter pullup strategy
- `none`: Disable semantic optimizer

To switch modes:

```bash
export DUCKDB_SEMANTIC_MODE=costmodel  # Use cost-based optimization
export DUCKDB_SEMANTIC_MODE=pullup     # Use filter pullup strategy
export DUCKDB_SEMANTIC_MODE=none       # Disable semantic optimizer
```

To see detailed query tree output (before and after semantic optimization):

```bash
export DUCKDB_SEMANTIC_DEBUG=1
```

## Single Demo

Run only this demo query:

```bash
export API_KEY="your-openai-api-key"
./build/release/duckdb -unsigned < test/semantic/demo_queries/semantic_filter_1.sql
```

## Notes

- If API key is missing or invalid, semantic calls will fail with 401.
- LLM call logs are written to local log files, e.g. `llm_calls_gpt-5-mini.log`.
