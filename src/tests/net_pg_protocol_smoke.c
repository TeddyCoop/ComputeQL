// Postgres wire protocol server smoke test
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
#include "application.h"
#include "server/pg_protocol.h"
#include "server/pg_server.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"
#include "application.c"
#include "server/pg_protocol.c"
#include "server/pg_server.c"

#define PGT_TEST_PORT 47323

internal void
pgt_server_thread_proc(void *ptr)
{
  U16 port = *(U16*)ptr;
  server_run_pg(port); 
}

typedef struct PGT_Response PGT_Response;
struct PGT_Response
{
  B32 got_ready;
  B32 got_error;
  U64 data_row_count;
  U64 command_complete_count;
  String8 all_bytes; // every message body concatenated, for substring checks
};

internal PGT_Response
pgt_read_until_ready(Arena *arena, OS_Handle conn)
{
  PGT_Response resp = {0};
  String8List parts = {0};
  for (U64 guard = 0; guard < 64; guard++)
  {
    U8 type = 0;
    String8 body = {0};
    if (!pg_recv_typed(arena, conn, &type, &body)) { break; }
    str8_list_push(arena, &parts, body);
    if (type == 'D') { resp.data_row_count++; }
    if (type == 'C') { resp.command_complete_count++; }
    if (type == 'E') { resp.got_error = 1; }
    if (type == 'Z') { resp.got_ready = 1; break; }
  }
  resp.all_bytes = str8_list_join(arena, &parts, 0);
  return resp;
}

internal B32
pgt_bytes_contain(String8 haystack, String8 needle)
{
  return str8_find_needle(haystack, 0, needle, 0) < haystack.size;
}

internal PGT_Response
pgt_simple_query(Arena *arena, OS_Handle conn, String8 sql)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, sql));
  B32 sent = pg_send_msg(conn, scratch.arena, 'Q', &parts);
  scratch_end(scratch);

  if (!sent)
  {
    PGT_Response empty = {0};
    return empty;
  }
  return pgt_read_until_ready(arena, conn);
}

internal OS_Handle
pgt_connect_and_handshake(Arena *arena, U16 port, String8 database_or_empty, B32 *out_ok)
{
  *out_ok = 0;
  OS_Handle conn = os_handle_zero();

  for (U64 attempt = 0; attempt < 40; attempt++)
  {
    conn = os_net_connect(str8_lit("127.0.0.1"), port);
    if (!os_handle_match(conn, os_handle_zero())) { break; }
    os_sleep_milliseconds(50);
  }
  if (os_handle_match(conn, os_handle_zero())) { return conn; }

  Temp scratch = scratch_begin(&arena, 1);

  // tec: SSLRequest. server must answer a single 'N' byte, no message framing
  {
    U8 ssl_req[8];
    U32 len = 8, code = PG_SSL_REQUEST_CODE;
    ssl_req[0]=(U8)(len>>24); ssl_req[1]=(U8)(len>>16); ssl_req[2]=(U8)(len>>8); ssl_req[3]=(U8)len;
    ssl_req[4]=(U8)(code>>24); ssl_req[5]=(U8)(code>>16); ssl_req[6]=(U8)(code>>8); ssl_req[7]=(U8)code;
    if (!os_net_send_exact(conn, ssl_req, sizeof(ssl_req))) { scratch_end(scratch); return conn; }
    U8 resp = 0;
    if (!os_net_recv_exact(conn, &resp, 1) || resp != 'N') { scratch_end(scratch); return conn; }
  }

  // tec: StartupMessage. protocol 3.0 + "user"/"database" params
  {
    String8List parts = {0};
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, PG_PROTOCOL_VERSION_3_0));
    str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, str8_lit("user")));
    str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, str8_lit("pgt_user")));
    if (database_or_empty.size > 0)
    {
      str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, str8_lit("database")));
      str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, database_or_empty));
    }
    U8 zero = 0;
    str8_list_push(scratch.arena, &parts, str8(&zero, 1));

    String8 body = str8_list_join(scratch.arena, &parts, 0);
    U8 len_bytes[4];
    U32 len = (U32)(body.size + 4);
    len_bytes[0]=(U8)(len>>24); len_bytes[1]=(U8)(len>>16); len_bytes[2]=(U8)(len>>8); len_bytes[3]=(U8)len;
    if (!os_net_send_exact(conn, len_bytes, 4) || !os_net_send_exact(conn, body.str, body.size))
    {
      scratch_end(scratch);
      return conn;
    }
  }

  // tec: consume AuthenticationOk / ParameterStatus* / BackendKeyData / ReadyForQuery
  for (U64 guard = 0; guard < 32; guard++)
  {
    U8 type = 0;
    String8 rbody = {0};
    if (!pg_recv_typed(arena, conn, &type, &rbody)) { scratch_end(scratch); return conn; }
    if (type == 'E') { scratch_end(scratch); return conn; } // tec: handshake rejected
    if (type == 'Z') { *out_ok = 1; break; }
  }

  scratch_end(scratch);
  return conn;
}

internal void
entry_point(CmdLine *cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();
  g_query_exec_mutex = os_mutex_alloc();

  B32 pass = 1;

  if (!os_file_path_exists(str8_lit("gdb_data/")))
  {
    os_make_directory(str8_lit("gdb_data/"));
  }

  gdb_init();
  gpu_init();

  if (!os_net_init())
  {
    log_error("net_pg_protocol_smoke: os_net_init failed");
    pass = 0;
  }
  else
  {
    U16 port = PGT_TEST_PORT;
    OS_Handle server_thread = os_thread_launch(pgt_server_thread_proc, &port, 0);
    os_thread_detach(server_thread);

    Temp scratch = scratch_begin(0, 0);

    B32 ok = 0;
    OS_Handle conn_a = pgt_connect_and_handshake(scratch.arena, port, str8_lit(""), &ok);
    if (!ok)
    {
      log_error("net_pg_protocol_smoke: FAIL - startup handshake (incl. SSL negotiation) did not complete");
      pass = 0;
    }
    else
    {
      log_info("net_pg_protocol_smoke: startup handshake (SSL negotiation + StartupMessage) OK");

      PGT_Response r = pgt_simple_query(scratch.arena, conn_a,
        str8_lit("CREATE DATABASE net_pg_smoke_test; USE net_pg_smoke_test; "
                 "CREATE TABLE t (id u32, name string8, price f64); "
                 "INSERT INTO t (id, name, price) VALUES (1, 'alpha', 9.99), (2, 'beta', 19.5);"));
      if (!r.got_ready || r.got_error || r.command_complete_count == 0)
      {
        log_error("net_pg_protocol_smoke: FAIL - DDL+INSERT simple query didn't complete cleanly");
        pass = 0;
      }
      else
      {
        log_info("net_pg_protocol_smoke: DDL+INSERT over simple query protocol OK");
      }

      r = pgt_simple_query(scratch.arena, conn_a, str8_lit("SELECT id, name, price FROM t ORDER BY id ASC;"));
      if (!r.got_ready || r.got_error || r.data_row_count != 2 ||
          !pgt_bytes_contain(r.all_bytes, str8_lit("alpha")) ||
          !pgt_bytes_contain(r.all_bytes, str8_lit("beta")))
      {
        log_error("net_pg_protocol_smoke: FAIL - structured SELECT didn't return the expected rows (data_row_count=%llu got_error=%d)",
                   r.data_row_count, r.got_error);
        pass = 0;
      }
      else
      {
        log_info("net_pg_protocol_smoke: structured SELECT (RowDescription+DataRow) OK");
      }

      r = pgt_simple_query(scratch.arena, conn_a, str8_lit("SELECT * FROM does_not_exist;"));
      if (!r.got_ready || !r.got_error)
      {
        log_error("net_pg_protocol_smoke: FAIL - missing-table SELECT * didn't come back as a clean error");
        pass = 0;
      }
      else
      {
        r = pgt_simple_query(scratch.arena, conn_a, str8_lit("SELECT id FROM t;"));
        if (!r.got_ready || r.got_error || r.data_row_count != 2)
        {
          log_error("net_pg_protocol_smoke: FAIL - connection unusable after an ErrorResponse (or the server didn't survive)");
          pass = 0;
        }
        else
        {
          log_info("net_pg_protocol_smoke: missing-table SELECT * handled without crashing the server OK");
        }
      }

      os_net_close(conn_a);
    }

    B32 ok2 = 0;
    OS_Handle conn_b = pgt_connect_and_handshake(scratch.arena, port, str8_lit(""), &ok2);
    if (!ok2)
    {
      log_error("net_pg_protocol_smoke: FAIL - second handshake (no database param) did not complete");
      pass = 0;
    }
    else
    {
      PGT_Response r = pgt_simple_query(scratch.arena, conn_b, str8_lit("SELECT id FROM t;"));
      if (!r.got_ready || !r.got_error || !pgt_bytes_contain(r.all_bytes, str8_lit("no database selected")))
      {
        log_error("net_pg_protocol_smoke: FAIL - second connection unexpectedly saw the first connection's database");
        pass = 0;
      }
      else
      {
        log_info("net_pg_protocol_smoke: per-connection database selection isolation OK");
      }
      os_net_close(conn_b);
    }

    scratch_end(scratch);
  }

  if (pass)
  {
    log_info("net_pg_protocol_smoke: PASS");
  }
  else
  {
    log_error("net_pg_protocol_smoke: FAIL");
  }

  ProfEnd();
  ProfEndCapture();
  log_release();
}
