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

#define AA_NUM_GROUPS 20

typedef struct AA_FactRow AA_FactRow;
struct AA_FactRow
{
  U32 id;
  U32 group_key;
  U32 user_id;
  F64 value;
};

internal AA_FactRow*
aa_generate_rows(Arena* arena, U64 row_count, U64 user_id_range, U64 rng_seed)
{
  AA_FactRow* rows = push_array(arena, AA_FactRow, row_count);
  Bench_Rng rng = {rng_seed};
  for (U64 i = 0; i < row_count; i++)
  {
    rows[i].id = (U32)(i + 1);
    rows[i].group_key = (U32)(i % AA_NUM_GROUPS);
    rows[i].user_id = (U32)(bench_rng_next(&rng) % Max(user_id_range, 1));
    U64 vraw = bench_rng_next(&rng) % 10000000ULL;
    rows[i].value = (F64)vraw / 100.0;
  }
  return rows;
}

internal B32
aa_write_csv(Arena* arena, String8 path, AA_FactRow* rows, U64 row_count)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,group_key,group_name,user_id,value\n");
  for (U64 i = 0; i < row_count; i++)
  {
    str8_list_pushf(scratch.arena, &lines, "%u,%u,g%02u,%u,%.4f\n",
                    rows[i].id, rows[i].group_key, rows[i].group_key, rows[i].user_id, rows[i].value);
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

internal void
aa_sqlite_create_schema(sqlite3* db)
{
  char* err = NULL;
  if (sqlite3_exec(db, "CREATE TABLE fact (id INTEGER, group_key INTEGER, group_name TEXT, user_id INTEGER, value REAL);", NULL, NULL, &err) != SQLITE_OK)
  {
    log_error("sqlite3_exec (CREATE TABLE fact) failed: %s", err ? err : "unknown error");
    sqlite3_free(err);
  }
}

internal void
aa_sqlite_bulk_insert(sqlite3* db, AA_FactRow* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  
  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(db, "INSERT INTO fact (id, group_key, group_name, user_id, value) VALUES (?, ?, ?, ?, ?);", -1, &stmt, NULL);
  
  for (U64 i = 0; i < row_count; i++)
  {
    String8 name = push_str8f(scratch.arena, "g%02u", rows[i].group_key);
    sqlite3_bind_int64(stmt, 1, (S64)rows[i].id);
    sqlite3_bind_int64(stmt, 2, (S64)rows[i].group_key);
    sqlite3_bind_text(stmt, 3, (const char*)name.str, (int)name.size, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (S64)rows[i].user_id);
    sqlite3_bind_double(stmt, 5, rows[i].value);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  
  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
  scratch_end(scratch);
}

internal void
aa_duckdb_create_schema(duckdb_connection conn)
{
  duckdb_result result = {0};
  if (duckdb_query(conn, "CREATE TABLE fact (id INTEGER, group_key INTEGER, group_name VARCHAR, user_id INTEGER, value DOUBLE);", &result) != DuckDBSuccess)
  {
    log_error("duckdb_query (CREATE TABLE fact) failed: %s", duckdb_result_error(&result));
  }
  duckdb_destroy_result(&result);
}

internal void
aa_duckdb_bulk_insert(duckdb_connection conn, AA_FactRow* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  
  duckdb_appender appender = NULL;
  if (duckdb_appender_create(conn, NULL, "fact", &appender) == DuckDBSuccess)
  {
    for (U64 i = 0; i < row_count; i++)
    {
      String8 name = push_str8f(scratch.arena, "g%02u", rows[i].group_key);
      duckdb_append_int32(appender, (S32)rows[i].id);
      duckdb_append_int32(appender, (S32)rows[i].group_key);
      duckdb_append_varchar_length(appender, (const char*)name.str, name.size);
      duckdb_append_int32(appender, (S32)rows[i].user_id);
      duckdb_append_double(appender, rows[i].value);
      duckdb_appender_end_row(appender);
    }
  }
  duckdb_appender_destroy(&appender);
  
  scratch_end(scratch);
}

internal PLAN_Materialized
aa_run_gdb(Arena* arena, GDB_Database* database, String8 sql_text)
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

internal F64
aa_hll_max_relative_error(U64 hll_precision)
{
  // tec: standard HLL relative-error bound (1.04/sqrt(m)) with a 3x safety margin
  // since this is a single sample rather than an average over many trials
  return 3.0 * 1.04 / sqrt_f64((F64)(1ull << hll_precision));
}

internal void
aa_check_relative_error(Bench_Report* report, String8 label, F64 approx_value, F64 exact_value, F64 max_relative_error)
{
  F64 denom = Max(abs_f64(exact_value), 1.0);
  F64 rel_err = abs_f64(approx_value - exact_value) / denom;
  B32 ok = rel_err <= max_relative_error;
  
  printf("  %-40.*s approx=%10.2f exact=%10.2f rel_err=%.4f (max %.4f) %s\n",
         str8_varg(label), approx_value, exact_value, rel_err, max_relative_error, ok ? "OK" : "FAIL");
  
  if (!ok)
  {
    bench_report_warn(report,
                      "relative error too high on '%.*s': approx=%.2f exact=%.2f rel_err=%.4f (max %.4f)",
                      str8_varg(label), approx_value, exact_value, rel_err, max_relative_error);
  }
}

internal F64
aa_duckdb_query_scalar(duckdb_connection conn, String8 sql_text)
{
  F64 value = 0.0;
  duckdb_result result = {0};
  if (duckdb_query(conn, (const char*)sql_text.str, &result) == DuckDBSuccess && duckdb_row_count(&result) > 0)
  {
    value = duckdb_value_double(&result, 0, 0);
  }
  else
  {
    log_error("duckdb_query failed: %s", duckdb_result_error(&result));
  }
  duckdb_destroy_result(&result);
  return value;
}

internal void
aa_check_exact(Bench_Report* report, String8 label, F64 actual_value, F64 expected_value)
{
  B32 ok = (actual_value == expected_value);
  printf("  %-40.*s actual=%.0f expected=%.0f %s\n", str8_varg(label), actual_value, expected_value, ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "'%.*s' expected exactly %.0f, got %.0f", str8_varg(label), expected_value, actual_value);
  }
}

internal void
aa_run_suite(Arena* arena, Bench_Report* report, U64 row_count, U64 user_id_range, char* label)
{
  printf("\n########## approx-agg suite: %s (%llu rows, %llu distinct user_id range, %u groups) ##########\n",
         label, row_count, user_id_range, (U32)AA_NUM_GROUPS);
  bench_report_section(report, "approx-agg suite: %s (%llu rows, %llu distinct user_id range)", label, row_count, user_id_range);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  AA_FactRow* rows = aa_generate_rows(scratch.arena, row_count, user_id_range, 0x517CADEADBEEFULL);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = push_str8f(scratch.arena, "bench_data/approx_agg_fact_%s.csv", label);
  aa_write_csv(scratch.arena, csv_path, rows, row_count);
  
  //- tec: seed compute_ql
  GDB_Database* database = gdb_database_alloc(str8_lit("approx_agg_db"));
  gdb_add_database(database);
  GDB_Table* fact_table = gdb_table_import_csv_streaming(database, str8_lit("fact"), csv_path);
  gdb_database_add_table(database, fact_table);
  
  //- tec: seed sqlite with the exact same rows
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  aa_sqlite_create_schema(sqlite_db);
  aa_sqlite_bulk_insert(sqlite_db, rows, row_count);
  
  //- tec: seed duckdb with the exact same rows
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  duckdb_open(NULL, &duckdb_db);
  duckdb_connect(duckdb_db, &duckdb_conn);
  aa_duckdb_create_schema(duckdb_conn);
  aa_duckdb_bulk_insert(duckdb_conn, rows, row_count);
  
  F64 hll_precision = (F64)settings_u64(str8_lit("QE_HLL_PRECISION"), 10);
  F64 max_hll_rel_err = aa_hll_max_relative_error((U64)hll_precision);
  // tec: t-digest here has no closed-form error bound (unlike HLL) since each centroid is a GPU
  // thread's own running mean over a handful of rows
  F64 max_pct_rel_err = 0.05;
  
  //- tec: exact per-group distinct counts and row counts, from sqlite, indexed by group_key (0..AA_NUM_GROUPS-1)
  F64 exact_distinct_by_group[AA_NUM_GROUPS] = {0};
  F64 exact_rowcount_by_group[AA_NUM_GROUPS] = {0};
  {
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(sqlite_db, "SELECT group_key, COUNT(*), COUNT(DISTINCT user_id) FROM fact GROUP BY group_key;", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      S64 group_key = sqlite3_column_int64(stmt, 0);
      if (group_key >= 0 && group_key < AA_NUM_GROUPS)
      {
        exact_rowcount_by_group[group_key] = (F64)sqlite3_column_int64(stmt, 1);
        exact_distinct_by_group[group_key] = (F64)sqlite3_column_int64(stmt, 2);
      }
    }
    sqlite3_finalize(stmt);
  }
  
  F64 exact_distinct_global = 0;
  {
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(sqlite_db, "SELECT COUNT(DISTINCT user_id) FROM fact;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
      exact_distinct_global = (F64)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  
  //- tec: global aggregate, no GROUP BY
  {
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database, str8_lit("SELECT APPROX_COUNT_DISTINCT(user_id) FROM fact;"));
    F64 approx = (m.column_count > 0 && m.count > 0) ? m.columns[0].numeric_values[0] : -1.0;
    aa_check_relative_error(report, str8_lit("global aggregate"), approx, exact_distinct_global, max_hll_rel_err);
  }
  
  //- tec: GROUP BY numeric key
  {
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database,
                                     str8_lit("SELECT group_key, APPROX_COUNT_DISTINCT(user_id) FROM fact GROUP BY group_key;"));
    for (U64 g = 0; g < m.count; g++)
    {
      U32 group_key = (U32)m.columns[0].numeric_values[g];
      F64 approx = m.columns[1].numeric_values[g];
      String8 row_label = push_str8f(scratch.arena, "GROUP BY numeric key (group_key=%u)", group_key);
      aa_check_relative_error(report, row_label, approx, exact_distinct_by_group[group_key % AA_NUM_GROUPS], max_hll_rel_err);
    }
  }
  
  //- tec: GROUP BY string key
  {
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database,
                                     str8_lit("SELECT group_name, APPROX_COUNT_DISTINCT(user_id) FROM fact GROUP BY group_name;"));
    for (U64 g = 0; g < m.count; g++)
    {
      String8 group_name = m.columns[0].string_values[g];
      F64 approx = m.columns[1].numeric_values[g];
      U32 group_key = 0;
      for (U64 i = 0; i < group_name.size; i++)
      {
        if (group_name.str[i] >= '0' && group_name.str[i] <= '9')
        {
          group_key = group_key * 10 + (group_name.str[i] - '0');
        }
      }
      String8 row_label = push_str8f(scratch.arena, "GROUP BY string key (%.*s)", str8_varg(group_name));
      aa_check_relative_error(report, row_label, approx, exact_distinct_by_group[group_key % AA_NUM_GROUPS], max_hll_rel_err);
    }
  }
  
  //- tec: mixed SELECT list
  {
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database,
                                     str8_lit("SELECT group_key, COUNT(*), APPROX_COUNT_DISTINCT(user_id) FROM fact GROUP BY group_key;"));
    for (U64 g = 0; g < m.count; g++)
    {
      U32 group_key = (U32)m.columns[0].numeric_values[g];
      F64 exact_count = m.columns[1].numeric_values[g];
      F64 approx_distinct = m.columns[2].numeric_values[g];
      String8 count_label = push_str8f(scratch.arena, "mixed SELECT COUNT(*) (group_key=%u)", group_key);
      aa_check_exact(report, count_label, exact_count, exact_rowcount_by_group[group_key % AA_NUM_GROUPS]);
      String8 distinct_label = push_str8f(scratch.arena, "mixed SELECT APPROX_COUNT_DISTINCT (group_key=%u)", group_key);
      aa_check_relative_error(report, distinct_label, approx_distinct, exact_distinct_by_group[group_key % AA_NUM_GROUPS], max_hll_rel_err);
    }
  }
  
  //- tec: APPROX_PERCENTILE, global aggregate, several fractions against duckdb's exact quantile_cont
  {
    F64 fractions[3] = { 0.5, 0.95, 0.99 };
    for (U32 i = 0; i < ArrayCount(fractions); i++)
    {
      String8 sql = push_str8f(scratch.arena, "SELECT APPROX_PERCENTILE(value, %.2f) FROM fact;", fractions[i]);
      PLAN_Materialized m = aa_run_gdb(scratch.arena, database, sql);
      F64 approx = (m.column_count > 0 && m.count > 0) ? m.columns[0].numeric_values[0] : -1.0;
      
      String8 duckdb_sql = push_str8f(scratch.arena, "SELECT quantile_cont(value, %.2f) FROM fact;", fractions[i]);
      F64 exact = aa_duckdb_query_scalar(duckdb_conn, duckdb_sql);
      
      String8 label = push_str8f(scratch.arena, "global APPROX_PERCENTILE(value, %.2f)", fractions[i]);
      aa_check_relative_error(report, label, approx, exact, max_pct_rel_err);
    }
  }
  
  //- tec: APPROX_PERCENTILE, GROUP BY numeric key
  {
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database,
                                     str8_lit("SELECT group_key, APPROX_PERCENTILE(value, 0.5) FROM fact GROUP BY group_key;"));
    for (U64 g = 0; g < m.count; g++)
    {
      U32 group_key = (U32)m.columns[0].numeric_values[g];
      F64 approx = m.columns[1].numeric_values[g];
      
      String8 duckdb_sql = push_str8f(scratch.arena, "SELECT quantile_cont(value, 0.5) FROM fact WHERE group_key = %u;", group_key);
      F64 exact = aa_duckdb_query_scalar(duckdb_conn, duckdb_sql);
      
      String8 label = push_str8f(scratch.arena, "GROUP BY numeric key APPROX_PERCENTILE(value, 0.5) (group_key=%u)", group_key);
      aa_check_relative_error(report, label, approx, exact, max_pct_rel_err);
    }
  }
  
  //- tec: two different fractions of the same column in one query
  {
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database,
                                     str8_lit("SELECT group_key, APPROX_PERCENTILE(value, 0.5), APPROX_PERCENTILE(value, 0.95) FROM fact GROUP BY group_key;"));
    for (U64 g = 0; g < m.count; g++)
    {
      U32 group_key = (U32)m.columns[0].numeric_values[g];
      F64 approx_p50 = m.columns[1].numeric_values[g];
      F64 approx_p95 = m.columns[2].numeric_values[g];
      
      String8 duckdb_p50_sql = push_str8f(scratch.arena, "SELECT quantile_cont(value, 0.5) FROM fact WHERE group_key = %u;", group_key);
      F64 exact_p50 = aa_duckdb_query_scalar(duckdb_conn, duckdb_p50_sql);
      String8 duckdb_p95_sql = push_str8f(scratch.arena, "SELECT quantile_cont(value, 0.95) FROM fact WHERE group_key = %u;", group_key);
      F64 exact_p95 = aa_duckdb_query_scalar(duckdb_conn, duckdb_p95_sql);
      
      String8 label_p50 = push_str8f(scratch.arena, "multi-percentile p50 (group_key=%u)", group_key);
      aa_check_relative_error(report, label_p50, approx_p50, exact_p50, max_pct_rel_err);
      String8 label_p95 = push_str8f(scratch.arena, "multi-percentile p95 (group_key=%u)", group_key);
      aa_check_relative_error(report, label_p95, approx_p95, exact_p95, max_pct_rel_err);
    }
  }
  
  sqlite3_close(sqlite_db);
  duckdb_disconnect(&duckdb_conn);
  duckdb_close(&duckdb_db);
  scratch_end(scratch);
}

internal void
aa_run_edge_cases(Arena* arena, Bench_Report* report)
{
  printf("\n########## approx-agg suite: edge cases ##########\n");
  bench_report_section(report, "approx-agg suite: edge cases");
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: zero rows
  {
    AA_FactRow* rows = aa_generate_rows(scratch.arena, 0, 1, 1);
    String8 csv_path = str8_lit("bench_data/approx_agg_fact_zero_rows.csv");
    aa_write_csv(scratch.arena, csv_path, rows, 0);
    
    GDB_Database* database = gdb_database_alloc(str8_lit("approx_agg_zero_db"));
    gdb_add_database(database);
    GDB_Table* fact_table = gdb_table_import_csv_streaming(database, str8_lit("fact"), csv_path);
    gdb_database_add_table(database, fact_table);
    
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database, str8_lit("SELECT APPROX_COUNT_DISTINCT(user_id) FROM fact;"));
    F64 approx = (m.column_count > 0 && m.count > 0) ? m.columns[0].numeric_values[0] : -1.0;
    aa_check_exact(report, str8_lit("zero rows"), approx, 0.0);
  }
  
  //- tec: one row -> exactly one distinct value
  {
    AA_FactRow* rows = aa_generate_rows(scratch.arena, 1, 1000, 2);
    String8 csv_path = str8_lit("bench_data/approx_agg_fact_one_row.csv");
    aa_write_csv(scratch.arena, csv_path, rows, 1);
    
    GDB_Database* database = gdb_database_alloc(str8_lit("approx_agg_one_db"));
    gdb_add_database(database);
    GDB_Table* fact_table = gdb_table_import_csv_streaming(database, str8_lit("fact"), csv_path);
    gdb_database_add_table(database, fact_table);
    
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database, str8_lit("SELECT APPROX_COUNT_DISTINCT(user_id) FROM fact;"));
    F64 approx = (m.column_count > 0 && m.count > 0) ? m.columns[0].numeric_values[0] : -1.0;
    aa_check_exact(report, str8_lit("one row"), approx, 1.0);
  }
  
  //- tec: many rows, all the same value -> still exactly one distinct value
  {
    U64 row_count = 1000;
    // tec: user_id_range=1 forces every row's user_id to 0
    AA_FactRow* rows = aa_generate_rows(scratch.arena, row_count, 1, 3); 
    String8 csv_path = str8_lit("bench_data/approx_agg_fact_all_identical.csv");
    aa_write_csv(scratch.arena, csv_path, rows, row_count);
    
    GDB_Database* database = gdb_database_alloc(str8_lit("approx_agg_identical_db"));
    gdb_add_database(database);
    GDB_Table* fact_table = gdb_table_import_csv_streaming(database, str8_lit("fact"), csv_path);
    gdb_database_add_table(database, fact_table);
    
    PLAN_Materialized m = aa_run_gdb(scratch.arena, database, str8_lit("SELECT APPROX_COUNT_DISTINCT(user_id) FROM fact;"));
    F64 approx = (m.column_count > 0 && m.count > 0) ? m.columns[0].numeric_values[0] : -1.0;
    aa_check_exact(report, str8_lit("all identical values"), approx, 1.0);
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
  
  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql APPROX_COUNT_DISTINCT (HyperLogLog) correctness");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  
  // tec: 1M rows / 20 groups = 50K rows/group = ~13 chunks/group at the default
  // QE_AGG_ROWS_PER_CHUNK=4096, giving each group's t-digest ~3300 weighted centroid samples
  aa_run_suite(arena, report, 1000000, 50000, "mid_cardinality");
  aa_run_edge_cases(arena, report);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/approx_agg_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
