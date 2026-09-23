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
sq_generate_rows(Arena* arena, U64 row_count, U64 seed)
{
  Bench_Row* rows = push_array(arena, Bench_Row, row_count);
  Bench_Rng rng = {seed};
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
sq_add_case(Bench_QueryCase* cases, U64* n, char* label, String8 sql)
{
  cases[*n].label = str8_cstring(label);
  cases[*n].gdb_sql = sql;
  cases[*n].sqlite_sql = sql;
  cases[*n].duckdb_sql = sql;
  (*n)++;
}

internal QE_TraceStrategy
sq_get_strategy(Arena* arena, GDB_Database* database, String8 sql_text)
{
  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;
  
  QE_TraceCtx* trace = qe_trace_ctx_alloc(arena);
  U64 temp_mark = database->temp_table_count;
  plan_run_select(arena, database, select_node, trace);
  gdb_database_release_temp_tables_from(database, temp_mark);
  
  // tec: the outermost scan is recorded last
  QE_TraceStrategy strategy = QE_TraceStrategy_None;
  for (QE_NodeTrace* nt = trace->records; nt; nt = nt->next)
  {
    if (nt->node_type == PLAN_NodeType_Filter || nt->node_type == PLAN_NodeType_Scan) strategy = nt->scan.strategy;
  }
  return strategy;
}

internal void
sq_check_strategy(Bench_Report* report, char* label, QE_TraceStrategy actual, QE_TraceStrategy expected)
{
  B32 ok = (actual == expected);
  printf("  %-48s strategy=%d (expected %d) %s\n", label, actual, expected, ok ? "OK" : "FAIL");
  if (!ok) bench_report_warn(report, "'%s' expected scan strategy %d, got %d", label, expected, actual);
}

// tec: a query the engine has to refuse without crashing
internal void
sq_expect_unsupported(Arena* arena, Bench_Report* report, GDB_Database* database, char* label, char* sql)
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

internal B32
sq_seed_table(Arena* arena, GDB_Database* database, sqlite3* sqlite_db, duckdb_connection duckdb_conn, String8 table_name, Bench_Row* rows, U64 row_count)
{
  String8 csv_path = push_str8f(arena, "bench_data/subquery_%.*s.csv", str8_varg(table_name));
  bench_write_csv(csv_path, rows, row_count);
  
  GDB_Table* table = gdb_table_import_csv_streaming(database, table_name, csv_path);
  gdb_database_add_table(database, table);
  
  bench_sqlite_create_schema(sqlite_db, table_name);
  bench_sqlite_bulk_insert(sqlite_db, table_name, rows, row_count);
  bench_duckdb_create_schema(duckdb_conn, table_name);
  bench_duckdb_bulk_insert(duckdb_conn, table_name, rows, row_count);
  return 1;
}

internal void
sq_run_suite(Arena* arena, Bench_Report* report, U64 bench_rows, char* label)
{
  printf("\n########## subquery suite: %s (%llu rows) ##########\n", label, bench_rows);
  bench_report_section(report, "subquery suite: %s (%llu rows)", label, bench_rows);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  
  GDB_Database* database = gdb_database_alloc(str8_lit("subquery_db"));
  gdb_add_database(database);
  
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  duckdb_open(NULL, &duckdb_db);
  duckdb_connect(duckdb_db, &duckdb_conn);
  
  Bench_Row* bench = sq_generate_rows(scratch.arena, bench_rows, 0x5B0E5EEDULL);
  Bench_Row* dim = sq_generate_rows(scratch.arena, 40, 0xD1D1D1D1ULL);
  sq_seed_table(scratch.arena, database, sqlite_db, duckdb_conn, str8_lit("bench"), bench, bench_rows);
  sq_seed_table(scratch.arena, database, sqlite_db, duckdb_conn, str8_lit("dim"), dim, 40);
  
  Bench_QueryCase cases[64];
  U64 n = 0;
  
  //- tec: scalar subqueries
  sq_add_case(cases, &n, "scalar: > avg", str8_lit("SELECT id FROM bench WHERE value > (SELECT AVG(value) FROM bench);"));
  sq_add_case(cases, &n, "scalar: = max", str8_lit("SELECT id, value FROM bench WHERE value = (SELECT MAX(value) FROM bench);"));
  sq_add_case(cases, &n, "scalar: = min id of other table", str8_lit("SELECT id FROM bench WHERE id = (SELECT MIN(id) FROM dim);"));
  sq_add_case(cases, &n, "scalar: zero rows", str8_lit("SELECT id FROM bench WHERE value > (SELECT value FROM bench WHERE id < 0);"));
  sq_add_case(cases, &n, "scalar: on the left", str8_lit("SELECT id FROM bench WHERE (SELECT MAX(value) FROM dim) > value;"));
  sq_add_case(cases, &n, "scalar: constant vs constant", str8_lit("SELECT id FROM bench WHERE (SELECT COUNT(*) FROM dim) > 10 AND id < 25;"));
  sq_add_case(cases, &n, "scalar: string", str8_lit("SELECT id FROM bench WHERE name = (SELECT name FROM bench WHERE id = 5);"));
  sq_add_case(cases, &n, "scalar: in HAVING", str8_lit("SELECT name, COUNT(*) AS c FROM bench GROUP BY name HAVING COUNT(*) > (SELECT COUNT(*) FROM dim);"));
  sq_add_case(cases, &n, "scalar: between two aggregates", str8_lit("SELECT id FROM bench WHERE value >= (SELECT MIN(value) FROM dim) AND value <= (SELECT MAX(value) FROM dim);"));
  
  //- tec: IN lists
  sq_add_case(cases, &n, "in list: numbers", str8_lit("SELECT id FROM bench WHERE id IN (5, 17, 42, 999);"));
  sq_add_case(cases, &n, "not in list: numbers", str8_lit("SELECT id FROM bench WHERE id NOT IN (5, 17, 42);"));
  sq_add_case(cases, &n, "in list: strings", str8_lit("SELECT id FROM bench WHERE name IN ('kilo', 'lima', 'zzz');"));
  sq_add_case(cases, &n, "not in list: strings", str8_lit("SELECT id FROM bench WHERE name NOT IN ('kilo', 'lima');"));
  sq_add_case(cases, &n, "in list: single item", str8_lit("SELECT id FROM bench WHERE id IN (7);"));
  sq_add_case(cases, &n, "in list: combined with or", str8_lit("SELECT id FROM bench WHERE id IN (1, 2, 3) OR value > 99000;"));
  sq_add_case(cases, &n, "in list: combined with and", str8_lit("SELECT id FROM bench WHERE id IN (1, 2, 3, 4, 5, 6) AND value > 50000;"));
  sq_add_case(cases, &n, "in list: in having", str8_lit("SELECT name, COUNT(*) AS c FROM bench GROUP BY name HAVING name IN ('alpha', 'bravo', 'zulu');"));
  
  // tec: longer than QE_IN_LIST_GPU_MAX_ITEMS, so the scan runs on the CPU
  String8 long_list = {0};
  {
    String8List parts = {0};
    for (U64 i = 0; i < 100; i++) str8_list_pushf(scratch.arena, &parts, "%llu", (i * 7) % (bench_rows + 20) + 1);
    long_list = str8_list_join(scratch.arena, &parts, &(StringJoin){.sep = str8_lit(", ")});
  }
  String8 long_in_sql = push_str8f(scratch.arena, "SELECT id FROM bench WHERE id IN (%.*s);", str8_varg(long_list));
  String8 long_not_in_sql = push_str8f(scratch.arena, "SELECT id FROM bench WHERE id NOT IN (%.*s);", str8_varg(long_list));
  sq_add_case(cases, &n, "in list: 100 items (CPU)", long_in_sql);
  sq_add_case(cases, &n, "not in list: 100 items (CPU)", long_not_in_sql);
  
  //- tec: IN (subquery)
  sq_add_case(cases, &n, "in subquery: numeric", str8_lit("SELECT id FROM bench WHERE id IN (SELECT id FROM dim);"));
  sq_add_case(cases, &n, "not in subquery: numeric", str8_lit("SELECT id FROM bench WHERE id NOT IN (SELECT id FROM dim WHERE id > 10);"));
  sq_add_case(cases, &n, "in subquery: string", str8_lit("SELECT id FROM bench WHERE name IN (SELECT name FROM dim WHERE id < 6);"));
  sq_add_case(cases, &n, "in subquery: aggregate + having", str8_lit("SELECT id FROM bench WHERE name IN (SELECT name FROM dim GROUP BY name HAVING COUNT(*) > 2);"));
  sq_add_case(cases, &n, "in subquery: empty result", str8_lit("SELECT id FROM bench WHERE id IN (SELECT id FROM dim WHERE id < 0);"));
  sq_add_case(cases, &n, "not in subquery: empty result", str8_lit("SELECT id FROM bench WHERE id NOT IN (SELECT id FROM dim WHERE id < 0) AND id < 30;"));
  sq_add_case(cases, &n, "in subquery: > 32 rows (CPU)", str8_lit("SELECT id FROM bench WHERE id IN (SELECT id FROM bench WHERE id < 200);"));
  sq_add_case(cases, &n, "in subquery: nested", str8_lit("SELECT id FROM bench WHERE id IN (SELECT id FROM dim WHERE value > (SELECT AVG(value) FROM dim));"));
  
  //- tec: EXISTS
  sq_add_case(cases, &n, "exists: true", str8_lit("SELECT id FROM bench WHERE EXISTS (SELECT id FROM dim WHERE id > 10) AND id < 20;"));
  sq_add_case(cases, &n, "exists: false", str8_lit("SELECT id FROM bench WHERE EXISTS (SELECT id FROM dim WHERE id < 0);"));
  sq_add_case(cases, &n, "not exists: true", str8_lit("SELECT id FROM bench WHERE NOT EXISTS (SELECT id FROM dim WHERE id < 0) AND id < 20;"));
  sq_add_case(cases, &n, "not exists: false", str8_lit("SELECT id FROM bench WHERE NOT EXISTS (SELECT id FROM dim WHERE id > 10);"));
  sq_add_case(cases, &n, "exists: with aggregate select", str8_lit("SELECT id FROM bench WHERE EXISTS (SELECT COUNT(*) FROM dim WHERE id < 0) AND id < 10;"));
  
  //- tec: combined with CTEs / derived tables
  sq_add_case(cases, &n, "derived + in subquery", str8_lit("SELECT id FROM (SELECT id, value FROM bench WHERE id IN (SELECT id FROM dim)) AS t WHERE value > 1000;"));
  sq_add_case(cases, &n, "cte + scalar subquery over it", str8_lit("WITH t AS (SELECT id, value FROM bench WHERE id < 500) SELECT id FROM t WHERE value > (SELECT AVG(value) FROM t);"));
  sq_add_case(cases, &n, "cte in an in subquery", str8_lit("WITH t AS (SELECT id FROM dim WHERE id < 20) SELECT id FROM bench WHERE id IN (SELECT id FROM t);"));
  sq_add_case(cases, &n, "subquery over derived table", str8_lit("SELECT id FROM bench WHERE value > (SELECT AVG(value) FROM (SELECT value FROM dim WHERE id < 30) AS d);"));
  
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
  
  //- tec: which scan path an IN list takes
  sq_check_strategy(report, "in list, 4 items (expect GPU)",
                    sq_get_strategy(scratch.arena, database, str8_lit("SELECT id FROM bench WHERE id IN (5, 17, 42, 999);")), QE_TraceStrategy_GpuScan);
  sq_check_strategy(report, "in list, 100 items (expect CPU)",
                    sq_get_strategy(scratch.arena, database, long_in_sql), QE_TraceStrategy_CpuScan);
  sq_check_strategy(report, "in subquery, 199 rows (expect CPU)",
                    sq_get_strategy(scratch.arena, database, str8_lit("SELECT id FROM bench WHERE id IN (SELECT id FROM bench WHERE id < 200);")), QE_TraceStrategy_CpuScan);
  
  //- tec: things that must be refused
  sq_expect_unsupported(scratch.arena, report, database, "correlated exists",
                        "SELECT id FROM bench b WHERE EXISTS (SELECT id FROM dim WHERE dim.value > b.value);");
  sq_expect_unsupported(scratch.arena, report, database, "correlated scalar",
                        "SELECT id FROM bench WHERE value > (SELECT AVG(value) FROM dim WHERE dim.id = bench.id);");
  sq_expect_unsupported(scratch.arena, report, database, "scalar returning many rows",
                        "SELECT id FROM bench WHERE value > (SELECT value FROM bench WHERE id < 5);");
  sq_expect_unsupported(scratch.arena, report, database, "scalar returning two columns",
                        "SELECT id FROM bench WHERE value > (SELECT id, value FROM dim WHERE id = 1);");
  sq_expect_unsupported(scratch.arena, report, database, "in subquery returning two columns",
                        "SELECT id FROM bench WHERE id IN (SELECT id, value FROM dim);");
  
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
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite vs duckdb - subqueries and IN lists");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  
  sq_run_suite(arena, report, 2000, "small");
  sq_run_suite(arena, report, 6000, "dict_eligible");
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/subquery_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
