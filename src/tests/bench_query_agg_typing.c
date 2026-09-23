// compute_ql: aggregate output typing correctness.
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(1)

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "thread_pool/thread_pool.h"
#include "settings/settings.h"
#include "gdb/gdb_inc.h"
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
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "optimizer/optimizer_inc.c"
#include "planner/planner.c"
#include "application.c"

#include "tests/bench_common.h"

internal PLAN_Materialized
at_run_gdb(Arena* arena, GDB_Database* database, String8 sql_text)
{
  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;
  ir_expand_star_to_columns(arena, database, select_node);
  PLAN_Node* plan = plan_build_from_select(arena, database, select_node);
  PLAN_ExecResult result = plan_execute(arena, database, plan, select_node, NULL);
  return result.materialized;
}

internal void
at_check_type(Bench_Report* report, String8 label, GDB_ColumnType actual, GDB_ColumnType expected)
{
  B32 ok = (actual == expected);
  printf("  %-32.*s type=%-12.*s expected=%-12.*s %s\n",
         str8_varg(label), str8_varg(string_from_gdb_column_type(actual)),
         str8_varg(string_from_gdb_column_type(expected)), ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "'%.*s' expected type %.*s, got %.*s",
                       str8_varg(label), str8_varg(string_from_gdb_column_type(expected)), str8_varg(string_from_gdb_column_type(actual)));
  }
}

internal void
at_check_value(Bench_Report* report, String8 label, F64 actual, F64 expected)
{
  B32 ok = (actual == expected);
  printf("  %-32.*s value=%12.2f expected=%12.2f %s\n", str8_varg(label), actual, expected, ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "'%.*s' expected value %.2f, got %.2f", str8_varg(label), expected, actual);
  }
}

internal void
at_run_suite(Arena* arena, Bench_Report* report)
{
  printf("\n########## aggregate output typing suite ##########\n");
  bench_report_section(report, "aggregate output typing suite");

  Temp scratch = scratch_begin(&arena, 1);

  U32 amounts[6] = { 500, 100, 900, 300, 700, 200 };
  U64 row_count = ArrayCount(amounts);

  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,amount\n");
  U64 expected_sum = 0;
  U32 expected_min = max_U32, expected_max = 0;
  for (U64 i = 0; i < row_count; i++)
  {
    str8_list_pushf(scratch.arena, &lines, "%llu,%u\n", i + 1, amounts[i]);
    expected_sum += amounts[i];
    if (amounts[i] < expected_min) expected_min = amounts[i];
    if (amounts[i] > expected_max) expected_max = amounts[i];
  }
  String8 content = str8_list_join(scratch.arena, &lines, 0);

  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = str8_lit("bench_data/agg_typing_fact.csv");
  OS_Handle file = os_file_open(OS_AccessFlag_Write, csv_path);
  os_file_write(file, r1u64(0, content.size), content.str);
  os_file_close(file);

  GDB_Database* database = gdb_database_alloc(str8_lit("agg_typing_db"));
  gdb_add_database(database);
  GDB_Table* fact_table = gdb_table_import_csv_streaming(database, str8_lit("fact"), csv_path);
  gdb_database_add_table(database, fact_table);

  PLAN_Materialized m = at_run_gdb(scratch.arena, database,
    str8_lit("SELECT MIN(amount), MAX(amount), SUM(amount), AVG(amount) FROM fact;"));

  if (m.column_count < 4 || m.count == 0)
  {
    log_error("agg typing suite: expected 4 output columns and 1 row, got column_count=%llu count=%llu", m.column_count, m.count);
  }
  else
  {
    at_check_type(report, str8_lit("MIN(amount) type"), m.columns[0].type, GDB_ColumnType_U32);
    at_check_value(report, str8_lit("MIN(amount) value"), m.columns[0].numeric_values[0], (F64)expected_min);

    at_check_type(report, str8_lit("MAX(amount) type"), m.columns[1].type, GDB_ColumnType_U32);
    at_check_value(report, str8_lit("MAX(amount) value"), m.columns[1].numeric_values[0], (F64)expected_max);

    at_check_type(report, str8_lit("SUM(amount) type"), m.columns[2].type, GDB_ColumnType_U64);
    at_check_value(report, str8_lit("SUM(amount) value"), m.columns[2].numeric_values[0], (F64)expected_sum);

    at_check_type(report, str8_lit("AVG(amount) type"), m.columns[3].type, GDB_ColumnType_F64);
    at_check_value(report, str8_lit("AVG(amount) value"), m.columns[3].numeric_values[0], (F64)expected_sum / (F64)row_count);
  }

  // tec: COUNT/COUNT(*) must be unaffected by this change - still always U64
  {
    PLAN_Materialized m2 = at_run_gdb(scratch.arena, database, str8_lit("SELECT COUNT(*) FROM fact;"));
    if (m2.column_count > 0 && m2.count > 0)
    {
      at_check_type(report, str8_lit("COUNT(*) type"), m2.columns[0].type, GDB_ColumnType_U64);
      at_check_value(report, str8_lit("COUNT(*) value"), m2.columns[0].numeric_values[0], (F64)row_count);
    }
  }

  scratch_end(scratch);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();
  gdb_init();
  gpu_init();

  Arena* arena = arena_alloc(.reserve_size = MB(256), .commit_size = MB(16));

  Bench_Report* report = bench_report_alloc(arena, "compute_ql aggregate output typing correctness");

  at_run_suite(arena, report);

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/agg_typing_report.md"));

  arena_release(arena);

  log_release();

  ProfEnd();
  ProfEndCapture();
}
