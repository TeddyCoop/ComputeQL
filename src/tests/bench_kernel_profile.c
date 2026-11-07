// compute_ql: GPU time per kernel for a set of queries, from the opt-in timestamps of each dispatch (g_gpu_kernel_profile). each query runs once to warm up and once timed;
// the kernels are listed by the time they took in that run. --rows=N sets the pq and fact table size
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

#define JT_DIM_ROWS 5000

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
pr_run(CQL_Database* database, char* sql)
{
  for (U32 pass = 0; pass < 2; pass++)
  {
    if (pass == 1)
    {
      gpu_kernel_profile_reset();
    }
    Temp scratch = scratch_begin(0, 0);
    Arena* arena = scratch.arena;
    String8 sql_text = str8_cstring(sql);
    SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
    SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
    IR_Query* ir_query = ir_generate_from_ast(arena, ast);
    IR_Node* select_node = ir_query->execution_nodes;
    U64 temp_mark = database->temp_table_count;
    U64 start = os_now_microseconds();
    PLAN_ExecResult result = plan_run_select(arena, database, select_node, NULL);
    U64 row_count = 0;
    bench_cql_consume_result(&result, select_node, &row_count);
    U64 elapsed = os_now_microseconds() - start;
    cql_database_release_temp_tables_from(database, temp_mark);
    scratch_end(scratch);
    if (pass == 1)
    {
      GPU_KernelProfile* profile = &g_gpu_kernel_profile;
      F64 gpu_total = 0;
      for (U32 k = 0; k < profile->count; k++)
      {
        gpu_total += (F64)profile->total_ns[k] / 1000000.0;
      }
      printf("%s\n  %llu rows, %.3f ms wall, %.3f ms in kernels\n", sql, row_count, (F64)elapsed / 1000.0, gpu_total);
      for (U32 k = 0; k < profile->count; k++)
      {
        printf("    %-34.*s %8.3f ms  x%llu\n", str8_varg(profile->names[k]), (F64)profile->total_ns[k] / 1000000.0, profile->dispatches[k]);
      }
      fflush(stdout);
    }
  }
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
  {
    char setting[96];
    snprintf(setting, sizeof(setting), "CQL_COLUMN_ARENA_RESERVE_SIZE:%llu", row_count * sizeof(U64) + MB(256));
    settings_load_from_string(str8_cstring(setting));
  }
  settings_load_from_string(str8_lit("QE_FUSE_AGGREGATE:0"));
  cql_init();
  gpu_init();
  qk_init();
  g_gpu_kernel_profile.enabled = 1;

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  CQL_Database* database = cql_database_alloc(str8_lit("pr_db"));
  cql_add_database(database);
  PQ_Data data = {0};
  pq_load_table(arena, database, &data, row_count);
  jt_load(arena, database, row_count);

  char* queries[] =
  {
    "SELECT a, COUNT(*), SUM(d), MIN(d), MAX(d) FROM pq GROUP BY a;",
    "SELECT a, COUNT(*) FROM pq GROUP BY a;",
    "SELECT b, COUNT(*), SUM(d) FROM pq GROUP BY b;",
    "SELECT tag, COUNT(*), SUM(d), MIN(e), MAX(e) FROM pq GROUP BY tag;",
    "SELECT tag, COUNT(*), AVG(e) FROM pq WHERE b > 0 GROUP BY tag;",
    "SELECT a, SUM(e), AVG(e) FROM pq GROUP BY a;",
    "SELECT name, COUNT(*) FROM pq GROUP BY name;",
    "SELECT c, COUNT(*) FROM pq GROUP BY c;",
    "SELECT a, b, COUNT(*) FROM pq GROUP BY a, b;",
    "SELECT COUNT(*) FROM fact JOIN dim ON fact.k = dim.k;",
    "SELECT dim.g, COUNT(*), SUM(fact.v) FROM fact JOIN dim ON fact.k = dim.k GROUP BY dim.g;",
    "SELECT dim.g, SUM(fact.w), MIN(fact.w), MAX(fact.w) FROM fact JOIN dim ON fact.k = dim.k GROUP BY dim.g;",
    "SELECT COUNT(*) FROM fact JOIN dim ON fact.ks = dim.ks;",
    "SELECT fact.v, dim.a FROM fact JOIN dim ON fact.k = dim.k WHERE dim.g < 3;",
    "SELECT dim.a, COUNT(*) FROM fact JOIN dim ON fact.k = dim.k GROUP BY dim.a;",
  };
  for (U32 i = 0; i < ArrayCount(queries); i++)
  {
    pr_run(database, queries[i]);
  }
  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
}
