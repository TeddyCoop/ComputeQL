// compute_ql vs sqlite vs duckdb: string predicates over a column that holds NULLs
// every benchmark table elsewhere has NULL free string columns, so the dictionary and bytecode string paths were never checked against SQL's three valued logic
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1

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

#define SNULL_NULL_EVERY 7
#define SNULL_INSERT_BATCH 500

typedef struct SNull_Row SNull_Row;
struct SNull_Row
{
  U32 id;
  String8 name;
  B32 name_is_null;
};

// tec: every SNULL_NULL_EVERY'th row has a NULL name. low cardinality picks from the word list, high cardinality gives nearly every row its own name
internal SNull_Row*
snull_generate_rows(Arena* arena, U64 row_count, B32 low_cardinality)
{
  SNull_Row* rows = push_array(arena, SNull_Row, row_count);
  Bench_Rng rng = {0x5E11CAFE5EEDULL};
  U64 word_count = ArrayCount(g_bench_words);
  for (U64 i = 0; i < row_count; i++)
  {
    rows[i].id = (U32)(i + 1);
    U64 word = bench_rng_next(&rng) % word_count;
    if (low_cardinality)
    {
      rows[i].name = str8_cstring(g_bench_words[word]);
    }
    else
    {
      rows[i].name = push_str8f(arena, "%s_%06llu", g_bench_words[word], bench_rng_next(&rng) % 1000000ULL);
    }
    rows[i].name_is_null = (i % SNULL_NULL_EVERY) == 3;
  }
  return rows;
}

internal void
snull_write_csv(String8 path, SNull_Row* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,name\n");
  for (U64 i = 0; i < row_count; i++)
  {
    // tec: an empty CSV field is a NULL
    if (rows[i].name_is_null)
    {
      str8_list_pushf(scratch.arena, &lines, "%u,\n", rows[i].id);
    }
    else
    {
      str8_list_pushf(scratch.arena, &lines, "%u,%.*s\n", rows[i].id, str8_varg(rows[i].name));
    }
  }
  String8 content = str8_list_join(scratch.arena, &lines, 0);
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if (!os_handle_match(file, os_handle_zero()))
  {
    os_file_write(file, r1u64(0, content.size), content.str);
    os_file_close(file);
  }
  scratch_end(scratch);
}

internal String8
snull_insert_sql(Arena* arena, SNull_Row* rows, U64 first, U64 last)
{
  String8List parts = {0};
  str8_list_pushf(arena, &parts, "INSERT INTO snull VALUES ");
  for (U64 i = first; i < last; i++)
  {
    if (i > first)
    {
      str8_list_pushf(arena, &parts, ",");
    }
    if (rows[i].name_is_null)
    {
      str8_list_pushf(arena, &parts, "(%u,NULL)", rows[i].id);
    }
    else
    {
      str8_list_pushf(arena, &parts, "(%u,'%.*s')", rows[i].id, str8_varg(rows[i].name));
    }
  }
  str8_list_pushf(arena, &parts, ";");
  return str8_list_join(arena, &parts, 0);
}

// tec: the harness hashes a NULL text cell from sqlite differently from the other two engines, so those rows cannot be compared against sqlite
internal B32
snull_skip_sqlite_compare(String8 label)
{
  return str8_match(label, str8_lit("name IS NULL"), 0);
}

internal B32
snull_is_known_gap(String8 label)
{
  return str8_match(label, str8_lit("KNOWN GAP"), StringMatchFlag_RightSideSloppy);
}

internal U64
snull_build_cases(Arena* arena, Bench_QueryCase* out_cases, String8 present_literal)
{
  U64 n = 0;

  char* labels[] =
  {
    "name = present",
    "name != present",
    "name = absent",
    "name != absent",
    "name != absent AND id > 100",
    "name != absent OR id = 1",
    "name IS NULL",
    "name IS NOT NULL",
    "name < present",
    "name >= present",
    "name contains 'a'",
  };
  char* predicates[] =
  {
    "name = '%.*s'",
    "name != '%.*s'",
    "name = 'zzznotinlist'",
    "name != 'zzznotinlist'",
    "name != 'zzznotinlist' AND id > 100",
    "name != 'zzznotinlist' OR id = 1",
    "name IS NULL",
    "name IS NOT NULL",
    "name < '%.*s'",
    "name >= '%.*s'",
    "name contains 'a'",
  };

  for (U64 i = 0; i < ArrayCount(labels); i++)
  {
    String8 predicate = push_str8f(arena, predicates[i], str8_varg(present_literal));
    out_cases[n].label = str8_cstring(labels[i]);
    out_cases[n].cql_sql = push_str8f(arena, "SELECT id, name FROM snull WHERE %.*s;", str8_varg(predicate));
    out_cases[n].sqlite_sql = out_cases[n].cql_sql;
    out_cases[n].duckdb_sql = out_cases[n].cql_sql;
    if (str8_match(str8_cstring(labels[i]), str8_lit("name contains 'a'"), 0))
    {
      out_cases[n].sqlite_sql = push_str8f(arena, "SELECT id, name FROM snull WHERE name LIKE '%%a%%';");
      out_cases[n].duckdb_sql = out_cases[n].sqlite_sql;
    }
    n += 1;
  }
  
  // tec: the bytecode compiler is also used by HAVING and join conditions, which skip the NULL aware CPU scan
  // KNOWN GAP cases need NULL aware aggregation (qe_aggregate refuses them), they are reported but not counted as failures
  char* extra_labels[] =
  {
    "KNOWN GAP: GROUP BY name",
    "KNOWN GAP: COUNT(name) per group",
    "KNOWN GAP: HAVING name != absent",
    "KNOWN GAP: HAVING name != present",
    "join ON residual name != absent",
    "join WHERE name != absent",
  };
  char* extra_sql[] =
  {
    "SELECT name, COUNT(*) FROM snull GROUP BY name;",
    "SELECT name, COUNT(name) FROM snull GROUP BY name;",
    "SELECT name, COUNT(*) FROM snull GROUP BY name HAVING name != 'zzznotinlist';",
    "SELECT name, COUNT(*) FROM snull GROUP BY name HAVING name != '%.*s';",
    "SELECT a.id FROM snull a JOIN snull b ON a.id = b.id AND a.name != 'zzznotinlist';",
    "SELECT a.id FROM snull a JOIN snull b ON a.id = b.id WHERE a.name != 'zzznotinlist';",
  };
  for (U64 i = 0; i < ArrayCount(extra_labels); i++)
  {
    out_cases[n].label = str8_cstring(extra_labels[i]);
    out_cases[n].cql_sql = push_str8f(arena, extra_sql[i], str8_varg(present_literal));
    out_cases[n].sqlite_sql = out_cases[n].cql_sql;
    out_cases[n].duckdb_sql = out_cases[n].cql_sql;
    n += 1;
  }
  return n;
}

internal void
snull_run_suite(Arena* arena, Bench_Report* report, U64 row_count, B32 low_cardinality, char* label)
{
  printf("\n########## string NULL suite: %s (%llu rows, %s) ##########\n", label, row_count, low_cardinality ? "low cardinality" : "high cardinality");
  bench_report_section(report, "string NULL suite: %s (%llu rows, %s, every %dth name is NULL)", label, row_count, low_cardinality ? "low cardinality" : "high cardinality", SNULL_NULL_EVERY);

  Temp scratch = scratch_begin(&arena, 1);
  SNull_Row* rows = snull_generate_rows(scratch.arena, row_count, low_cardinality);

  String8 present_literal = str8_lit("kilo");
  for (U64 i = 0; i < row_count; i++)
  {
    if (!rows[i].name_is_null)
    {
      present_literal = rows[i].name;
      break;
    }
  }

  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  String8 csv_path = push_str8f(scratch.arena, "bench_data/string_null_%s.csv", label);
  snull_write_csv(csv_path, rows, row_count);

  CQL_Database* database = cql_database_alloc(str8_lit("string_null_db"));
  cql_add_database(database);
  CQL_Table* table = cql_table_import_csv_streaming(database, str8_lit("snull"), csv_path);
  cql_database_add_table(database, table);

  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  sqlite3_exec(sqlite_db, "CREATE TABLE snull (id INTEGER, name TEXT);", NULL, NULL, NULL);

  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  duckdb_open(NULL, &duckdb_db);
  duckdb_connect(duckdb_db, &duckdb_conn);
  duckdb_result create_result;
  duckdb_query(duckdb_conn, "CREATE TABLE snull (id INTEGER, name VARCHAR);", &create_result);
  duckdb_destroy_result(&create_result);

  for (U64 first = 0; first < row_count; first += SNULL_INSERT_BATCH)
  {
    Temp batch_scratch = temp_begin(scratch.arena);
    U64 last = Min(first + SNULL_INSERT_BATCH, row_count);
    String8 sql = snull_insert_sql(batch_scratch.arena, rows, first, last);
    sqlite3_exec(sqlite_db, (char*)sql.str, NULL, NULL, NULL);
    duckdb_result insert_result;
    duckdb_query(duckdb_conn, (char*)sql.str, &insert_result);
    duckdb_destroy_result(&insert_result);
    temp_end(batch_scratch);
  }

  Bench_QueryCase cases[24];
  U64 case_count = snull_build_cases(scratch.arena, cases, present_literal);

  // tec: text written between table rows ends the table, so the known gap lines wait until it is complete
  String8List gap_notes = {0};
  bench_print_table_header(report, label);
  for (U64 i = 0; i < case_count; i++)
  {
    U64 cql_rows = 0, cql_checksum = 0;
    Bench_Stats cql_stats = bench_run_cql_query(database, cases[i].cql_sql, &cql_rows, &cql_checksum);
    bench_print_table_row(report, cases[i].label, "cql", cql_rows, cql_checksum, &cql_stats);

    U64 sqlite_rows = 0, sqlite_checksum = 0;
    Bench_Stats sqlite_stats = bench_run_sqlite_query(sqlite_db, cases[i].sqlite_sql, &sqlite_rows, &sqlite_checksum);
    bench_print_table_row(report, cases[i].label, "sqlite", sqlite_rows, sqlite_checksum, &sqlite_stats);

    U64 duckdb_rows = 0, duckdb_checksum = 0;
    Bench_Stats duckdb_stats = bench_run_duckdb_query(duckdb_conn, cases[i].duckdb_sql, &duckdb_rows, &duckdb_checksum);
    bench_print_table_row(report, cases[i].label, "duckdb", duckdb_rows, duckdb_checksum, &duckdb_stats);

    if (snull_is_known_gap(cases[i].label))
    {
      B32 agrees = (cql_rows == duckdb_rows && cql_checksum == duckdb_checksum);
      str8_list_pushf(scratch.arena, &gap_notes, "%.*s: cql %s duckdb (cql rows=%llu, duckdb rows=%llu)", str8_varg(cases[i].label), agrees ? "agrees with" : "differs from", cql_rows, duckdb_rows);
      continue;
    }
    if (!snull_skip_sqlite_compare(cases[i].label))
    {
      bench_check_match(report, cases[i].label, "cql", cql_rows, cql_checksum, "sqlite", sqlite_rows, sqlite_checksum);
    }
    bench_check_match(report, cases[i].label, "cql", cql_rows, cql_checksum, "duckdb", duckdb_rows, duckdb_checksum);
  }

  for (String8Node* note = gap_notes.first; note != 0; note = note->next)
  {
    bench_report_text(report, "%.*s", str8_varg(note->string));
  }

  sqlite3_close(sqlite_db);
  duckdb_disconnect(&duckdb_conn);
  duckdb_close(&duckdb_db);
  scratch_end(scratch);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();

  cql_init();
  gpu_init();

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));

  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite vs duckdb - string predicates over NULLs");

  snull_run_suite(arena, report, 2000, 1, "below_dict_threshold");
  snull_run_suite(arena, report, 20000, 1, "dict_eligible");
  snull_run_suite(arena, report, 20000, 0, "high_cardinality_no_dict");

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/string_null_report.md"));

  arena_release(arena);

  log_release();

  ProfEnd();
  ProfEndCapture();
}
