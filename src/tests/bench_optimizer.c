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
#include "planner/planner.c"
#include "application.c"

#include "tests/bench_common.h"

#define OPT_CASE_MAX 64

typedef struct Opt_Case Opt_Case;
struct Opt_Case
{
  String8 label;
  String8 sql;
  B32 known_gap;
};

typedef struct Opt_Fixture Opt_Fixture;
struct Opt_Fixture
{
  GDB_Database* database;
  sqlite3* sqlite_db;
  duckdb_database duckdb_db;
  duckdb_connection duckdb_conn;
};

internal Bench_Row*
opt_generate_rows(Arena* arena, U64 row_count, U64 seed)
{
  Bench_Row* rows = push_array(arena, Bench_Row, row_count);
  Bench_Rng rng = {seed};
  U64 word_count = ArrayCount(g_bench_words);
  for (U64 i = 0; i < row_count; i += 1)
  {
    rows[i].id = (U32)(i + 1);
    U64 word_index = bench_rng_next(&rng) % word_count;
    rows[i].name = str8_cstring(g_bench_words[word_index]);
    U64 raw_value = bench_rng_next(&rng) % 10000000ULL;
    rows[i].value = (F64)raw_value / 100.0;
  }
  return rows;
}

// tec: one row per word so 'name' is unique, which makes it a dimension key
internal Bench_Row*
opt_generate_dimension_rows(Arena* arena, U64 row_count, U64 seed)
{
  Bench_Row* rows = push_array(arena, Bench_Row, row_count);
  Bench_Rng rng = {seed};
  U64 word_count = ArrayCount(g_bench_words);
  for (U64 i = 0; i < row_count; i += 1)
  {
    rows[i].id = (U32)(i + 1);
    rows[i].name = str8_cstring(g_bench_words[i % word_count]);
    U64 raw_value = bench_rng_next(&rng) % 10000000ULL;
    rows[i].value = (F64)raw_value / 100.0;
  }
  return rows;
}

internal void
opt_fixture_add_table(Arena* arena, Opt_Fixture* fixture, char* table_name_cstr, Bench_Row* rows, U64 row_count)
{
  String8 table_name = str8_cstring(table_name_cstr);
  String8 csv_path = push_str8f(arena, "bench_data/opt_%s.csv", table_name_cstr);
  bench_write_csv(csv_path, rows, row_count);

  GDB_Table* table = gdb_table_import_csv_streaming(fixture->database, table_name, csv_path);
  gdb_database_add_table(fixture->database, table);

  bench_sqlite_create_schema(fixture->sqlite_db, table_name);
  bench_sqlite_bulk_insert(fixture->sqlite_db, table_name, rows, row_count);

  bench_duckdb_create_schema(fixture->duckdb_conn, table_name);
  bench_duckdb_bulk_insert(fixture->duckdb_conn, table_name, rows, row_count);
}

// tec: 'big' and 'mid' share ids 1..N so id joins are 1:1 on the smaller side, 'small' and 'tiny' are dimensions
internal Opt_Fixture*
opt_fixture_create(Arena* arena)
{
  Temp scratch = scratch_begin(&arena, 1);

  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }

  Opt_Fixture* fixture = push_array(arena, Opt_Fixture, 1);
  fixture->database = gdb_database_alloc(str8_lit("optimizer_db"));
  gdb_add_database(fixture->database);

  sqlite3_open(":memory:", &fixture->sqlite_db);
  duckdb_open(NULL, &fixture->duckdb_db);
  duckdb_connect(fixture->duckdb_db, &fixture->duckdb_conn);

  U64 big_count = 30000;
  U64 mid_count = 3000;
  U64 small_count = 16;
  U64 tiny_count = 5;

  Bench_Row* big_rows = opt_generate_rows(scratch.arena, big_count, 0xB16B00B5ULL);
  Bench_Row* mid_rows = opt_generate_rows(scratch.arena, mid_count, 0x31D31D31ULL);
  Bench_Row* small_rows = opt_generate_dimension_rows(scratch.arena, small_count, 0x5A11ULL);
  Bench_Row* tiny_rows = opt_generate_dimension_rows(scratch.arena, tiny_count, 0x717ULL);

  opt_fixture_add_table(scratch.arena, fixture, "big", big_rows, big_count);
  opt_fixture_add_table(scratch.arena, fixture, "mid", mid_rows, mid_count);
  opt_fixture_add_table(scratch.arena, fixture, "small", small_rows, small_count);
  opt_fixture_add_table(scratch.arena, fixture, "tiny", tiny_rows, tiny_count);

  scratch_end(scratch);
  return fixture;
}

internal void
opt_fixture_destroy(Opt_Fixture* fixture)
{
  sqlite3_close(fixture->sqlite_db);
  duckdb_disconnect(&fixture->duckdb_conn);
  duckdb_close(&fixture->duckdb_db);
}

internal void
opt_add_case(Opt_Case* cases, U64* count, char* label, char* sql, B32 known_gap)
{
  cases[*count].label = str8_cstring(label);
  cases[*count].sql = str8_cstring(sql);
  cases[*count].known_gap = known_gap;
  *count += 1;
}

//~ tec: round trip counting

// tec: the first run primes kernel caches and pooled buffers, so the counted run is steady state
internal U64
opt_count_round_trips(GDB_Database* database, String8 sql_text)
{
  U64 round_trips = 0;

  for (U64 run = 0; run < 2; run += 1)
  {
    Temp scratch = scratch_begin(0, 0);
    Arena* arena = scratch.arena;

    SQL_TokenizeResult tokens = sql_tokenize_from_text(arena, sql_text);
    SQL_Node* ast = sql_parse(arena, tokens.tokens, tokens.count, sql_text);
    IR_Query* ir_query = ir_generate_from_ast(arena, ast);
    IR_Node* select_node = ir_query->execution_nodes;

    U64 temp_mark = database->temp_table_count;
    U64 submits_before = g_gpu_vulkan_submit_count;
    PLAN_ExecResult result = plan_run_select(arena, database, select_node, NULL);
    U64 row_count = 0;
    bench_gdb_consume_result(&result, select_node, &row_count);
    U64 submits_after = g_gpu_vulkan_submit_count;
    gdb_database_release_temp_tables_from(database, temp_mark);

    round_trips = submits_after - submits_before;
    scratch_end(scratch);
  }

  return round_trips;
}

//~ tec: result parity

internal void
opt_run_parity_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  printf("\n########## optimizer baseline: result parity ##########\n");
  bench_report_section(report, "optimizer baseline: result parity");

  Opt_Case cases[OPT_CASE_MAX];
  U64 count = 0;

  //- tec: two-table joins
  opt_add_case(cases, &count, "join: filter on dimension",
               "SELECT b.id, s.name FROM big b JOIN small s ON b.name = s.name WHERE s.id < 4;", 0);
  opt_add_case(cases, &count, "join: count over fk join",
               "SELECT COUNT(*) AS n FROM big b JOIN small s ON b.name = s.name;", 0);
  opt_add_case(cases, &count, "join: group by (handoff shape)",
               "SELECT s.name, COUNT(*) AS c, SUM(b.value) AS total FROM small s JOIN big b ON s.name = b.name GROUP BY s.name;", 0);
  opt_add_case(cases, &count, "join: big probes small build",
               "SELECT b.id, m.value FROM big b JOIN mid m ON b.id = m.id WHERE m.value > 90000;", 0);
  opt_add_case(cases, &count, "join: small written first",
               "SELECT m.id, b.value FROM mid m JOIN big b ON m.id = b.id WHERE b.value > 90000;", 0);

  //- tec: three and four table joins, textual order chosen to be bad
  opt_add_case(cases, &count, "chain: explicit joins",
               "SELECT b.id, m.value, s.name FROM big b JOIN mid m ON b.id = m.id JOIN small s ON m.id = s.id;", 0);
  opt_add_case(cases, &count, "comma join: two tables",
               "SELECT b.id, s.name FROM big b, small s WHERE b.name = s.name AND s.id < 4;", 0);
  // tec: only the outermost cross join sees the WHERE, so the inner one finds no equi predicate
  opt_add_case(cases, &count, "chain: comma joins, predicates in where",
               "SELECT b.id, m.value FROM big b, mid m, small s WHERE b.id = m.id AND m.id = s.id AND s.id < 10;", 1);
  opt_add_case(cases, &count, "chain: comma joins, constant on join column",
               "SELECT b.id FROM big b, mid m, small s WHERE b.id = m.id AND m.id = s.id AND b.id = 5;", 1);
  opt_add_case(cases, &count, "star: filters on two dimensions",
               "SELECT COUNT(*) AS n FROM big b JOIN mid m ON b.id = m.id JOIN small s ON b.name = s.name WHERE m.value > 50000 AND s.id < 8;", 0);
  opt_add_case(cases, &count, "four tables",
               "SELECT b.id, t.name FROM big b JOIN mid m ON b.id = m.id JOIN small s ON m.id = s.id JOIN tiny t ON s.id = t.id;", 0);
  opt_add_case(cases, &count, "four tables: aggregate",
               "SELECT t.name, COUNT(*) AS n FROM big b JOIN mid m ON b.id = m.id JOIN small s ON m.id = s.id JOIN tiny t ON s.id = t.id GROUP BY t.name;", 0);

  //- tec: left joins are ordering barriers
  // tec: the checksum harness hashes NULL cells differently per engine, so left join cases return left-side columns only
  opt_add_case(cases, &count, "left join: basic",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id WHERE m.id < 40;", 0);
  opt_add_case(cases, &count, "left join: null-rejecting where on right side",
               "SELECT m.id, s.name FROM mid m LEFT JOIN small s ON m.id = s.id WHERE s.id > 3;", 0);
  opt_add_case(cases, &count, "left join: is null on right side",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id WHERE s.id IS NULL AND m.id < 40;", 0);
  opt_add_case(cases, &count, "left join then inner join",
               "SELECT m.id, s.name, t.name FROM mid m LEFT JOIN small s ON m.id = s.id JOIN tiny t ON m.id = t.id;", 0);

  //- tec: ON clauses with more than one conjunct
  opt_add_case(cases, &count, "on: two equi keys",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id AND b.name = m.name;", 1);
  opt_add_case(cases, &count, "on: equi key plus range on right",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id AND m.value > 50000;", 1);
  opt_add_case(cases, &count, "on: equi key plus range on left",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id AND b.value > 50000;", 1);
  opt_add_case(cases, &count, "on: left join with extra conjunct",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id AND s.id < 5 WHERE s.id IS NULL AND m.id < 40;", 1);

  //- tec: sort, limit, scans without a predicate
  opt_add_case(cases, &count, "order by limit",
               "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", 0);
  opt_add_case(cases, &count, "join then order by limit",
               "SELECT b.id FROM big b JOIN small s ON b.name = s.name ORDER BY b.value DESC LIMIT 5;", 0);
  opt_add_case(cases, &count, "scan: tiny table, no predicate",
               "SELECT id, name FROM tiny;", 0);
  opt_add_case(cases, &count, "scan: tiny table, predicate",
               "SELECT id, name FROM tiny WHERE id > 2;", 0);
  opt_add_case(cases, &count, "scan: count over unfiltered table",
               "SELECT COUNT(*) AS n FROM mid;", 0);
  opt_add_case(cases, &count, "scan: selective range",
               "SELECT id FROM big WHERE id > 29990;", 0);
  opt_add_case(cases, &count, "scan: unselective range",
               "SELECT id FROM big WHERE id > 100;", 0);

  //- tec: predicates the optimizer can prove or simplify
  opt_add_case(cases, &count, "where: contradiction on one column",
               "SELECT id FROM big WHERE id = 1 AND id = 2;", 0);
  opt_add_case(cases, &count, "where: constant false",
               "SELECT id FROM big WHERE 1 = 0;", 0);
  opt_add_case(cases, &count, "where: constant true",
               "SELECT COUNT(*) AS n FROM mid WHERE 1 = 1;", 0);

  //- tec: ctes, derived tables and subqueries
  opt_add_case(cases, &count, "cte: single reference joined",
               "WITH t AS (SELECT id, value FROM big WHERE value > 90000) SELECT t.id, s.name FROM t JOIN small s ON t.id = s.id;", 0);
  opt_add_case(cases, &count, "cte: unreferenced",
               "WITH unused AS (SELECT id FROM big), t AS (SELECT id FROM small) SELECT COUNT(*) AS n FROM t;", 0);
  opt_add_case(cases, &count, "cte: referenced twice",
               "WITH t AS (SELECT id FROM mid WHERE value > 90000) SELECT COUNT(*) AS n FROM t a JOIN t b ON a.id = b.id;", 0);
  opt_add_case(cases, &count, "derived: filter above",
               "SELECT id FROM (SELECT id, value FROM big) AS t WHERE value > 99000;", 0);
  opt_add_case(cases, &count, "subquery: in list from table",
               "SELECT id FROM mid WHERE id IN (SELECT id FROM small);", 0);
  opt_add_case(cases, &count, "subquery: not in",
               "SELECT COUNT(*) AS n FROM mid WHERE id NOT IN (SELECT id FROM small);", 0);

  bench_print_table_header(report, "optimizer baseline");

  U64 known_gap_open = 0;
  U64 known_gap_closed = 0;

  for (U64 i = 0; i < count; i += 1)
  {
    Opt_Case* test_case = &cases[i];

    U64 gdb_rows = 0;
    U64 gdb_checksum = 0;
    Bench_Stats gdb_stats = bench_run_gdb_query(fixture->database, test_case->sql, &gdb_rows, &gdb_checksum);
    bench_print_table_row(report, test_case->label, "gdb", gdb_rows, gdb_checksum, &gdb_stats);

    U64 sqlite_rows = 0;
    U64 sqlite_checksum = 0;
    Bench_Stats sqlite_stats = bench_run_sqlite_query(fixture->sqlite_db, test_case->sql, &sqlite_rows, &sqlite_checksum);
    bench_print_table_row(report, test_case->label, "sqlite", sqlite_rows, sqlite_checksum, &sqlite_stats);

    U64 duckdb_rows = 0;
    U64 duckdb_checksum = 0;
    Bench_Stats duckdb_stats = bench_run_duckdb_query(fixture->duckdb_conn, test_case->sql, &duckdb_rows, &duckdb_checksum);
    bench_print_table_row(report, test_case->label, "duckdb", duckdb_rows, duckdb_checksum, &duckdb_stats);

    B32 matches_sqlite = (gdb_rows == sqlite_rows) && (gdb_checksum == sqlite_checksum);
    B32 matches_duckdb = (gdb_rows == duckdb_rows) && (gdb_checksum == duckdb_checksum);
    B32 matches = matches_sqlite && matches_duckdb;

    if (test_case->known_gap)
    {
      if (matches)
      {
        known_gap_closed += 1;
        printf("  KNOWN GAP now passing, clear the flag: '%.*s'\n", str8_varg(test_case->label));
        bench_report_text(report, "known gap now passing: %.*s", str8_varg(test_case->label));
      }
      else
      {
        known_gap_open += 1;
        printf("  KNOWN GAP still open: '%.*s' (gdb rows=%llu, sqlite rows=%llu)\n",
               str8_varg(test_case->label), gdb_rows, sqlite_rows);
        bench_report_text(report, "known gap open: %.*s (gdb rows=%llu checksum=%llu, sqlite rows=%llu checksum=%llu)",
                          str8_varg(test_case->label), gdb_rows, gdb_checksum, sqlite_rows, sqlite_checksum);
      }
    }
    else
    {
      bench_check_match(report, test_case->label, "gdb", gdb_rows, gdb_checksum, "sqlite", sqlite_rows, sqlite_checksum);
      bench_check_match(report, test_case->label, "gdb", gdb_rows, gdb_checksum, "duckdb", duckdb_rows, duckdb_checksum);
    }
  }

  printf("\n%llu case(s), %llu known gap(s) open, %llu known gap(s) closed\n", count, known_gap_open, known_gap_closed);
  bench_report_text(report, "%llu case(s), %llu known gap(s) open, %llu known gap(s) closed", count, known_gap_open, known_gap_closed);

  if (fixture->database->temp_table_count != 0)
  {
    printf("  temp tables leaked: %llu FAIL\n", fixture->database->temp_table_count);
    bench_report_warn(report, "%llu temp table(s) still registered after the suite", fixture->database->temp_table_count);
  }

  //- tec: baseline GPU round trips per case, to be beaten by later milestones
  printf("\n---- GPU submit+wait round trips (steady state) ----\n");
  bench_report_section(report, "GPU round trips per query (steady state)");
  for (U64 i = 0; i < count; i += 1)
  {
    U64 round_trips = opt_count_round_trips(fixture->database, cases[i].sql);
    printf("  %-48.*s %llu\n", str8_varg(cases[i].label), round_trips);
    bench_report_text(report, "%.*s: %llu", str8_varg(cases[i].label), round_trips);
  }
}

//~ tec: plan text

internal U64
opt_count_occurrences(String8 haystack, char* needle_cstr)
{
  String8 needle = str8_cstring(needle_cstr);
  U64 occurrences = 0;
  U64 position = 0;
  while (position < haystack.size)
  {
    U64 found = str8_find_needle(haystack, position, needle, 0);
    if (found >= haystack.size)
    {
      break;
    }
    occurrences += 1;
    position = found + needle.size;
  }
  return occurrences;
}

internal void
opt_check_occurrences(Bench_Report* report, char* label, String8 output, char* needle, U64 expected, B32 known_gap)
{
  U64 actual = opt_count_occurrences(output, needle);
  B32 ok = (actual == expected);

  if (known_gap)
  {
    if (ok)
    {
      printf("  %-52s KNOWN GAP now passing, clear the flag\n", label);
      bench_report_text(report, "known gap now passing: %s", label);
    }
    else
    {
      printf("  %-52s KNOWN GAP still open ('%s' x%llu, expected x%llu)\n", label, needle, actual, expected);
      bench_report_text(report, "known gap open: %s ('%s' x%llu, expected x%llu)", label, needle, actual, expected);
    }
    return;
  }

  printf("  %-52s %s ('%s' x%llu)\n", label, ok ? "OK" : "FAIL", needle, actual);
  if (!ok)
  {
    bench_report_warn(report, "'%s' expected '%s' x%llu but found x%llu", label, needle, expected, actual);
  }
}

internal void
opt_run_plan_text_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  printf("\n########## optimizer baseline: plan text ##########\n");
  bench_report_section(report, "optimizer baseline: plan text");

  GDB_Database* database = fixture->database;
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));

  //- tec: a filtered scan should be reported on one line, today the Filter and its Scan child both print
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN ANALYZE SELECT * FROM big WHERE id > 100;"), &database, NULL);
    opt_check_occurrences(report, "explain analyze: filtered scan printed once", result.output_text, "table='big'", 1, 1);
    arena_clear(query_arena);
  }

  //- tec: the join tree stays left-deep in textual order until the optimizer exists
  {
    APP_QueryResult result = app_execute_query_capture(query_arena,
                                                       str8_lit("EXPLAIN SELECT b.id FROM big b JOIN mid m ON b.id = m.id JOIN small s ON m.id = s.id;"),
                                                       &database, NULL);
    opt_check_occurrences(report, "explain: three scans in the plan", result.output_text, "[PLAN_NodeType_Scan]", 3, 0);
    opt_check_occurrences(report, "explain: two joins in the plan", result.output_text, "[PLAN_NodeType_Join]", 2, 0);
    arena_clear(query_arena);
  }

  //- tec: plain EXPLAIN carries no estimates yet, later milestones flip this expectation
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM big WHERE id > 100;"), &database, NULL);
    opt_check_occurrences(report, "explain: no estimates before M3", result.output_text, "est_rows", 0, 0);
    arena_clear(query_arena);
  }

  arena_release(query_arena);
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

  Bench_Report* report = bench_report_alloc(arena, "compute_ql optimizer baseline");

  Opt_Fixture* fixture = opt_fixture_create(arena);

  opt_run_parity_suite(arena, report, fixture);
  opt_run_plan_text_suite(arena, report, fixture);

  opt_fixture_destroy(fixture);

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/optimizer_report.md"));

  arena_release(arena);

  log_release();

  ProfEnd();
  ProfEndCapture();
}
