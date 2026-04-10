# Horrila

Fork from https://github.com/duckdb/duckdb.

## Build

```bash
make
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
