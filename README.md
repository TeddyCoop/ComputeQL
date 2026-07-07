# ComputeQL - A GPU-Accelerated SQL Database

ComputeQL is a from-scratch, columnar SQL database written in C99 that executes queries as **GPU compute kernels** instead of on the CPU. Rather than compiling a bespoke shader per query, it interprets a compact **bytecode VM** inside a small library of precompiled Vulkan compute shaders — so arbitrary WHERE-clause expressions can be evaluated on the GPU without any runtime shader compilation.

> **Status:** ComputeQL is the successor of a previous project GDB. The storage engine, Vulkan execution backend, and SQL parser are functional; query planning and multi-operator GPU execution (joins, sorts, aggregates) are in progress. See [Roadmap](#roadmap) below for exactly what's implemented today versus planned.

## How it works

1. **Storage** — Data lives in a hand-rolled columnar format (`GDB_Database` → `GDB_Table` → `GDB_Column`), one file per column, memory-mapped on load. Small columns are loaded fully into memory; columns past a size threshold are streamed/disk-backed automatically.
2. **Parsing** — A hand-rolled recursive-descent SQL tokenizer/parser turns query text into an AST, with position-aware syntax error reporting (line, column, and a caret pointing at the offending token).
3. **IR lowering** — The AST is lowered into an intermediate representation (IR) tree that the execution layer walks directly.
4. **GPU execution** — WHERE-clause expressions are compiled to a small stack-based bytecode ISA. The CPU uploads column data and the bytecode program to fixed GPU descriptor bindings, dispatches a Vulkan compute shader that interprets the bytecode per-row, and reads back matching row indices. Tables larger than the configured GPU buffer size are automatically chunked across multiple dispatches.

## Current capabilities

**Storage engine**
- Columnar on-disk format with binary metadata + one data file per column
- Supported column types: `u32`, `u64`, `f32`, `f64`, `string8`
- Automatic in-memory vs. disk-backed storage depending on column size
- CSV import (streaming, for datasets larger than memory) and CSV export

**SQL support**
- `USE`, `CREATE DATABASE`, `CREATE TABLE`, `ALTER TABLE` (add/drop column, rename), `DROP`
- `INSERT INTO ... (columns) VALUES (...), (...), ...` (multi-row)
- `IMPORT INTO <table> FROM '<csv path>'`
- `SELECT` with:
  - Multi-table `FROM t1, t2` and explicit `INNER JOIN` / `LEFT [OUTER] JOIN ... ON`
  - Qualified column references (`table.column`) and `AS` aliases on tables and columns
  - `WHERE` with full operator precedence: `AND`/`OR` (correct binding, parenthesized sub-expressions), comparisons (`=`, `==`, `!=`, `<`, `>`, `<=`, `>=`), and string `contains`/`equals`
  - `GROUP BY`, `HAVING`, and aggregate calls (`COUNT(*)`, `SUM(col)`, etc.) in both the SELECT list and HAVING expressions
  - `ORDER BY` (`ASC`/`DESC`), `LIMIT`, `OFFSET`
- Real syntax error messages with source position (line/column + caret)

**GPU execution**
- Vulkan compute backend with double-precision (`shaderFloat64`) comparisons for exact numeric semantics
- A reusable per-row bytecode VM (comparisons, boolean logic, string equality/`contains`) interpreted by a single scan+filter shader
- Automatic chunking for tables that exceed the GPU's max buffer size

**Platform**
- Windows x64 (MSVC or Clang), Vulkan 1.x required. Linux/macOS are not currently supported.

## Roadmap

Ordered from "next" to "later":

- [ ] **Query planner** - introduce a logical -> physical planning stage between IR generation and execution (predicate pushdown, column pruning, join ordering), replacing today's direct single-shot scan+filter dispatch.
- [ ] **GPU join kernel** - hash join for the inner/left equi-joins the parser already accepts.
- [ ] **GPU sort kernel** - for `ORDER BY` (a GPU bitonic sort is the natural fit).
- [ ] **GPU group-by/aggregate kernel** — `SUM`/`COUNT`/`AVG`/`MIN`/`MAX`, keyed by group.
- [ ] **GPU projection kernel** - move column projection off the CPU path and into the same operator pipeline as scan/filter/join/sort.
- [ ] **`INSERT`/`ALTER`/`DELETE` execution hardening** - `DELETE` currently parses but isn't executed yet, and `INSERT` without an explicit column list has a known crash (tracked internally); both need attention alongside the planner work.
- [ ] **Cross-engine benchmarking** - driver scripts to run head-to-head comparisons and publish results.
- [ ] **Multi-GPU support**
- [ ] **Additional GPU backends** (e.g. CUDA) alongside Vulkan
- [ ] **Cross-platform support** (Linux/macOS)
- [ ] **Additional query languages** on top of the same execution engine

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
build\gdb.exe --query="CREATE DATABASE shop; USE shop; CREATE TABLE products (id u32, price f64, name string8); INSERT INTO products (id, price, name) VALUES (1, 9.99, 'widget'), (2, 19.99, 'gadget');"
build\gdb.exe --query="USE shop; SELECT id, name, price FROM products WHERE price > 5.00;"
```

Each invocation of `gdb.exe` runs the queries passed via `--query`, loading and saving the affected database(s) under `gdb_data/` in the working directory.

## Project layout

```
src/
  main.c              - unity build entry point (includes every .c in dependency order)
  application.c/h     - top-level query execution loop, dispatches parsed statements
  gdb/                - columnar storage engine (databases, tables, columns, disk I/O, CSV)
  ir_gen/
    sql_parser.c/h    - SQL tokenizer + recursive-descent parser -> AST
    ir_gen.c/h        - AST -> IR lowering
  query_exec/         - bytecode compiler (IR -> GPU bytecode) + GPU dispatch orchestration
  gpu/
    vulkan/           - Vulkan backend + compute shaders (GLSL, compiled to SPIR-V at build time)
  os/                 - OS abstraction layer (Windows only today)
```