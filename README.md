# ComputeQL - A GPU-Accelerated SQL Database

ComputeQL is a from-scratch, columnar SQL database written in C99 that executes queries as **GPU compute kernels** instead of on the CPU. Rather than compiling a bespoke shader per query, it interprets a compact **bytecode VM** inside a small library of precompiled Vulkan compute shaders - so arbitrary WHERE-clause expressions can be evaluated on the GPU without any runtime shader compilation.

> **Status:** ComputeQL is the successor of a previous project GDB. The storage engine, SQL parser, query planner, and multi-operator GPU execution (joins, sorts, aggregates) are all functional end-to-end. Current work is focused on constraints/indexing and closing the performance gap with CPU databases on small/selective queries. See [Roadmap](#roadmap) below for exactly what's implemented today versus planned.

## How it works

1. **Storage** - Data lives in a hand-rolled columnar format (`GDB_Database` -> `GDB_Table` -> `GDB_Column`), one file per column, memory-mapped on load. Small columns are loaded fully into memory; columns past a size threshold are streamed/disk-backed automatically.
2. **Parsing** - A hand-rolled recursive-descent SQL tokenizer/parser turns query text into an AST, with position-aware syntax error reporting (line, column, and a caret pointing at the offending token).
3. **IR lowering** - The AST is lowered into an intermediate representation (IR) tree that the execution layer walks directly.
4. **Planning** - The IR for a `SELECT` is lowered into a plan tree (`Limit(Sort(Project(Having(Aggregate(Filter(Join-tree))))))`) that composes fixed physical operators - there's no cost-based optimizer yet (no join reordering or predicate pushdown), so today there's exactly one plan per query.
5. **GPU execution** - Each physical operator (scan+filter, hash join, bitonic sort, group-by/aggregate) is backed by a Vulkan compute shader. WHERE/ON/HAVING-style expressions are compiled to a small stack-based bytecode ISA and interpreted per-row/per-group on the GPU; the CPU uploads column data and the bytecode program to fixed descriptor bindings, dispatches, and reads back results. Tables larger than the configured GPU buffer size are automatically chunked across multiple dispatches. An equality/range predicate on an indexed column can instead be served by a CPU-side binary search over that column, skipping the GPU dispatch entirely.

## Current capabilities

**Storage engine**
- Columnar on-disk format with binary metadata + one data file per column
- Supported column types: `u32`, `u64`, `f32`, `f64`, `string8`
- Automatic in-memory vs. disk-backed storage depending on column size
- CSV import (streaming, for datasets larger than memory) and CSV export
- Per-column NULL tracking
- Single-column constraints: `NOT NULL`, `UNIQUE`, `PRIMARY KEY`, `FOREIGN KEY ... REFERENCES table(column)`, `CHECK(...)` - enforced on `INSERT`
- `CREATE INDEX ... ON table(column)` / `DROP INDEX` - index metadata persists with the table; the sorted scan order itself is built on demand at query time
- A reserved `column_catalog` system table exposing per-column metadata (type, constraints, index membership), queryable like any other table, plus `DESCRIBE <table>`

**SQL support**
- `USE`, `CREATE DATABASE`, `CREATE TABLE` (with column constraints), `CREATE INDEX`, `ALTER TABLE` (add/drop column, rename), `DROP INDEX`
- `INSERT INTO ... (columns) VALUES (...), (...), ...` (multi-row), constraint-checked against `NOT NULL`/`UNIQUE`/`PRIMARY KEY`/`FOREIGN KEY`/`CHECK`
- `IMPORT INTO <table> FROM '<csv path>'`
- `SELECT` with:
  - Multi-table `FROM t1, t2` and explicit `INNER JOIN` / `LEFT [OUTER] JOIN ... ON` (equi-joins, numeric or string keys)
  - Qualified column references (`table.column`) and `AS` aliases on tables and columns
  - `WHERE`/`ON`/`HAVING` with full operator precedence: `AND`/`OR` (correct binding, parenthesized sub-expressions), comparisons (`=`, `==`, `!=`, `<`, `>`, `<=`, `>=`), `IS [NOT] NULL`, and string `contains`/`equals`
  - `GROUP BY`, `HAVING`, and aggregate calls (`COUNT(*)`, `SUM`, `AVG`, `MIN`, `MAX`) in both the SELECT list and HAVING expressions
  - `ORDER BY` (`ASC`/`DESC`, up to 4 numeric columns), `LIMIT`, `OFFSET`
  - `DESCRIBE <table>`
- `EXPLAIN <select>` - prints the query planner's plan tree without executing the query
- Real syntax error messages with source position (line/column + caret)

**GPU execution**
- Vulkan compute backend with double-precision (`shaderFloat64`) comparisons for exact numeric semantics
- A reusable bytecode VM (comparisons, boolean logic, string equality/`contains`) shared across operators, interpreted by dedicated compute shaders for scan+filter, hash join (build/probe), bitonic sort, and group-by/aggregate (hash + CSR scatter + per-group reduction)
- Cached shader modules/pipelines/descriptor sets and a pooled GPU buffer allocator, so steady-state queries don't pay pipeline/allocation setup cost repeatedly
- Bulk column materialization for `SELECT` output (one gather pass per column instead of a syscall per row/column)
- Automatic chunking for tables that exceed the GPU's max buffer size

**Platform**
- Windows x64 (MSVC or Clang), Vulkan 1.x required. Linux/macOS are not currently supported.

## Performance

A benchmark harness (`src/tests/`, run via `build_and_run_tests.bat`) compares ComputeQL against SQLite on small (50-row) and large (100k-row) tables, reporting prepare/execute/total timings and cross-checking row counts and a checksum between engines. Bulk column materialization, GPU resource pooling/caching, and batched GPU submission across every operator (scan+filter, join, sort, group-by/aggregate) keep per-query overhead low. The remaining performance focus is continuing to close the gap with in-memory CPU engines like SQLite on small, highly selective queries.

## Roadmap

Ordered from "next" to "later":
- [ ] **Chunked cross-bucket hashing** - support `GROUP BY`/hash-join tables larger than the GPU's max buffer size.
- [ ] **Persistent/sorted index storage** - maintain index sort order on disk instead of rebuilding it on demand each query.
- [ ] **Cross-engine benchmarking against more engines** - extend the current SQLite-only harness to DuckDB/ClickHouse/Postgres and publish results.
- [ ] **Client-server mode** - a long-running server process that keeps databases and the GPU context loaded and accepts queries over a network connection, instead of today's one-shot CLI invocation. A natural fit is speaking the Postgres wire protocol, so existing clients, ORMs, and BI tools work against it out of the box.
- [ ] **Users & access control** - accounts and authentication for network connections, plus a role/permission model (`GRANT`/`REVOKE`) scoped to databases and tables, so a shared server isn't all-or-nothing access.
- [ ] **Multi-GPU support**
- [ ] **Additional GPU backends** (e.g. CUDA) alongside Vulkan
- [ ] **Cross-platform support** (Linux/macOS)
- [ ] **Additional query languages** on top of the same execution engine
- [ ] **GPU projection kernel** - move column projection for `SELECT` off the CPU and into the GPU operator pipeline.

## Building from source

*Currently, only Windows x64 is supported.*

### Prerequisites

- **MSVC Build Tools** (Visual Studio 2017 "Microsoft C/C++ Build Tools" or later), for the Windows SDK plus the `cl`/`link` toolchain. Building with Clang is possible if the Windows SDK is present but is untested. Compiler must be on the PATH variable.
- **Vulkan SDK**, with `%VULKAN_SDK%\Bin\glslc.exe` available - `build.bat` uses `glslc` to precompile the GPU compute shaders (`.comp` -> SPIR-V `.spv`) at build time. Get it from [vulkan.lunarg.com](https://vulkan.lunarg.com/).

### Build

From the repository root:

```
build.bat
```

On success, `build\gdb.exe` is produced, along with compiled shaders under `build\shaders\`.

### Run a query

```
build\gdb.exe --query="CREATE DATABASE shop; USE shop; CREATE TABLE products (id u32 PRIMARY KEY, price f64 NOT NULL, name string8); CREATE INDEX idx_price ON products (price); INSERT INTO products (id, price, name) VALUES (1, 9.99, 'widget'), (2, 19.99, 'gadget');"
build\gdb.exe --query="USE shop; SELECT id, name, price FROM products WHERE price > 5.00;"
```

Each invocation of `gdb.exe` runs the queries passed via `--query`, loading and saving the affected database(s) under `gdb_data/` in the working directory.

## Project layout

```
src/
  main.c              - unity build entry point (includes every .c in dependency order)
  application.c/h     - top-level query execution loop, dispatches parsed statements
  gdb/                - columnar storage engine (databases, tables, columns, disk I/O, CSV, constraints, indexes)
  ir_gen/
    sql_parser.c/h    - SQL tokenizer + recursive-descent parser -> AST
    ir_gen.c/h        - AST -> IR lowering
  planner/            - IR -> physical plan tree (join/filter/aggregate/having/project/sort/limit)
  query_exec/         - bytecode compiler (IR -> GPU bytecode) + per-operator GPU dispatch orchestration
  gpu/
    vulkan/           - Vulkan backend + compute shaders (GLSL, compiled to SPIR-V at build time)
  os/                 - OS abstraction layer (Windows only today)
  tests/              - benchmark harness comparing against SQLite (build_and_run_tests.bat)
```