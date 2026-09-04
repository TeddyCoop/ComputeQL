# ComputeQL - A GPU-Accelerated SQL Database

ComputeQL is a from-scratch, columnar SQL database written in C99 that executes queries as **GPU compute kernels** instead of on the CPU. Rather than compiling a bespoke shader per query, it interprets a compact **bytecode VM** inside a small library of precompiled Vulkan compute shaders - so arbitrary WHERE-clause expressions can be evaluated on the GPU without any runtime shader compilation.

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
- Single Column Index
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

TODO

## Roadmap

Ordered from "next" to "later":
- [ ] **Cross-engine benchmarking against more engines** - extend to ClickHouse/Postgres (client-server engines, needing a running server + client library) and publish results.
- [ ] **Users & access control** - accounts and authentication for network connections, plus a role/permission model (`GRANT`/`REVOKE`) scoped to databases and tables, so a shared server isn't all-or-nothing access.
- [ ] **Multi-GPU support**
- [ ] **Additional GPU backends** (e.g. HIP / ROCm, DirectX12 Compute, CUDA) alongside Vulkan
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
  server/             - client-server mode: long-running server, one thread + session per connection
    pg_protocol.c/h   - Postgres wire protocol (v3) message framing/encoding, OID mapping
    pg_server.c/h     - Postgres wire protocol server: startup handshake, simple query loop, extended query protocol (prepared statements/portals per connection)
  client/             - client-server mode: one-shot and interactive (--connect) network clients
  os/                 - OS abstraction layer (Windows only today), including os/net for TCP sockets
  tests/              - benchmark harness comparing against SQLite/DuckDB, plus client-server smoke tests (build_and_run_tests.bat)
```