// compute_ql: high cardinality GROUP BY on the generated hash aggregation kernel against the precompiled one. each query runs with QE_GENERATE_HASH_REDUCE:0 and :1,
// fused aggregation is switched off so the plan goes through qe_aggregate; the answers have to agree and the kernels used are listed
// --rows=N sets the table size, --narrow gives a and b small ranges
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

typedef struct PQ_Case PQ_Case;
struct PQ_Case
{
  char* label;
  String8 where;
  B32 count_only;
};

typedef struct PQ_Data PQ_Data;
struct PQ_Data
{
  U64 row_count;
  U32* a;
  S32* b;
  U64* c;
  F32* d;
  F64* e;
  String8* name;
  String8* tag;
  U64 sample_c;
  String8 sample_name;
};

global B32 g_pq_narrow = 0;

global char* g_pq_tags[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot" };

internal void
pq_load_table(Arena* arena, CQL_Database* database, PQ_Data* data, U64 row_count)
{
  data->row_count = row_count;
  CQL_Table* table = cql_table_alloc(str8_lit("pq"));
  char* names[] = { "id", "a", "b", "c", "d", "e", "name", "tag" };
  CQL_ColumnType types[] = { CQL_ColumnType_U32, CQL_ColumnType_U32, CQL_ColumnType_I32, CQL_ColumnType_U64, CQL_ColumnType_F32, CQL_ColumnType_F64, CQL_ColumnType_String8, CQL_ColumnType_String8 };
  for (U32 c = 0; c < ArrayCount(names); c++)
  {
    cql_table_add_column(table, cql_column_schema_create(str8_cstring(names[c]), types[c]));
  }
  cql_database_add_table(database, table);

  U32* id = (U32*)cql_column_alloc_fixed_rows(table->columns[0], row_count);
  data->a = (U32*)cql_column_alloc_fixed_rows(table->columns[1], row_count);
  data->b = (S32*)cql_column_alloc_fixed_rows(table->columns[2], row_count);
  data->c = (U64*)cql_column_alloc_fixed_rows(table->columns[3], row_count);
  data->d = (F32*)cql_column_alloc_fixed_rows(table->columns[4], row_count);
  data->e = (F64*)cql_column_alloc_fixed_rows(table->columns[5], row_count);
  data->name = push_array(arena, String8, row_count);
  data->tag = push_array(arena, String8, row_count);

  Bench_Rng rng = { 0x9E3779B97F4A7C15ULL };
  for (U64 row = 0; row < row_count; row++)
  {
    U64 r0 = bench_rng_next(&rng);
    U64 r1 = bench_rng_next(&rng);
    U64 r2 = bench_rng_next(&rng);
    id[row] = (U32)row;
    // tec: --narrow gives a a 16 bit range and b a byte range, id is already its row number, so a streamed scan holds all three narrow
    data->a[row] = g_pq_narrow ? (U32)(r0 % 60000u) : (U32)(r0 % 2000000u);
    data->b[row] = g_pq_narrow ? (S32)((r0 >> 32) % 200u) - 100 : (S32)(r0 >> 32);
    data->c[row] = r1 % 1000000000000ULL;
    data->d[row] = (F32)((r1 >> 40) % 1000u) / 1000.0f;
    data->e[row] = (F64)(r2 % 100000u) / 100.0;
    U64 w = (r2 >> 20) % ArrayCount(g_bench_words);
    data->name[row] = push_str8f(arena, "%s_%06llu", g_bench_words[w], (r2 >> 32) % 1000000ull);
    data->tag[row] = str8_cstring(g_pq_tags[(r2 >> 50) % ArrayCount(g_pq_tags)]);
  }
  cql_column_fill_from_strings(table->columns[6], data->name, 0, row_count);
  cql_column_fill_from_strings(table->columns[7], data->tag, 0, row_count);
  table->row_count = row_count;

  data->sample_c = data->c[row_count / 3];
  data->sample_name = data->name[row_count / 2];
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  log_alloc();

  U64 row_count = 3000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    row_count = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  g_pq_narrow = cmd_line_has_flag(cmdline, str8_lit("narrow"));
  {
    char setting[96];
    snprintf(setting, sizeof(setting), "CQL_COLUMN_ARENA_RESERVE_SIZE:%llu", row_count * sizeof(U64) + MB(256));
    settings_load_from_string(str8_cstring(setting));
  }
  settings_load_from_string(str8_lit("QE_FUSE_AGGREGATE:0"));
  cql_init();
  gpu_init();
  qk_init();

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  CQL_Database* database = cql_database_alloc(str8_lit("pq_db"));
  cql_add_database(database);
  PQ_Data data = {0};
  pq_load_table(arena, database, &data, row_count);

  char* queries[] =
  {
    "SELECT a, COUNT(*) FROM pq GROUP BY a;",
    "SELECT a, COUNT(*), SUM(d), MIN(d), MAX(d) FROM pq GROUP BY a;",
    "SELECT a, AVG(d), MAX(d) FROM pq GROUP BY a;",
    "SELECT a, SUM(e), MIN(e), MAX(e), COUNT(*) FROM pq GROUP BY a;",
    "SELECT a, AVG(e) FROM pq GROUP BY a;",
    "SELECT a, SUM(d), SUM(e) FROM pq WHERE b > 0 GROUP BY a;",
    "SELECT a, b, COUNT(*), MAX(d) FROM pq GROUP BY a, b;",
    "SELECT c, COUNT(*), MIN(e) FROM pq GROUP BY c;",
    "SELECT name, COUNT(*), SUM(a) FROM pq GROUP BY name;",
    "SELECT name, tag, COUNT(*), MAX(d) FROM pq GROUP BY name, tag;",
    "SELECT tag, a, COUNT(*), MIN(e) FROM pq GROUP BY tag, a;",
    "SELECT a, b, c, COUNT(*) FROM pq GROUP BY a, b, c;",
  };
  printf("hash group by: %llu rows\n", row_count);
  U64 failures = 0;
  U64 generated_count = 0;
  for (U32 i = 0; i < ArrayCount(queries); i++)
  {
    U64 rows[2] = {0};
    U64 sums[2] = {0};
    Bench_Stats stats[2];
    String8 kernels[2];
    for (U32 mode = 0; mode < 2; mode++)
    {
      settings_load_from_string(mode ? str8_lit("QE_GENERATE_HASH_REDUCE:1") : str8_lit("QE_GENERATE_HASH_REDUCE:0"));
      gpu_kernel_log_reset();
      stats[mode] = bench_run_cql_query(database, str8_cstring(queries[i]), &rows[mode], &sums[mode]);
      kernels[mode] = bench_kernel_log_string(arena);
    }
    B32 precompiled_hash = str8_find_needle(kernels[0], 0, str8_lit("aggregate_hash_reduce"), 0) < kernels[0].size;
    B32 assign_replaced = str8_find_needle(kernels[0], 0, str8_lit("aggregate_assign"), 0) < kernels[0].size &&
      str8_find_needle(kernels[1], 0, str8_lit("aggregate_assign"), 0) >= kernels[1].size;
    B32 generated_hash = assign_replaced || (str8_find_needle(kernels[1], 0, str8_lit("aggregate_hash_reduce"), 0) >= kernels[1].size && precompiled_hash);
    if (generated_hash)
    {
      generated_count += 1;
    }
    B32 match = rows[0] == rows[1] && sums[0] == sums[1];
    if (!match)
    {
      failures += 1;
    }
    printf("%-9llu groups  precompiled %8.3f ms  generated %8.3f ms  %5.2fx%s%s\n    [%.*s]\n    [%.*s]\n    %s\n",
           rows[1], stats[0].execute_median, stats[1].execute_median, stats[0].execute_median / Max(stats[1].execute_median, 0.0001),
           match ? "" : "  !! MISMATCH", precompiled_hash ? "" : "  (hash reduce not used)", str8_varg(kernels[0]), str8_varg(kernels[1]), queries[i]);
    fflush(stdout);
  }
  printf("%llu of %u queries ran the group slot assignment or the hash aggregation on a generated kernel, %llu failures\n", generated_count, (U32)ArrayCount(queries), failures);
  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0 || generated_count == 0)
  {
    exit(1);
  }
}
