// compute_ql: the bytecode interpreter (scan_filter) against the fixed function comparison kernel (scan_simple) on the same filters, 1 to 8 predicates.
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)
#define BENCH_TIMED_RUNS 9
#define BENCH_WARMUP_RUNS 3
#define BENCH_WARMUP_MIN_MICROSECONDS 300000

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

#define IO_COLUMNS 8
#define IO_THRESHOLD 4294000000u
// tec: --selective makes the last comparison c_j < this (about 0.01% of rows), so almost no row matches and the scan writes almost nothing, while every comparison before it still runs
#define IO_SELECTIVE_THRESHOLD 429497u

global U64 g_io_multipliers[IO_COLUMNS] = { 2654435761ULL, 2246822519ULL, 3266489917ULL, 668265263ULL, 374761393ULL, 2870177453ULL, 3141592653ULL, 1597334677ULL };

internal void
io_load_table(CQL_Database* database, U64 row_count)
{
  CQL_Table* table = cql_table_alloc(str8_lit("t"));
  for (U32 c = 0; c < IO_COLUMNS; c++)
  {
    String8 name = push_str8f(g_cql_state->arena, "c%u", c);
    cql_table_add_column(table, cql_column_schema_create(name, CQL_ColumnType_U32));
  }
  cql_database_add_table(database, table);
  for (U32 c = 0; c < IO_COLUMNS; c++)
  {
    U32* data = (U32*)cql_column_alloc_fixed_rows(table->columns[c], row_count);
    if (data == 0)
    {
      log_error("could not allocate %llu rows", row_count);
      return;
    }
    for (U64 row = 0; row < row_count; row++)
    {
      data[row] = (U32)((row + c) * g_io_multipliers[c]);
    }
  }
  table->row_count = row_count;
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  
  U64 row_count = 10000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    row_count = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  g_bench_print_samples = cmd_line_has_flag(cmdline, str8_lit("samples"));
  B32 selective = cmd_line_has_flag(cmdline, str8_lit("selective"));
  {
    char setting[96];
    snprintf(setting, sizeof(setting), "CQL_COLUMN_ARENA_RESERVE_SIZE:%llu", row_count * sizeof(U32) + MB(256));
    settings_load_from_string(str8_cstring(setting));
  }
  String8List setting_list = cmd_line_strings(cmdline, str8_lit("setting"));
  for (String8Node* setting_node = setting_list.first; setting_node; setting_node = setting_node->next)
  {
    settings_load_from_string(setting_node->string);
  }
  
  cql_init();
  gpu_init();
  
  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql bytecode interpreter against scan_simple");
  bench_report_text(report, "t: %llu rows, %u U32 columns over the whole 32 bit range; each predicate is c_j < %u (about 99.98%% of rows); %u timed runs per query",
                    row_count, (U32)IO_COLUMNS, IO_THRESHOLD, (U32)BENCH_TIMED_RUNS);
  
  CQL_Database* database = cql_database_alloc(str8_lit("interp_db"));
  cql_add_database(database);
  io_load_table(database, row_count);
  
  bench_report_section(report, "predicates (%llu rows)%s", row_count, selective ? ", the last one selective (--selective)" : "");
  bench_report_row_begin(report);
  bench_report_cellf(report, "predicates");
  bench_report_cellf(report, "kernel");
  bench_report_cellf(report, "count");
  bench_report_cellf(report, "exec min (ms)");
  bench_report_cellf(report, "exec med (ms)");
  bench_report_cellf(report, "exec max (ms)");
  bench_report_cellf(report, "GB read");
  bench_report_cellf(report, "GB/s at median");
  bench_report_cellf(report, "kernels dispatched");
  bench_report_table_header_end(report);
  
  U32 predicate_counts[] = { 1, 2, 4, 8 };
  U64 mismatches = 0;
  for (U32 p = 0; p < ArrayCount(predicate_counts); p++)
  {
    U32 k = predicate_counts[p];
    Temp scratch = scratch_begin(0, 0);
    String8List sql_parts = {0};
    str8_list_pushf(scratch.arena, &sql_parts, "SELECT COUNT(*) FROM t WHERE ");
    for (U32 c = 0; c < k; c++)
    {
      U32 threshold = (selective && c + 1 == k) ? IO_SELECTIVE_THRESHOLD : IO_THRESHOLD;
      str8_list_pushf(scratch.arena, &sql_parts, "%sc%u < %u", c == 0 ? "" : " AND ", c, threshold);
    }
    str8_list_pushf(scratch.arena, &sql_parts, ";");
    String8 sql = str8_list_join(scratch.arena, &sql_parts, 0);
    
    U64 checksums[2] = {0};
    for (U32 mode = 0; mode < 2; mode++)
    {
      settings_load_from_string(mode == 0 ? str8_lit("QE_SCAN_SIMPLE_KERNEL:1") : str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
      gpu_kernel_log_reset();
      U64 rows = 0;
      Bench_Stats stats = bench_run_cql_query(database, sql, &rows, &checksums[mode]);
      String8 kernels = bench_kernel_log_string(scratch.arena);
      F64 gigabytes = (F64)(row_count * k * sizeof(U32)) / 1e9;
      char* mode_name = mode == 0 ? "scan_simple allowed" : "interpreter";
      printf("%u predicates, %-20s exec median %8.3f ms (min %.3f max %.3f) %6.1f GB/s  kernels: %.*s\n",
             k, mode_name, stats.execute_median, stats.execute_min, stats.execute_max, gigabytes / (stats.execute_median / 1000.0), str8_varg(kernels));
      bench_report_row_begin(report);
      bench_report_cellf(report, "%u", k);
      bench_report_cellf(report, "%s", mode_name);
      bench_report_cellf(report, "%llu", checksums[mode]);
      bench_report_cellf(report, "%.3f", stats.execute_min);
      bench_report_cellf(report, "%.3f", stats.execute_median);
      bench_report_cellf(report, "%.3f", stats.execute_max);
      bench_report_cellf(report, "%.3f", gigabytes);
      bench_report_cellf(report, "%.1f", gigabytes / (stats.execute_median / 1000.0));
      bench_report_cellf(report, "%.*s", str8_varg(kernels));
      bench_report_row_end(report);
      fflush(stdout);
    }
    if (checksums[0] != checksums[1])
    {
      mismatches += 1;
      bench_report_warn(report, "%u predicates: the two kernels disagree", k);
      printf("  !! MISMATCH: %u predicates, the two kernels disagree\n", k);
    }
    scratch_end(scratch);
  }
  settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  char report_path[96];
  snprintf(report_path, sizeof(report_path), "bench_reports/interp_overhead_%llu%s_report.md", row_count, selective ? "_selective" : "");
  bench_report_write(report, str8_cstring(report_path));
  
  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (mismatches > 0)
  {
    exit(1);
  }
}
