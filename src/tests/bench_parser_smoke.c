// parser regression smoke test: tokenizes + parses one query per SQL clause type

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

typedef struct Parser_Case Parser_Case;
struct Parser_Case
{
  char *name;
  char *query;
};

global Parser_Case g_parser_cases[] =
{
  { "join",                "select a.id, b.name from a inner join b on a.id = b.a_id;" },
  { "left outer join",     "select a.id from a left outer join b on a.id = b.a_id where a.id > 1;" },
  { "group by/having",     "select department, count(id) from employees group by department having count(id) > 1;" },
  { "order by asc/desc",   "select id, name from users order by name desc, id asc;" },
  { "limit/offset",        "select id from users limit 10 offset 5;" },
  { "and/or/parens",       "select id from users where (age > 18 and age < 65) or is_admin = 1;" },
  { "is null",             "select id from users where deleted_at is null;" },
  { "is not null",         "select id from users where deleted_at is not null;" },
  { "create table",        "create table orders (id u64 primary key, user_id u64 not null references users(id), total f64 check (total >= 0), name string8 unique);" },
  { "create index",        "create index idx_users_name on users (name);" },
  { "alter add column",    "alter table users add column age u32;" },
  { "alter drop column",   "alter table users drop column age;" },
  { "alter rename",        "alter table users rename to customers;" },
  { "drop index",          "drop index idx_users_name on users;" },
  { "delete where",        "delete from users where id = 1;" },
  { "describe",            "describe users;" },
  { "use",                 "use mydb;" },
  { "insert values",       "insert into users (id, name) values (1, 'a'), (2, 'b');" },
};

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();

  Arena* arena = arena_alloc(.reserve_size = MB(64), .commit_size = KB(64));

  U64 pass_count = 0;
  U64 fail_count = 0;

  for (U64 i = 0; i < ArrayCount(g_parser_cases); i++)
  {
    Temp scratch = scratch_begin(0, 0);

    String8 query = str8_cstring(g_parser_cases[i].query);
    SQL_TokenizeResult tok = sql_tokenize_from_text(scratch.arena, query);
    SQL_Node* ast = sql_parse(scratch.arena, tok.tokens, tok.count, query);

    B32 ok = ast != NULL && !g_sql_parse_error.has_error;
    if (ok)
    {
      pass_count++;
      printf("[PASS] %s\n", g_parser_cases[i].name);
    }
    else
    {
      fail_count++;
      printf("[FAIL] %s : %s\n", g_parser_cases[i].name, g_parser_cases[i].query);
    }

    scratch_end(scratch);
  }

  printf("\nparser smoke test: %llu passed, %llu failed (of %llu)\n",
         pass_count, fail_count, (U64)ArrayCount(g_parser_cases));

  arena_release(arena);
  log_release();

  ProfEnd();
  ProfEndCapture();

  if (fail_count > 0)
  {
    os_abort(1);
  }
}
