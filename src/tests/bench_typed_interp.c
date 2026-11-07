// compute_ql's  typed interpreter (scan_filter_typed) against the double interpreter (scan_filter) on u32, i32, f32, bool and f64 columns.
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define BENCH_TIMED_RUNS 3
#define BENCH_WARMUP_RUNS 1

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "thread_pool/thread_pool.h"
#include "settings/settings.h"
#include "cql/cql_inc.h"
#include "ir_gen/ir_gen_inc.h"
#include "gpu/gpu_inc.h"
#include "planner/plan_node.h"
#include "query_exec/query_exec.h"
#include "optimizer/optimizer_inc.h"
#include "planner/planner.h"
#include "application.h"
#include "third_party/sqlite/sqlite3.h"
#include "third_party/duckdb/duckdb.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "thread_pool/thread_pool.c"
#include "settings/settings.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "cql/cql_inc.c"
#include "query_exec/query_exec.c"
#include "optimizer/optimizer_inc.c"
#include "planner/planner.c"
#include "application.c"

#include "tests/bench_common.h"

#define TI_ROWS 1000000

typedef struct TI_Case TI_Case;
struct TI_Case
{
  char* where;
  B32 expect_typed;
};

internal void ti_load_table(CQL_Database* database, U64 row_count);

internal void
ti_load_table(CQL_Database* database, U64 row_count)
{
  CQL_Table* table = cql_table_alloc(str8_lit("t"));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("u"), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("i"), CQL_ColumnType_I32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("f"), CQL_ColumnType_F32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("b"), CQL_ColumnType_Bool));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("d"), CQL_ColumnType_F64));
  cql_database_add_table(database, table);
  
  U32* u = (U32*)cql_column_alloc_fixed_rows(table->columns[0], row_count);
  S32* i = (S32*)cql_column_alloc_fixed_rows(table->columns[1], row_count);
  F32* f = (F32*)cql_column_alloc_fixed_rows(table->columns[2], row_count);
  U8* b = (U8*)cql_column_alloc_fixed_rows(table->columns[3], row_count);
  F64* d = (F64*)cql_column_alloc_fixed_rows(table->columns[4], row_count);
  for (U64 row = 0; row < row_count; row++)
  {
    u[row] = (U32)(row * 2654435761ULL);
    i[row] = (S32)((row * 7919) % 2000001) - 1000000;
    f[row] = (F32)(row % 20000) * 0.25f - 2500.0f;
    if (row % 1000 == 7)
    {
      f[row] = -0.0f;
    }
    b[row] = (row % 3 == 0) ? 1 : 0;
    d[row] = (F64)row * 0.5;
  }
  table->row_count = row_count;
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  cql_init();
  gpu_init();
  qk_init();
  
  CQL_Database* database = cql_database_alloc(str8_lit("typed_db"));
  cql_add_database(database);
  ti_load_table(database, TI_ROWS);
  
  TI_Case cases[] =
  {
    { "u < 2147483648", 1 },
    { "u >= 4000000000 OR u < 100000", 1 },
    { "i < 0", 1 },
    { "i >= 0 AND i != 12345", 1 },
    { "i < 500000 AND i > 400000", 1 },
    { "f > 100.25", 1 },
    { "f = 0", 1 },
    { "f != 0", 1 },
    { "f <= 2499.75", 1 },
    { "b = 1", 1 },
    { "b != 1 AND u < 3000000000", 1 },
    { "i < 0 AND f > 0 AND u < 3000000000", 1 },
    { "(i < 100 OR f > 1000) AND b = 0", 1 },
    { "f < 0.1", 0 },
    { "u < 3.5", 0 },
    { "i < 4000000000", 0 },
    { "d < 1000.5", 0 },
    { "u < 2000000000 AND d > 10", 0 },
  };
  
  // tec: this compares the two interpreters, so the kernels generated for the query are off
  settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
  settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
  U64 failures = 0;
  for (U64 c = 0; c < ArrayCount(cases); c++)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 sql = push_str8f(scratch.arena, "SELECT COUNT(*), SUM(u) FROM t WHERE %s;", cases[c].where);
    U64 checksums[2] = {0};
    U64 rows[2] = {0};
    String8 kernels[2] = {0};
    for (U32 mode = 0; mode < 2; mode++)
    {
      settings_load_from_string(mode == 0 ? str8_lit("QE_SCAN_TYPED_INTERP:0") : str8_lit("QE_SCAN_TYPED_INTERP:1"));
      gpu_kernel_log_reset();
      bench_run_cql_query(database, sql, &rows[mode], &checksums[mode]);
      kernels[mode] = bench_kernel_log_string(scratch.arena);
    }
    B32 ran_typed = str8_find_needle(kernels[1], 0, str8_lit("scan_filter_typed"), 0) < kernels[1].size;
    B32 same = checksums[0] == checksums[1] && rows[0] == rows[1];
    B32 ok = same && ran_typed == cases[c].expect_typed;
    if (!ok)
    {
      failures += 1;
    }
    printf("%-4s %-40s double %20llu  typed %20llu  typed run: %.*s\n", ok ? "ok" : "FAIL", cases[c].where, checksums[0], checksums[1], str8_varg(kernels[1]));
    scratch_end(scratch);
  }
  settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:1"));
  settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
  settings_load_from_string(str8_lit("QE_SCAN_TYPED_INTERP:0"));
  printf("%llu of %llu filters failed\n", failures, (U64)ArrayCount(cases));
  
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0)
  {
    exit(1);
  }
}
