#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE MB(4)

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

#define AF_GROUPS 10000
#define AF_REGIONS 50
#define AF_AMOUNT_RANGE 10000
#define AF_PRODUCTS 10000
#define AF_BIG_DIM_ROWS 100000
#define AF_KEY_MULTIPLIER 2654435761ULL

typedef struct AF_Case AF_Case;
struct AF_Case
{
  char* label;
  char* sql;
};

typedef struct AF_Run AF_Run;
struct AF_Run
{
  B32 supported;
  U64 rows;
  U64 checksum;
};

typedef struct AF_Tally AF_Tally;
struct AF_Tally
{
  U64 attempts;
  U64 errors;
  U64 correct;
  U64 wrong;
  U64 unreached;
  U64 poisoned;
};

internal void af_write_fact_csv(String8 path, U64 row_count);
internal void af_write_dim_csv(String8 path);
internal void af_write_dim2_csv(String8 path);
internal void af_load_u32_table(CQL_Database* database, char* name, char** column_names, U32 column_count, U64 row_count, U32 (*value)(U32 column, U64 row));
internal U32 af_scale_value(U32 column, U64 row);
internal U32 af_region_value(U32 column, U64 row);
internal U32 af_product_value(U32 column, U64 row);
internal U32 af_big_dim_value(U32 column, U64 row);
internal AF_Run af_run(CQL_Database* database, String8 sql);
internal void af_drop_caches(void);

global U64 g_af_scale_rows = 0;

//~ tec: data

internal void
af_write_fact_csv(String8 path, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  String8List lines = {0};
  Bench_Rng rng = {0xC0FFEE12345ULL};
  str8_list_pushf(scratch.arena, &lines, "id,group_key,group_name,value\n");
  for (U64 i = 0; i < row_count; i++)
  {
    U32 group = (U32)(i % AF_GROUPS);
    U64 value = bench_rng_next(&rng) % 100000ULL;
    str8_list_pushf(scratch.arena, &lines, "%llu,%u,g%05u,%llu\n", i + 1, group, group, value);
  }
  String8 content = str8_list_join(scratch.arena, &lines, 0);
  os_write_data_to_file_path(path, content);
  scratch_end(scratch);
}

internal void
af_write_dim_csv(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "dim_key,dim_name\n");
  for (U64 g = 0; g < AF_GROUPS; g++)
  {
    str8_list_pushf(scratch.arena, &lines, "%llu,dimname_g%05llu\n", g, g);
  }
  String8 content = str8_list_join(scratch.arena, &lines, 0);
  os_write_data_to_file_path(path, content);
  scratch_end(scratch);
}

internal void
af_write_dim2_csv(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "fk_name,weight\n");
  for (U64 g = 0; g < AF_GROUPS; g++)
  {
    for (U64 r = 0; r < 3; r++)
    {
      str8_list_pushf(scratch.arena, &lines, "g%05llu,%llu\n", g, r + 1);
    }
  }
  String8 content = str8_list_join(scratch.arena, &lines, 0);
  os_write_data_to_file_path(path, content);
  scratch_end(scratch);
}

// tec: the scale_events columns of bench_select_scale: id, region_id, amount, needle_key
internal U32
af_scale_value(U32 column, U64 row)
{
  U32 result = 0;
  switch (column)
  {
    case 0: { result = (U32)(row + 1); } break;
    case 1: { result = (U32)(row % AF_REGIONS); } break;
    case 2: { result = (U32)(row % AF_AMOUNT_RANGE); } break;
    case 3: { result = (U32)(row * AF_KEY_MULTIPLIER); } break;
  }
  return result;
}

internal U32
af_region_value(U32 column, U64 row)
{
  U32 result = (U32)row;
  if (column == 1)
  {
    result = (U32)(row / 10);
  }
  return result;
}

internal U32
af_product_value(U32 column, U64 row)
{
  U32 result = (U32)row;
  if (column == 1)
  {
    result = (U32)(row % 20);
  }
  return result;
}

// tec: half the keys match one scale_events row each, the other half match none
internal U32
af_big_dim_value(U32 column, U64 row)
{
  U32 result = (U32)(row % 100);
  if (column == 0)
  {
    U64 stride = g_af_scale_rows / AF_BIG_DIM_ROWS;
    U64 source_row = (row % 2 == 0) ? row * stride : g_af_scale_rows + row;
    result = (U32)(source_row * AF_KEY_MULTIPLIER);
  }
  return result;
}

internal void
af_load_u32_table(CQL_Database* database, char* name, char** column_names, U32 column_count, U64 row_count, U32 (*value)(U32 column, U64 row))
{
  CQL_Table* table = cql_table_alloc(str8_cstring(name));
  for (U32 c = 0; c < column_count; c++)
  {
    cql_table_add_column(table, cql_column_schema_create(str8_cstring(column_names[c]), CQL_ColumnType_U32));
  }
  cql_database_add_table(database, table);
  for (U32 c = 0; c < column_count; c++)
  {
    U32* data = (U32*)cql_column_alloc_fixed_rows(table->columns[c], row_count);
    if (data == 0)
    {
      log_error("could not allocate %llu rows for %s", row_count, name);
      return;
    }
    for (U64 row = 0; row < row_count; row++)
    {
      data[row] = value(c, row);
    }
  }
  table->row_count = row_count;
}

//~ tec: running

internal AF_Run
af_run(CQL_Database* database, String8 sql)
{
  AF_Run out = {0};
  Temp scratch = scratch_begin(0, 0);
  SQL_TokenizeResult tok = sql_tokenize_from_text(scratch.arena, sql);
  SQL_Node* ast = sql_parse(scratch.arena, tok.tokens, tok.count, sql);
  IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;
  U64 temp_mark = database->temp_table_count;
  PLAN_ExecResult result = plan_run_select(scratch.arena, database, select_node, NULL);
  out.supported = result.supported;
  if (result.supported)
  {
    out.checksum = bench_cql_consume_result(&result, select_node, &out.rows);
  }
  cql_database_release_temp_tables_from(database, temp_mark);
  scratch_end(scratch);
  return out;
}

// tec: every pooled buffer and resident chunk goes, so the next run allocates what it needs again
internal void
af_drop_caches(void)
{
  qe_resident_clear();
  gpu_pool_begin_epoch();
  gpu_vulkan_pool_evict(max_U64);
}

//~ tec: entry

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  // tec: an injected failure logs an error on purpose, and there are thousands of them
  log_set_level(LogLevel_Warn);
  
  // tec: a buffer smaller than the suballocation block is carved out of a block that is never freed, so later runs would not allocate it again. 0 gives every buffer memory of its own
  settings_load_from_string(str8_lit("GPU_VULKAN_MEM_BLOCK_SIZE:0"));
  String8List setting_list = cmd_line_strings(cmdline, str8_lit("setting"));
  for (String8Node* setting_node = setting_list.first; setting_node; setting_node = setting_node->next)
  {
    settings_load_from_string(setting_node->string);
  }
  
  U64 fact_rows = 2000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    fact_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  g_af_scale_rows = 4000000;
  if (cmd_line_has_argument(cmdline, str8_lit("scale_rows")))
  {
    g_af_scale_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("scale_rows")), 10);
  }
  U64 start_query = 0;
  U64 start_k = 1;
  if (cmd_line_has_argument(cmdline, str8_lit("start_query")))
  {
    start_query = u64_from_str8(cmd_line_string(cmdline, str8_lit("start_query")), 10);
  }
  if (cmd_line_has_argument(cmdline, str8_lit("start_k")))
  {
    start_k = u64_from_str8(cmd_line_string(cmdline, str8_lit("start_k")), 10);
  }
  String8 only = cmd_line_string(cmdline, str8_lit("only"));
  
  cql_init();
  gpu_init();
  
  Arena* arena = arena_alloc(.reserve_size = GB(4), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql device allocation fault injection");
  bench_report_text(report, "every device memory allocation attempt of a query, after all pooled buffers and resident chunks are dropped, is failed in turn. "
                    "fact: %llu rows, scale_events: %llu rows, GPU_MAX_BUFFER_SIZE %llu bytes, GPU_VULKAN_MEM_BLOCK_SIZE 0",
                    fact_rows, g_af_scale_rows, (U64)GPU_MAX_BUFFER_SIZE);
  
  //- tec: tables
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  af_write_fact_csv(str8_lit("bench_data/alloc_fault_fact.csv"), fact_rows);
  af_write_dim_csv(str8_lit("bench_data/alloc_fault_dim.csv"));
  af_write_dim2_csv(str8_lit("bench_data/alloc_fault_dim2.csv"));
  
  CQL_Database* database = cql_database_alloc(str8_lit("alloc_fault_db"));
  cql_add_database(database);
  cql_database_add_table(database, cql_table_import_csv_streaming(database, str8_lit("fact"), str8_lit("bench_data/alloc_fault_fact.csv")));
  cql_database_add_table(database, cql_table_import_csv_streaming(database, str8_lit("dim"), str8_lit("bench_data/alloc_fault_dim.csv")));
  cql_database_add_table(database, cql_table_import_csv_streaming(database, str8_lit("dim2"), str8_lit("bench_data/alloc_fault_dim2.csv")));
  
  char* scale_columns[] = { "id", "region_id", "amount", "needle_key" };
  char* region_columns[] = { "region_id", "zone" };
  char* product_columns[] = { "product_id", "category" };
  char* big_dim_columns[] = { "big_key", "weight" };
  af_load_u32_table(database, "scale_events", scale_columns, 4, g_af_scale_rows, af_scale_value);
  af_load_u32_table(database, "regions", region_columns, 2, AF_REGIONS, af_region_value);
  af_load_u32_table(database, "products", product_columns, 2, AF_PRODUCTS, af_product_value);
  af_load_u32_table(database, "big_dim", big_dim_columns, 2, AF_BIG_DIM_ROWS, af_big_dim_value);
  
  char needle_sql[160];
  U64 needle_row = g_af_scale_rows / 2;
  snprintf(needle_sql, sizeof(needle_sql), "SELECT COUNT(*) FROM scale_events WHERE needle_key = %u;", af_scale_value(3, needle_row));
  
  AF_Case cases[] =
  {
    // tec: the chunked-hash suite
    { "group by numeric key", "SELECT group_key, COUNT(*), SUM(value), AVG(value), MIN(value), MAX(value) FROM fact GROUP BY group_key;" },
    { "group by string key", "SELECT group_name, COUNT(*), SUM(value) FROM fact GROUP BY group_name;" },
    { "global aggregate", "SELECT COUNT(*), SUM(value), MIN(value), MAX(value) FROM fact;" },
    { "inner join", "SELECT dim.dim_name, fact.id, fact.value FROM dim JOIN fact ON dim.dim_key = fact.group_key;" },
    { "left join", "SELECT dim.dim_name, fact.id FROM dim LEFT JOIN fact ON dim.dim_key = fact.group_key;" },
    { "join + group by, dict key", "SELECT dim2.fk_name, COUNT(*), SUM(fact.value) FROM dim2 JOIN fact ON dim2.fk_name = fact.group_name GROUP BY dim2.fk_name;" },
    { "group by, hash-reduce shape", "SELECT group_key, COUNT(*), SUM(id), AVG(id), MIN(id), MAX(id) FROM fact GROUP BY group_key;" },
    // tec: scans, sort, top-N, window, string search and an approximate aggregate on the same table
    { "filter rows", "SELECT id, value FROM fact WHERE value > 99900;" },
    { "string contains", "SELECT COUNT(*) FROM fact WHERE group_name contains '77';" },
    { "sort", "SELECT id, value FROM fact WHERE group_key = 7 ORDER BY value, id;" },
    { "top-n", "SELECT id, value FROM fact ORDER BY value DESC, id LIMIT 100;" },
    { "window rank", "SELECT COUNT(*) FROM (SELECT id, ROW_NUMBER() OVER (PARTITION BY group_key ORDER BY value DESC, id) AS rn FROM fact WHERE value > 90000) AS t WHERE rn <= 3;" },
    { "approx count distinct", "SELECT APPROX_COUNT_DISTINCT(group_key) FROM fact;" },
    // tec: the select-scale suite, streamed through chunks because every column is larger than the buffer limit
    { "scale: count key", needle_sql },
    { "scale: sum, min, max", "SELECT SUM(amount), MIN(amount), MAX(amount) FROM scale_events;" },
    { "scale: filter count", "SELECT COUNT(*) FROM scale_events WHERE amount > 9990;" },
    { "scale: id range", "SELECT COUNT(*) FROM scale_events WHERE id > 1000 AND id <= 3000000;" },
    { "scale: group by region", "SELECT region_id, COUNT(*), SUM(amount) FROM scale_events GROUP BY region_id ORDER BY region_id;" },
    { "scale: group by amount", "SELECT amount, COUNT(*), SUM(region_id) FROM scale_events GROUP BY amount ORDER BY amount DESC;" },
    { "scale: group by key after filter", "SELECT COUNT(*) FROM (SELECT needle_key FROM scale_events WHERE amount > 9990 GROUP BY needle_key) AS d;" },
    { "scale: join 50 rows", "SELECT COUNT(*), SUM(scale_events.amount) FROM scale_events JOIN regions ON scale_events.region_id = regions.region_id WHERE regions.zone = 2;" },
    { "scale: join 10k rows", "SELECT products.category, COUNT(*) FROM scale_events JOIN products ON scale_events.amount = products.product_id GROUP BY products.category;" },
    { "scale: join big dim", "SELECT COUNT(*), SUM(big_dim.weight) FROM scale_events JOIN big_dim ON scale_events.needle_key = big_dim.big_key;" },
  };
  
  bench_report_section(report, "outcomes per query");
  bench_report_row_begin(report);
  bench_report_cellf(report, "query");
  bench_report_cellf(report, "allocations");
  bench_report_cellf(report, "error");
  bench_report_cellf(report, "correct");
  bench_report_cellf(report, "WRONG");
  bench_report_cellf(report, "unreached");
  bench_report_cellf(report, "POISONED");
  bench_report_table_header_end(report);
  
  AF_Tally total = {0};
  for (U64 q = start_query; q < ArrayCount(cases); q++)
  {
    String8 label = str8_cstring(cases[q].label);
    if (only.size > 0 && str8_find_needle(label, 0, only, 0) >= label.size)
    {
      continue;
    }
    String8 sql = str8_cstring(cases[q].sql);
    
    AF_Run reference = af_run(database, sql);
    if (!reference.supported)
    {
      printf("query %llu '%s': the uninjected run failed, skipped\n", q, cases[q].label);
      bench_report_warn(report, "%s: the uninjected run failed, skipped", cases[q].label);
      continue;
    }
    
    af_drop_caches();
    U64 count_start = gpu_memory_stats().alloc_calls;
    AF_Run cold = af_run(database, sql);
    U64 allocation_count = gpu_memory_stats().alloc_calls - count_start;
    if (!cold.supported || cold.checksum != reference.checksum || cold.rows != reference.rows)
    {
      printf("query %llu '%s': the cold uninjected run differs from the warm one\n", q, cases[q].label);
      bench_report_warn(report, "%s: the cold uninjected run differs from the warm one", cases[q].label);
    }
    printf("query %llu '%s': %llu rows, %llu allocation attempts from cold\n", q, cases[q].label, reference.rows, allocation_count);
    fflush(stdout);
    
    AF_Tally tally = {0};
    U64 first_k = (q == start_query) ? start_k : 1;
    for (U64 k = first_k; k <= allocation_count; k++)
    {
      printf("attempt query=%llu k=%llu\n", q, k);
      fflush(stdout);
      
      af_drop_caches();
      U64 target = gpu_memory_stats().alloc_calls + k;
      g_gpu_fault_alloc_at = target;
      AF_Run injected = af_run(database, sql);
      B32 fired = gpu_memory_stats().alloc_calls >= target;
      g_gpu_fault_alloc_at = 0;
      
      tally.attempts += 1;
      char* outcome = "";
      if (!fired)
      {
        tally.unreached += 1;
        outcome = "unreached";
      }
      else if (!injected.supported)
      {
        tally.errors += 1;
        outcome = "error";
      }
      else if (injected.checksum == reference.checksum && injected.rows == reference.rows)
      {
        tally.correct += 1;
        outcome = "correct";
      }
      else
      {
        tally.wrong += 1;
        outcome = "WRONG";
        printf("  WRONG: query %llu '%s' with attempt %llu failed returned %llu rows checksum %llu, reference %llu rows checksum %llu\n",
               q, cases[q].label, k, injected.rows, injected.checksum, reference.rows, reference.checksum);
        bench_report_warn(report, "WRONG: %s with attempt %llu failed returned %llu rows (reference %llu)", cases[q].label, k, injected.rows, reference.rows);
      }
      
      AF_Run after = af_run(database, sql);
      if (!after.supported || after.checksum != reference.checksum || after.rows != reference.rows)
      {
        tally.poisoned += 1;
        printf("  POISONED: query %llu '%s', the run after attempt %llu failed returned %llu rows (supported %d), reference %llu rows\n",
               q, cases[q].label, k, after.rows, after.supported, reference.rows);
        bench_report_warn(report, "POISONED: %s, the run after attempt %llu failed returned %llu rows (supported %d)", cases[q].label, k, after.rows, after.supported);
      }
      printf("  -> %s\n", outcome);
    }
    
    bench_report_row_begin(report);
    bench_report_cellf(report, "%s", cases[q].label);
    bench_report_cellf(report, "%llu", allocation_count);
    bench_report_cellf(report, "%llu", tally.errors);
    bench_report_cellf(report, "%llu", tally.correct);
    bench_report_cellf(report, "%llu", tally.wrong);
    bench_report_cellf(report, "%llu", tally.unreached);
    bench_report_cellf(report, "%llu", tally.poisoned);
    bench_report_row_end(report);
    printf("query %llu '%s': %llu attempts, %llu error, %llu correct, %llu WRONG, %llu unreached, %llu POISONED\n",
           q, cases[q].label, tally.attempts, tally.errors, tally.correct, tally.wrong, tally.unreached, tally.poisoned);
    fflush(stdout);
    
    total.attempts += tally.attempts;
    total.errors += tally.errors;
    total.correct += tally.correct;
    total.wrong += tally.wrong;
    total.unreached += tally.unreached;
    total.poisoned += tally.poisoned;
  }
  
  bench_report_text(report, "total: %llu injected failures, %llu error, %llu correct, %llu WRONG, %llu unreached, %llu POISONED",
                    total.attempts, total.errors, total.correct, total.wrong, total.unreached, total.poisoned);
  printf("\ntotal: %llu injected failures, %llu error, %llu correct, %llu WRONG, %llu unreached, %llu POISONED\n",
         total.attempts, total.errors, total.correct, total.wrong, total.unreached, total.poisoned);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/alloc_fault_report.md"));
  
  arena_release(arena);
  log_release();
  
  ProfEnd();
  ProfEndCapture();
  
  if (total.wrong > 0 || total.poisoned > 0)
  {
    exit(1);
  }
}
