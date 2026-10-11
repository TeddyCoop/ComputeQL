#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(1)

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

//~ tec: the streamed scan against the CPU scan, GPU_MAX_BUFFER_SIZE is far below the column sizes so every scan is chunked

global U64 g_failed_checks = 0;

internal void
check(B32 condition, char* description)
{
  printf("  [%s] %s\n", condition ? "pass" : "FAIL", description);
  if (!condition)
  {
    g_failed_checks += 1;
  }
}

internal int
compare_u64(const void* a, const void* b)
{
  U64 left = *(const U64*)a;
  U64 right = *(const U64*)b;
  return left < right ? -1 : (left > right ? 1 : 0);
}

internal CQL_Table*
build_table(CQL_Database* database, U64 row_count)
{
  CQL_Table* table = cql_table_alloc(str8_lit("stream_t"));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("id"), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("value"), CQL_ColumnType_F64));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("region"), CQL_ColumnType_U32));
  cql_table_add_column(table, cql_column_schema_create(str8_lit("code"), CQL_ColumnType_I32));
  cql_database_add_table(database, table);
  for (U64 row = 0; row < row_count; row += 1)
  {
    U32 id = (U32)row;
    F64 value = (F64)((row * 2654435761ull) % 100003ull);
    U32 region = (U32)(row % 50);
    S32 code = (S32)((row * 7919ull) % 3000ull) - 1000;
    void* row_data[4] = { &id, &value, &region, &code };
    cql_table_add_row(table, row_data, NULL);
  }
  return table;
}

internal IR_Node*
parse_where(Arena* arena, CQL_Table* table, char* condition)
{
  String8 sql = push_str8f(arena, "SELECT id FROM %.*s WHERE %s;", str8_varg(table->name), condition);
  SQL_TokenizeResult tokens = sql_tokenize_from_text(arena, sql);
  SQL_Node* ast = sql_parse(arena, tokens.tokens, tokens.count, sql);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  return ir_node_find_child(ir_query->execution_nodes, IR_NodeType_Where);
}

internal void
test_condition(Arena* arena, CQL_Database* database, CQL_Table* table, char* condition)
{
  printf("WHERE %s\n", condition);
  Temp scratch = scratch_begin(&arena, 1);
  IR_Node* where = parse_where(scratch.arena, table, condition);

  QE_ScanResult expected = qe_cpu_scan_filter(scratch.arena, table, where, NULL);
  quick_sort(expected.indices, expected.count, sizeof(U64), compare_u64);

  QE_ScanTrace trace = {0};
  U64 start = os_now_microseconds();
  QE_ScanResult streamed = qe_scan_filter(scratch.arena, database, table, where, &trace);
  U64 streamed_us = os_now_microseconds() - start;
  quick_sort(streamed.indices, streamed.count, sizeof(U64), compare_u64);

  B32 same = streamed.count == expected.count;
  for (U64 index = 0; index < expected.count && same; index += 1)
  {
    same = streamed.indices[index] == expected.indices[index];
  }
  check(trace.strategy_reason.size >= 8 && str8_find_needle(trace.strategy_reason, 0, str8_lit("streamed"), 0) < trace.strategy_reason.size, "ran streamed");
  check(same, "row ids match the CPU scan");

  start = os_now_microseconds();
  settings_load_from_string(str8_lit("QE_SCAN_STREAM: 0"));
  QE_ScanResult fallback = qe_scan_filter(scratch.arena, database, table, where, NULL);
  U64 fallback_us = os_now_microseconds() - start;
  settings_load_from_string(str8_lit("QE_SCAN_STREAM: 1"));
  check(fallback.count == expected.count, "the chunked CPU fallback agrees");

  QE_DeviceSelection selection = {0};
  selection.count_only_requested = 1;
  QE_ScanResult counted = qe_scan_filter_selected(scratch.arena, database, table, where, NULL, &selection, 0);
  check(selection.count_only_done && selection.count == expected.count, "count only returns the same count");
  (void)counted;

  printf("  %llu rows, %llu chunks, streamed %.1f ms, chunked CPU %.1f ms\n", expected.count, trace.chunk_count, (F64)streamed_us / 1000.0, (F64)fallback_us / 1000.0);
  scratch_end(scratch);
}

internal void
test_residency(Arena* arena, CQL_Database* database, CQL_Table* table)
{
  printf("resident chunks\n");
  Temp scratch = scratch_begin(&arena, 1);
  IR_Node* where = parse_where(scratch.arena, table, "value < 5000");
  QE_ScanResult expected = qe_cpu_scan_filter(scratch.arena, table, where, NULL);

  qe_resident_clear();
  check(qe_resident_stats().bytes_in_use == 0, "clear leaves nothing resident");

  QE_ScanTrace cold = {0};
  QE_ScanResult cold_result = qe_scan_filter(scratch.arena, database, table, where, &cold);
  check(cold_result.count == expected.count, "cold scan is correct");
  check(cold.stream_resident_hit_bytes == 0 && cold.stream_resident_miss_bytes > 0, "cold scan copies and holds the column");

  QE_ScanTrace warm = {0};
  QE_ScanResult warm_result = qe_scan_filter(scratch.arena, database, table, where, &warm);
  check(warm_result.count == expected.count, "warm scan is correct");
  check(warm.stream_resident_miss_bytes == 0 && warm.stream_resident_hit_bytes == cold.stream_resident_miss_bytes, "warm scan copies nothing");
  printf("  cold copied %llu MB, warm hit %llu MB, %llu MB resident\n", cold.stream_resident_miss_bytes / MB(1), warm.stream_resident_hit_bytes / MB(1), qe_resident_stats().bytes_in_use / MB(1));

  // tec: a narrow column is held at its narrow size
  qe_resident_clear();
  IR_Node* region_where = parse_where(scratch.arena, table, "region = 7");
  QE_ScanTrace narrow = {0};
  qe_scan_filter(scratch.arena, database, table, region_where, &narrow);
  printf("  region (U32 with 50 values), %llu rows: %llu bytes copied and held\n", table->row_count, narrow.stream_resident_miss_bytes);
  check(narrow.stream_resident_miss_bytes < table->row_count * 2, "a column of 50 values is held at about a byte per row, not four");
  qe_resident_clear();
  IR_Node* code_where = parse_where(scratch.arena, table, "code < 500");
  QE_ScanTrace narrow_two = {0};
  qe_scan_filter(scratch.arena, database, table, code_where, &narrow_two);
  printf("  code (I32 with 3000 values): %llu bytes copied and held\n", narrow_two.stream_resident_miss_bytes);
  check(narrow_two.stream_resident_miss_bytes < table->row_count * 3 && narrow_two.stream_resident_miss_bytes >= table->row_count * 2, "a column of 3000 values is held at two bytes per row");
  qe_resident_clear();

  // tec: id is its row number, so a scan of it moves and holds nothing
  qe_resident_clear();
  IR_Node* id_where = parse_where(scratch.arena, table, "id >= 10 AND id < 5000000");
  QE_ScanTrace sequence_trace = {0};
  QE_ScanResult sequence_result = qe_scan_filter(scratch.arena, database, table, id_where, &sequence_trace);
  QE_ScanResult sequence_expected = qe_cpu_scan_filter(scratch.arena, table, id_where, NULL);
  check(sequence_result.count == sequence_expected.count && sequence_trace.stream_resident_miss_bytes == 0 && qe_resident_stats().bytes_in_use == 0, "a column that is its row number is not copied or held");
  qe_resident_clear();
  
  // tec: a write makes every chunk of the column stale
  U32 id = 4000000000u;
  F64 value = 1.0;
  U32 extra_region = 1;
  S32 extra_code = 1;
  void* row_data[4] = { &id, &value, &extra_region, &extra_code };
  cql_table_add_row(table, row_data, NULL);
  expected = qe_cpu_scan_filter(scratch.arena, table, where, NULL);
  QE_ScanTrace after_write = {0};
  QE_ScanResult after_write_result = qe_scan_filter(scratch.arena, database, table, where, &after_write);
  check(after_write_result.count == expected.count, "scan after a write sees the new row");
  check(after_write.stream_resident_hit_bytes == 0, "a write drops the resident chunks of its columns");

  // tec: a budget smaller than the column, the first chunks stay and the rest stream
  qe_resident_clear();
  settings_load_from_string(str8_lit("QE_RESIDENT_BUDGET_BYTES: 12582912"));
  QE_ScanTrace small_first = {0};
  qe_scan_filter(scratch.arena, database, table, where, &small_first);
  QE_ScanTrace small_second = {0};
  QE_ScanResult small_result = qe_scan_filter(scratch.arena, database, table, where, &small_second);
  check(small_result.count == expected.count, "scan with a small budget is correct");
  check(small_second.stream_resident_hit_bytes > 0 && small_second.stream_resident_miss_bytes > 0, "a column over the budget keeps part of itself resident");
  check(qe_resident_stats().bytes_in_use <= MB(12), "the budget holds");
  printf("  budget 12 MB: second scan hit %llu MB, copied %llu MB, %llu MB resident\n", small_second.stream_resident_hit_bytes / MB(1),
         small_second.stream_resident_miss_bytes / MB(1), qe_resident_stats().bytes_in_use / MB(1));
  settings_load_from_string(str8_lit("QE_RESIDENT_BUDGET_BYTES: 0"));
  qe_resident_clear();
  scratch_end(scratch);
}

internal void
test_group_by(Arena* arena, CQL_Database* database, U64 row_count)
{
  printf("GROUP BY with dense groups\n");
  struct { char* sql; char* key; B32 signed_key; U64 key_range; } cases[] =
  {
    { "SELECT region, COUNT(*), SUM(id), MIN(code), MAX(code), AVG(code) FROM stream_t GROUP BY region ORDER BY region;", "region", 0, 50 },
    { "SELECT code, COUNT(*), SUM(id), MIN(id), MAX(id) FROM stream_t GROUP BY code ORDER BY code;", "code", 1, 3000 },
  };
  for (U32 c = 0; c < ArrayCount(cases); c += 1)
  {
    U64 key_range = cases[c].key_range;
    U64* counts = push_array(arena, U64, key_range);
    F64* sum_id = push_array(arena, F64, key_range);
    F64* min_id = push_array(arena, F64, key_range);
    F64* max_id = push_array(arena, F64, key_range);
    F64* min_code = push_array(arena, F64, key_range);
    F64* max_code = push_array(arena, F64, key_range);
    F64* sum_code = push_array(arena, F64, key_range);
    for (U64 k = 0; k < key_range; k += 1)
    {
      min_id[k] = 1e300;
      max_id[k] = -1e300;
      min_code[k] = 1e300;
      max_code[k] = -1e300;
    }
    for (U64 row = 0; row < row_count; row += 1)
    {
      S32 code = (S32)((row * 7919ull) % 3000ull) - 1000;
      U64 k = c == 0 ? row % 50 : (U64)(code + 1000);
      counts[k] += 1;
      sum_id[k] += (F64)row;
      min_id[k] = Min(min_id[k], (F64)row);
      max_id[k] = Max(max_id[k], (F64)row);
      min_code[k] = Min(min_code[k], (F64)code);
      max_code[k] = Max(max_code[k], (F64)code);
      sum_code[k] += (F64)code;
    }

    printf("%s\n", cases[c].sql);
    for (U32 mode = 0; mode < 2; mode += 1)
    {
      settings_load_from_string(mode == 0 ? str8_lit("QE_AGG_STREAM: 1") : str8_lit("QE_AGG_STREAM: 0"));
      Temp scratch = scratch_begin(&arena, 1);
      APP_ResultSet result_set = {0};
      U64 start = os_now_microseconds();
      app_execute_query_capture(scratch.arena, str8_cstring(cases[c].sql), &database, &result_set);
      U64 elapsed = os_now_microseconds() - start;

      U64 present = 0;
      for (U64 k = 0; k < key_range; k += 1)
      {
        present += counts[k] != 0;
      }
      B32 same = result_set.valid && result_set.row_count == present;
      U64 group = 0;
      for (U64 k = 0; k < key_range && same; k += 1)
      {
        if (counts[k] == 0)
        {
          continue;
        }
        F64* cells = &result_set.cell_numeric[group * result_set.column_count];
        F64 want_key = c == 0 ? (F64)k : (F64)((S64)k - 1000);
        B32 row_same = cells[0] == want_key && cells[1] == (F64)counts[k] && cells[2] == sum_id[k];
        if (c == 0)
        {
          row_same = row_same && cells[3] == min_code[k] && cells[4] == max_code[k] && cells[5] == sum_code[k] / (F64)counts[k];
        }
        else
        {
          row_same = row_same && cells[3] == min_id[k] && cells[4] == max_id[k];
        }
        if (!row_same)
        {
          printf("  group %llu differs\n", k);
          same = 0;
        }
        group += 1;
      }
      check(same, mode == 0 ? "streamed group by matches the closed form" : "the old path matches the closed form");
      printf("  %s %.1f ms\n", mode == 0 ? "streamed" : "old path", (F64)elapsed / 1000.0);
      scratch_end(scratch);
    }
  }
  settings_load_from_string(str8_lit("QE_AGG_STREAM: 1"));
}

internal void
build_dimensions(CQL_Database* database)
{
  CQL_Table* dim = cql_table_alloc(str8_lit("dim_t"));
  cql_table_add_column(dim, cql_column_schema_create(str8_lit("k"), CQL_ColumnType_U32));
  cql_table_add_column(dim, cql_column_schema_create(str8_lit("zone"), CQL_ColumnType_U32));
  cql_database_add_table(database, dim);
  for (U32 k = 0; k < 50; k += 1)
  {
    U32 zone = k / 10;
    void* row_data[2] = { &k, &zone };
    cql_table_add_row(dim, row_data, NULL);
  }

  // tec: keys spread over the whole id range, some of them past its end. the fact side key (id) has no small range, so these joins are probed
  CQL_Table* sparse = cql_table_alloc(str8_lit("sparse_t"));
  cql_table_add_column(sparse, cql_column_schema_create(str8_lit("k"), CQL_ColumnType_U32));
  cql_table_add_column(sparse, cql_column_schema_create(str8_lit("w"), CQL_ColumnType_U32));
  cql_table_add_column(sparse, cql_column_schema_create(str8_lit("g"), CQL_ColumnType_U32));
  cql_database_add_table(database, sparse);
  for (U32 i = 0; i < 7000; i += 1)
  {
    U32 k = i * 997;
    U32 w = i % 13 + 1;
    U32 g = i % 7;
    void* row_data[3] = { &k, &w, &g };
    cql_table_add_row(sparse, row_data, NULL);
  }

  // tec: keys 0..24, each twice, with a weight that depends on the key
  CQL_Table* dup = cql_table_alloc(str8_lit("dup_t"));
  cql_table_add_column(dup, cql_column_schema_create(str8_lit("k"), CQL_ColumnType_U32));
  cql_table_add_column(dup, cql_column_schema_create(str8_lit("w"), CQL_ColumnType_U32));
  cql_database_add_table(database, dup);
  for (U32 copy = 0; copy < 2; copy += 1)
  {
    for (U32 k = 0; k < 25; k += 1)
    {
      U32 weight = k % 7 + 1;
      void* row_data[2] = { &k, &weight };
      cql_table_add_row(dup, row_data, NULL);
    }
  }
}

internal void
run_join_query(Arena* arena, CQL_Database* database, char* sql, F64* expected, U64 expected_cells, U64 expected_rows, char* description)
{
  printf("%s\n", sql);
  for (U32 mode = 0; mode < 2; mode += 1)
  {
    settings_load_from_string(mode == 0 ? str8_lit("QE_JOIN_PUSHDOWN: 1") : str8_lit("QE_JOIN_PUSHDOWN: 0"));
    Temp scratch = scratch_begin(&arena, 1);
    APP_ResultSet result_set = {0};
    U64 start = os_now_microseconds();
    app_execute_query_capture(scratch.arena, str8_cstring(sql), &database, &result_set);
    U64 elapsed = os_now_microseconds() - start;
    B32 same = result_set.valid && result_set.row_count == expected_rows && result_set.row_count * result_set.column_count == expected_cells;
    for (U64 cell = 0; cell < expected_cells && same; cell += 1)
    {
      same = result_set.cell_numeric[cell] == expected[cell];
      if (!same)
      {
        printf("  cell %llu: got %.17g, expected %.17g\n", cell, result_set.cell_numeric[cell], expected[cell]);
      }
    }
    char label[160];
    snprintf(label, sizeof(label), "%s %s", mode == 0 ? "pushdown:" : "general join:", description);
    check(same, label);
    printf("  %.1f ms\n", (F64)elapsed / 1000.0);
    scratch_end(scratch);
  }
  settings_load_from_string(str8_lit("QE_JOIN_PUSHDOWN: 1"));
}

internal void
test_join(Arena* arena, CQL_Database* database, U64 row_count)
{
  printf("aggregates over a join\n");
  build_dimensions(database);

  F64 zone_count[5] = {0};
  F64 zone_sum_code[5] = {0};
  F64 zone_min_id[5] = {1e300, 1e300, 1e300, 1e300, 1e300};
  F64 zone_max_id[5] = {-1e300, -1e300, -1e300, -1e300, -1e300};
  F64 zone2_sum_id = 0.0;
  F64 dup_count = 0.0;
  F64 dup_sum_w = 0.0;
  for (U64 row = 0; row < row_count; row += 1)
  {
    U64 region = row % 50;
    U64 zone = region / 10;
    S32 code = (S32)((row * 7919ull) % 3000ull) - 1000;
    zone_count[zone] += 1.0;
    zone_sum_code[zone] += (F64)code;
    zone_min_id[zone] = Min(zone_min_id[zone], (F64)row);
    zone_max_id[zone] = Max(zone_max_id[zone], (F64)row);
    if (zone == 2)
    {
      zone2_sum_id += (F64)row;
    }
    if (region < 25)
    {
      dup_count += 2.0;
      dup_sum_w += 2.0 * (F64)(region % 7 + 1);
    }
  }

  F64 filtered[2] = { zone_count[2], zone2_sum_id };
  run_join_query(arena, database, "SELECT COUNT(*), SUM(stream_t.id) FROM stream_t JOIN dim_t ON stream_t.region = dim_t.k WHERE dim_t.zone = 2;", filtered, 2, 1, "filtered dim");

  // tec: ordered by zone, so the rows can be compared in order
  F64 grouped[5 * 5];
  for (U32 zone = 0; zone < 5; zone += 1)
  {
    grouped[zone * 5 + 0] = (F64)zone;
    grouped[zone * 5 + 1] = zone_count[zone];
    grouped[zone * 5 + 2] = zone_sum_code[zone];
    grouped[zone * 5 + 3] = zone_min_id[zone];
    grouped[zone * 5 + 4] = zone_max_id[zone];
  }
  run_join_query(arena, database,
                 "SELECT dim_t.zone, COUNT(*), SUM(stream_t.code), MIN(stream_t.id), MAX(stream_t.id) FROM stream_t JOIN dim_t ON stream_t.region = dim_t.k GROUP BY dim_t.zone ORDER BY dim_t.zone;",
                 grouped, 25, 5, "group by a dim column");

  // tec: the probed join, against closed forms
  F64 probe_count = 0.0;
  F64 probe_sum_w = 0.0;
  F64 probe_sum_region = 0.0;
  F64 probe_min_code = 1e300;
  F64 probe_max_code = -1e300;
  F64 group_count[7] = {0};
  F64 group_sum_region[7] = {0};
  for (U32 i = 0; i < 7000; i += 1)
  {
    U64 k = (U64)i * 997;
    if (k >= row_count)
    {
      continue;
    }
    S32 code = (S32)((k * 7919ull) % 3000ull) - 1000;
    probe_count += 1.0;
    probe_sum_w += (F64)(i % 13 + 1);
    probe_sum_region += (F64)(k % 50);
    probe_min_code = Min(probe_min_code, (F64)code);
    probe_max_code = Max(probe_max_code, (F64)code);
    group_count[i % 7] += 1.0;
    group_sum_region[i % 7] += (F64)(k % 50);
  }
  F64 probed[5] = { probe_count, probe_sum_w, probe_sum_region, probe_min_code, probe_max_code };
  run_join_query(arena, database,
                 "SELECT COUNT(*), SUM(sparse_t.w), SUM(stream_t.region), MIN(stream_t.code), MAX(stream_t.code) FROM stream_t JOIN sparse_t ON stream_t.id = sparse_t.k;",
                 probed, 5, 1, "hash probe, aggregates of both sides");
  F64 probed_groups[21];
  for (U32 g = 0; g < 7; g += 1)
  {
    probed_groups[g * 3 + 0] = (F64)g;
    probed_groups[g * 3 + 1] = group_count[g];
    probed_groups[g * 3 + 2] = group_sum_region[g];
  }
  run_join_query(arena, database,
                 "SELECT sparse_t.g, COUNT(*), SUM(stream_t.region) FROM stream_t JOIN sparse_t ON stream_t.id = sparse_t.k GROUP BY sparse_t.g ORDER BY sparse_t.g;",
                 probed_groups, 21, 7, "hash probe, group by a dim column");

  F64 duplicated[2] = { dup_count, dup_sum_w };
  run_join_query(arena, database, "SELECT COUNT(*), SUM(dup_t.w) FROM stream_t JOIN dup_t ON stream_t.region = dup_t.k;", duplicated, 2, 1, "repeated dim keys, aggregate of a dim column");
}

internal void
test_aggregate(Arena* arena, CQL_Database* database, U64 row_count)
{
  printf("SELECT SUM, MIN, MAX, COUNT over both columns\n");
  F64 expected[8] = {0};
  expected[1] = 1e300;
  expected[2] = -1e300;
  expected[5] = 1e300;
  expected[6] = -1e300;
  for (U64 row = 0; row < row_count; row += 1)
  {
    F64 id = (F64)row;
    F64 value = (F64)((row * 2654435761ull) % 100003ull);
    expected[0] += id;
    expected[1] = Min(expected[1], id);
    expected[2] = Max(expected[2], id);
    expected[4] += value;
    expected[5] = Min(expected[5], value);
    expected[6] = Max(expected[6], value);
  }

  char* sql = "SELECT SUM(id), MIN(id), MAX(id), COUNT(*), SUM(value), MIN(value), MAX(value) FROM stream_t;";
  for (U32 mode = 0; mode < 2; mode += 1)
  {
    settings_load_from_string(mode == 0 ? str8_lit("QE_AGG_STREAM: 1") : str8_lit("QE_AGG_STREAM: 0"));
    Temp scratch = scratch_begin(&arena, 1);
    APP_ResultSet result_set = {0};
    U64 start = os_now_microseconds();
    app_execute_query_capture(scratch.arena, str8_cstring(sql), &database, &result_set);
    U64 elapsed = os_now_microseconds() - start;
    B32 same = result_set.valid && result_set.row_count == 1 && result_set.column_count == 7;
    for (U64 column = 0; column < 7 && same; column += 1)
    {
      F64 got = result_set.cell_numeric[column];
      F64 want = column == 3 ? (F64)row_count : expected[column];
      same = (got == want);
      if (!same)
      {
        printf("  column %llu: got %.17g, expected %.17g\n", column, got, want);
      }
    }
    check(same, mode == 0 ? "streamed aggregate matches the closed form" : "chunked CPU aggregate matches the closed form");
    printf("  %s %.1f ms\n", mode == 0 ? "streamed" : "chunked CPU", (F64)elapsed / 1000.0);
    scratch_end(scratch);
  }
  settings_load_from_string(str8_lit("QE_AGG_STREAM: 1"));
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  setvbuf(stdout, NULL, _IONBF, 0);
  log_alloc();
  g_query_exec_mutex = os_mutex_alloc();

  if (cmd_line_has_flag(cmdline, str8_lit("no_copy_queue")))
  {
    settings_load_from_string(str8_lit("GPU_VULKAN_COPY_QUEUE: 0"));
  }
  settings_load_from_string(str8_lit("GPU_MAX_BUFFER_SIZE: 4194304"));
  settings_load_from_string(str8_lit("QE_SCAN_STREAM_CHUNK_ROWS: 262144"));
  settings_load_from_string(str8_lit("QE_SCAN_STREAM_SLOTS: 3"));

  cql_init();
  gpu_init();
  if (cmd_line_has_flag(cmdline, str8_lit("no_import")))
  {
    g_vulkan_state->has_external_memory_host_ext = 0;
  }
  printf("copy queue %s, host import %s\n", g_vulkan_state->copy_queue_separate ? "separate" : "same as compute", g_vulkan_state->has_external_memory_host_ext ? "on" : "off");

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  CQL_Database* database = cql_database_alloc(str8_lit("stream_db"));
  cql_add_database(database);

  U64 row_count = 6000000;
  CQL_Table* table = build_table(database, row_count);
  printf("table %llu rows\n", row_count);

  test_condition(arena, database, table, "value < 100");
  test_condition(arena, database, table, "id >= 3000000 AND value < 50000");
  test_condition(arena, database, table, "id = 4567891");
  test_condition(arena, database, table, "value < 0");
  test_condition(arena, database, table, "id < 100 OR value < 20");
  // tec: region is held one byte per row and code two, the comparisons are made on the codes plus the base
  test_condition(arena, database, table, "region = 7");
  test_condition(arena, database, table, "code < 500 AND region >= 20");
  test_condition(arena, database, table, "code >= 1500");
  test_condition(arena, database, table, "code = -1000 AND region < 50");
  // tec: the whole number form of a comparison, targets outside the column's range and between whole numbers
  test_condition(arena, database, table, "code < -1500");
  test_condition(arena, database, table, "code > 5000");
  test_condition(arena, database, table, "code >= -1000.5 AND code <= -998.5");
  test_condition(arena, database, table, "region = 7.5");
  test_condition(arena, database, table, "region < 7.2");
  test_condition(arena, database, table, "region > 48.1");
  test_condition(arena, database, table, "id >= 4000000 AND id < 4000010");
  test_condition(arena, database, table, "id > 5999990");

  test_aggregate(arena, database, row_count);
  test_group_by(arena, database, row_count);
  test_join(arena, database, row_count);
  test_residency(arena, database, table);
  
  printf("%s\n", g_failed_checks == 0 ? "all checks passed" : "CHECKS FAILED");
  log_release();
  ProfEndCapture();
  if (g_failed_checks != 0)
  {
    os_abort(1);
  }
}
