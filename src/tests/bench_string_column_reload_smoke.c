// string8 column persistence regression test:
//
// once a string8 column's saved .dat file grows past GDB_DISK_BACKED_THRESHOLD_SIZE (its reserved
// data-blob capacity pads the file well beyond the handful of bytes actually used), reloading the
// table marks that column disk-backed. appending a new row after such a reload used to write the
// new string past the real end of the existing data and record a garbage end-offset for it,
// producing a huge run of zero bytes ahead of the new (or a later) row's content. see
// gdb_table_load's disk-backed String8 branch in gdb.c.

#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "gdb/gdb_inc.h"
#include "ir_gen/ir_gen_inc.h"
#include "gpu/gpu_inc.h"
#include "query_exec/query_exec.h"
#include "planner/planner.h"
#include "third_party/sqlite/sqlite3.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"

internal B32
scr_check_string(GDB_Column *column, U64 index, String8 expected, B32 *pass)
{
  Temp scratch = scratch_begin(0, 0);
  String8 got = gdb_column_get_string(scratch.arena, column, index);
  B32 ok = str8_match(got, expected, 0);
  if (!ok)
  {
    printf("[FAIL] row %llu: expected '%.*s' (%llu bytes), got '%.*s' (%llu bytes)\n",
           index, str8_varg(expected), expected.size, str8_varg(got), got.size);
    *pass = 0;
  }
  scratch_end(scratch);
  return ok;
}

internal void
entry_point(CmdLine *cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();
  gdb_init();

  B32 pass = 1;

  String8 table_dir = str8_lit("gdb_data/string_reload_smoke_test");
  {
    Temp cleanup_scratch = scratch_begin(0, 0);
    os_delete_file_at_path(push_str8f(cleanup_scratch.arena, "%.*s/t.meta", str8_varg(table_dir)));
    os_delete_file_at_path(push_str8f(cleanup_scratch.arena, "%.*s/id.dat", str8_varg(table_dir)));
    os_delete_file_at_path(push_str8f(cleanup_scratch.arena, "%.*s/name.dat", str8_varg(table_dir)));
    scratch_end(cleanup_scratch);
  }

  if (!os_file_path_exists(str8_lit("gdb_data/"))) { os_make_directory(str8_lit("gdb_data/")); }
  if (!os_file_path_exists(table_dir)) { os_make_directory(table_dir); }

  //- tec: build a small table entirely in memory, matching what CREATE TABLE + INSERT would produce
  GDB_Table *table = gdb_table_alloc(str8_lit("t"));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("id"), GDB_ColumnType_U32));
  gdb_table_add_column(table, gdb_column_schema_create(str8_lit("name"), GDB_ColumnType_String8));

  {
    U32 id = 1; String8 name = str8_lit("alpha");
    void *row[2] = { &id, &name };
    gdb_table_add_row(table, row, NULL);
  }
  {
    U32 id = 2; String8 name = str8_lit("beta");
    void *row[2] = { &id, &name };
    gdb_table_add_row(table, row, NULL);
  }

  gdb_table_save(table, table_dir);
  gdb_table_release(table);

  //- tec: reload from disk, exactly like a second process 'USE'-ing the database would
  Temp scratch = scratch_begin(0, 0);
  String8 meta_path = push_str8f(scratch.arena, "%.*s/t.meta", str8_varg(table_dir));
  GDB_Table *loaded = gdb_table_load(table_dir, meta_path);

  if (!loaded || loaded->column_count != 2)
  {
    printf("[FAIL] table failed to reload\n");
    pass = 0;
  }
  else
  {
    GDB_Column *name_column = gdb_table_find_column(loaded, str8_lit("name"));

    if (!name_column->is_disk_backed)
    {
      // tec: the whole point of this test is to exercise the disk-backed reload path -
      // if this ever stops holding (e.g. GDB_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE or
      // GDB_DISK_BACKED_THRESHOLD_SIZE change), the test needs more/larger rows to still trigger it
      printf("[FAIL] name column did not come back disk-backed - test no longer exercises the reload bug\n");
      pass = 0;
    }

    scr_check_string(name_column, 0, str8_lit("alpha"), &pass);
    scr_check_string(name_column, 1, str8_lit("beta"), &pass);

    //- tec: append a new row after the reload, the way a second INSERT would
    U32 new_id = 3; String8 new_name = str8_lit("gamma");
    void *row[2] = { &new_id, &new_name };
    gdb_table_add_row(loaded, row, NULL);

    scr_check_string(name_column, 0, str8_lit("alpha"), &pass);
    scr_check_string(name_column, 1, str8_lit("beta"), &pass);
    scr_check_string(name_column, 2, str8_lit("gamma"), &pass);
  }
  scratch_end(scratch);

  if (pass)
  {
    printf("string column reload smoke test: PASS\n");
  }
  else
  {
    printf("string column reload smoke test: FAIL\n");
  }

  log_release();

  ProfEnd();
  ProfEndCapture();

  if (!pass)
  {
    fflush(stdout); // tec: os_abort -> ExitProcess skips CRT atexit flushing
    os_abort(1);
  }
}
