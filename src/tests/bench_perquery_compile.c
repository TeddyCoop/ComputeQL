// compute_ql: what a new query shape costs. random WHERE trees over the numeric columns of a table, each shape run on a kernel generated for it
//   cold:     the first execution of the shape (generate GLSL, shaderc, pipeline creation, dispatch)
//   variant:  the same shape with other literals (the kernel is already built, so this is a cache hit)
//   steady:   median of the timed runs after that
//   --route:  the router policy (QE_ROUTE_CONJUNCTIONS:1). a conjunction of plain comparisons runs on the precompiled path the first time, so its cold column is that run, and
//             the generation happens on the second run ("second run" column), where shaderc and pipeline creation are counted. other shapes behave as without --route
//   --idrange: instead of random shapes, the one query "SELECT COUNT(*) FROM pq WHERE id > 1000 AND id <= 900000000" of the scale test (a range on the row number), with the same columns of the report: the steady times on the interpreter, the precompiled path and the generated kernel, and the size, registers and instruction lines of the generated pipeline
//   --sweep:  instead of random shapes, a few fixed shapes at selectivities from 0.1% to 50% (numeric single, AND of two, AND of four and OR of two comparisons) and string and dictionary shapes, each as SELECT COUNT(*),
//             with the steady times on the interpreter, the precompiled path and the generated kernel, and the cold first run and size of the generated pipeline of the first literals of each shape
//   --lib:    only the precompiled path (QE_PERQUERY_KERNELS:0): the first run of each shape, which uses kernels that are not specific to the shape, its steady time and the interpreter's,
//             to be run in a fresh process after an earlier process has filled the kernel caches, to compare with the cold first run of the generated kernel
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

// tec: --skew=S and --cluster change the data of the table, --select=LIST the select list of the shape queries
global F64 g_pc_skew = 1.0;
global B32 g_pc_cluster = 0;
global char g_pc_select[128] = "id";

typedef struct PC_Table PC_Table;
struct PC_Table
{
  U32* a;
  S32* b;
  U64* c;
  F32* d;
  F64* e;
  String8* name;
  String8* tag;
};

global char* g_pc_tags[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot" };

// tec: with strings the table also has name (a word and a 6 digit number) and tag (6 values), generated as in bench_query_perquery
internal void
pc_load_table(Arena* arena, CQL_Database* database, PC_Table* data, U64 row_count, B32 strings)
{
  CQL_Table* table = cql_table_alloc(str8_lit("pq"));
  char* names[] = { "id", "a", "b", "c", "d", "e", "name", "tag" };
  CQL_ColumnType types[] = { CQL_ColumnType_U32, CQL_ColumnType_U32, CQL_ColumnType_I32, CQL_ColumnType_U64, CQL_ColumnType_F32, CQL_ColumnType_F64, CQL_ColumnType_String8, CQL_ColumnType_String8 };
  U32 column_count = strings ? ArrayCount(names) : 6;
  for (U32 c = 0; c < column_count; c++)
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
  if (strings)
  {
    data->name = push_array(arena, String8, row_count);
    data->tag = push_array(arena, String8, row_count);
  }

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
    if (g_pc_skew != 1.0)
    {
      // tec: --skew=S, each value is the S-th power of its uniform draw within the same range, so values pile up at the low end for S above 1
      F64 ua = (F64)(r0 % 2000000u) / 2000000.0;
      F64 ub = (F64)(U32)(r0 >> 32) / 4294967296.0;
      F64 uc = (F64)(r1 % 1000000000000ULL) / 1000000000000.0;
      F64 ud = (F64)((r1 >> 40) % 1000u) / 1000.0;
      F64 ue = (F64)(r2 % 100000u) / 100000.0;
      data->a[row] = (U32)(2000000.0 * pow(ua, g_pc_skew));
      data->b[row] = (S32)(4294967296.0 * pow(ub, g_pc_skew) - 2147483648.0);
      data->c[row] = (U64)(1000000000000.0 * pow(uc, g_pc_skew));
      data->d[row] = (F32)pow(ud, g_pc_skew);
      data->e[row] = 1000.0 * pow(ue, g_pc_skew);
    }
    if (g_pc_cluster)
    {
      // tec: --cluster, column a is in row order, so a zone map can skip the chunks that a comparison on a cannot match
      data->a[row] = (U32)((U64)row * 2000000u / row_count);
    }
    if (strings)
    {
      U64 w = (r2 >> 20) % ArrayCount(g_bench_words);
      data->name[row] = push_str8f(arena, "%s_%06llu", g_bench_words[w], (r2 >> 32) % 1000000ull);
      data->tag[row] = str8_cstring(g_pc_tags[(r2 >> 50) % ArrayCount(g_pc_tags)]);
    }
  }
  if (strings)
  {
    cql_column_fill_from_strings(table->columns[6], data->name, 0, row_count);
    cql_column_fill_from_strings(table->columns[7], data->tag, 0, row_count);
  }
  table->row_count = row_count;
}

// tec: --fraction=F, the share of the rows that one leaf passes, 0.001 keeps the literals of the first version of the study
global F64 g_pc_fraction = 0.001;

// tec: a leaf passes about 0.1% of the rows, so a tree of ORs stays small and the output does not dominate the time. the literal moves with the variant, the shape does not
internal String8
pc_leaf(Arena* arena, U32 column, B32 upper, B32 inclusive, U64 variant)
{
  char* op = upper ? (inclusive ? ">=" : ">") : (inclusive ? "<=" : "<");
  U64 step = variant * 7;
  if (g_pc_fraction != 0.001)
  {
    F64 f = g_pc_fraction;
    switch (column)
    {
      case 0:
      {
        F64 value = upper ? 2000000.0 * (1.0 - f) - (F64)step : 2000000.0 * f + (F64)step;
        return push_str8f(arena, "a %s %llu", op, (U64)value);
      }
      case 1:
      {
        F64 value = upper ? 2147483648.0 - 4294967296.0 * f - (F64)step * 1000.0 : -2147483648.0 + 4294967296.0 * f + (F64)step * 1000.0;
        return push_str8f(arena, "b %s %lld", op, (S64)value);
      }
      case 2:
      {
        F64 value = upper ? 1000000000000.0 * (1.0 - f) - (F64)step * 1000.0 : 1000000000000.0 * f + (F64)step * 1000.0;
        return push_str8f(arena, "c %s %llu", op, (U64)value);
      }
      case 3:
      {
        F64 value = upper ? (1.0 - f) - (F64)step / 100000.0 : f + (F64)step / 100000.0;
        return push_str8f(arena, "d %s %.5f", op, value);
      }
      default:
      {
        F64 value = upper ? 1000.0 * (1.0 - f) - (F64)step / 100.0 : 1000.0 * f + (F64)step / 100.0;
        return push_str8f(arena, "e %s %.2f", op, value);
      }
    }
  }
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
// tec: the first value of the result of the last run, the count of a SELECT COUNT(*)
global U64 g_pc_last_value = 0;

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
  g_pc_last_value = (result.is_materialized && result.materialized.count > 0 && result.materialized.column_count > 0) ? (U64)result.materialized.columns[0].numeric_values[0] : 0;
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

global B32 g_pc_spinwarm = 0;

internal F64
pc_steady(CQL_Database* database, String8 sql, U64* out_rows, U64* out_checksum)
{
  F64 samples[BENCH_TIMED_RUNS];
  for (U32 w = 0; w < BENCH_WARMUP_RUNS; w++)
  {
    pc_run_once(database, sql, out_rows, out_checksum);
  }
  // tec: --spinwarm keeps the GPU busy for 300 ms with the query before the timed runs, so a timed run does not find the clock of an idle GPU after the compile and the pipeline creation
  if (g_pc_spinwarm)
  {
    U64 spin_start = os_now_microseconds();
    while ((os_now_microseconds() - spin_start) < 300000)
    {
      pc_run_once(database, sql, out_rows, out_checksum);
    }
  }
  for (U32 i = 0; i < BENCH_TIMED_RUNS; i++)
  {
    samples[i] = pc_run_once(database, sql, out_rows, out_checksum);
  }
  return pc_median(samples, BENCH_TIMED_RUNS);
}


#define PC_SWEEP_STEPS 6
// tec: --warmup runs one throwaway shape on each of the three paths before the sweep, so the first literal is not the first query of the process
global B32 g_pc_warmup = 0;
// tec: --touch makes a --lib run read every column once before the first shape, so the upload of the columns is not in the first run of the shape that happens to use a column first. the generated runs have the columns resident by then, the interpreter and the precompiled path run before them
global B32 g_pc_touch = 0;
// tec: --resteady measures the interpreter, the precompiled path and the generated kernel of every shape a second time after the last shape, with every kernel built
global B32 g_pc_resteady = 0;
global F64 g_pc_sweep_target[PC_SWEEP_STEPS] = { 0.001, 0.01, 0.05, 0.1, 0.25, 0.5 };
// tec: the selectivity of one comparison that makes k independent comparisons joined by AND pass the target together (the k-th root of the target), and for the OR of two, half of it
global F64 g_pc_sweep_and2[PC_SWEEP_STEPS] = { 0.0316, 0.1, 0.2236, 0.3162, 0.5, 0.7071 };
global F64 g_pc_sweep_and4[PC_SWEEP_STEPS] = { 0.1778, 0.3162, 0.4729, 0.5623, 0.7071, 0.8409 };

internal String8
pc_sweep_leaf(Arena* arena, U32 column, F64 p)
{
  switch (column)
  {
    case 0:
    {
      return push_str8f(arena, "a < %llu", (U64)(p * 2000000.0));
    }
    case 1:
    {
      S64 value = -2147483648LL + (S64)(p * 4294967296.0);
      return push_str8f(arena, "b < %lld", value);
    }
    case 3:
    {
      return push_str8f(arena, "d < %.3f", (F64)(U64)(p * 1000.0) / 1000.0);
    }
    default:
    {
      return push_str8f(arena, "e < %.2f", p * 1000.0);
    }
  }
}

internal String8
pc_sweep_where(Arena* arena, U32 shape, U32 step)
{
  switch (shape)
  {
    case 0:
    {
      return pc_sweep_leaf(arena, 0, g_pc_sweep_target[step]);
    }
    case 1:
    {
      return push_str8f(arena, "%.*s AND %.*s", str8_varg(pc_sweep_leaf(arena, 0, g_pc_sweep_and2[step])), str8_varg(pc_sweep_leaf(arena, 1, g_pc_sweep_and2[step])));
    }
    case 2:
    {
      return push_str8f(arena, "%.*s AND %.*s AND %.*s AND %.*s", str8_varg(pc_sweep_leaf(arena, 0, g_pc_sweep_and4[step])), str8_varg(pc_sweep_leaf(arena, 1, g_pc_sweep_and4[step])),
                        str8_varg(pc_sweep_leaf(arena, 3, g_pc_sweep_and4[step])), str8_varg(pc_sweep_leaf(arena, 4, g_pc_sweep_and4[step])));
    }
    default:
    {
      return push_str8f(arena, "%.*s OR %.*s", str8_varg(pc_sweep_leaf(arena, 0, g_pc_sweep_target[step] / 2.0)), str8_varg(pc_sweep_leaf(arena, 1, g_pc_sweep_target[step] / 2.0)));
    }
  }
}

typedef struct PC_SweepShape PC_SweepShape;
struct PC_SweepShape
{
  char* group;
  char* label;
  U32 where_count;
  String8 where[PC_SWEEP_STEPS];
  F64 target[PC_SWEEP_STEPS];
};

internal void
pc_run_sweep(Arena* arena, CQL_Database* database, PC_Table* data, Bench_Report* report, U64 row_count, U64* failures)
{
  PC_SweepShape shapes[16] = {0};
  U32 shape_count = 0;
  char* numeric_labels[] = { "one comparison", "AND of 2 comparisons", "AND of 4 comparisons", "OR of 2 comparisons" };
  for (U32 n = 0; n < 4; n++)
  {
    PC_SweepShape* shape = &shapes[shape_count];
    shape_count += 1;
    shape->group = "numeric";
    shape->label = numeric_labels[n];
    shape->where_count = PC_SWEEP_STEPS;
    for (U32 step = 0; step < PC_SWEEP_STEPS; step++)
    {
      shape->where[step] = pc_sweep_where(arena, n, step);
      shape->target[step] = g_pc_sweep_target[step];
    }
  }
  String8 sample_name = data->name[row_count / 2];
  struct { char* label; String8 where; } text_cases[] =
  {
    { "dictionary equality", str8_lit("tag = 'alpha'") },
    { "dictionary equality AND numeric", str8_lit("tag = 'alpha' AND a < 200000") },
    { "dictionary not equal AND numeric", str8_lit("tag != 'alpha' AND a < 200000") },
    { "dictionary OR numeric", str8_lit("tag = 'echo' OR a < 20000") },
    { "string contains, rare", str8_lit("name contains 'delta_12'") },
    { "string contains, common", str8_lit("name contains 'o_5'") },
    { "string equality, one row", {0} },
    { "string range", str8_lit("name >= 'kilo_5' AND name <= 'kilo_6'") },
    { "numeric AND string range AND dictionary", str8_lit("a < 1500000 AND name >= 'kilo_5' AND tag = 'alpha'") },
    { "similarity", str8_lit("SIMILARITY(name, 'delta_123456') > 0.45") },
  };
  for (U32 t = 0; t < ArrayCount(text_cases); t++)
  {
    PC_SweepShape* shape = &shapes[shape_count];
    shape_count += 1;
    shape->group = "string";
    shape->label = text_cases[t].label;
    shape->where_count = 1;
    shape->where[0] = text_cases[t].where.size > 0 ? text_cases[t].where : push_str8f(arena, "name = '%.*s'", str8_varg(sample_name));
    shape->target[0] = 0.0;
  }

  bench_report_section(report, "selectivity sweep and string shapes (%llu rows, SELECT COUNT(*))", row_count);
  bench_report_row_begin(report);
  bench_report_cellf(report, "group");
  bench_report_cellf(report, "shape");
  bench_report_cellf(report, "target (%%)");
  bench_report_cellf(report, "rows");
  bench_report_cellf(report, "selectivity (%%)");
  bench_report_cellf(report, "interpreter (ms)");
  bench_report_cellf(report, "precompiled (ms)");
  bench_report_cellf(report, "generated steady (ms)");
  bench_report_cellf(report, "generated / precompiled");
  bench_report_cellf(report, "cold first run (ms)");
  bench_report_cellf(report, "shaderc (ms)");
  bench_report_cellf(report, "glsl (bytes)");
  bench_report_cellf(report, "vgprs");
  bench_report_cellf(report, "isa lines");
  bench_report_cellf(report, "where");
  bench_report_table_header_end(report);

  if (g_pc_warmup)
  {
    String8 warm_sql = str8_lit("SELECT COUNT(*) FROM pq WHERE a < 1500000 AND b < 0;");
    U64 warm_rows = 0, warm_checksum = 0;
    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
    settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
    pc_steady(database, warm_sql, &warm_rows, &warm_checksum);
    settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
    pc_steady(database, warm_sql, &warm_rows, &warm_checksum);
    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:2"));
    pc_steady(database, warm_sql, &warm_rows, &warm_checksum);
  }

  for (U32 sh = 0; sh < shape_count; sh++)
  {
    PC_SweepShape* shape = &shapes[sh];
    F64 interp_ms[PC_SWEEP_STEPS] = {0};
    F64 pre_ms[PC_SWEEP_STEPS] = {0};
    F64 gen_ms[PC_SWEEP_STEPS] = {0};
    U64 counts[PC_SWEEP_STEPS] = {0};
    U64 checksums[PC_SWEEP_STEPS] = {0};
    String8 sql[PC_SWEEP_STEPS] = {0};

    // tec: the interpreter and the precompiled path first, so they do not warm the generated kernel
    for (U32 step = 0; step < shape->where_count; step++)
    {
      sql[step] = push_str8f(arena, "SELECT COUNT(*) FROM pq WHERE %.*s;", str8_varg(shape->where[step]));
      settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
      settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
      U64 interp_rows = 0, interp_checksum = 0;
      interp_ms[step] = pc_steady(database, sql[step], &interp_rows, &interp_checksum);
      U64 interp_count = g_pc_last_value;
      settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
      U64 pre_rows = 0, pre_checksum = 0;
      pre_ms[step] = pc_steady(database, sql[step], &pre_rows, &pre_checksum);
      counts[step] = interp_count;
      checksums[step] = interp_checksum;
      if (pre_rows != interp_rows || pre_checksum != interp_checksum || g_pc_last_value != interp_count)
      {
        *failures += 1;
        bench_report_warn(report, "%s: precompiled %llu rows checksum %llu differs from the interpreter (%llu, %llu)", shape->label, pre_rows, pre_checksum, interp_rows, interp_checksum);
      }
    }

    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:2"));
    U64 requested_before = 0, compiled_before = 0, failed_before = 0, us_before = 0;
    qk_stats(&requested_before, &compiled_before, &failed_before, &us_before);
    U64 gpu_before = 0, evicted_before = 0;
    gpu_generated_kernel_stats(&gpu_before, &evicted_before);
    U64 cold_rows = 0, cold_checksum = 0;
    F64 cold_ms = pc_run_once(database, sql[0], &cold_rows, &cold_checksum);
    U64 requested_after = 0, compiled_after = 0, failed_after = 0, us_after = 0;
    qk_stats(&requested_after, &compiled_after, &failed_after, &us_after);
    U64 gpu_after = 0, evicted_after = 0;
    gpu_generated_kernel_stats(&gpu_after, &evicted_after);
    B32 built = gpu_after > gpu_before;
    U64 glsl_bytes = 0, spirv_bytes = 0;
    qk_last_sizes(&glsl_bytes, &spirv_bytes);
    B32 stats_valid = g_vulkan_state->last_stats_valid;
    U32 vgprs = g_vulkan_state->last_vgprs;
    U64 isa_lines = g_vulkan_state->last_isa_lines;
    F64 shaderc_ms = (F64)(us_after - us_before) / 1000.0;
    if (cold_rows != 1 || g_pc_last_value != counts[0] || cold_checksum != checksums[0])
    {
      *failures += 1;
      bench_report_warn(report, "%s: generated first run %llu rows checksum %llu differs from the interpreter", shape->label, cold_rows, cold_checksum);
    }

    for (U32 step = 0; step < shape->where_count; step++)
    {
      U64 rows = 0, checksum = 0;
      gen_ms[step] = pc_steady(database, sql[step], &rows, &checksum);
      if (g_pc_last_value != counts[step] || checksum != checksums[step])
      {
        *failures += 1;
        bench_report_warn(report, "%s: generated %llu rows checksum %llu differs from the interpreter (%llu, %llu)", shape->label, rows, checksum, counts[step], checksums[step]);
      }
    }
    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
    settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));

    for (U32 step = 0; step < shape->where_count; step++)
    {
      F64 selectivity = 100.0 * (F64)counts[step] / (F64)row_count;
      printf("%s: %-40s target %5.1f%%  selectivity %6.2f%%  interpreter %8.3f ms  precompiled %8.3f ms  generated %8.3f ms%s\n",
             shape->group, shape->label, 100.0 * shape->target[step], selectivity, interp_ms[step], pre_ms[step], gen_ms[step], step == 0 ? (built ? "" : "  [kernel already cached]") : "");
      fflush(stdout);
      bench_report_row_begin(report);
      bench_report_cellf(report, "%s", shape->group);
      bench_report_cellf(report, "%s", shape->label);
      bench_report_cellf(report, "%.1f", 100.0 * shape->target[step]);
      bench_report_cellf(report, "%llu", counts[step]);
      bench_report_cellf(report, "%.3f", selectivity);
      bench_report_cellf(report, "%.3f", interp_ms[step]);
      bench_report_cellf(report, "%.3f", pre_ms[step]);
      bench_report_cellf(report, "%.3f", gen_ms[step]);
      bench_report_cellf(report, "%.2f", pre_ms[step] > 0.0 ? gen_ms[step] / pre_ms[step] : 0.0);
      bench_report_cellf(report, "%.3f", step == 0 ? cold_ms : 0.0);
      bench_report_cellf(report, "%.3f", step == 0 ? shaderc_ms : 0.0);
      bench_report_cellf(report, "%llu", step == 0 && built ? glsl_bytes : 0ull);
      bench_report_cellf(report, "%u", step == 0 && built && stats_valid ? vgprs : 0u);
      bench_report_cellf(report, "%llu", step == 0 && built && stats_valid ? isa_lines : 0ull);
      bench_report_cellf(report, "%.*s", str8_varg(shape->where[step]));
      bench_report_row_end(report);
    }
  }
}

// tec: the cold cost of one process, itemized: each probe is one execution, with the time inside shaderc, the time inside vkCreateComputePipelines and the rest
typedef struct PC_ColdProbe PC_ColdProbe;
struct PC_ColdProbe
{
  char* label;
  char* kernels;
  char* simple;
  char* where;
  B32 check;
};

internal void
pc_run_cold(Arena* arena, CQL_Database* database, Bench_Report* report, U64 row_count, U64* failures)
{
  PC_ColdProbe probes[] =
  {
    { "first query, no filter (upload, statistics)", "0", "1", "", 0 },
    { "scan_simple kernel, first use", "0", "1", "WHERE a < 20000", 0 },
    { "scan_simple kernel, repeat", "0", "1", "WHERE a < 20000", 0 },
    { "scan_simple kernel, new literal", "0", "1", "WHERE a < 40000", 0 },
    { "interpreter kernel, first use", "0", "0", "WHERE a < 20000", 0 },
    { "interpreter kernel, repeat", "0", "0", "WHERE a < 20000", 0 },
    { "interpreter kernel, other predicate", "0", "0", "WHERE a < 20000 OR b < 0", 0 },
    { "generated, first shape", "2", "1", "WHERE a < 1500000 AND b < 0", 1 },
    { "generated, repeat", "2", "1", "WHERE a < 1500000 AND b < 0", 1 },
    { "generated, new literal, same shape", "2", "1", "WHERE a < 1700000 AND b < 100000", 1 },
    { "generated, second shape", "2", "1", "WHERE a < 20000 OR b < 0", 1 },
    { "generated, third shape", "2", "1", "WHERE a < 1500000 AND b < 0 AND d < 900.0", 1 },
    { "generated, fourth shape", "2", "1", "WHERE a < 1500000 OR b < 0 OR e < 5.0", 1 },
  };

  bench_report_section(report, "itemized cold cost (%llu rows, SELECT COUNT(*), one process)", row_count);
  bench_report_row_begin(report);
  bench_report_cellf(report, "probe");
  bench_report_cellf(report, "run (ms)");
  bench_report_cellf(report, "shaderc (ms)");
  bench_report_cellf(report, "pipeline creation (ms)");
  bench_report_cellf(report, "pipelines created");
  bench_report_cellf(report, "other (ms)");
  bench_report_cellf(report, "count");
  bench_report_table_header_end(report);

  for (U32 p = 0; p < ArrayCount(probes); p++)
  {
    PC_ColdProbe* probe = &probes[p];
    char setting[64];
    snprintf(setting, sizeof(setting), "QE_PERQUERY_KERNELS:%s", probe->kernels);
    settings_load_from_string(str8_cstring(setting));
    snprintf(setting, sizeof(setting), "QE_SCAN_SIMPLE_KERNEL:%s", probe->simple);
    settings_load_from_string(str8_cstring(setting));
    String8 sql = push_str8f(arena, "SELECT COUNT(*) FROM pq %s;", probe->where);

    U64 requested = 0, compiled = 0, failed = 0, shaderc_before = 0;
    qk_stats(&requested, &compiled, &failed, &shaderc_before);
    U64 pipeline_us_before = g_vulkan_state->pipeline_create_us;
    U64 pipeline_count_before = g_vulkan_state->pipeline_create_count;
    U64 rows = 0, checksum = 0;
    F64 ms = pc_run_once(database, sql, &rows, &checksum);
    U64 count = g_pc_last_value;
    U64 shaderc_after = 0;
    qk_stats(&requested, &compiled, &failed, &shaderc_after);
    F64 shaderc_ms = (F64)(shaderc_after - shaderc_before) / 1000.0;
    F64 pipeline_ms = (F64)(g_vulkan_state->pipeline_create_us - pipeline_us_before) / 1000.0;
    U64 pipelines = g_vulkan_state->pipeline_create_count - pipeline_count_before;
    F64 other_ms = ms - shaderc_ms - pipeline_ms;

    if (probe->check)
    {
      settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
      settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
      U64 ref_rows = 0, ref_checksum = 0;
      pc_run_once(database, sql, &ref_rows, &ref_checksum);
      if (g_pc_last_value != count || ref_checksum != checksum)
      {
        *failures += 1;
        bench_report_warn(report, "%s: generated count %llu differs from the interpreter %llu", probe->label, count, g_pc_last_value);
      }
    }

    printf("%-48s %9.3f ms  shaderc %8.3f ms  pipeline creation %8.3f ms (%llu)  other %9.3f ms  count %llu\n", probe->label, ms, shaderc_ms, pipeline_ms, pipelines, other_ms, count);
    bench_report_row_begin(report);
    bench_report_cellf(report, "%s", probe->label);
    bench_report_cellf(report, "%.3f", ms);
    bench_report_cellf(report, "%.3f", shaderc_ms);
    bench_report_cellf(report, "%.3f", pipeline_ms);
    bench_report_cellf(report, "%llu", pipelines);
    bench_report_cellf(report, "%.3f", other_ms);
    bench_report_cellf(report, "%llu", count);
    bench_report_row_end(report);
  }
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
  B32 route = cmd_line_has_flag(cmdline, str8_lit("route"));
  B32 idrange = cmd_line_has_flag(cmdline, str8_lit("idrange"));
  B32 sweep = cmd_line_has_flag(cmdline, str8_lit("sweep"));
  B32 lib = cmd_line_has_flag(cmdline, str8_lit("lib"));
  B32 cold = cmd_line_has_flag(cmdline, str8_lit("cold"));
  if (cmd_line_has_flag(cmdline, str8_lit("skew")))
  {
    g_pc_skew = f64_from_str8(cmd_line_string(cmdline, str8_lit("skew")));
  }
  g_pc_cluster = cmd_line_has_flag(cmdline, str8_lit("cluster"));
  if (cmd_line_has_flag(cmdline, str8_lit("select")))
  {
    String8 select_list = cmd_line_string(cmdline, str8_lit("select"));
    snprintf(g_pc_select, sizeof(g_pc_select), "%.*s", (int)Min(select_list.size, (U64)100), select_list.str);
  }
  if (cmd_line_has_flag(cmdline, str8_lit("fraction")))
  {
    g_pc_fraction = f64_from_str8(cmd_line_string(cmdline, str8_lit("fraction")));
  }
  g_pc_warmup = cmd_line_has_flag(cmdline, str8_lit("warmup"));
  g_pc_touch = cmd_line_has_flag(cmdline, str8_lit("touch"));
  g_pc_spinwarm = cmd_line_has_flag(cmdline, str8_lit("spinwarm"));
  g_pc_resteady = cmd_line_has_flag(cmdline, str8_lit("resteady"));
  if (idrange)
  {
    shape_count = 1;
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
  pc_load_table(arena, database, &data, row_count, sweep);

  if (cold)
  {
    U64 cold_failures = 0;
    pc_run_cold(arena, database, report, row_count, &cold_failures);
    if (!os_file_path_exists(str8_lit("bench_reports/")))
    {
      os_make_directory(str8_lit("bench_reports/"));
    }
    bench_report_write(report, str8_lit("bench_reports/perquery_compile_report.md"));
    arena_release(arena);
    log_release();
    if (cold_failures > 0)
    {
      exit(1);
    }
    return;
  }

  if (sweep)
  {
    U64 sweep_failures = 0;
    pc_run_sweep(arena, database, &data, report, row_count, &sweep_failures);
    {
      U64 requested = 0, compiled = 0, compile_failures = 0, compile_us = 0;
      qk_stats(&requested, &compiled, &compile_failures, &compile_us);
      printf("kernels requested %llu, compiled %llu, failed %llu, pipelines created %llu, specialized requests %llu\n", requested, compiled, compile_failures, g_vulkan_state->pipeline_create_count, qk_spec_request_count() + qk_count_spec_request_count());
    }
    if (sweep_failures > 0)
    {
      bench_report_warn(report, "%llu comparisons disagreed with the interpreter", sweep_failures);
    }
    if (!os_file_path_exists(str8_lit("bench_reports/")))
    {
      os_make_directory(str8_lit("bench_reports/"));
    }
    bench_report_write(report, str8_lit("bench_reports/perquery_compile_report.md"));
    arena_release(arena);
    log_release();
    return;
  }

  bench_report_section(report, "per shape (%llu rows)", row_count);
  bench_report_row_begin(report);
  bench_report_cellf(report, "shape");
  bench_report_cellf(report, "comparisons");
  bench_report_cellf(report, "rows");
  if (lib)
  {
    bench_report_cellf(report, "library first run (ms)");
    bench_report_cellf(report, "library steady (ms)");
    bench_report_cellf(report, "interpreter (ms)");
    bench_report_cellf(report, "where");
    bench_report_table_header_end(report);
  }
  else
  {
  bench_report_cellf(report, "interpreter (ms)");
  bench_report_cellf(report, "precompiled (ms)");
  bench_report_cellf(report, "generated steady (ms)");
  bench_report_cellf(report, "cold first run (ms)");
  bench_report_cellf(report, "shaderc (ms)");
  bench_report_cellf(report, "pipeline and first use (ms)");
  bench_report_cellf(report, "other literals first run (ms)");
  bench_report_cellf(report, "where");
  bench_report_cellf(report, "glsl (bytes)");
  bench_report_cellf(report, "spirv (bytes)");
  bench_report_cellf(report, "vgprs");
  bench_report_cellf(report, "isa lines");
  if (route)
  {
    bench_report_cellf(report, "routed (deferred)");
    bench_report_cellf(report, "second run (ms)");
  }
  bench_report_table_header_end(report);
  }

  if (lib && g_pc_touch)
  {
    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
    settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
    U64 touch_rows = 0, touch_checksum = 0;
    F64 touch_ms = pc_run_once(database, str8_lit("SELECT COUNT(*) FROM pq WHERE id < 0 AND a < 0 AND b < -2147483647 AND c < 0 AND d < -1.0 AND e < -1.0;"), &touch_rows, &touch_checksum);
    printf("touch of every column %8.3f ms\n", touch_ms);
    bench_report_text(report, "touch of every column before the first shape: %.3f ms", touch_ms);
  }

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
    String8 sql = push_str8f(arena, "SELECT %s FROM pq WHERE %.*s;", g_pc_select, str8_varg(where));
    String8 variant_where = pc_where(arena, &shape, 5);
    String8 variant_sql = push_str8f(arena, "SELECT %s FROM pq WHERE %.*s;", g_pc_select, str8_varg(variant_where));
    if (idrange)
    {
      where = str8_lit("id > 1000 AND id <= 900000000");
      sql = str8_lit("SELECT COUNT(*) FROM pq WHERE id > 1000 AND id <= 900000000;");
      variant_sql = sql;
      shape.leaf_count = 2;
    }

    if (lib)
    {
      settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
      settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
      U64 lib_rows = 0, lib_checksum = 0;
      F64 lib_first_ms = pc_run_once(database, sql, &lib_rows, &lib_checksum);
      U64 steady_lib_rows = 0, steady_lib_checksum = 0;
      F64 lib_steady_ms = pc_steady(database, sql, &steady_lib_rows, &steady_lib_checksum);
      settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
      U64 ref_rows = 0, ref_checksum = 0;
      F64 ref_ms = pc_steady(database, sql, &ref_rows, &ref_checksum);
      if (lib_rows != ref_rows || lib_checksum != ref_checksum || steady_lib_checksum != ref_checksum)
      {
        failures += 1;
        bench_report_warn(report, "shape %llu: library %llu rows checksum %llu differs from the interpreter", s, lib_rows, lib_checksum);
      }
      printf("shape %2llu  %u comparisons  %9llu rows  library first run %8.3f ms  steady %8.3f ms  interpreter %8.3f ms\n", s, shape.leaf_count, lib_rows, lib_first_ms, lib_steady_ms, ref_ms);
      fflush(stdout);
      bench_report_row_begin(report);
      bench_report_cellf(report, "%llu", s);
      bench_report_cellf(report, "%u", shape.leaf_count);
      bench_report_cellf(report, "%llu", lib_rows);
      bench_report_cellf(report, "%.3f", lib_first_ms);
      bench_report_cellf(report, "%.3f", lib_steady_ms);
      bench_report_cellf(report, "%.3f", ref_ms);
      bench_report_cellf(report, "%.*s", str8_varg(where));
      bench_report_row_end(report);
      cold_samples[measured] = lib_first_ms;
      measured += 1;
      continue;
    }

    // tec: the interpreter first, so its run does not warm the generated kernel
    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
    settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
    U64 interp_rows = 0, interp_checksum = 0;
    F64 interp_ms = pc_steady(database, sql, &interp_rows, &interp_checksum);

    // tec: the precompiled path: scan_simple where the WHERE clause is an AND of plain comparisons, the interpreter otherwise
    settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
    U64 pre_rows = 0, pre_checksum = 0;
    F64 pre_ms = pc_steady(database, sql, &pre_rows, &pre_checksum);

    settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:2"));
    if (route)
    {
      settings_load_from_string(str8_lit("QE_ROUTE_CONJUNCTIONS:1"));
    }
    U64 requested_before = 0, compiled_before = 0, failed_before = 0, us_before = 0;
    qk_stats(&requested_before, &compiled_before, &failed_before, &us_before);
    U64 gpu_before = 0, evicted_before = 0;
    gpu_generated_kernel_stats(&gpu_before, &evicted_before);

    U64 rows = 0, checksum = 0;
    U64 deferred_before = qk_routed_deferrals();
    F64 cold_ms = pc_run_once(database, sql, &rows, &checksum);
    B32 deferred = qk_routed_deferrals() > deferred_before;
    F64 second_ms = 0.0;
    if (route && deferred)
    {
      // tec: the second execution of a routed shape is the one that generates, builds the pipeline and dispatches it
      U64 second_rows = 0, second_checksum = 0;
      second_ms = pc_run_once(database, sql, &second_rows, &second_checksum);
      if (second_rows != rows || second_checksum != checksum)
      {
        failures += 1;
        bench_report_warn(report, "shape %llu: the routed first run gave %llu rows checksum %llu, the second %llu rows checksum %llu", s, rows, checksum, second_rows, second_checksum);
      }
    }

    U64 requested_after = 0, compiled_after = 0, failed_after = 0, us_after = 0;
    qk_stats(&requested_after, &compiled_after, &failed_after, &us_after);
    U64 gpu_after = 0, evicted_after = 0;
    gpu_generated_kernel_stats(&gpu_after, &evicted_after);
    B32 built = gpu_after > gpu_before;
    U64 glsl_bytes = 0, spirv_bytes = 0;
    qk_last_sizes(&glsl_bytes, &spirv_bytes);
    B32 stats_valid = g_vulkan_state->last_stats_valid;
    U32 vgprs = g_vulkan_state->last_vgprs;
    U64 isa_lines = g_vulkan_state->last_isa_lines;

    U64 variant_rows = 0, variant_checksum = 0;
    F64 variant_ms = pc_run_once(database, variant_sql, &variant_rows, &variant_checksum);
    U64 gpu_variant = 0, evicted_variant = 0;
    gpu_generated_kernel_stats(&gpu_variant, &evicted_variant);

    U64 steady_rows = 0, steady_checksum = 0;
    F64 steady_ms = pc_steady(database, sql, &steady_rows, &steady_checksum);

    if (pre_rows != interp_rows || pre_checksum != interp_checksum)
    {
      failures += 1;
      bench_report_warn(report, "shape %llu: precompiled %llu rows checksum %llu differs from the interpreter", s, pre_rows, pre_checksum);
    }
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
    F64 pipeline_ms = (route && deferred ? second_ms : cold_ms) - steady_ms - shaderc_ms;
    printf("shape %2llu  %u comparisons  %9llu rows  interpreter %8.3f ms  precompiled %8.3f ms  generated %8.3f ms  cold %8.3f ms (shaderc %7.3f ms)  other literals %8.3f ms%s\n",
           s, shape.leaf_count, rows, interp_ms, pre_ms, steady_ms, cold_ms, shaderc_ms, variant_ms, built ? "" : "  [kernel already cached]");
    fflush(stdout);

    bench_report_row_begin(report);
    bench_report_cellf(report, "%llu", s);
    bench_report_cellf(report, "%u", shape.leaf_count);
    bench_report_cellf(report, "%llu", rows);
    bench_report_cellf(report, "%.3f", interp_ms);
    bench_report_cellf(report, "%.3f", pre_ms);
    bench_report_cellf(report, "%.3f", steady_ms);
    bench_report_cellf(report, "%.3f", cold_ms);
    bench_report_cellf(report, "%.3f", shaderc_ms);
    bench_report_cellf(report, "%.3f", pipeline_ms);
    bench_report_cellf(report, "%.3f", variant_ms);
    bench_report_cellf(report, "%.*s", str8_varg(where));
    bench_report_cellf(report, "%llu", built ? glsl_bytes : 0ull);
    bench_report_cellf(report, "%llu", built ? spirv_bytes : 0ull);
    bench_report_cellf(report, "%u", built && stats_valid ? vgprs : 0u);
    bench_report_cellf(report, "%llu", built && stats_valid ? isa_lines : 0ull);
    if (route)
    {
      bench_report_cellf(report, "%u", (U32)deferred);
      bench_report_cellf(report, "%.3f", second_ms);
    }
    bench_report_row_end(report);

    if (built)
    {
      cold_samples[measured] = (route && deferred) ? second_ms : cold_ms;
      shaderc_samples[measured] = shaderc_ms;
      pipeline_samples[measured] = pipeline_ms;
      variant_samples[measured] = variant_ms;
      gain_samples[measured] = interp_ms - steady_ms;
      measured += 1;
    }
  }
  if (g_pc_resteady && !lib)
  {
    bench_report_section(report, "second steady measurement of every shape, after the last shape (%llu rows)", row_count);
    bench_report_row_begin(report);
    bench_report_cellf(report, "shape");
    bench_report_cellf(report, "interpreter (ms)");
    bench_report_cellf(report, "precompiled (ms)");
    bench_report_cellf(report, "generated steady (ms)");
    bench_report_table_header_end(report);
    Bench_Rng rng2 = { seed };
    for (U64 s2 = 0; s2 < shape_count; s2++)
    {
      PC_Shape shape2 = pc_random_shape(&rng2);
      String8 where2 = pc_where(arena, &shape2, 0);
      String8 sql2 = push_str8f(arena, "SELECT %s FROM pq WHERE %.*s;", g_pc_select, str8_varg(where2));
      U64 r2_rows = 0, r2_checksum = 0;
      settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
      settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:0"));
      F64 interp2 = pc_steady(database, sql2, &r2_rows, &r2_checksum);
      settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));
      F64 pre2 = pc_steady(database, sql2, &r2_rows, &r2_checksum);
      settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:2"));
      F64 gen2 = pc_steady(database, sql2, &r2_rows, &r2_checksum);
      printf("again shape %2llu  interpreter %8.3f ms  precompiled %8.3f ms  generated %8.3f ms\n", s2, interp2, pre2, gen2);
      fflush(stdout);
      bench_report_row_begin(report);
      bench_report_cellf(report, "%llu", s2);
      bench_report_cellf(report, "%.3f", interp2);
      bench_report_cellf(report, "%.3f", pre2);
      bench_report_cellf(report, "%.3f", gen2);
      bench_report_row_end(report);
    }
  }
  settings_load_from_string(str8_lit("QE_ROUTE_CONJUNCTIONS:0"));
  settings_load_from_string(str8_lit("QE_PERQUERY_KERNELS:0"));
  settings_load_from_string(str8_lit("QE_SCAN_SIMPLE_KERNEL:1"));

  if (lib && measured > 0)
  {
    bench_report_section(report, "summary");
    bench_report_text(report, "library first run: median %.3f ms, first shape %.3f ms", pc_median(cold_samples, (U32)measured), cold_samples[0]);
    printf("library first run median %.3f ms, first shape %.3f ms\n", pc_median(cold_samples, (U32)measured), cold_samples[0]);
  }
  else if (measured > 0)
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
  {
    U64 requested = 0, compiled = 0, compile_failures = 0, compile_us = 0;
    qk_stats(&requested, &compiled, &compile_failures, &compile_us);
    bench_report_section(report, "generator counters");
    bench_report_text(report, "kernels requested %llu, compiled %llu, failed %llu, pipelines created %llu, specialized requests %llu", requested, compiled, compile_failures, g_vulkan_state->pipeline_create_count, qk_spec_request_count() + qk_count_spec_request_count());
    printf("kernels requested %llu, compiled %llu, failed %llu, pipelines created %llu, specialized requests %llu, shapes that disagreed with the interpreter %llu\n", requested, compiled, compile_failures, g_vulkan_state->pipeline_create_count, qk_spec_request_count(), failures);
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
