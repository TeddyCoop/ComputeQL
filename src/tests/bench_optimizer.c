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

#define OPT_CASE_MAX 160

typedef struct Opt_Case Opt_Case;
struct Opt_Case
{
  String8 label;
  String8 sql;
  B32 known_gap;
  B32 requires_optimizer;
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

  // tec: same rows as big, with an index on the unique id and one on the value
  opt_fixture_add_table(scratch.arena, fixture, "ordered", big_rows, big_count);
  GDB_Table* ordered = gdb_database_find_table(fixture->database, str8_lit("ordered"));
  gdb_table_create_index(ordered, str8_lit("ordered_id"), gdb_table_find_column(ordered, str8_lit("id")));
  gdb_table_create_index(ordered, str8_lit("ordered_value"), gdb_table_find_column(ordered, str8_lit("value")));

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
  if (*count >= OPT_CASE_MAX)
  {
    printf("opt_add_case: OPT_CASE_MAX (%d) is too small for '%s'\n", OPT_CASE_MAX, label);
    os_abort(1);
  }
  cases[*count].label = str8_cstring(label);
  cases[*count].sql = str8_cstring(sql);
  cases[*count].known_gap = known_gap;
  cases[*count].requires_optimizer = 0;
  *count += 1;
}

// tec: set OPT_ONLY to a label substring to run just the matching cases
internal B32
opt_case_selected(Opt_Case* test_case)
{
  char* filter = getenv("OPT_ONLY");
  if (!filter)
  {
    return 1;
  }
  String8 label = test_case->label;
  String8 needle = str8_cstring(filter);
  return str8_find_needle(label, 0, needle, 0) < label.size;
}

internal void
opt_set_requires_optimizer(Opt_Case* cases, U64 count, char* label)
{
  String8 wanted = str8_cstring(label);
  for (U64 index = 0; index < count; index += 1)
  {
    if (str8_match(cases[index].label, wanted, 0))
    {
      cases[index].requires_optimizer = 1;
    }
  }
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
    U64 submits_before = g_vulkan_state->submit_count;
    PLAN_ExecResult result = plan_run_select(arena, database, select_node, NULL);
    U64 row_count = 0;
    bench_gdb_consume_result(&result, select_node, &row_count);
    U64 submits_after = g_vulkan_state->submit_count;
    gdb_database_release_temp_tables_from(database, temp_mark);

    round_trips = submits_after - submits_before;
    scratch_end(scratch);
  }

  return round_trips;
}

//~ tec: result parity

internal U64
opt_build_cases(Opt_Case* cases)
{
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

  opt_add_case(cases, &count, "chain: comma joins, predicates in where",
               "SELECT b.id, m.value FROM big b, mid m, small s WHERE b.id = m.id AND m.id = s.id AND s.id < 10;", 0);
  opt_add_case(cases, &count, "chain: comma joins, constant on join column",
               "SELECT b.id FROM big b, mid m, small s WHERE b.id = m.id AND m.id = s.id AND b.id = 5;", 0);
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
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id AND b.name = m.name;", 0);
  opt_add_case(cases, &count, "on: equi key plus range on right",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id AND m.value > 50000;", 0);
  opt_add_case(cases, &count, "on: equi key plus range on left",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id AND b.value > 50000;", 0);
  opt_add_case(cases, &count, "on: left join with extra conjunct",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id AND s.id < 5 WHERE s.id IS NULL AND m.id < 40;", 0);

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
  
  //- tec: constant folding, contradictions and constants carried across equality joins
  opt_add_case(cases, &count, "fold: constant true beside a filter",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id WHERE 1 = 1 AND m.id < 4;", 0);
  opt_add_case(cases, &count, "fold: constant false in a join",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id WHERE 1 = 2;", 0);
  opt_add_case(cases, &count, "propagate: constant on join key, explicit join",
               "SELECT b.id, m.value FROM big b JOIN mid m ON b.id = m.id WHERE b.id = 5;", 0);
  opt_add_case(cases, &count, "propagate: constant on join key, comma join",
               "SELECT m.id, b.value FROM big b, mid m WHERE b.id = m.id AND m.id = 7;", 0);
  opt_add_case(cases, &count, "propagate: string key",
               "SELECT b.id FROM big b JOIN small s ON b.name = s.name WHERE s.name = 'alpha';", 0);
  opt_add_case(cases, &count, "propagate: chain of three",
               "SELECT b.id, s.name FROM big b JOIN mid m ON b.id = m.id JOIN small s ON m.id = s.id WHERE b.id = 3;", 0);
  opt_add_case(cases, &count, "contradiction: two values through a join key",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id WHERE b.id = 5 AND m.id = 6;", 0);
  opt_add_case(cases, &count, "contradiction: two values on one column",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id WHERE m.id = 3 AND m.id = 4;", 0);
  opt_add_case(cases, &count, "contradiction: empty range",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id WHERE b.id > 10 AND b.id < 5;", 0);
  opt_add_case(cases, &count, "contradiction: touching strict bounds",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id WHERE m.id >= 10 AND m.id < 10;", 0);
  opt_add_case(cases, &count, "contradiction: two strings",
               "SELECT b.id FROM big b JOIN small s ON b.name = s.name WHERE s.name = 'alpha' AND s.name = 'bravo';", 0);
  opt_add_case(cases, &count, "range: satisfiable narrow",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id WHERE m.id >= 10 AND m.id <= 10;", 0);
  opt_add_case(cases, &count, "left join: constant on the preserved side",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id WHERE m.id = 5;", 0);
  opt_add_case(cases, &count, "left join: constant on the joined side",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id WHERE s.id = 5;", 0);
  opt_add_case(cases, &count, "left join: false constant beside it",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id AND 1 = 2 WHERE m.id < 40;", 0);
  
  //- tec: ctes and derived tables that can be folded into the query
  opt_add_case(cases, &count, "inline: derived with filter, joined",
               "SELECT t.id, s.name FROM (SELECT id, value FROM big WHERE value > 90000) t JOIN small s ON t.id = s.id;", 0);
  opt_add_case(cases, &count, "inline: outer filter reaches the derived table",
               "SELECT t.id FROM (SELECT id, value FROM big) t JOIN mid m ON t.id = m.id WHERE t.value > 99000;", 0);
  opt_add_case(cases, &count, "inline: hidden column resolves elsewhere",
               "SELECT t.id FROM (SELECT id FROM big) t JOIN mid m ON t.id = m.id WHERE value > 50000;", 0);
  opt_add_case(cases, &count, "inline: bare star over an explicit list",
               "SELECT * FROM (SELECT id, name FROM big WHERE id < 20) t;", 0);
  opt_add_case(cases, &count, "inline: bare star over a bare star",
               "SELECT * FROM (SELECT * FROM small WHERE id < 5) t;", 0);
  opt_add_case(cases, &count, "inline: left join with a filtered derived table",
               "SELECT m.id FROM mid m LEFT JOIN (SELECT id FROM small WHERE id < 5) t ON m.id = t.id WHERE m.id < 40;", 0);
  opt_add_case(cases, &count, "inline: left join with an unfiltered derived table",
               "SELECT m.id FROM mid m LEFT JOIN (SELECT id, name FROM small) t ON m.id = t.id WHERE m.id < 40;", 0);
  opt_add_case(cases, &count, "inline: cte used once, single table outside",
               "WITH t AS (SELECT id, value FROM big WHERE value > 50000) SELECT COUNT(*) AS n FROM t WHERE id < 1000;", 0);
  opt_add_case(cases, &count, "inline: cte used once, joined",
               "WITH t AS (SELECT id, value FROM mid WHERE value > 50000) SELECT t.id, b.value FROM t JOIN big b ON t.id = b.id;", 0);
  opt_add_case(cases, &count, "inline: cte alias in the reference",
               "WITH t AS (SELECT id, value FROM mid WHERE value > 50000) SELECT x.id FROM t x JOIN small s ON x.id = s.id;", 0);
  opt_add_case(cases, &count, "inline: two ctes, both used once",
               "WITH a AS (SELECT id FROM mid WHERE id < 100), b AS (SELECT id FROM small WHERE id < 8) SELECT COUNT(*) AS n FROM a JOIN b ON a.id = b.id;", 0);
  opt_add_case(cases, &count, "inline: aliased column is not inlined",
               "SELECT t.k FROM (SELECT id AS k FROM small WHERE id < 6) t;", 0);
  opt_add_case(cases, &count, "inline: derived over a join is not inlined",
               "SELECT t.id FROM (SELECT b.id FROM big b JOIN small s ON b.name = s.name WHERE s.id < 3) t;", 0);
  opt_add_case(cases, &count, "inline: derived with limit is not inlined",
               "SELECT t.id FROM (SELECT id FROM big WHERE id > 100 ORDER BY id LIMIT 5) t;", 0);
  
  //- tec: join order, written so the first join is the wrong one
  opt_add_case(cases, &count, "order: small filter joined last",
               "SELECT b.id FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id;", 0);
  opt_add_case(cases, &count, "order: comma join, wrong first pair",
               "SELECT b.id, m.value FROM big b, small s, mid m WHERE b.name = s.name AND b.id = m.id;", 0);
  opt_add_case(cases, &count, "order: chain through a shared key",
               "SELECT b.id, s.name FROM big b JOIN mid m ON b.id = m.id JOIN small s ON m.id = s.id;", 0);
  opt_add_case(cases, &count, "order: many to many join written first",
               "SELECT COUNT(*) AS n FROM mid a JOIN mid c ON a.name = c.name JOIN small s ON s.id = a.id;", 0);
  opt_add_case(cases, &count, "order: four tables, dimensions last",
               "SELECT COUNT(*) AS n FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id JOIN tiny t ON t.id = m.id;", 0);
  opt_add_case(cases, &count, "order: filters on two relations",
               "SELECT b.id FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id WHERE m.value > 90000 AND s.id < 4;", 0);
  opt_add_case(cases, &count, "order: left join stays put",
               "SELECT b.id FROM big b JOIN mid m ON b.id = m.id LEFT JOIN small s ON b.name = s.name WHERE b.id < 200;", 0);
  opt_add_case(cases, &count, "order: inner join after a left join stays put",
               "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id JOIN tiny t ON m.id = t.id;", 0);
  opt_add_case(cases, &count, "order: aggregate over reordered joins",
               "SELECT s.name, COUNT(*) AS c FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id GROUP BY s.name;", 0);
  opt_add_case(cases, &count, "order: order by over reordered joins",
               "SELECT b.id, m.value FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id ORDER BY m.value DESC LIMIT 5;", 0);

  // tec: more relations than the exhaustive search holds, so the greedy search orders them
  opt_add_case(cases, &count, "order: twelve relations use the greedy search",
               "SELECT COUNT(*) AS n FROM tiny a JOIN tiny b ON a.id = b.id JOIN tiny c ON b.id = c.id JOIN tiny d ON c.id = d.id "
               "JOIN tiny e ON d.id = e.id JOIN tiny f ON e.id = f.id JOIN tiny g ON f.id = g.id JOIN tiny h ON g.id = h.id "
               "JOIN tiny i ON h.id = i.id JOIN tiny j ON i.id = j.id JOIN tiny k ON j.id = k.id JOIN tiny l ON k.id = l.id;", 0);

  //- tec: ordering, top-K and small inputs. the checksum ignores row order, so what these check is which rows come back
  opt_add_case(cases, &count, "topn: numeric key descending",
               "SELECT id, value FROM big ORDER BY value DESC, id LIMIT 10;", 0);
  opt_add_case(cases, &count, "topn: limit with offset",
               "SELECT id FROM big ORDER BY id LIMIT 5 OFFSET 20;", 0);
  opt_add_case(cases, &count, "topn: string key",
               "SELECT id, name FROM small ORDER BY name DESC, id LIMIT 4;", 0);
  opt_add_case(cases, &count, "topn: after a filter",
               "SELECT id, value FROM big WHERE value > 50000 ORDER BY value, id LIMIT 7;", 0);
  opt_add_case(cases, &count, "topn: limit larger than the input",
               "SELECT id FROM tiny ORDER BY id DESC LIMIT 100;", 0);
  opt_add_case(cases, &count, "topn: over an aggregate",
               "SELECT name, COUNT(*) AS c FROM big GROUP BY name ORDER BY c DESC, name LIMIT 5;", 0);
  opt_add_case(cases, &count, "topn: over a join",
               "SELECT b.id, s.name FROM big b JOIN small s ON b.name = s.name ORDER BY b.id DESC, s.id LIMIT 5;", 0);
  opt_add_case(cases, &count, "topn: two numeric keys",
               "SELECT id, name, value FROM mid ORDER BY name, value DESC LIMIT 25;", 0);
  opt_add_case(cases, &count, "sort: small numeric input",
               "SELECT id, value FROM tiny ORDER BY value DESC, id;", 0);
  opt_add_case(cases, &count, "sort: mid sized numeric input",
               "SELECT id, value FROM mid ORDER BY value, id;", 0);
  opt_add_case(cases, &count, "sort: aggregate output without a limit",
               "SELECT name, COUNT(*) AS c FROM big GROUP BY name ORDER BY c, name;", 0);
  opt_add_case(cases, &count, "index order: range scan sorted ascending",
               "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY id;", 0);
  opt_add_case(cases, &count, "index order: range scan sorted descending",
               "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY id DESC;", 0);
  opt_add_case(cases, &count, "index order: walk without a filter",
               "SELECT id, value FROM ordered ORDER BY id DESC LIMIT 10;", 0);
  opt_add_case(cases, &count, "index order: walk with a filter and offset",
               "SELECT id, value FROM ordered WHERE value > 50000 ORDER BY id LIMIT 10 OFFSET 5;", 0);
  opt_add_case(cases, &count, "index order: walk that finds fewer rows than asked for",
               "SELECT id, value FROM ordered WHERE value > 99990 ORDER BY id LIMIT 500;", 0);
  opt_add_case(cases, &count, "index order: walk with a filter that matches nothing",
               "SELECT id FROM ordered WHERE value > 100000000 ORDER BY id LIMIT 5;", 0);
  opt_add_case(cases, &count, "aggregate: cpu string key",
               "SELECT name, COUNT(*) AS c, SUM(value) AS s, AVG(value) AS a, MIN(value) AS lo, MAX(value) AS hi FROM mid GROUP BY name;", 0);
  opt_add_case(cases, &count, "aggregate: cpu numeric key with many groups",
               "SELECT id, COUNT(*) AS c, SUM(value) AS s FROM mid GROUP BY id;", 0);
  opt_add_case(cases, &count, "aggregate: cpu without group by",
               "SELECT COUNT(*) AS n, SUM(value) AS s, MIN(value) AS lo, MAX(value) AS hi, AVG(value) AS a FROM small;", 0);
  opt_add_case(cases, &count, "aggregate: cpu over a join",
               "SELECT s.name, COUNT(*) AS c FROM mid m JOIN small s ON m.name = s.name GROUP BY s.name;", 0);
  opt_add_case(cases, &count, "aggregate: gpu above the crossover",
               "SELECT name, COUNT(*) AS c, SUM(value) AS s FROM ordered GROUP BY name;", 0);
  opt_add_case(cases, &count, "aggregate: many groups sized from the estimate",
               "SELECT id, COUNT(*) AS c FROM ordered GROUP BY id;", 0);
  opt_add_case(cases, &count, "join: many to many fan out",
               "SELECT COUNT(*) AS n FROM mid a JOIN mid c ON a.name = c.name;", 0);

  //- tec: a qualified column in the WHERE of a select over one aliased table
  opt_add_case(cases, &count, "where: qualified column on one aliased table",
               "SELECT m.id FROM mid m WHERE m.id < 10;", 0);
  opt_add_case(cases, &count, "where: qualified columns in an IN list and a range",
               "SELECT m.id, m.value FROM mid m WHERE m.id IN (3, 5, 7) OR m.value > 99000;", 0);
  //- tec: results that straddle the number of rows a scan copies back in its first submit
  opt_add_case(cases, &count, "scan: result just over the speculative rows",
               "SELECT id, value FROM big WHERE value > 86000;", 0);
  opt_add_case(cases, &count, "scan: result just under the speculative rows",
               "SELECT id, value FROM big WHERE value > 87000;", 0);

  //- tec: these only work once the optimizer places the predicates
  opt_set_requires_optimizer(cases, count, "chain: comma joins, predicates in where");
  opt_set_requires_optimizer(cases, count, "chain: comma joins, constant on join column");
  opt_set_requires_optimizer(cases, count, "on: left join with extra conjunct");
  opt_set_requires_optimizer(cases, count, "order: comma join, wrong first pair");
  opt_set_requires_optimizer(cases, count, "left join: false constant beside it");
  
  return count;
}

internal void
opt_run_parity_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture, Opt_Case* cases, U64 count)
{
  printf("\n########## optimizer baseline: result parity ##########\n");
  bench_report_section(report, "optimizer baseline: result parity");

  bench_print_table_header(report, "optimizer baseline");

  U64 known_gap_open = 0;
  U64 known_gap_closed = 0;

  for (U64 i = 0; i < count; i += 1)
  {
    Opt_Case* test_case = &cases[i];
    if (!opt_case_selected(test_case))
    {
      continue;
    }
    
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
    if (!opt_case_selected(&cases[i]))
    {
      continue;
    }
    U64 round_trips = opt_count_round_trips(fixture->database, cases[i].sql);
    printf("  %-48.*s %llu\n", str8_varg(cases[i].label), round_trips);
    bench_report_text(report, "%.*s: %llu", str8_varg(cases[i].label), round_trips);
  }
}

//~ tec: optimizer on versus off

internal void
opt_set_optimizer(B32 enabled)
{
  if (enabled)
  {
    settings_load_from_string(str8_lit("QE_OPTIMIZER: true"));
  }
  else
  {
    settings_load_from_string(str8_lit("QE_OPTIMIZER: false"));
  }
}

internal void
opt_run_ab_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture, Opt_Case* cases, U64 count)
{
  printf("\n########## optimizer on vs off ##########\n");
  bench_report_section(report, "optimizer on vs off (gdb only, median total ms, GPU round trips)");
  printf("  %-48s %10s %10s %7s %9s\n", "query", "on ms", "off ms", "ratio", "trips");

  U64 slower_count = 0;
  U64 compared_count = 0;

  for (U64 index = 0; index < count; index += 1)
  {
    Opt_Case* test_case = &cases[index];
    if (test_case->requires_optimizer || !opt_case_selected(test_case))
    {
      continue;
    }
    
    opt_set_optimizer(1);
    U64 rows_on = 0;
    U64 checksum_on = 0;
    Bench_Stats stats_on = bench_run_gdb_query(fixture->database, test_case->sql, &rows_on, &checksum_on);
    U64 trips_on = opt_count_round_trips(fixture->database, test_case->sql);

    opt_set_optimizer(0);
    U64 rows_off = 0;
    U64 checksum_off = 0;
    Bench_Stats stats_off = bench_run_gdb_query(fixture->database, test_case->sql, &rows_off, &checksum_off);
    U64 trips_off = opt_count_round_trips(fixture->database, test_case->sql);

    opt_set_optimizer(1);

    if (rows_on != rows_off || checksum_on != checksum_off)
    {
      printf("  !! MISMATCH on '%.*s': optimizer on rows=%llu, off rows=%llu\n", str8_varg(test_case->label), rows_on, rows_off);
      bench_report_warn(report, "MISMATCH on '%.*s': optimizer on rows=%llu checksum=%llu, off rows=%llu checksum=%llu",
                        str8_varg(test_case->label), rows_on, checksum_on, rows_off, checksum_off);
    }

    F64 ratio = 0.0;
    if (stats_off.total_median > 0.0)
    {
      ratio = stats_on.total_median / stats_off.total_median;
    }
    compared_count += 1;
    // tec: a difference under 50 microseconds is planning overhead on a query that takes about that long in total
    B32 slower = ratio > 1.10 && (stats_on.total_median - stats_off.total_median) > 0.05;
    if (slower)
    {
      slower_count += 1;
    }

    printf("  %-48.*s %10.3f %10.3f %6.2fx %4llu/%-4llu\n", str8_varg(test_case->label),
           stats_on.total_median, stats_off.total_median, ratio, trips_on, trips_off);
    bench_report_text(report, "%.*s: on=%.3f ms off=%.3f ms ratio=%.2fx trips on=%llu off=%llu", str8_varg(test_case->label),
                      stats_on.total_median, stats_off.total_median, ratio, trips_on, trips_off);
  }

  printf("\n%llu of %llu queries ran more than 10%% slower with the optimizer on\n", slower_count, compared_count);
  bench_report_text(report, "%llu of %llu queries ran more than 10%% slower with the optimizer on", slower_count, compared_count);
}

//~ tec: scan strategy choices

internal void
opt_write_text_file(String8 path, String8 content)
{
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if (!os_handle_match(file, os_handle_zero()))
  {
    os_file_write(file, r1u64(0, content.size), content.str);
    os_file_close(file);
  }
}

// tec: 'a' never holds a NULL, 'b' is NULL on every tenth row
internal void
opt_write_nullable_csv(Arena* arena, String8 path, U64 row_count)
{
  String8List lines = {0};
  str8_list_pushf(arena, &lines, "id,a,b\n");
  for (U64 id = 1; id <= row_count; id += 1)
  {
    if (id % 10 == 0)
    {
      str8_list_pushf(arena, &lines, "%llu,%llu,\n", id, id % 100);
    }
    else
    {
      str8_list_pushf(arena, &lines, "%llu,%llu,%llu\n", id, id % 100, id % 50);
    }
  }
  opt_write_text_file(path, str8_list_join(arena, &lines, 0));
}

internal void
opt_check_output_contains(Bench_Report* report, char* label, String8 output, char* needle)
{
  B32 found = str8_find_needle(output, 0, str8_cstring(needle), 0) < output.size;
  printf("  %-56s %s ('%s')\n", label, found ? "OK" : "FAIL", needle);
  if (!found)
  {
    bench_report_warn(report, "'%s' expected output to contain '%s'", label, needle);
  }
}

internal PLAN_Node*
opt_find_filter(PLAN_Node* plan)
{
  if (!plan)
  {
    return NULL;
  }
  if (plan->type == PLAN_NodeType_Filter)
  {
    return plan;
  }
  PLAN_Node* from_input = opt_find_filter(plan->input);
  if (from_input)
  {
    return from_input;
  }
  return opt_find_filter(plan->input2);
}

// tec: the column of the first condition the plan's filter will evaluate
internal String8
opt_first_condition_column(Arena* arena, GDB_Database* database, String8 sql_text)
{
  SQL_TokenizeResult tokens = sql_tokenize_from_text(arena, sql_text);
  SQL_Node* ast = sql_parse(arena, tokens.tokens, tokens.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(arena, ast);
  IR_Node* select_node = ir_query->execution_nodes;

  PLAN_Node* plan = plan_build_from_select(arena, database, select_node);
  PLAN_Node* filter = opt_find_filter(plan);
  if (!filter || !filter->condition || !filter->condition->first)
  {
    return str8_lit("");
  }

  IR_Node* leaves[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  U32 leaf_count = optimizer_ir_collect_conjuncts(filter->condition->first, leaves, 0, OPT_ESTIMATE_LEAF_CAPACITY);
  if (leaf_count == 0 || !leaves[0]->first)
  {
    return str8_lit("");
  }
  return leaves[0]->first->value;
}

internal void
opt_run_strategy_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  printf("\n########## optimizer: scan strategy ##########\n");
  bench_report_section(report, "optimizer: scan strategy");

  Temp scratch = scratch_begin(&arena, 1);
  GDB_Database* database = fixture->database;
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));

  //- tec: an indexed table with an oracle copy in sqlite and duckdb, so index results are checked too
  Bench_Row* indexed_rows = opt_generate_rows(scratch.arena, 20000, 0x1DE5ULL);
  opt_fixture_add_table(scratch.arena, fixture, "indexed", indexed_rows, 20000);
  GDB_Table* indexed_table = gdb_database_find_table(database, str8_lit("indexed"));
  GDB_Column* indexed_id = gdb_table_find_column(indexed_table, str8_lit("id"));
  gdb_table_create_index(indexed_table, str8_lit("indexed_id_index"), indexed_id);

  //- tec: a table with a NULL in one column only
  U64 nullable_rows = 60000;
  opt_write_nullable_csv(scratch.arena, str8_lit("bench_data/opt_nully.csv"), nullable_rows);
  gdb_database_add_table(database, gdb_table_import_csv_streaming(database, str8_lit("nully"), str8_lit("bench_data/opt_nully.csv")));

  //- tec: plan choices
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM tiny;"), &database, NULL);
    opt_check_output_contains(report, "no predicate: identity", result.output_text, "strategy=identity");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM tiny WHERE id > 2;"), &database, NULL);
    opt_check_output_contains(report, "small table: cpu", result.output_text, "strategy=cpu");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM big WHERE value > 90000;"), &database, NULL);
    opt_check_output_contains(report, "large table: gpu", result.output_text, "strategy=gpu");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM indexed WHERE id = 500;"), &database, NULL);
    opt_check_output_contains(report, "selective indexed equality: index", result.output_text, "strategy=index");
    opt_check_output_contains(report, "index reason names the match count", result.output_text, "index narrows to 1 rows");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM indexed WHERE id > 100 AND value > 50;"), &database, NULL);
    opt_check_output_contains(report, "wide index range with a residual: not index", result.output_text, "strategy=gpu");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM indexed WHERE id = 500 AND value > 50;"), &database, NULL);
    opt_check_output_contains(report, "selective index leaf beside a residual: index", result.output_text, "strategy=index");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM nully WHERE a < 10;"), &database, NULL);
    opt_check_output_contains(report, "nulls elsewhere in the table: gpu", result.output_text, "strategy=gpu");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM nully WHERE b < 5;"), &database, NULL);
    opt_check_output_contains(report, "null in a referenced column: cpu", result.output_text, "strategy=cpu");
    opt_check_output_contains(report, "cpu reason names the nulls", result.output_text, "a referenced column has NULLs");
    arena_clear(query_arena);
  }

  //- tec: and the answers stay right on every path
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("SELECT COUNT(*) AS n FROM nully WHERE a < 10;"), &database, NULL);
    opt_check_output_contains(report, "count on the gpu path beside nulls", result.output_text, "6000");
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("SELECT COUNT(*) AS n FROM nully WHERE b < 5;"), &database, NULL);
    opt_check_output_contains(report, "count on the cpu path over nulls", result.output_text, "4800");
    arena_clear(query_arena);
  }

  Opt_Case cases[8];
  U64 count = 0;
  opt_add_case(cases, &count, "index: equality", "SELECT id, value FROM indexed WHERE id = 500;", 0);
  opt_add_case(cases, &count, "index: narrow range", "SELECT id FROM indexed WHERE id < 200;", 0);
  opt_add_case(cases, &count, "index: wide range", "SELECT id FROM indexed WHERE id > 100;", 0);
  opt_add_case(cases, &count, "index: leaf plus residual", "SELECT id FROM indexed WHERE id > 15000 AND value > 50000;", 0);
  opt_add_case(cases, &count, "index: equality plus residual", "SELECT id FROM indexed WHERE id = 500 AND value > 50;", 0);
  opt_add_case(cases, &count, "index: two indexable leaves", "SELECT id FROM indexed WHERE id > 100 AND id < 130;", 0);
  opt_add_case(cases, &count, "index: join with an indexed table", "SELECT i.id FROM indexed i JOIN small s ON i.name = s.name WHERE i.id < 50;", 0);

  bench_print_table_header(report, "scan strategy");
  for (U64 index = 0; index < count; index += 1)
  {
    Opt_Case* test_case = &cases[index];

    U64 gdb_rows = 0;
    U64 gdb_checksum = 0;
    Bench_Stats gdb_stats = bench_run_gdb_query(database, test_case->sql, &gdb_rows, &gdb_checksum);
    bench_print_table_row(report, test_case->label, "gdb", gdb_rows, gdb_checksum, &gdb_stats);

    U64 sqlite_rows = 0;
    U64 sqlite_checksum = 0;
    Bench_Stats sqlite_stats = bench_run_sqlite_query(fixture->sqlite_db, test_case->sql, &sqlite_rows, &sqlite_checksum);
    bench_print_table_row(report, test_case->label, "sqlite", sqlite_rows, sqlite_checksum, &sqlite_stats);

    bench_check_match(report, test_case->label, "gdb", gdb_rows, gdb_checksum, "sqlite", sqlite_rows, sqlite_checksum);
  }

  //- tec: a CPU scan puts its most selective condition first, whatever order the query wrote them in
  {
    String8 written_first = opt_first_condition_column(query_arena, database, str8_lit("SELECT id FROM nully WHERE b >= 0 AND a = 5;"));
    B32 ordered = str8_match(written_first, str8_lit("a"), 0);
    printf("  %-56s %s (first condition reads '%.*s')\n", "cpu scan puts the selective condition first", ordered ? "OK" : "FAIL", str8_varg(written_first));
    if (!ordered)
    {
      bench_report_warn(report, "expected the selective condition on 'a' first, found '%.*s'", str8_varg(written_first));
    }
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("SELECT COUNT(*) AS n FROM nully WHERE b >= 0 AND a = 5;"), &database, NULL);
    opt_check_output_contains(report, "reordered conditions give the same count", result.output_text, "600");
    arena_clear(query_arena);
  }
  {
    String8 sql = str8_lit("SELECT id FROM nully WHERE b >= 0 AND a = 5;");
    settings_load_from_string(str8_lit("QE_OPT_SCAN_COST: true"));
    U64 rows_on = 0;
    U64 checksum_on = 0;
    Bench_Stats stats_on = bench_run_gdb_query(database, sql, &rows_on, &checksum_on);
    settings_load_from_string(str8_lit("QE_OPT_SCAN_COST: false"));
    U64 rows_off = 0;
    U64 checksum_off = 0;
    Bench_Stats stats_off = bench_run_gdb_query(database, sql, &rows_off, &checksum_off);
    settings_load_from_string(str8_lit("QE_OPT_SCAN_COST: true"));

    printf("  cpu scan, written order vs reordered: %.3f ms vs %.3f ms\n", stats_off.total_median, stats_on.total_median);
    bench_report_text(report, "cpu scan over 60000 rows, written order %.3f ms, reordered %.3f ms", stats_off.total_median, stats_on.total_median);
    if (rows_on != rows_off || checksum_on != checksum_off)
    {
      bench_report_warn(report, "reordering changed the result: on rows=%llu, off rows=%llu", rows_on, rows_off);
    }
  }

  arena_release(query_arena);
  scratch_end(scratch);
}

//~ tec: join order

// tec: the tables of a plan from its leftmost scan to its rightmost, for example "mid,big,small"
internal String8
opt_explain_table_order(Arena* arena, GDB_Database** database, char* sql)
{
  String8 query = push_str8f(arena, "EXPLAIN %s", sql);
  APP_QueryResult result = app_execute_query_capture(arena, query, database, NULL);

  String8 needle = str8_lit("table='");
  String8List names = {0};
  U64 position = 0;
  while (position < result.output_text.size)
  {
    U64 found = str8_find_needle(result.output_text, position, needle, 0);
    if (found >= result.output_text.size)
    {
      break;
    }
    U64 name_start = found + needle.size;
    U64 name_end = name_start;
    while (name_end < result.output_text.size && result.output_text.str[name_end] != '\'')
    {
      name_end += 1;
    }
    str8_list_push(arena, &names, str8(result.output_text.str + name_start, name_end - name_start));
    position = name_end;
  }
  return str8_list_join(arena, &names, &(StringJoin){.sep = str8_lit(",")});
}

internal void
opt_check_table_order(Bench_Report* report, Arena* arena, GDB_Database** database, char* label, char* sql, char* expected)
{
  String8 order = opt_explain_table_order(arena, database, sql);
  B32 ok = str8_match(order, str8_cstring(expected), 0);
  printf("  %-56s %s (%.*s)\n", label, ok ? "OK" : "FAIL", str8_varg(order));
  if (!ok)
  {
    bench_report_warn(report, "'%s' expected join order %s but got %.*s", label, expected, str8_varg(order));
  }
}

// tec: passes when the plan's join order is not the written one
internal void
opt_check_table_order_differs(Bench_Report* report, Arena* arena, GDB_Database** database, char* label, char* sql, char* written)
{
  String8 order = opt_explain_table_order(arena, database, sql);
  B32 ok = !str8_match(order, str8_cstring(written), 0);
  printf("  %-56s %s (%.*s)\n", label, ok ? "OK" : "FAIL", str8_varg(order));
  if (!ok)
  {
    bench_report_warn(report, "'%s' expected the join order to change from %s", label, written);
  }
}

// tec: passes when the last table in the plan's join order is the given one
internal void
opt_check_table_order_ends_with(Bench_Report* report, Arena* arena, GDB_Database** database, char* label, char* sql, char* last_table)
{
  String8 order = opt_explain_table_order(arena, database, sql);
  String8 suffix = push_str8f(arena, ",%s", last_table);
  B32 ok = order.size >= suffix.size && str8_match(str8_postfix(order, suffix.size), suffix, 0);
  printf("  %-56s %s (%.*s)\n", label, ok ? "OK" : "FAIL", str8_varg(order));
  if (!ok)
  {
    bench_report_warn(report, "'%s' expected %s to be joined last, plan order is %.*s", label, last_table, str8_varg(order));
  }
}

internal void
opt_check_unit(Bench_Report* report, char* label, B32 ok)
{
  printf("  %-56s %s\n", label, ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "unit check failed: %s", label);
  }
}

// tec: a-b and b-c written, so a-c has to be implied
internal void
opt_run_derived_edge_unit(Bench_Report* report, GDB_Database* database)
{
  GDB_Table* big = gdb_database_find_table(database, str8_lit("big"));
  GDB_Table* mid = gdb_database_find_table(database, str8_lit("mid"));
  GDB_Table* small_table = gdb_database_find_table(database, str8_lit("small"));
  GDB_Column* big_id = gdb_table_find_column(big, str8_lit("id"));
  GDB_Column* mid_id = gdb_table_find_column(mid, str8_lit("id"));
  GDB_Column* small_id = gdb_table_find_column(small_table, str8_lit("id"));
  GDB_Column* big_name = gdb_table_find_column(big, str8_lit("name"));
  GDB_Column* small_name = gdb_table_find_column(small_table, str8_lit("name"));

  OPT_JoinGraph graph = {0};
  graph.relation_count = 3;
  optimizer_join_add_edge(&graph, 0, big_id, str8_lit("id"), 1, mid_id, str8_lit("id"), 0);
  optimizer_join_add_edge(&graph, 1, mid_id, str8_lit("id"), 2, small_id, str8_lit("id"), 0);
  optimizer_join_add_derived_edges(&graph);

  OPT_JoinEdge* implied = NULL;
  B32 has_implied = optimizer_join_has_edge_between(&graph, 0, 2, 0, &implied);
  opt_check_unit(report, "unit: a=b and b=c imply a=c", has_implied && implied->is_derived);
  opt_check_unit(report, "unit: written edges are not marked derived", optimizer_join_has_edge_between(&graph, 0, 1, 1, NULL) && optimizer_join_has_edge_between(&graph, 1, 2, 1, NULL));
  opt_check_unit(report, "unit: the implied edge is not written", !optimizer_join_has_edge_between(&graph, 0, 2, 1, NULL));

  // tec: two different key columns are different classes, nothing is implied between them
  OPT_JoinGraph separate = {0};
  separate.relation_count = 3;
  optimizer_join_add_edge(&separate, 0, big_id, str8_lit("id"), 1, mid_id, str8_lit("id"), 0);
  optimizer_join_add_edge(&separate, 0, big_name, str8_lit("name"), 2, small_name, str8_lit("name"), 0);
  optimizer_join_add_derived_edges(&separate);
  opt_check_unit(report, "unit: different key columns imply nothing", !optimizer_join_has_edge_between(&separate, 1, 2, 0, NULL));

  // tec: a relation with no edge cannot be joined, the search must say so
  OPT_JoinGraph disconnected = {0};
  disconnected.relation_count = 3;
  disconnected.rows[0] = 100.0;
  disconnected.rows[1] = 100.0;
  disconnected.rows[2] = 100.0;
  optimizer_join_add_edge(&disconnected, 0, big_id, str8_lit("id"), 1, mid_id, str8_lit("id"), 0);
  OPT_CostModel model = optimizer_cost_model_load();
  U32 order[OPT_MAX_RELATIONS] = {0};
  F64 cost = 0.0;
  opt_check_unit(report, "unit: no order exists without a connecting edge", !optimizer_join_order_dp(&disconnected, &model, 3, order, &cost));

  // tec: relations 0 and 1 share a low cardinality key so joining them first makes a huge intermediate result, relation 2 is tiny and joins on a unique key
  GDB_Column* mid_name = gdb_table_find_column(mid, str8_lit("name"));
  OPT_JoinGraph blowup = {0};
  blowup.relation_count = 3;
  blowup.rows[0] = 3000.0;
  blowup.rows[1] = 3000.0;
  blowup.rows[2] = 16.0;
  optimizer_join_add_edge(&blowup, 0, mid_name, str8_lit("name"), 1, mid_name, str8_lit("name"), 0);
  optimizer_join_add_edge(&blowup, 0, mid_id, str8_lit("id"), 2, small_id, str8_lit("id"), 0);
  optimizer_join_add_derived_edges(&blowup);

  B32 found = optimizer_join_order_dp(&blowup, &model, 3, order, &cost);
  opt_check_unit(report, "unit: search finds an order for the many to many chain", found);
  B32 first_pair_is_blowup = (order[0] == 0 && order[1] == 1) || (order[0] == 1 && order[1] == 0);
  opt_check_unit(report, "unit: search does not join the many to many pair first", found && !first_pair_is_blowup);

  U32 written[3] = { 0, 1, 2 };
  B32 written_valid = 0;
  F64 written_cost = optimizer_join_order_cost(&blowup, &model, written, 3, &written_valid);
  opt_check_unit(report, "unit: the found order is cheaper than the written one", found && written_valid && cost < written_cost);

  U32 greedy_order[OPT_MAX_RELATIONS] = {0};
  F64 greedy_cost = 0.0;
  B32 greedy_found = optimizer_join_order_greedy(&blowup, &model, 3, greedy_order, &greedy_cost);
  opt_check_unit(report, "unit: greedy starts from the smallest relation", greedy_found && greedy_order[0] == 2);
}

internal void
opt_run_join_order_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  printf("\n########## optimizer: join order ##########\n");
  bench_report_section(report, "optimizer: join order");

  GDB_Database* database = fixture->database;
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));

  opt_run_derived_edge_unit(report, database);

  //- tec: what the plan looks like
  opt_check_table_order_ends_with(report, query_arena, &database, "the 16 row dimension moves to the end",
                                  "SELECT b.id FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id;", "small");
  opt_check_table_order_differs(report, query_arena, &database, "many to many join is not first",
                               "SELECT COUNT(*) AS n FROM mid a JOIN mid c ON a.name = c.name JOIN small s ON s.id = a.id;", "mid,mid,small");
  opt_check_table_order(report, query_arena, &database, "already good order is kept",
                       "SELECT b.id FROM big b JOIN mid m ON b.id = m.id;", "big,mid");

  //- tec: ordering is off limits when it could change what a column name means
  opt_check_table_order(report, query_arena, &database, "ambiguous unqualified column blocks reordering",
                       "SELECT id FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id;", "big,small,mid");
  opt_check_table_order(report, query_arena, &database, "left join and what follows keep their place",
                       "SELECT m.id FROM mid m LEFT JOIN small s ON m.id = s.id JOIN tiny t ON m.id = t.id;", "mid,small,tiny");

  //- tec: the same queries with only the join order switched off, results must agree and time is reported
  char* timed_queries[] =
  {
    "SELECT COUNT(*) AS n FROM mid a JOIN mid c ON a.name = c.name JOIN small s ON s.id = a.id;",
    "SELECT b.id FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id;",
    "SELECT COUNT(*) AS n FROM big b JOIN small s ON b.name = s.name JOIN mid m ON b.id = m.id JOIN tiny t ON t.id = m.id;",
  };

  printf("  %-64s %10s %10s\n", "query", "ordered ms", "written ms");
  for (U64 index = 0; index < ArrayCount(timed_queries); index += 1)
  {
    String8 sql = str8_cstring(timed_queries[index]);
    settings_load_from_string(str8_lit("QE_OPT_JOIN_ORDER: true"));
    U64 rows_ordered = 0;
    U64 checksum_ordered = 0;
    Bench_Stats stats_ordered = bench_run_gdb_query(database, sql, &rows_ordered, &checksum_ordered);
    U64 trips_ordered = opt_count_round_trips(database, sql);

    settings_load_from_string(str8_lit("QE_OPT_JOIN_ORDER: false"));
    U64 rows_written = 0;
    U64 checksum_written = 0;
    Bench_Stats stats_written = bench_run_gdb_query(database, sql, &rows_written, &checksum_written);
    settings_load_from_string(str8_lit("QE_OPT_JOIN_ORDER: true"));

    printf("  %-64.64s %10.3f %10.3f\n", timed_queries[index], stats_ordered.total_median, stats_written.total_median);
    bench_report_text(report, "%s: ordered %.3f ms, written order %.3f ms (%llu round trips)", timed_queries[index], stats_ordered.total_median, stats_written.total_median, trips_ordered);
    if (rows_ordered != rows_written || checksum_ordered != checksum_written)
    {
      bench_report_warn(report, "join order changed the result of '%s': rows %llu vs %llu", timed_queries[index], rows_ordered, rows_written);
    }
  }

  arena_release(query_arena);
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
    opt_check_occurrences(report, "explain analyze: filtered scan printed once", result.output_text, "table='big'", 1, 0);
    opt_check_occurrences(report, "explain analyze: q_error on every reported node", result.output_text, "q_error=", 2, 0);
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

  //- tec: plain EXPLAIN carries an estimate on every node, Project, Filter and Scan here
  {
    APP_QueryResult result = app_execute_query_capture(query_arena, str8_lit("EXPLAIN SELECT id FROM big WHERE id > 100;"), &database, NULL);
    opt_check_occurrences(report, "explain: an estimate on every node", result.output_text, "est_rows=", 3, 0);
    arena_clear(query_arena);
  }
  
  //- tec: a CTE that is not inlined is estimated from its own plan instead of reported as not found
  {
    String8 query = str8_lit("EXPLAIN WITH t AS (SELECT name, COUNT(*) AS c FROM big GROUP BY name) SELECT name FROM t WHERE c > 10;");
    APP_QueryResult result = app_execute_query_capture(query_arena, query, &database, NULL);
    opt_check_occurrences(report, "explain: cte shown as derived", result.output_text, "(derived)", 1, 0);
    opt_check_occurrences(report, "explain: cte not reported as missing", result.output_text, "NOT FOUND", 0, 0);
    arena_clear(query_arena);
  }

  arena_release(query_arena);
}

//~ tec: ordering, sizing and small inputs

typedef struct Opt_OrderContext Opt_OrderContext;
struct Opt_OrderContext
{
  F64* values;
};

internal B32
opt_order_less(void* context, U64 a, U64 b)
{
  Opt_OrderContext* order_context = (Opt_OrderContext*)context;
  return order_context->values[a] < order_context->values[b];
}

// tec: the heap has to return exactly what a stable sort would put first, including the order of equal keys
internal void
opt_run_order_unit(Arena* arena, Bench_Report* report)
{
  U64 count = 1000;
  Temp scratch = scratch_begin(&arena, 1);

  Opt_OrderContext context = {0};
  context.values = push_array(scratch.arena, F64, count);
  Bench_Rng rng = {0x0DDBA11ULL};
  for (U64 index = 0; index < count; index += 1)
  {
    context.values[index] = (F64)(bench_rng_next(&rng) % 50);
  }

  U64* full = push_array(scratch.arena, U64, count);
  U64* buffer = push_array(scratch.arena, U64, count);
  for (U64 index = 0; index < count; index += 1)
  {
    full[index] = index;
  }
  qe_order_merge_sort(full, buffer, count, opt_order_less, &context);

  B32 sorted = 1;
  B32 stable = 1;
  for (U64 index = 1; index < count; index += 1)
  {
    F64 previous_value = context.values[full[index - 1]];
    F64 value = context.values[full[index]];
    if (previous_value > value)
    {
      sorted = 0;
    }
    if (previous_value == value && full[index - 1] > full[index])
    {
      stable = 0;
    }
  }
  opt_check_unit(report, "unit: merge sort orders the keys", sorted);
  opt_check_unit(report, "unit: merge sort keeps equal keys in input order", stable);

  U64 keeps[] = { 1, 7, 100, 999, 1000 };
  for (U64 keep_index = 0; keep_index < ArrayCount(keeps); keep_index += 1)
  {
    U64 keep = keeps[keep_index];
    U64* selected = push_array(scratch.arena, U64, count);
    for (U64 index = 0; index < count; index += 1)
    {
      selected[index] = index;
    }
    U64 kept = qe_order_select_top(selected, buffer, count, keep, opt_order_less, &context);

    B32 same = (kept == keep);
    for (U64 index = 0; index < keep && same; index += 1)
    {
      if (selected[index] != full[index])
      {
        same = 0;
      }
    }
    char label[96];
    snprintf(label, sizeof(label), "unit: top %llu of %llu matches the stable sort", keep, count);
    opt_check_unit(report, label, same);
  }

  scratch_end(scratch);
}

internal void
opt_run_capacity_unit(Bench_Report* report)
{
  U64 hard_max = 100000000ull;

  U64 default_capacity = qe_join_output_capacity(3000, 3000, NULL, hard_max);
  opt_check_unit(report, "unit: no hint keeps the probe plus build guess", default_capacity == 6000);

  QE_JoinHints fan_out = {0};
  fan_out.output_rows = 500000;
  U64 hinted_capacity = qe_join_output_capacity(3000, 3000, &fan_out, hard_max);
  opt_check_unit(report, "unit: a fan out hint sizes the buffer above the estimate", hinted_capacity >= 500000 && hinted_capacity <= 700000);

  QE_JoinHints small_hint = {0};
  small_hint.output_rows = 10;
  U64 small_capacity = qe_join_output_capacity(3000, 3000, &small_hint, hard_max);
  opt_check_unit(report, "unit: a small hint never shrinks below the default guess", small_capacity == 6000);

  QE_JoinHints wild = {0};
  wild.output_rows = 1000000000000ull;
  U64 wild_capacity = qe_join_output_capacity(3000, 3000, &wild, hard_max);
  opt_check_unit(report, "unit: an absurd estimate is capped", wild_capacity == QE_JOIN_HINT_MAX_PAIRS);

  U64 device_capped = qe_join_output_capacity(3000, 3000, &fan_out, 1000);
  opt_check_unit(report, "unit: the device limit wins over every hint", device_capped == 1000);

  OPT_CostModel model = optimizer_cost_model_load();
  U64 sort_crossover = optimizer_sort_gpu_min_rows(&model);
  U64 aggregate_crossover = optimizer_aggregate_cpu_max_rows(&model);
  printf("  sort stays on the CPU below %llu rows, aggregates below %llu rows\n", sort_crossover, aggregate_crossover);
  opt_check_unit(report, "unit: the sort crossover is a real size", sort_crossover > 16 && sort_crossover < OPT_SMALL_INPUT_SEARCH_LIMIT);
  opt_check_unit(report, "unit: the aggregate crossover is a real size", aggregate_crossover > 16 && aggregate_crossover < OPT_SMALL_INPUT_SEARCH_LIMIT);
}

// tec: the planner only walks when it expects to find enough rows, so the short and empty cases are checked directly
internal void
opt_run_walk_unit(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  GDB_Table* table = gdb_database_find_table(fixture->database, str8_lit("ordered"));
  GDB_Column* id_column = gdb_table_find_column(table, str8_lit("id"));
  GDB_Index* index = gdb_table_find_index_on_column(table, id_column);

  Temp scratch = scratch_begin(&arena, 1);
  String8 sql = str8_lit("SELECT id FROM ordered WHERE value > 99990;");
  SQL_TokenizeResult tokens = sql_tokenize_from_text(scratch.arena, sql);
  SQL_Node* ast = sql_parse(scratch.arena, tokens.tokens, tokens.count, sql);
  IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
  IR_Node* where = ir_node_find_child(ir_query->execution_nodes, IR_NodeType_Where);

  U64 matching_rows = 0;
  U64 matching_checksum = 0;
  bench_run_gdb_query(fixture->database, sql, &matching_rows, &matching_checksum);

  QE_ScanResult everything = {0};
  qe_index_ordered_scan(scratch.arena, table, str8_lit(""), index, where->first, 0, table->row_count, &everything);
  opt_check_unit(report, "unit: index walk finds every matching row", everything.count == matching_rows && matching_rows > 0);

  B32 ascending = 1;
  for (U64 position = 1; position < everything.count; position += 1)
  {
    if (everything.indices[position - 1] >= everything.indices[position])
    {
      ascending = 0;
    }
  }
  opt_check_unit(report, "unit: index walk returns the rows in key order", ascending);

  QE_ScanResult limited = {0};
  qe_index_ordered_scan(scratch.arena, table, str8_lit(""), index, where->first, 0, 2, &limited);
  B32 limited_matches = limited.count == 2 && everything.count >= 2 && limited.indices[0] == everything.indices[0] && limited.indices[1] == everything.indices[1];
  opt_check_unit(report, "unit: index walk stops after the requested rows", limited_matches);

  QE_ScanResult descending = {0};
  qe_index_ordered_scan(scratch.arena, table, str8_lit(""), index, where->first, 1, everything.count, &descending);
  B32 reversed = descending.count == everything.count && everything.count > 0;
  for (U64 position = 0; position < descending.count && reversed; position += 1)
  {
    if (descending.indices[position] != everything.indices[everything.count - 1 - position])
    {
      reversed = 0;
    }
  }
  opt_check_unit(report, "unit: a descending walk is the ascending one reversed", reversed);

  String8 nothing_sql = str8_lit("SELECT id FROM ordered WHERE value > 100000000;");
  SQL_TokenizeResult nothing_tokens = sql_tokenize_from_text(scratch.arena, nothing_sql);
  SQL_Node* nothing_ast = sql_parse(scratch.arena, nothing_tokens.tokens, nothing_tokens.count, nothing_sql);
  IR_Query* nothing_query = ir_generate_from_ast(scratch.arena, nothing_ast);
  IR_Node* nothing_where = ir_node_find_child(nothing_query->execution_nodes, IR_NodeType_Where);
  QE_ScanResult nothing = {0};
  qe_index_ordered_scan(scratch.arena, table, str8_lit(""), index, nothing_where->first, 0, 5, &nothing);
  opt_check_unit(report, "unit: index walk over a filter that matches nothing is empty", nothing.count == 0);

  QE_ScanResult unfiltered = {0};
  qe_index_ordered_scan(scratch.arena, table, str8_lit(""), index, NULL, 0, 3, &unfiltered);
  B32 first_rows = unfiltered.count == 3 && unfiltered.indices[0] < unfiltered.indices[1] && unfiltered.indices[1] < unfiltered.indices[2];
  opt_check_unit(report, "unit: index walk without a filter returns the first rows", first_rows);

  scratch_end(scratch);
}

internal String8
opt_explain_text(Arena* arena, GDB_Database** database, char* sql)
{
  String8 query = push_str8f(arena, "EXPLAIN %s", sql);
  APP_QueryResult result = app_execute_query_capture(arena, query, database, NULL);
  return result.output_text;
}

internal void
opt_check_explain_count(Bench_Report* report, Arena* arena, GDB_Database** database, char* label, char* sql, char* needle, U64 expected)
{
  String8 text = opt_explain_text(arena, database, sql);
  opt_check_occurrences(report, label, text, needle, expected, 0);
}

// tec: the rows come back in the order the query asked for, so equal text with the optimizer on and off means the same order
internal void
opt_check_same_output(Bench_Report* report, Arena* arena, GDB_Database** database, char* label, char* sql)
{
  opt_set_optimizer(1);
  APP_QueryResult with_optimizer = app_execute_query_capture(arena, str8_cstring(sql), database, NULL);
  String8 on_text = push_str8_copy(arena, with_optimizer.output_text);

  opt_set_optimizer(0);
  APP_QueryResult without_optimizer = app_execute_query_capture(arena, str8_cstring(sql), database, NULL);
  String8 off_text = push_str8_copy(arena, without_optimizer.output_text);
  opt_set_optimizer(1);

  B32 parsed = !with_optimizer.had_parse_error && !without_optimizer.had_parse_error;
  B32 same = parsed && str8_match(on_text, off_text, 0);
  printf("  %-56s %s (%llu bytes)\n", label, same ? "OK" : "FAIL", on_text.size);
  if (!same)
  {
    bench_report_warn(report, "'%s' returned different rows or a different order with the optimizer on", label);
    printf("    on : %.*s\n    off: %.*s\n", str8_varg(str8_prefix(on_text, 400)), str8_varg(str8_prefix(off_text, 400)));
  }
}

internal void
opt_check_trips_lower(Bench_Report* report, GDB_Database* database, char* label, char* sql)
{
  String8 sql_text = str8_cstring(sql);

  opt_set_optimizer(1);
  U64 trips_on = opt_count_round_trips(database, sql_text);
  opt_set_optimizer(0);
  U64 trips_off = opt_count_round_trips(database, sql_text);
  opt_set_optimizer(1);

  B32 ok = trips_on < trips_off;
  printf("  %-56s %s (on %llu, off %llu round trips)\n", label, ok ? "OK" : "FAIL", trips_on, trips_off);
  if (!ok)
  {
    bench_report_warn(report, "'%s' expected fewer round trips with the optimizer on: on=%llu off=%llu", label, trips_on, trips_off);
  }
}

internal void
opt_run_operator_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  printf("\n########## ordering, sizing and small inputs ##########\n");
  bench_report_section(report, "ordering, sizing and small inputs");

  GDB_Database* database = fixture->database;
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));

  opt_run_order_unit(query_arena, report);
  opt_run_capacity_unit(report);
  opt_run_walk_unit(query_arena, report, fixture);

  //- tec: plan shape
  opt_check_explain_count(report, query_arena, &database, "topn: sort and limit become one node",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", "[PLAN_NodeType_TopN]", 1);
  opt_check_explain_count(report, query_arena, &database, "topn: no separate sort left",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", "[PLAN_NodeType_Sort]", 0);
  opt_check_explain_count(report, query_arena, &database, "topn: no separate limit left",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", "[PLAN_NodeType_Limit]", 0);
  opt_check_explain_count(report, query_arena, &database, "topn: heap without an index",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", "strategy=heap", 1);
  opt_check_explain_count(report, query_arena, &database, "topn: a limit past the heap bound stays a sort",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 100000;", "[PLAN_NodeType_TopN]", 0);
  opt_check_explain_count(report, query_arena, &database, "topn: order by without a limit stays a sort",
                          "SELECT id, value FROM big ORDER BY value DESC;", "[PLAN_NodeType_Sort]", 1);
  opt_check_explain_count(report, query_arena, &database, "index order: walks the index for a small limit",
                          "SELECT id, value FROM ordered ORDER BY id LIMIT 10;", "strategy=index walk", 1);
  opt_check_explain_count(report, query_arena, &database, "index order: walk with a filter on another column",
                          "SELECT id, value FROM ordered WHERE value > 50000 ORDER BY id LIMIT 10;", "strategy=index walk", 1);
  opt_check_explain_count(report, query_arena, &database, "index order: no index on the sort column, no walk",
                          "SELECT id, value FROM big ORDER BY id LIMIT 10;", "strategy=index walk", 0);
  opt_check_explain_count(report, query_arena, &database, "index order: a range scan already sorted",
                          "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY id;", "strategy=presorted", 1);
  opt_check_explain_count(report, query_arena, &database, "index order: a different sort column is not presorted",
                          "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY value;", "strategy=presorted", 0);
  opt_check_explain_count(report, query_arena, &database, "index order: two sort keys are not presorted",
                          "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY id, value;", "strategy=presorted", 0);

  //- tec: the switches
  settings_load_from_string(str8_lit("QE_OPT_TOP_N: false"));
  opt_check_explain_count(report, query_arena, &database, "topn switch off: keeps the sort",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", "[PLAN_NodeType_Sort]", 1);
  opt_check_explain_count(report, query_arena, &database, "topn switch off: keeps the limit",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", "[PLAN_NodeType_Limit]", 1);
  settings_load_from_string(str8_lit("QE_OPT_TOP_N: true"));

  settings_load_from_string(str8_lit("QE_OPT_SORT_ELIMINATION: false"));
  opt_check_explain_count(report, query_arena, &database, "elimination switch off: the range scan is sorted again",
                          "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY id;", "strategy=presorted", 0);
  settings_load_from_string(str8_lit("QE_OPT_SORT_ELIMINATION: true"));

  opt_set_optimizer(0);
  opt_check_explain_count(report, query_arena, &database, "optimizer off: plain sort and limit",
                          "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;", "[PLAN_NodeType_TopN]", 0);
  opt_set_optimizer(1);

  //- tec: the order of the rows, not only which rows
  printf("\n---- row order, optimizer on against off ----\n");
  opt_check_same_output(report, query_arena, &database, "heap: numeric key descending with a tie break",
                        "SELECT id, value FROM big ORDER BY value DESC, id LIMIT 10;");
  opt_check_same_output(report, query_arena, &database, "heap: string key descending",
                        "SELECT id, name FROM small ORDER BY name DESC, id LIMIT 4;");
  opt_check_same_output(report, query_arena, &database, "heap: offset",
                        "SELECT id FROM big ORDER BY id LIMIT 5 OFFSET 20;");
  opt_check_same_output(report, query_arena, &database, "heap: over a join",
                        "SELECT b.id, s.name FROM big b JOIN small s ON b.name = s.name ORDER BY b.id DESC, s.id LIMIT 5;");
  opt_check_same_output(report, query_arena, &database, "heap: over an aggregate",
                        "SELECT name, COUNT(*) AS c FROM big GROUP BY name ORDER BY c DESC, name LIMIT 5;");
  opt_check_same_output(report, query_arena, &database, "sort: small numeric input on the cpu",
                        "SELECT id, value FROM tiny ORDER BY value DESC, id;");
  opt_check_same_output(report, query_arena, &database, "sort: mid sized input on the cpu",
                        "SELECT id, value FROM mid ORDER BY value, id;");
  opt_check_same_output(report, query_arena, &database, "sort: aggregate output, merge sort",
                        "SELECT name, COUNT(*) AS c FROM big GROUP BY name ORDER BY c, name;");
  opt_check_same_output(report, query_arena, &database, "presorted: range scan ascending",
                        "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY id;");
  opt_check_same_output(report, query_arena, &database, "presorted: range scan descending",
                        "SELECT id, value FROM ordered WHERE id > 29900 ORDER BY id DESC;");
  opt_check_same_output(report, query_arena, &database, "index walk: descending, no filter",
                        "SELECT id, value FROM ordered ORDER BY id DESC LIMIT 10;");
  opt_check_same_output(report, query_arena, &database, "index walk: ascending with a filter and an offset",
                        "SELECT id, value FROM ordered WHERE value > 50000 ORDER BY id LIMIT 10 OFFSET 5;");
  opt_check_same_output(report, query_arena, &database, "index walk: fewer matches than the limit",
                        "SELECT id, value FROM ordered WHERE value > 99990 ORDER BY id LIMIT 500;");
  opt_check_same_output(report, query_arena, &database, "index walk: nothing matches",
                        "SELECT id FROM ordered WHERE value > 100000000 ORDER BY id LIMIT 5;");
  arena_clear(query_arena);

  //- tec: fewer GPU round trips
  printf("\n---- round trips, optimizer on against off ----\n");
  opt_check_trips_lower(report, database, "topn: a heap needs no sort dispatch",
                        "SELECT id, value FROM big ORDER BY value DESC LIMIT 10;");
  opt_check_trips_lower(report, database, "index walk: no scan and no sort",
                        "SELECT id, value FROM ordered ORDER BY id DESC LIMIT 10;");
  opt_check_trips_lower(report, database, "cpu aggregate: a tiny group by",
                        "SELECT name, COUNT(*) AS c, SUM(value) AS s FROM small GROUP BY name;");
  opt_check_trips_lower(report, database, "cpu sort: a small input",
                        "SELECT id, value FROM tiny ORDER BY value DESC, id;");
  opt_check_trips_lower(report, database, "join sizing: fan out needs one probe pass",
                        "SELECT COUNT(*) AS n FROM mid a JOIN mid c ON a.name = c.name;");

  //- tec: the group estimate sizes the hash table, so a group by with many groups does not retry
  {
    String8 many_groups = str8_lit("SELECT id, COUNT(*) AS c FROM ordered GROUP BY id;");
    opt_set_optimizer(1);
    U64 trips_on = opt_count_round_trips(database, many_groups);
    opt_set_optimizer(0);
    U64 trips_off = opt_count_round_trips(database, many_groups);
    opt_set_optimizer(1);
    B32 ok = trips_on <= trips_off;
    printf("  %-56s %s (on %llu, off %llu round trips)\n", "aggregate sizing: many groups do not add passes", ok ? "OK" : "FAIL", trips_on, trips_off);
    if (!ok)
    {
      bench_report_warn(report, "aggregate sizing: on=%llu off=%llu round trips", trips_on, trips_off);
    }
  }

  arena_release(query_arena);
}

//~ tec: semi and anti joins

internal void
opt_check_parity_query(Bench_Report* report, Opt_Fixture* fixture, char* label, char* sql)
{
  String8 sql_text = str8_cstring(sql);
  String8 label_text = str8_cstring(label);

  U64 gdb_rows = 0;
  U64 gdb_checksum = 0;
  bench_run_gdb_query(fixture->database, sql_text, &gdb_rows, &gdb_checksum);
  U64 sqlite_rows = 0;
  U64 sqlite_checksum = 0;
  bench_run_sqlite_query(fixture->sqlite_db, sql_text, &sqlite_rows, &sqlite_checksum);
  U64 duckdb_rows = 0;
  U64 duckdb_checksum = 0;
  bench_run_duckdb_query(fixture->duckdb_conn, sql_text, &duckdb_rows, &duckdb_checksum);

  printf("  %-56s rows=%llu\n", label, gdb_rows);
  bench_check_match(report, label_text, "gdb", gdb_rows, gdb_checksum, "sqlite", sqlite_rows, sqlite_checksum);
  bench_check_match(report, label_text, "gdb", gdb_rows, gdb_checksum, "duckdb", duckdb_rows, duckdb_checksum);
}

// tec: makes a CPU scan expensive enough that a long IN list is answered by a semi join
internal void
opt_force_semi_join(B32 forced)
{
  if (forced)
  {
    settings_load_from_string(str8_lit("QE_COST_CPU_SCAN_PER_ROW_US: 1.0"));
  }
  else
  {
    settings_load_from_string(str8_lit("QE_COST_CPU_SCAN_PER_ROW_US: 0.0215"));
  }
}

internal void
opt_check_refused(Bench_Report* report, Arena* arena, Opt_Fixture* fixture, char* label, char* sql)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8 sql_text = str8_cstring(sql);
  SQL_TokenizeResult tokens = sql_tokenize_from_text(scratch.arena, sql_text);
  SQL_Node* ast = sql_parse(scratch.arena, tokens.tokens, tokens.count, sql_text);
  IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
  U64 temp_mark = fixture->database->temp_table_count;
  PLAN_ExecResult result = plan_run_select(scratch.arena, fixture->database, ir_query->execution_nodes, NULL);
  gdb_database_release_temp_tables_from(fixture->database, temp_mark);

  B32 refused = !result.supported;
  printf("  %-56s %s\n", label, refused ? "OK" : "FAIL");
  if (!refused)
  {
    bench_report_warn(report, "'%s' should have been refused", label);
  }
  scratch_end(scratch);
}

internal void
opt_run_semi_unit(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  Temp scratch = scratch_begin(&arena, 1);

  // tec: 5 left rows, probe 1 matched once, probe 3 matched twice, probe 4 unmatched
  U64 left_values[5] = { 10, 11, 12, 13, 14 };
  U64* left_indices[1] = { left_values };
  GDB_Table* left_tables[1] = { gdb_database_find_table(fixture->database, str8_lit("tiny")) };
  String8 left_aliases[1] = { str8_lit("t") };
  PLAN_RowSet left = {0};
  left.tables = left_tables;
  left.aliases = left_aliases;
  left.table_count = 1;
  left.row_indices = left_indices;
  left.count = 5;

  U32 pairs[4 * 4] =
  {
    1, 0, 7, 0,
    3, 0, 8, 0,
    3, 0, 9, 0,
    4, 0, max_U32, max_U32,
  };
  PLAN_RowSet matched = qe_join_filter_left_rows(scratch.arena, &left, pairs, 4, 1);
  B32 matched_ok = matched.count == 2 && matched.row_indices[0][0] == 11 && matched.row_indices[0][1] == 13;
  opt_check_unit(report, "unit: a semi join keeps each matched left row once, in order", matched_ok);

  PLAN_RowSet unmatched = qe_join_filter_left_rows(scratch.arena, &left, pairs, 4, 0);
  B32 unmatched_ok = unmatched.count == 1 && unmatched.row_indices[0][0] == 14;
  opt_check_unit(report, "unit: an anti join keeps the left rows with no partner", unmatched_ok);

  GDB_Table* mid = gdb_database_find_table(fixture->database, str8_lit("mid"));
  GDB_Column* name_column = gdb_table_find_column(mid, str8_lit("name"));
  U64 distinct_names = 0;
  U64* name_rows = qe_distinct_key_rows(scratch.arena, name_column, NULL, mid->row_count, &distinct_names);

  B32 names_distinct = distinct_names > 1 && distinct_names < mid->row_count;
  for (U64 first = 0; first < distinct_names && names_distinct; first += 1)
  {
    for (U64 second = first + 1; second < distinct_names; second += 1)
    {
      if (qe_key_equal(name_column, name_rows[first], name_rows[second]))
      {
        names_distinct = 0;
      }
    }
  }
  B32 names_covered = 1;
  for (U64 row = 0; row < mid->row_count && names_covered; row += 1)
  {
    B32 found = 0;
    for (U64 kept = 0; kept < distinct_names && !found; kept += 1)
    {
      found = qe_key_equal(name_column, name_rows[kept], row);
    }
    names_covered = found;
  }
  opt_check_unit(report, "unit: distinct string keys are unique and cover every row", names_distinct && names_covered);

  GDB_Column* id_column = gdb_table_find_column(mid, str8_lit("id"));
  U64 distinct_ids = 0;
  qe_distinct_key_rows(scratch.arena, id_column, NULL, mid->row_count, &distinct_ids);
  opt_check_unit(report, "unit: a unique numeric key keeps every row", distinct_ids == mid->row_count);

  U64 subset[4] = { 5, 6, 5, 7 };
  U64 distinct_subset = 0;
  U64* subset_rows = qe_distinct_key_rows(scratch.arena, id_column, subset, 4, &distinct_subset);
  opt_check_unit(report, "unit: distinct keys over a row list drop the repeated row", distinct_subset == 3 && subset_rows[0] == 5 && subset_rows[1] == 6 && subset_rows[2] == 7);

  OPT_CostModel model = optimizer_cost_model_load();
  opt_check_unit(report, "unit: a list a GPU scan handles stays a list", !optimizer_semi_pays_off(&model, 10, 10000000.0));
  opt_check_unit(report, "unit: a long list over a tiny table stays a list", !optimizer_semi_pays_off(&model, 100, 50.0));
  opt_check_unit(report, "unit: a long list over a large table becomes a semi join", optimizer_semi_pays_off(&model, 100, 10000000.0));

  scratch_end(scratch);
}

internal void
opt_run_semi_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture)
{
  printf("\n########## semi and anti joins ##########\n");
  bench_report_section(report, "semi and anti joins");

  GDB_Database* database = fixture->database;
  Arena* query_arena = arena_alloc(.reserve_size = MB(64), .commit_size = MB(4));

  opt_run_semi_unit(query_arena, report, fixture);

  //- tec: EXISTS with an equality between the two tables
  printf("\n---- correlated EXISTS, parity with sqlite and duckdb ----\n");
  opt_check_parity_query(report, fixture, "exists: string key",
                         "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);");
  opt_check_parity_query(report, fixture, "not exists: string key",
                         "SELECT m.id FROM mid m WHERE NOT EXISTS (SELECT s.id FROM small s WHERE s.name = m.name AND s.id < 4);");
  opt_check_parity_query(report, fixture, "exists: filter on the inner table",
                         "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name AND s.id < 4);");
  opt_check_parity_query(report, fixture, "exists: numeric key with a filter on both sides",
                         "SELECT b.id FROM big b WHERE b.id < 5000 AND EXISTS (SELECT m.id FROM mid m WHERE m.id = b.id AND m.value > 50000);");
  opt_check_parity_query(report, fixture, "exists: inner side has many rows per key",
                         "SELECT s.id FROM small s WHERE EXISTS (SELECT b.id FROM big b WHERE b.name = s.name);");
  opt_check_parity_query(report, fixture, "not exists: inner side has many rows per key",
                         "SELECT s.id FROM small s WHERE NOT EXISTS (SELECT b.id FROM big b WHERE b.name = s.name AND b.id < 100);");
  opt_check_parity_query(report, fixture, "exists: over a join",
                         "SELECT b.id, s.name FROM big b JOIN small s ON b.name = s.name WHERE EXISTS (SELECT m.id FROM mid m WHERE m.id = b.id);");
  opt_check_parity_query(report, fixture, "exists: under an aggregate",
                         "SELECT COUNT(*) AS n FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);");
  opt_check_parity_query(report, fixture, "exists: qualified names, unqualified select list",
                         "SELECT id FROM mid WHERE EXISTS (SELECT id FROM tiny WHERE tiny.id = mid.id);");
  opt_check_parity_query(report, fixture, "exists and not exists together",
                         "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name) AND NOT EXISTS (SELECT t.id FROM tiny t WHERE t.id = m.id);");
  opt_check_parity_query(report, fixture, "exists: the same table on both sides",
                         "SELECT a.id FROM mid a WHERE EXISTS (SELECT c.id FROM mid c WHERE c.name = a.name AND c.id > 2990);");
  opt_check_parity_query(report, fixture, "exists: nothing on the inner side qualifies",
                         "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name AND s.id < 0);");
  opt_check_parity_query(report, fixture, "not exists: nothing on the inner side qualifies",
                         "SELECT m.id FROM mid m WHERE NOT EXISTS (SELECT s.id FROM small s WHERE s.name = m.name AND s.id < 0);");
  opt_check_parity_query(report, fixture, "exists: with order by and limit",
                         "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name) ORDER BY m.id DESC LIMIT 5;");

  //- tec: IN and NOT IN over a subquery, forced to the semi join by making the CPU scan expensive
  opt_force_semi_join(1);
  printf("\n---- IN and NOT IN over a subquery, parity with sqlite and duckdb ----\n");
  opt_check_parity_query(report, fixture, "in subquery: numeric, long list",
                         "SELECT b.id FROM big b WHERE b.id IN (SELECT m.id FROM mid m WHERE m.value > 30000);");
  opt_check_parity_query(report, fixture, "not in subquery: numeric, long list",
                         "SELECT b.id FROM big b WHERE b.id NOT IN (SELECT m.id FROM mid m WHERE m.value > 30000);");
  opt_check_parity_query(report, fixture, "in subquery: string, repeated values",
                         "SELECT id FROM mid WHERE name IN (SELECT name FROM big WHERE id < 100 AND id > 40);");
  opt_check_parity_query(report, fixture, "not in subquery: string, repeated values",
                         "SELECT id FROM mid WHERE name NOT IN (SELECT name FROM big WHERE id < 100 AND id > 40);");
  opt_check_parity_query(report, fixture, "in subquery: with another condition",
                         "SELECT b.id FROM big b WHERE b.value > 50000 AND b.id IN (SELECT m.id FROM mid m WHERE m.id > 40);");
  opt_check_parity_query(report, fixture, "in subquery: under an aggregate",
                         "SELECT COUNT(*) AS n FROM big b WHERE b.id IN (SELECT m.id FROM mid m WHERE m.id > 40);");
  opt_check_parity_query(report, fixture, "in subquery: result under the GPU list limit",
                         "SELECT b.id FROM big b WHERE b.id IN (SELECT m.id FROM mid m WHERE m.id < 10);");
  opt_check_parity_query(report, fixture, "in subquery: empty result",
                         "SELECT b.id FROM big b WHERE b.id IN (SELECT m.id FROM mid m WHERE m.id < 0);");
  opt_check_parity_query(report, fixture, "not in subquery: empty result",
                         "SELECT COUNT(*) AS n FROM big b WHERE b.id NOT IN (SELECT m.id FROM mid m WHERE m.id < 0);");
  opt_check_parity_query(report, fixture, "in subquery: over a join",
                         "SELECT b.id, s.name FROM big b JOIN small s ON b.name = s.name WHERE b.id IN (SELECT m.id FROM mid m WHERE m.id > 40);");
  opt_check_parity_query(report, fixture, "in subquery: two of them",
                         "SELECT b.id FROM big b WHERE b.id IN (SELECT m.id FROM mid m WHERE m.id > 40) AND b.id NOT IN (SELECT m.id FROM mid m WHERE m.id > 2000);");

  //- tec: plan shape
  opt_check_explain_count(report, query_arena, &database, "explain: EXISTS is a semi join",
                          "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);", "[PLAN_NodeType_SemiJoin]", 1);
  opt_check_explain_count(report, query_arena, &database, "explain: NOT EXISTS is an anti join",
                          "SELECT m.id FROM mid m WHERE NOT EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);", "[PLAN_NodeType_AntiJoin]", 1);
  opt_check_explain_count(report, query_arena, &database, "explain: the inner filter sits under the semi join",
                          "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name AND s.id < 4);", "[PLAN_NodeType_Filter]", 1);
  opt_check_explain_count(report, query_arena, &database, "explain: the semi join carries an estimate",
                          "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);", "est_rows=", 4);
  {
    APP_QueryResult result = app_execute_query_capture(query_arena,
                                                       str8_lit("EXPLAIN ANALYZE SELECT b.id FROM big b WHERE b.id IN (SELECT m.id FROM mid m WHERE m.value > 30000);"),
                                                       &database, NULL);
    opt_check_occurrences(report, "analyze: a long IN list runs as a semi join", result.output_text, "[SemiJoin]", 1, 0);
    arena_clear(query_arena);
  }
  {
    APP_QueryResult result = app_execute_query_capture(query_arena,
                                                       str8_lit("EXPLAIN ANALYZE SELECT b.id FROM big b WHERE b.id NOT IN (SELECT m.id FROM mid m WHERE m.value > 30000);"),
                                                       &database, NULL);
    opt_check_occurrences(report, "analyze: a long NOT IN list runs as an anti join", result.output_text, "[AntiJoin]", 1, 0);
    arena_clear(query_arena);
  }
  opt_force_semi_join(0);
  {
    APP_QueryResult result = app_execute_query_capture(query_arena,
                                                       str8_lit("EXPLAIN ANALYZE SELECT m.id FROM mid m WHERE m.id IN (SELECT b.id FROM big b WHERE b.value > 30000);"),
                                                       &database, NULL);
    opt_check_occurrences(report, "analyze: a cheap CPU scan keeps the IN list", result.output_text, "[SemiJoin]", 0, 0);
    arena_clear(query_arena);
  }

  //- tec: the switches, and what is still refused
  settings_load_from_string(str8_lit("QE_OPT_SEMI_JOIN: false"));
  opt_check_explain_count(report, query_arena, &database, "semi switch off: no semi join in the plan",
                          "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);", "[PLAN_NodeType_SemiJoin]", 0);
  opt_check_refused(report, query_arena, fixture, "semi switch off: correlated EXISTS is refused again",
                    "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);");
  settings_load_from_string(str8_lit("QE_OPT_SEMI_JOIN: true"));

  opt_check_refused(report, query_arena, fixture, "correlated EXISTS with an inequality is still refused",
                    "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.id > m.id);");
  opt_check_refused(report, query_arena, fixture, "correlated EXISTS inside an OR is still refused",
                    "SELECT m.id FROM mid m WHERE m.id < 3 OR EXISTS (SELECT s.id FROM small s WHERE s.name = m.name);");
  opt_check_refused(report, query_arena, fixture, "correlated EXISTS with two correlations is still refused",
                    "SELECT m.id FROM mid m WHERE EXISTS (SELECT s.id FROM small s WHERE s.name = m.name AND s.id = m.id);");

  if (fixture->database->temp_table_count != 0)
  {
    printf("  temp tables leaked: %llu FAIL\n", fixture->database->temp_table_count);
    bench_report_warn(report, "%llu temp table(s) still registered after the semi join suite", fixture->database->temp_table_count);
  }

  arena_release(query_arena);
}

//~ tec: fused GPU round trips

// tec: one column of U32 keys, key = row % key_modulus
internal GDB_Table*
opt_build_key_table(GDB_Database* database, char* name_prefix, U64 row_count, U64 key_modulus)
{
  Temp scratch = scratch_begin(0, 0);
  String8 name = push_str8f(scratch.arena, "%s_%llu_%llu", name_prefix, row_count, key_modulus);
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

typedef struct Opt_JoinRun Opt_JoinRun;
struct Opt_JoinRun
{
  U64 rows;
  U64 checksum;
  U64 round_trips;
};

// tec: the checksum ignores pair order, which the GPU does not promise
internal Opt_JoinRun
opt_run_key_join(Arena* arena, GDB_Table* probe_table, GDB_Table* build_table, QE_JoinHints* hints)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8 sql = push_str8f(scratch.arena, "SELECT p.k FROM %.*s p JOIN %.*s b ON p.k = b.k;", str8_varg(probe_table->name), str8_varg(build_table->name));
  SQL_TokenizeResult tokens = sql_tokenize_from_text(scratch.arena, sql);
  SQL_Node* ast = sql_parse(scratch.arena, tokens.tokens, tokens.count, sql);
  IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
  IR_Node* join_node = ir_node_find_child(ir_query->execution_nodes, IR_NodeType_Join);
  IR_Node* condition = join_node->last;

  U64* identity = push_array(scratch.arena, U64, Max(probe_table->row_count, (U64)1));
  for (U64 row = 0; row < probe_table->row_count; row += 1)
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
  left.count = probe_table->row_count;

  // tec: the first call primes kernels and pooled buffers, the second is the one counted
  Opt_JoinRun run = {0};
  for (U64 pass = 0; pass < 2; pass += 1)
  {
    Temp pass_scratch = scratch_begin(&scratch.arena, 1);
    U64 trips_before = g_vulkan_state->submit_count;
    PLAN_RowSet joined = qe_hash_join(pass_scratch.arena, &left, build_table, str8_lit("b"), NULL, 0, str8_lit("inner"), condition, hints, NULL);
    run.round_trips = g_vulkan_state->submit_count - trips_before;
    run.rows = joined.count;
    run.checksum = 0;
    for (U64 pair = 0; pair < joined.count; pair += 1)
    {
      U64 mixed = (joined.row_indices[0][pair] * 0x9E3779B97F4A7C15ull) ^ (joined.row_indices[1][pair] * 0xC2B2AE3D27D4EB4Full);
      run.checksum += mixed ^ (mixed >> 29);
    }
    scratch_end(pass_scratch);
  }

  scratch_end(scratch);
  return run;
}

internal void
opt_check_join_run_matches(Bench_Report* report, char* label, Opt_JoinRun* expected, Opt_JoinRun* actual, U64 expected_trips)
{
  B32 same_rows = expected->rows == actual->rows && expected->checksum == actual->checksum;
  B32 trips_ok = expected_trips == 0 || actual->round_trips == expected_trips;
  printf("  %-56s %s (rows %llu, round trips %llu)\n", label, (same_rows && trips_ok) ? "OK" : "FAIL", actual->rows, actual->round_trips);
  if (!same_rows)
  {
    bench_report_warn(report, "'%s' rows=%llu checksum=%llu, expected rows=%llu checksum=%llu", label, actual->rows, actual->checksum, expected->rows, expected->checksum);
  }
  if (!trips_ok)
  {
    bench_report_warn(report, "'%s' took %llu round trips, expected %llu", label, actual->round_trips, expected_trips);
  }
}

internal F64
opt_sum_round_trips(PLAN_Node* plan)
{
  if (!plan)
  {
    return 0.0;
  }
  return plan->est_round_trips + opt_sum_round_trips(plan->input) + opt_sum_round_trips(plan->input2);
}

// tec: how close the plan's own round trip estimate is to the submits the query really makes, over every case that has no CTE or derived table
internal void
opt_run_round_trip_estimate_check(Arena* arena, Bench_Report* report, Opt_Fixture* fixture, Opt_Case* cases, U64 count)
{
  printf("\n---- estimated against measured round trips ----\n");
  opt_set_optimizer(1);

  U64 compared = 0;
  U64 exact = 0;
  U64 within_one = 0;
  F64 total_error = 0.0;
  for (U64 index = 0; index < count; index += 1)
  {
    Opt_Case* test_case = &cases[index];
    B32 has_cte = str8_find_needle(test_case->sql, 0, str8_lit("WITH "), 0) < test_case->sql.size;
    B32 has_nested = str8_find_needle(test_case->sql, 0, str8_lit("(SELECT"), 0) < test_case->sql.size;
    if (has_cte || has_nested)
    {
      continue;
    }

    Temp scratch = scratch_begin(&arena, 1);
    SQL_TokenizeResult tokens = sql_tokenize_from_text(scratch.arena, test_case->sql);
    SQL_Node* ast = sql_parse(scratch.arena, tokens.tokens, tokens.count, test_case->sql);
    IR_Query* ir_query = ir_generate_from_ast(scratch.arena, ast);
    OPT_SourceEstimates inherited = {0};
    PLAN_Node* plan = plan_build_for_explain(scratch.arena, fixture->database, ir_query->execution_nodes, &inherited);
    optimizer_annotate_plan(scratch.arena, plan);
    F64 estimated = opt_sum_round_trips(plan);
    scratch_end(scratch);

    U64 measured = opt_count_round_trips(fixture->database, test_case->sql);
    F64 error = estimated - (F64)measured;
    if (error < 0.0)
    {
      error = -error;
    }
    compared += 1;
    total_error += error;
    if (error < 0.5)
    {
      exact += 1;
    }
    if (error < 1.5)
    {
      within_one += 1;
    }
    if (error >= 1.5)
    {
      printf("  %-56.*s estimated %.0f, measured %llu\n", str8_varg(test_case->label), estimated, measured);
    }
  }

  F64 mean_error = compared > 0 ? total_error / (F64)compared : 0.0;
  printf("  %llu queries: %llu exact, %llu within one, mean error %.2f round trips\n", compared, exact, within_one, mean_error);
  bench_report_text(report, "round trip estimate: %llu queries, %llu exact, %llu within one, mean error %.2f", compared, exact, within_one, mean_error);
  B32 ok = mean_error < 0.75;
  printf("  %-56s %s\n", "round trip estimates track the measured submits", ok ? "OK" : "FAIL");
  if (!ok)
  {
    bench_report_warn(report, "round trip estimate mean error %.2f", mean_error);
  }
}

internal void
opt_run_fusion_suite(Arena* arena, Bench_Report* report, Opt_Fixture* fixture, Opt_Case* cases, U64 case_count)
{
  printf("\n########## fused GPU round trips ##########\n");
  bench_report_section(report, "fused GPU round trips");

  // tec: bucket counts on both sides of the 256 entry block the prefix sum works in, and past 65536 where the block totals need a chunked scan
  U64 build_sizes[] = { 1, 2, 255, 256, 257, 4096, 70000, 300000 };
  for (U64 index = 0; index < ArrayCount(build_sizes); index += 1)
  {
    U64 build_rows = build_sizes[index];
    GDB_Table* build_table = opt_build_key_table(fixture->database, "fusion_build", build_rows, build_rows);
    GDB_Table* probe_table = opt_build_key_table(fixture->database, "fusion_probe", 1000, build_rows + 3);

    QE_JoinHints plain = {0};
    QE_JoinHints fused = {0};
    fused.fuse_round_trips = 1;
    Opt_JoinRun expected = opt_run_key_join(arena, probe_table, build_table, &plain);
    Opt_JoinRun actual = opt_run_key_join(arena, probe_table, build_table, &fused);

    char label[96];
    snprintf(label, sizeof(label), "fused join, %llu build rows, prefix sum on the GPU", build_rows);
    opt_check_join_run_matches(report, label, &expected, &actual, 2);
    B32 fewer = actual.round_trips < expected.round_trips;
    if (!fewer)
    {
      bench_report_warn(report, "'%s' did not save a round trip: %llu against %llu", label, actual.round_trips, expected.round_trips);
    }
  }

  // tec: many build rows per key, so buckets hold long lists and one probe row matches several pairs
  {
    GDB_Table* build_table = opt_build_key_table(fixture->database, "fusion_dup_build", 5000, 50);
    GDB_Table* probe_table = opt_build_key_table(fixture->database, "fusion_dup_probe", 100, 60);
    QE_JoinHints plain = {0};
    QE_JoinHints fused = {0};
    fused.fuse_round_trips = 1;
    Opt_JoinRun expected = opt_run_key_join(arena, probe_table, build_table, &plain);
    Opt_JoinRun actual = opt_run_key_join(arena, probe_table, build_table, &fused);
    opt_check_join_run_matches(report, "fused join, duplicate keys", &expected, &actual, 0);

    //- tec: the pairs ride along with the probe when the estimate is right, and a second download covers a wrong one
    QE_JoinHints exact = fused;
    exact.output_rows = expected.rows;
    Opt_JoinRun with_exact_hint = opt_run_key_join(arena, probe_table, build_table, &exact);
    opt_check_join_run_matches(report, "fused join, exact output estimate is one submit", &expected, &with_exact_hint, 1);

    QE_JoinHints low = fused;
    low.output_rows = 10;
    Opt_JoinRun with_low_hint = opt_run_key_join(arena, probe_table, build_table, &low);
    opt_check_join_run_matches(report, "fused join, estimate far too low still correct", &expected, &with_low_hint, 0);

    QE_JoinHints high = fused;
    high.output_rows = expected.rows * 20;
    Opt_JoinRun with_high_hint = opt_run_key_join(arena, probe_table, build_table, &high);
    opt_check_join_run_matches(report, "fused join, estimate far too high still correct", &expected, &with_high_hint, 1);
  }

  // tec: a probe side that matches nothing and a build side that is empty
  {
    GDB_Table* build_table = opt_build_key_table(fixture->database, "fusion_none_build", 100, 100);
    GDB_Table* probe_table = opt_build_key_table(fixture->database, "fusion_none_probe", 100, 100000);
    GDB_Table* shifted = opt_build_key_table(fixture->database, "fusion_shift", 50, 100000);
    (void)probe_table;
    QE_JoinHints plain = {0};
    QE_JoinHints fused = {0};
    fused.fuse_round_trips = 1;
    Opt_JoinRun expected = opt_run_key_join(arena, shifted, build_table, &plain);
    Opt_JoinRun actual = opt_run_key_join(arena, shifted, build_table, &fused);
    opt_check_join_run_matches(report, "fused join, small build with partial matches", &expected, &actual, 0);
  }

  // tec: the group by fuses its scatter into the reduce
  printf("\n---- round trips, fusion on against off ----\n");
  opt_set_optimizer(1);
  {
    String8 sql = str8_lit("SELECT name, COUNT(*) AS c, SUM(value) AS s FROM big GROUP BY name;");
    settings_load_from_string(str8_lit("QE_OPT_FUSE: false"));
    U64 trips_off = opt_count_round_trips(fixture->database, sql);
    settings_load_from_string(str8_lit("QE_OPT_FUSE: true"));
    U64 trips_on = opt_count_round_trips(fixture->database, sql);
    B32 ok = trips_on + 1 == trips_off;
    printf("  %-56s %s (on %llu, off %llu round trips)\n", "aggregate: scatter and reduce share a submit", ok ? "OK" : "FAIL", trips_on, trips_off);
    if (!ok)
    {
      bench_report_warn(report, "aggregate fusion: on=%llu off=%llu round trips", trips_on, trips_off);
    }
  }
  {
    String8 sql = str8_lit("SELECT id FROM big WHERE value > 99500;");
    settings_load_from_string(str8_lit("QE_OPT_FUSE: false"));
    U64 trips_off = opt_count_round_trips(fixture->database, sql);
    settings_load_from_string(str8_lit("QE_OPT_FUSE: true"));
    U64 trips_on = opt_count_round_trips(fixture->database, sql);
    B32 ok = trips_on < trips_off;
    printf("  %-56s %s (on %llu, off %llu round trips)\n", "scan: a selective result rides along with the count", ok ? "OK" : "FAIL", trips_on, trips_off);
    if (!ok)
    {
      bench_report_warn(report, "scan fusion: on=%llu off=%llu round trips", trips_on, trips_off);
    }
  }
  {
    String8 sql = str8_lit("SELECT s.name, COUNT(*) AS c, SUM(b.value) AS total FROM small s JOIN big b ON s.name = b.name GROUP BY s.name;");
    settings_load_from_string(str8_lit("QE_OPT_FUSE: false"));
    U64 trips_off = opt_count_round_trips(fixture->database, sql);
    settings_load_from_string(str8_lit("QE_OPT_FUSE: true"));
    U64 trips_on = opt_count_round_trips(fixture->database, sql);
    B32 ok = trips_on <= 3 && trips_on < trips_off;
    printf("  %-56s %s (on %llu, off %llu round trips)\n", "handoff join and group by: at most 3 round trips", ok ? "OK" : "FAIL", trips_on, trips_off);
    if (!ok)
    {
      bench_report_warn(report, "handoff shape: on=%llu off=%llu round trips", trips_on, trips_off);
    }
  }
  settings_load_from_string(str8_lit("QE_OPT_FUSE: true"));

  opt_run_round_trip_estimate_check(arena, report, fixture, cases, case_count);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  // tec: keeps progress lines visible when a query crashes the process
  setvbuf(stdout, NULL, _IONBF, 0);
  
  log_alloc();
  g_query_exec_mutex = os_mutex_alloc();
  gdb_init();
  gpu_init();
  
  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql optimizer baseline");

  // tec: OPT_SETTINGS holds settings lines to apply before anything runs, for isolating one switch
  char* extra_settings = getenv("OPT_SETTINGS");
  if (extra_settings)
  {
    settings_load_from_string(str8_cstring(extra_settings));
  }

  Opt_Fixture* fixture = opt_fixture_create(arena);

  Opt_Case cases[OPT_CASE_MAX];
  U64 case_count = opt_build_cases(cases);
  
  opt_run_parity_suite(arena, report, fixture, cases, case_count);
  opt_run_ab_suite(arena, report, fixture, cases, case_count);
  opt_run_strategy_suite(arena, report, fixture);
  opt_run_join_order_suite(arena, report, fixture);
  opt_run_plan_text_suite(arena, report, fixture);
  opt_run_operator_suite(arena, report, fixture);
  opt_run_semi_suite(arena, report, fixture);
  opt_run_fusion_suite(arena, report, fixture, cases, case_count);

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
