// compute_ql: joins on generated kernels against the precompiled ones. each query runs with QE_GENERATE_JOIN:0 and :1, the row counts and checksums have to agree,
// and the kernels used are listed so the run shows which path it took
// --rows=N sets the fact table size, --small-buffers lowers the GPU buffer limit so the streamed join pushdown runs
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)
#define BENCH_TIMED_RUNS 5
#define BENCH_WARMUP_RUNS 2

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

#define JT_DIM_ROWS 5000

typedef struct JT_Case JT_Case;
struct JT_Case
{
  char* sql;
};

internal void
jt_load(Arena* arena, CQL_Database* database, U64 fact_rows)
{
  Bench_Rng rng = { 0x9E3779B97F4A7C15ULL };

  CQL_Table* dim = cql_table_alloc(str8_lit("dim"));
  char* dim_names[] = { "k", "g", "a", "ks" };
  CQL_ColumnType dim_types[] = { CQL_ColumnType_U32, CQL_ColumnType_U32, CQL_ColumnType_U32, CQL_ColumnType_String8 };
  for (U32 c = 0; c < 4; c++)
  {
    cql_table_add_column(dim, cql_column_schema_create(str8_cstring(dim_names[c]), dim_types[c]));
  }
  cql_database_add_table(database, dim);
  U32* dim_k = (U32*)cql_column_alloc_fixed_rows(dim->columns[0], JT_DIM_ROWS);
  U32* dim_g = (U32*)cql_column_alloc_fixed_rows(dim->columns[1], JT_DIM_ROWS);
  U32* dim_a = (U32*)cql_column_alloc_fixed_rows(dim->columns[2], JT_DIM_ROWS);
  String8* dim_ks = push_array(arena, String8, JT_DIM_ROWS);
  for (U32 i = 0; i < JT_DIM_ROWS; i++)
  {
    // tec: distinct keys spread over a range far too wide for a dense table
    dim_k[i] = i * 8009u + (i % 7u);
    dim_g[i] = i % 37u;
    dim_a[i] = (U32)(bench_rng_next(&rng) % 1000u);
    dim_ks[i] = push_str8f(arena, "key_%u", dim_k[i]);
  }
  cql_column_fill_from_strings(dim->columns[3], dim_ks, 0, JT_DIM_ROWS);
  dim->row_count = JT_DIM_ROWS;

  CQL_Table* fact = cql_table_alloc(str8_lit("fact"));
  char* fact_names[] = { "k", "v", "w", "u", "ks" };
  CQL_ColumnType fact_types[] = { CQL_ColumnType_U32, CQL_ColumnType_U32, CQL_ColumnType_I32, CQL_ColumnType_U32, CQL_ColumnType_String8 };
  for (U32 c = 0; c < 5; c++)
  {
    cql_table_add_column(fact, cql_column_schema_create(str8_cstring(fact_names[c]), fact_types[c]));
  }
  cql_database_add_table(database, fact);
  U32* fk = (U32*)cql_column_alloc_fixed_rows(fact->columns[0], fact_rows);
  U32* fv = (U32*)cql_column_alloc_fixed_rows(fact->columns[1], fact_rows);
  S32* fw = (S32*)cql_column_alloc_fixed_rows(fact->columns[2], fact_rows);
  U32* fu = (U32*)cql_column_alloc_fixed_rows(fact->columns[3], fact_rows);
  String8* fks = push_array(arena, String8, fact_rows);
  for (U64 i = 0; i < fact_rows; i++)
  {
    U64 r = bench_rng_next(&rng);
    U32 pick = (U32)(r % JT_DIM_ROWS);
    // tec: about 3 rows in 4 find a dim row, the rest miss
    fk[i] = ((r >> 40) & 3u) != 0 ? pick * 8009u + (pick % 7u) : (U32)(r >> 8) | 1u;
    fv[i] = (U32)((r >> 20) % 200u);
    fw[i] = (S32)((r >> 32) % 60000u) - 30000;
    fu[i] = (U32)(r >> 16) * 2654435761u;
    fks[i] = push_str8f(arena, "key_%u", fk[i]);
  }
  cql_column_fill_from_strings(fact->columns[4], fks, 0, fact_rows);
  fact->row_count = fact_rows;
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  log_alloc();

  U64 fact_rows = 3000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    fact_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  {
    char setting[96];
    snprintf(setting, sizeof(setting), "CQL_COLUMN_ARENA_RESERVE_SIZE:%llu", fact_rows * sizeof(U64) + MB(256));
    settings_load_from_string(str8_cstring(setting));
  }
  if (cmd_line_has_flag(cmdline, str8_lit("small-buffers")))
  {
    // tec: a fact table past the buffer limit takes the streamed paths, which is where the join pushdown and its probe kernel are
    settings_load_from_string(str8_lit("GPU_MAX_BUFFER_SIZE:4000000"));
  }
  cql_init();
  gpu_init();
  qk_init();

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  CQL_Database* database = cql_database_alloc(str8_lit("jt_db"));
  cql_add_database(database);
  jt_load(arena, database, fact_rows);

  JT_Case cases[] =
  {
    { "SELECT COUNT(*) FROM fact JOIN dim ON fact.k = dim.k;" },
    { "SELECT dim.g, COUNT(*), SUM(fact.v) FROM fact JOIN dim ON fact.k = dim.k GROUP BY dim.g;" },
    { "SELECT dim.g, SUM(fact.w), MIN(fact.w), MAX(fact.w) FROM fact JOIN dim ON fact.k = dim.k GROUP BY dim.g;" },
    { "SELECT dim.g, SUM(fact.u), MAX(fact.u), MIN(fact.v) FROM fact JOIN dim ON fact.k = dim.k GROUP BY dim.g;" },
    { "SELECT dim.g, AVG(fact.v), AVG(fact.w), COUNT(*) FROM fact JOIN dim ON fact.k = dim.k GROUP BY dim.g;" },
    { "SELECT dim.a, SUM(fact.v), SUM(fact.w) FROM fact JOIN dim ON fact.k = dim.k WHERE dim.g < 10 GROUP BY dim.a;" },
    { "SELECT fact.v, dim.a FROM fact JOIN dim ON fact.k = dim.k WHERE fact.v < 3;" },
    { "SELECT fact.v, dim.a FROM dim LEFT JOIN fact ON dim.k = fact.k WHERE fact.v < 2;" },
    { "SELECT fact.w, dim.g FROM fact JOIN dim ON fact.ks = dim.ks WHERE fact.v < 2;" },
    { "SELECT dim.ks, fact.v FROM dim LEFT JOIN fact ON dim.ks = fact.ks WHERE fact.w < -29990;" },
    { "SELECT dim.g, COUNT(*), SUM(fact.v) FROM fact JOIN dim ON fact.ks = dim.ks GROUP BY dim.g;" },
  };

  printf("joins: %llu fact rows, %u dim rows\n", fact_rows, (U32)JT_DIM_ROWS);
  U64 failures = 0;
  for (U32 i = 0; i < ArrayCount(cases); i++)
  {
    U64 rows[2] = {0};
    U64 sums[2] = {0};
    Bench_Stats stats[2];
    String8 kernels[2];
    for (U32 mode = 0; mode < 2; mode++)
    {
      settings_load_from_string(mode ? str8_lit("QE_GENERATE_JOIN:1") : str8_lit("QE_GENERATE_JOIN:0"));
      gpu_kernel_log_reset();
      stats[mode] = bench_run_cql_query(database, str8_cstring(cases[i].sql), &rows[mode], &sums[mode]);
      kernels[mode] = bench_kernel_log_string(arena);
    }
    B32 match = rows[0] == rows[1] && sums[0] == sums[1];
    if (!match)
    {
      failures += 1;
    }
    printf("%-9llu rows  precompiled %8.3f ms  generated %8.3f ms  %5.2fx%s\n    [%.*s]\n    [%.*s]\n    %s\n",
           rows[1], stats[0].execute_median, stats[1].execute_median, stats[0].execute_median / Max(stats[1].execute_median, 0.0001),
           match ? "" : "  !! MISMATCH", str8_varg(kernels[0]), str8_varg(kernels[1]), cases[i].sql);
    fflush(stdout);
  }
  printf("%llu failures\n", failures);
  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0)
  {
    exit(1);
  }
}
