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

#define EST_CASE_MAX 64
#define EST_Q_ERROR_MAX_SAMPLES 128

typedef struct Est_Case Est_Case;
struct Est_Case
{
  char* label;
  char* sql;
  F64 max_q_error;
};

internal void
est_write_file(String8 path, String8 content)
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

// tec: grp cycles 0..9, skew is 7 on half the rows, cat leans toward the first word, maybe is NULL on a tenth of the rows
internal void
est_write_fact_csv(Arena* arena, String8 path, U64 row_count)
{
  String8List lines = {0};
  str8_list_pushf(arena, &lines, "id,grp,skew,val,cat,maybe\n");
  
  Bench_Rng rng = {0xE57157A7ULL + row_count};
  U64 word_count = ArrayCount(g_bench_words);
  for (U64 i = 0; i < row_count; i += 1)
  {
    U64 skew = bench_rng_next(&rng) % 100;
    if (bench_rng_next(&rng) % 2 == 0)
    {
      skew = 7;
    }
    
    F64 value = (F64)(bench_rng_next(&rng) % 100000) / 100.0;
    
    U64 word = bench_rng_next(&rng) % word_count;
    if (bench_rng_next(&rng) % 5 < 2)
    {
      word = 0;
    }
    
    B32 maybe_is_null = (bench_rng_next(&rng) % 10) == 0;
    U64 maybe = bench_rng_next(&rng) % 100;
    
    if (maybe_is_null)
    {
      str8_list_pushf(arena, &lines, "%llu,%llu,%llu,%.2f,%s,\n", i + 1, i % 10, skew, value, g_bench_words[word]);
    }
    else
    {
      str8_list_pushf(arena, &lines, "%llu,%llu,%llu,%.2f,%s,%llu\n", i + 1, i % 10, skew, value, g_bench_words[word], maybe);
    }
  }
  est_write_file(path, str8_list_join(arena, &lines, 0));
}

internal void
est_write_dimension_csv(Arena* arena, String8 path, U64 row_count)
{
  String8List lines = {0};
  str8_list_pushf(arena, &lines, "k,label\n");
  for (U64 i = 0; i < row_count; i += 1)
  {
    str8_list_pushf(arena, &lines, "%llu,d%llu\n", i, i);
  }
  est_write_file(path, str8_list_join(arena, &lines, 0));
}

internal void
est_add_case(Est_Case* cases, U64* count, char* label, char* sql, F64 max_q_error)
{
  if (*count >= EST_CASE_MAX)
  {
    printf("est_add_case: EST_CASE_MAX (%d) is too small for '%s'\n", EST_CASE_MAX, label);
    os_abort(1);
  }
  cases[*count].label = label;
  cases[*count].sql = sql;
  cases[*count].max_q_error = max_q_error;
  *count += 1;
}

internal U64
est_build_cases(Est_Case* cases)
{
  U64 count = 0;
  
  //- tec: single predicates
  est_add_case(cases, &count, "range on a unique column", "SELECT id FROM fact WHERE id < 5000;", 1.2);
  est_add_case(cases, &count, "range on a float", "SELECT id FROM fact WHERE val > 900;", 1.3);
  est_add_case(cases, &count, "equality, uniform column", "SELECT id FROM fact WHERE grp = 3;", 1.2);
  est_add_case(cases, &count, "equality, top value", "SELECT id FROM fact WHERE skew = 7;", 1.2);
  est_add_case(cases, &count, "equality, ordinary value", "SELECT id FROM fact WHERE skew = 42;", 2.0);
  est_add_case(cases, &count, "equality, value outside the range", "SELECT id FROM fact WHERE grp = 55;", 2.0);
  est_add_case(cases, &count, "not equal", "SELECT id FROM fact WHERE grp != 3;", 1.2);
  est_add_case(cases, &count, "string equality, top value", "SELECT id FROM fact WHERE cat = 'alpha';", 1.2);
  est_add_case(cases, &count, "string equality, ordinary value", "SELECT id FROM fact WHERE cat = 'kilo';", 2.0);
  est_add_case(cases, &count, "is null", "SELECT id FROM fact WHERE maybe IS NULL;", 1.2);
  est_add_case(cases, &count, "is not null", "SELECT id FROM fact WHERE maybe IS NOT NULL;", 1.2);
  est_add_case(cases, &count, "in list", "SELECT id FROM fact WHERE grp IN (1, 2, 3);", 1.2);
  est_add_case(cases, &count, "not in list", "SELECT id FROM fact WHERE grp NOT IN (1, 2, 3);", 1.2);
  
  //- tec: combined predicates
  est_add_case(cases, &count, "and of two independent columns", "SELECT id FROM fact WHERE val > 100 AND skew = 7;", 1.3);
  est_add_case(cases, &count, "or on one column", "SELECT id FROM fact WHERE grp = 3 OR grp = 4;", 1.2);
  est_add_case(cases, &count, "range between two bounds", "SELECT id FROM fact WHERE val > 200 AND val < 400;", 1.5);
  
  //- tec: joins
  est_add_case(cases, &count, "foreign key join", "SELECT fact.id, dim.label FROM fact JOIN dim ON fact.grp = dim.k;", 1.2);
  est_add_case(cases, &count, "join with a filter on the dimension", "SELECT fact.id FROM fact JOIN dim ON fact.grp = dim.k WHERE dim.k < 3;", 1.3);
  est_add_case(cases, &count, "join on a skewed key", "SELECT fact.id FROM fact JOIN dim100 ON fact.skew = dim100.k;", 1.2);
  est_add_case(cases, &count, "join with a filter on the fact table", "SELECT fact.id FROM fact JOIN dim ON fact.grp = dim.k WHERE fact.val > 500;", 1.3);
  
  //- tec: grouping and limits
  est_add_case(cases, &count, "group by a low cardinality column", "SELECT grp, COUNT(*) FROM fact GROUP BY grp;", 1.2);
  est_add_case(cases, &count, "group by a skewed column", "SELECT skew, COUNT(*) FROM fact GROUP BY skew;", 1.3);
  est_add_case(cases, &count, "group by two columns", "SELECT grp, cat, COUNT(*) FROM fact GROUP BY grp, cat;", 1.5);
  est_add_case(cases, &count, "aggregate without grouping", "SELECT COUNT(*) FROM fact;", 1.0);
  est_add_case(cases, &count, "limit", "SELECT id FROM fact ORDER BY val DESC LIMIT 100;", 1.0);
  est_add_case(cases, &count, "limit above the row count", "SELECT id FROM fact WHERE grp = 3 LIMIT 100000;", 1.2);
  
  return count;
}

//~ tec: reading q_error values out of EXPLAIN ANALYZE text

internal U64
est_parse_q_errors(String8 text, F64* out_values, U64 capacity)
{
  String8 needle = str8_lit("q_error=");
  U64 count = 0;
  U64 position = 0;
  while (position < text.size && count < capacity)
  {
    U64 found = str8_find_needle(text, position, needle, 0);
    if (found >= text.size)
    {
      break;
    }
    
    U64 value_start = found + needle.size;
    U64 value_end = value_start;
    while (value_end < text.size && (text.str[value_end] == '.' || (text.str[value_end] >= '0' && text.str[value_end] <= '9')))
    {
      value_end += 1;
    }
    
    String8 number = str8(text.str + value_start, value_end - value_start);
    out_values[count] = f64_from_str8(number);
    count += 1;
    position = value_end;
  }
  return count;
}

internal int
est_compare_f64(const void* a, const void* b)
{
  F64 left = *(const F64*)a;
  F64 right = *(const F64*)b;
  if (left < right)
  {
    return -1;
  }
  if (left > right)
  {
    return 1;
  }
  return 0;
}

internal void
est_run_suite(Arena* arena, Bench_Report* report, U64 row_count, char* label)
{
  printf("\n########## cardinality estimates: %s (%llu rows) ##########\n", label, row_count);
  bench_report_section(report, "cardinality estimates: %s (%llu rows)", label, row_count);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  String8 fact_path = push_str8f(scratch.arena, "bench_data/est_fact_%llu.csv", row_count);
  est_write_fact_csv(scratch.arena, fact_path, row_count);
  est_write_dimension_csv(scratch.arena, str8_lit("bench_data/est_dim.csv"), 10);
  est_write_dimension_csv(scratch.arena, str8_lit("bench_data/est_dim100.csv"), 100);
  
  GDB_Database* database = gdb_database_alloc(push_str8f(scratch.arena, "estimates_db_%llu", row_count));
  gdb_add_database(database);
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("fact"), fact_path));
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("dim"), str8_lit("bench_data/est_dim.csv")));
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("dim100"), str8_lit("bench_data/est_dim100.csv")));
  
  Est_Case cases[EST_CASE_MAX];
  U64 case_count = est_build_cases(cases);
  
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));
  F64 all_worst[EST_CASE_MAX] = {0};
  U64 failures = 0;
  
  printf("  %-42s %8s %8s %s\n", "query", "worst q", "bound", "");
  for (U64 index = 0; index < case_count; index += 1)
  {
    Est_Case* test_case = &cases[index];
    String8 query = push_str8f(query_arena, "EXPLAIN ANALYZE %s", test_case->sql);
    APP_QueryResult result = app_execute_query_capture(query_arena, query, &database, NULL);
    
    F64 values[EST_Q_ERROR_MAX_SAMPLES] = {0};
    U64 value_count = est_parse_q_errors(result.output_text, values, EST_Q_ERROR_MAX_SAMPLES);
    
    F64 worst = 0.0;
    for (U64 value_index = 0; value_index < value_count; value_index += 1)
    {
      worst = Max(worst, values[value_index]);
    }
    all_worst[index] = worst;
    
    B32 ok = value_count > 0 && worst <= test_case->max_q_error;
    printf("  %-42s %8.2f %8.2f %s\n", test_case->label, worst, test_case->max_q_error, ok ? "OK" : "FAIL");
    if (!ok)
    {
      failures += 1;
      bench_report_warn(report, "'%s' worst q_error=%.2f exceeds %.2f (%llu nodes reported)", test_case->label, worst, test_case->max_q_error, value_count);
      printf("%.*s\n", str8_varg(result.output_text));
    }
    bench_report_text(report, "%s: worst q_error=%.2f bound=%.2f", test_case->label, worst, test_case->max_q_error);
    arena_clear(query_arena);
  }
  
  quick_sort(all_worst, case_count, sizeof(F64), est_compare_f64);
  F64 median = all_worst[case_count / 2];
  printf("\n  median of the worst q_error per query: %.2f, %llu of %llu over their bound\n", median, failures, case_count);
  bench_report_text(report, "median of the worst q_error per query: %.2f, %llu of %llu over their bound", median, failures, case_count);
  
  arena_release(query_arena);
  scratch_end(scratch);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  setvbuf(stdout, NULL, _IONBF, 0);
  log_alloc();
  g_query_exec_mutex = os_mutex_alloc();
  gdb_init();
  gpu_init();
  
  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql cardinality estimates");
  
  // tec: fits inside the statistics sample, so the histogram and top values are exact
  est_run_suite(arena, report, 50000, "exact statistics");
  // tec: past the sample, everything comes from a systematic sample and HyperLogLog
  est_run_suite(arena, report, 200000, "sampled statistics");
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/estimates_report.md"));
  
  arena_release(arena);
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
