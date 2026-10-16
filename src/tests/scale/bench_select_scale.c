#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)
#define BENCH_TIMED_RUNS 5
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

//~ tec: select statements over one narrow table, sized by --rows (run 10M, 100M and 1B through run_select_scale.bat)
// every column is a closed form of the row index so expected answers are computed here, duckdb is only a timing reference
//
//   id         = index + 1
//   region_id  = index % 50
//   amount     = index % 10000
//   needle_key = (index * 2654435761) % 2^32
//
// the GROUP BY and join queries come after those and use three small tables next to scale_events, which are cheap to build and the same on both engines
//
//   regions(region_id, zone)         50 rows, region_id = 0..49, zone = region_id / 10
//   products(product_id, category)   10000 rows, product_id = 0..9999, category = product_id % 20
//   big_dim(big_key, weight)         1000000 rows, row j has the needle_key of row j * (rows / 1000000) of scale_events, weight = j % 100
//                                    keys spread over the whole key range, so a join on needle_key probes every row
//
// every query reads a whole column and returns one value or one row. the multiplier is odd so needle_key values are all different and spread over nearly the whole 32 bit range in any 8192 rows, a zone map cannot rule a chunk out

#define SS_REGIONS      50
#define SS_AMOUNT_RANGE 10000
#define SS_HIGH_AMOUNT  9990
#define SS_MAX_VALUES   3
#define SS_ZONE_SIZE    10
#define SS_PRODUCT_CATEGORIES 20
#define SS_PRODUCT_ROWS 10000
#define SS_BIG_DIM_ROWS 1000000
#define SS_BIG_DIM_WEIGHTS 100
#define SS_GROUP_JOIN_MIN_ROWS 1000000
#define SS_KEY_MULTIPLIER 2654435761ULL
#define SS_MAX_ROWS     3000000000ULL

// tec: --big_dim_rows and --big_dim_match_percent size big_dim and set how many of its keys are in scale_events. 
// a key that is not there is the key of a row number past the end of the table
global U64 g_ss_big_dim_rows = SS_BIG_DIM_ROWS;
global U64 g_ss_big_dim_matched = SS_BIG_DIM_ROWS;

typedef struct SS_Case SS_Case;
struct SS_Case
{
  char* label;
  char* sql;
  U64 expected_rows;
  U64 value_count;
  F64 expected_values[SS_MAX_VALUES];
  U64 expected_first_row;
  U64 scan_bytes;
};

typedef struct SS_Result SS_Result;
struct SS_Result
{
  B32 ok;
  U64 rows;
  F64 values[SS_MAX_VALUES];
  U64 value_count;
  U64 first_row;
};

//~ tec: expected answers

internal F64
ss_sum_amount(U64 row_count)
{
  U64 full_cycles = row_count / SS_AMOUNT_RANGE;
  U64 remainder = row_count % SS_AMOUNT_RANGE;
  U64 cycle_sum = (U64)SS_AMOUNT_RANGE * (SS_AMOUNT_RANGE - 1) / 2;
  return (F64)(full_cycles * cycle_sum + (remainder > 0 ? remainder * (remainder - 1) / 2 : 0));
}

internal U64
ss_count_amount_above(U64 row_count, U64 threshold)
{
  U64 full_cycles = row_count / SS_AMOUNT_RANGE;
  U64 remainder = row_count % SS_AMOUNT_RANGE;
  U64 per_cycle = SS_AMOUNT_RANGE - 1 - threshold;
  U64 partial = (remainder > threshold + 1) ? (remainder - threshold - 1) : 0;
  return full_cycles * per_cycle + partial;
}

internal U64
ss_count_region(U64 row_count, U64 region)
{
  return row_count / SS_REGIONS + ((row_count % SS_REGIONS) > region ? 1 : 0);
}

// tec: rows of one region, and the sum of their amounts. the rows of region r are r + 50k, and the amount of such a row is r + 50 * (k % 200)
internal U64
ss_region_rows(U64 row_count, U64 region)
{
  return row_count / SS_REGIONS + ((row_count % SS_REGIONS) > region ? 1 : 0);
}

internal F64
ss_region_amount_sum(U64 row_count, U64 region)
{
  U64 rows = ss_region_rows(row_count, region);
  U64 cycles = rows / 200;
  U64 remainder = rows % 200;
  U64 cycle_sum = 200 * 199 / 2;
  U64 partial = remainder > 0 ? remainder * (remainder - 1) / 2 : 0;
  return (F64)(rows * region + 50 * (cycles * cycle_sum + partial));
}

internal U64
ss_amount_rows(U64 row_count, U64 amount)
{
  return row_count / SS_AMOUNT_RANGE + ((row_count % SS_AMOUNT_RANGE) > amount ? 1 : 0);
}

// tec: sum of j % 100 for j = 0 .. count - 1
internal F64
ss_weight_sum(U64 count)
{
  U64 cycles = count / SS_BIG_DIM_WEIGHTS;
  U64 remainder = count % SS_BIG_DIM_WEIGHTS;
  U64 partial = remainder > 0 ? remainder * (remainder - 1) / 2 : 0;
  return (F64)(cycles * (SS_BIG_DIM_WEIGHTS * (SS_BIG_DIM_WEIGHTS - 1) / 2) + partial);
}

internal U32
ss_key_of_row(U64 row)
{
  return (U32)(row * SS_KEY_MULTIPLIER);
}

// tec: a key near either end of the 32 bit range could fall outside the min and max of a chunk, and a zone map would skip it without a scan
internal B32
ss_key_is_searchable(U32 key)
{
  return key >= (1u << 24) && key <= (0xffffffffu - (1u << 24));
}

//~ tec: loaders

internal void
ss_fill_columns(CQL_Table* table, U64 row_count)
{
  U32* id = (U32*)cql_column_alloc_fixed_rows(table->columns[0], row_count);
  U32* region = (U32*)cql_column_alloc_fixed_rows(table->columns[1], row_count);
  U32* amount = (U32*)cql_column_alloc_fixed_rows(table->columns[2], row_count);
  U32* needle_key = (U32*)cql_column_alloc_fixed_rows(table->columns[3], row_count);
  if (id == 0 || region == 0 || amount == 0 || needle_key == 0)
  {
    log_error("could not allocate %llu rows", row_count);
    return;
  }
  
  U32 region_value = 0;
  U32 amount_value = 0;
  for (U64 i = 0; i < row_count; i++)
  {
    id[i] = (U32)(i + 1);
    region[i] = region_value;
    amount[i] = amount_value;
    needle_key[i] = ss_key_of_row(i);
    region_value = (region_value + 1 == SS_REGIONS) ? 0 : region_value + 1;
    amount_value = (amount_value + 1 == SS_AMOUNT_RANGE) ? 0 : amount_value + 1;
  }
  table->row_count = row_count;
}

internal CQL_Table*
ss_load_cql(CQL_Database* database, U64 row_count)
{
  CQL_Table* table = cql_table_alloc(str8_lit("scale_events"));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("id"), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("region_id"), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("amount"), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("needle_key"), CQL_ColumnType_U32));
  cql_database_add_table(database, table);
  ss_fill_columns(table, row_count);
  return table;
}

// tec: a two column table of U32, key = key_start + row, value = (key / value_divisor) % value_modulus
internal void
ss_load_dimension(CQL_Database* database, char* name, char* key_name, char* value_name, U64 rows, U32 key_start, U32 value_divisor, U32 value_modulus)
{
  CQL_Table* table = cql_table_alloc(str8_cstring(name));
  cql_table_add_column(table, cql_column_schema_create(str8_cstring(key_name), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_cstring(value_name), CQL_ColumnType_U32));
  cql_database_add_table(database, table);
  
  U32* keys = (U32*)cql_column_alloc_fixed_rows(table->columns[0], rows);
  U32* values = (U32*)cql_column_alloc_fixed_rows(table->columns[1], rows);
  if (keys == 0 || values == 0)
  {
    log_error("could not allocate %llu rows for %s", rows, name);
    return;
  }
  for (U64 i = 0; i < rows; i++)
  {
    U32 key = key_start + (U32)i;
    keys[i] = key;
    values[i] = (key / value_divisor) % value_modulus;
  }
  table->row_count = rows;
}

// tec: row j < g_ss_big_dim_matched has the needle_key of row j * (row_count / g_ss_big_dim_rows) of scale_events,
// so it matches exactly one row there. the other rows have the key of row row_count + j, which no row has
internal U32
ss_big_dim_key(U64 fact_row_count, U64 j)
{
  U64 stride = Max(fact_row_count / g_ss_big_dim_rows, (U64)1);
  if (j < g_ss_big_dim_matched)
  {
    return ss_key_of_row(j * stride);
  }
  return ss_key_of_row(fact_row_count + j);
}

internal void
ss_load_key_dimension(CQL_Database* database, U64 fact_row_count)
{
  CQL_Table* table = cql_table_alloc(str8_lit("big_dim"));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("big_key"), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("weight"), CQL_ColumnType_U32));
  cql_database_add_table(database, table);
  
  U32* keys = (U32*)cql_column_alloc_fixed_rows(table->columns[0], g_ss_big_dim_rows);
  U32* weights = (U32*)cql_column_alloc_fixed_rows(table->columns[1], g_ss_big_dim_rows);
  if (keys == 0 || weights == 0)
  {
    log_error("could not allocate %llu rows for big_dim", g_ss_big_dim_rows);
    return;
  }
  for (U64 j = 0; j < g_ss_big_dim_rows; j++)
  {
    keys[j] = ss_big_dim_key(fact_row_count, j);
    weights[j] = (U32)(j % SS_BIG_DIM_WEIGHTS);
  }
  table->row_count = g_ss_big_dim_rows;
}

internal void
ss_load_dimensions_cql(CQL_Database* database, U64 fact_row_count)
{
  ss_load_dimension(database, "regions", "region_id", "zone", SS_REGIONS, 0, SS_ZONE_SIZE, 0xffffffffu);
  ss_load_dimension(database, "products", "product_id", "category", SS_PRODUCT_ROWS, 0, 1, SS_PRODUCT_CATEGORIES);
  ss_load_key_dimension(database, fact_row_count);
}

// tec: returns 0 when duckdb could not build the table, at a billion rows that is its memory running out next to ours
internal B32
ss_load_duckdb(duckdb_connection conn, U64 row_count)
{
  char sql[640];
  snprintf(sql, sizeof(sql),
           "CREATE TABLE scale_events AS SELECT CAST(i + 1 AS INTEGER) AS id, CAST(i %% %d AS INTEGER) AS region_id, "
           "CAST(i %% %d AS INTEGER) AS amount, CAST((i * %llu) %% 4294967296 AS UINTEGER) AS needle_key FROM range(%llu) AS r(i);",
           SS_REGIONS, SS_AMOUNT_RANGE, SS_KEY_MULTIPLIER, row_count);
  duckdb_result result = {0};
  B32 loaded = 1;
  if (duckdb_query(conn, sql, &result) != DuckDBSuccess)
  {
    log_error("duckdb create table failed: %s", duckdb_result_error(&result));
    loaded = 0;
  }
  duckdb_destroy_result(&result);
  
  // tec: the small tables, built the same way ss_load_dimensions_cql builds them
  char dimension_sql[3][320];
  snprintf(dimension_sql[0], sizeof(dimension_sql[0]),
           "CREATE TABLE regions AS SELECT CAST(i AS INTEGER) AS region_id, CAST(i // %d AS INTEGER) AS zone FROM range(%d) AS r(i);",
           SS_ZONE_SIZE, SS_REGIONS);
  snprintf(dimension_sql[1], sizeof(dimension_sql[1]),
           "CREATE TABLE products AS SELECT CAST(i AS INTEGER) AS product_id, CAST(i %% %d AS INTEGER) AS category FROM range(%d) AS r(i);",
           SS_PRODUCT_CATEGORIES, SS_PRODUCT_ROWS);
  U64 big_dim_stride = Max(row_count / g_ss_big_dim_rows, (U64)1);
  snprintf(dimension_sql[2], sizeof(dimension_sql[2]),
           "CREATE TABLE big_dim AS SELECT CAST(((CASE WHEN i < %llu THEN i * %llu ELSE %llu + i END) * %llu) %% 4294967296 AS UINTEGER) AS big_key, "
           "CAST(i %% %d AS INTEGER) AS weight FROM range(%llu) AS r(i);",
           g_ss_big_dim_matched, big_dim_stride, row_count, SS_KEY_MULTIPLIER, SS_BIG_DIM_WEIGHTS, g_ss_big_dim_rows);
  for (U32 d = 0; d < 3 && loaded; d++)
  {
    duckdb_result dimension_result = {0};
    if (duckdb_query(conn, dimension_sql[d], &dimension_result) != DuckDBSuccess)
    {
      log_error("duckdb create dimension table failed: %s", duckdb_result_error(&dimension_result));
      loaded = 0;
    }
    duckdb_destroy_result(&dimension_result);
  }
  return loaded;
}

//~ tec: verification run, reads the answer back from the engine

internal SS_Result
ss_run_cql_once(CQL_Database* database, String8 sql_text)
{
  SS_Result out = {0};
  Temp scratch = scratch_begin(0, 0);
  Arena* arena = scratch.arena;
  
  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;
  
  U64 temp_mark = database->temp_table_count;
  PLAN_ExecResult result = plan_run_select(arena, database, select_node, NULL);
  if (result.supported)
  {
    out.ok = 1;
    if (result.is_materialized)
    {
      out.rows = result.materialized.count;
      out.value_count = Min(result.materialized.column_count, SS_MAX_VALUES);
      for (U64 c = 0; c < out.value_count && out.rows > 0; c++)
      {
        out.values[c] = result.materialized.columns[c].numeric_values[0];
      }
    }
    else
    {
      out.rows = result.rows.count;
      if (out.rows > 0 && result.rows.table_count > 0)
      {
        out.first_row = result.rows.row_indices[0][0];
      }
    }
  }
  cql_database_release_temp_tables_from(database, temp_mark);
  
  scratch_end(scratch);
  return out;
}

internal B32
ss_check_result(Bench_Report* report, SS_Case* test_case, SS_Result* result)
{
  B32 passed = 1;
  if (!result->ok)
  {
    bench_report_warn(report, "%s: query not supported", test_case->label);
    return 0;
  }
  if (result->rows != test_case->expected_rows)
  {
    bench_report_warn(report, "%s: expected %llu rows, got %llu", test_case->label, test_case->expected_rows, result->rows);
    passed = 0;
  }
  if (test_case->expected_first_row != max_U64 && result->first_row != test_case->expected_first_row)
  {
    bench_report_warn(report, "%s: expected first row index %llu, got %llu", test_case->label, test_case->expected_first_row, result->first_row);
    passed = 0;
  }
  if (test_case->value_count > 0 && result->value_count < test_case->value_count)
  {
    bench_report_warn(report, "%s: expected %llu value columns, got %llu (result was not materialized)",
                      test_case->label, test_case->value_count, result->value_count);
    return 0;
  }
  for (U64 v = 0; v < test_case->value_count; v++)
  {
    if (result->values[v] != test_case->expected_values[v])
    {
      bench_report_warn(report, "%s: value %llu expected %.0f, got %.0f",
                        test_case->label, v, test_case->expected_values[v], result->values[v]);
      passed = 0;
    }
  }
  return passed;
}

//~ tec: memory bandwidth
// a query that reads a whole column cannot finish faster than the column streams from memory. reported on large runs against the theoretical peak of system RAM and of GPU memory,
// next to the read bandwidth measured here before the data is loaded

#define SS_DEFAULT_RAM_PEAK_GBPS  51.2
#define SS_DEFAULT_VRAM_PEAK_GBPS 384.0
#define SS_DEFAULT_BANDWIDTH_MIN_ROWS 100000000
#define SS_BANDWIDTH_BUFFER_BYTES GB(2)
#define SS_GPU_BANDWIDTH_BUFFER_BYTES GB(1)
#define SS_MAX_BANDWIDTH_SAMPLES 32

#define SS_TOUCH_COLUMN_COUNT 4
global char* g_ss_touch_columns[SS_TOUCH_COLUMN_COUNT] = { "id", "region_id", "amount", "needle_key" };

typedef struct SS_BandwidthTask SS_BandwidthTask;
struct SS_BandwidthTask
{
  U64* data;
  Rng1U64* ranges;
  U64* sums;
  B32 use_avx2;
};

typedef struct SS_BandwidthSample SS_BandwidthSample;
struct SS_BandwidthSample
{
  char* label;
  char* engine;
  U64 bytes;
  F64 exec_ms;
};

internal THREAD_POOL_TASK_FUNC(ss_bandwidth_read_task)
{
  SS_BandwidthTask* task = (SS_BandwidthTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  U64 total = 0;
  U64 i = range.min;
  
  if (task->use_avx2)
  {
    // tec: 128 bytes per step in four independent chains, so the loads are limited by memory and not by the adds
    __m256i a0 = _mm256_setzero_si256();
    __m256i a1 = _mm256_setzero_si256();
    __m256i a2 = _mm256_setzero_si256();
    __m256i a3 = _mm256_setzero_si256();
    for (; i + 16 <= range.max; i += 16)
    {
      a0 = _mm256_add_epi64(a0, _mm256_loadu_si256((__m256i*)(task->data + i + 0)));
      a1 = _mm256_add_epi64(a1, _mm256_loadu_si256((__m256i*)(task->data + i + 4)));
      a2 = _mm256_add_epi64(a2, _mm256_loadu_si256((__m256i*)(task->data + i + 8)));
      a3 = _mm256_add_epi64(a3, _mm256_loadu_si256((__m256i*)(task->data + i + 12)));
    }
    __m256i sum = _mm256_add_epi64(_mm256_add_epi64(a0, a1), _mm256_add_epi64(a2, a3));
    U64 lanes[4];
    _mm256_storeu_si256((__m256i*)lanes, sum);
    total = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  }
  else
  {
    U64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (; i + 4 <= range.max; i += 4)
    {
      a0 += task->data[i + 0];
      a1 += task->data[i + 1];
      a2 += task->data[i + 2];
      a3 += task->data[i + 3];
    }
    total = a0 + a1 + a2 + a3;
  }
  for (; i < range.max; i++)
  {
    total += task->data[i];
  }
  task->sums[task_id] = total;
}

internal B32
ss_cpu_has_avx2(void)
{
  int info[4] = {0};
  __cpuid(info, 0);
  if (info[0] < 7)
  {
    return 0;
  }
  __cpuidex(info, 7, 0);
  return (info[1] >> 5) & 1;
}

// tec: best of several passes of every worker streaming its own slice of a buffer much bigger than the CPU caches, with one worker per core and one per thread
internal F64
ss_measure_ram_read_gbps(void)
{
  U64 word_count = SS_BANDWIDTH_BUFFER_BYTES / sizeof(U64);
  Arena* buffer_arena = arena_alloc(.reserve_size = SS_BANDWIDTH_BUFFER_BYTES + MB(64), .commit_size = MB(64));
  U64* data = push_array(buffer_arena, U64, word_count);
  for (U64 i = 0; i < word_count; i++)
  {
    data[i] = i * 2654435761ULL;
  }
  
  TP_Context* pool = app_thread_pool();
  TP_Arena* pool_arena = app_thread_pool_arena();
  B32 use_avx2 = ss_cpu_has_avx2();
  
  U64 task_counts[2] = { Max((U64)1, (U64)pool->worker_count / 2), Max((U64)1, (U64)pool->worker_count) };
  
  F64 best_gbps = 0.0;
  U64 sink = 0;
  for (U32 variant = 0; variant < ArrayCount(task_counts); variant++)
  {
    U64 task_count = task_counts[variant];
    Temp scratch = scratch_begin(0, 0);
    SS_BandwidthTask task = {0};
    task.data = data;
    task.ranges = tp_divide_work(scratch.arena, word_count, (U32)task_count);
    task.sums = push_array(scratch.arena, U64, task_count);
    task.use_avx2 = use_avx2;
    
    for (U32 pass = 0; pass < 8; pass++)
    {
      U64 start = os_now_microseconds();
      TP_Temp temp = tp_temp_begin(pool_arena);
      tp_for_parallel(pool, pool_arena, task_count, ss_bandwidth_read_task, &task);
      tp_temp_end(temp);
      F64 seconds = (F64)(os_now_microseconds() - start) / 1000000.0;
      
      // tec: the first pass warms the pages and the pool, it is not a sample
      if (pass > 0 && seconds > 0.0)
      {
        best_gbps = Max(best_gbps, (F64)SS_BANDWIDTH_BUFFER_BYTES / 1000000000.0 / seconds);
      }
      for (U64 t = 0; t < task_count; t++)
      {
        sink += task.sums[t];
      }
    }
    scratch_end(scratch);
  }
  arena_release(buffer_arena);
  
  // tec: keeps the reads from being optimized away
  if (sink == 1)
  {
    printf(" ");
  }
  return best_gbps;
}

// tec: the same for the card, a kernel that only reads a buffer much bigger than its caches. bandwidth_read.comp reads 8 vectors per thread, bandwidth_read_narrow.comp one word per thread
// timed with the GPU's timestamps, returns 0 when the card cannot run it
internal F64
ss_measure_gpu_read_gbps(char* kernel_name, U64 bytes_per_thread)
{
  U64 bytes = SS_GPU_BANDWIDTH_BUFFER_BYTES;
  U64 thread_count = bytes / bytes_per_thread;
  
  GPU_Kernel* kernel = gpu_kernel_alloc(str8_cstring(kernel_name));
  GPU_Buffer* input = gpu_buffer_alloc(bytes, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* sink = gpu_buffer_alloc(16, GPU_BufferFlag_ReadWrite, 0);
  if (!kernel || !input || !sink)
  {
    if (input)
    {
      gpu_buffer_release(input);
    }
    if (sink)
    {
      gpu_buffer_release(sink);
    }
    return 0.0;
  }
  
  GPU_Batch* fill = gpu_batch_begin(0, 0);
  gpu_batch_buffer_fill(fill, input, bytes, 0x5a5a5a5au);
  gpu_batch_end(fill);
  
  gpu_kernel_set_arg_buffer(kernel, 0, input);
  gpu_kernel_set_arg_buffer(kernel, 3, sink);
  gpu_kernel_set_arg_u64(kernel, 0, thread_count);
  gpu_kernel_set_arg_u64(kernel, 1, 1);
  
  F64 best_gbps = 0.0;
  for (U32 pass = 0; pass < 10; pass++)
  {
    GPU_Batch* batch = gpu_batch_begin(0, 0);
    gpu_batch_kernel_execute(batch, kernel, (U32)thread_count, 256);
    gpu_batch_end(batch);
    U64 microseconds = gpu_get_executed_kernel_time_microseconds();
    
    // tec: the first pass warms the pipeline, it is not a sample
    if (pass > 0 && microseconds > 0)
    {
      best_gbps = Max(best_gbps, (F64)bytes / 1000000000.0 / ((F64)microseconds / 1000000.0));
    }
  }
  
  gpu_kernel_release(kernel);
  gpu_buffer_release(input);
  gpu_buffer_release(sink);
  return best_gbps;
}

// tec: the peak of the installed memory is its configured transfer rate x 8 bytes x channels, a kit below its rated speed shows its real rate
internal B32
ss_detect_ram_peak_gbps(F64* out_gbps, char* out_description, U64 description_size)
{
  FILE* pipe = _popen("powershell -NoProfile -Command \"(Get-CimInstance Win32_PhysicalMemory).ConfiguredClockSpeed\"", "r");
  if (!pipe)
  {
    return 0;
  }
  
  U32 module_count = 0;
  U32 slowest = 0;
  char line[64];
  while (fgets(line, sizeof(line), pipe))
  {
    U32 speed = (U32)atoi(line);
    if (speed > 0)
    {
      module_count += 1;
      if (slowest == 0 || speed < slowest)
      {
        slowest = speed;
      }
    }
  }
  _pclose(pipe);
  
  if (module_count == 0)
  {
    return 0;
  }
  U32 channels = Min(module_count, (U32)2);
  *out_gbps = (F64)slowest * 8.0 * (F64)channels / 1000.0;
  snprintf(out_description, description_size, "detected: %u module(s) configured at %u MT/s, %u channel(s)", module_count, slowest, channels);
  return 1;
}

internal F64
ss_f64_argument(CmdLine* cmdline, char* name, F64 fallback)
{
  String8 key = str8_cstring(name);
  if (cmd_line_has_argument(cmdline, key))
  {
    return f64_from_str8(cmd_line_string(cmdline, key));
  }
  return fallback;
}

internal void
ss_report_bandwidth(Bench_Report* report, SS_BandwidthSample* samples, U64 sample_count, F64 ram_peak_gbps, char* ram_peak_source, F64 vram_peak_gbps, F64 ram_measured_gbps, F64 vram_measured_gbps, F64 vram_narrow_gbps)
{
  // tec: a column bigger than one GPU buffer is scanned on the CPU out of system RAM, a smaller one is streamed from GPU memory
  U64 gpu_buffer_cap = settings_u64(str8_lit("GPU_MAX_BUFFER_SIZE"), GPU_MAX_BUFFER_SIZE);
  
  bench_report_section(report, "memory bandwidth");
  bench_report_text(report,
                    "data scanned is the bytes of every column a query has to read in full, so it is the least a query can move. "
                    "every query in this test reads a whole column and returns one value or one row");
  bench_report_text(report,
                    "limits: system RAM theoretical peak %.1f GB/s (%s, override with --ram_gbps), "
                    "system RAM read bandwidth sustained on this machine %.1f GB/s (measured by this run, wide loads on all cores), "
                    "GPU memory theoretical peak %.1f GB/s (from the card's bus width and memory rate, override with --vram_gbps), "
                    "GPU memory read bandwidth sustained on this card %.1f GB/s (measured by this run with a kernel that only reads, 8 loads of 16 bytes per thread). "
                    "the same read with one 4 byte load per thread, the way a kernel that handles one row per thread loads, sustains %.1f GB/s. GB is 10^9 bytes",
                    ram_peak_gbps, ram_peak_source, ram_measured_gbps, vram_peak_gbps, vram_measured_gbps, vram_narrow_gbps);
  bench_report_text(report,
                    "the limit that applies to a row is the memory the engine streams the column from, and the measured column compares with the sustained read rate of that same memory. cql scans a column bigger than %.1f GB on the CPU "
                    "out of system RAM and a smaller one on the GPU out of GPU memory. duckdb always reads system RAM, and it compresses this data, "
                    "so its GB/s is the uncompressed size over its time and is not a memory rate",
                    (F64)gpu_buffer_cap / 1000000000.0);
  
  printf("\n=== memory bandwidth ===\n");
  printf("system RAM: theoretical %.1f GB/s (%s), sustained read measured %.1f GB/s\n",
         ram_peak_gbps, ram_peak_source, ram_measured_gbps);
  printf("GPU memory: theoretical %.1f GB/s, sustained read measured %.1f GB/s (%.1f GB/s with one 4 byte load per thread)\n",
         vram_peak_gbps, vram_measured_gbps, vram_narrow_gbps);
  printf("%-28s %-8s %9s %10s %9s %-11s %13s %13s\n",
         "query", "engine", "data GB", "exec ms", "GB/s", "limit", "% theoretical", "% measured");
  
  bench_report_row_begin(report);
  bench_report_cellf(report, "query");
  bench_report_cellf(report, "engine");
  bench_report_cellf(report, "data scanned (GB)");
  bench_report_cellf(report, "exec median (ms)");
  bench_report_cellf(report, "achieved (GB/s)");
  bench_report_cellf(report, "limit that applies");
  bench_report_cellf(report, "%% of theoretical peak");
  bench_report_cellf(report, "%% of measured sustained");
  bench_report_table_header_end(report);
  
  for (U64 i = 0; i < sample_count; i++)
  {
    SS_BandwidthSample* sample = &samples[i];
    F64 gigabytes = (F64)sample->bytes / 1000000000.0;
    F64 gbps = sample->exec_ms > 0.0 ? gigabytes / (sample->exec_ms / 1000.0) : 0.0;
    
    B32 is_cql = sample->engine[0] != 'd';
    B32 from_gpu_memory = is_cql && sample->bytes <= gpu_buffer_cap;
    char* limit_name = from_gpu_memory ? "GPU memory" : "system RAM";
    F64 theoretical_percent = gbps / (from_gpu_memory ? vram_peak_gbps : ram_peak_gbps) * 100.0;
    F64 measured_gbps = from_gpu_memory ? vram_measured_gbps : ram_measured_gbps;
    F64 measured_percent = measured_gbps > 0.0 ? gbps / measured_gbps * 100.0 : -1.0;
    
    if (measured_percent >= 0.0)
    {
      printf("%-28s %-8s %9.3f %10.3f %9.1f %-11s %12.1f%% %12.1f%%\n",
             sample->label, sample->engine, gigabytes, sample->exec_ms, gbps, limit_name, theoretical_percent, measured_percent);
    }
    else
    {
      printf("%-28s %-8s %9.3f %10.3f %9.1f %-11s %12.1f%% %13s\n",
             sample->label, sample->engine, gigabytes, sample->exec_ms, gbps, limit_name, theoretical_percent, "-");
    }
    
    bench_report_row_begin(report);
    bench_report_cellf(report, "%s", sample->label);
    bench_report_cellf(report, "%s", sample->engine);
    bench_report_cellf(report, "%.3f", gigabytes);
    bench_report_cellf(report, "%.3f", sample->exec_ms);
    bench_report_cellf(report, "%.1f", gbps);
    bench_report_cellf(report, "%s", limit_name);
    bench_report_cellf(report, "%.1f%%", theoretical_percent);
    if (measured_percent >= 0.0)
    {
      bench_report_cellf(report, "%.1f%%", measured_percent);
    }
    else
    {
      bench_report_cellf(report, "-");
    }
    bench_report_row_end(report);
  }
}

//~ tec: entry

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  
  // tec: --log-level=error|warn|info|verbose. verbose prints the wall time of every GPU submit with its tag, which shows where a query waits
  String8 log_level_str = cmd_line_string(cmdline, str8_lit("log-level"));
  if (log_level_str.size != 0)
  {
    LogLevel log_level = LogLevel_Info;
    if (log_level_from_string(log_level_str, &log_level))
    {
      log_set_level(log_level);
    }
    else
    {
      log_error("--log-level expects error, warn, info or verbose, got '%.*s'", str8_varg(log_level_str));
    }
  }
  
  U64 row_count = 1000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    row_count = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  U64 duckdb_max_rows = 1000000000;
  if (cmd_line_has_argument(cmdline, str8_lit("duckdb_max_rows")))
  {
    duckdb_max_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("duckdb_max_rows")), 10);
  }
  B32 run_duckdb = !cmd_line_has_flag(cmdline, str8_lit("no_duckdb")) && row_count <= duckdb_max_rows;
  B32 explain = cmd_line_has_flag(cmdline, str8_lit("explain"));
  B32 explain_cold = cmd_line_has_flag(cmdline, str8_lit("explain_cold"));
  B32 run_group_join = !cmd_line_has_flag(cmdline, str8_lit("no_group_join"));
  B32 run_duckdb_narrow = cmd_line_has_flag(cmdline, str8_lit("duckdb_narrow"));
  B32 run_duckdb_rewrite = cmd_line_has_flag(cmdline, str8_lit("duckdb_rewrite"));
  B32 duckdb_narrow_only = cmd_line_has_flag(cmdline, str8_lit("duckdb_narrow_only"));
  run_duckdb_narrow = run_duckdb_narrow || duckdb_narrow_only;
  String8 only = cmd_line_string(cmdline, str8_lit("only"));
  g_bench_print_samples = cmd_line_has_flag(cmdline, str8_lit("samples"));
  // tec: --duckdb_threads=N runs duckdb with N threads instead of its default of one per hardware thread
  U64 duckdb_threads = 0;
  if (cmd_line_has_argument(cmdline, str8_lit("duckdb_threads")))
  {
    duckdb_threads = u64_from_str8(cmd_line_string(cmdline, str8_lit("duckdb_threads")), 10);
  }
  if (cmd_line_has_argument(cmdline, str8_lit("big_dim_rows")))
  {
    g_ss_big_dim_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("big_dim_rows")), 10);
  }
  U64 big_dim_match_percent = 100;
  if (cmd_line_has_argument(cmdline, str8_lit("big_dim_match_percent")))
  {
    U64 requested_percent = u64_from_str8(cmd_line_string(cmdline, str8_lit("big_dim_match_percent")), 10);
    big_dim_match_percent = Min(requested_percent, (U64)100);
  }
  g_ss_big_dim_matched = g_ss_big_dim_rows * big_dim_match_percent / 100;
  
  // tec: ids are U32, and the keys have to stay different from each other, which they do for fewer rows than the 32 bit range holds. duckdb
  // multiplies the row index by the key multiplier in 64 bits, which stays in range for the same limit
  if (row_count == 0 || row_count > SS_MAX_ROWS)
  {
    printf("--rows must be between 1 and %llu\n", SS_MAX_ROWS);
    exit(1);
  }
  if (g_ss_big_dim_rows == 0 || g_ss_big_dim_rows > row_count)
  {
    printf("--big_dim_rows must be between 1 and --rows\n");
    exit(1);
  }
  
  // tec: a column arena has to hold the whole column, reserve is address space only
  {
    char setting[96];
    snprintf(setting, sizeof(setting), "CQL_COLUMN_ARENA_RESERVE_SIZE:%llu", row_count * sizeof(U32) + MB(256));
    settings_load_from_string(str8_cstring(setting));
  }
  // tec: --setting can be given more than once
  String8List setting_list = cmd_line_strings(cmdline, str8_lit("setting"));
  for (String8Node* setting_node = setting_list.first; setting_node; setting_node = setting_node->next)
  {
    settings_load_from_string(setting_node->string);
  }
  
  cql_init();
  gpu_init();
  
  U64 bandwidth_min_rows = SS_DEFAULT_BANDWIDTH_MIN_ROWS;
  if (cmd_line_has_argument(cmdline, str8_lit("bandwidth_min_rows")))
  {
    bandwidth_min_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("bandwidth_min_rows")), 10);
  }
  B32 report_bandwidth = !cmd_line_has_flag(cmdline, str8_lit("no_bandwidth")) && row_count >= bandwidth_min_rows;
  F64 ram_peak_gbps = SS_DEFAULT_RAM_PEAK_GBPS;
  char ram_peak_source[160] = "assumed dual channel DDR4-3200, nothing could be detected";
  if (cmd_line_has_argument(cmdline, str8_lit("ram_gbps")))
  {
    ram_peak_gbps = ss_f64_argument(cmdline, "ram_gbps", SS_DEFAULT_RAM_PEAK_GBPS);
    snprintf(ram_peak_source, sizeof(ram_peak_source), "given with --ram_gbps");
  }
  else if (report_bandwidth)
  {
    F64 detected_gbps = 0.0;
    char detected_description[128] = {0};
    if (ss_detect_ram_peak_gbps(&detected_gbps, detected_description, sizeof(detected_description)))
    {
      ram_peak_gbps = detected_gbps;
      snprintf(ram_peak_source, sizeof(ram_peak_source), "%s", detected_description);
    }
  }
  F64 vram_peak_gbps = ss_f64_argument(cmdline, "vram_gbps", SS_DEFAULT_VRAM_PEAK_GBPS);
  
  // tec: measured before the table is loaded so it has the memory to itself
  F64 ram_measured_gbps = 0.0;
  F64 vram_measured_gbps = 0.0;
  F64 vram_narrow_gbps = 0.0;
  if (report_bandwidth)
  {
    ram_measured_gbps = ss_measure_ram_read_gbps();
    printf("system RAM theoretical peak: %.1f GB/s (%s), sustained read bandwidth: %.1f GB/s\n", ram_peak_gbps, ram_peak_source, ram_measured_gbps);
    vram_measured_gbps = ss_measure_gpu_read_gbps("bandwidth_read", 128);
    vram_narrow_gbps = ss_measure_gpu_read_gbps("bandwidth_read_narrow", 4);
    printf("GPU memory theoretical peak: %.1f GB/s, sustained read bandwidth: %.1f GB/s, with one 4 byte load per thread: %.1f GB/s\n",
           vram_peak_gbps, vram_measured_gbps, vram_narrow_gbps);
  }
  
  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql select scale test");
  bench_report_text(report, "scale_events: %llu rows, 4 U32 columns (%.2f GB); %u timed runs per query",
                    row_count, (F64)(row_count * 4 * sizeof(U32)) / (F64)GB(1), (U32)BENCH_TIMED_RUNS);
  bench_report_text(report, "big_dim: %llu rows, %llu of them match one row of scale_events each, so the join on needle_key matches %.4f%% of scale_events",
                    g_ss_big_dim_rows, g_ss_big_dim_matched, 100.0 * (F64)g_ss_big_dim_matched / (F64)row_count);
  
  bench_report_section(report, "load");
  CQL_Database* database = cql_database_alloc(str8_lit("select_scale_db"));
  cql_add_database(database);
  
  U64 load_start = os_now_microseconds();
  ss_load_cql(database, row_count);
  ss_load_dimensions_cql(database, row_count);
  
  // tec: what the first query on a column pays besides the copy to VRAM: reading the memory, building the zone map and the optimizer statistics. measured before duckdb loads its copy
  F64 touch_read_ms[SS_TOUCH_COLUMN_COUNT] = {0};
  F64 touch_zone_ms[SS_TOUCH_COLUMN_COUNT] = {0};
  F64 touch_stats_ms[SS_TOUCH_COLUMN_COUNT] = {0};
  {
    CQL_Table* touch_table = cql_database_find_table(database, str8_lit("scale_events"));
    for (U64 c = 0; c < SS_TOUCH_COLUMN_COUNT; c++)
    {
      CQL_Column* touch_column = cql_table_find_column(touch_table, str8_cstring(g_ss_touch_columns[c]));
      U32* touch_values = (U32*)touch_column->data;
      U64 read_start = os_now_microseconds();
      U64 read_sum = 0;
      for (U64 row = 0; row < row_count; row++)
      {
        read_sum += touch_values[row];
      }
      touch_read_ms[c] = (F64)(os_now_microseconds() - read_start) / 1000.0;
      
      U64 zone_start = os_now_microseconds();
      cql_column_ensure_zone_map(touch_column);
      touch_zone_ms[c] = (F64)(os_now_microseconds() - zone_start) / 1000.0;
      U64 stats_start = os_now_microseconds();
      cql_column_ensure_stats(touch_column);
      touch_stats_ms[c] = (F64)(os_now_microseconds() - stats_start) / 1000.0;
      printf("first touch %-12s read %9.1f ms (sum %llu)  zone map %9.1f ms  statistics %9.1f ms\n", g_ss_touch_columns[c], touch_read_ms[c], read_sum, touch_zone_ms[c], touch_stats_ms[c]);
    }
  }
  F64 load_seconds = (F64)(os_now_microseconds() - load_start) / 1000000.0;
  printf("load cql    %12llu rows  %8.2f s\n", row_count, load_seconds);
  bench_report_text(report, "load cql: %llu rows in %.2f s", row_count, load_seconds);
  
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  if (run_duckdb)
  {
    duckdb_open(NULL, &duckdb_db);
    duckdb_connect(duckdb_db, &duckdb_conn);
    if (duckdb_threads > 0)
    {
      char threads_sql[64];
      snprintf(threads_sql, sizeof(threads_sql), "SET threads = %llu;", duckdb_threads);
      duckdb_result threads_result = {0};
      if (duckdb_query(duckdb_conn, threads_sql, &threads_result) != DuckDBSuccess)
      {
        log_error("duckdb '%s' failed: %s", threads_sql, duckdb_result_error(&threads_result));
      }
      duckdb_destroy_result(&threads_result);
      bench_report_text(report, "duckdb runs with %llu threads (--duckdb_threads)", duckdb_threads);
    }
    load_start = os_now_microseconds();
    B32 duckdb_loaded = ss_load_duckdb(duckdb_conn, row_count);
    load_seconds = (F64)(os_now_microseconds() - load_start) / 1000000.0;
    if (duckdb_loaded)
    {
      printf("load duckdb %12llu rows  %8.2f s\n", row_count, load_seconds);
      bench_report_text(report, "load duckdb: %llu rows in %.2f s", row_count, load_seconds);
    }
    else
    {
      // tec: the queries below would only fail, so the run goes on without duckdb
      printf("load duckdb failed after %.2f s, continuing without it\n", load_seconds);
      bench_report_warn(report, "duckdb could not load %llu rows (likely out of memory next to cql's table), so it is not in this report", row_count);
      duckdb_disconnect(&duckdb_conn);
      duckdb_close(&duckdb_db);
      run_duckdb = 0;
    }
    
    // tec: --duckdb_narrow adds a second copy with region_id as TINYINT and amount as SMALLINT, 
    //  which is how duckdb would be given these columns by someone tuning for it. 
    // it costs the memory of a second copy
    if (run_duckdb && run_duckdb_narrow)
    {
      duckdb_result narrow_result = {0};
      if (duckdb_query(duckdb_conn, "CREATE TABLE scale_events_narrow AS SELECT id, CAST(region_id AS TINYINT) AS region_id, CAST(amount AS SMALLINT) AS amount, needle_key FROM scale_events;", &narrow_result) != DuckDBSuccess)
      {
        log_error("duckdb create narrow table failed: %s", duckdb_result_error(&narrow_result));
        bench_report_warn(report, "duckdb could not build the narrow copy of the table, so there is no duckdb narrow row");
        run_duckdb_narrow = 0;
      }
      duckdb_destroy_result(&narrow_result);
      
      // tec: --duckdb_narrow_only gives duckdb the narrow table alone, under the original name, so every duckdb row is on 1 and 2 byte columns and there is no second copy in memory
      if (run_duckdb_narrow && duckdb_narrow_only)
      {
        duckdb_result swap_result = {0};
        if (duckdb_query(duckdb_conn, "DROP TABLE scale_events; ALTER TABLE scale_events_narrow RENAME TO scale_events;", &swap_result) != DuckDBSuccess)
        {
          log_error("duckdb swap to the narrow table failed: %s", duckdb_result_error(&swap_result));
          bench_report_warn(report, "duckdb could not replace the table with its narrow copy, so its rows are on 4 byte columns");
        }
        else
        {
          bench_report_text(report, "duckdb runs on region_id as TINYINT and amount as SMALLINT (--duckdb_narrow_only), needle_key and id stay INTEGER");
        }
        duckdb_destroy_result(&swap_result);
        run_duckdb_narrow = 0;
      }
    }
  }
  else
  {
    bench_report_text(report, "duckdb skipped (--no_duckdb, or rows above --duckdb_max_rows=%llu)", duckdb_max_rows);
  }
  
  // tec: the row searched for sits in the middle of the table, and the key that is not in it comes from a row number past the end, which no
  // row has, so the keys of two rows never collide
  U64 needle_row = row_count / 2;
  while (!ss_key_is_searchable(ss_key_of_row(needle_row)))
  {
    needle_row += 1;
  }
  U32 needle_key = ss_key_of_row(needle_row);
  
  U64 absent_row = row_count + 12345;
  while (!ss_key_is_searchable(ss_key_of_row(absent_row)))
  {
    absent_row += 1;
  }
  U32 absent_key = ss_key_of_row(absent_row);
  
  U64 high_amount_rows = ss_count_amount_above(row_count, SS_HIGH_AMOUNT);
  
  char sql_needle_row[200], sql_needle_count[200], sql_absent_count[200], sql_count_high[160], sql_region[160];
  char sql_id_range[160];
  snprintf(sql_id_range, sizeof(sql_id_range), "SELECT COUNT(*) FROM scale_events WHERE id > 1000 AND id <= 900000000;");
  snprintf(sql_needle_row, sizeof(sql_needle_row), "SELECT id, amount FROM scale_events WHERE needle_key = %u;", needle_key);
  snprintf(sql_needle_count, sizeof(sql_needle_count), "SELECT COUNT(*) FROM scale_events WHERE needle_key = %u;", needle_key);
  snprintf(sql_absent_count, sizeof(sql_absent_count), "SELECT COUNT(*) FROM scale_events WHERE needle_key = %u;", absent_key);
  snprintf(sql_count_high, sizeof(sql_count_high), "SELECT COUNT(*) FROM scale_events WHERE amount > %d;", SS_HIGH_AMOUNT);
  snprintf(sql_region, sizeof(sql_region), "SELECT COUNT(*) FROM scale_events WHERE region_id = 7;");
  
  // tec: zone 2 holds the regions 20..29
  U64 join_zone_rows = 0;
  F64 join_zone_amount = 0.0;
  for (U64 r = 2 * SS_ZONE_SIZE; r < 3 * SS_ZONE_SIZE; r++)
  {
    join_zone_rows += ss_region_rows(row_count, r);
    join_zone_amount += ss_region_amount_sum(row_count, r);
  }
  
  SS_Case cases[] =
  {
    { "find one row by key",
      sql_needle_row,
      1, 0, {0}, needle_row, row_count * sizeof(U32) },
    { "count key, present once",
      sql_needle_count,
      1, 1, { 1.0 }, max_U64, row_count * sizeof(U32) },
    { "count key, not present",
      sql_absent_count,
      1, 1, { 0.0 }, max_U64, row_count * sizeof(U32) },
    { "sum, min, max",
      "SELECT SUM(amount), MIN(amount), MAX(amount) FROM scale_events;",
      1, 3, { ss_sum_amount(row_count), 0, (F64)Min((U64)(SS_AMOUNT_RANGE - 1), row_count - 1) }, max_U64, row_count * sizeof(U32) },
    { "filter count, 0.1%",
      sql_count_high,
      1, 1, { (F64)high_amount_rows }, max_U64, row_count * sizeof(U32) },
    { "filter count, 2%",
      sql_region,
      1, 1, { (F64)ss_count_region(row_count, 7) }, max_U64, row_count * sizeof(U32) },
    
    // tec: id is its row number plus one, which the GPU computes instead of reading
    { "filter count, id range (row number)",
      sql_id_range,
      1, 1, { (F64)(Min(row_count, (U64)900000000) - Min(row_count, (U64)1000)) }, max_U64, row_count * sizeof(U32) },
    
    // tec: GROUP BY and joins come last so a failure cannot hide earlier results. an ordered group by has its first row checked, the others are compared with duckdb, one row joins against closed forms
    { "group by region, 50 groups",
      "SELECT region_id, COUNT(*), SUM(amount) FROM scale_events GROUP BY region_id ORDER BY region_id;",
      SS_REGIONS, 3, { 0.0, (F64)ss_region_rows(row_count, 0), ss_region_amount_sum(row_count, 0) }, max_U64, 2 * row_count * sizeof(U32) },
    { "group by amount, 10k groups",
      "SELECT amount, COUNT(*), SUM(region_id) FROM scale_events GROUP BY amount ORDER BY amount DESC;",
      SS_AMOUNT_RANGE, 3, { (F64)(SS_AMOUNT_RANGE - 1), (F64)ss_amount_rows(row_count, SS_AMOUNT_RANGE - 1),
        (F64)(ss_amount_rows(row_count, SS_AMOUNT_RANGE - 1) * ((SS_AMOUNT_RANGE - 1) % SS_REGIONS)) }, max_U64, 2 * row_count * sizeof(U32) },
    { "group by key after filter",
      "SELECT COUNT(*) FROM (SELECT needle_key FROM scale_events WHERE amount > 9990 GROUP BY needle_key) AS d;",
      1, 1, { (F64)high_amount_rows }, max_U64, row_count * sizeof(U32) },
    { "join 50 row table, filtered",
      "SELECT COUNT(*), SUM(scale_events.amount) FROM scale_events JOIN regions ON scale_events.region_id = regions.region_id WHERE regions.zone = 2;",
      1, 2, { (F64)join_zone_rows, join_zone_amount }, max_U64, row_count * sizeof(U32) },
    { "join 10k row table, group by",
      "SELECT products.category, COUNT(*) FROM scale_events JOIN products ON scale_events.amount = products.product_id GROUP BY products.category;",
      SS_PRODUCT_CATEGORIES, 0, {0}, max_U64, row_count * sizeof(U32) },
    { "join 1M row table",
      "SELECT COUNT(*), SUM(big_dim.weight) FROM scale_events JOIN big_dim ON scale_events.needle_key = big_dim.big_key;",
      1, (g_ss_big_dim_matched > 0) ? 2 : 1, { (F64)g_ss_big_dim_matched, ss_weight_sum(g_ss_big_dim_matched) }, max_U64, row_count * sizeof(U32) },
  };
  U64 select_case_count = 7;
  U64 case_count = (run_group_join && row_count >= SS_GROUP_JOIN_MIN_ROWS) ? ArrayCount(cases) : select_case_count;
  
  bench_report_section(report, "select queries (%llu rows)", row_count);
  bench_print_table_header(report, "select scale");
  
  SS_BandwidthSample bandwidth_samples[SS_MAX_BANDWIDTH_SAMPLES] = {0};
  U64 bandwidth_sample_count = 0;
  
  // tec: text written between the rows of a markdown table ends the table, so the notes wait until it is complete
  String8List notes = {0};
  
  U64 failures = 0;
  for (U64 i = 0; i < case_count; i++)
  {
    SS_Case* test_case = &cases[i];
    String8 label = str8_cstring(test_case->label);
    String8 sql = str8_cstring(test_case->sql);
    
    // tec: --only=<part of a label> runs the matching queries, avoid parentheses and spaces in it, cmd mangles them
    if (only.size > 0 && str8_find_needle(label, 0, only, 0) >= label.size)
    {
      continue;
    }
    
    // tec: the first run starts with nothing resident on the GPU, so it pays the whole copy. the timed runs after it find the chunks it left in VRAM
    // unless --cold, which empties the cache before every run too
    // tec: --explain_cold runs the query once under EXPLAIN ANALYZE with nothing resident first, to see where a first execution spends its time
    if (explain_cold)
    {
      qe_resident_clear();
      Arena* cold_arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(4));
      String8 cold_sql = push_str8f(cold_arena, "EXPLAIN ANALYZE %.*s", str8_varg(sql));
      APP_QueryResult cold_result = app_execute_query_capture(cold_arena, cold_sql, &database, NULL);
      printf("cold explain, %.*s:\n%.*s\n", str8_varg(label), str8_varg(cold_result.output_text));
      arena_release(cold_arena);
    }
    qe_resident_clear();
    QE_ResidentStats resident_before = qe_resident_stats();
    gpu_kernel_log_reset();
    U64 cold_start = os_now_microseconds();
    SS_Result verified = ss_run_cql_once(database, sql);
    F64 cold_ms = (F64)(os_now_microseconds() - cold_start) / 1000.0;
    {
      Temp kernel_scratch = scratch_begin(0, 0);
      String8 kernels = bench_kernel_log_string(kernel_scratch.arena);
      printf("  kernels: %.*s\n", str8_varg(kernels));
      str8_list_pushf(report->arena, &notes, "%.*s: kernels %.*s", str8_varg(label), str8_varg(kernels));
      scratch_end(kernel_scratch);
    }
    QE_ResidentStats resident_after = qe_resident_stats();
    if (!ss_check_result(report, test_case, &verified))
    {
      failures += 1;
      printf("FAIL %s\n", test_case->label);
    }
    
    U64 cql_rows = 0, cql_checksum = 0;
    Bench_Stats cql_stats = bench_run_cql_query(database, sql, &cql_rows, &cql_checksum);
    bench_print_table_row(report, label, "cql", cql_rows, cql_checksum, &cql_stats);
    QE_ResidentStats resident_warm = qe_resident_stats();
    printf("  cold (nothing resident) %.1f ms, copied %.0f MB; warm median %.1f ms, %.0f MB resident, %.0f MB hit during the timed runs\n",
           cold_ms, (F64)(resident_after.miss_bytes - resident_before.miss_bytes) / (1024 * 1024), cql_stats.execute_median, (F64)resident_warm.bytes_in_use / (1024 * 1024),
           (F64)(resident_warm.hit_bytes - resident_after.hit_bytes) / (1024 * 1024));
    printf("  device memory after this query: %.0f MB free of %.0f MB, resident cache %.0f MB\n", (F64)gpu_device_free_memory() / (1024 * 1024), (F64)gpu_device_total_memory() / (1024 * 1024),
           (F64)resident_warm.bytes_in_use / (1024 * 1024));
    str8_list_pushf(report->arena, &notes, "%.*s: cold %.1f ms (copied %.0f MB), warm median %.1f ms, %.0f MB resident", str8_varg(label), cold_ms,
                    (F64)(resident_after.miss_bytes - resident_before.miss_bytes) / (1024 * 1024), cql_stats.execute_median, (F64)resident_warm.bytes_in_use / (1024 * 1024));
    
    // tec: the plan of the query with the time each stage took, after the timed runs so everything it uses is warm
    if (explain)
    {
      Arena* explain_arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(4));
      String8 explain_sql = push_str8f(explain_arena, "EXPLAIN ANALYZE %.*s", str8_varg(sql));
      APP_QueryResult explain_result = app_execute_query_capture(explain_arena, explain_sql, &database, NULL);
      printf("%.*s\n", str8_varg(explain_result.output_text));
      str8_list_pushf(report->arena, &notes, "explain analyze, %.*s:\n\n```text\n%.*s\n```", str8_varg(label), str8_varg(explain_result.output_text));
      arena_release(explain_arena);
    }
    if (report_bandwidth && test_case->scan_bytes > 0 && bandwidth_sample_count < SS_MAX_BANDWIDTH_SAMPLES)
    {
      bandwidth_samples[bandwidth_sample_count++] = (SS_BandwidthSample){ test_case->label, "cql", test_case->scan_bytes, cql_stats.execute_median };
    }
    
    if (run_duckdb)
    {
      U64 rows = 0, checksum = 0;
      Bench_Stats stats = bench_run_duckdb_query(duckdb_conn, sql, &rows, &checksum);
      bench_print_table_row(report, label, "duckdb", rows, checksum, &stats);
      if (report_bandwidth && test_case->scan_bytes > 0 && bandwidth_sample_count < SS_MAX_BANDWIDTH_SAMPLES)
      {
        bandwidth_samples[bandwidth_sample_count++] = (SS_BandwidthSample){ test_case->label, "duckdb", test_case->scan_bytes, stats.execute_median };
      }
      bench_check_match(report, label, "cql", cql_rows, cql_checksum, "duckdb", rows, checksum);
    }
    
    // tec: the same query on a duckdb table with 1 and 2 byte columns. a query that reads neither of them is the same as the duckdb row above
    if (run_duckdb && run_duckdb_narrow && (strstr(test_case->sql, "amount") || strstr(test_case->sql, "region_id")))
    {
      char narrow_sql[640];
      U64 narrow_size = 0;
      char* cursor = test_case->sql;
      while (*cursor != 0 && narrow_size + 32 < sizeof(narrow_sql))
      {
        if (strncmp(cursor, "scale_events", 12) == 0)
        {
          narrow_size += snprintf(narrow_sql + narrow_size, sizeof(narrow_sql) - narrow_size, "scale_events_narrow");
          cursor += 12;
        }
        else
        {
          narrow_sql[narrow_size++] = *cursor;
          cursor += 1;
        }
      }
      narrow_sql[narrow_size] = 0;
      U64 rows = 0, checksum = 0;
      Bench_Stats stats = bench_run_duckdb_query(duckdb_conn, str8_cstring(narrow_sql), &rows, &checksum);
      bench_print_table_row(report, label, "duckdb narrow", rows, checksum, &stats);
    }
    
    // tec: the 2d joins with the large side reduced to one row per key first, which is the plan cql takes.
    // --duckdb_rewrite gives duckdb the same shape
    if (run_duckdb && run_duckdb_rewrite && (strcmp(test_case->label, "join 50 row table, filtered") == 0 || strcmp(test_case->label, "join 10k row table, group by") == 0))
    {
      char* fact = run_duckdb_narrow ? "scale_events_narrow" : "scale_events";
      char rewrite_sql[640];
      if (strcmp(test_case->label, "join 50 row table, filtered") == 0)
      {
        snprintf(rewrite_sql, sizeof(rewrite_sql),
                 "SELECT SUM(e.c), SUM(e.s) FROM (SELECT region_id, COUNT(*) AS c, SUM(amount) AS s FROM %s GROUP BY region_id) AS e "
                 "JOIN regions ON e.region_id = regions.region_id WHERE regions.zone = 2;", fact);
      }
      else
      {
        snprintf(rewrite_sql, sizeof(rewrite_sql),
                 "SELECT products.category, SUM(e.c) FROM (SELECT amount, COUNT(*) AS c FROM %s GROUP BY amount) AS e "
                 "JOIN products ON e.amount = products.product_id GROUP BY products.category;", fact);
      }
      U64 rows = 0, checksum = 0;
      Bench_Stats stats = bench_run_duckdb_query(duckdb_conn, str8_cstring(rewrite_sql), &rows, &checksum);
      bench_print_table_row(report, label, "duckdb rewrite", rows, checksum, &stats);
      bench_check_match(report, label, "cql", cql_rows, cql_checksum, "duckdb rewrite", rows, checksum);
    }
    
    fflush(stdout);
  }
  
  for (String8Node* note = notes.first; note != 0; note = note->next)
  {
    bench_report_text(report, "%.*s", str8_varg(note->string));
  }
  
  bench_report_section(report, "first touch cost of each column");
  bench_report_text(report, "one pass over the column in host memory, then a build of its zone map and of its optimizer statistics, in ms, measured right "
                    "after the table is built and before duckdb loads its copy, so the host memory is not under pressure. a column pays the zone map and "
                    "statistics cost once, in the first query that needs them, so the cold figures above do not include it");
  bench_report_row_begin(report);
  bench_report_cellf(report, "column");
  bench_report_cellf(report, "read memory (ms)");
  bench_report_cellf(report, "zone map build (ms)");
  bench_report_cellf(report, "statistics build (ms)");
  bench_report_table_header_end(report);
  for (U64 c = 0; c < SS_TOUCH_COLUMN_COUNT; c++)
  {
    bench_report_row_begin(report);
    bench_report_cellf(report, "%s", g_ss_touch_columns[c]);
    bench_report_cellf(report, "%.1f", touch_read_ms[c]);
    bench_report_cellf(report, "%.1f", touch_zone_ms[c]);
    bench_report_cellf(report, "%.1f", touch_stats_ms[c]);
    bench_report_row_end(report);
  }
  
  if (report_bandwidth)
  {
    ss_report_bandwidth(report, bandwidth_samples, bandwidth_sample_count, ram_peak_gbps, ram_peak_source, vram_peak_gbps, ram_measured_gbps, vram_measured_gbps, vram_narrow_gbps);
  }
  
  bench_report_text(report, "%llu of %llu queries failed verification", failures, case_count);
  printf("%llu of %llu queries failed verification\n", failures, case_count);
  
  if (run_duckdb)
  {
    duckdb_disconnect(&duckdb_conn);
    duckdb_close(&duckdb_db);
  }
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  char report_path[96];
  snprintf(report_path, sizeof(report_path), "bench_reports/select_scale_%llu_report.md", row_count);
  bench_report_write(report, str8_cstring(report_path));
  
  arena_release(arena);
  log_release();
  
  ProfEnd();
  ProfEndCapture();
  
  if (failures > 0)
  {
    exit(1);
  }
}
