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

internal Bench_Row*
scmp_generate_low_cardinality_rows(Arena* arena, U64 row_count)
{
  Bench_Row* rows = push_array(arena, Bench_Row, row_count);
  Bench_Rng rng = {0x5CA1AB1E5EEDULL};
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

internal B32
scmp_write_csv(String8 path, Bench_Row* rows, U64 row_count, B32 null_first_value)
{
  Temp scratch = scratch_begin(0, 0);
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,name,value\n");
  for (U64 i = 0; i < row_count; i++)
  {
    if (null_first_value && i == 0)
    {
      str8_list_pushf(scratch.arena, &lines, "%u,%.*s,\n", rows[i].id, str8_varg(rows[i].name));
    }
    else
    {
      str8_list_pushf(scratch.arena, &lines, "%u,%.*s,%.4f\n", rows[i].id, str8_varg(rows[i].name), rows[i].value);
    }
  }
  String8 content = str8_list_join(scratch.arena, &lines, 0);
  
  B32 ok = 0;
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if (!os_handle_match(file, os_handle_zero()))
  {
    os_file_write(file, r1u64(0, content.size), content.str);
    os_file_close(file);
    ok = 1;
  }
  scratch_end(scratch);
  return ok;
}

internal QE_TraceStrategy
scmp_get_strategy(Arena* arena, GDB_Database* database, String8 sql_text)
{
  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;
  ir_expand_star_to_columns(arena, database, select_node);
  PLAN_Node* plan = plan_build_from_select(arena, database, select_node);
  
  QE_TraceCtx* trace = qe_trace_ctx_alloc(arena);
  plan_execute(arena, database, plan, select_node, trace);
  
  QE_TraceStrategy strategy = QE_TraceStrategy_None;
  for (QE_NodeTrace* nt = trace->records; nt; nt = nt->next)
  {
    if (nt->node_type == PLAN_NodeType_Filter || nt->node_type == PLAN_NodeType_Scan)
    {
      strategy = nt->scan.strategy;
    }
  }
  return strategy;
}

internal void
scmp_check_strategy(Bench_Report* report, String8 label, QE_TraceStrategy actual, QE_TraceStrategy expected)
{
  B32 ok = (actual == expected);
  printf("  %-48.*s strategy=%d (expected %d) %s\n", str8_varg(label), actual, expected, ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "'%.*s' expected scan strategy %d, got %d", str8_varg(label), expected, actual);
  }
}

internal U64
scmp_build_cases(Arena* arena, Bench_QueryCase* out_cases, String8 table_name, String8 select_list, B32 include_having)
{
  U64 n = 0;
  
  out_cases[n].label = str8_lit("name = (dict hit)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name = 'kilo';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name != (dict hit)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name != 'kilo';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name < (relational)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name < 'kilo';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name > (relational)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name > 'kilo';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name <= (relational)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name <= 'kilo';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name >= (relational)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name >= 'kilo';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name = (literal not in dict)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name = 'zzznotinlist';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name != (literal not in dict)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name != 'zzznotinlist';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  out_cases[n].label = str8_lit("name = (empty string literal)");
  out_cases[n].gdb_sql = push_str8f(arena, "SELECT %.*s FROM %.*s WHERE name = '';", str8_varg(select_list), str8_varg(table_name));
  out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  
  if (include_having)
  {
    out_cases[n].label = str8_lit("HAVING name > (qe_having_eval)");
    out_cases[n].gdb_sql = push_str8f(arena, "SELECT name, COUNT(*) FROM %.*s GROUP BY name HAVING name > 'kilo';", str8_varg(table_name));
    out_cases[n].sqlite_sql = out_cases[n].gdb_sql; out_cases[n].duckdb_sql = out_cases[n].gdb_sql; n++;
  }
  
  return n;
}

internal void
scmp_run_suite(Arena* arena, Bench_Report* report, U64 row_count, B32 low_cardinality, char* label)
{
  printf("\n########## string comparison suite: %s (%llu rows, %s) ##########\n",
         label, row_count, low_cardinality ? "low cardinality, dict-eligible" : "high cardinality, dict-ineligible");
  bench_report_section(report, "string comparison suite: %s (%llu rows, %s)",
                       label, row_count, low_cardinality ? "low cardinality, dict-eligible" : "high cardinality, dict-ineligible");
  
  String8 table_name = str8_lit("scmp");
  Temp scratch = scratch_begin(&arena, 1);
  
  Bench_Row* rows = low_cardinality
    ? scmp_generate_low_cardinality_rows(scratch.arena, row_count)
    : bench_generate_rows(scratch.arena, row_count);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = push_str8f(scratch.arena, "bench_data/string_cmp_%s.csv", label);
  bench_write_csv(csv_path, rows, row_count);
  
  GDB_Database* database = gdb_database_alloc(str8_lit("string_cmp_db"));
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
  
  Bench_QueryCase cases[16];
  U64 case_count = scmp_build_cases(scratch.arena, cases, table_name, str8_lit("*"), low_cardinality);
  
  bench_print_table_header(report, label);
  for (U64 i = 0; i < case_count; i++)
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
  
  // tec: confirm the '<' case above actually took the GPU bytecode path
  String8 lt_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name < 'kilo';", str8_varg(table_name));
  QE_TraceStrategy strategy = scmp_get_strategy(scratch.arena, database, lt_sql);
  scmp_check_strategy(report, str8_lit("name < strategy (expect GPU)"), strategy, QE_TraceStrategy_GpuScan);
  
  sqlite3_close(sqlite_db);
  duckdb_disconnect(&duckdb_conn);
  duckdb_close(&duckdb_db);
  scratch_end(scratch);
}

internal void
scmp_run_cpu_fallback_check(Arena* arena, Bench_Report* report, U64 row_count)
{
  printf("\n########## string comparison suite: cpu_fallback (%llu rows, forced CPU scan) ##########\n", row_count);
  bench_report_section(report, "string comparison suite: cpu_fallback (%llu rows, forced CPU scan)", row_count);
  
  String8 table_name = str8_lit("scmp_cpu");
  Temp scratch = scratch_begin(&arena, 1);
  
  Bench_Row* rows = scmp_generate_low_cardinality_rows(scratch.arena, row_count);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = push_str8f(scratch.arena, "bench_data/string_cmp_cpu_fallback.csv");
  scmp_write_csv(csv_path, rows, row_count, 1);
  
  GDB_Database* database = gdb_database_alloc(str8_lit("string_cmp_cpu_db"));
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
  
  Bench_QueryCase cases[16];
  U64 case_count = scmp_build_cases(scratch.arena, cases, table_name, str8_lit("id"), 1);
  
  bench_print_table_header(report, "cpu_fallback");
  for (U64 i = 0; i < case_count; i++)
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
  
  String8 lt_sql = push_str8f(scratch.arena, "SELECT id FROM %.*s WHERE name < 'kilo';", str8_varg(table_name));
  QE_TraceStrategy strategy = scmp_get_strategy(scratch.arena, database, lt_sql);
  scmp_check_strategy(report, str8_lit("name < strategy (expect CPU fallback)"), strategy, QE_TraceStrategy_CpuScan);
  
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
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite vs duckdb - string comparison opcodes");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  
  scmp_run_suite(arena, report, 2000, 1, "below_dict_threshold");
  
  scmp_run_suite(arena, report, 20000, 1, "dict_eligible");
  
  scmp_run_suite(arena, report, 20000, 0, "high_cardinality_no_dict");
  
  scmp_run_cpu_fallback_check(arena, report, 5000);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/string_cmp_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
