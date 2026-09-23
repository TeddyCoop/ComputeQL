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

// tec: 16 distinct names regardless of row_count
internal Bench_Row*
cte_generate_rows(Arena* arena, U64 row_count)
{
  Bench_Row* rows = push_array(arena, Bench_Row, row_count);
  Bench_Rng rng = {0xC7E5EED5ULL};
  U64 word_count = ArrayCount(g_bench_words);
  for (U64 i = 0; i < row_count; i++)
  {
    rows[i].id = (U32)(i + 1);
    rows[i].name = str8_cstring(g_bench_words[bench_rng_next(&rng) % word_count]);
    U64 vraw = bench_rng_next(&rng) % 10000000ULL;
    rows[i].value = (F64)vraw / 100.0;
  }
  return rows;
}

internal void
cte_add_case(Bench_QueryCase* cases, U64* n, char* label, char* sql)
{
  cases[*n].label = str8_cstring(label);
  cases[*n].gdb_sql = str8_cstring(sql);
  cases[*n].sqlite_sql = cases[*n].gdb_sql;
  cases[*n].duckdb_sql = cases[*n].gdb_sql;
  (*n)++;
}

internal void
cte_run_suite(Arena* arena, Bench_Report* report, U64 row_count, char* label)
{
  printf("\n########## cte / derived table suite: %s (%llu rows) ##########\n", label, row_count);
  bench_report_section(report, "cte / derived table suite: %s (%llu rows)", label, row_count);
  
  String8 table_name = str8_lit("bench");
  Temp scratch = scratch_begin(&arena, 1);
  
  Bench_Row* rows = cte_generate_rows(scratch.arena, row_count);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = push_str8f(scratch.arena, "bench_data/cte_%s.csv", label);
  bench_write_csv(csv_path, rows, row_count);
  
  GDB_Database* database = gdb_database_alloc(str8_lit("cte_db"));
  gdb_add_database(database);
  GDB_Table* table = gdb_table_import_csv_streaming(database, table_name, csv_path);
  gdb_database_add_table(database, table);
  
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  bench_sqlite_create_schema(sqlite_db, table_name);
  bench_sqlite_bulk_insert(sqlite_db, table_name, rows, row_count);
  
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  duckdb_open(NULL, &duckdb_db);
  duckdb_connect(duckdb_db, &duckdb_conn);
  bench_duckdb_create_schema(duckdb_conn, table_name);
  bench_duckdb_bulk_insert(duckdb_conn, table_name, rows, row_count);
  
  Bench_QueryCase cases[40];
  U64 n = 0;
  
  //- tec: derived tables
  cte_add_case(cases, &n, "derived: filter over filter",
               "SELECT id, value FROM (SELECT id, value FROM bench WHERE id > 100) AS t WHERE value > 50000;");
  cte_add_case(cases, &n, "derived: bare alias (no AS)",
               "SELECT id FROM (SELECT id, value FROM bench WHERE value > 90000) t;");
  cte_add_case(cases, &n, "derived: count over derived",
               "SELECT COUNT(*) AS n FROM (SELECT id FROM bench WHERE value > 50000) AS t;");
  cte_add_case(cases, &n, "derived: aggregate inside, filter outside",
               "SELECT name, total FROM (SELECT name, SUM(value) AS total FROM bench GROUP BY name) AS t WHERE total > 0;");
  cte_add_case(cases, &n, "derived: aggregate inside, aggregate outside",
               "SELECT COUNT(*) AS groups, SUM(c) AS rows_total FROM (SELECT name, COUNT(*) AS c FROM bench GROUP BY name) AS t;");
  cte_add_case(cases, &n, "derived: two deep",
               "SELECT id FROM (SELECT id FROM (SELECT id, value FROM bench WHERE value > 20000) AS a WHERE id < 1500) AS b;");
  cte_add_case(cases, &n, "derived: order by + limit inside",
               "SELECT id FROM (SELECT id, value FROM bench ORDER BY value DESC LIMIT 10) AS t;");
  cte_add_case(cases, &n, "derived: string column passes through",
               "SELECT name, id FROM (SELECT name, id FROM bench WHERE id < 200) AS t WHERE name > 'kilo';");
  cte_add_case(cases, &n, "derived: joined to base table",
               "SELECT b.id, t.c FROM bench b JOIN (SELECT name, COUNT(*) AS c FROM bench GROUP BY name) t ON b.name = t.name WHERE b.id < 300;");
  cte_add_case(cases, &n, "derived: select star",
               "SELECT * FROM (SELECT id, name FROM bench WHERE id < 50) AS t;");
  cte_add_case(cases, &n, "derived: empty result",
               "SELECT id FROM (SELECT id FROM bench WHERE id < 0) AS t;");
  
  //- tec: CTEs
  cte_add_case(cases, &n, "cte: single",
               "WITH t AS (SELECT id, value FROM bench WHERE value > 50000) SELECT id FROM t WHERE id < 1000;");
  cte_add_case(cases, &n, "cte: aggregate body",
               "WITH t AS (SELECT name, COUNT(*) AS c FROM bench GROUP BY name) SELECT name, c FROM t WHERE c > 0;");
  cte_add_case(cases, &n, "cte: second reads first",
               "WITH a AS (SELECT name, value FROM bench WHERE value > 10000), b AS (SELECT name, SUM(value) AS s FROM a GROUP BY name) SELECT name, s FROM b;");
  cte_add_case(cases, &n, "cte: used twice (self join)",
               "WITH t AS (SELECT name, COUNT(*) AS c FROM bench GROUP BY name) SELECT x.name, x.c FROM t x JOIN t y ON x.name = y.name;");
  cte_add_case(cases, &n, "cte: joined to base table",
               "WITH t AS (SELECT name, COUNT(*) AS c FROM bench GROUP BY name) SELECT b.id, t.c FROM bench b JOIN t ON b.name = t.name WHERE b.id < 300;");
  // tec: sqlite reads 'WITH bench AS (select ... from bench)' as circular, so shadowing isn't covered here
  cte_add_case(cases, &n, "cte: aggregate with having over it",
               "WITH t AS (SELECT name, value FROM bench WHERE id < 800) SELECT name, COUNT(*) AS c FROM t GROUP BY name HAVING COUNT(*) > 30;");
  cte_add_case(cases, &n, "cte: select star",
               "WITH t AS (SELECT * FROM bench WHERE id < 50) SELECT * FROM t;");
  cte_add_case(cases, &n, "cte: order by + limit outside",
               "WITH t AS (SELECT id, value FROM bench WHERE value > 1000) SELECT id FROM t ORDER BY value DESC LIMIT 5;");
  cte_add_case(cases, &n, "cte over derived",
               "WITH t AS (SELECT id FROM (SELECT id, value FROM bench WHERE value > 30000) AS d WHERE id < 900) SELECT COUNT(*) AS n FROM t;");
  
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
  
  // tec: nothing should be left registered on the database afterwards
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
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite vs duckdb - CTEs and derived tables");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  
  // tec: below GDB_DICT_ENCODE_MIN_ROWS, string join keys go through the raw-bytes path
  cte_run_suite(arena, report, 2000, "raw_strings");
  // tec: past it, name is dictionary encoded
  cte_run_suite(arena, report, 6000, "dict_strings");
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/cte_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
