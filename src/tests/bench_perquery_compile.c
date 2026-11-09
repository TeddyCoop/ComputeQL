// compute_ql: what a new query shape costs. random WHERE trees over the numeric columns of a table, each shape run on a kernel generated for it
//   cold:     the first execution of the shape (generate GLSL, shaderc, pipeline creation, dispatch)
//   variant:  the same shape with other literals (the kernel is already built, so this is a cache hit)
//   steady:   median of the timed runs after that
//   interp:   the same query on the bytecode interpreter (QE_PERQUERY_KERNELS:0, QE_SCAN_SIMPLE_KERNEL:0), row count and checksum must match
// shaderc time comes from the generator's own counter, the rest of cold minus steady is pipeline creation and first use.
// --rows=N table size, --shapes=N number of shapes, --seed=N shape seed (default: the clock, so the shapes are new to the driver's shader cache; rerun with the same seed for the driver cache hit case)
// the SPIR-V disk cache and the pipeline cache file must be empty for a cold run: run it with --setting=QE_SPIRV_DISK_CACHE:0 after deleting shaders\pipeline_cache.bin
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)
#define BENCH_TIMED_RUNS 7
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

#define PC_COLUMN_COUNT 5

global char* g_pc_column_names[PC_COLUMN_COUNT] = { "a", "b", "c", "d", "e" };

typedef struct PC_Table PC_Table;
struct PC_Table
{
  U32* a;
  S32* b;
  U64* c;
  F32* d;
  F64* e;
};

internal void
pc_load_table(CQL_Database* database, PC_Table* data, U64 row_count)
{
  CQL_Table* table = cql_table_alloc(str8_lit("pq"));
  char* names[] = { "id", "a", "b", "c", "d", "e" };
  CQL_ColumnType types[] = { CQL_ColumnType_U32, CQL_ColumnType_U32, CQL_ColumnType_I32, CQL_ColumnType_U64, CQL_ColumnType_F32, CQL_ColumnType_F64 };
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

  Bench_Rng rng = { 0x9E3779B97F4A7C15ULL };
  for (U64 row = 0; row < row_count; row++)
  {
    U64 r0 = bench_rng_next(&rng);
    U64 r1 = bench_rng_next(&rng);
    U64 r2 = bench_rng_next(&rng);
    id[row] = (U32)row;
    data->a[row] = (U32)(r0 % 2000000u);
    data->b[row] = (S32)(r0 >> 32);
    data->c[row] = r1 % 1000000000000ULL;
    data->d[row] = (F32)((r1 >> 40) % 1000u) / 1000.0f;
    data->e[row] = (F64)(r2 % 100000u) / 100.0;
  }
  table->row_count = row_count;
}

// tec: a leaf passes about 0.1% of the rows, so a tree of ORs stays small and the output does not dominate the time. the literal moves with the variant, the shape does not
internal String8
pc_leaf(Arena* arena, U32 column, B32 upper, B32 inclusive, U64 variant)
{
  char* op = upper ? (inclusive ? ">=" : ">") : (inclusive ? "<=" : "<");
  U64 step = variant * 7;
  switch (column)
  {
    case 0:
    {
      U64 value = upper ? 1998000 - step : 2000 + step;
      return push_str8f(arena, "a %s %llu", op, value);
    }
    case 1:
    {
      S64 value = upper ? 2143000000LL - (S64)step * 1000 : -2143000000LL + (S64)step * 1000;
      return push_str8f(arena, "b %s %lld", op, value);
    }
    case 2:
    {
      U64 value = upper ? 999000000000ULL - step * 1000 : 1000000000ULL + step * 1000;
      return push_str8f(arena, "c %s %llu", op, value);
    }
    case 3:
    {
      F64 value = upper ? 0.999 - (F64)step / 100000.0 : 0.001 + (F64)step / 100000.0;
      return push_str8f(arena, "d %s %.5f", op, value);
    }
    default:
    {
      F64 value = upper ? 999.0 - (F64)step / 100.0 : 1.0 + (F64)step / 100.0;
      return push_str8f(arena, "e %s %.2f", op, value);
    }
  }
}

typedef struct PC_Leaf PC_Leaf;
struct PC_Leaf
{
  U32 column;
  B32 upper;
  B32 inclusive;
};

typedef struct PC_Shape PC_Shape;
struct PC_Shape
{
  U32 leaf_count;
  PC_Leaf leaves[8];
  // tec: joiner[i] sits between leaf i and leaf i+1, 1 is OR. group_open[i] opens a parenthesis before leaf i, group_close[i] closes one after it
  B32 joiner[8];
  B32 group_open[8];
  B32 group_close[8];
};

internal PC_Shape
pc_random_shape(Bench_Rng* rng)
{
  PC_Shape shape = {0};
  shape.leaf_count = 1 + (U32)(bench_rng_next(rng) % 8);
  for (U32 i = 0; i < shape.leaf_count; i++)
  {
    U64 r = bench_rng_next(rng);
    shape.leaves[i].column = (U32)(r % PC_COLUMN_COUNT);
    shape.leaves[i].upper = (r >> 8) & 1;
    shape.leaves[i].inclusive = (r >> 9) & 1;
    shape.joiner[i] = (r >> 10) % 4 == 0;
  }
  // tec: one group over two neighbouring leaves when there are enough, so the trees are not all flat
  if (shape.leaf_count >= 4)
  {
    U32 start = (U32)(bench_rng_next(rng) % (shape.leaf_count - 2));
    shape.group_open[start] = 1;
    shape.group_close[start + 1] = 1;
    shape.joiner[start] = !shape.joiner[start + 1];
  }
  return shape;
}

internal String8
pc_where(Arena* arena, PC_Shape* shape, U64 variant)
{
  String8List parts = {0};
  for (U32 i = 0; i < shape->leaf_count; i++)
  {
    if (shape->group_open[i])
    {
      str8_list_push(arena, &parts, str8_lit("("));
    }
    PC_Leaf* leaf = &shape->leaves[i];
    str8_list_push(arena, &parts, pc_leaf(arena, leaf->column, leaf->upper, leaf->inclusive, variant));
    if (shape->group_close[i])
    {
      str8_list_push(arena, &parts, str8_lit(")"));
    }
    if (i + 1 < shape->leaf_count)
    {
      str8_list_push(arena, &parts, shape->joiner[i] ? str8_lit(" OR ") : str8_lit(" AND "));
    }
  }
  return str8_list_join(arena, &parts, 0);
}

// tec: one execution, planning to the last fetched value, in milliseconds
internal F64
pc_run_once(CQL_Database* database, String8 sql, U64* out_rows, U64* out_checksum)
{
  Temp scratch = scratch_begin(0, 0);
  Arena* arena = scratch.arena;
  U64 t0 = os_now_microseconds();
  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql);
  SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;
  U64 temp_mark = database->temp_table_count;
  PLAN_ExecResult result = plan_run_select(arena, database, select_node, NULL);
  U64 rows = 0;
  U64 checksum = bench_cql_consume_result(&result, select_node, &rows);
  cql_database_release_temp_tables_from(database, temp_mark);
  U64 t1 = os_now_microseconds();
  scratch_end(scratch);
  *out_rows = rows;
  *out_checksum = checksum;
  return (F64)(t1 - t0) / 1000.0;
}

internal F64
pc_median(F64* samples, U32 count)
{
  for (U32 i = 1; i < count; i++)
  {
    F64 key = samples[i];
    U32 j = i;
    while (j > 0 && samples[j - 1] > key)
    {
      samples[j] = samples[j - 1];
      j -= 1;
    }
    samples[j] = key;
  }
  return samples[count / 2];
}

internal F64
pc_steady(CQL_Database* database, String8 sql, U64* out_rows, U64* out_checksum)
{
  F64 samples[BENCH_TIMED_RUNS];
  for (U32 w = 0; w < BENCH_WARMUP_RUNS; w++)
  {
    pc_run_once(database, sql, out_rows, out_checksum);
  }
  for (U32 i = 0; i < BENCH_TIMED_RUNS; i++)
  {
    samples[i] = pc_run_once(database, sql, out_rows, out_checksum);
  }
  return pc_median(samples, BENCH_TIMED_RUNS);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  log_alloc();

  U64 row_count = 20000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    row_count = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  U64 shape_count = 40;
  if (cmd_line_has_argument(cmdline, str8_lit("shapes")))
  {
    shape_count = u64_from_str8(cmd_line_string(cmdline, str8_lit("shapes")), 10);
  }
  U64 seed = os_now_microseconds();
  if (cmd_line_has_argument(cmdline, str8_lit("seed")))
  {
    seed = u64_from_str8(cmd_line_string(cmdline, str8_lit("seed")), 10);
  }
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
  Bench_Report* report = bench_report_alloc(arena, "compute_ql cost of a new query shape");
  bench_report_text(report, "pq: %llu rows, columns id u32, a u32, b i32, c u64, d f32, e f64; %llu random WHERE trees of 1 to 8 comparisons, seed %llu; %u timed runs per steady measurement", row_count, shape_count, seed, (U32)BENCH_TIMED_RUNS);

  CQL_Database* database = cql_database_alloc(str8_lit("pq_db"));
  cql_add_database(database);
  PC_Table data = {0};
  pc_load_table(database, &data, row_count);

  bench_report_section(report, "per shape (%llu rows)", row_count);
  bench_report_row_begin(report);
  bench_report_cellf(report, "shape");
  bench_report_cellf(report, "comparisons");
  bench_report_cellf(report, "rows");
  bench_report_cellf(report, "interpreter (ms)");
  bench_report_cellf(report, "generated steady (ms)");
  bench_report_cellf(report, "cold first run (ms)");
  bench_report_cellf(report, "shaderc (ms)");
  bench_report_cellf(report, "pipeline and first use (ms)");
  bench_report_cellf(report, "other literals first run (ms)");
  bench_report_cellf(report, "where");
  bench_report_table_header_end(report);

  Bench_Rng rng = { seed };
  F64* cold_samples = push_array(arena, F64, shape_count);
  F64* shaderc_samples = push_array(arena, F64, shape_count);
  F64* pipeline_samples = push_array(arena, F64, shape_count);
  F64* variant_samples = push_array(arena, F64, shape_count);
  F64* gain_samples = push_array(arena, F64, shape_count);
  U64 measured = 0;
  U64 failures = 0;
  U64 reused_shapes = 0;

  for (U64 s = 0; s < shape_count; s++)
  {
    PC_Shape shape = pc_random_shape(&rng);
    String8 where = pc_where(arena, &shape, 0);
    String8 sql = push_str8f(arena, "SELECT id FROM pq WHERE %.*s;", str8_varg(where));
    String8 variant_where = pc_where(arena, &shape, 5);
    String8 variant_sql = push_str8f(arena, "SELECT id FROM pq WHERE %.*s;", str8_varg(variant_where));

    // tec: the interpreter first, so its run does not warm the generated kernel
    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
    settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
    U64 interp_rows = 0, interp_checksum = 0;
    F64 interp_ms = pc_steady(database, sql, &interp_rows, &interp_checksum);

    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:2"));
    U64 requested_before = 0, compiled_before = 0, failed_before = 0, us_before = 0;
    qk_stats(&requested_before, &compiled_before, &failed_before, &us_before);
    U64 gpu_before = 0, evicted_before = 0;
    gpu_generated_kernel_stats(&gpu_before, &evicted_before);

    U64 rows = 0, checksum = 0;
    F64 cold_ms = pc_run_once(database, sql, &rows, &checksum);

    U64 requested_after = 0, compiled_after = 0, failed_after = 0, us_after = 0;
    qk_stats(&requested_after, &compiled_after, &failed_after, &us_after);
    U64 gpu_after = 0, evicted_after = 0;
    gpu_generated_kernel_stats(&gpu_after, &evicted_after);
    B32 built = gpu_after > gpu_before;

    U64 variant_rows = 0, variant_checksum = 0;
    F64 variant_ms = pc_run_once(database, variant_sql, &variant_rows, &variant_checksum);
    U64 gpu_variant = 0, evicted_variant = 0;
    gpu_generated_kernel_stats(&gpu_variant, &evicted_variant);

    U64 steady_rows = 0, steady_checksum = 0;
    F64 steady_ms = pc_steady(database, sql, &steady_rows, &steady_checksum);

    if (steady_rows != interp_rows || steady_checksum != interp_checksum || rows != interp_rows || checksum != interp_checksum)
    {
      failures += 1;
      bench_report_warn(report, "shape %llu: interpreter %llu rows checksum %llu, generated %llu rows checksum %llu", s, interp_rows, interp_checksum, steady_rows, steady_checksum);
    }
    if (gpu_variant != gpu_after)
    {
      bench_report_warn(report, "shape %llu: the same shape with other literals built another pipeline", s);
    }
    if (!built)
    {
      // tec: two random trees can generate the same source, the second finds its kernel in the cache and has no compile to report
      reused_shapes += 1;
    }
    F64 shaderc_ms = (F64)(us_after - us_before) / 1000.0;
    F64 pipeline_ms = cold_ms - steady_ms - shaderc_ms;
    printf("shape %2llu  %u comparisons  %9llu rows  interpreter %8.3f ms  generated %8.3f ms  cold %8.3f ms (shaderc %7.3f ms)  other literals %8.3f ms%s\n",
           s, shape.leaf_count, rows, interp_ms, steady_ms, cold_ms, shaderc_ms, variant_ms, built ? "" : "  [kernel already cached]");
    fflush(stdout);

    bench_report_row_begin(report);
    bench_report_cellf(report, "%llu", s);
    bench_report_cellf(report, "%u", shape.leaf_count);
    bench_report_cellf(report, "%llu", rows);
    bench_report_cellf(report, "%.3f", interp_ms);
    bench_report_cellf(report, "%.3f", steady_ms);
    bench_report_cellf(report, "%.3f", cold_ms);
    bench_report_cellf(report, "%.3f", shaderc_ms);
    bench_report_cellf(report, "%.3f", pipeline_ms);
    bench_report_cellf(report, "%.3f", variant_ms);
    bench_report_cellf(report, "%.*s", str8_varg(where));
    bench_report_row_end(report);

    if (built)
    {
      cold_samples[measured] = cold_ms;
      shaderc_samples[measured] = shaderc_ms;
      pipeline_samples[measured] = pipeline_ms;
      variant_samples[measured] = variant_ms;
      gain_samples[measured] = interp_ms - steady_ms;
      measured += 1;
    }
  }
  settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
  settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));

  if (measured > 0)
  {
    F64 cold_total = 0.0, shaderc_total = 0.0;
    F64 cold_min = cold_samples[0], cold_max = cold_samples[0];
    for (U64 i = 0; i < measured; i++)
    {
      cold_total += cold_samples[i];
      shaderc_total += shaderc_samples[i];
      cold_min = Min(cold_min, cold_samples[i]);
      cold_max = Max(cold_max, cold_samples[i]);
    }
    F64 variant_median = pc_median(variant_samples, (U32)measured);
    F64 shaderc_median = pc_median(shaderc_samples, (U32)measured);
    F64 pipeline_median = pc_median(pipeline_samples, (U32)measured);
    F64 gain_median = pc_median(gain_samples, (U32)measured);
    F64 cold_median = pc_median(cold_samples, (U32)measured);
    bench_report_section(report, "summary");
    bench_report_text(report, "%llu shapes built a new kernel (%llu found theirs in the cache)", measured, reused_shapes);
    bench_report_text(report, "cold first run: median %.3f ms, min %.3f ms, max %.3f ms, total %.1f ms", cold_median, cold_min, cold_max, cold_total);
    bench_report_text(report, "shaderc: median %.3f ms, total %.1f ms; pipeline and first use: median %.3f ms", shaderc_median, shaderc_total, pipeline_median);
    bench_report_text(report, "same shape, other literals, first run: median %.3f ms", variant_median);
    bench_report_text(report, "median time the generated kernel saves per execution against the interpreter: %.3f ms", gain_median);
    printf("cold first run median %.3f ms (min %.3f, max %.3f), shaderc median %.3f ms, other literals median %.3f ms, saving per execution median %.3f ms\n",
           cold_median, cold_min, cold_max, shaderc_median, variant_median, gain_median);
  }
  if (failures > 0)
  {
    bench_report_warn(report, "%llu shapes disagreed with the interpreter", failures);
  }

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/perquery_compile_report.md"));

  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0)
  {
    exit(1);
  }
}
