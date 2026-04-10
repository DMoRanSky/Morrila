#!/usr/bin/env python3
"""
Export DataAgentBench GitHub DBs into CSVs expected by the local semantic tests.

Writes these files into: test/semantic/agent_bench/dataset/github_repo/
  - files.csv
  - contents.csv
  - languages.csv
  - licenses.csv
  - repos.csv

The script reads:
  - DataAgentBench/query_GITHUB_REPOS/query_dataset/repo_artifacts.db  (DuckDB)
  - DataAgentBench/query_GITHUB_REPOS/query_dataset/repo_metadata.db   (SQLite)

It will back up any existing CSVs to *.bak before writing.

Usage: python tools/export_github_dataset.py
"""
import os
import sys
import csv
import shutil
import sqlite3

try:
    import duckdb
except Exception as e:
    print('duckdb python module required to read repo_artifacts.db:', e)
    sys.exit(1)


ROOT = os.path.abspath(os.path.dirname(os.path.dirname(__file__)))
# try a couple of known locations for the DataAgentBench dataset
candidate_db_dirs = [
    '/home/moransky/DataAgentBench/query_GITHUB_REPOS/query_dataset',
    os.path.join(ROOT, 'DataAgentBench', 'query_GITHUB_REPOS', 'query_dataset'),
]
DB_DIR = None
for c in candidate_db_dirs:
    if os.path.exists(c):
        DB_DIR = c
        break
if DB_DIR is None:
    # allow overriding via environment variable
    env = os.environ.get('DATAAGENTBENCH_QUERY_DB')
    if env and os.path.exists(env):
        DB_DIR = env
    else:
        DB_DIR = candidate_db_dirs[0]

DUCKDB_PATH = os.path.join(DB_DIR, 'repo_artifacts.db')
SQLITE_PATH = os.path.join(DB_DIR, 'repo_metadata.db')
OUT_DIR = os.path.join(ROOT, 'test', 'semantic', 'agent_bench', 'dataset', 'github_repo')
os.makedirs(OUT_DIR, exist_ok=True)


def backup(path):
    if os.path.exists(path):
        bak = path + '.bak'
        print(f'Backing up {path} -> {bak}')
        shutil.copy2(path, bak)


def export_duckdb_tables():
    if not os.path.exists(DUCKDB_PATH):
        print('DuckDB file not found:', DUCKDB_PATH)
        return {}
    con = duckdb.connect(DUCKDB_PATH)
    tables = [r[0] for r in con.execute("SHOW TABLES").fetchall()]
    print('DuckDB tables:', tables)
    exported = {}
    # export files and contents if present
    for want, outname in [('files', 'files.csv'), ('contents', 'contents.csv')]:
        tbl = None
        for t in tables:
            if t.lower() == want or want in t.lower():
                tbl = t
                break
        if not tbl:
            print('DuckDB: table for', want, 'not found')
            continue
        outp = os.path.join(OUT_DIR, outname)
        backup(outp)
        q = con.execute(f"SELECT * FROM {tbl}")
        cols = [d[0] for d in q.description]
        rows = q.fetchall()
        print(f'Exporting DuckDB table {tbl}: {len(rows)} rows -> {outp}')
        with open(outp, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(cols)
            for r in rows:
                w.writerow([ (x.decode('utf-8') if isinstance(x, (bytes, bytearray)) else x) for x in r ])
        exported[want] = outp
    con.close()
    return exported


def export_sqlite_tables():
    if not os.path.exists(SQLITE_PATH):
        print('SQLite file not found:', SQLITE_PATH)
        return {}
    conn = sqlite3.connect(SQLITE_PATH)
    cur = conn.cursor()
    cur.execute("SELECT name FROM sqlite_master WHERE type='table'")
    tables = [r[0] for r in cur.fetchall()]
    print('SQLite tables:', tables)
    exported = {}
    # heuristics: look for tables providing repos, languages, licenses
    # try several likely names
    candidates = {
        'repos': ['repos', 'repositories', 'repo'],
        'languages': ['languages', 'repo_languages', 'language'],
        'licenses': ['licenses', 'repo_licenses', 'license']
    }
    for want, names in candidates.items():
        tbl = None
        for n in names:
            for t in tables:
                if t.lower() == n or n in t.lower():
                    tbl = t
                    break
            if tbl:
                break
        if not tbl:
            print('SQLite: table for', want, 'not found')
            continue
        outp = os.path.join(OUT_DIR, f'{want}.csv')
        backup(outp)
        cur.execute(f"SELECT * FROM {tbl}")
        cols = [d[0] for d in cur.description]
        rows = cur.fetchall()
        print(f'Exporting SQLite table {tbl}: {len(rows)} rows -> {outp}')
        with open(outp, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(cols)
            for r in rows:
                w.writerow(r)
        exported[want] = outp
    conn.close()
    return exported


def main():
    print('Export target dir:', OUT_DIR)
    d1 = export_duckdb_tables()
    d2 = export_sqlite_tables()
    print('\nExport summary:')
    for k, v in {**d1, **d2}.items():
        print(' -', k, '->', v)


if __name__ == '__main__':
    main()
