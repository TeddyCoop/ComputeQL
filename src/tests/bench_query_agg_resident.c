#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(1)
#define BENCH_TIMED_RUNS 2

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

#define AR_ROWS     500000
#define AR_GROUPS   40
#define AR_DIM_KEYS 100000
#define AR_PRODUCTS 1000

typedef struct AR_Row AR_Row;
struct AR_Row
{
  U32 id;
  U32 group;
  F64 whole;
  F64 decimal;
};

// tec: whole is whole numbers so its sums are exact in any order, decimal has 2 places and only feeds SUM/MIN/MAX
internal AR_Row
ar_make_row(Bench_Rng* rng, U32 id)
{
  AR_Row row = {0};
  row.id = id;
  row.group = (U32)(bench_rng_next(rng) % AR_GROUPS);
  row.whole = (F64)(bench_rng_next(rng) % 1001);
  row.decimal = (F64)(bench_rng_next(rng) % 10000) / 100.0;
  return row;
}

internal void
ar_gdb_add_row(GDB_Table* table, AR_Row* row)
{
  char name_buffer[16] = {0};
  snprintf(name_buffer, sizeof(name_buffer), "k%02u", row->group);
  String8 name = str8_cstring(name_buffer);
  U32 key = row->id % AR_DIM_KEYS;
  void* row_data[6] = { &row->id, &row->group, &name, &row->whole, &row->decimal, &key };
  gdb_table_add_row(table, row_data, NULL);
}

internal void
ar_sqlite_run(sqlite3* db, String8 sql)
{
  Temp scratch = scratch_begin(0, 0);
  U8* text = push_array(scratch.arena, U8, sql.size + 1);
  MemoryCopy(text, sql.str, sql.size);
  char* err = NULL;
  if (sqlite3_exec(db, (const char*)text, NULL, NULL, &err) != SQLITE_OK)
  {
    log_error("sqlite3_exec failed: %s", err ? err : "unknown error");
    sqlite3_free(err);
  }
  scratch_end(scratch);
}

internal void
ar_sqlite_add_row(sqlite3* db, AR_Row* row)
{
  Temp scratch = scratch_begin(0, 0);
  ar_sqlite_run(db, push_str8f(scratch.arena, "INSERT INTO t VALUES (%u, %u, 'k%02u', %.4f, %.4f, %u);", row->id, row->group, row->group, row->whole, row->decimal, row->id % AR_DIM_KEYS));
  scratch_end(scratch);
}

// tec: small dimension tables, loaded into both engines
internal GDB_Table*
ar_gdb_make_table(GDB_Database* database, char* name, char** column_names, GDB_ColumnType* column_types, U64 column_count)
{
  GDB_Table* table = gdb_table_alloc(str8_cstring(name));
  for (U64 c = 0; c < column_count; c++)
  {
    gdb_table_add_column(table, gdb_column_schema_create(str8_cstring(column_names[c]), column_types[c]));
  }
  gdb_database_add_table(database, table);
  return table;
}

internal void
ar_add_dimensions(GDB_Database* database, sqlite3* sqlite_db)
{
  char* d_names[] = { "grp", "label", "weight", "cat" };
  GDB_ColumnType d_types[] = { GDB_ColumnType_U32, GDB_ColumnType_String8, GDB_ColumnType_F64, GDB_ColumnType_U32 };
  GDB_Table* d_table = ar_gdb_make_table(database, "d", d_names, d_types, 4);
  ar_sqlite_run(sqlite_db, str8_lit("CREATE TABLE d (grp INTEGER, label TEXT, weight REAL, cat INTEGER);"));

  char* e_names[] = { "cat", "factor" };
  GDB_ColumnType e_types[] = { GDB_ColumnType_U32, GDB_ColumnType_F64 };
  GDB_Table* e_table = ar_gdb_make_table(database, "e", e_names, e_types, 2);
  ar_sqlite_run(sqlite_db, str8_lit("CREATE TABLE e (cat INTEGER, factor REAL);"));

  char* d2_names[] = { "grp", "v" };
  GDB_ColumnType d2_types[] = { GDB_ColumnType_U32, GDB_ColumnType_F64 };
  GDB_Table* d2_table = ar_gdb_make_table(database, "d2", d2_names, d2_types, 2);
  ar_sqlite_run(sqlite_db, str8_lit("CREATE TABLE d2 (grp INTEGER, v REAL);"));

  Temp scratch = scratch_begin(0, 0);

  // tec: group AR_GROUPS itself has no row in d, so it is the unmatched side of the left join once it is inserted
  for (U32 g = 0; g < AR_GROUPS; g++)
  {
    char label_buffer[16] = {0};
    snprintf(label_buffer, sizeof(label_buffer), "L%02u", g);
    String8 label = str8_cstring(label_buffer);
    F64 weight = (F64)((g * 7) % 50);
    U32 cat = g % 5;
    void* d_row[4] = { &g, &label, &weight, &cat };
    gdb_table_add_row(d_table, d_row, NULL);
    ar_sqlite_run(sqlite_db, push_str8f(scratch.arena, "INSERT INTO d VALUES (%u, '%s', %.1f, %u);", g, label_buffer, weight, cat));

    for (U32 copy = 1; copy <= 2; copy++)
    {
      F64 v = (F64)copy;
      void* d2_row[2] = { &g, &v };
      gdb_table_add_row(d2_table, d2_row, NULL);
      ar_sqlite_run(sqlite_db, push_str8f(scratch.arena, "INSERT INTO d2 VALUES (%u, %.1f);", g, v));
    }
  }
  for (U32 cat = 0; cat < 5; cat++)
  {
    F64 factor = (F64)(10 * (cat + 1));
    void* e_row[2] = { &cat, &factor };
    gdb_table_add_row(e_table, e_row, NULL);
    ar_sqlite_run(sqlite_db, push_str8f(scratch.arena, "INSERT INTO e VALUES (%u, %.1f);", cat, factor));
  }

  // tec: dimensions big enough that a filter on them leaves the join's row order shuffled, which the small ones above do not
  char* u_names[] = { "uid", "seg", "age", "pid" };
  GDB_ColumnType u_types[] = { GDB_ColumnType_U32, GDB_ColumnType_String8, GDB_ColumnType_U32, GDB_ColumnType_U32 };
  GDB_Table* u_table = ar_gdb_make_table(database, "u", u_names, u_types, 4);
  ar_sqlite_run(sqlite_db, str8_lit("CREATE TABLE u (uid INTEGER, seg TEXT, age INTEGER, pid INTEGER);"));
  char* p_names[] = { "pid", "brand", "cost" };
  GDB_ColumnType p_types[] = { GDB_ColumnType_U32, GDB_ColumnType_String8, GDB_ColumnType_F64 };
  GDB_Table* p_table = ar_gdb_make_table(database, "p", p_names, p_types, 3);
  ar_sqlite_run(sqlite_db, str8_lit("CREATE TABLE p (pid INTEGER, brand TEXT, cost REAL);"));

  ar_sqlite_run(sqlite_db, str8_lit("BEGIN TRANSACTION;"));
  for (U32 uid = 0; uid < AR_DIM_KEYS; uid++)
  {
    char seg_buffer[16] = {0};
    snprintf(seg_buffer, sizeof(seg_buffer), "S%02u", (uid * 7919) % 8);
    String8 seg = str8_cstring(seg_buffer);
    U32 age = 18 + (uid * 31) % 63;
    U32 pid = uid % AR_PRODUCTS;
    void* u_row[4] = { &uid, &seg, &age, &pid };
    gdb_table_add_row(u_table, u_row, NULL);
    ar_sqlite_run(sqlite_db, push_str8f(scratch.arena, "INSERT INTO u VALUES (%u, '%s', %u, %u);", uid, seg_buffer, age, pid));
  }
  for (U32 pid = 0; pid < AR_PRODUCTS; pid++)
  {
    char brand_buffer[16] = {0};
    snprintf(brand_buffer, sizeof(brand_buffer), "B%02u", pid % 50);
    String8 brand = str8_cstring(brand_buffer);
    F64 cost = (F64)((pid * 37) % 400);
    void* p_row[3] = { &pid, &brand, &cost };
    gdb_table_add_row(p_table, p_row, NULL);
    ar_sqlite_run(sqlite_db, push_str8f(scratch.arena, "INSERT INTO p VALUES (%u, '%s', %.1f);", pid, brand_buffer, cost));
  }
  ar_sqlite_run(sqlite_db, str8_lit("COMMIT;"));

  scratch_end(scratch);
}

typedef struct AR_Query AR_Query;
struct AR_Query
{
  char* label;
  char* sql;
};

global AR_Query g_ar_queries[] =
{
  { "narrow args, numeric key",       "SELECT grp, COUNT(*), SUM(wv), MIN(wv), MAX(wv) FROM t GROUP BY grp;" },
  { "f64 args, numeric key (tile)",   "SELECT grp, SUM(dv), MIN(dv), MAX(dv) FROM t GROUP BY grp;" },
  { "dict string key",                "SELECT name, COUNT(*), SUM(wv) FROM t GROUP BY name;" },
  { "filtered, not the whole table",  "SELECT grp, SUM(dv) FROM t WHERE wv > 500 GROUP BY grp;" },
  { "global aggregate",               "SELECT COUNT(*), SUM(wv), MIN(dv), MAX(dv) FROM t;" },
  { "join: key from the right side",  "SELECT d.label, COUNT(*), SUM(t.wv) FROM t JOIN d ON t.grp = d.grp GROUP BY d.label;" },
  { "join: args from both sides",     "SELECT t.grp, SUM(d.weight), MIN(t.dv), MAX(t.dv) FROM t JOIN d ON t.grp = d.grp GROUP BY t.grp;" },
  { "join: count only",               "SELECT COUNT(*) FROM t JOIN d ON t.grp = d.grp;" },
  { "join: filtered right side",      "SELECT d.label, SUM(t.wv) FROM t JOIN d ON t.grp = d.grp WHERE d.weight > 20 GROUP BY d.label;" },
  { "join: filtered left side",       "SELECT d.label, SUM(t.dv) FROM t JOIN d ON t.grp = d.grp WHERE t.wv > 500 GROUP BY d.label;" },
  { "join: left join, unmatched rows","SELECT t.grp, COUNT(*), SUM(t.wv) FROM t LEFT JOIN d ON t.grp = d.grp GROUP BY t.grp;" },
  { "join: three tables",             "SELECT e.factor, COUNT(*), SUM(t.wv) FROM t JOIN d ON t.grp = d.grp JOIN e ON d.cat = e.cat GROUP BY e.factor;" },
  { "join: two rows per key",         "SELECT COUNT(*), SUM(t.wv), SUM(d2.v) FROM t JOIN d2 ON t.grp = d2.grp;" },
  { "join: three tables, filtered",   "SELECT d.label, e.factor, SUM(t.wv) FROM t JOIN d ON t.grp = d.grp JOIN e ON d.cat = e.cat WHERE d.weight > 15 AND e.factor > 20 GROUP BY d.label, e.factor;" },
  { "join: filtered on both sides",   "SELECT d.label, SUM(t.dv) FROM t JOIN d ON t.grp = d.grp WHERE t.wv > 500 AND d.weight > 20 GROUP BY d.label;" },
  { "join: chained, large filtered",  "SELECT u.seg, p.brand, SUM(t.wv) FROM t JOIN u ON t.k = u.uid JOIN p ON u.pid = p.pid WHERE u.age > 60 AND p.cost > 250 GROUP BY u.seg, p.brand;" },
  { "join: both sides filtered, large", "SELECT u.seg, SUM(t.wv), COUNT(*) FROM t JOIN u ON t.k = u.uid WHERE t.wv > 500 AND u.age > 60 GROUP BY u.seg;" },
  { "join: filtered left, three","SELECT e.factor, SUM(t.wv) FROM t JOIN d ON t.grp = d.grp JOIN e ON d.cat = e.cat WHERE t.wv > 500 AND e.factor > 10 GROUP BY e.factor;" },
};

global U64 g_ar_checks = 0;
global U64 g_ar_failures = 0;

// tec: runs every query on both engines, checks them against each other, and against the previous gdb answer when one is given
internal void
ar_run_phase(Bench_Report* report, GDB_Database* database, sqlite3* sqlite_db, char* phase, U64* gdb_checksums, B32 expect_same_as_previous)
{
  printf("\n=== %s ===\n", phase);
  for (U64 i = 0; i < ArrayCount(g_ar_queries); i++)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 sql = str8_cstring(g_ar_queries[i].sql);
    String8 label = push_str8f(scratch.arena, "%s: %s", phase, g_ar_queries[i].label);
    
    U64 gdb_rows = 0, gdb_checksum = 0;
    bench_run_gdb_query(database, sql, &gdb_rows, &gdb_checksum);
    U64 sqlite_rows = 0, sqlite_checksum = 0;
    bench_run_sqlite_query(sqlite_db, sql, &sqlite_rows, &sqlite_checksum);
    
    g_ar_checks += 1;
    B32 ok = gdb_rows == sqlite_rows && gdb_checksum == sqlite_checksum && gdb_rows > 0;
    if (expect_same_as_previous && gdb_checksums[i] != gdb_checksum)
    {
      ok = 0;
      bench_report_warn(report, "%.*s: gdb answer changed between two runs with no write in between", str8_varg(label));
    }
    gdb_checksums[i] = gdb_checksum;
    
    printf("  %-64.*s %s (rows %llu)\n", str8_varg(label), ok ? "OK" : "FAIL", gdb_rows);
    if (!ok)
    {
      g_ar_failures += 1;
      bench_report_warn(report, "FAIL %.*s: gdb rows=%llu checksum=%llu, sqlite rows=%llu checksum=%llu", str8_varg(label), gdb_rows, gdb_checksum, sqlite_rows, sqlite_checksum);
    }
    scratch_end(scratch);
  }
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  
  gdb_init();
  gpu_init();
  
  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  Bench_Report* report = bench_report_alloc(arena, "compute_ql GROUP BY over GPU resident columns correctness");
  
  GDB_Database* database = gdb_database_alloc(str8_lit("agg_resident_db"));
  gdb_add_database(database);
  GDB_Table* table = gdb_table_alloc(str8_lit("t"));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("id"), GDB_ColumnType_U32));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("grp"), GDB_ColumnType_U32));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("name"), GDB_ColumnType_String8));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("wv"), GDB_ColumnType_F64));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("dv"), GDB_ColumnType_F64));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("k"), GDB_ColumnType_U32));
  gdb_database_add_table(database, table);
  
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  ar_sqlite_run(sqlite_db, str8_lit("CREATE TABLE t (id INTEGER, grp INTEGER, name TEXT, wv REAL, dv REAL, k INTEGER);"));
  ar_sqlite_run(sqlite_db, str8_lit("BEGIN TRANSACTION;"));
  
  Bench_Rng rng = { 0xA66E5EEDULL };
  for (U32 i = 0; i < AR_ROWS; i++)
  {
    AR_Row row = ar_make_row(&rng, i + 1);
    ar_gdb_add_row(table, &row);
    ar_sqlite_add_row(sqlite_db, &row);
  }
  ar_sqlite_run(sqlite_db, str8_lit("COMMIT;"));
  ar_add_dimensions(database, sqlite_db);

  U64 checksums[ArrayCount(g_ar_queries)] = {0};
  
  bench_report_section(report, "resident column reuse");
  ar_run_phase(report, database, sqlite_db, "cold", checksums, 0);
  ar_run_phase(report, database, sqlite_db, "warm", checksums, 1);
  ar_run_phase(report, database, sqlite_db, "warm again", checksums, 1);
  
  bench_report_section(report, "after writes");
  {
    // tec: a new group plus a new column maximum and minimum, so a stale resident copy cannot give the right answer
    AR_Row rows[3] = {0};
    rows[0] = (AR_Row){ AR_ROWS + 1, AR_GROUPS, 1000.0, 999.99 };
    rows[1] = (AR_Row){ AR_ROWS + 2, 3, 0.0, 0.0 };
    rows[2] = (AR_Row){ AR_ROWS + 3, 7, 1000.0, 999.99 };
    ar_sqlite_run(sqlite_db, str8_lit("BEGIN TRANSACTION;"));
    for (U64 i = 0; i < ArrayCount(rows); i++)
    {
      ar_gdb_add_row(table, &rows[i]);
      ar_sqlite_add_row(sqlite_db, &rows[i]);
    }
    ar_sqlite_run(sqlite_db, str8_lit("COMMIT;"));
  }
  ar_run_phase(report, database, sqlite_db, "after insert", checksums, 0);
  ar_run_phase(report, database, sqlite_db, "after insert, warm", checksums, 1);
  
  {
    gdb_table_remove_row(table, 10);
    ar_sqlite_run(sqlite_db, str8_lit("DELETE FROM t WHERE id = 11;"));
  }
  ar_run_phase(report, database, sqlite_db, "after delete", checksums, 0);
  ar_run_phase(report, database, sqlite_db, "after delete, warm", checksums, 1);
  
  bench_report_section(report, "reduce paths");
  settings_load_from_string(str8_lit("QE_AGG_TILE_REDUCE: 0"));
  ar_run_phase(report, database, sqlite_db, "tile reduce off", checksums, 1);
  settings_load_from_string(str8_lit("QE_AGG_TILE_REDUCE: 1"));
  ar_run_phase(report, database, sqlite_db, "tile reduce on", checksums, 1);
  
  printf("\n%llu checks, %llu failed\n", g_ar_checks, g_ar_failures);
  bench_report_text(report, "%llu checks, %llu failed", g_ar_checks, g_ar_failures);
  
  sqlite3_close(sqlite_db);
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/agg_resident_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
