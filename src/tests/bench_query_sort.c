// compute_ql: ORDER BY on the generated sort kernel against the precompiled bitonic sort. keys that pack into 64 bits go to the composite sort, so it is switched off here
// every query is run both ways and the output is checked row by row against the table: same row count, every row once, keys in order
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

typedef struct PQ_SortKey PQ_SortKey;
struct PQ_SortKey
{
  char column;
  B32 desc;
};

typedef struct PQ_SortCase PQ_SortCase;
struct PQ_SortCase
{
  char* order_by;
  U32 key_count;
  PQ_SortKey keys[4];
};

internal F64
pq_sort_value(PQ_Data* data, char column, U64 row)
{
  switch (column)
  {
    case 'a': return (F64)data->a[row];
    case 'b': return (F64)data->b[row];
    case 'c': return (F64)data->c[row];
    case 'd': return (F64)data->d[row];
    case 'e': return data->e[row];
  }
  return 0.0;
}

// tec: 0 when the output is a permutation of the table in key order, otherwise the number of the first bad position plus one
internal U64
pq_sort_verify(PQ_Data* data, PQ_SortCase* test_case, PLAN_ExecResult* result, U64 row_count)
{
  if (result->rows.count != row_count)
  {
    return 1;
  }
  U64* ids = result->rows.row_indices[0];
  U8* seen = (U8*)calloc(row_count, 1);
  U64 bad = 0;
  for (U64 i = 0; i < row_count && !bad; i++)
  {
    if (ids[i] >= row_count || seen[ids[i]])
    {
      bad = i + 1;
      break;
    }
    seen[ids[i]] = 1;
    if (i == 0)
    {
      continue;
    }
    for (U32 k = 0; k < test_case->key_count; k++)
    {
      F64 prev = pq_sort_value(data, test_case->keys[k].column, ids[i - 1]);
      F64 cur = pq_sort_value(data, test_case->keys[k].column, ids[i]);
      if (prev == cur)
      {
        continue;
      }
      B32 in_order = test_case->keys[k].desc ? prev > cur : prev < cur;
      if (!in_order)
      {
        bad = i + 1;
      }
      break;
    }
  }
  free(seen);
  return bad;
}

typedef struct PQ_SortResult PQ_SortResult;
struct PQ_SortResult
{
  F64 best_ms;
  U64 bad;
  String8 kernels;
};

internal PQ_SortResult
pq_sort_run(Arena* arena, CQL_Database* database, PQ_Data* data, PQ_SortCase* test_case, B32 generated)
{
  settings_load_from_string(generated ? str8_lit("QE_GENERATE_SORT:1") : str8_lit("QE_GENERATE_SORT:0"));
  String8 sql = push_str8f(arena, "SELECT id FROM pq ORDER BY %s;", test_case->order_by);
  PQ_SortResult out = {0};
  out.best_ms = 1e30;
  for (U32 run = 0; run < 4; run++)
  {
    Temp scratch = scratch_begin(0, 0);
    SQL_TokenizeResult tok = sql_tokenize_from_text(scratch.arena, sql);
    SQL_Node* ast = sql_parse(scratch.arena, tok.tokens, tok.count, sql);
    IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
    IR_Node* select_node = ir_query->execution_nodes;
    U64 temp_mark = database->temp_table_count;
    gpu_kernel_log_reset();
    U64 t0 = os_now_microseconds();
    PLAN_ExecResult result = plan_run_select(scratch.arena, database, select_node, NULL);
    F64 ms = (F64)(os_now_microseconds() - t0) / 1000.0;
    if (run > 0 && ms < out.best_ms)
    {
      out.best_ms = ms;
    }
    if (run == 0)
    {
      out.bad = pq_sort_verify(data, test_case, &result, data->row_count);
      out.kernels = bench_kernel_log_string(arena);
    }
    cql_database_release_temp_tables_from(database, temp_mark);
    scratch_end(scratch);
  }
  return out;
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
  String8List setting_list = cmd_line_strings(cmdline, str8_lit("setting"));
  for (String8Node* setting_node = setting_list.first; setting_node; setting_node = setting_node->next)
  {
    settings_load_from_string(setting_node->string);
  }
  settings_load_from_string(str8_lit("QE_SORT_COMPOSITE:0"));

  cql_init();
  gpu_init();
  qk_init();

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  CQL_Database* database = cql_database_alloc(str8_lit("pq_db"));
  cql_add_database(database);
  PQ_Data data = {0};
  pq_load_table(arena, database, &data, row_count);

  PQ_SortCase cases[] =
  {
    { "a", 1, { { 'a', 0 } } },
    { "a DESC", 1, { { 'a', 1 } } },
    { "e", 1, { { 'e', 0 } } },
    { "c DESC", 1, { { 'c', 1 } } },
    { "b, a DESC", 2, { { 'b', 0 }, { 'a', 1 } } },
    { "d DESC, a", 2, { { 'd', 1 }, { 'a', 0 } } },
    { "e, c", 2, { { 'e', 0 }, { 'c', 0 } } },
    { "c, e DESC, d", 3, { { 'c', 0 }, { 'e', 1 }, { 'd', 0 } } },
    { "c, d DESC, a, b DESC", 4, { { 'c', 0 }, { 'd', 1 }, { 'a', 0 }, { 'b', 1 } } },
    { "b, d", 2, { { 'b', 0 }, { 'd', 0 } } },
  };

  printf("sort: %llu rows\n", row_count);
  U64 failures = 0;
  U64 generated_count = 0;
  for (U32 i = 0; i < ArrayCount(cases); i++)
  {
    PQ_SortResult old_run = pq_sort_run(arena, database, &data, &cases[i], 0);
    PQ_SortResult new_run = pq_sort_run(arena, database, &data, &cases[i], 1);
    B32 ran_generated = str8_find_needle(new_run.kernels, 0, str8_lit("qk_"), 0) < new_run.kernels.size;
    if (ran_generated)
    {
      generated_count += 1;
    }
    B32 ok = old_run.bad == 0 && new_run.bad == 0;
    if (!ok)
    {
      failures += 1;
    }
    printf("%-26s precompiled %9.3f ms  generated %9.3f ms  %5.2fx  [%.*s]%s%s%s\n",
           cases[i].order_by, old_run.best_ms, new_run.best_ms, old_run.best_ms / Max(new_run.best_ms, 0.0001),
           str8_varg(new_run.kernels), ran_generated ? "" : "  (not generated)",
           old_run.bad ? "  !! PRECOMPILED OUT OF ORDER" : "", new_run.bad ? "  !! GENERATED OUT OF ORDER" : "");
    fflush(stdout);
  }
  printf("%llu of %u sorts ran on a generated kernel, %llu failures\n", generated_count, (U32)ArrayCount(cases), failures);

  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0 || generated_count == 0)
  {
    exit(1);
  }
}
