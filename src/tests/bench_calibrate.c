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

#define CAL_WARM_RUNS 15
#define CAL_ROW_SIZE_COUNT 6

typedef struct Cal_Sample Cal_Sample;
struct Cal_Sample
{
  U64 rows;
  F64 gpu_cold_us;
  F64 gpu_warm_us;
  F64 cpu_us;
  F64 index_us;
  F64 fill_us;
  F64 gpu_bare_us;
  F64 cpu_three_leaves_us;
  F64 cpu_three_reversed_us;
  F64 filter_us;
};

internal int
cal_compare_f64(const void* a, const void* b)
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

internal F64
cal_median(F64* samples, U64 count)
{
  quick_sort(samples, count, sizeof(F64), cal_compare_f64);
  return samples[count / 2];
}

internal GDB_Table*
cal_build_table(GDB_Database* database, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  String8 name = push_str8f(scratch.arena, "cal_%llu", row_count);
  GDB_Table* table = gdb_table_alloc(name);
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("id"), GDB_ColumnType_U32));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("value"), GDB_ColumnType_F64));
  gdb_database_add_table(database, table);
  
  for (U64 row = 0; row < row_count; row += 1)
  {
    U32 id = (U32)row;
    F64 value = (F64)row;
    void* row_data[2] = { &id, &value };
    gdb_table_add_row(table, row_data, NULL);
  }
  scratch_end(scratch);
  return table;
}

// tec: the parsed WHERE keeps living in the arena the caller passes
internal IR_Node*
cal_parse_where(Arena* arena, String8 sql_text)
{
  SQL_TokenizeResult tokens = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tokens.tokens, tokens.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  return ir_node_find_child(ir_query->execution_nodes, IR_NodeType_Where);
}

internal Cal_Sample
cal_measure(Arena* arena, GDB_Database* database, U64 row_count)
{
  Cal_Sample sample = {0};
  sample.rows = row_count;
  
  GDB_Table* table = cal_build_table(database, row_count);
  
  // tec: matches a tenth of the rows
  U64 threshold = row_count / 10;
  Temp scratch = scratch_begin(&arena, 1);
  String8 sql = push_str8f(scratch.arena, "SELECT id FROM %.*s WHERE value < %llu;", str8_varg(table->name), threshold);
  IR_Node* where = cal_parse_where(scratch.arena, sql);
  
  GDB_Column* value_column = gdb_table_find_column(table, str8_lit("value"));
  GDB_Index* index = gdb_table_create_index(table, str8_lit("cal_index"), value_column);
  (void)index;
  
  //- tec: GPU scan, the first call uploads the column and later calls reuse it
  {
    U64 start = os_now_microseconds();
    QE_ScanResult cold = qe_scan_filter(scratch.arena, database, table, where, NULL);
    sample.gpu_cold_us = (F64)(os_now_microseconds() - start);
    (void)cold;
    
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64 run_start = os_now_microseconds();
      QE_ScanResult warm = qe_scan_filter(run_scratch.arena, database, table, where, NULL);
      samples[run] = (F64)(os_now_microseconds() - run_start);
      (void)warm;
      scratch_end(run_scratch);
    }
    sample.gpu_warm_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  //- tec: CPU scan of the same predicate
  {
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64 run_start = os_now_microseconds();
      QE_ScanResult cpu = qe_cpu_scan_filter(run_scratch.arena, table, where, NULL);
      samples[run] = (F64)(os_now_microseconds() - run_start);
      (void)cpu;
      scratch_end(run_scratch);
    }
    sample.cpu_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  //- tec: index range fetch of the same tenth of the rows
  {
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      QE_ScanResult indexed = {0};
      U64 run_start = os_now_microseconds();
      B32 used = qe_try_index_scan(run_scratch.arena, table, where, &indexed);
      samples[run] = (F64)(os_now_microseconds() - run_start);
      (void)used;
      scratch_end(run_scratch);
    }
    sample.index_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  //- tec: a scan with no predicate at all, one GPU pass to list every row id
  {
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64 run_start = os_now_microseconds();
      QE_ScanResult bare = qe_scan_filter(run_scratch.arena, database, table, NULL, NULL);
      samples[run] = (F64)(os_now_microseconds() - run_start);
      (void)bare;
      scratch_end(run_scratch);
    }
    sample.gpu_bare_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  //- tec: the same CPU scan with three conditions, every row passes the last two
  {
    String8 three_sql = push_str8f(scratch.arena, "SELECT id FROM %.*s WHERE value < %llu AND id >= 0 AND value >= 0;", str8_varg(table->name), threshold);
    IR_Node* three_where = cal_parse_where(scratch.arena, three_sql);
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64 run_start = os_now_microseconds();
      QE_ScanResult cpu = qe_cpu_scan_filter(run_scratch.arena, table, three_where, NULL);
      samples[run] = (F64)(os_now_microseconds() - run_start);
      (void)cpu;
      scratch_end(run_scratch);
    }
    sample.cpu_three_leaves_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  //- tec: the same three conditions with the selective one last, so every row pays for all three
  {
    String8 reversed_sql = push_str8f(scratch.arena, "SELECT id FROM %.*s WHERE id >= 0 AND value >= 0 AND value < %llu;", str8_varg(table->name), threshold);
    IR_Node* reversed_where = cal_parse_where(scratch.arena, reversed_sql);
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64 run_start = os_now_microseconds();
      QE_ScanResult cpu = qe_cpu_scan_filter(run_scratch.arena, table, reversed_where, NULL);
      samples[run] = (F64)(os_now_microseconds() - run_start);
      (void)cpu;
      scratch_end(run_scratch);
    }
    sample.cpu_three_reversed_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  //- tec: the serial filter that runs on the rows an index narrowed down, here over every row
  {
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64* indices = push_array(run_scratch.arena, U64, Max(row_count, 1));
      for (U64 row = 0; row < row_count; row += 1)
      {
        indices[row] = row;
      }
      GDB_Table* tables[1] = { table };
      String8 aliases[1] = {0};
      U64* index_lists[1] = { indices };
      PLAN_RowSet rows = {0};
      rows.tables = tables;
      rows.aliases = aliases;
      rows.table_count = 1;
      rows.row_indices = index_lists;
      rows.count = row_count;
      
      U64 run_start = os_now_microseconds();
      PLAN_RowSet filtered = qe_filter_joined_rows(run_scratch.arena, &rows, where->first);
      samples[run] = (F64)(os_now_microseconds() - run_start);
      (void)filtered;
      scratch_end(run_scratch);
    }
    sample.filter_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  //- tec: filling every row id on the CPU, what a scan with no predicate would cost without a dispatch
  {
    F64 samples[CAL_WARM_RUNS] = {0};
    for (U64 run = 0; run < CAL_WARM_RUNS; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64 run_start = os_now_microseconds();
      U64* indices = push_array(run_scratch.arena, U64, Max(row_count, 1));
      for (U64 row = 0; row < row_count; row += 1)
      {
        indices[row] = row;
      }
      samples[run] = (F64)(os_now_microseconds() - run_start);
      scratch_end(run_scratch);
    }
    sample.fill_us = cal_median(samples, CAL_WARM_RUNS);
  }
  
  scratch_end(scratch);
  return sample;
}

//~ tec: hash join, probe rows all match exactly one build row so the output equals the probe side

typedef struct Cal_JoinSample Cal_JoinSample;
struct Cal_JoinSample
{
  U64 build_rows;
  U64 probe_rows;
  F64 join_us;
  U64 round_trips;
  F64 fused_us;
  U64 fused_round_trips;
};

internal GDB_Table*
cal_build_join_table(GDB_Database* database, char* prefix, U64 row_count, U64 key_modulus)
{
  Temp scratch = scratch_begin(0, 0);
  String8 name = push_str8f(scratch.arena, "%s_%llu", prefix, row_count);
  GDB_Table* table = gdb_table_alloc(name);
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("k"), GDB_ColumnType_U32));
  gdb_database_add_table(database, table);
  for (U64 row = 0; row < row_count; row += 1)
  {
    U32 key = (U32)(row % key_modulus);
    void* row_data[1] = { &key };
    gdb_table_add_row(table, row_data, NULL);
  }
  scratch_end(scratch);
  return table;
}

internal Cal_JoinSample
cal_measure_join(Arena* arena, GDB_Database* database, U64 build_rows, U64 probe_rows)
{
  Cal_JoinSample sample = {0};
  sample.build_rows = build_rows;
  sample.probe_rows = probe_rows;
  
  GDB_Table* build_table = cal_build_join_table(database, "jb", build_rows, build_rows);
  GDB_Table* probe_table = cal_build_join_table(database, "jp", probe_rows, build_rows);
  
  Temp scratch = scratch_begin(&arena, 1);
  String8 sql = push_str8f(scratch.arena, "SELECT p.k FROM %.*s p JOIN %.*s b ON p.k = b.k;", str8_varg(probe_table->name), str8_varg(build_table->name));
  SQL_TokenizeResult tokens = sql_tokenize_from_text(scratch.arena, sql);
  SQL_Node* ast = sql_parse(scratch.arena, tokens.tokens, tokens.count, sql);
  IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
  IR_Node* join_node = ir_node_find_child(ir_query->execution_nodes, IR_NodeType_Join);
  IR_Node* condition = join_node->last;
  
  U64* identity = push_array(scratch.arena, U64, Max(probe_rows, (U64)1));
  for (U64 row = 0; row < probe_rows; row += 1)
  {
    identity[row] = row;
  }
  GDB_Table* left_tables[1] = { probe_table };
  String8 left_aliases[1] = { str8_lit("p") };
  U64* left_indices[1] = { identity };
  PLAN_RowSet left = {0};
  left.tables = left_tables;
  left.aliases = left_aliases;
  left.table_count = 1;
  left.row_indices = left_indices;
  left.count = probe_rows;
  
  for (U64 mode = 0; mode < 2; mode += 1)
  {
    QE_JoinHints hints = {0};
    hints.fuse_round_trips = (B32)mode;
    F64 samples[CAL_WARM_RUNS] = {0};
    U64 trips = 0;
    for (U64 run = 0; run < CAL_WARM_RUNS + 1; run += 1)
    {
      Temp run_scratch = scratch_begin(&scratch.arena, 1);
      U64 trips_before = g_vulkan_state->submit_count;
      U64 run_start = os_now_microseconds();
      PLAN_RowSet joined = qe_hash_join(run_scratch.arena, &left, build_table, str8_lit("b"), NULL, 0, str8_lit("inner"), condition, &hints, NULL);
      F64 elapsed = (F64)(os_now_microseconds() - run_start);
      trips = g_vulkan_state->submit_count - trips_before;
      if (run == 1 && joined.count != probe_rows)
      {
        printf("  !! join returned %llu rows, expected %llu (fused=%llu)\n", joined.count, probe_rows, mode);
      }
      if (run > 0)
      {
        samples[run - 1] = elapsed;
      }
      scratch_end(run_scratch);
    }
    if (mode == 0)
    {
      sample.join_us = cal_median(samples, CAL_WARM_RUNS);
      sample.round_trips = trips;
    }
    else
    {
      sample.fused_us = cal_median(samples, CAL_WARM_RUNS);
      sample.fused_round_trips = trips;
    }
  }
  
  scratch_end(scratch);
  return sample;
}

#define CAL_OPERATOR_RUNS 7

typedef struct Cal_OperatorSample Cal_OperatorSample;
struct Cal_OperatorSample
{
  U64 rows;
  F64 sort_gpu_us;
  F64 sort_cpu_us;
  F64 top_10_us;
  F64 top_1000_us;
  F64 top_4096_us;
  F64 aggregate_gpu_us;
  F64 aggregate_cpu_us;
  F64 aggregate_fused_us;
};

// tec: g repeats every 100 rows so the aggregate has 100 groups, value is scrambled so the sort has real work to do
internal GDB_Table*
cal_build_operator_table(GDB_Database* database, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  String8 name = push_str8f(scratch.arena, "calop_%llu", row_count);
  GDB_Table* table = gdb_table_alloc(name);
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("g"), GDB_ColumnType_U32));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("value"), GDB_ColumnType_F64));
  gdb_database_add_table(database, table);
  
  for (U64 row = 0; row < row_count; row += 1)
  {
    U32 group = (U32)(row % 100);
    F64 value = (F64)((row * 2654435761ull) % 1000003ull);
    void* row_data[2] = { &group, &value };
    gdb_table_add_row(table, row_data, NULL);
  }
  scratch_end(scratch);
  return table;
}

internal PLAN_RowSet
cal_identity_rows(Arena* arena, GDB_Table* table, String8* alias_storage, GDB_Table** table_storage, U64** indices_storage)
{
  U64* identity = push_array(arena, U64, Max(table->row_count, (U64)1));
  for (U64 row = 0; row < table->row_count; row += 1)
  {
    identity[row] = row;
  }
  table_storage[0] = table;
  alias_storage[0] = str8_lit("t");
  indices_storage[0] = identity;
  
  PLAN_RowSet rows = {0};
  rows.tables = table_storage;
  rows.aliases = alias_storage;
  rows.table_count = 1;
  rows.row_indices = indices_storage;
  rows.count = table->row_count;
  return rows;
}

internal IR_Node*
cal_parse_select(Arena* arena, String8 sql_text)
{
  SQL_TokenizeResult tokens = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tokens.tokens, tokens.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  return ir_query->execution_nodes;
}

internal F64
cal_time_sort(Arena* arena, PLAN_RowSet* rows, IR_Node* order_by, QE_SortHints* hints)
{
  F64 samples[CAL_OPERATOR_RUNS] = {0};
  for (U64 run = 0; run < CAL_OPERATOR_RUNS + 1; run += 1)
  {
    Temp run_scratch = scratch_begin(&arena, 1);
    U64 start = os_now_microseconds();
    PLAN_RowSet sorted = qe_sort_rows(run_scratch.arena, rows, order_by, hints, NULL);
    F64 elapsed = (F64)(os_now_microseconds() - start);
    (void)sorted;
    if (run > 0)
    {
      samples[run - 1] = elapsed;
    }
    scratch_end(run_scratch);
  }
  return cal_median(samples, CAL_OPERATOR_RUNS);
}

internal F64
cal_time_aggregate(Arena* arena, GDB_Database* database, PLAN_RowSet* rows, IR_Node* group_by, IR_Node* column_list, QE_AggregateHints* hints)
{
  F64 samples[CAL_OPERATOR_RUNS] = {0};
  for (U64 run = 0; run < CAL_OPERATOR_RUNS + 1; run += 1)
  {
    Temp run_scratch = scratch_begin(&arena, 1);
    U64 start = os_now_microseconds();
    PLAN_Materialized result = qe_aggregate(run_scratch.arena, database, rows, group_by, column_list, NULL, hints, NULL);
    F64 elapsed = (F64)(os_now_microseconds() - start);
    (void)result;
    if (run > 0)
    {
      samples[run - 1] = elapsed;
    }
    scratch_end(run_scratch);
  }
  return cal_median(samples, CAL_OPERATOR_RUNS);
}

internal Cal_OperatorSample
cal_measure_operators(Arena* arena, GDB_Database* database, U64 row_count)
{
  Cal_OperatorSample sample = {0};
  sample.rows = row_count;
  
  GDB_Table* table = cal_build_operator_table(database, row_count);
  Temp scratch = scratch_begin(&arena, 1);
  
  String8 alias_storage[1] = {0};
  GDB_Table* table_storage[1] = {0};
  U64* indices_storage[1] = {0};
  PLAN_RowSet rows = cal_identity_rows(scratch.arena, table, alias_storage, table_storage, indices_storage);
  
  String8 sort_sql = push_str8f(scratch.arena, "SELECT g FROM %.*s ORDER BY value;", str8_varg(table->name));
  IR_Node* sort_select = cal_parse_select(scratch.arena, sort_sql);
  IR_Node* order_by = ir_node_find_child(sort_select, IR_NodeType_OrderBy);
  
  QE_SortHints gpu_sort = {0};
  QE_SortHints cpu_sort = {0};
  cpu_sort.gpu_min_rows = max_U64;
  sample.sort_gpu_us = cal_time_sort(scratch.arena, &rows, order_by, &gpu_sort);
  sample.sort_cpu_us = cal_time_sort(scratch.arena, &rows, order_by, &cpu_sort);
  
  U64 top_sizes[3] = { 10, 1000, 4096 };
  F64* top_results[3] = { &sample.top_10_us, &sample.top_1000_us, &sample.top_4096_us };
  for (U64 index = 0; index < 3; index += 1)
  {
    QE_SortHints top_hints = {0};
    top_hints.top_k = top_sizes[index];
    *top_results[index] = cal_time_sort(scratch.arena, &rows, order_by, &top_hints);
  }
  
  String8 aggregate_sql = push_str8f(scratch.arena, "SELECT g, SUM(value), COUNT(*) FROM %.*s GROUP BY g;", str8_varg(table->name));
  IR_Node* aggregate_select = cal_parse_select(scratch.arena, aggregate_sql);
  IR_Node* group_by = ir_node_find_child(aggregate_select, IR_NodeType_GroupBy);
  IR_Node* column_list = ir_node_find_child(aggregate_select, IR_NodeType_ColumnList);
  
  QE_AggregateHints gpu_aggregate = {0};
  QE_AggregateHints cpu_aggregate = {0};
  cpu_aggregate.cpu_max_rows = max_U64;
  sample.aggregate_gpu_us = cal_time_aggregate(scratch.arena, database, &rows, group_by, column_list, &gpu_aggregate);
  sample.aggregate_cpu_us = cal_time_aggregate(scratch.arena, database, &rows, group_by, column_list, &cpu_aggregate);
  QE_AggregateHints fused_aggregate = {0};
  fused_aggregate.fuse_round_trips = 1;
  sample.aggregate_fused_us = cal_time_aggregate(scratch.arena, database, &rows, group_by, column_list, &fused_aggregate);
  
  scratch_end(scratch);
  return sample;
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
  
  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql cost model calibration");
  
  GDB_Database* database = gdb_database_alloc(str8_lit("calibrate_db"));
  gdb_add_database(database);
  
  U64 sizes[CAL_ROW_SIZE_COUNT] = { 64, 512, 4096, 32768, 262144, 2097152 };
  Cal_Sample samples[CAL_ROW_SIZE_COUNT] = {0};
  
  printf("\nthread pool workers: %u\n", (U32)app_thread_pool()->worker_count);
  printf("\n%10s %12s %12s %12s %12s %12s %12s %12s %12s\n", "rows", "gpu cold us", "gpu warm us", "cpu us", "index us", "fill us", "gpu bare us", "cpu 3 leaf", "filter us");
  bench_report_section(report, "median microseconds per operation, predicate matches a tenth of the rows");
  for (U64 index = 0; index < CAL_ROW_SIZE_COUNT; index += 1)
  {
    samples[index] = cal_measure(arena, database, sizes[index]);
    Cal_Sample* sample = &samples[index];
    printf("%10llu %12.1f %12.1f %12.1f %12.1f %12.1f %12.1f %12.1f %12.1f\n", sample->rows, sample->gpu_cold_us, sample->gpu_warm_us, sample->cpu_us, sample->index_us,
           sample->fill_us, sample->gpu_bare_us, sample->cpu_three_leaves_us, sample->filter_us);
    bench_report_text(report, "rows=%llu gpu_cold=%.1f gpu_warm=%.1f cpu=%.1f index=%.1f fill=%.1f gpu_bare=%.1f cpu_three=%.1f filter=%.1f", sample->rows,
                      sample->gpu_cold_us, sample->gpu_warm_us, sample->cpu_us, sample->index_us, sample->fill_us, sample->gpu_bare_us, sample->cpu_three_leaves_us,
                      sample->filter_us);
  }
  
  //- tec: per-row slopes from the two largest sizes, where fixed costs no longer dominate
  Cal_Sample* lower = &samples[CAL_ROW_SIZE_COUNT - 2];
  Cal_Sample* upper = &samples[CAL_ROW_SIZE_COUNT - 1];
  F64 row_delta = (F64)(upper->rows - lower->rows);
  printf("\nthree conditions, selective one first vs last, us:\n");
  for (U64 index = 2; index < CAL_ROW_SIZE_COUNT; index += 1)
  {
    printf("  %10llu rows: first %10.1f  last %10.1f  (%.2fx)\n", samples[index].rows, samples[index].cpu_three_leaves_us, samples[index].cpu_three_reversed_us,
           samples[index].cpu_three_reversed_us / samples[index].cpu_three_leaves_us);
  }
  
  printf("\nper-row slope, us: gpu warm %.5f, gpu bare %.5f, cpu %.5f, cpu 3 leaf %.5f, filter %.5f, fill %.5f, index %.5f\n",
         (upper->gpu_warm_us - lower->gpu_warm_us) / row_delta,
         (upper->gpu_bare_us - lower->gpu_bare_us) / row_delta,
         (upper->cpu_us - lower->cpu_us) / row_delta,
         (upper->cpu_three_leaves_us - lower->cpu_three_leaves_us) / row_delta,
         (upper->filter_us - lower->filter_us) / row_delta,
         (upper->fill_us - lower->fill_us) / row_delta,
         (upper->index_us - lower->index_us) / row_delta);
  printf("fixed cost, us: gpu warm %.1f, cpu %.1f\n", samples[0].gpu_warm_us, samples[0].cpu_us);
  
  printf("\nhash join, every probe row matches one build row:\n");
  printf("%12s %12s %12s %8s %12s %8s\n", "build rows", "probe rows", "join us", "trips", "fused us", "trips");
  U64 join_shapes[][2] = { {16, 16}, {1000, 1000}, {1000, 100000}, {100000, 1000}, {100000, 100000}, {10000, 10000}, {1000000, 1000}, {1000, 1000000}, {500000, 500000} };
  for (U64 shape = 0; shape < ArrayCount(join_shapes); shape += 1)
  {
    Cal_JoinSample join_sample = cal_measure_join(arena, database, join_shapes[shape][0], join_shapes[shape][1]);
    printf("%12llu %12llu %12.1f %8llu %12.1f %8llu\n", join_sample.build_rows, join_sample.probe_rows, join_sample.join_us, join_sample.round_trips,
           join_sample.fused_us, join_sample.fused_round_trips);
    bench_report_text(report, "hash join build=%llu probe=%llu: %.1f us, %llu round trips", join_sample.build_rows, join_sample.probe_rows, join_sample.join_us, join_sample.round_trips);
  }
  
  printf("\nsort, aggregate and top-K, 100 groups:\n");
  printf("%10s %12s %12s %10s %10s %10s %12s %12s\n", "rows", "sort gpu", "sort cpu", "top 10", "top 1000", "top 4096", "agg gpu", "agg cpu");
  U64 operator_sizes[] = { 256, 2048, 16384, 131072, 1048576 };
  Cal_OperatorSample operator_samples[ArrayCount(operator_sizes)] = {0};
  for (U64 index = 0; index < ArrayCount(operator_sizes); index += 1)
  {
    Cal_OperatorSample* operator_sample = &operator_samples[index];
    *operator_sample = cal_measure_operators(arena, database, operator_sizes[index]);
    printf("%10llu %12.1f %12.1f %10.1f %10.1f %10.1f %12.1f %12.1f\n", operator_sample->rows, operator_sample->sort_gpu_us, operator_sample->sort_cpu_us,
           operator_sample->top_10_us, operator_sample->top_1000_us, operator_sample->top_4096_us, operator_sample->aggregate_gpu_us, operator_sample->aggregate_cpu_us);
    bench_report_text(report, "operators rows=%llu sort_gpu=%.1f sort_cpu=%.1f top10=%.1f top1000=%.1f top4096=%.1f agg_gpu=%.1f agg_cpu=%.1f", operator_sample->rows,
                      operator_sample->sort_gpu_us, operator_sample->sort_cpu_us, operator_sample->top_10_us, operator_sample->top_1000_us, operator_sample->top_4096_us,
                      operator_sample->aggregate_gpu_us, operator_sample->aggregate_cpu_us);
  }
  
  Cal_OperatorSample* op_lower = &operator_samples[ArrayCount(operator_sizes) - 2];
  Cal_OperatorSample* op_upper = &operator_samples[ArrayCount(operator_sizes) - 1];
  F64 op_delta = (F64)(op_upper->rows - op_lower->rows);
  F64 largest_rows = (F64)op_upper->rows;
  printf("\nsort cpu per row per log2(rows), us: %.5f\n", op_upper->sort_cpu_us / (largest_rows * log2(largest_rows)));
  printf("sort gpu: fixed %.1f us, per row slope %.5f us\n", operator_samples[0].sort_gpu_us, (op_upper->sort_gpu_us - op_lower->sort_gpu_us) / op_delta);
  printf("top-K per row, us: k=10 %.5f, k=1000 %.5f, k=4096 %.5f\n", op_upper->top_10_us / largest_rows, op_upper->top_1000_us / largest_rows, op_upper->top_4096_us / largest_rows);
  printf("aggregate gpu: fixed %.1f us, per row slope %.5f us\n", operator_samples[0].aggregate_gpu_us, (op_upper->aggregate_gpu_us - op_lower->aggregate_gpu_us) / op_delta);
  printf("aggregate cpu: fixed %.1f us, per row slope %.5f us\n", operator_samples[0].aggregate_cpu_us, (op_upper->aggregate_cpu_us - op_lower->aggregate_cpu_us) / op_delta);
  printf("aggregate gpu fused: fixed %.1f us, per row slope %.5f us\n", operator_samples[0].aggregate_fused_us, (op_upper->aggregate_fused_us - op_lower->aggregate_fused_us) / op_delta);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/calibrate_report.md"));
  
  arena_release(arena);
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
