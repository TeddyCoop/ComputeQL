#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)
#define BENCH_TIMED_RUNS 3

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

//~ tec: every measure column holds whole numbers so sums are exact in F64 regardless of summation order,
// which keeps the cross engine checksums comparable at 10M rows. score is the one 2 decimal column and is only used with MIN/MAX

#define XL_NUM_USERS    1000000
#define XL_NUM_PRODUCTS 10000
#define XL_MAX_COLS     32

typedef enum XL_ColKind XL_ColKind;
enum XL_ColKind
{
  XL_ColKind_U32,
  XL_ColKind_F64,
  XL_ColKind_Str,
};

typedef enum XL_StrKind XL_StrKind;
enum XL_StrKind
{
  XL_StrKind_None,
  XL_StrKind_Country,
  XL_StrKind_Category,
  XL_StrKind_Status,
  XL_StrKind_Channel,
  XL_StrKind_Session,
  XL_StrKind_Segment,
  XL_StrKind_Brand,
};

typedef struct XL_Col XL_Col;
struct XL_Col
{
  char* name;
  XL_ColKind kind;
  U32 slot;
  XL_StrKind str_kind;
};

typedef struct XL_Row XL_Row;
struct XL_Row
{
  U32 u[12];
  F64 f[16];
  U32 s[6];
};

typedef void XL_GenFunc(Bench_Rng* rng, U64 index, XL_Row* row);

typedef struct XL_TableSpec XL_TableSpec;
struct XL_TableSpec
{
  char* name;
  XL_Col* cols;
  U64 col_count;
  XL_GenFunc* gen;
  U64 row_count;
  U64 seed;
};

global char* g_xl_status[]  = { "new", "paid", "shipped", "returned", "cancelled" };
global char* g_xl_channel[] = { "web", "ios", "android", "store", "phone", "partner", "email", "kiosk" };
global char* g_xl_segment[] = { "enterprise", "smb", "consumer", "education", "government", "nonprofit", "startup", "reseller" };

global XL_Col g_xl_event_cols[] =
{
  { "id",         XL_ColKind_U32, 0,  XL_StrKind_None },
  { "user_id",    XL_ColKind_U32, 1,  XL_StrKind_None },
  { "product_id", XL_ColKind_U32, 2,  XL_StrKind_None },
  { "region_id",  XL_ColKind_U32, 3,  XL_StrKind_None },
  { "store_id",   XL_ColKind_U32, 4,  XL_StrKind_None },
  { "ts",         XL_ColKind_U32, 5,  XL_StrKind_None },
  { "quantity",   XL_ColKind_U32, 6,  XL_StrKind_None },
  { "k1",         XL_ColKind_U32, 7,  XL_StrKind_None },
  { "k2",         XL_ColKind_U32, 8,  XL_StrKind_None },
  { "k3",         XL_ColKind_U32, 9,  XL_StrKind_None },
  { "k4",         XL_ColKind_U32, 10, XL_StrKind_None },
  { "price",      XL_ColKind_F64, 0,  XL_StrKind_None },
  { "discount",   XL_ColKind_F64, 1,  XL_StrKind_None },
  { "tax",        XL_ColKind_F64, 2,  XL_StrKind_None },
  { "amount",     XL_ColKind_F64, 3,  XL_StrKind_None },
  { "cost",       XL_ColKind_F64, 4,  XL_StrKind_None },
  { "score",      XL_ColKind_F64, 5,  XL_StrKind_None },
  { "m1",         XL_ColKind_F64, 6,  XL_StrKind_None },
  { "m2",         XL_ColKind_F64, 7,  XL_StrKind_None },
  { "m3",         XL_ColKind_F64, 8,  XL_StrKind_None },
  { "m4",         XL_ColKind_F64, 9,  XL_StrKind_None },
  { "m5",         XL_ColKind_F64, 10, XL_StrKind_None },
  { "m6",         XL_ColKind_F64, 11, XL_StrKind_None },
  { "m7",         XL_ColKind_F64, 12, XL_StrKind_None },
  { "m8",         XL_ColKind_F64, 13, XL_StrKind_None },
  { "country",    XL_ColKind_Str, 0,  XL_StrKind_Country },
  { "category",   XL_ColKind_Str, 1,  XL_StrKind_Category },
  { "status",     XL_ColKind_Str, 2,  XL_StrKind_Status },
  { "channel",    XL_ColKind_Str, 3,  XL_StrKind_Channel },
  { "session",    XL_ColKind_Str, 4,  XL_StrKind_Session },
};

global XL_Col g_xl_user_cols[] =
{
  { "user_id",        XL_ColKind_U32, 0, XL_StrKind_None },
  { "age",            XL_ColKind_U32, 1, XL_StrKind_None },
  { "signup_year",    XL_ColKind_U32, 2, XL_StrKind_None },
  { "lifetime_value", XL_ColKind_F64, 0, XL_StrKind_None },
  { "segment",        XL_ColKind_Str, 0, XL_StrKind_Segment },
};

global XL_Col g_xl_product_cols[] =
{
  { "product_id", XL_ColKind_U32, 0, XL_StrKind_None },
  { "cost",       XL_ColKind_F64, 0, XL_StrKind_None },
  { "weight",     XL_ColKind_F64, 1, XL_StrKind_None },
  { "category",   XL_ColKind_Str, 0, XL_StrKind_Category },
  { "brand",      XL_ColKind_Str, 1, XL_StrKind_Brand },
};

//~ tec: row generation, deterministic per seed so all three engines load byte identical data

internal void
xl_gen_event(Bench_Rng* rng, U64 index, XL_Row* row)
{
  row->u[0] = (U32)(index + 1);
  row->u[1] = (U32)(bench_rng_next(rng) % XL_NUM_USERS);
  row->u[2] = (U32)(bench_rng_next(rng) % XL_NUM_PRODUCTS);
  row->u[3] = (U32)(bench_rng_next(rng) % 50);
  row->u[4] = (U32)(bench_rng_next(rng) % 2000);
  row->u[5] = (U32)(1600000000ULL + index / 4);
  row->u[6] = (U32)(1 + bench_rng_next(rng) % 20);
  for (U32 k = 0; k < 4; k++)
  {
    row->u[7 + k] = (U32)(bench_rng_next(rng) % 100000);
  }
  
  row->f[0] = (F64)(bench_rng_next(rng) % 501);
  row->f[1] = (F64)(bench_rng_next(rng) % 31);
  row->f[2] = (F64)(bench_rng_next(rng) % 101);
  row->f[3] = (F64)(bench_rng_next(rng) % 10001);
  row->f[4] = (F64)(bench_rng_next(rng) % 301);
  row->f[5] = (F64)(bench_rng_next(rng) % 10000) / 100.0;
  for (U32 m = 0; m < 8; m++)
  {
    row->f[6 + m] = (F64)(bench_rng_next(rng) % 1001);
  }
  
  row->s[0] = (U32)(bench_rng_next(rng) % 200);
  row->s[1] = (U32)(bench_rng_next(rng) % 20);
  row->s[2] = (U32)(bench_rng_next(rng) % 5);
  row->s[3] = (U32)(bench_rng_next(rng) % 8);
  row->s[4] = (U32)(bench_rng_next(rng) % 5000000);
}

internal void
xl_gen_user(Bench_Rng* rng, U64 index, XL_Row* row)
{
  row->u[0] = (U32)index;
  row->u[1] = (U32)(18 + bench_rng_next(rng) % 63);
  row->u[2] = (U32)(2010 + bench_rng_next(rng) % 16);
  row->f[0] = (F64)(bench_rng_next(rng) % 100001);
  row->s[0] = (U32)(bench_rng_next(rng) % 8);
}

internal void
xl_gen_product(Bench_Rng* rng, U64 index, XL_Row* row)
{
  row->u[0] = (U32)index;
  row->f[0] = (F64)(bench_rng_next(rng) % 301);
  row->f[1] = (F64)(bench_rng_next(rng) % 51);
  row->s[0] = (U32)(bench_rng_next(rng) % 20);
  row->s[1] = (U32)(bench_rng_next(rng) % 100);
}

internal String8
xl_render_string(char* buf, U64 buf_size, XL_StrKind kind, U32 value)
{
  int length = 0;
  switch (kind)
  {
    case XL_StrKind_Country:  { length = snprintf(buf, buf_size, "c%03u", value); } break;
    case XL_StrKind_Category: { length = snprintf(buf, buf_size, "cat%02u", value); } break;
    case XL_StrKind_Session:  { length = snprintf(buf, buf_size, "s%07u", value); } break;
    case XL_StrKind_Brand:    { length = snprintf(buf, buf_size, "brand%03u", value); } break;
    case XL_StrKind_Status:   { return str8_cstring(g_xl_status[value]); }
    case XL_StrKind_Channel:  { return str8_cstring(g_xl_channel[value]); }
    case XL_StrKind_Segment:  { return str8_cstring(g_xl_segment[value]); }
    default: break;
  }
  return str8((U8*)buf, (U64)length);
}

//~ tec: loaders

internal char*
xl_cstr(Arena* arena, String8 text)
{
  U8* bytes = push_array(arena, U8, text.size + 1);
  MemoryCopy(bytes, text.str, text.size);
  bytes[text.size] = 0;
  return (char*)bytes;
}

internal GDB_Table*
xl_load_gdb(GDB_Database* database, XL_TableSpec* spec)
{
  GDB_Table* table = gdb_table_alloc(str8_cstring(spec->name));
  for (U64 c = 0; c < spec->col_count; c++)
  {
    GDB_ColumnType type = GDB_ColumnType_String8;
    if (spec->cols[c].kind == XL_ColKind_U32)
    {
      type = GDB_ColumnType_U32;
    }
    else if (spec->cols[c].kind == XL_ColKind_F64)
    {
      type = GDB_ColumnType_F64;
    }
    gdb_table_add_column(table, gdb_column_schema_create(str8_cstring(spec->cols[c].name), type));
  }
  gdb_database_add_table(database, table);
  
  Bench_Rng rng = { spec->seed };
  XL_Row row = {0};
  void* row_data[XL_MAX_COLS] = {0};
  String8 strings[XL_MAX_COLS] = {0};
  char buffers[XL_MAX_COLS][16] = {0};
  
  for (U64 i = 0; i < spec->row_count; i++)
  {
    spec->gen(&rng, i, &row);
    for (U64 c = 0; c < spec->col_count; c++)
    {
      XL_Col* col = &spec->cols[c];
      if (col->kind == XL_ColKind_U32)
      {
        row_data[c] = &row.u[col->slot];
      }
      else if (col->kind == XL_ColKind_F64)
      {
        row_data[c] = &row.f[col->slot];
      }
      else
      {
        strings[c] = xl_render_string(buffers[c], sizeof(buffers[c]), col->str_kind, row.s[col->slot]);
        row_data[c] = &strings[c];
      }
    }
    gdb_table_add_row(table, row_data, NULL);
  }
  return table;
}

internal void
xl_sqlite_run(sqlite3* db, String8 sql)
{
  Temp scratch = scratch_begin(0, 0);
  char* err = NULL;
  if (sqlite3_exec(db, xl_cstr(scratch.arena, sql), NULL, NULL, &err) != SQLITE_OK)
  {
    log_error("sqlite3_exec failed: %s", err ? err : "unknown error");
    sqlite3_free(err);
  }
  scratch_end(scratch);
}

internal void
xl_load_sqlite(sqlite3* db, XL_TableSpec* spec)
{
  Temp scratch = scratch_begin(0, 0);
  
  String8List ddl = {0};
  String8List insert = {0};
  str8_list_pushf(scratch.arena, &ddl, "CREATE TABLE %s (", spec->name);
  str8_list_pushf(scratch.arena, &insert, "INSERT INTO %s VALUES (", spec->name);
  for (U64 c = 0; c < spec->col_count; c++)
  {
    char* sql_type = "TEXT";
    if (spec->cols[c].kind == XL_ColKind_U32)
    {
      sql_type = "INTEGER";
    }
    else if (spec->cols[c].kind == XL_ColKind_F64)
    {
      sql_type = "REAL";
    }
    str8_list_pushf(scratch.arena, &ddl, "%s%s %s", c ? ", " : "", spec->cols[c].name, sql_type);
    str8_list_pushf(scratch.arena, &insert, "%s?", c ? "," : "");
  }
  str8_list_pushf(scratch.arena, &ddl, ");");
  str8_list_pushf(scratch.arena, &insert, ");");
  xl_sqlite_run(db, str8_list_join(scratch.arena, &ddl, 0));
  String8 insert_sql = str8_list_join(scratch.arena, &insert, 0);
  
  xl_sqlite_run(db, str8_lit("BEGIN TRANSACTION;"));
  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(db, (const char*)insert_sql.str, (int)insert_sql.size, &stmt, NULL);
  
  Bench_Rng rng = { spec->seed };
  XL_Row row = {0};
  char buffer[16] = {0};
  for (U64 i = 0; i < spec->row_count; i++)
  {
    spec->gen(&rng, i, &row);
    for (U64 c = 0; c < spec->col_count; c++)
    {
      XL_Col* col = &spec->cols[c];
      int param = (int)c + 1;
      if (col->kind == XL_ColKind_U32)
      {
        sqlite3_bind_int64(stmt, param, (S64)row.u[col->slot]);
      }
      else if (col->kind == XL_ColKind_F64)
      {
        sqlite3_bind_double(stmt, param, row.f[col->slot]);
      }
      else
      {
        String8 text = xl_render_string(buffer, sizeof(buffer), col->str_kind, row.s[col->slot]);
        sqlite3_bind_text(stmt, param, (const char*)text.str, (int)text.size, SQLITE_TRANSIENT);
      }
    }
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
  xl_sqlite_run(db, str8_lit("COMMIT;"));
  
  scratch_end(scratch);
}

internal void
xl_load_duckdb(duckdb_connection conn, XL_TableSpec* spec)
{
  Temp scratch = scratch_begin(0, 0);
  
  String8List ddl = {0};
  str8_list_pushf(scratch.arena, &ddl, "CREATE TABLE %s (", spec->name);
  for (U64 c = 0; c < spec->col_count; c++)
  {
    char* sql_type = "VARCHAR";
    if (spec->cols[c].kind == XL_ColKind_U32)
    {
      sql_type = "INTEGER";
    }
    else if (spec->cols[c].kind == XL_ColKind_F64)
    {
      sql_type = "DOUBLE";
    }
    str8_list_pushf(scratch.arena, &ddl, "%s%s %s", c ? ", " : "", spec->cols[c].name, sql_type);
  }
  str8_list_pushf(scratch.arena, &ddl, ");");
  String8 ddl_text = str8_list_join(scratch.arena, &ddl, 0);
  
  duckdb_result result = {0};
  if (duckdb_query(conn, xl_cstr(scratch.arena, ddl_text), &result) != DuckDBSuccess)
  {
    log_error("duckdb_query (CREATE TABLE %s) failed: %s", spec->name, duckdb_result_error(&result));
  }
  duckdb_destroy_result(&result);
  
  duckdb_appender appender = NULL;
  if (duckdb_appender_create(conn, NULL, spec->name, &appender) == DuckDBSuccess)
  {
    Bench_Rng rng = { spec->seed };
    XL_Row row = {0};
    char buffer[16] = {0};
    for (U64 i = 0; i < spec->row_count; i++)
    {
      spec->gen(&rng, i, &row);
      for (U64 c = 0; c < spec->col_count; c++)
      {
        XL_Col* col = &spec->cols[c];
        if (col->kind == XL_ColKind_U32)
        {
          duckdb_append_int32(appender, (S32)row.u[col->slot]);
        }
        else if (col->kind == XL_ColKind_F64)
        {
          duckdb_append_double(appender, row.f[col->slot]);
        }
        else
        {
          String8 text = xl_render_string(buffer, sizeof(buffer), col->str_kind, row.s[col->slot]);
          duckdb_append_varchar_length(appender, (const char*)text.str, text.size);
        }
      }
      duckdb_appender_end_row(appender);
    }
  }
  duckdb_appender_destroy(&appender);
  
  scratch_end(scratch);
}

//~ tec: query cases

typedef struct XL_Case XL_Case;
struct XL_Case
{
  char* label;
  char* gdb_sql;
  char* other_sql;
};

// tec: gdb spells substring search as `contains`, sqlite and duckdb use LIKE
global XL_Case g_xl_cases[] =
{
  { "filter: selective 0.1%",
    "SELECT id, user_id, amount FROM events WHERE amount > 9990;", NULL },
  { "filter: half the table, count",
    "SELECT COUNT(*) FROM events WHERE amount > 5000;", NULL },
  { "filter: three predicates",
    "SELECT COUNT(*), SUM(amount) FROM events WHERE region_id = 7 AND status = 'shipped' AND amount >= 1000 AND amount <= 9000;", NULL },
  { "filter: dict string equality",
    "SELECT COUNT(*) FROM events WHERE country = 'c042';", NULL },
  { "filter: high card string search",
    "SELECT COUNT(*) FROM events WHERE session contains '777';",
    "SELECT COUNT(*) FROM events WHERE session LIKE '%777%';" },
  { "filter: wide projection 1%",
    "SELECT * FROM events WHERE amount > 9900;", NULL },
  
  { "agg: global five aggregates",
    "SELECT COUNT(*), SUM(amount), AVG(amount), MIN(score), MAX(score) FROM events;", NULL },
  { "agg: group by region (50)",
    "SELECT region_id, COUNT(*), SUM(amount), AVG(price), MIN(score), MAX(score) FROM events GROUP BY region_id;", NULL },
  { "agg: group by region, narrow measures",
    "SELECT region_id, COUNT(*), SUM(amount), AVG(price), MIN(m1), MAX(m2) FROM events GROUP BY region_id;", NULL },
  { "agg: group by product (10k)",
    "SELECT product_id, COUNT(*), SUM(amount) FROM events GROUP BY product_id;", NULL },
  { "agg: group by user (1M)",
    "SELECT user_id, COUNT(*), SUM(amount) FROM events GROUP BY user_id;", NULL },
  { "agg: two keys (region, category)",
    "SELECT region_id, category, SUM(amount), COUNT(*) FROM events GROUP BY region_id, category;", NULL },
  { "agg: group by dict string (200)",
    "SELECT country, COUNT(*), SUM(amount) FROM events GROUP BY country;", NULL },
  { "agg: group by session (4M)",
    "SELECT session, COUNT(*) FROM events GROUP BY session;", NULL },
  { "agg: eight measures by region",
    "SELECT region_id, SUM(m1), SUM(m2), SUM(m3), SUM(m4), SUM(m5), SUM(m6), SUM(m7), SUM(m8) FROM events GROUP BY region_id;", NULL },
  { "agg: filtered group by product",
    "SELECT product_id, SUM(amount) FROM events WHERE region_id < 10 GROUP BY product_id;", NULL },
  { "agg: having over 1M groups",
    "SELECT user_id, SUM(amount) FROM events GROUP BY user_id HAVING SUM(amount) > 80000;", NULL },
  
  { "join: products (10k), by category",
    "SELECT products.category, SUM(events.amount), COUNT(*) FROM events JOIN products ON events.product_id = products.product_id GROUP BY products.category;", NULL },
  { "join: users (1M), by segment",
    "SELECT users.segment, SUM(events.amount), COUNT(*) FROM events JOIN users ON events.user_id = users.user_id GROUP BY users.segment;", NULL },
  { "join: three way, filtered dims",
    "SELECT users.segment, products.brand, SUM(events.amount) FROM events JOIN users ON events.user_id = users.user_id JOIN products ON events.product_id = products.product_id WHERE users.age > 60 AND products.cost > 250 GROUP BY users.segment, products.brand;", NULL },
  { "join: count star, users",
    "SELECT COUNT(*) FROM events JOIN users ON events.user_id = users.user_id;", NULL },
  { "join: 1M row output",
    "SELECT events.id, users.segment, events.amount FROM events JOIN users ON events.user_id = users.user_id WHERE events.amount > 9000;", NULL },
  
  { "topn: 100 by amount",
    "SELECT id, amount FROM events ORDER BY amount DESC, id LIMIT 100;", NULL },
  { "topn: filtered, 1000",
    "SELECT id, amount FROM events WHERE region_id = 3 ORDER BY amount DESC, id LIMIT 1000;", NULL },
  { "topn: over 1M groups",
    "SELECT user_id, SUM(amount) AS total FROM events GROUP BY user_id ORDER BY total DESC, user_id LIMIT 100;", NULL },
  { "sort: 200k rows",
    "SELECT id, amount FROM events WHERE region_id = 3 ORDER BY amount, id;", NULL },
  
  { "window: top 3 per region, 1M rows",
    "SELECT COUNT(*) FROM (SELECT id, ROW_NUMBER() OVER (PARTITION BY region_id ORDER BY amount DESC, id) AS rn FROM events WHERE amount > 9000) AS t WHERE rn <= 3;", NULL },
};

internal void
xl_print_load(Bench_Report* report, char* engine, char* table, U64 rows, U64 start_us)
{
  F64 seconds = (F64)(os_now_microseconds() - start_us) / 1000000.0;
  printf("load %-7s %-9s %10llu rows  %8.2f s  (%.2f M rows/s)\n", engine, table, rows, seconds, (F64)rows / seconds / 1000000.0);
  bench_report_text(report, "load %s %s: %llu rows in %.2f s (%.2f M rows/s)", engine, table, rows, seconds, (F64)rows / seconds / 1000000.0);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  
  U64 event_rows = 10000000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    event_rows = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  B32 run_sqlite = !cmd_line_has_flag(cmdline, str8_lit("no_sqlite"));
  B32 run_duckdb = !cmd_line_has_flag(cmdline, str8_lit("no_duckdb"));
  String8 only = cmd_line_string(cmdline, str8_lit("only"));
  String8 setting = cmd_line_string(cmdline, str8_lit("setting"));
  if (setting.size > 0)
  {
    settings_load_from_string(setting);
  }
  
  U64 t0 = os_now_microseconds();
  gdb_init();
  gpu_init();
  U64 t1 = os_now_microseconds();
  printf("engine startup (gdb_init + gpu_init, one-time): %.4f ms\n", (F64)(t1 - t0) / 1000.0);
  
  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  
  Bench_Report* report = bench_report_alloc(arena, "compute_ql vs sqlite vs duckdb - large scale suite");
  bench_report_text(report, "engine startup (gdb_init + gpu_init, one-time): %.4f ms", (F64)(t1 - t0) / 1000.0);
  bench_report_text(report, "events: %llu rows, %u columns; users: %u rows; products: %u rows; %u timed runs per query",
                    event_rows, (U32)ArrayCount(g_xl_event_cols), (U32)XL_NUM_USERS, (U32)XL_NUM_PRODUCTS, (U32)BENCH_TIMED_RUNS);
  
  XL_TableSpec specs[3] =
  {
    { "events",   g_xl_event_cols,   ArrayCount(g_xl_event_cols),   xl_gen_event,   event_rows,      0xE7E47500ULL },
    { "users",    g_xl_user_cols,    ArrayCount(g_xl_user_cols),    xl_gen_user,    XL_NUM_USERS,    0x05E45ULL },
    { "products", g_xl_product_cols, ArrayCount(g_xl_product_cols), xl_gen_product, XL_NUM_PRODUCTS, 0x960D0C7ULL },
  };
  
  bench_report_section(report, "load");
  
  GDB_Database* database = gdb_database_alloc(str8_lit("xlarge_db"));
  gdb_add_database(database);
  for (U64 s = 0; s < ArrayCount(specs); s++)
  {
    U64 start = os_now_microseconds();
    xl_load_gdb(database, &specs[s]);
    xl_print_load(report, "gdb", specs[s].name, specs[s].row_count, start);
  }
  
  sqlite3* sqlite_db = NULL;
  if (run_sqlite)
  {
    sqlite3_open(":memory:", &sqlite_db);
    for (U64 s = 0; s < ArrayCount(specs); s++)
    {
      U64 start = os_now_microseconds();
      xl_load_sqlite(sqlite_db, &specs[s]);
      xl_print_load(report, "sqlite", specs[s].name, specs[s].row_count, start);
    }
  }
  
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  if (run_duckdb)
  {
    duckdb_open(NULL, &duckdb_db);
    duckdb_connect(duckdb_db, &duckdb_conn);
    for (U64 s = 0; s < ArrayCount(specs); s++)
    {
      U64 start = os_now_microseconds();
      xl_load_duckdb(duckdb_conn, &specs[s]);
      xl_print_load(report, "duckdb", specs[s].name, specs[s].row_count, start);
    }
  }
  
  bench_report_section(report, "large scale queries (%llu event rows)", event_rows);
  bench_print_table_header(report, "large scale");
  
  for (U64 i = 0; i < ArrayCount(g_xl_cases); i++)
  {
    XL_Case* test_case = &g_xl_cases[i];
    String8 label = str8_cstring(test_case->label);
    if (only.size > 0 && str8_find_needle(label, 0, only, 0) >= label.size)
    {
      continue;
    }
    String8 gdb_sql = str8_cstring(test_case->gdb_sql);
    String8 other_sql = str8_cstring(test_case->other_sql ? test_case->other_sql : test_case->gdb_sql);
    
    U64 gdb_rows = 0, gdb_checksum = 0;
    Bench_Stats gdb_stats = bench_run_gdb_query(database, gdb_sql, &gdb_rows, &gdb_checksum);
    bench_print_table_row(report, label, "gdb", gdb_rows, gdb_checksum, &gdb_stats);
    
    if (run_sqlite)
    {
      U64 rows = 0, checksum = 0;
      Bench_Stats stats = bench_run_sqlite_query(sqlite_db, other_sql, &rows, &checksum);
      bench_print_table_row(report, label, "sqlite", rows, checksum, &stats);
      bench_check_match(report, label, "gdb", gdb_rows, gdb_checksum, "sqlite", rows, checksum);
    }
    
    if (run_duckdb)
    {
      U64 rows = 0, checksum = 0;
      Bench_Stats stats = bench_run_duckdb_query(duckdb_conn, other_sql, &rows, &checksum);
      bench_print_table_row(report, label, "duckdb", rows, checksum, &stats);
      bench_check_match(report, label, "gdb", gdb_rows, gdb_checksum, "duckdb", rows, checksum);
    }
    fflush(stdout);
  }
  
  if (run_sqlite)
  {
    sqlite3_close(sqlite_db);
  }
  if (run_duckdb)
  {
    duckdb_disconnect(&duckdb_conn);
    duckdb_close(&duckdb_db);
  }
  
  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  bench_report_write(report, str8_lit("bench_reports/xlarge_report.md"));
  
  arena_release(arena);
  
  log_release();
  
  ProfEnd();
  ProfEndCapture();
}
