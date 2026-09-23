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

typedef struct FS_Row FS_Row;
struct FS_Row
{
  U32 id;
  String8 text;
};

internal FS_Row*
fs_generate_rows(Arena* arena, U64 row_count, U64 rng_seed)
{
  FS_Row* rows = push_array(arena, FS_Row, row_count);
  Bench_Rng rng = {rng_seed};
  U64 word_count = ArrayCount(g_bench_words);
  
  for (U64 i = 0; i < row_count; i++)
  {
    rows[i].id = (U32)(i + 1);
    
    U64 words_in_phrase = 2 + (bench_rng_next(&rng) % 2);
    String8List parts = {0};
    for (U64 w = 0; w < words_in_phrase; w++)
    {
      char* word = g_bench_words[bench_rng_next(&rng) % word_count];
      str8_list_pushf(arena, &parts, "%s", word);
    }
    String8 phrase = str8_list_join(arena, &parts, &(StringJoin){.sep = str8_lit(" ")});
    
    // tec: mutate ~1 in 3 rows with a single byte substitution, to exercise a spread of
    // similarity scores/edit distances instead of only exact-word matches
    if (bench_rng_next(&rng) % 3 == 0 && phrase.size > 0)
    {
      U64 mutate_at = bench_rng_next(&rng) % phrase.size;
      U8 replacement = (U8)('a' + (bench_rng_next(&rng) % 26));
      phrase.str[mutate_at] = replacement;
    }
    
    rows[i].text = phrase;
  }
  return rows;
}

// tec: `value` is an otherwise-unused numeric column - row 0's is left blank in the CSV to give
// the "forced CPU fallback" table exactly one NULL, tripping gdb_table_may_have_nulls for the
// whole table (qe_row_condition_eval's fuzzy branch doesn't care, it never touches `value`)
internal B32
fs_write_csv(String8 path, FS_Row* rows, U64 row_count, B32 null_first_value)
{
  Temp scratch = scratch_begin(0, 0);
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,text,value\n");
  for (U64 i = 0; i < row_count; i++)
  {
    if (null_first_value && i == 0)
    {
      str8_list_pushf(scratch.arena, &lines, "%u,%.*s,\n", rows[i].id, str8_varg(rows[i].text));
    }
    else
    {
      str8_list_pushf(scratch.arena, &lines, "%u,%.*s,%u\n", rows[i].id, str8_varg(rows[i].text), (U32)i);
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

internal U32*
fs_run_and_collect_ids(Arena* arena, GDB_Database* database, String8 sql_text, QE_TraceStrategy* out_strategy, U64* out_count)
{
  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;
  ir_expand_star_to_columns(arena, database, select_node);
  PLAN_Node* plan = plan_build_from_select(arena, database, select_node);
  
  QE_TraceCtx* trace = qe_trace_ctx_alloc(arena);
  PLAN_ExecResult result = plan_execute(arena, database, plan, select_node, trace);
  
  *out_count = 0;
  if (out_strategy) *out_strategy = QE_TraceStrategy_None;
  if (!result.supported || result.is_materialized || result.rows.table_count != 1) return 0;
  
  if (out_strategy)
  {
    for (QE_NodeTrace* nt = trace->records; nt; nt = nt->next)
    {
      if (nt->node_type == PLAN_NodeType_Filter || nt->node_type == PLAN_NodeType_Scan)
      {
        *out_strategy = nt->scan.strategy;
      }
    }
  }
  
  GDB_Column* id_column = gdb_table_find_column(result.rows.tables[0], str8_lit("id"));
  F64* id_values = qe_gather_numeric_column(arena, &result.rows, 0, id_column);
  
  U64 count = result.rows.count;
  U32* ids = push_array(arena, U32, Max(count, 1));
  for (U64 i = 0; i < count; i++) ids[i] = (U32)id_values[i];
  
  *out_count = count;
  return ids;
}

internal void
fs_sort_u32(U32* a, U64 n)
{
  for (U64 i = 1; i < n; i++)
  {
    U32 key = a[i];
    S64 j = (S64)i - 1;
    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
    a[j + 1] = key;
  }
}

// tec: compares as sets (sorted arrays) since scan_filter.comp's atomicAdd match ordering isnt guaranteed to match row order
internal void
fs_check_id_set_match(Arena* arena, Bench_Report* report, String8 label,
                      U32* expected, U64 expected_count, U32* actual, U64 actual_count)
{
  Temp scratch = scratch_begin(&arena, 1);
  U32* exp_sorted = push_array(scratch.arena, U32, Max(expected_count, 1));
  U32* act_sorted = push_array(scratch.arena, U32, Max(actual_count, 1));
  MemoryCopy(exp_sorted, expected, expected_count * sizeof(U32));
  MemoryCopy(act_sorted, actual, actual_count * sizeof(U32));
  fs_sort_u32(exp_sorted, expected_count);
  fs_sort_u32(act_sorted, actual_count);
  
  B32 ok = (expected_count == actual_count);
  if (ok)
  {
    for (U64 i = 0; i < expected_count; i++)
    {
      if (exp_sorted[i] != act_sorted[i]) { ok = 0; break; }
    }
  }
  
  printf("  %-48.*s expected=%6llu actual=%6llu %s\n", str8_varg(label), expected_count, actual_count, ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "fuzzy search id-set mismatch on '%.*s': expected %llu rows, got %llu",
                      str8_varg(label), expected_count, actual_count);
  }
  scratch_end(scratch);
}

internal void
fs_check_strategy(Bench_Report* report, String8 label, QE_TraceStrategy actual, QE_TraceStrategy expected)
{
  B32 ok = (actual == expected);
  printf("  %-48.*s strategy=%d (expected %d) %s\n", str8_varg(label), actual, expected, ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "'%.*s' expected scan strategy %d, got %d", str8_varg(label), expected, actual);
  }
}

internal void
fs_run_suite(Arena* arena, Bench_Report* report, U64 row_count)
{
  printf("\n########## fuzzy search suite (%llu rows) ##########\n", row_count);
  bench_report_section(report, "fuzzy search suite (%llu rows)", row_count);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  FS_Row* rows = fs_generate_rows(scratch.arena, row_count, 0xF0220B0E5EA5C4ULL);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  
  //- tec: GPU path table. without nulls
  String8 gpu_csv_path = str8_lit("bench_data/fuzzy_search_gpu.csv");
  fs_write_csv(gpu_csv_path, rows, row_count, 0);
  GDB_Database* gpu_db = gdb_database_alloc(str8_lit("fuzzy_gpu_db"));
  gdb_add_database(gpu_db);
  GDB_Table* gpu_table = gdb_table_import_csv_streaming(gpu_db, str8_lit("t"), gpu_csv_path);
  gdb_database_add_table(gpu_db, gpu_table);
  
  //- tec: CPU fallback table. same rows, but one NULL in an unrelated column forces qe_cpu_scan_filter for the whole table (gdb_table_may_have_nulls)
  String8 cpu_csv_path = str8_lit("bench_data/fuzzy_search_cpu.csv");
  fs_write_csv(cpu_csv_path, rows, row_count, 1);
  GDB_Database* cpu_db = gdb_database_alloc(str8_lit("fuzzy_cpu_db"));
  gdb_add_database(cpu_db);
  GDB_Table* cpu_table = gdb_table_import_csv_streaming(cpu_db, str8_lit("t"), cpu_csv_path);
  gdb_database_add_table(cpu_db, cpu_table);
  
  char* needles[] = { "alpha bravo", "charlie", "zzz not present", g_bench_words[7] /* "hotel" */ };
  F64 sim_thresholds[] = { 0.5, 0.2, 0.1, 0.6 };
  U64 edit_thresholds[] = { 3, 2, 1, 4 };
  
  for (U32 c = 0; c < ArrayCount(needles); c++)
  {
    String8 needle = str8_cstring(needles[c]);
    
    //- tec: SIMILARITY() - direct CPU reference over the generator's own rows
    {
      U64 expected_count = 0;
      U32* expected = push_array(scratch.arena, U32, row_count);
      for (U64 i = 0; i < row_count; i++)
      {
        if (qe_str8_trigram_similarity(rows[i].text, needle) > sim_thresholds[c])
        {
          expected[expected_count++] = rows[i].id;
        }
      }
      
      String8 sql = push_str8f(scratch.arena, "SELECT id FROM t WHERE SIMILARITY(text, '%.*s') > %.2f;", str8_varg(needle), sim_thresholds[c]);
      
      QE_TraceStrategy gpu_strategy = QE_TraceStrategy_None;
      U64 gpu_count = 0;
      U32* gpu_ids = fs_run_and_collect_ids(scratch.arena, gpu_db, sql, &gpu_strategy, &gpu_count);
      String8 gpu_label = push_str8f(scratch.arena, "SIMILARITY '%.*s' > %.2f (GPU)", str8_varg(needle), sim_thresholds[c]);
      fs_check_id_set_match(scratch.arena, report, gpu_label, expected, expected_count, gpu_ids, gpu_count);
      fs_check_strategy(report, gpu_label, gpu_strategy, QE_TraceStrategy_GpuScan);
      
      QE_TraceStrategy cpu_strategy = QE_TraceStrategy_None;
      U64 cpu_count = 0;
      U32* cpu_ids = fs_run_and_collect_ids(scratch.arena, cpu_db, sql, &cpu_strategy, &cpu_count);
      String8 cpu_label = push_str8f(scratch.arena, "SIMILARITY '%.*s' > %.2f (CPU fallback)", str8_varg(needle), sim_thresholds[c]);
      fs_check_id_set_match(scratch.arena, report, cpu_label, expected, expected_count, cpu_ids, cpu_count);
      fs_check_strategy(report, cpu_label, cpu_strategy, QE_TraceStrategy_CpuScan);
    }
    
    //- tec: EDIT_DISTANCE() - same shape
    {
      U64 expected_count = 0;
      U32* expected = push_array(scratch.arena, U32, row_count);
      for (U64 i = 0; i < row_count; i++)
      {
        if (qe_str8_edit_distance(rows[i].text, needle) <= edit_thresholds[c])
        {
          expected[expected_count++] = rows[i].id;
        }
      }
      
      String8 sql = push_str8f(scratch.arena, "SELECT id FROM t WHERE EDIT_DISTANCE(text, '%.*s') <= %llu;", str8_varg(needle), edit_thresholds[c]);
      
      QE_TraceStrategy gpu_strategy = QE_TraceStrategy_None;
      U64 gpu_count = 0;
      U32* gpu_ids = fs_run_and_collect_ids(scratch.arena, gpu_db, sql, &gpu_strategy, &gpu_count);
      String8 gpu_label = push_str8f(scratch.arena, "EDIT_DISTANCE '%.*s' <= %llu (GPU)", str8_varg(needle), edit_thresholds[c]);
      fs_check_id_set_match(scratch.arena, report, gpu_label, expected, expected_count, gpu_ids, gpu_count);
      fs_check_strategy(report, gpu_label, gpu_strategy, QE_TraceStrategy_GpuScan);
      
      QE_TraceStrategy cpu_strategy = QE_TraceStrategy_None;
      U64 cpu_count = 0;
      U32* cpu_ids = fs_run_and_collect_ids(scratch.arena, cpu_db, sql, &cpu_strategy, &cpu_count);
      String8 cpu_label = push_str8f(scratch.arena, "EDIT_DISTANCE '%.*s' <= %llu (CPU fallback)", str8_varg(needle), edit_thresholds[c]);
      fs_check_id_set_match(scratch.arena, report, cpu_label, expected, expected_count, cpu_ids, cpu_count);
      fs_check_strategy(report, cpu_label, cpu_strategy, QE_TraceStrategy_CpuScan);
    }
  }
  
  scratch_end(scratch);
}

internal void
fs_run_edge_cases(Arena* arena, Bench_Report* report)
{
  printf("\n########## fuzzy search suite: edge cases ##########\n");
  bench_report_section(report, "fuzzy search suite: edge cases");
  
  //- tec: identical strings -> similarity exactly 1.0, edit distance exactly 0
  {
    String8 a = str8_lit("alpha bravo charlie");
    F64 sim = qe_str8_trigram_similarity(a, a);
    U64 dist = qe_str8_edit_distance(a, a);
    B32 ok = (sim == 1.0 && dist == 0);
    printf("  %-48s sim=%.4f dist=%llu %s\n", "identical strings", sim, dist, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "identical strings: expected sim=1.0 dist=0, got sim=%.4f dist=%llu", sim, dist);
  }
  
  //- tec: completely disjoint strings -> similarity 0.0
  {
    String8 a = str8_lit("aaa");
    String8 b = str8_lit("zzz");
    F64 sim = qe_str8_trigram_similarity(a, b);
    B32 ok = (sim == 0.0);
    printf("  %-48s sim=%.4f %s\n", "disjoint trigrams", sim, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "disjoint trigrams: expected sim=0.0, got %.4f", sim);
  }
  
  //- tec: empty needle vs non-empty haystack -> similarity 0.0 (neither has a full n-gram)
  {
    String8 a = str8_lit("alpha");
    String8 b = str8_lit("");
    F64 sim = qe_str8_trigram_similarity(a, b);
    B32 ok = (sim == 0.0);
    printf("  %-48s sim=%.4f %s\n", "empty needle", sim, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "empty needle: expected sim=0.0, got %.4f", sim);
  }
  
  //- tec: textbook edit distance ("kitten" -> "sitting" = 3)
  {
    U64 dist = qe_str8_edit_distance(str8_lit("kitten"), str8_lit("sitting"));
    B32 ok = (dist == 3);
    printf("  %-48s dist=%llu (expected 3) %s\n", "kitten -> sitting", dist, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "kitten->sitting: expected edit distance 3, got %llu", dist);
  }
  
  //- tec: edit distance is symmetric
  {
    U64 d1 = qe_str8_edit_distance(str8_lit("kitten"), str8_lit("sitting"));
    U64 d2 = qe_str8_edit_distance(str8_lit("sitting"), str8_lit("kitten"));
    B32 ok = (d1 == d2);
    printf("  %-48s d1=%llu d2=%llu %s\n", "edit distance symmetry", d1, d2, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "edit distance not symmetric: %llu vs %llu", d1, d2);
  }
}

// tec: ranked retrieval (ORDER BY SIMILARITY()/EDIT_DISTANCE())
internal void
fs_run_ranking_suite(Arena* arena, Bench_Report* report, U64 row_count)
{
  printf("\n########## fuzzy search suite: ranking (%llu rows) ##########\n", row_count);
  bench_report_section(report, "fuzzy search suite: ranking (%llu rows)", row_count);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  FS_Row* rows = fs_generate_rows(scratch.arena, row_count, 0xA11CE5EED1234ULL);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = str8_lit("bench_data/fuzzy_search_ranking.csv");
  fs_write_csv(csv_path, rows, row_count, 0);
  GDB_Database* database = gdb_database_alloc(str8_lit("fuzzy_ranking_db"));
  gdb_add_database(database);
  GDB_Table* table = gdb_table_import_csv_streaming(database, str8_lit("t"), csv_path);
  gdb_database_add_table(database, table);
  
  String8 needle = str8_lit("alpha bravo");
  
  //- tec: WHERE and ORDER BY use the same fuzzy call
  {
    String8 sql = push_str8f(scratch.arena, "SELECT id FROM t WHERE SIMILARITY(text, '%.*s') > 0.05 ORDER BY SIMILARITY(text, '%.*s') DESC LIMIT 20;",
                             str8_varg(needle), str8_varg(needle));
    U64 count = 0;
    U32* ids = fs_run_and_collect_ids(scratch.arena, database, sql, 0, &count);
    
    B32 ok = (count > 0);
    F64 prev = 2.0; // tec: above the [0,1] similarity range
    for (U64 i = 0; i < count; i++)
    {
      F64 sim = qe_str8_trigram_similarity(rows[ids[i] - 1].text, needle);
      if (sim > prev) { ok = 0; break; }
      prev = sim;
    }
    printf("  %-48s count=%llu %s\n", "ORDER BY SIMILARITY DESC (WHERE matches)", count, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "ORDER BY SIMILARITY DESC (WHERE matches): scores not monotonically non-increasing, or zero rows");
  }
  
  //- tec: no WHERE at all
  {
    String8 sql = push_str8f(scratch.arena, "SELECT id FROM t ORDER BY SIMILARITY(text, '%.*s') DESC LIMIT 20;", str8_varg(needle));
    U64 count = 0;
    U32* ids = fs_run_and_collect_ids(scratch.arena, database, sql, 0, &count);
    
    B32 ok = (count > 0);
    F64 prev = 2.0;
    for (U64 i = 0; i < count; i++)
    {
      F64 sim = qe_str8_trigram_similarity(rows[ids[i] - 1].text, needle);
      if (sim > prev) { ok = 0; break; }
      prev = sim;
    }
    printf("  %-48s count=%llu %s\n", "ORDER BY SIMILARITY DESC (no WHERE)", count, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "ORDER BY SIMILARITY DESC (no WHERE): scores not monotonically non-increasing, or zero rows");
  }
  
  //- tec: WHERE and ORDER BY use *different* fuzzy calls
  {
    String8 other_needle = str8_lit("charlie");
    String8 sql = push_str8f(scratch.arena, "SELECT id FROM t WHERE SIMILARITY(text, '%.*s') > 0.01 ORDER BY EDIT_DISTANCE(text, '%.*s') ASC LIMIT 20;",
                             str8_varg(needle), str8_varg(other_needle));
    U64 count = 0;
    U32* ids = fs_run_and_collect_ids(scratch.arena, database, sql, 0, &count);
    
    B32 ok = (count > 0);
    F64 prev = -1.0;
    for (U64 i = 0; i < count; i++)
    {
      F64 dist = (F64)qe_str8_edit_distance(rows[ids[i] - 1].text, other_needle);
      if (prev >= 0.0 && dist < prev) { ok = 0; break; }
      prev = dist;
    }
    printf("  %-48s count=%llu %s\n", "ORDER BY EDIT_DISTANCE ASC (mismatched WHERE)", count, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "ORDER BY EDIT_DISTANCE ASC (mismatched WHERE): scores not monotonically non-decreasing, or zero rows - possible stale score reuse");
  }
  
  //- tec: SELECT-list score exposur
  {
    String8 sql = push_str8f(scratch.arena,
                             "SELECT id, SIMILARITY(text, '%.*s') AS score FROM t WHERE SIMILARITY(text, '%.*s') > 0.05 ORDER BY SIMILARITY(text, '%.*s') DESC LIMIT 10;",
                             str8_varg(needle), str8_varg(needle), str8_varg(needle));
    Arena* query_arena = arena_alloc(.reserve_size = MB(16), .commit_size = MB(1));
    APP_ResultSet result_set = {0};
    app_execute_query_capture(query_arena, sql, &database, &result_set);
    
    B32 ok = result_set.valid && result_set.row_count > 0 && result_set.column_count == 2;
    F64 prev = 2.0;
    for (U64 r = 0; ok && r < result_set.row_count; r++)
    {
      U64 id_cell = r * result_set.column_count + 0;
      U64 score_cell = r * result_set.column_count + 1;
      if (result_set.cell_is_null[id_cell] || result_set.cell_is_null[score_cell]) { ok = 0; break; }
      
      U32 id = (U32)result_set.cell_numeric[id_cell];
      F64 reported_score = result_set.cell_numeric[score_cell];
      F64 expected_score = qe_str8_trigram_similarity(rows[id - 1].text, needle);
      
      if (abs_f64(reported_score - expected_score) > 0.0001) { ok = 0; break; }
      if (reported_score > prev) { ok = 0; break; }
      prev = reported_score;
    }
    printf("  %-48s rows=%llu %s\n", "SELECT ... SIMILARITY AS score", result_set.row_count, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "SELECT ... SIMILARITY AS score: reported score column didn't match the CPU reference, or wasn't sorted");
    arena_release(query_arena);
  }
  
  //- tec: plain-column ORDER BY regression check
  {
    String8 sql = str8_lit("SELECT id FROM t ORDER BY id DESC LIMIT 5;");
    U64 count = 0;
    U32* ids = fs_run_and_collect_ids(scratch.arena, database, sql, 0, &count);
    
    B32 ok = (count == 5);
    for (U64 i = 0; ok && i < count; i++)
    {
      ok = (ids[i] == (U32)(row_count - i));
    }
    printf("  %-48s count=%llu %s\n", "ORDER BY id DESC (plain column)", count, ok ? "OK" : "FAIL");
    if (!ok) bench_report_warn(report, "ORDER BY id DESC (plain column): expected the top 5 ids descending, got a different sequence");
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
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql GPU-native fuzzy search (SIMILARITY/EDIT_DISTANCE) correctness");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  
  fs_run_suite(arena, report, 4000);
  fs_run_ranking_suite(arena, report, 4000);
  fs_run_edge_cases(arena, report);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/fuzzy_search_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
