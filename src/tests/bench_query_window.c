#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)

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

// tec: `value` has only 20 levels, so ties are common
internal Bench_Row*
win_generate_rows(Arena* arena, U64 row_count, U64 seed)
{
  Bench_Row* rows = push_array(arena, Bench_Row, row_count);
  Bench_Rng rng = {seed};
  U64 word_count = ArrayCount(g_bench_words);
  for (U64 i = 0; i < row_count; i++)
  {
    rows[i].id = (U32)(i + 1);
    rows[i].name = str8_cstring(g_bench_words[bench_rng_next(&rng) % word_count]);
    rows[i].value = (F64)(bench_rng_next(&rng) % 20);
  }
  return rows;
}

internal void
win_add_case(Bench_QueryCase* cases, U64* n, char* label, char* sql)
{
  cases[*n].label = str8_cstring(label);
  cases[*n].gdb_sql = str8_cstring(sql);
  cases[*n].sqlite_sql = cases[*n].gdb_sql;
  cases[*n].duckdb_sql = cases[*n].gdb_sql;
  (*n)++;
}

// tec: `value` is NULL for every 7th id
internal void
win_seed_null_table(Arena* arena, GDB_Database* database, sqlite3* sqlite_db, duckdb_connection duckdb_conn, Bench_Row* rows, U64 row_count)
{
  String8 csv_path = str8_lit("bench_data/window_wn.csv");
  {
    String8List lines = {0};
    str8_list_pushf(arena, &lines, "id,name,value\n");
    for (U64 i = 0; i < row_count; i++)
    {
      if (rows[i].id % 7 == 0) str8_list_pushf(arena, &lines, "%u,%.*s,\n", rows[i].id, str8_varg(rows[i].name));
      else str8_list_pushf(arena, &lines, "%u,%.*s,%.4f\n", rows[i].id, str8_varg(rows[i].name), rows[i].value);
    }
    String8 content = str8_list_join(arena, &lines, 0);
    OS_Handle file = os_file_open(OS_AccessFlag_Write, csv_path);
    if (!os_handle_match(file, os_handle_zero()))
    {
      os_file_write(file, r1u64(0, content.size), content.str);
      os_file_close(file);
    }
  }
  GDB_Table* table = gdb_table_import_csv_streaming(database, str8_lit("wn"), csv_path);
  gdb_database_add_table(database, table);
  
  sqlite3_exec(sqlite_db, "CREATE TABLE wn (id INTEGER, name TEXT, value REAL);", NULL, NULL, NULL);
  sqlite3_exec(sqlite_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(sqlite_db, "INSERT INTO wn (id, name, value) VALUES (?, ?, ?);", -1, &stmt, NULL);
  for (U64 i = 0; i < row_count; i++)
  {
    sqlite3_bind_int64(stmt, 1, (S64)rows[i].id);
    sqlite3_bind_text(stmt, 2, (const char*)rows[i].name.str, (int)rows[i].name.size, SQLITE_TRANSIENT);
    if (rows[i].id % 7 == 0) sqlite3_bind_null(stmt, 3);
    else sqlite3_bind_double(stmt, 3, rows[i].value);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_exec(sqlite_db, "COMMIT;", NULL, NULL, NULL);
  
  duckdb_result result = {0};
  duckdb_query(duckdb_conn, "CREATE TABLE wn (id INTEGER, name VARCHAR, value DOUBLE);", &result);
  duckdb_destroy_result(&result);
  duckdb_appender appender = NULL;
  if (duckdb_appender_create(duckdb_conn, NULL, "wn", &appender) == DuckDBSuccess)
  {
    for (U64 i = 0; i < row_count; i++)
    {
      duckdb_append_int32(appender, (S32)rows[i].id);
      duckdb_append_varchar_length(appender, (const char*)rows[i].name.str, rows[i].name.size);
      if (rows[i].id % 7 == 0) duckdb_append_null(appender);
      else duckdb_append_double(appender, rows[i].value);
      duckdb_appender_end_row(appender);
    }
  }
  duckdb_appender_destroy(&appender);
}

// tec: a query the engine has to refuse without crashing
internal void
win_expect_unsupported(Arena* arena, Bench_Report* report, GDB_Database* database, char* label, char* sql)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8 sql_text = str8_cstring(sql);
  
  SQL_TokenizeResult tok = sql_tokenize_from_text(scratch.arena, sql_text);
  SQL_Node* ast = sql_parse(scratch.arena, tok.tokens, tok.count, sql_text);
  
  B32 refused = 0;
  if (!ast)
  {
    refused = 1;
  }
  else
  {
    IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
    U64 temp_mark = database->temp_table_count;
    PLAN_ExecResult result = plan_run_select(scratch.arena, database, ir_query->execution_nodes, NULL);
    refused = !result.supported;
    gdb_database_release_temp_tables_from(database, temp_mark);
  }
  
  printf("  %-48s refused=%d %s\n", label, refused, refused ? "OK" : "FAIL");
  if (!refused) bench_report_warn(report, "'%s' should have been refused but ran", label);
  scratch_end(scratch);
}

internal void
win_run_suite(Arena* arena, Bench_Report* report, U64 row_count, char* label)
{
  printf("\n########## window function suite: %s (%llu rows) ##########\n", label, row_count);
  bench_report_section(report, "window function suite: %s (%llu rows)", label, row_count);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  
  GDB_Database* database = gdb_database_alloc(str8_lit("window_db"));
  gdb_add_database(database);
  
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  duckdb_open(NULL, &duckdb_db);
  duckdb_connect(duckdb_db, &duckdb_conn);
  
  Bench_Row* rows = win_generate_rows(scratch.arena, row_count, 0x71D0E5EEDULL);
  {
    String8 csv_path = str8_lit("bench_data/window_win.csv");
    bench_write_csv(csv_path, rows, row_count);
    GDB_Table* table = gdb_table_import_csv_streaming(database, str8_lit("win"), csv_path);
    gdb_database_add_table(database, table);
    bench_sqlite_create_schema(sqlite_db, str8_lit("win"));
    bench_sqlite_bulk_insert(sqlite_db, str8_lit("win"), rows, row_count);
    bench_duckdb_create_schema(duckdb_conn, str8_lit("win"));
    bench_duckdb_bulk_insert(duckdb_conn, str8_lit("win"), rows, row_count);
  }
  
  // tec: one row per name, so a join to it keeps every win row
  Bench_Row dimn[16];
  for (U64 i = 0; i < 16; i++)
  {
    dimn[i].id = (U32)(i + 1);
    dimn[i].name = str8_cstring(g_bench_words[i]);
    dimn[i].value = (F64)(i * 3);
  }
  {
    String8 csv_path = str8_lit("bench_data/window_dimn.csv");
    bench_write_csv(csv_path, dimn, 16);
    GDB_Table* table = gdb_table_import_csv_streaming(database, str8_lit("dimn"), csv_path);
    gdb_database_add_table(database, table);
    bench_sqlite_create_schema(sqlite_db, str8_lit("dimn"));
    bench_sqlite_bulk_insert(sqlite_db, str8_lit("dimn"), dimn, 16);
    bench_duckdb_create_schema(duckdb_conn, str8_lit("dimn"));
    bench_duckdb_bulk_insert(duckdb_conn, str8_lit("dimn"), dimn, 16);
  }
  
  win_seed_null_table(scratch.arena, database, sqlite_db, duckdb_conn, rows, row_count);
  
  Bench_QueryCase cases[80];
  U64 n = 0;
  
  //- tec: ranking
  win_add_case(cases, &n, "row_number: partition + order", "SELECT id, ROW_NUMBER() OVER (PARTITION BY name ORDER BY id) AS rn FROM win;");
  win_add_case(cases, &n, "row_number: order only", "SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM win;");
  win_add_case(cases, &n, "row_number: order desc, two keys", "SELECT id, ROW_NUMBER() OVER (PARTITION BY name ORDER BY value DESC, id) AS rn FROM win;");
  win_add_case(cases, &n, "row_number: partition only (any order)", "SELECT COUNT(*) AS n FROM (SELECT ROW_NUMBER() OVER (PARTITION BY name) AS rn FROM win) AS t WHERE rn = 1;");
  win_add_case(cases, &n, "rank: partition + order (ties)", "SELECT id, RANK() OVER (PARTITION BY name ORDER BY value) AS r FROM win;");
  win_add_case(cases, &n, "dense_rank: partition + order (ties)", "SELECT id, DENSE_RANK() OVER (PARTITION BY name ORDER BY value) AS r FROM win;");
  win_add_case(cases, &n, "rank: order desc only", "SELECT id, RANK() OVER (ORDER BY value DESC) AS r FROM win;");
  win_add_case(cases, &n, "rank + dense_rank together", "SELECT id, RANK() OVER (ORDER BY value) AS r, DENSE_RANK() OVER (ORDER BY value) AS d FROM win;");
  
  //- tec: aggregates as window functions
  win_add_case(cases, &n, "sum: whole partition", "SELECT id, SUM(value) OVER (PARTITION BY name) AS s FROM win;");
  win_add_case(cases, &n, "sum: running by unique key", "SELECT id, SUM(value) OVER (PARTITION BY name ORDER BY id) AS s FROM win;");
  win_add_case(cases, &n, "sum: running with peer ties", "SELECT id, SUM(value) OVER (PARTITION BY name ORDER BY value) AS s FROM win;");
  win_add_case(cases, &n, "sum: no partition, running", "SELECT id, SUM(value) OVER (ORDER BY id) AS s FROM win;");
  win_add_case(cases, &n, "sum: integer column", "SELECT id, SUM(id) OVER (PARTITION BY name ORDER BY id) AS s FROM win;");
  win_add_case(cases, &n, "count(*): whole partition", "SELECT id, COUNT(*) OVER (PARTITION BY name) AS c FROM win;");
  win_add_case(cases, &n, "count(col): running", "SELECT id, COUNT(value) OVER (PARTITION BY name ORDER BY id) AS c FROM win;");
  win_add_case(cases, &n, "avg: whole partition", "SELECT id, AVG(value) OVER (PARTITION BY name) AS a FROM win;");
  win_add_case(cases, &n, "avg: running", "SELECT id, AVG(value) OVER (PARTITION BY name ORDER BY id) AS a FROM win;");
  win_add_case(cases, &n, "min: whole partition", "SELECT id, MIN(value) OVER (PARTITION BY name) AS m FROM win;");
  win_add_case(cases, &n, "max: running", "SELECT id, MAX(value) OVER (PARTITION BY name ORDER BY id) AS m FROM win;");
  win_add_case(cases, &n, "count(*): two partition keys", "SELECT id, COUNT(*) OVER (PARTITION BY name, value) AS c FROM win;");
  
  //- tec: defaults are REAL literals, since sqlite would hash an integer default in a REAL column as an integer
  win_add_case(cases, &n, "lag: default offset", "SELECT id, LAG(value) OVER (PARTITION BY name ORDER BY id) AS l FROM win WHERE id > 0 AND id < 100000000 AND value >= 0;");
  win_add_case(cases, &n, "lag: offset 2", "SELECT id, LAG(value, 2) OVER (PARTITION BY name ORDER BY id) AS l FROM win;");
  win_add_case(cases, &n, "lag: with default", "SELECT id, LAG(value, 1, 0.0) OVER (PARTITION BY name ORDER BY id) AS l FROM win;");
  win_add_case(cases, &n, "lead: default offset", "SELECT id, LEAD(value) OVER (PARTITION BY name ORDER BY id) AS l FROM win;");
  win_add_case(cases, &n, "lead: offset 3 with default", "SELECT id, LEAD(value, 3, -1.0) OVER (PARTITION BY name ORDER BY id) AS l FROM win;");
  win_add_case(cases, &n, "lag: string column with default", "SELECT id, LAG(name, 1, 'none') OVER (ORDER BY id) AS l FROM win;");
  win_add_case(cases, &n, "lead: order desc", "SELECT id, LEAD(value, 1, 0.0) OVER (PARTITION BY name ORDER BY id DESC) AS l FROM win;");
  
  //- tec: shapes
  win_add_case(cases, &n, "several windows, different specs",
               "SELECT id, ROW_NUMBER() OVER (PARTITION BY name ORDER BY id) AS a, RANK() OVER (ORDER BY value) AS b, SUM(value) OVER (PARTITION BY name) AS c FROM win;");
  win_add_case(cases, &n, "plain columns beside a window",
               "SELECT id, name, value, ROW_NUMBER() OVER (PARTITION BY name ORDER BY id) AS rn FROM win;");
  win_add_case(cases, &n, "over a filtered row set",
               "SELECT id, ROW_NUMBER() OVER (PARTITION BY name ORDER BY id) AS rn FROM win WHERE id < 200 AND value > 3;");
  win_add_case(cases, &n, "over a joined row set",
               "SELECT a.id, d.value, ROW_NUMBER() OVER (PARTITION BY a.name ORDER BY a.id) AS rn FROM win a JOIN dimn d ON a.name = d.name;");
  win_add_case(cases, &n, "order by the window alias + limit",
               "SELECT id, ROW_NUMBER() OVER (ORDER BY value, id) AS rn FROM win ORDER BY rn LIMIT 10;");
  win_add_case(cases, &n, "order by a column not selected + limit",
               "SELECT ROW_NUMBER() OVER (PARTITION BY name ORDER BY id) AS rn FROM win ORDER BY id LIMIT 20;");
  win_add_case(cases, &n, "top 3 per group (derived table)",
               "SELECT id FROM (SELECT id, ROW_NUMBER() OVER (PARTITION BY name ORDER BY value DESC, id) AS rn FROM win) AS t WHERE rn <= 3;");
  win_add_case(cases, &n, "top 3 per group (cte)",
               "WITH t AS (SELECT id, name, ROW_NUMBER() OVER (PARTITION BY name ORDER BY value DESC, id) AS rn FROM win) SELECT id, name FROM t WHERE rn <= 3;");
  win_add_case(cases, &n, "running total then filter",
               "SELECT id, s FROM (SELECT id, SUM(value) OVER (ORDER BY id) AS s FROM win) AS t WHERE s > 500;");
  win_add_case(cases, &n, "aggregate over a windowed derived table",
               "SELECT COUNT(*) AS n, MAX(rn) AS biggest FROM (SELECT ROW_NUMBER() OVER (PARTITION BY name ORDER BY id) AS rn FROM win) AS t;");
  win_add_case(cases, &n, "window in a subquery predicate",
               "SELECT id FROM win WHERE id IN (SELECT id FROM (SELECT id, ROW_NUMBER() OVER (ORDER BY value DESC, id) AS rn FROM win) AS t WHERE rn <= 5);");
  
  //- tec: NULL values in the argument column
  win_add_case(cases, &n, "nulls: sum whole partition", "SELECT id, SUM(value) OVER (PARTITION BY name) AS s FROM wn;");
  win_add_case(cases, &n, "nulls: running count(col)", "SELECT id, COUNT(value) OVER (PARTITION BY name ORDER BY id) AS c FROM wn;");
  win_add_case(cases, &n, "nulls: running count(*)", "SELECT id, COUNT(*) OVER (PARTITION BY name ORDER BY id) AS c FROM wn;");
  win_add_case(cases, &n, "nulls: avg whole partition", "SELECT id, AVG(value) OVER (PARTITION BY name) AS a FROM wn;");
  win_add_case(cases, &n, "nulls: min / max", "SELECT id, MIN(value) OVER (PARTITION BY name) AS lo, MAX(value) OVER (PARTITION BY name) AS hi FROM wn;");
  win_add_case(cases, &n, "nulls: lag with default", "SELECT id, LAG(value, 1, 99.0) OVER (PARTITION BY name ORDER BY id) AS l FROM wn;");
  win_add_case(cases, &n, "nulls: rank ignores them", "SELECT id, ROW_NUMBER() OVER (PARTITION BY name ORDER BY id) AS rn FROM wn;");
  
  bench_print_table_header(report, label);
  for (U64 i = 0; i < n; i++)
  {
    U64 gdb_rows = 0, gdb_checksum = 0;
    Bench_Stats gdb_stats = bench_run_gdb_query(database, cases[i].gdb_sql, &gdb_rows, &gdb_checksum);
    bench_print_table_row(report, cases[i].label, "gdb", gdb_rows, gdb_checksum, &gdb_stats);
    
    U64 sqlite_rows = 0, sqlite_checksum = 0;
    Bench_Stats sqlite_stats = bench_run_sqlite_query(sqlite_db, cases[i].sqlite_sql, &sqlite_rows, &sqlite_checksum);
    bench_print_table_row(report, cases[i].label, "sqlite", sqlite_rows, sqlite_checksum, &sqlite_stats);
    
    U64 duckdb_rows = 0, duckdb_checksum = 0;
    Bench_Stats duckdb_stats = bench_run_duckdb_query(duckdb_conn, cases[i].duckdb_sql, &duckdb_rows, &duckdb_checksum);
    bench_print_table_row(report, cases[i].label, "duckdb", duckdb_rows, duckdb_checksum, &duckdb_stats);
    
    bench_check_match(report, cases[i].label, "gdb", gdb_rows, gdb_checksum, "sqlite", sqlite_rows, sqlite_checksum);
    bench_check_match(report, cases[i].label, "gdb", gdb_rows, gdb_checksum, "duckdb", duckdb_rows, duckdb_checksum);
  }
  
  //- tec: things that must be refused
  win_expect_unsupported(scratch.arena, report, database, "window + group by",
                         "SELECT name, COUNT(*), ROW_NUMBER() OVER (ORDER BY name) AS rn FROM win GROUP BY name;");
  win_expect_unsupported(scratch.arena, report, database, "window + plain aggregate",
                         "SELECT SUM(value), ROW_NUMBER() OVER (ORDER BY id) AS rn FROM win;");
  win_expect_unsupported(scratch.arena, report, database, "unsupported window function",
                         "SELECT id, NTILE(4) OVER (ORDER BY id) AS q FROM win;");
  win_expect_unsupported(scratch.arena, report, database, "row_number with an argument",
                         "SELECT id, ROW_NUMBER(id) OVER (ORDER BY id) AS q FROM win;");
  win_expect_unsupported(scratch.arena, report, database, "sum over a string column",
                         "SELECT id, SUM(name) OVER (ORDER BY id) AS q FROM win;");
  win_expect_unsupported(scratch.arena, report, database, "malformed over clause",
                         "SELECT id, ROW_NUMBER() OVER ORDER BY id FROM win;");
  
  if (database->temp_table_count != 0)
  {
    printf("  temp tables leaked: %llu FAIL\n", database->temp_table_count);
    bench_report_warn(report, "%llu temp table(s) still registered after the suite", database->temp_table_count);
  }
  
  sqlite3_close(sqlite_db);
  duckdb_disconnect(&duckdb_conn);
  duckdb_close(&duckdb_db);
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
  
  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite vs duckdb - window functions");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  
  win_run_suite(arena, report, 700, "small");
  win_run_suite(arena, report, 6000, "larger");
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/window_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
