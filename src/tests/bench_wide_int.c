// compute_ql's handling of 64 bit whole numbers past 2^53, where a double no longer holds every value.
// every query runs with the check off, to show what the engine answers, and on, where it must answer exactly or fail.
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define BENCH_TIMED_RUNS 1
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

#define WI_TWO_53 9007199254740992ull

#define WI_SUM_ROWS  2500000ull
#define WI_HASH_ROWS 300000ull

global U64 g_wi_rows = 512;

typedef enum WI_Kind
{
  WI_Kind_Value,      // tec: the first cell of the result is expected
  WI_Kind_RowCount,   // tec: the number of result rows is expected
  WI_Kind_ProjectK,   // tec: the sum of k - 2^53 over the result rows, read from the table as stored, is expected
  WI_Kind_OrderG,     // tec: the result rows are expected in table order 0, 1, 2, ...
} WI_Kind;

typedef struct WI_Case WI_Case;
struct WI_Case
{
  char* sql;
  WI_Kind kind;
  U64 expected;
  B32 must_run;       // tec: no value past 2^53 is compared, grouped, sorted or summed, so failing the query is wrong too
};

typedef struct WI_Outcome WI_Outcome;
struct WI_Outcome
{
  B32 ran;
  U64 value;
  String8 kernels;
};

internal CQL_Table* wi_table(CQL_Database* database, char* name);
internal void wi_load_tables(CQL_Database* database);
internal WI_Outcome wi_run(Arena* out_arena, CQL_Database* database, WI_Case* c);

internal CQL_Table*
wi_table(CQL_Database* database, char* name)
{
  CQL_Table* table = cql_table_alloc(str8_cstring(name));
  cql_database_add_table(database, table);
  return table;
}

internal void
wi_load_tables(CQL_Database* database)
{
  // tec: w.k counts up from 2^53, so k and k + 1 round to the same double for every even k
  CQL_Table* w = wi_table(database, "w");
  cql_table_add_column(w, cql_column_schema_create(str8_lit("g"), CQL_ColumnType_U32));
  cql_table_add_column(w, cql_column_schema_create(str8_lit("k"), CQL_ColumnType_U64));
  cql_table_add_column(w, cql_column_schema_create(str8_lit("s"), CQL_ColumnType_I64));
  U32* w_g = (U32*)cql_column_alloc_fixed_rows(w->columns[0], g_wi_rows);
  U64* w_k = (U64*)cql_column_alloc_fixed_rows(w->columns[1], g_wi_rows);
  S64* w_s = (S64*)cql_column_alloc_fixed_rows(w->columns[2], g_wi_rows);
  for (U64 row = 0; row < g_wi_rows; row++)
  {
    w_g[row] = (U32)row;
    w_k[row] = WI_TWO_53 + row;
    w_s[row] = -(S64)WI_TWO_53 - (S64)row;
  }
  w->row_count = g_wi_rows;

  // tec: d.k holds every even w.k, so a join on k matches half of w
  CQL_Table* d = wi_table(database, "d");
  cql_table_add_column(d, cql_column_schema_create(str8_lit("h"), CQL_ColumnType_U32));
  cql_table_add_column(d, cql_column_schema_create(str8_lit("k"), CQL_ColumnType_U64));
  U32* d_h = (U32*)cql_column_alloc_fixed_rows(d->columns[0], g_wi_rows / 2);
  U64* d_k = (U64*)cql_column_alloc_fixed_rows(d->columns[1], g_wi_rows / 2);
  for (U64 row = 0; row < g_wi_rows / 2; row++)
  {
    d_h[row] = (U32)row;
    d_k[row] = WI_TWO_53 + row * 2;
  }
  d->row_count = g_wi_rows / 2;

  // tec: the control, a U64 column below 2^53 that must still run
  CQL_Table* c = wi_table(database, "c");
  cql_table_add_column(c, cql_column_schema_create(str8_lit("g"), CQL_ColumnType_U32));
  cql_table_add_column(c, cql_column_schema_create(str8_lit("k"), CQL_ColumnType_U64));
  U32* c_g = (U32*)cql_column_alloc_fixed_rows(c->columns[0], g_wi_rows);
  U64* c_k = (U64*)cql_column_alloc_fixed_rows(c->columns[1], g_wi_rows);
  for (U64 row = 0; row < g_wi_rows; row++)
  {
    c_g[row] = (U32)row;
    c_k[row] = (WI_TWO_53 / 2) + row;
  }
  c->row_count = g_wi_rows;

  // tec: a two key join whose first key is large, the case a packed 64 bit key would get wrong
  CQL_Table* p = wi_table(database, "p");
  cql_table_add_column(p, cql_column_schema_create(str8_lit("a"), CQL_ColumnType_U32));
  cql_table_add_column(p, cql_column_schema_create(str8_lit("b"), CQL_ColumnType_U32));
  U32* p_a = (U32*)cql_column_alloc_fixed_rows(p->columns[0], g_wi_rows);
  U32* p_b = (U32*)cql_column_alloc_fixed_rows(p->columns[1], g_wi_rows);
  for (U64 row = 0; row < g_wi_rows; row++)
  {
    p_a[row] = 4000000000u - (U32)row;
    p_b[row] = (U32)(row % 7);
  }
  p->row_count = g_wi_rows;

  CQL_Table* q = wi_table(database, "q");
  cql_table_add_column(q, cql_column_schema_create(str8_lit("a"), CQL_ColumnType_U32));
  cql_table_add_column(q, cql_column_schema_create(str8_lit("b"), CQL_ColumnType_U32));
  U32* q_a = (U32*)cql_column_alloc_fixed_rows(q->columns[0], g_wi_rows / 2);
  U32* q_b = (U32*)cql_column_alloc_fixed_rows(q->columns[1], g_wi_rows / 2);
  for (U64 row = 0; row < g_wi_rows / 2; row++)
  {
    q_a[row] = 4000000000u - (U32)(row * 2);
    q_b[row] = (U32)((row * 2) % 7);
  }
  q->row_count = g_wi_rows / 2;

  // tec: sums that grow past 2^53 from 32 bit columns, no value is wide. u.v summed over all rows is about 1.07e16
  CQL_Table* u = wi_table(database, "u");
  cql_table_add_column(u, cql_column_schema_create(str8_lit("g"), CQL_ColumnType_U32));
  cql_table_add_column(u, cql_column_schema_create(str8_lit("v"), CQL_ColumnType_U32));
  U32* u_g = (U32*)cql_column_alloc_fixed_rows(u->columns[0], WI_SUM_ROWS);
  U32* u_v = (U32*)cql_column_alloc_fixed_rows(u->columns[1], WI_SUM_ROWS);
  for (U64 row = 0; row < WI_SUM_ROWS; row++)
  {
    u_g[row] = (U32)row;
    u_v[row] = 4294967295u;
  }
  u->row_count = WI_SUM_ROWS;

  // tec: groups of three rows, each group's sum is 50331645, whole but not a float: a float sum of the three rounds. keys are far apart so no dense kernel
  CQL_Table* h = wi_table(database, "h");
  cql_table_add_column(h, cql_column_schema_create(str8_lit("k"), CQL_ColumnType_U32));
  cql_table_add_column(h, cql_column_schema_create(str8_lit("v"), CQL_ColumnType_U32));
  U32* h_k = (U32*)cql_column_alloc_fixed_rows(h->columns[0], WI_HASH_ROWS);
  U32* h_v = (U32*)cql_column_alloc_fixed_rows(h->columns[1], WI_HASH_ROWS);
  for (U64 row = 0; row < WI_HASH_ROWS; row++)
  {
    h_k[row] = (U32)(row / 3) * 1000u;
    h_v[row] = 16777215u;
  }
  h->row_count = WI_HASH_ROWS;
}

internal WI_Outcome
wi_run(Arena* out_arena, CQL_Database* database, WI_Case* c)
{
  WI_Outcome outcome = {0};
  Temp scratch = scratch_begin(&out_arena, 1);
  Arena* arena = scratch.arena;
  String8 sql = str8_cstring(c->sql);

  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql);
  SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  U64 temp_mark = database->temp_table_count;
  gpu_kernel_log_reset();
  PLAN_ExecResult result = plan_run_select(arena, database, ir_query->execution_nodes, NULL);
  outcome.ran = result.supported;
  outcome.kernels = bench_kernel_log_string(out_arena);

  if (outcome.ran)
  {
    U64 count = result.is_materialized ? result.materialized.count : result.rows.count;
    if (c->kind == WI_Kind_RowCount)
    {
      outcome.value = count;
    }
    else if (c->kind == WI_Kind_Value)
    {
      outcome.value = (count > 0 && result.is_materialized) ? (U64)result.materialized.columns[0].numeric_values[0] : max_U64;
    }
    else if (result.is_materialized)
    {
      // tec: these two read the table as stored, a materialized result has already put every value through a double
      outcome.value = max_U64;
    }
    else
    {
      CQL_Table* table = result.rows.tables[0];
      U64* rows = result.rows.row_indices[0];
      U64* k = (U64*)cql_table_find_column(table, str8_lit("k"))->data;
      U64 value = 0;
      for (U64 i = 0; i < count; i++)
      {
        U64 row = rows ? rows[i] : i;
        if (c->kind == WI_Kind_ProjectK)
        {
          value += k[row] - WI_TWO_53;
        }
        else if (row != i)
        {
          value += 1;
        }
      }
      outcome.value = value;
    }
  }

  cql_database_release_temp_tables_from(database, temp_mark);
  scratch_end(scratch);
  return outcome;
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

  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    g_wi_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  printf("%llu rows\n", g_wi_rows);
  
  CQL_Database* database = cql_database_alloc(str8_lit("wide_db"));
  cql_add_database(database);
  wi_load_tables(database);

  WI_Case cases[] =
  {
    { "SELECT COUNT(*) FROM w WHERE k = 9007199254740993;", WI_Kind_Value, 1, 0 },
    { "SELECT COUNT(*) FROM w WHERE k > 9007199254740993;", WI_Kind_Value, g_wi_rows - 2, 0 },
    { "SELECT COUNT(*) FROM w WHERE k > 9007199254741000;", WI_Kind_Value, g_wi_rows - 9, 0 },
    { "SELECT COUNT(*) FROM w WHERE s < -9007199254740993;", WI_Kind_Value, g_wi_rows - 2, 0 },
    { "SELECT COUNT(*) FROM w WHERE k = s;", WI_Kind_Value, 0, 0 },
    { "SELECT COUNT(*) FROM w JOIN d ON w.k = d.k;", WI_Kind_Value, g_wi_rows / 2, 0 },
    { "SELECT k, COUNT(*) FROM w GROUP BY k;", WI_Kind_RowCount, g_wi_rows, 0 },
    { "SELECT g FROM w ORDER BY k;", WI_Kind_OrderG, 0, 0 },
    { "SELECT MAX(k) FROM w;", WI_Kind_Value, WI_TWO_53 + g_wi_rows - 1, 0 },
    { "SELECT COUNT(*) FROM c WHERE k < 9007199254740993;", WI_Kind_Value, g_wi_rows, 0 },
    { "SELECT k FROM w WHERE g = 7;", WI_Kind_ProjectK, 7, 1 },
    { "SELECT k FROM w WHERE g < 100;", WI_Kind_ProjectK, 99 * 100 / 2, 1 },
    { "SELECT k, g FROM w WHERE g < 12 ORDER BY g;", WI_Kind_ProjectK, 66, 1 },
    { "SELECT COUNT(*) FROM c WHERE k = 4503599627370497;", WI_Kind_Value, 1, 1 },
    { "SELECT COUNT(*) FROM c WHERE k > 4503599627370596;", WI_Kind_Value, g_wi_rows - 101, 1 },
    { "SELECT k, COUNT(*) FROM c GROUP BY k;", WI_Kind_RowCount, g_wi_rows, 1 },
    { "SELECT MAX(k) FROM c;", WI_Kind_Value, WI_TWO_53 / 2 + g_wi_rows - 1, 1 },
    { "SELECT COUNT(*) FROM p JOIN q ON p.a = q.a AND p.b = q.b;", WI_Kind_Value, g_wi_rows / 2, 1 },
    { "SELECT SUM(v) FROM u;", WI_Kind_Value, WI_SUM_ROWS * 4294967295ull, 0 },
    { "SELECT SUM(v), COUNT(*) FROM u;", WI_Kind_Value, WI_SUM_ROWS * 4294967295ull, 0 },
    { "SELECT SUM(v) FROM u WHERE g < 1000000;", WI_Kind_Value, 1000000ull * 4294967295ull, 1 },
    { "SELECT SUM(v), k FROM h GROUP BY k;", WI_Kind_Value, 50331645, 1 },
  };

  U64 failures = 0;
  U64 wrong_unchecked = 0;
  for (U64 i = 0; i < ArrayCount(cases); i++)
  {
    WI_Case* c = &cases[i];
    Temp scratch = scratch_begin(0, 0);
    settings_load_from_string(str8_lit("QE_WIDE_INT_CHECK:0"));
    WI_Outcome unchecked = wi_run(scratch.arena, database, c);
    settings_load_from_string(str8_lit("QE_WIDE_INT_CHECK:1"));
    WI_Outcome checked = wi_run(scratch.arena, database, c);

    B32 unchecked_right = unchecked.ran && unchecked.value == c->expected;
    B32 checked_right = checked.ran && checked.value == c->expected;
    B32 ok = checked_right || (!checked.ran && !c->must_run);
    if (!ok)
    {
      failures += 1;
    }
    if (unchecked.ran && !unchecked_right)
    {
      wrong_unchecked += 1;
    }

    printf("%-4s %-62s expected %20llu | check off: %-6s %20llu | check on: %-8s\n", ok ? "ok" : "FAIL", c->sql, c->expected,
           !unchecked.ran ? "error" : (unchecked_right ? "right" : "WRONG"), unchecked.value,
           !checked.ran ? "rejected" : (checked_right ? "right" : "WRONG"));
    printf("     kernels with the check off: %.*s\n", str8_varg(unchecked.kernels));
    scratch_end(scratch);
  }
  printf("check off: %llu of %llu queries answered wrongly\n", wrong_unchecked, (U64)ArrayCount(cases));
  printf("check on: %llu of %llu queries failed the test\n", failures, (U64)ArrayCount(cases));

  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0)
  {
    exit(1);
  }
}
