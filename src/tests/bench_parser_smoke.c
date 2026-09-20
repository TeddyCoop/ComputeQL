// parser regression smoke test: tokenizes + parses one query per SQL clause type

#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)

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

  // tec: bare table alias
  { "bare table alias",    "select u.id from users u where u.id > 1;" },
  { "bare alias in join",  "select a.id from a x join b y on x.id = y.a_id;" },

  // tec: derived tables
  { "derived table",       "select id from (select id from users where id > 1) as t where id < 9;" },
  { "derived bare alias",  "select t.id from (select id from users) t;" },
  { "derived, nested",     "select id from (select id from (select id from users) a where id > 1) b;" },
  { "derived in join",     "select u.id from users u join (select user_id from orders group by user_id) o on u.id = o.user_id;" },

  // tec: common table expressions
  { "cte",                 "with t as (select id from users) select id from t;" },
  { "cte, two",            "with a as (select id from users), b as (select id from a where id > 1) select id from b;" },
  { "cte with clauses",    "with t as (select dept, count(*) as c from emp group by dept having count(*) > 1 order by dept limit 5) select dept from t;" },
  { "cte then clauses",    "with t as (select id from users) select id from t where id > 2 order by id limit 3;" },
  { "cte, two statements", "with a as (select id from users) select id from a; with b as (select id from users) select id from b;" },

  // tec: subqueries
  { "scalar subquery",     "select id from users where age > (select avg(age) from users);" },
  { "scalar on the left",  "select id from users where (select max(age) from users) > age;" },
  { "in subquery",         "select id from users where id in (select user_id from orders);" },
  { "not in subquery",     "select id from users where id not in (select user_id from orders where total > 5);" },
  { "in list",             "select id from users where id in (1, 2, 3);" },
  { "not in list",         "select id from users where name not in ('a', 'b');" },
  { "exists",              "select id from users where exists (select id from orders where total > 5);" },
  { "not exists",          "select id from users where not exists (select id from orders);" },
  { "exists + and",        "select id from users where exists (select id from orders) and id > 3;" },
  { "subquery in having",  "select dept, count(*) from emp group by dept having count(*) > (select count(*) from users);" },
  { "nested subqueries",   "select id from users where id in (select user_id from orders where total > (select avg(total) from orders));" },
  { "parens still group",  "select id from users where (age > 1 and age < 9) or (age = 20);" },

  // tec: window functions
  { "row_number",          "select id, row_number() over (order by id) as rn from users;" },
  { "over partition",      "select id, rank() over (partition by dept order by salary desc) as r from emp;" },
  { "over partition only", "select id, sum(salary) over (partition by dept) as s from emp;" },
  { "over empty",          "select id, count(*) over () as n from emp;" },
  { "over two keys",       "select id, dense_rank() over (partition by a, b order by c, d desc) as r from t;" },
  { "lag with args",       "select id, lag(salary, 2, 0) over (partition by dept order by id) as l from emp;" },
  { "window, no alias",    "select id, sum(salary) over (order by id) from emp;" },
  { "window in derived",   "select id from (select id, row_number() over (partition by g order by v desc) as rn from t) d where rn <= 3;" },
};

// tec: each of these must be rejected by the parser
global Parser_Case g_parser_error_cases[] =
{
  { "with, no select",       "with t as (select id from users);" },
  { "with, no as",           "with t (select id from users) select id from t;" },
  { "with, no parens",       "with t as select id from users select id from t;" },
  { "derived, no alias",     "select id from (select id from users);" },
  { "derived, unclosed",     "select id from (select id from users as t;" },
  { "subquery, unclosed",    "select id from users where id in (select id from orders;" },
  { "in, no list",           "select id from users where id in ;" },
  { "in, empty list",        "select id from users where id in ();" },
  { "in list, bad item",     "select id from users where id in (1, name);" },
  { "exists, no subquery",   "select id from users where exists id;" },
  { "over, no parens",       "select id, row_number() over order by id from users;" },
  { "over, unclosed",        "select id, row_number() over (order by id from users;" },
  { "partition without by",  "select id, rank() over (partition id) from users;" },
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

  for (U64 i = 0; i < ArrayCount(g_parser_error_cases); i++)
  {
    Temp scratch = scratch_begin(0, 0);

    String8 query = str8_cstring(g_parser_error_cases[i].query);
    SQL_TokenizeResult tok = sql_tokenize_from_text(scratch.arena, query);
    SQL_Node* ast = sql_parse(scratch.arena, tok.tokens, tok.count, query);

    B32 rejected = (ast == NULL) || g_sql_parse_error.has_error;
    if (rejected)
    {
      pass_count++;
      printf("[PASS] rejects: %s\n", g_parser_error_cases[i].name);
    }
    else
    {
      fail_count++;
      printf("[FAIL] rejects: %s : %s\n", g_parser_error_cases[i].name, g_parser_error_cases[i].query);
    }

    scratch_end(scratch);
  }

  //- tec: the shape of the AST for the new forms, not just that they parse
  {
    Temp scratch = scratch_begin(0, 0);
    String8 query = str8_lit("with t as (select id from users) select id from (select id from t) d where id in (select id from t) and exists (select id from t);");
    SQL_TokenizeResult tok = sql_tokenize_from_text(scratch.arena, query);
    SQL_Node* ast = sql_parse(scratch.arena, tok.tokens, tok.count, query);

    B32 ok = ast && ast->type == SQL_NodeType_Select;
    B32 has_cte_list = 0, derived_ok = 0, in_ok = 0, exists_ok = 0;
    for (SQL_Node* child = ok ? ast->first : NULL; child; child = child->next)
    {
      if (child->type == SQL_NodeType_CteList)
      {
        has_cte_list = child->first && child->first->type == SQL_NodeType_Cte &&
          str8_match(child->first->value, str8_lit("t"), 0) && child->first->first && child->first->first->type == SQL_NodeType_Select;
      }
      if (child->type == SQL_NodeType_Table)
      {
        derived_ok = child->first && child->first->type == SQL_NodeType_Select && child->last && child->last->type == SQL_NodeType_Alias &&
          str8_match(child->last->value, str8_lit("d"), 0);
      }
      if (child->type == SQL_NodeType_Where && child->first && child->first->type == SQL_NodeType_Operator)
      {
        SQL_Node* and_node = child->first;
        SQL_Node* in_node = and_node->first;
        SQL_Node* exists_node = and_node->last;
        in_ok = in_node && str8_match(in_node->value, str8_lit("in"), 0) && in_node->last && in_node->last->type == SQL_NodeType_Subquery;
        exists_ok = exists_node && exists_node->type == SQL_NodeType_Exists && exists_node->first && exists_node->first->type == SQL_NodeType_Select;
      }
    }

    struct { char* name; B32 ok; } shape_checks[] =
    {
      { "ast: with -> CteList/Cte/Select", has_cte_list },
      { "ast: derived Table has Select then Alias", derived_ok },
      { "ast: 'in (select)' -> Operator with Subquery", in_ok },
      { "ast: 'exists (select)' -> Exists with Select", exists_ok },
    };
    for (U64 i = 0; i < ArrayCount(shape_checks); i++)
    {
      if (shape_checks[i].ok) { pass_count++; printf("[PASS] %s\n", shape_checks[i].name); }
      else { fail_count++; printf("[FAIL] %s\n", shape_checks[i].name); }
    }

    scratch_end(scratch);
  }

  printf("\nparser smoke test: %llu passed, %llu failed (of %llu)\n",
         pass_count, fail_count, (U64)(ArrayCount(g_parser_cases) + ArrayCount(g_parser_error_cases) + 4));

  arena_release(arena);
  log_release();

  ProfEnd();
  ProfEndCapture();

  if (fail_count > 0)
  {
    os_abort(1);
  }
}
