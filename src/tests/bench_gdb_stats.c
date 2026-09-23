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

#define STATS_LOW_DISTINCT 10
#define STATS_MID_DISTINCT 5000
#define STATS_SVAL_DISTINCT 30000
#define STATS_PRICE_DISTINCT 1000000
#define STATS_MAYBE_DISTINCT 100

typedef struct Stats_Truth Stats_Truth;
struct Stats_Truth
{
  U64 row_count;
  U64 maybe_null_count;
  U64 low_distinct;
  U64 mid_distinct;
  U64 price_distinct;
  U64 grp_distinct;
  U64 sval_distinct;
  U64 maybe_distinct;
  F64 low_top_fraction;
  F64 grp_top_fraction;
  F64 price_min;
  F64 price_max;
  F64* price_sorted;
};

internal U64
stats_count_distinct(U8* seen, U64 size)
{
  U64 distinct = 0;
  for (U64 i = 0; i < size; i += 1)
  {
    if (seen[i])
    {
      distinct += 1;
    }
  }
  return distinct;
}

internal GDB_Table*
stats_build_table(Arena* arena, GDB_Database* database, char* table_name, U64 row_count, Stats_Truth* truth)
{
  GDB_Table* table = gdb_table_alloc(str8_cstring(table_name));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("id"), GDB_ColumnType_U64));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("low"), GDB_ColumnType_I32));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("mid"), GDB_ColumnType_I32));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("price"), GDB_ColumnType_F64));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("grp"), GDB_ColumnType_String8));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("sval"), GDB_ColumnType_String8));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("maybe"), GDB_ColumnType_I32));
  gdb_database_add_table(database, table);
  
  U8* low_seen = push_array(arena, U8, STATS_LOW_DISTINCT);
  U8* mid_seen = push_array(arena, U8, STATS_MID_DISTINCT);
  U8* sval_seen = push_array(arena, U8, STATS_SVAL_DISTINCT);
  U8* price_seen = push_array(arena, U8, STATS_PRICE_DISTINCT);
  U8* maybe_seen = push_array(arena, U8, STATS_MAYBE_DISTINCT);
  U8* grp_seen = push_array(arena, U8, ArrayCount(g_bench_words));
  U64 low_dominant_count = 0;
  U64 grp_dominant_count = 0;
  
  MemoryZeroStruct(truth);
  truth->row_count = row_count;
  truth->price_sorted = push_array(arena, F64, row_count);
  
  Bench_Rng rng = {0x57A75000ULL + row_count};
  U64 word_count = ArrayCount(g_bench_words);
  
  for (U64 i = 0; i < row_count; i += 1)
  {
    U64 id = i + 1;
    
    // tec: half of all rows share one value so the top-value share is known
    S32 low = (S32)(bench_rng_next(&rng) % STATS_LOW_DISTINCT);
    if (bench_rng_next(&rng) % 2 == 0)
    {
      low = 7;
    }
    
    S32 mid = (S32)(bench_rng_next(&rng) % STATS_MID_DISTINCT);
    
    U64 price_raw = bench_rng_next(&rng) % STATS_PRICE_DISTINCT;
    F64 price = (F64)price_raw / 100.0;
    
    U64 grp_index = bench_rng_next(&rng) % word_count;
    if (bench_rng_next(&rng) % 5 < 2)
    {
      grp_index = 0;
    }
    String8 grp = str8_cstring(g_bench_words[grp_index]);
    
    U64 sval_index = bench_rng_next(&rng) % STATS_SVAL_DISTINCT;
    String8 sval = push_str8f(arena, "s%llu", sval_index);
    
    S32 maybe = (S32)(bench_rng_next(&rng) % STATS_MAYBE_DISTINCT);
    B32 maybe_is_null = (bench_rng_next(&rng) % 10) == 0;
    
    low_seen[low] = 1;
    mid_seen[mid] = 1;
    price_seen[price_raw] = 1;
    grp_seen[grp_index] = 1;
    sval_seen[sval_index] = 1;
    if (low == 7)
    {
      low_dominant_count += 1;
    }
    if (grp_index == 0)
    {
      grp_dominant_count += 1;
    }
    if (maybe_is_null)
    {
      truth->maybe_null_count += 1;
    }
    else
    {
      maybe_seen[maybe] = 1;
    }
    
    truth->price_sorted[i] = price;
    
    void* row_data[7] = { &id, &low, &mid, &price, &grp, &sval, &maybe };
    B32 null_flags[7] = {0};
    null_flags[6] = maybe_is_null;
    gdb_table_add_row(table, row_data, null_flags);
  }
  
  truth->low_distinct = stats_count_distinct(low_seen, STATS_LOW_DISTINCT);
  truth->mid_distinct = stats_count_distinct(mid_seen, STATS_MID_DISTINCT);
  truth->price_distinct = stats_count_distinct(price_seen, STATS_PRICE_DISTINCT);
  truth->grp_distinct = stats_count_distinct(grp_seen, word_count);
  truth->sval_distinct = stats_count_distinct(sval_seen, STATS_SVAL_DISTINCT);
  truth->maybe_distinct = stats_count_distinct(maybe_seen, STATS_MAYBE_DISTINCT);
  truth->low_top_fraction = (F64)low_dominant_count / (F64)row_count;
  truth->grp_top_fraction = (F64)grp_dominant_count / (F64)row_count;
  
  quick_sort(truth->price_sorted, row_count, sizeof(F64), gdb_stats_compare_f64);
  truth->price_min = truth->price_sorted[0];
  truth->price_max = truth->price_sorted[row_count - 1];
  
  return table;
}

internal void
stats_check(Bench_Report* report, char* label, B32 ok, char* detail_fmt, ...)
{
  char detail[256];
  va_list args;
  va_start(args, detail_fmt);
  vsnprintf(detail, sizeof(detail), detail_fmt, args);
  va_end(args);
  
  printf("  %-44s %s (%s)\n", label, ok ? "OK" : "FAIL", detail);
  if (!ok)
  {
    bench_report_warn(report, "%s: %s", label, detail);
  }
}

internal B32
stats_within_relative(U64 actual, U64 expected, F64 tolerance)
{
  F64 difference = (F64)actual - (F64)expected;
  if (difference < 0.0)
  {
    difference = -difference;
  }
  return difference <= tolerance * (F64)expected;
}

internal void
stats_check_distinct(Bench_Report* report, char* label, GDB_Column* column, U64 expected, B32 exact_expected)
{
  U64 actual = column->stats.distinct_count;
  B32 ok = exact_expected ? (actual == expected) : stats_within_relative(actual, expected, 0.03);
  stats_check(report, label, ok, "ndv=%llu expected=%llu", actual, expected);
}

internal void
stats_check_mcv(Bench_Report* report, char* label, GDB_Column* column, U64 expected_key, F64 expected_fraction)
{
  GDB_ColumnStats* stats = &column->stats;
  B32 found = 0;
  F64 fraction = 0.0;
  if (stats->mcv_count > 0 && stats->mcv[0].key == expected_key)
  {
    found = 1;
    fraction = stats->mcv[0].fraction;
  }
  
  F64 difference = fraction - expected_fraction;
  if (difference < 0.0)
  {
    difference = -difference;
  }
  stats_check(report, label, found && difference <= 0.02, "found=%d fraction=%.4f expected=%.4f", (int)found, fraction, expected_fraction);
}

internal void
stats_check_histogram(Bench_Report* report, GDB_Column* column, Stats_Truth* truth)
{
  GDB_ColumnStats* stats = &column->stats;
  
  B32 monotone = 1;
  for (U32 bucket = 0; bucket < stats->histogram_bucket_count; bucket += 1)
  {
    if (stats->histogram_bounds[bucket] > stats->histogram_bounds[bucket + 1])
    {
      monotone = 0;
    }
  }
  stats_check(report, "price histogram is monotone", monotone && stats->histogram_bucket_count == GDB_STATS_HISTOGRAM_BUCKETS,
              "buckets=%u", stats->histogram_bucket_count);
  
  B32 range_ok = stats->has_range && stats->min_value == truth->price_min && stats->max_value == truth->price_max;
  stats_check(report, "price min and max exact", range_ok, "min=%.2f max=%.2f", stats->min_value, stats->max_value);
  
  // tec: the true rank of each bound should sit near k/buckets of the rows
  F64 worst_rank_error = 0.0;
  for (U32 bucket = 1; bucket < stats->histogram_bucket_count; bucket += 1)
  {
    F64 bound = stats->histogram_bounds[bucket];
    U64 rank = 0;
    while (rank < truth->row_count && truth->price_sorted[rank] <= bound)
    {
      rank += 1;
    }
    F64 rank_fraction = (F64)rank / (F64)truth->row_count;
    F64 expected_fraction = (F64)bucket / (F64)stats->histogram_bucket_count;
    F64 error = rank_fraction - expected_fraction;
    if (error < 0.0)
    {
      error = -error;
    }
    if (error > worst_rank_error)
    {
      worst_rank_error = error;
    }
  }
  stats_check(report, "price histogram is equi-depth", worst_rank_error <= 0.02, "worst rank error=%.4f", worst_rank_error);
}

internal void
stats_save_table(Arena* arena, GDB_Table* table, String8 table_dir)
{
  if (!os_file_path_exists(str8_lit("gdb_data/")))
  {
    os_make_directory(str8_lit("gdb_data/"));
  }
  if (!os_file_path_exists(table_dir))
  {
    os_make_directory(table_dir);
  }
  gdb_table_save(table, table_dir);
}

internal GDB_Table*
stats_load_table(Arena* arena, String8 table_dir, String8 table_name)
{
  String8 meta_path = push_str8f(arena, "%.*s/%.*s.meta", str8_varg(table_dir), str8_varg(table_name));
  GDB_Table* loaded = gdb_table_load(NULL, table_dir, meta_path);
  return loaded;
}

internal B32
stats_equal(GDB_ColumnStats* a, GDB_ColumnStats* b)
{
  B32 scalars_equal = a->row_count == b->row_count && a->null_count == b->null_count && a->distinct_count == b->distinct_count &&
    a->has_range == b->has_range && a->min_value == b->min_value && a->max_value == b->max_value &&
    a->histogram_bucket_count == b->histogram_bucket_count && a->mcv_count == b->mcv_count;
  if (!scalars_equal)
  {
    return 0;
  }
  
  for (U32 bound = 0; bound < a->histogram_bucket_count; bound += 1)
  {
    if (a->histogram_bounds[bound] != b->histogram_bounds[bound])
    {
      return 0;
    }
  }
  for (U32 entry = 0; entry < a->mcv_count; entry += 1)
  {
    if (a->mcv[entry].key != b->mcv[entry].key || a->mcv[entry].fraction != b->mcv[entry].fraction)
    {
      return 0;
    }
  }
  return 1;
}

internal void
stats_run_size(Arena* arena, Bench_Report* report, U64 row_count, char* label)
{
  printf("\n########## column stats: %s (%llu rows) ##########\n", label, row_count);
  bench_report_section(report, "column stats: %s (%llu rows)", label, row_count);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  GDB_Database* database = gdb_database_alloc(push_str8f(scratch.arena, "stats_db_%llu", row_count));
  gdb_add_database(database);
  
  Stats_Truth truth = {0};
  GDB_Table* table = stats_build_table(scratch.arena, database, "stats_table", row_count, &truth);
  
  GDB_Column* id_column = gdb_table_find_column(table, str8_lit("id"));
  GDB_Column* low_column = gdb_table_find_column(table, str8_lit("low"));
  GDB_Column* mid_column = gdb_table_find_column(table, str8_lit("mid"));
  GDB_Column* price_column = gdb_table_find_column(table, str8_lit("price"));
  GDB_Column* grp_column = gdb_table_find_column(table, str8_lit("grp"));
  GDB_Column* sval_column = gdb_table_find_column(table, str8_lit("sval"));
  GDB_Column* maybe_column = gdb_table_find_column(table, str8_lit("maybe"));
  
  B32 exact = row_count <= GDB_STATS_SAMPLE_ROWS;
  
  stats_check(report, "stats absent before first use", !gdb_column_stats_is_current(price_column), "computed=%d", (int)price_column->stats.is_computed);
  
  String8 dir_without_stats = push_str8f(scratch.arena, "gdb_data/stats_test_%llu_without", row_count);
  stats_save_table(scratch.arena, table, dir_without_stats);
  
  U64 start = os_now_microseconds();
  gdb_table_ensure_stats(table);
  U64 elapsed = os_now_microseconds() - start;
  printf("  computing stats for %llu columns took %.2f ms\n", table->column_count, (F64)elapsed / 1000.0);
  bench_report_text(report, "%s: stats for all columns took %.2f ms", label, (F64)elapsed / 1000.0);
  
  stats_check(report, "stats current after ensure", gdb_column_stats_is_current(price_column), "computed=%d", (int)price_column->stats.is_computed);
  stats_check(report, "row count recorded", price_column->stats.row_count == row_count, "rows=%llu", price_column->stats.row_count);
  
  stats_check_distinct(report, "ndv id (unique)", id_column, row_count, exact);
  stats_check_distinct(report, "ndv low", low_column, truth.low_distinct, exact);
  stats_check_distinct(report, "ndv mid", mid_column, truth.mid_distinct, exact);
  stats_check_distinct(report, "ndv price", price_column, truth.price_distinct, exact);
  stats_check_distinct(report, "ndv grp (string)", grp_column, truth.grp_distinct, exact);
  stats_check_distinct(report, "ndv sval (string)", sval_column, truth.sval_distinct, exact);
  stats_check_distinct(report, "ndv maybe (nullable)", maybe_column, truth.maybe_distinct, exact);
  
  stats_check(report, "null count exact", maybe_column->stats.null_count == truth.maybe_null_count,
              "nulls=%llu expected=%llu", maybe_column->stats.null_count, truth.maybe_null_count);
  stats_check(report, "no nulls elsewhere", price_column->stats.null_count == 0 && grp_column->stats.null_count == 0,
              "price=%llu grp=%llu", price_column->stats.null_count, grp_column->stats.null_count);
  
  stats_check_mcv(report, "top value low", low_column, gdb_stats_hash_f64(7.0), truth.low_top_fraction);
  stats_check_mcv(report, "top value grp (string)", grp_column, gdb_stats_hash_bytes((U8*)g_bench_words[0], strlen(g_bench_words[0])), truth.grp_top_fraction);
  
  stats_check(report, "string column has no range or histogram", !grp_column->stats.has_range && grp_column->stats.histogram_bucket_count == 0,
              "has_range=%d buckets=%u", (int)grp_column->stats.has_range, grp_column->stats.histogram_bucket_count);
  
  stats_check_histogram(report, price_column, &truth);
  
  //- tec: stats survive a save and reload, and a table saved without them loads without them
  {
    String8 dir_with_stats = push_str8f(scratch.arena, "gdb_data/stats_test_%llu_with", row_count);
    stats_save_table(scratch.arena, table, dir_with_stats);
    
    GDB_Table* reloaded = stats_load_table(scratch.arena, dir_with_stats, table->name);
    GDB_Table* reloaded_without = stats_load_table(scratch.arena, dir_without_stats, table->name);
    
    B32 loaded_ok = reloaded != NULL && reloaded_without != NULL;
    stats_check(report, "saved tables reload", loaded_ok, "with=%d without=%d", (int)(reloaded != NULL), (int)(reloaded_without != NULL));
    
    if (loaded_ok)
    {
      B32 all_equal = 1;
      B32 all_current = 1;
      B32 none_current = 1;
      for (U64 column_index = 0; column_index < table->column_count; column_index += 1)
      {
        GDB_Column* original = table->columns[column_index];
        GDB_Column* loaded = gdb_table_find_column(reloaded, original->name);
        GDB_Column* loaded_without = gdb_table_find_column(reloaded_without, original->name);
        
        all_current = all_current && gdb_column_stats_is_current(loaded);
        none_current = none_current && !gdb_column_stats_is_current(loaded_without);
        all_equal = all_equal && stats_equal(&original->stats, &loaded->stats);
      }
      stats_check(report, "reloaded stats are current", all_current, "current=%d", (int)all_current);
      stats_check(report, "reloaded stats match the originals", all_equal, "equal=%d", (int)all_equal);
      stats_check(report, "table saved without stats has none", none_current, "none_current=%d", (int)none_current);
    }
  }
  
  //- tec: writes invalidate, and a recompute sees the new row
  {
    U64 id = row_count + 1;
    S32 low = 1;
    S32 mid = 1;
    F64 price = 1.0;
    String8 grp = str8_lit("alpha");
    String8 sval = str8_lit("s1");
    S32 maybe = 1;
    void* row_data[7] = { &id, &low, &mid, &price, &grp, &sval, &maybe };
    B32 null_flags[7] = {0};
    gdb_table_add_row(table, row_data, null_flags);
    
    stats_check(report, "write invalidates stats", !gdb_column_stats_is_current(price_column), "computed_generation=%llu write_generation=%llu",
                price_column->stats.computed_generation, price_column->write_generation);
    
    gdb_column_ensure_stats(price_column);
    stats_check(report, "recompute sees the new row", price_column->stats.row_count == row_count + 1, "rows=%llu", price_column->stats.row_count);
  }
  
  //- tec: a second call with no writes must not recompute
  {
    U64 generation_before = price_column->stats.computed_generation;
    gdb_column_ensure_stats(price_column);
    stats_check(report, "unchanged column is not recomputed", price_column->stats.computed_generation == generation_before, "generation=%llu", generation_before);
  }
  
  scratch_end(scratch);
}

internal void
stats_check_contains(Bench_Report* report, char* label, String8 output, char* needle)
{
  B32 found = str8_find_needle(output, 0, str8_cstring(needle), 0) < output.size;
  stats_check(report, label, found, "needle='%s'", needle);
}

internal void
stats_run_analyze_statement(Arena* arena, Bench_Report* report)
{
  printf("\n########## ANALYZE statement and column_catalog ##########\n");
  bench_report_section(report, "ANALYZE statement and column_catalog");
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: 50 rows, 'grp' has 5 distinct values, 'sparse' is NULL on every tenth row
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,grp,sparse\n");
  for (U64 i = 1; i <= 50; i += 1)
  {
    if (i % 10 == 0)
    {
      str8_list_pushf(scratch.arena, &lines, "%llu,%llu,\n", i, i % 5);
    }
    else
    {
      str8_list_pushf(scratch.arena, &lines, "%llu,%llu,%llu\n", i, i % 5, i);
    }
  }
  String8 csv = str8_list_join(scratch.arena, &lines, 0);
  
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  OS_Handle file = os_file_open(OS_AccessFlag_Write, str8_lit("bench_data/analyze_stmt.csv"));
  os_file_write(file, r1u64(0, csv.size), csv.str);
  os_file_close(file);
  
  GDB_Database* database = gdb_database_alloc(str8_lit("analyze_stmt_db"));
  gdb_add_database(database);
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("astats"), str8_lit("bench_data/analyze_stmt.csv")));
  
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));
  
  {
    String8 query = str8_lit("SELECT column_name, distinct_count FROM column_catalog WHERE table_name = 'astats';");
    APP_QueryResult result = app_execute_query_capture(query_arena, query, &database, NULL);
    stats_check(report, "catalog query parses before ANALYZE", !result.had_parse_error, "parse_error=%d", (int)result.had_parse_error);
    printf("%.*s\n", str8_varg(result.output_text));
    arena_clear(query_arena);
  }
  
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("ANALYZE astats;"), &database, NULL);
    stats_check(report, "ANALYZE parses", !result.had_parse_error, "parse_error=%d", (int)result.had_parse_error);
    stats_check_contains(report, "ANALYZE names the table", result.output_text, "astats");
    stats_check_contains(report, "ANALYZE reports a header", result.output_text, "Distinct");
    printf("%.*s\n", str8_varg(result.output_text));
    arena_clear(query_arena);
  }
  
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("ANALYZE;"), &database, NULL);
    stats_check(report, "ANALYZE with no table parses", !result.had_parse_error, "parse_error=%d", (int)result.had_parse_error);
    stats_check_contains(report, "ANALYZE with no table covers astats", result.output_text, "astats");
    arena_clear(query_arena);
  }
  
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("ANALYZE missing_table;"), &database, NULL);
    B32 no_report = str8_find_needle(result.output_text, 0, str8_lit("Distinct"), 0) >= result.output_text.size;
    stats_check(report, "ANALYZE on a missing table emits no report", no_report, "output_size=%llu", result.output_text.size);
    arena_clear(query_arena);
  }
  
  {
    String8 query = str8_lit("SELECT column_name, distinct_count, null_count FROM column_catalog WHERE table_name = 'astats';");
    APP_QueryResult result = app_execute_query_capture(query_arena, query, &database, NULL);
    printf("%.*s\n", str8_varg(result.output_text));
    stats_check_contains(report, "catalog shows grp column", result.output_text, "grp");
    arena_clear(query_arena);
  }
  
  GDB_Table* table = gdb_database_find_table(database, str8_lit("astats"));
  GDB_Column* id_column = gdb_table_find_column(table, str8_lit("id"));
  GDB_Column* grp_column = gdb_table_find_column(table, str8_lit("grp"));
  GDB_Column* sparse_column = gdb_table_find_column(table, str8_lit("sparse"));
  
  B32 id_ok = id_column->stats.distinct_count == 50 && id_column->stats.null_count == 0;
  stats_check(report, "ANALYZE computed id", id_ok, "ndv=%llu nulls=%llu", id_column->stats.distinct_count, id_column->stats.null_count);
  
  B32 grp_ok = grp_column->stats.distinct_count == 5;
  stats_check(report, "ANALYZE computed grp", grp_ok, "ndv=%llu", grp_column->stats.distinct_count);
  
  B32 sparse_ok = sparse_column->stats.distinct_count == 45 && sparse_column->stats.null_count == 5;
  stats_check(report, "ANALYZE computed sparse", sparse_ok, "ndv=%llu nulls=%llu", sparse_column->stats.distinct_count, sparse_column->stats.null_count);
  
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
  
  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql column statistics");
  
  // tec: fits inside the sample, so counts are exact
  stats_run_size(arena, report, 3000, "exact path");
  // tec: past the sample, distinct counts come from HyperLogLog
  stats_run_size(arena, report, 200000, "sampled path");
  stats_run_analyze_statement(arena, report);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/gdb_stats_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
