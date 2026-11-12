# ComputeQL - a GPU-accelerated SQL database

ComputeQL is a from-scratch, columnar SQL database written in C99 that runs queries on the **GPU** through [Vulkan](https://en.wikipedia.org/wiki/Vulkan_(API)) compute shaders. You write ordinary SQL (`SELECT ... JOIN ... WHERE ... GROUP BY ...`); the engine parses it, plans and optimizes it, generates a small compute kernel for the query's filter, and runs scans, joins, aggregates and sorts across thousands of GPU threads. It works on any Vulkan 1.x GPU (NVIDIA, AMD, Intel), not just CUDA hardware, and in server mode it speaks the PostgreSQL wire protocol, so `psql` and standard Postgres drivers can connect to it.

The goal is large data: scans, joins and aggregates over tens of millions to billions of rows, where GPU memory bandwidth beats a CPU. It is an early, single-developer project (see [Limitations](#limitations)).

## Performance

Measured on a Ryzen 5 5600X (6 cores / 12 threads), 32 GB RAM, **AMD Radeon RX 6700 XT** (12 GB), Windows 11. All numbers are medians of the harness's timed runs, end to end (prepare + execute + fetch), against DuckDB (C API, all threads) on the same machine, with result checksums compared between engines.

"Warm" means the touched columns are already resident in VRAM, which is the steady state for repeated queries on a table that fits. A cold first query pays the host-to-GPU copy once: at 1B rows, 440–700 ms for 1.9–3.8 GB of columns.

### 1 billion rows (4 × u32 columns, 14.9 GB)

| query | ComputeQL | DuckDB | speedup |
|---|---:|---:|---:|
| `SUM`, `MIN`, `MAX` | 8.5 ms | 467 ms | 55× |
| filter + count (2% selective) | 6.3 ms | 247 ms | 39× |
| `GROUP BY` region (50 groups) | 20 ms | 628 ms | 31× |
| join 50-row table, filtered | 19 ms | 609 ms | 32× |
| find one row by key | 17 ms | 282 ms | 16× |
| `GROUP BY` amount (10k groups) | 120 ms | 1,697 ms | 14× |
| join 1M-row table | 129 ms | 1,593 ms | 12× |

### 100 million rows

| query | ComputeQL | DuckDB | speedup |
|---|---:|---:|---:|
| `SUM`, `MIN`, `MAX` | 1.3 ms | 41 ms | 32× |
| `GROUP BY` region (50 groups) | 3.4 ms | 61 ms | 18× |
| `GROUP BY` amount (10k groups) | 14.5 ms | 179 ms | 12× |
| join 1M-row table | 28 ms | 175 ms | 6× |

### 10 million rows, 30-column event table (37 queries)

ComputeQL was faster on 36 of 37 queries. A selection:

| query | ComputeQL | DuckDB | speedup |
|---|---:|---:|---:|
| group by dict string (200 groups) | 0.43 ms | 39.8 ms | 93× |
| group by status (5 groups), f64 measures | 0.79 ms | 62.2 ms | 79× |
| filter on dictionary string equality | 0.31 ms | 8.2 ms | 27× |
| group by region (50), 5 aggregates | 1.0 ms | 21.6 ms | 22× |
| three-predicate filter | 1.1 ms | 13.0 ms | 12× |
| join users (1M), group by segment | 10.5 ms | 69.6 ms | 6.6× |
| group by user (1M groups) | 56 ms | 219 ms | 3.9× |
| sort 200k rows | 9.5 ms | 17.9 ms | 1.9× |

Where DuckDB still wins or ties: `COUNT(DISTINCT)` over 4M sessions (0.8×), window `top 3 per region` (1.1×, windows run on the CPU), and materializing 30 columns of a 1% filter (1.6×).

### Per-query kernels

The `WHERE` clause is compiled to GLSL for exactly that query shape (shaderc loaded at run time), instead of being interpreted as bytecode. Constants stay in buffers, so queries that differ only in literals share a kernel. On a 3M-row table this is 1.4–7× faster than the interpreter across 29 filter shapes, and an expression that the interpreter cannot run at all (stack depth 12 `OR` chain) drops from 749 ms to 0.9 ms.

To reproduce, see [Tests and benchmarks](#tests-and-benchmarks)

## How a query runs

```
  SQL text
     |
     v
  +--------------------+
  | Parser             |  recursive descent, AST, positioned errors
  +--------------------+
     |
     v
  +--------------------+
  | IR lowering        |
  +--------------------+
     |
     v
  +--------------------+
  | Planner            |  plan tree
  +--------------------+
     |
     v
  +--------------------+
  | Optimizer          |  column stats + calibrated cost model
  +--------------------+
     |
     v
  +--------------------+      +------------------------------+
  | Query executor     |<-----| Columnar storage             |
  | bytecode / GLSL    |      | zonemaps, dictionaries,      |
  +--------------------+      | indexes, disk streaming      |
     |                        +------------------------------+
     v
  +--------------------+
  | Vulkan backend     |  compute shaders on the GPU
  +--------------------+
     |
     v
  Result rows --> CLI / REPL, or Postgres wire server
```

1. **Storage** (`src/cql`): one file per column, memory-mapped, with per-8192-row zonemaps (min/max, used to skip chunks), dictionary encoding for low-cardinality strings, NULL tracking, constraints and single-column indexes. Columns larger than a threshold stream from disk, and tables larger than GPU memory are processed in chunks.
2. **Parse and lower** (`src/ir_gen`): a hand written parser produces an AST with line/column/caret syntax errors, then lowers it to an IR.
3. **Plan and optimize** (`src/planner`, `src/optimizer`): the IR becomes a `Limit(Sort(Project(Having(Aggregate(Filter(Join)))))))` tree. A cost-based pass (per-column statistics, a cost model calibrated on the local machine) pushes predicates down, inlines CTEs and derived tables, reorders inner joins, picks CPU/GPU/index strategies per operator and sizes buffers. Every rule can be switched off in `settings.cfg`.
4. **Execute** (`src/query_exec`, `src/gpu`): filters are turned into per-query GLSL kernels (or a stack-bytecode interpreter as a fallback), then run as Vulkan compute shaders for scan+filter, hash join, bitonic sort, and hash group-by/aggregate (including HyperLogLog and t-digest sketches for `APPROX_COUNT_DISTINCT` and `APPROX_PERCENTILE`). Fuzzy matching (`SIMILARITY`, `EDIT_DISTANCE`) runs on the GPU as well.

## SQL and features

- DDL/DML: `CREATE DATABASE/TABLE/INDEX`, `ALTER TABLE`, `DROP INDEX`, multi-row `INSERT`, `IMPORT INTO ... FROM 'file.csv'` (streaming), CSV export, `DESCRIBE`.
- Types: `u32`, `u64`, `i32`, `f32`, `f64`, `string8`, `enum`. Constraints: `NOT NULL`, `UNIQUE`, `PRIMARY KEY`, `FOREIGN KEY`, `CHECK`.
- `SELECT`: inner/left joins (GPU hash join, numeric or string keys), `WHERE`/`HAVING` with full boolean precedence, `GROUP BY`, aggregates (`COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `APPROX_COUNT_DISTINCT`, `APPROX_PERCENTILE`), `ORDER BY`, `LIMIT`/`OFFSET`, CTEs, subqueries, window functions (CPU).
- `EXPLAIN` and `EXPLAIN ANALYZE` (zonemap pruning, dictionary hits, GPU cache hits, per-stage timing, estimated vs actual rows).
- One-shot CLI, interactive REPL, or a server (`--serve` for the native protocol, `--serve-pg` for the PostgreSQL protocol) with matching network clients.

## Project structure

```
compute_ql/
|-- build.bat                 build the engine (MSVC or Clang)
|-- run.bat / test.bat        launch cql.exe / run a sample query
|-- build_and_run_tests.bat   build and run every bench_*.c and net_*.c test
|-- run_all_reports.bat       run every suite that writes a report
|-- run_scale_bench.bat       10M-row suite vs SQLite and DuckDB
|-- run_select_scale.bat      10M / 100M / 1B row select suite
|-- settings.cfg              runtime settings (copied next to cql.exe)
|-- docs/                     design notes, perf handoffs, paper runs
|-- src/
|   |-- main.c                unityentry point (includes every .c)
|   |-- application.c/h       top-level statement dispatch
|   |-- base/                 arenas, strings, threads, logging, command line
|   |-- os/                   OS layer (Win32 core, networking)
|   |-- thread_pool/          worker pool
|   |-- settings/             settings.cfg loader
|   |-- cql/                  columnar storage engine and statistics
|   |-- ir_gen/               SQL tokenizer, parser, AST -> IR
|   |-- planner/              IR -> plan tree
|   |-- optimizer/            cost model, join ordering, rewrites
|   |-- query_exec/           bytecode + GLSL kernel generation, GPU orchestration
|   |-- gpu/
|   |   |-- gpu.h             backend-neutral GPU interface
|   |   |-- vulkan/           Vulkan backend and shaders/*.comp
|   |-- server/               native and PostgreSQL-protocol servers
|   |-- client/               one-shot and interactive network clients
|   |-- tests/                correctness + benchmark suites (SQLite/DuckDB), scale/ for 1B rows
|   |-- third_party/          Vulkan headers/loader lib, shaderc/glslang headers, SQLite, DuckDB, stb
|-- build/                    output (cql.exe, shaders/, shaderc_shared.dll, bench_reports/)
```

## Building

Windows x64 only today.

### Prerequisites

- **A C compiler**: MSVC (Visual Studio 2017+ or Build Tools, with the Windows SDK) **or** Clang/LLVM 14+ with `lld` (the LLVM installer, or Visual Studio's "C++ Clang tools"). The compiler must be on `PATH`; with MSVC, build from an "x64 Native Tools Command Prompt".
- **[Vulkan SDK](https://vulkan.lunarg.com/)** (`VULKAN_SDK` set). The build copies `shaderc_shared.dll` from it next to `cql.exe`; kernels are compiled from GLSL with shaderc at run time. The Vulkan headers and `vulkan-1.lib` are vendored in `src/third_party/vulkan`.
- A GPU with a Vulkan driver supporting `shaderFloat64`.

### Build

From the repository root, in a prompt where the compiler is available:

```bat
build.bat            :: MSVC, release (default)
build.bat clang      :: Clang + lld, release
build.bat debug      :: MSVC, debug
build.bat clang debug
```

This produces `build\cql.exe`, `build\settings.cfg`, `build\shaders\*.comp` and `build\shaderc_shared.dll`. Both compilers need the Windows SDK, so use a Visual Studio developer prompt for either.

## Running

Run from `build\` (shaders and settings are found relative to the exe).

```
cd build

:: one-shot query
cql.exe --query="CREATE DATABASE shop; USE shop; CREATE TABLE products (id u32, price f64, name string8); INSERT INTO products (id, price, name) VALUES (1, 9.99, 'widget'), (2, 19.99, 'gadget'), (3, 4.50, 'gizmo');"
cql.exe --query="USE shop; SELECT id, name, price FROM products WHERE price > 5.00;"

:: interactive REPL (no --query)
cql.exe

:: load a CSV
cql.exe --query="USE shop; IMPORT INTO products FROM 'products.csv';"

:: see what the engine did
cql.exe --query="USE shop; EXPLAIN ANALYZE SELECT name FROM products WHERE price > 5.00;"

:: server, then a client
cql.exe --serve=5433
cql.exe --connect=127.0.0.1:5433 --query="USE shop; SELECT * FROM products;"

:: PostgreSQL protocol, usable from psql and Postgres drivers
cql.exe --serve-pg=5432
```

Other flags: `--gpu=vulkan`, `--settings=<path>`, `--log-level=error|warn|info|verbose`.
### Tests and benchmarks

The suites compare against SQLite and DuckDB, which are not bundled. Download the [SQLite amalgamation](https://sqlite.org/download.html) (`sqlite3.c`, `sqlite3.h`) into `src/third_party/sqlite/` and the [DuckDB C library](https://github.com/duckdb/duckdb/releases) (`libduckdb-windows-amd64.zip`: `duckdb.h`, `duckdb.lib`, `duckdb.dll`) into `src/third_party/duckdb/`. The test scripts build with MSVC by default. Run `set CQL_CLANG=1` first to build them (and `cql.exe`) with Clang instead. Clang builds keep a separate cached SQLite object (`sqlite3_clang.obj`).

```bat
build_and_run_tests.bat    :: every bench_*.c / net_*.c test -> build\bench_reports\*.md
run_scale_bench.bat        :: 10M-row suite (--no_duckdb, --no_sqlite, --only=<label>)
run_select_scale.bat       :: 10M / 100M / 1B rows (--no_duckdb, --rows=N)
run_all_reports.bat        :: everything above plus the transfer benchmark
```

The 1B-row suite needs about 15 GB of RAM for ComputeQL alone, and more if DuckDB loads its own copy (pass `--duckdb_max_rows=100000000` to skip it at 1B).

## Limitations

- **Small queries.** A GPU dispatch has fixed submit/wait cost; for small tables SQLite or DuckDB will be faster. Tuning targets 10M+ rows.
- **No durability story.** No transactions, WAL, crash recovery, concurrent writers, or authentication
- **SQL coverage.** No recursive CTEs or `UNION`/`INTERSECT`/`EXCEPT`; window functions run on the CPU; the optimizer reorders only the inner joins before the first `LEFT JOIN` and does not search whole plan alternatives.
- **Platform.** Windows only, and needs a working Vulkan driver.
- **Hardware coverage.** All numbers come from a single GPU (RX 6700 XT).

## Future work

- **More GPUs.** Run the full correctness and benchmark suites on NVIDIA and Intel hardware, integrated GPUs, and other AMD generations; calibrate the optimizer cost model per device and check driver differences (subgroup sizes, `shaderFloat64` performance, buffer limits).
- **Multi-GPU support**: partition tables across devices, with a merge step for aggregates and sorts.
- **Linux and macOS** (Vulkan and MoltenVK), which also means finishing the OS layer outside Win32.
- **Other GPU backends** (CUDA, HIP/ROCm, DirectX 12 compute) behind the existing backend interface.
- **Users and access control**: authentication for network connections, `GRANT`/`REVOKE`.
- **Transactions, WAL and recovery.**
- **Wider SQL**: recursive CTEs, set operations, GPU window functions, a GPU projection kernel for `SELECT` output.
- **Benchmarks against more engines** (ClickHouse, PostgreSQL) and standard suites such as TPC-H and ClickBench.
- **Additional query languages** on the same execution engine.
