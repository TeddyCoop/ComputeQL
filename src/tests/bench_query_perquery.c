// compute_ql: per query generated scan kernels against the bytecode interpreter. every WHERE clause runs three ways and the row counts and checksums have to agree
//   reference: interpreter only (QE_PERQUERY_KERNELS:0, QE_SCAN_SIMPLE_KERNEL:0)
//   default:   scan_simple where it applies, interpreter otherwise (QE_PERQUERY_KERNELS:0)
//   compiled:  a kernel generated for the query (QE_PERQUERY_KERNELS:2)
// --rows=N sets the table size, --samples prints every timed run, --setting=A:1,B:2 applies extra settings
// --narrow gives columns a and b small value ranges. run it with --setting=GPU_MAX_BUFFER_SIZE:1000000 so the numeric filters stream and hold those columns at 1 and 2 bytes
// the mixed cases (24 on) put plain comparisons next to terms that are not, which run on one kernel with scan_simple's layout. configurations to run
//   bench_query_perquery
//   bench_query_perquery --setting=GPU_MAX_BUFFER_SIZE:4000000
//   bench_query_perquery --narrow --setting=GPU_MAX_BUFFER_SIZE:1000000
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)
#define BENCH_TIMED_RUNS 5
#define BENCH_WARMUP_RUNS 2

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

typedef struct PQ_Case PQ_Case;
struct PQ_Case
{
  char* label;
  String8 where;
  B32 count_only;
};

typedef struct PQ_Data PQ_Data;
struct PQ_Data
{
  U64 row_count;
  U32* a;
  S32* b;
  U64* c;
  F32* d;
  F64* e;
  String8* name;
  String8* tag;
  U64 sample_c;
  String8 sample_name;
};

global B32 g_pq_narrow = 0;

global char* g_pq_tags[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot" };

internal void
pq_load_table(Arena* arena, CQL_Database* database, PQ_Data* data, U64 row_count)
{
  data->row_count = row_count;
  CQL_Table* table = cql_table_alloc(str8_lit("pq"));
  char* names[] = { "id", "a", "b", "c", "d", "e", "name", "tag" };
  CQL_ColumnType types[] = { CQL_ColumnType_U32, CQL_ColumnType_U32, CQL_ColumnType_I32, CQL_ColumnType_U64, CQL_ColumnType_F32, CQL_ColumnType_F64, CQL_ColumnType_String8, CQL_ColumnType_String8 };
  for (U32 c = 0; c < ArrayCount(names); c++)
  {
    cql_table_add_column(table, cql_column_schema_create(str8_cstring(names[c]), types[c]));
  }
  cql_database_add_table(database, table);

  U32* id = (U32*)cql_column_alloc_fixed_rows(table->columns[0], row_count);
  data->a = (U32*)cql_column_alloc_fixed_rows(table->columns[1], row_count);
  data->b = (S32*)cql_column_alloc_fixed_rows(table->columns[2], row_count);
  data->c = (U64*)cql_column_alloc_fixed_rows(table->columns[3], row_count);
  data->d = (F32*)cql_column_alloc_fixed_rows(table->columns[4], row_count);
  data->e = (F64*)cql_column_alloc_fixed_rows(table->columns[5], row_count);
  data->name = push_array(arena, String8, row_count);
  data->tag = push_array(arena, String8, row_count);

  Bench_Rng rng = { 0x9E3779B97F4A7C15ULL };
  for (U64 row = 0; row < row_count; row++)
  {
    U64 r0 = bench_rng_next(&rng);
    U64 r1 = bench_rng_next(&rng);
    U64 r2 = bench_rng_next(&rng);
    id[row] = (U32)row;
    // tec: --narrow gives a a 16 bit range and b a byte range, id is already its row number, so a streamed scan holds all three narrow
    data->a[row] = g_pq_narrow ? (U32)(r0 % 60000u) : (U32)(r0 % 2000000u);
    data->b[row] = g_pq_narrow ? (S32)((r0 >> 32) % 200u) - 100 : (S32)(r0 >> 32);
    data->c[row] = r1 % 1000000000000ULL;
    data->d[row] = (F32)((r1 >> 40) % 1000u) / 1000.0f;
    data->e[row] = (F64)(r2 % 100000u) / 100.0;
    U64 w = (r2 >> 20) % ArrayCount(g_bench_words);
    data->name[row] = push_str8f(arena, "%s_%06llu", g_bench_words[w], (r2 >> 32) % 1000000ull);
    data->tag[row] = str8_cstring(g_pq_tags[(r2 >> 50) % ArrayCount(g_pq_tags)]);
  }
  cql_column_fill_from_strings(table->columns[6], data->name, 0, row_count);
  cql_column_fill_from_strings(table->columns[7], data->tag, 0, row_count);
  table->row_count = row_count;

  data->sample_c = data->c[row_count / 3];
  data->sample_name = data->name[row_count / 2];
}

typedef struct PQ_Result PQ_Result;
struct PQ_Result
{
  U64 rows;
  U64 checksum;
  Bench_Stats stats;
  String8 kernels;
};

internal PQ_Result
pq_run(Arena* arena, CQL_Database* database, PQ_Case* test_case, U32 mode)
{
  settings_load_from_string(mode == 2 ? str8_lit("QE_PERQUERY_KERNELS:2") : str8_lit("QE_PERQUERY_KERNELS:0"));
  settings_load_from_string(mode == 0 ? str8_lit("QE_SCAN_SIMPLE_KERNEL:0") : str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
  String8 sql = test_case->count_only
    ? push_str8f(arena, "SELECT COUNT(*) FROM pq WHERE %.*s;", str8_varg(test_case->where))
    : push_str8f(arena, "SELECT id, name FROM pq WHERE %.*s;", str8_varg(test_case->where));
  PQ_Result result = {0};
  gpu_kernel_log_reset();
  result.stats = bench_run_cql_query(database, sql, &result.rows, &result.checksum);
  result.kernels = bench_kernel_log_string(arena);
  return result;
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  log_alloc();

  U64 row_count = 3000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    row_count = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  g_bench_print_samples = cmd_line_has_flag(cmdline, str8_lit("samples"));
  g_pq_narrow = cmd_line_has_flag(cmdline, str8_lit("narrow"));
  {
    char setting[96];
    snprintf(setting, sizeof(setting), "CQL_COLUMN_ARENA_RESERVE_SIZE:%llu", row_count * sizeof(U64) + MB(256));
    settings_load_from_string(str8_cstring(setting));
  }
  String8List setting_list = cmd_line_strings(cmdline, str8_lit("setting"));
  for (String8Node* setting_node = setting_list.first; setting_node; setting_node = setting_node->next)
  {
    settings_load_from_string(setting_node->string);
  }

  cql_init();
  gpu_init();

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql per query kernels against the interpreter");
  bench_report_text(report, "pq: %llu rows, columns id u32, a u32, b i32, c u64, d f32, e f64, name string8, tag string8 (low cardinality); %u timed runs per query", row_count, (U32)BENCH_TIMED_RUNS);

  CQL_Database* database = cql_database_alloc(str8_lit("pq_db"));
  cql_add_database(database);
  PQ_Data data = {0};
  pq_load_table(arena, database, &data, row_count);

  U32 tag_half = (U32)(row_count / 2);
  (void)tag_half;
  PQ_Case cases[] =
  {
    { "1 u32 lt", str8_lit("a < 20000"), 0 },
    { "2 u32 and i32", str8_lit("a < 400000 AND b > 2000000000"), 0 },
    { "3 four numeric and", str8_lit("a < 1500000 AND b > 0 AND d < 0.5 AND e > 990.0"), 0 },
    { "4 or of two types", str8_lit("a < 5000 OR b > 2147000000"), 0 },
    { "5 u64 range", str8_lit("c > 999900000000"), 0 },
    { "6 u64 equality", push_str8f(arena, "c = %llu", data.sample_c), 0 },
    { "7 f32 ge", str8_lit("d >= 0.998"), 0 },
    { "8 f64 lt", str8_lit("e < 0.05"), 0 },
    { "9 string contains and u32", str8_lit("name contains 'delta_12' AND a < 1000000"), 0 },
    { "10 string equality", push_str8f(arena, "name = '%.*s'", str8_varg(data.sample_name)), 0 },
    { "11 dict equality and u32", str8_lit("tag = 'alpha' AND a < 20000"), 0 },
    { "12 dict not equal and i32", str8_lit("tag != 'alpha' AND b < -2146000000"), 0 },
    { "13 string less than", str8_lit("name < 'alpha_000500'"), 0 },
    { "14 nested and or", str8_lit("(a < 20000 OR (b > 2140000000 AND e < 50.0)) AND (tag = 'echo' OR c < 5000000000)"), 0 },
    { "15 eight and (count)", str8_lit("a < 1900000 AND b > -2000000000 AND b < 2000000000 AND c > 1000 AND c < 999000000000 AND d > 0.01 AND d < 0.99 AND e < 999.0"), 1 },
    { "16 twelve or (count)", str8_lit("a < 50000 OR a > 1990000 OR b < -2100000000 OR b > 2100000000 OR c < 10000000000 OR c > 990000000000 OR d > 0.998 OR d < 0.002 OR e < 1.0 OR e > 998.0 OR tag = 'delta' AND a < 100000 OR name contains 'hotel_00000'"), 1 },
    { "17 count ge", str8_lit("a >= 1000000"), 1 },
    { "18 count string contains", str8_lit("name contains 'o_5'"), 1 },
    { "19 string ge and le", str8_lit("name >= 'kilo_5' AND name <= 'kilo_6'"), 0 },
    { "20 true constant", str8_lit("a >= 0"), 1 },
    { "21 similarity", str8_lit("SIMILARITY(name, 'delta_123456') > 0.45"), 0 },
    { "23 right deep or (stack depth 12)", str8_lit("a < 3000 OR (a < 6000 OR (a < 9000 OR (a < 12000 OR (a < 15000 OR (a < 18000 OR (a < 21000 OR (a < 24000 OR (a < 27000 OR (a < 30000 OR (b > 2147000000 OR (e < 0.5 OR c < 100000000)))))))))))"), 0 },
    { "22 edit distance and u32", str8_lit("EDIT_DISTANCE(name, 'echo_654321') <= 3 AND a < 1500000"), 0 },
    //- tec: AND terms that are plain comparisons next to terms that are not, the first group runs on scan_simple's layout and the second only for the rows it leaves
    { "24 two leaves and an or", str8_lit("a < 1500000 AND b > 0 AND (e < 10.0 OR d > 0.99)"), 0 },
    { "25 leaves and not equal", str8_lit("a < 1000000 AND b != 5 AND d >= 0.5"), 0 },
    { "26 leaf column read again by an or", str8_lit("a < 1000000 AND (a > 500000 OR a < 1000)"), 0 },
    { "27 leaf, string order and dict", str8_lit("a < 1500000 AND name >= 'kilo_5' AND tag = 'alpha'"), 0 },
    { "28 u64 term and two leaves", str8_lit("c > 500000000000 AND a < 1000000 AND e < 500.0"), 0 },
    { "29 nine leaves and an or", str8_lit("a < 1999000 AND a > 10 AND b < 2147000000 AND b > -2147000000 AND d < 0.999 AND d > 0.001 AND e < 999.0 AND e > 0.01 AND id < 1999000 AND (c < 999000000000 OR tag = 'alpha')"), 0 },
    { "30 row number leaf and contains", str8_lit("id >= 1000 AND id < 5000000 AND name contains 'o_1'"), 0 },
    { "31 leaf that never matches", str8_lit("a > 5000000 AND name contains 'a'"), 0 },
    { "32 count leaf and or", str8_lit("a < 1000000 AND (b < 0 OR e > 500.0)"), 1 },
    { "33 constant on the left", str8_lit("20000 > a AND name contains 'e'"), 0 },
    { "34 leaf and similarity", str8_lit("a < 1900000 AND SIMILARITY(name, 'delta_123456') > 0.45"), 0 },
    { "35 leaf and equality on f64", str8_lit("e = 12.5 AND (a < 1000000 OR tag = 'echo')"), 0 },
    { "36 count leaf and string equality", push_str8f(arena, "b > 0 AND name = '%.*s'", str8_varg(data.sample_name)), 1 },
  };

  bench_report_section(report, "WHERE clauses (%llu rows)", row_count);
  bench_report_row_begin(report);
  bench_report_cellf(report, "case");
  bench_report_cellf(report, "rows");
  bench_report_cellf(report, "reference med (ms)");
  bench_report_cellf(report, "default med (ms)");
  bench_report_cellf(report, "compiled med (ms)");
  bench_report_cellf(report, "ref/compiled");
  bench_report_cellf(report, "default kernels");
  bench_report_cellf(report, "compiled kernels");
  bench_report_table_header_end(report);

  U64 failures = 0;
  U64 unused = 0;
  for (U32 i = 0; i < ArrayCount(cases); i++)
  {
    PQ_Result reference = pq_run(arena, database, &cases[i], 0);
    PQ_Result standard = pq_run(arena, database, &cases[i], 1);
    PQ_Result compiled = pq_run(arena, database, &cases[i], 2);

    B32 match = reference.rows == compiled.rows && reference.checksum == compiled.checksum &&
      reference.rows == standard.rows && reference.checksum == standard.checksum;
    B32 used = str8_find_needle(compiled.kernels, 0, str8_lit("qk_scan_"), 0) < compiled.kernels.size;
    printf("%-28s rows %9llu  ref %8.3f ms  default %8.3f ms  compiled %8.3f ms  %5.2fx  [%.*s] -> [%.*s]%s%s\n",
           cases[i].label, reference.rows, reference.stats.execute_median, standard.stats.execute_median, compiled.stats.execute_median,
           reference.stats.execute_median / Max(compiled.stats.execute_median, 0.0001),
           str8_varg(standard.kernels), str8_varg(compiled.kernels), match ? "" : "  !! MISMATCH", used ? "" : "  (no generated kernel ran)");
    bench_report_row_begin(report);
    bench_report_cellf(report, "%s", cases[i].label);
    bench_report_cellf(report, "%llu", reference.rows);
    bench_report_cellf(report, "%.3f", reference.stats.execute_median);
    bench_report_cellf(report, "%.3f", standard.stats.execute_median);
    bench_report_cellf(report, "%.3f", compiled.stats.execute_median);
    bench_report_cellf(report, "%.2f", reference.stats.execute_median / Max(compiled.stats.execute_median, 0.0001));
    bench_report_cellf(report, "%.*s", str8_varg(standard.kernels));
    bench_report_cellf(report, "%.*s", str8_varg(compiled.kernels));
    bench_report_row_end(report);
    if (!match)
    {
      failures += 1;
      bench_report_warn(report, "%s: reference %llu rows checksum %llu, default %llu rows checksum %llu, compiled %llu rows checksum %llu",
                        cases[i].label, reference.rows, reference.checksum, standard.rows, standard.checksum, compiled.rows, compiled.checksum);
    }
    if (!used)
    {
      unused += 1;
    }
    fflush(stdout);
  }
  settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
  settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));

  U64 requested = 0, compiled_count = 0, compile_failures = 0, compile_us = 0;
  qk_stats(&requested, &compiled_count, &compile_failures, &compile_us);
  U64 gpu_compiles = 0, gpu_evictions = 0;
  gpu_generated_kernel_stats(&gpu_compiles, &gpu_evictions);
  printf("generated kernels: %llu requested, %llu compiled, %llu failed, shaderc %.1f ms total, %llu pipelines built, %llu evicted\n",
         requested, compiled_count, compile_failures, (F64)compile_us / 1000.0, gpu_compiles, gpu_evictions);
  bench_report_text(report, "generated kernels: %llu requested, %llu compiled, %llu failed, shaderc %.1f ms total, %llu pipelines built, %llu evicted",
                    requested, compiled_count, compile_failures, (F64)compile_us / 1000.0, gpu_compiles, gpu_evictions);
  if (unused > 0)
  {
    bench_report_warn(report, "%llu cases never ran a generated kernel", unused);
  }

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/perquery_report.md"));

  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0 || unused > 0)
  {
    exit(1);
  }
}
