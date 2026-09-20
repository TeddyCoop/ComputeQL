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
#include "planner/planner.c"
#include "application.c"

#include "tests/bench_common.h"

typedef struct APP_ExpectedRow APP_ExpectedRow;
struct APP_ExpectedRow
{
  F64 a;
  F64 b;
};

internal B32
app_contains(String8 haystack, String8 needle)
{
  return str8_find_needle(haystack, 0, needle, 0) < haystack.size;
}

internal void
app_report(Bench_Report* report, char* label, B32 ok, char* detail)
{
  printf("  %-56s %s%s%s\n", label, ok ? "OK" : "FAIL", detail ? " " : "", detail ? detail : "");
  if (!ok) bench_report_warn(report, "'%s' failed%s%s", label, detail ? ": " : "", detail ? detail : "");
}

// tec: also checks that no temp table is left on the database afterwards
internal void
app_expect_numbers(Arena* arena, Bench_Report* report, GDB_Database** database, char* label, char* sql,
                   U64 expected_columns, APP_ExpectedRow* expected, U64 expected_count)
{
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(1));
  APP_ResultSet result_set = {0};
  app_execute_query_capture(query_arena, str8_cstring(sql), database, &result_set);
  
  B32 ok = result_set.valid && result_set.row_count == expected_count && result_set.column_count == expected_columns;
  char detail[160] = {0};
  if (!ok) snprintf(detail, sizeof(detail), "valid=%d rows=%llu (want %llu) cols=%llu (want %llu)", result_set.valid, result_set.row_count, expected_count, result_set.column_count, expected_columns);
  
  for (U64 r = 0; ok && r < expected_count; r++)
  {
    F64 a = result_set.cell_numeric[r * result_set.column_count + 0];
    if (a != expected[r].a) { ok = 0; snprintf(detail, sizeof(detail), "row %llu col 0 = %g, want %g", r, a, expected[r].a); }
    if (ok && expected_columns > 1)
    {
      F64 b = result_set.cell_numeric[r * result_set.column_count + 1];
      if (b != expected[r].b) { ok = 0; snprintf(detail, sizeof(detail), "row %llu col 1 = %g, want %g", r, b, expected[r].b); }
    }
  }
  
  if (ok && (*database)->temp_table_count != 0)
  {
    ok = 0;
    snprintf(detail, sizeof(detail), "%llu temp table(s) left registered", (*database)->temp_table_count);
  }
  
  app_report(report, label, ok, ok ? 0 : detail);
  arena_release(query_arena);
}

internal void
app_run_suite(Arena* arena, Bench_Report* report)
{
  printf("\n########## sql surface through the app path ##########\n");
  bench_report_section(report, "sql surface through the app path");
  
  Temp scratch = scratch_begin(&arena, 1);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  
  // tec: ids 1..10, names alpha..juliet, value = id * 1.5
  Bench_Row rows[10];
  for (U64 i = 0; i < 10; i++)
  {
    rows[i].id = (U32)(i + 1);
    rows[i].name = str8_cstring(g_bench_words[i]);
    rows[i].value = (F64)(i + 1) * 1.5;
  }
  String8 csv_path = str8_lit("bench_data/sql_surface_app.csv");
  bench_write_csv(csv_path, rows, 10);
  
  GDB_Database* database = gdb_database_alloc(str8_lit("sql_surface_app_db"));
  gdb_add_database(database);
  GDB_Table* table = gdb_table_import_csv_streaming(database, str8_lit("t"), csv_path);
  gdb_database_add_table(database, table);
  
  //- tec: CTEs
  {
    APP_ExpectedRow expected[] = { {8, 12}, {9, 13.5}, {10, 15} };
    app_expect_numbers(scratch.arena, report, &database, "cte, row-set result",
                       "WITH big AS (SELECT id, value FROM t WHERE id > 7) SELECT id, value FROM big ORDER BY id;", 2, expected, 3);
  }
  {
    APP_ExpectedRow expected[] = { {9, 0}, {10, 0} };
    app_expect_numbers(scratch.arena, report, &database, "two ctes, second reads the first",
                       "WITH a AS (SELECT id FROM t WHERE id > 5), b AS (SELECT id FROM a WHERE id > 8) SELECT id FROM b ORDER BY id;", 1, expected, 2);
  }
  {
    APP_ExpectedRow expected[] = { {3, 0} };
    app_expect_numbers(scratch.arena, report, &database, "cte with an aggregate",
                       "WITH c AS (SELECT COUNT(*) AS n FROM t WHERE id < 4) SELECT n FROM c;", 1, expected, 1);
  }
  
  //- tec: derived tables
  {
    APP_ExpectedRow expected[] = { {1, 0}, {2, 0}, {3, 0} };
    app_expect_numbers(scratch.arena, report, &database, "derived table",
                       "SELECT id FROM (SELECT id FROM t WHERE id < 4) AS d ORDER BY id;", 1, expected, 3);
  }
  {
    APP_ExpectedRow expected[] = { {9, 0}, {10, 0} };
    app_expect_numbers(scratch.arena, report, &database, "derived table, ordered + limited inside",
                       "SELECT id FROM (SELECT id, value FROM t ORDER BY value DESC LIMIT 2) AS d ORDER BY id;", 1, expected, 2);
  }
  
  //- tec: subqueries
  {
    APP_ExpectedRow expected[] = { {9, 0}, {10, 0} };
    app_expect_numbers(scratch.arena, report, &database, "in (subquery)",
                       "SELECT id FROM t WHERE id IN (SELECT id FROM t WHERE id > 8) ORDER BY id;", 1, expected, 2);
  }
  {
    APP_ExpectedRow expected[] = { {1, 0}, {2, 0} };
    app_expect_numbers(scratch.arena, report, &database, "not in (subquery)",
                       "SELECT id FROM t WHERE id NOT IN (SELECT id FROM t WHERE id > 2) ORDER BY id;", 1, expected, 2);
  }
  {
    APP_ExpectedRow expected[] = { {10, 15} };
    app_expect_numbers(scratch.arena, report, &database, "scalar subquery",
                       "SELECT id, value FROM t WHERE value = (SELECT MAX(value) FROM t);", 2, expected, 1);
  }
  {
    APP_ExpectedRow expected[] = { {1, 0}, {2, 0}, {3, 0} };
    app_expect_numbers(scratch.arena, report, &database, "exists",
                       "SELECT id FROM t WHERE EXISTS (SELECT id FROM t WHERE id = 5) AND id < 4 ORDER BY id;", 1, expected, 3);
  }
  {
    APP_ExpectedRow expected[] = { {2, 0}, {3, 0} };
    app_expect_numbers(scratch.arena, report, &database, "in (literal list)",
                       "SELECT id FROM t WHERE id IN (2, 3, 99) ORDER BY id;", 1, expected, 2);
  }
  
  //- tec: window functions
  {
    APP_ExpectedRow expected[] = { {1, 10}, {2, 9}, {3, 8} };
    app_expect_numbers(scratch.arena, report, &database, "row_number over (order by id desc)",
                       "SELECT id, ROW_NUMBER() OVER (ORDER BY id DESC) AS rn FROM t ORDER BY id LIMIT 3;", 2, expected, 3);
  }
  {
    APP_ExpectedRow expected[] = { {1, 1.5}, {2, 4.5}, {3, 9} };
    app_expect_numbers(scratch.arena, report, &database, "running sum",
                       "SELECT id, SUM(value) OVER (ORDER BY id) AS s FROM t ORDER BY id LIMIT 3;", 2, expected, 3);
  }
  {
    APP_ExpectedRow expected[] = { {8, 3}, {9, 2}, {10, 1} };
    app_expect_numbers(scratch.arena, report, &database, "window in a derived table, filtered outside",
                       "SELECT id, rn FROM (SELECT id, ROW_NUMBER() OVER (ORDER BY id DESC) AS rn FROM t) AS d WHERE rn <= 3 ORDER BY id;", 2, expected, 3);
  }
  
  //- tec: only the last SELECT of a batch is captured
  {
    APP_ExpectedRow expected[] = { {4, 0} };
    app_expect_numbers(scratch.arena, report, &database, "multi-statement batch, both with ctes",
                       "WITH a AS (SELECT id FROM t WHERE id < 3) SELECT id FROM a; WITH b AS (SELECT id FROM t WHERE id = 4) SELECT id FROM b;", 1, expected, 1);
  }
  
  //- tec: text output (no structured capture)
  {
    Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(1));
    APP_QueryResult result = app_execute_query_capture(query_arena,
                                                       str8_lit("WITH c AS (SELECT id, name FROM t WHERE id = 3) SELECT id, name FROM c;"), &database, NULL);
    B32 ok = app_contains(result.output_text, str8_lit("charlie")) && database->temp_table_count == 0;
    app_report(report, "text output of a cte query", ok, ok ? 0 : "expected 'charlie' in the output and no leaked temp table");
    arena_release(query_arena);
  }
  
  //- tec: a refused query leaves nothing registered and doesn't break the next one
  {
    Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(1));
    APP_ResultSet result_set = {0};
    app_execute_query_capture(query_arena,
                              str8_lit("SELECT id FROM t x WHERE EXISTS (SELECT id FROM t WHERE t.value > x.value);"), &database, &result_set);
    B32 ok = (!result_set.valid || result_set.row_count == 0) && database->temp_table_count == 0;
    app_report(report, "correlated subquery refused, nothing leaked", ok, 0);
    arena_release(query_arena);
  }
  {
    APP_ExpectedRow expected[] = { {1, 0} };
    app_expect_numbers(scratch.arena, report, &database, "a normal query still works afterwards",
                       "SELECT id FROM t WHERE id = 1;", 1, expected, 1);
  }
  
  scratch_end(scratch);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  
  U64 t0 = os_now_microseconds();
  gdb_init();
  gpu_init();
  U64 t1 = os_now_microseconds();
  printf("engine startup (gdb_init + gpu_init, one-time): %.4f ms\n", (F64)(t1 - t0) / 1000.0);
  
  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql - CTEs, subqueries and window functions through the app path");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  
  app_run_suite(arena, report);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/sql_surface_app_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
