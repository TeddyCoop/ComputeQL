// compute_ql vs sqlite: GROUP BY / hash-join hash tables larger than a single GPU buffer.
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE MB(4)

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "gdb/gdb_inc.h"
#include "ir_gen/ir_gen_inc.h"
#include "gpu/gpu_inc.h"
#include "query_exec/query_exec.h"
#include "planner/planner.h"
#include "third_party/sqlite/sqlite3.h"
#include "third_party/duckdb/duckdb.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"

#include "tests/bench_common.h"

#define CH_NUM_GROUPS 10000

typedef struct CH_FactRow CH_FactRow;
struct CH_FactRow
{
  U32 id;
  U32 group_key;
  F64 value;
};

internal CH_FactRow*
ch_generate_fact_rows(Arena* arena, U64 row_count)
{
  CH_FactRow* rows = push_array(arena, CH_FactRow, row_count);
  Bench_Rng rng = {0xC0FFEE12345ULL};
  for (U64 i = 0; i < row_count; i++)
  {
    rows[i].id = (U32)(i + 1);
    rows[i].group_key = (U32)(i % CH_NUM_GROUPS);
    U64 vraw = bench_rng_next(&rng) % 10000000ULL;
    rows[i].value = (F64)vraw / 100.0;
  }
  return rows;
}

internal B32
ch_write_fact_csv(Arena* arena, String8 path, CH_FactRow* rows, U64 row_count)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,group_key,group_name,value\n");
  for (U64 i = 0; i < row_count; i++)
  {
    str8_list_pushf(scratch.arena, &lines, "%u,%u,g%05u,%.4f\n",
                     rows[i].id, rows[i].group_key, rows[i].group_key, rows[i].value);
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
ch_sqlite_create_fact_schema(sqlite3* db)
{
  char* err = NULL;
  if (sqlite3_exec(db, "CREATE TABLE fact (id INTEGER, group_key INTEGER, group_name TEXT, value REAL);", NULL, NULL, &err) != SQLITE_OK)
  {
    log_error("sqlite3_exec (CREATE TABLE fact) failed: %s", err ? err : "unknown error");
    sqlite3_free(err);
  }
}

internal void
ch_sqlite_bulk_insert_fact(sqlite3* db, CH_FactRow* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);

  sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(db, "INSERT INTO fact (id, group_key, group_name, value) VALUES (?, ?, ?, ?);", -1, &stmt, NULL);

  for (U64 i = 0; i < row_count; i++)
  {
    String8 name = push_str8f(scratch.arena, "g%05u", rows[i].group_key);
    sqlite3_bind_int64(stmt, 1, (S64)rows[i].id);
    sqlite3_bind_int64(stmt, 2, (S64)rows[i].group_key);
    sqlite3_bind_text(stmt, 3, (const char*)name.str, (int)name.size, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, rows[i].value);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }

  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

  scratch_end(scratch);
}

internal void
ch_duckdb_create_fact_schema(duckdb_connection conn)
{
  duckdb_result result = {0};
  if (duckdb_query(conn, "CREATE TABLE fact (id INTEGER, group_key INTEGER, group_name VARCHAR, value DOUBLE);", &result) != DuckDBSuccess)
  {
    log_error("duckdb_query (CREATE TABLE fact) failed: %s", duckdb_result_error(&result));
  }
  duckdb_destroy_result(&result);
}

internal void
ch_duckdb_bulk_insert_fact(duckdb_connection conn, CH_FactRow* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);

  duckdb_appender appender = NULL;
  if (duckdb_appender_create(conn, NULL, "fact", &appender) == DuckDBSuccess)
  {
    for (U64 i = 0; i < row_count; i++)
    {
      String8 name = push_str8f(scratch.arena, "g%05u", rows[i].group_key);
      duckdb_append_int32(appender, (S32)rows[i].id);
      duckdb_append_int32(appender, (S32)rows[i].group_key);
      duckdb_append_varchar_length(appender, (const char*)name.str, name.size);
      duckdb_append_double(appender, rows[i].value);
      duckdb_appender_end_row(appender);
    }
  }
  duckdb_appender_destroy(&appender);

  scratch_end(scratch);
}

internal void
ch_duckdb_create_dim_schema(duckdb_connection conn)
{
  duckdb_result result = {0};
  if (duckdb_query(conn, "CREATE TABLE dim (dim_key INTEGER, dim_name VARCHAR);", &result) != DuckDBSuccess)
  {
    log_error("duckdb_query (CREATE TABLE dim) failed: %s", duckdb_result_error(&result));
  }
  duckdb_destroy_result(&result);
}

internal void
ch_duckdb_bulk_insert_dim(duckdb_connection conn, U64 num_groups)
{
  Temp scratch = scratch_begin(0, 0);

  duckdb_appender appender = NULL;
  if (duckdb_appender_create(conn, NULL, "dim", &appender) == DuckDBSuccess)
  {
    for (U64 g = 0; g < num_groups; g++)
    {
      String8 name = push_str8f(scratch.arena, "dimname_g%05llu", g);
      duckdb_append_int32(appender, (S32)g);
      duckdb_append_varchar_length(appender, (const char*)name.str, name.size);
      duckdb_appender_end_row(appender);
    }
  }
  duckdb_appender_destroy(&appender);

  scratch_end(scratch);
}

internal B32
ch_write_dim_csv(Arena* arena, String8 path, U64 num_groups)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "dim_key,dim_name\n");
  for (U64 g = 0; g < num_groups; g++)
  {
    str8_list_pushf(scratch.arena, &lines, "%llu,dimname_g%05llu\n", g, g);
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
ch_sqlite_create_dim_schema(sqlite3* db)
{
  char* err = NULL;
  if (sqlite3_exec(db, "CREATE TABLE dim (dim_key INTEGER, dim_name TEXT);", NULL, NULL, &err) != SQLITE_OK)
  {
    log_error("sqlite3_exec (CREATE TABLE dim) failed: %s", err ? err : "unknown error");
    sqlite3_free(err);
  }
}

internal void
ch_sqlite_bulk_insert_dim(sqlite3* db, U64 num_groups)
{
  Temp scratch = scratch_begin(0, 0);

  sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(db, "INSERT INTO dim (dim_key, dim_name) VALUES (?, ?);", -1, &stmt, NULL);

  for (U64 g = 0; g < num_groups; g++)
  {
    String8 name = push_str8f(scratch.arena, "dimname_g%05llu", g);
    sqlite3_bind_int64(stmt, 1, (S64)g);
    sqlite3_bind_text(stmt, 2, (const char*)name.str, (int)name.size, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }

  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

  scratch_end(scratch);
}

internal void
ch_run_suite(Arena* arena, Bench_Report* report, U64 row_count, char* label)
{
  printf("\n########## chunked-hash suite: %s (%llu fact rows, %u groups) ##########\n", label, row_count, (U32)CH_NUM_GROUPS);
  bench_report_section(report, "chunked-hash suite: %s (%llu fact rows, %u groups)", label, row_count, (U32)CH_NUM_GROUPS);

  Temp scratch = scratch_begin(&arena, 1);

  CH_FactRow* fact_rows = ch_generate_fact_rows(scratch.arena, row_count);

  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 fact_csv_path = push_str8f(scratch.arena, "bench_data/chunked_hash_fact_%s.csv", label);
  String8 dim_csv_path = push_str8f(scratch.arena, "bench_data/chunked_hash_dim_%s.csv", label);
  ch_write_fact_csv(scratch.arena, fact_csv_path, fact_rows, row_count);
  ch_write_dim_csv(scratch.arena, dim_csv_path, CH_NUM_GROUPS);

  //- tec: seed compute_ql
  GDB_Database* database = gdb_database_alloc(str8_lit("chunked_hash_db"));
  gdb_add_database(database);
  GDB_Table* fact_table = gdb_table_import_csv_streaming(database, str8_lit("fact"), fact_csv_path);
  gdb_database_add_table(database, fact_table);
  GDB_Table* dim_table = gdb_table_import_csv_streaming(database, str8_lit("dim"), dim_csv_path);
  gdb_database_add_table(database, dim_table);

  //- tec: seed sqlite with the exact same rows
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  ch_sqlite_create_fact_schema(sqlite_db);
  ch_sqlite_bulk_insert_fact(sqlite_db, fact_rows, row_count);
  ch_sqlite_create_dim_schema(sqlite_db);
  ch_sqlite_bulk_insert_dim(sqlite_db, CH_NUM_GROUPS);

  //- tec: seed duckdb with the exact same rows
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  duckdb_open(NULL, &duckdb_db);
  duckdb_connect(duckdb_db, &duckdb_conn);
  ch_duckdb_create_fact_schema(duckdb_conn);
  ch_duckdb_bulk_insert_fact(duckdb_conn, fact_rows, row_count);
  ch_duckdb_create_dim_schema(duckdb_conn);
  ch_duckdb_bulk_insert_dim(duckdb_conn, CH_NUM_GROUPS);

  Bench_QueryCase cases[5];
  cases[0].label = str8_lit("GROUP BY numeric key");
  cases[0].gdb_sql = str8_lit("SELECT group_key, COUNT(*), SUM(value), AVG(value), MIN(value), MAX(value) FROM fact GROUP BY group_key;");
  cases[0].sqlite_sql = cases[0].gdb_sql;
  cases[0].duckdb_sql = cases[0].gdb_sql;

  cases[1].label = str8_lit("GROUP BY string key");
  cases[1].gdb_sql = str8_lit("SELECT group_name, COUNT(*), SUM(value) FROM fact GROUP BY group_name;");
  cases[1].sqlite_sql = cases[1].gdb_sql;
  cases[1].duckdb_sql = cases[1].gdb_sql;

  cases[2].label = str8_lit("global aggregate (no GROUP BY)");
  cases[2].gdb_sql = str8_lit("SELECT COUNT(*), SUM(value), MIN(value), MAX(value) FROM fact;");
  cases[2].sqlite_sql = cases[2].gdb_sql;
  cases[2].duckdb_sql = cases[2].gdb_sql;

  cases[3].label = str8_lit("INNER JOIN, fact as build side");
  cases[3].gdb_sql = str8_lit("SELECT dim.dim_name, fact.id, fact.value FROM dim JOIN fact ON dim.dim_key = fact.group_key;");
  cases[3].sqlite_sql = str8_lit("SELECT dim.dim_name, fact.id, fact.value FROM dim INNER JOIN fact ON dim.dim_key = fact.group_key;");
  cases[3].duckdb_sql = cases[3].sqlite_sql;

  cases[4].label = str8_lit("LEFT JOIN, fact as build side");
  cases[4].gdb_sql = str8_lit("SELECT dim.dim_name, fact.id FROM dim LEFT JOIN fact ON dim.dim_key = fact.group_key;");
  cases[4].sqlite_sql = str8_lit("SELECT dim.dim_name, fact.id FROM dim LEFT JOIN fact ON dim.dim_key = fact.group_key;");
  cases[4].duckdb_sql = cases[4].sqlite_sql;

  bench_print_table_header(report, label);
  for (U64 i = 0; i < ArrayCount(cases); i++)
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
  printf("GPU_MAX_BUFFER_SIZE = %llu bytes (deliberately lowered for this test - see file header)\n", (U64)GPU_MAX_BUFFER_SIZE);

  Arena* arena = arena_alloc(.reserve_size = GB(4), .commit_size = MB(64));

  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite vs duckdb - chunked cross-bucket hashing correctness");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  bench_report_text(report, "GPU_MAX_BUFFER_SIZE = %llu bytes (lowered for this test)", (U64)GPU_MAX_BUFFER_SIZE);

  ch_run_suite(arena, report, 50000, "below_threshold");

  ch_run_suite(arena, report, 2000000, "past_threshold");

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/chunked_hash_report.md"));

  arena_release(arena);

  log_release();

  ProfEnd();
  ProfEndCapture();
}
