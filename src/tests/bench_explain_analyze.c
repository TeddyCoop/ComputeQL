// compute_ql: EXPLAIN ANALYZE correctness.
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

internal B32
ea_contains(String8 haystack, String8 needle)
{
  return str8_find_needle(haystack, 0, needle, 0) < haystack.size;
}

internal void
ea_check_contains(Bench_Report* report, String8 label, String8 output, char* needle)
{
  B32 ok = ea_contains(output, str8_cstring(needle));
  printf("  %-40.*s %s ('%s')\n", str8_varg(label), ok ? "OK" : "FAIL", needle);
  if (!ok)
  {
    bench_report_warn(report, "'%.*s' expected output to contain '%s' but it didn't", str8_varg(label), needle);
  }
}

internal void
ea_check_not_contains(Bench_Report* report, String8 label, String8 output, char* needle)
{
  B32 ok = !ea_contains(output, str8_cstring(needle));
  printf("  %-40.*s %s (should not contain '%s')\n", str8_varg(label), ok ? "OK" : "FAIL", needle);
  if (!ok)
  {
    bench_report_warn(report, "'%.*s' expected output to NOT contain '%s' but it did", str8_varg(label), needle);
  }
}

internal void
ea_write_csv(Arena* arena, String8 path, String8 content)
{
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if (!os_handle_match(file, os_handle_zero()))
  {
    os_file_write(file, r1u64(0, content.size), content.str);
    os_file_close(file);
  }
}

internal void
ea_run_suite(Arena* arena, Bench_Report* report)
{
  printf("\n########## EXPLAIN ANALYZE suite ##########\n");
  bench_report_section(report, "EXPLAIN ANALYZE suite");
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: 'fact' - no NULLs, big enough to exercise zone-map pruning and dictionary encoding.
  // 'value' is monotonically increasing with row order so zone-map chunks each cover a distinct
  // sub-range, and 'category' cycles through 4 distinct strings (well under the dict-encoding
  // cardinality-ratio threshold) so a dictionary gets built.
  U64 fact_row_count = 50000;
  char* categories[4] = { "apple", "banana", "cherry", "date" };
  {
    String8List lines = {0};
    str8_list_pushf(scratch.arena, &lines, "id,group_key,category,value\n");
    for (U64 i = 0; i < fact_row_count; i++)
    {
      str8_list_pushf(scratch.arena, &lines, "%llu,%llu,%s,%llu\n", i + 1, i % 10, categories[i % 4], i);
    }
    ea_write_csv(scratch.arena, str8_lit("bench_data/explain_fact.csv"), str8_list_join(scratch.arena, &lines, 0));
  }
  
  //- tec: 'dim' - small lookup table for the JOIN case
  {
    String8List lines = {0};
    str8_list_pushf(scratch.arena, &lines, "dim_key,dim_name\n");
    for (U64 g = 0; g < 10; g++)
    {
      str8_list_pushf(scratch.arena, &lines, "%llu,g%llu\n", g, g);
    }
    ea_write_csv(scratch.arena, str8_lit("bench_data/explain_dim.csv"), str8_list_join(scratch.arena, &lines, 0));
  }
  
  //- tec: 'nullable_fact' - a table with at least one NULL, to force the CPU-scan fallback path
  // (gdb_table_may_have_nulls checks the whole table, so this needs its own table separate from
  // 'fact' - a nullable column on 'fact' would force every filter on it through the CPU path too)
  {
    String8List lines = {0};
    str8_list_pushf(scratch.arena, &lines, "id,flag\n");
    str8_list_pushf(scratch.arena, &lines, "1,10\n");
    str8_list_pushf(scratch.arena, &lines, "2,\n"); // tec: empty field -> NULL
    str8_list_pushf(scratch.arena, &lines, "3,30\n");
    ea_write_csv(scratch.arena, str8_lit("bench_data/explain_nullable.csv"), str8_list_join(scratch.arena, &lines, 0));
  }
  
  GDB_Database* database = gdb_database_alloc(str8_lit("explain_analyze_db"));
  gdb_add_database(database);
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("fact"), str8_lit("bench_data/explain_fact.csv")));
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("dim"), str8_lit("bench_data/explain_dim.csv")));
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("nullable_fact"), str8_lit("bench_data/explain_nullable.csv")));
  
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));
  
  //- tec: zone-map pruning - most of 'value's range is below the threshold, so most zone-map
  // chunks should be provably empty and get pruned
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT * FROM fact WHERE value > 45000;"), &database, NULL);
    ea_check_contains(report, str8_lit("zone-map pruning: 'pruned'"), result.output_text, "pruned");
    ea_check_contains(report, str8_lit("zone-map pruning: 'zone map'"), result.output_text, "zone map");
    ea_check_contains(report, str8_lit("zone-map pruning: scan timing ('gpu=')"), result.output_text, "gpu=");
    ea_check_contains(report, str8_lit("zone-map pruning: scan timing ('ms')"), result.output_text, "ms");
    arena_clear(query_arena);
  }
  
  //- tec: dictionary hit - 'apple' exists in the category dictionary
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT * FROM fact WHERE category = 'apple';"), &database, NULL);
    ea_check_contains(report, str8_lit("dict hit: 'dict=hit'"), result.output_text, "dict=hit");
    ea_check_contains(report, str8_lit("dict hit: scan timing ('gpu=')"), result.output_text, "gpu=");
    arena_clear(query_arena);
  }
  
  //- tec: dictionary miss
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT * FROM fact WHERE category = 'nonexistent';"), &database, NULL);
    ea_check_contains(report, str8_lit("dict miss: 'dict=miss'"), result.output_text, "dict=miss");
    ea_check_contains(report, str8_lit("dict miss: '0 rows scanned'"), result.output_text, "0 rows scanned");
    ea_check_contains(report, str8_lit("dict miss: scan still timed ('ms')"), result.output_text, "ms");
    arena_clear(query_arena);
  }
  
  //- tec: GROUP BY
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT group_key, COUNT(*) FROM fact GROUP BY group_key;"), &database, NULL);
    ea_check_contains(report, str8_lit("GROUP BY: '[Aggregate]'"), result.output_text, "[Aggregate]");
    ea_check_contains(report, str8_lit("GROUP BY: 'groups'"), result.output_text, "groups");
    ea_check_contains(report, str8_lit("GROUP BY: gather phase timing ('gather=')"), result.output_text, "gather=");
    ea_check_contains(report, str8_lit("GROUP BY: assign phase timing ('assign=')"), result.output_text, "assign=");
    ea_check_contains(report, str8_lit("GROUP BY: reduce phase timing ('reduce=')"), result.output_text, "reduce=");
    ea_check_contains(report, str8_lit("GROUP BY: combine phase timing ('combine=')"), result.output_text, "combine=");
    arena_clear(query_arena);
  }
  
  //- tec: JOIN
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT dim.dim_name, fact.id FROM dim JOIN fact ON dim.dim_key = fact.group_key;"), &database, NULL);
    ea_check_contains(report, str8_lit("JOIN: '[Join]'"), result.output_text, "[Join]");
    ea_check_contains(report, str8_lit("JOIN: 'build_rows'"), result.output_text, "build_rows");
    ea_check_contains(report, str8_lit("JOIN: 'probe_rows'"), result.output_text, "probe_rows");
    ea_check_contains(report, str8_lit("JOIN: build phase timing ('build=')"), result.output_text, "build=");
    ea_check_contains(report, str8_lit("JOIN: probe phase timing ('probe=')"), result.output_text, "probe=");
    ea_check_contains(report, str8_lit("JOIN: download phase timing ('download=')"), result.output_text, "download=");
    arena_clear(query_arena);
  }
  
  //- tec: NULL-containing table forces the CPU-scan fallback
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT * FROM nullable_fact WHERE flag > 5;"), &database, NULL);
    ea_check_contains(report, str8_lit("CPU fallback: 'CPU scan'"), result.output_text, "CPU scan");
    ea_check_contains(report, str8_lit("CPU fallback: 'NULLs'"), result.output_text, "NULLs");
    ea_check_contains(report, str8_lit("CPU fallback: scan timing ('time=')"), result.output_text, "time=");
    arena_clear(query_arena);
  }
  
  //- tec: ORDER BY on a plain (non aggregated) row set
  // exercises the bitonic sort GPU timing
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT * FROM fact ORDER BY value DESC LIMIT 10;"), &database, NULL);
    ea_check_contains(report, str8_lit("ORDER BY: '[Sort]'"), result.output_text, "[Sort]");
    ea_check_contains(report, str8_lit("ORDER BY: sort phase timing ('gpu=')"), result.output_text, "gpu=");
    arena_clear(query_arena);
  }
  
  //- tec: regression guard - plain EXPLAIN (no ANALYZE) must stay a static plan dump, no stats at all
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT * FROM fact WHERE value > 45000;"), &database, NULL);
    ea_check_not_contains(report, str8_lit("plain EXPLAIN: no 'pruned'"), result.output_text, "pruned");
    ea_check_not_contains(report, str8_lit("plain EXPLAIN: no 'gpu='"), result.output_text, "gpu=");
    ea_check_not_contains(report, str8_lit("plain EXPLAIN: no 'analyzed'"), result.output_text, "analyzed");
    ea_check_not_contains(report, str8_lit("plain EXPLAIN: no timing ('ms')"), result.output_text, "ms");
    ea_check_contains(report, str8_lit("plain EXPLAIN: still has 'Query plan:'"), result.output_text, "Query plan:");
    arena_clear(query_arena);
  }
  
  arena_release(query_arena);
  scratch_end(scratch);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  g_query_exec_mutex = os_mutex_alloc();
  gdb_init();
  gpu_init();
  
  Arena* arena = arena_alloc(.reserve_size = MB(256), .commit_size = MB(16));
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql EXPLAIN ANALYZE correctness");
  
  ea_run_suite(arena, report);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/explain_analyze_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
