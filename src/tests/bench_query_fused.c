// compute_ql: global aggregates fused with their WHERE clause against the unfused plan. every query runs three ways and the answers have to agree
//   reference: interpreter scan, then the precompiled aggregate (QE_PERQUERY_KERNELS:0, QE_FUSE_AGGREGATE:0)
//   unfused:   generated scan kernel, then the aggregate (QE_PERQUERY_KERNELS:1, QE_FUSE_AGGREGATE:0)
//   fused:     one generated kernel for filter and aggregate (QE_FUSE_AGGREGATE:1)
// sums are taken over whole number columns so they are exact whatever the order of the additions
// --rows=N sets the table size, --samples prints every timed run, --setting=A:1,B:2 applies extra settings
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

typedef struct PQ_Case PQ_AggCase;

internal PQ_Result
pq_run(Arena* arena, CQL_Database* database, PQ_Case* test_case, U32 mode)
{
  settings_load_from_string(mode == 0 ? str8_lit("QE_PERQUERY_KERNELS:0") : str8_lit("QE_PERQUERY_KERNELS:1"));
  settings_load_from_string(mode == 2 ? str8_lit("QE_FUSE_AGGREGATE:1") : str8_lit("QE_FUSE_AGGREGATE:0"));
  // tec: count_only here carries the select list in `label`, the WHERE clause in `where`
  String8 sql = push_str8f(arena, "SELECT %s FROM pq WHERE %.*s;", test_case->label, str8_varg(test_case->where));
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
  qk_init();

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql fused filter and aggregate");
  bench_report_text(report, "pq: %llu rows, columns id u32, a u32, b i32, c u64, d f32, e f64, name string8, tag string8; %u timed runs per query", row_count, (U32)BENCH_TIMED_RUNS);

  CQL_Database* database = cql_database_alloc(str8_lit("pq_db"));
  cql_add_database(database);
  PQ_Data data = {0};
  pq_load_table(arena, database, &data, row_count);

  // tec: label is the select list
  PQ_Case cases[] =
  {
    { "COUNT(*)", str8_lit("a < 20000"), 0 },
    { "COUNT(*), SUM(a)", str8_lit("a < 400000"), 0 },
    { "SUM(a), MIN(a), MAX(a)", str8_lit("a < 1500000 AND b > 0"), 0 },
    { "SUM(b), MIN(b), MAX(b), COUNT(*)", str8_lit("a < 1500000 AND b > 0 AND d < 0.5 AND e > 990.0"), 0 },
    { "SUM(a), SUM(b)", str8_lit("a < 5000 OR b > 2147000000"), 0 },
    { "MIN(d), MAX(d), MIN(e), MAX(e)", str8_lit("c > 999000000000"), 0 },
    { "COUNT(*), MAX(a)", str8_lit("c = 12345"), 0 },
    { "SUM(a)", str8_lit("a > 99999999"), 0 },
    { "MIN(a), MAX(a), COUNT(a)", str8_lit("a >= 0"), 0 },
    { "AVG(a), AVG(b)", str8_lit("a < 1000000 AND b < 0"), 0 },
    { "SUM(a), MAX(e)", str8_lit("(a < 20000 OR (b > 2140000000 AND e < 50.0)) AND d > 0.1"), 0 },
    { "SUM(a), MIN(b)", str8_lit("a < 1900000 AND b > -2000000000 AND b < 2000000000 AND c > 1000 AND c < 999000000000 AND d > 0.01 AND d < 0.99 AND e < 999.0"), 0 },
    { "MIN(e), MAX(e), COUNT(*)", str8_lit("e = 12.5 OR a < 1000"), 0 },
    { "SUM(a)", str8_lit("20000 > a"), 0 },
    { "SUM(id), MAX(id)", str8_lit("id >= 1000 AND id < 5000000"), 0 },
    { "SUM(a), SUM(b), MIN(a), MAX(b), MIN(id)", str8_lit("a < 1000000"), 0 },
    // tec: GROUP BY one small range column. under --narrow b has 200 values and fuses, otherwise these must still agree
    { "b, COUNT(*), SUM(a)", str8_lit("a < 30000 GROUP BY b"), 0 },
    { "b, MIN(a), MAX(a), SUM(id)", str8_lit("a >= 0 GROUP BY b"), 0 },
    { "b, COUNT(*), AVG(a)", str8_lit("a < 1000 OR d > 0.9 GROUP BY b"), 0 },
    { "b, SUM(b), MIN(b), MAX(b)", str8_lit("a > 20000 AND e < 500.0 GROUP BY b"), 0 },
    { "b, COUNT(*)", str8_lit("a > 99999999 GROUP BY b"), 0 },
    { "b, SUM(a), SUM(id), MIN(id)", str8_lit("c > 500000000000 GROUP BY b"), 0 },
    // tec: dictionary coded string keys and filters, and two keys
    { "tag, COUNT(*), SUM(a), MAX(id)", str8_lit("a < 1500000 GROUP BY tag"), 0 },
    { "tag, COUNT(*)", str8_lit("tag != 'alpha' AND b > 0 GROUP BY tag"), 0 },
    { "tag, b, COUNT(*)", str8_lit("a < 1500000 GROUP BY tag, b"), 0 },
    { "tag, b, SUM(a), MIN(id)", str8_lit("a >= 0 GROUP BY b, tag"), 0 },
    { "COUNT(*), SUM(a), MAX(id)", str8_lit("tag = 'alpha' AND a < 1500000"), 0 },
    { "SUM(a)", str8_lit("tag != 'echo' OR b > 0"), 0 },
    // tec: string terms are not fused yet, these must still agree and run unfused
    { "SUM(a)", str8_lit("name contains 'delta_12' AND a < 1000000"), 0 },
    { "COUNT(*), MAX(a)", str8_lit("tag = 'alpha' AND a < 200000"), 0 },
  };

  bench_report_section(report, "aggregates (%llu rows)", row_count);
  bench_report_row_begin(report);
  bench_report_cellf(report, "select list");
  bench_report_cellf(report, "where");
  bench_report_cellf(report, "reference med (ms)");
  bench_report_cellf(report, "unfused med (ms)");
  bench_report_cellf(report, "fused med (ms)");
  bench_report_cellf(report, "unfused/fused");
  bench_report_cellf(report, "fused kernels");
  bench_report_table_header_end(report);

  U64 failures = 0;
  U64 fused_count = 0;
  for (U32 i = 0; i < ArrayCount(cases); i++)
  {
    PQ_Result reference = pq_run(arena, database, &cases[i], 0);
    PQ_Result unfused = pq_run(arena, database, &cases[i], 1);
    PQ_Result fused = pq_run(arena, database, &cases[i], 2);

    B32 match = reference.rows == fused.rows && reference.checksum == fused.checksum && reference.rows == unfused.rows && reference.checksum == unfused.checksum;
    B32 ran_fused = str8_find_needle(fused.kernels, 0, str8_lit("qk_scan_"), 0) < fused.kernels.size &&
      str8_find_needle(fused.kernels, 0, str8_lit("selection_gather"), 0) >= fused.kernels.size;
    if (ran_fused)
    {
      fused_count += 1;
    }
    printf("%-36s ref %8.3f ms  unfused %8.3f ms  fused %8.3f ms  %5.2fx  [%.*s]%s%s   %.*s\n",
           cases[i].label, reference.stats.execute_median, unfused.stats.execute_median, fused.stats.execute_median,
           unfused.stats.execute_median / Max(fused.stats.execute_median, 0.0001), str8_varg(fused.kernels),
           match ? "" : "  !! MISMATCH", ran_fused ? "" : "  (not fused)", str8_varg(cases[i].where));
    bench_report_row_begin(report);
    bench_report_cellf(report, "%s", cases[i].label);
    bench_report_cellf(report, "%.*s", str8_varg(cases[i].where));
    bench_report_cellf(report, "%.3f", reference.stats.execute_median);
    bench_report_cellf(report, "%.3f", unfused.stats.execute_median);
    bench_report_cellf(report, "%.3f", fused.stats.execute_median);
    bench_report_cellf(report, "%.2f", unfused.stats.execute_median / Max(fused.stats.execute_median, 0.0001));
    bench_report_cellf(report, "%.*s", str8_varg(fused.kernels));
    bench_report_row_end(report);
    if (!match)
    {
      failures += 1;
      bench_report_warn(report, "%s WHERE %.*s: reference %llu rows checksum %llu, unfused %llu rows checksum %llu, fused %llu rows checksum %llu",
                        cases[i].label, str8_varg(cases[i].where), reference.rows, reference.checksum, unfused.rows, unfused.checksum, fused.rows, fused.checksum);
    }
    fflush(stdout);
  }
  settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:1"));
  settings_load_from_string(str8_lit("QE_FUSE_AGGREGATE:1"));
  printf("%llu of %u queries ran fused\n", fused_count, (U32)ArrayCount(cases));
  bench_report_text(report, "%llu of %u queries ran fused", fused_count, (U32)ArrayCount(cases));

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/fused_report.md"));

  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0 || fused_count == 0)
  {
    exit(1);
  }
}
