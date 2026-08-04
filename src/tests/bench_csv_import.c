#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "gdb/gdb_inc.h"
#include "ir_gen/ir_gen_inc.h"
#include "gpu/gpu_inc.h"
#include "query_exec/query_exec.h"
#include "planner/planner.h"
#include "third_party/sqlite/sqlite3.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"
// tec: sqlite3 is compiled own its own

#include "tests/bench_common.h"

#define IMPORT_WARMUP_RUNS 1
#define IMPORT_TIMED_RUNS  5

internal void
bench_sqlite_import_csv(sqlite3* db, String8 table_name, String8 csv_path)
{
  Temp scratch = scratch_begin(0, 0);

  OS_Handle file = os_file_open(OS_AccessFlag_Read, csv_path);
  U64 file_size = os_properties_from_file(file).size;
  U8* buffer = push_array(scratch.arena, U8, file_size);
  os_file_read(file, r1u64(0, file_size), buffer);
  os_file_close(file);

  sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

  String8 insert_sql = push_str8f(scratch.arena, "INSERT INTO %.*s (id, name, value) VALUES (?, ?, ?);", str8_varg(table_name));
  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(db, (const char*)insert_sql.str, (int)insert_sql.size, &stmt, NULL);

  String8 fields[3];
  U64 pos = 0;
  B32 skipped_header = 0;
  while (pos < file_size)
  {
    U64 line_start = pos;
    while (pos < file_size && buffer[pos] != '\n') pos++;
    U64 line_len = pos - line_start;
    if (line_len > 0 && buffer[line_start + line_len - 1] == '\r') line_len--;

    if (!skipped_header)
    {
      skipped_header = 1;
    }
    else if (line_len > 0)
    {
      U64 field_count = parse_csv_line(buffer + line_start, line_len, fields, 3);
      if (field_count == 3)
      {
        sqlite3_bind_int64(stmt, 1, (S64)u64_from_str8(fields[0], 10));
        sqlite3_bind_text(stmt, 2, (const char*)fields[1].str, (int)fields[1].size, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, f64_from_str8(fields[2]));
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
      }
    }

    pos++; // skip '\n'
  }

  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

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

  U64 row_count = 100000;
  printf("\n########## csv import: %llu rows ##########\n", row_count);

  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));

  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite - csv import");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  bench_report_section(report, "csv import: %llu rows", row_count);

  Bench_Row* rows = bench_generate_rows(arena, row_count);

  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = str8_lit("bench_data/csv_import.csv");
  bench_write_csv(csv_path, rows, row_count);

  String8 table_name = str8_lit("bench_import");

  //- tec: compute_ql import
  GDB_Database* gdb_import_db = gdb_database_alloc(str8_lit("bench_import_db"));
  GDB_Table* previous_import_table = NULL;

  for (U64 run = 0; run < IMPORT_WARMUP_RUNS; run++)
  {
    GDB_Table* table = gdb_table_import_csv_streaming(gdb_import_db, table_name, csv_path);
    previous_import_table = table;
  }

  F64 gdb_samples[IMPORT_TIMED_RUNS];
  U64 gdb_last_row_count = 0;
  for (U64 run = 0; run < IMPORT_TIMED_RUNS; run++)
  {
    if (previous_import_table)
    {
      for (U64 c = 0; c < previous_import_table->column_count; c++)
      {
        gdb_column_close(previous_import_table->columns[c]);
      }
    }

    U64 s0 = os_now_microseconds();
    GDB_Table* table = gdb_table_import_csv_streaming(gdb_import_db, table_name, csv_path);
    U64 s1 = os_now_microseconds();
    gdb_samples[run] = (F64)(s1 - s0) / 1000.0;
    gdb_last_row_count = table->row_count;
    previous_import_table = table;
  }

  gdb_database_replace_table(gdb_import_db, previous_import_table);

  //- tec: sqlite import
  for (U64 run = 0; run < IMPORT_WARMUP_RUNS; run++)
  {
    sqlite3* db = NULL;
    sqlite3_open(":memory:", &db);
    bench_sqlite_create_schema(db, table_name);
    bench_sqlite_import_csv(db, table_name, csv_path);
    sqlite3_close(db);
  }

  F64 sqlite_samples[IMPORT_TIMED_RUNS];
  U64 sqlite_last_row_count = 0;
  for (U64 run = 0; run < IMPORT_TIMED_RUNS; run++)
  {
    U64 s0 = os_now_microseconds();
    sqlite3* db = NULL;
    sqlite3_open(":memory:", &db);
    bench_sqlite_create_schema(db, table_name);
    bench_sqlite_import_csv(db, table_name, csv_path);
    U64 s1 = os_now_microseconds();

    sqlite3_stmt* count_stmt = NULL;
    String8 count_sql = push_str8f(arena, "SELECT COUNT(*) FROM %.*s;", str8_varg(table_name));
    if (sqlite3_prepare_v2(db, (const char*)count_sql.str, (int)count_sql.size, &count_stmt, NULL) == SQLITE_OK &&
        sqlite3_step(count_stmt) == SQLITE_ROW)
    {
      sqlite_last_row_count = (U64)sqlite3_column_int64(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    sqlite3_close(db);
    sqlite_samples[run] = (F64)(s1 - s0) / 1000.0;
  }

  F64 gdb_min, gdb_med, gdb_avg;
  F64 sqlite_min, sqlite_med, sqlite_avg;
  bench_reduce(gdb_samples, IMPORT_TIMED_RUNS, &gdb_min, &gdb_med, &gdb_avg);
  bench_reduce(sqlite_samples, IMPORT_TIMED_RUNS, &sqlite_min, &sqlite_med, &sqlite_avg);

  printf("\n%-10s %10s %10s %10s %10s\n", "engine", "rows", "min(ms)", "median(ms)", "avg(ms)");
  printf("%-10s %10llu %10.4f %10.4f %10.4f\n", "gdb", gdb_last_row_count, gdb_min, gdb_med, gdb_avg);
  printf("%-10s %10llu %10.4f %10.4f %10.4f\n", "sqlite", sqlite_last_row_count, sqlite_min, sqlite_med, sqlite_avg);

  bench_report_row_begin(report);
  bench_report_cellf(report, "engine");
  bench_report_cellf(report, "rows");
  bench_report_cellf(report, "min (ms)");
  bench_report_cellf(report, "median (ms)");
  bench_report_cellf(report, "avg (ms)");
  bench_report_table_header_end(report);

  bench_report_row_begin(report);
  bench_report_cellf(report, "gdb");
  bench_report_cellf(report, "%llu", gdb_last_row_count);
  bench_report_cellf(report, "%.4f", gdb_min);
  bench_report_cellf(report, "%.4f", gdb_med);
  bench_report_cellf(report, "%.4f", gdb_avg);
  bench_report_row_end(report);

  bench_report_row_begin(report);
  bench_report_cellf(report, "sqlite");
  bench_report_cellf(report, "%llu", sqlite_last_row_count);
  bench_report_cellf(report, "%.4f", sqlite_min);
  bench_report_cellf(report, "%.4f", sqlite_med);
  bench_report_cellf(report, "%.4f", sqlite_avg);
  bench_report_row_end(report);

  if (gdb_last_row_count != row_count || sqlite_last_row_count != row_count)
  {
    printf("  !! row count mismatch: expected %llu, gdb imported %llu, sqlite imported %llu\n",
           row_count, gdb_last_row_count, sqlite_last_row_count);
    bench_report_warn(report, "row count mismatch: expected %llu, gdb imported %llu, sqlite imported %llu",
                       row_count, gdb_last_row_count, sqlite_last_row_count);
  }

  printf("(sqlite has no native CSV loader via the C API - this times the idiomatic bulk-load path: one transaction, one prepared INSERT, bind+step+reset per parsed row)\n");
  bench_report_text(report, "(sqlite has no native CSV loader via the C API - this times the idiomatic bulk-load path: one transaction, one prepared INSERT, bind+step+reset per parsed row)");

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/csv_import_report.md"));

  arena_release(arena);
  log_release();

  ProfEnd();
  ProfEndCapture();
}
