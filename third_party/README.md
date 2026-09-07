# third_party/ — the optional OSS backbone

This directory is **empty in the shipped tree, on purpose.**

De-Sentry builds and runs completely with nothing here. Every workload is
served by the five from-scratch router backends (`kv`, `columnar_lite`,
`ts_rollup`, `vector_hnsw_lite`, `graph_adj`), which depend on nothing beyond
OpenSSL and the C++17 standard library. That is the shipping configuration and
it is what makes the product's central promise — *fully functional with no
internet, nothing fetched at runtime* — true rather than aspirational.

The libraries below are **opt-in upgrades**. Each buys a specific capability
the from-scratch family does not have. Turning one on means vendoring its
source here yourself and rebuilding; the CMake build never downloads anything,
and it fails loudly (rather than silently skipping) if you enable an option
whose sources are missing.

## What each one buys, and when it is worth it

| Engine | Licence | What it adds over the from-scratch equivalent | Worth it when |
|---|---|---|---|
| **SQLite** | Public domain | Real SQL, JSON1 path queries, FTS5 full-text search, and the relational/metadata core the cross-engine index can live in | You want to query documents by content, not just by key |
| **sqlite-vec** | Apache-2.0 / MIT | *Exact* nearest-neighbour search that joins against the same database's relational tables in one query | Collections up to ~10⁵ vectors where exactness and SQL-joinability beat sub-linear search |
| **DuckDB** | MIT | A vectorised analytical engine: `GROUP BY`, window functions, cross-collection joins, Parquet/CSV export | You are doing analytics, not just storage — `columnar_lite` already covers compact columnar storage |
| **LMDB** | OpenLDAP Public Licence | Memory-mapped copy-on-write B+Tree with single-writer/many-reader MVCC, so a long scan never blocks a write and a crash can never tear a page | The transit store is hot: written by replication and read by returning owners concurrently |

## What is deliberately excluded, and why

These are not oversights — each was considered and rejected for a stated
reason:

- **Xapian** (full-text search) — GPL. Linking it would make the whole
  distributed binary GPL, which is incompatible with shipping this as a
  desktop application. FTS5 covers the same need under a public-domain
  licence.
- **TDengine** (time-series) — AGPL, with the same problem, more sharply:
  the AGPL's network clause is triggered by exactly what this product does.
  `ts_rollup` exists specifically because of this.
- **ClickHouse, QuestDB, InfluxDB** — all servers. A separate server process
  breaks the one-app, no-daemon install this product is built around, and
  would mean the user has to keep a database running to use their database.

Every included library is permissively licensed, statically linkable, and
embeddable in-process. That is not a coincidence; it is the selection
criterion.

## Vendoring instructions

All of these are single-file or few-file amalgamations, chosen for exactly
that reason: no submodules, no build system to integrate, no transitive
dependency tree.

### SQLite → `third_party/sqlite/`

Download the *amalgamation* (not the source tree) from
<https://www.sqlite.org/download.html>, then:

```
third_party/sqlite/
  sqlite3.c
  sqlite3.h
  sqlite3ext.h     # required only if you also vendor sqlite-vec
```

Build with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDESENTRY_WITH_SQLITE=ON
```

The build defines `SQLITE_ENABLE_JSON1`, `SQLITE_ENABLE_FTS5`,
`SQLITE_THREADSAFE=1` and `SQLITE_DQS=0` for you. `SQLITE_DQS=0` disables
MySQL-style double-quoted string literals, which are a footgun rather than a
feature.

### sqlite-vec → `third_party/sqlite-vec/`

From <https://github.com/asg017/sqlite-vec> releases, take the amalgamation:

```
third_party/sqlite-vec/
  sqlite-vec.c
  sqlite-vec.h
```

Requires SQLite to be vendored too:

```bash
cmake -B build -DDESENTRY_WITH_SQLITE=ON -DDESENTRY_WITH_SQLITE_VEC=ON
```

It is registered as a **static auto-extension** before the connection opens,
so no `.so`/`.dll` is ever loaded at runtime — which is what keeps the
offline guarantee true for this backend too.

### DuckDB → `third_party/duckdb/`

From <https://duckdb.org/docs/installation/> take the *amalgamation* build
(`duckdb.cpp` / `duckdb.hpp` / `duckdb.h`):

```
third_party/duckdb/
  duckdb.cpp
  duckdb.hpp
  duckdb.h
```

```bash
cmake -B build -DDESENTRY_WITH_DUCKDB=ON
```

Expect a long first compile — the amalgamation is tens of MB of C++. It is
built as its own CMake target so an incremental engine change does not
rebuild it.

### LMDB → `third_party/lmdb/`

From <https://www.symas.com/lmdb> (or the `openldap` source tree's
`libraries/liblmdb`):

```
third_party/lmdb/
  mdb.c
  midl.c
  midl.h
  lmdb.h
```

```bash
cmake -B build -DDESENTRY_WITH_LMDB=ON
```

## Reference links

- SQLite — <https://www.sqlite.org/>
- SQLite FTS5 — <https://sqlite.org/fts5.html>
- SQLite JSON1 — <https://sqlite.org/json1.html>
- DuckDB — <https://duckdb.org/>
- LMDB — <https://www.symas.com/lmdb>
- sqlite-vec — <https://github.com/asg017/sqlite-vec>

## Checking what a built binary actually has

Do not guess from the CMake flags you *think* you used — ask the binary:

```bash
curl -s http://127.0.0.1:7701/_engines | python3 -m json.tool
```

Each engine reports `compiled_in`, `active_on_this_node`, and — when it is
not compiled in — the exact `enable_with` CMake option that would add it.
