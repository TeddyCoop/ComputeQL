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
#include "server/server.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"
#include "application.c"
#include "server/server.c"

#define NCS_TEST_PORT 47322

internal void
ncs_server_thread_proc(void *ptr)
{
  U16 port = *(U16*)ptr;
  server_run(port); // tec: never returns - fine, this test process exits when done
}

// tec: one request/response round trip over an already-connected socket
internal B32
ncs_request(Arena *arena, OS_Handle conn, String8 sql, U8 *out_status, String8 *out_payload)
{
  U64 sql_len = sql.size;
  if (!os_net_send_exact(conn, &sql_len, sizeof(sql_len))) { return 0; }
  if (!os_net_send_exact(conn, sql.str, sql_len)) { return 0; }

  U8 status = 0;
  U64 payload_len = 0;
  if (!os_net_recv_exact(conn, &status, sizeof(status))) { return 0; }
  if (!os_net_recv_exact(conn, &payload_len, sizeof(payload_len))) { return 0; }

  String8 payload = {0};
  if (payload_len > 0)
  {
    U8 *bytes = push_array_no_zero(arena, U8, payload_len);
    if (!os_net_recv_exact(conn, bytes, payload_len)) { return 0; }
    payload = str8(bytes, payload_len);
  }

  *out_status = status;
  *out_payload = payload;
  return 1;
}

internal B32
ncs_payload_contains(String8 haystack, String8 needle)
{
  return str8_find_needle(haystack, 0, needle, 0) < haystack.size;
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
    log_error("net_client_server_session: os_net_init failed");
    pass = 0;
  }
  else
  {
    U16 port = NCS_TEST_PORT;
    OS_Handle server_thread = os_thread_launch(ncs_server_thread_proc, &port, 0);
    os_thread_detach(server_thread);

    OS_Handle conn_a = os_handle_zero();
    for (U64 attempt = 0; attempt < 40; attempt++)
    {
      conn_a = os_net_connect(str8_lit("127.0.0.1"), port);
      if (!os_handle_match(conn_a, os_handle_zero())) { break; }
      os_sleep_milliseconds(50);
    }

    if (os_handle_match(conn_a, os_handle_zero()))
    {
      log_error("net_client_server_session: could not connect to test server on port %u", (U32)port);
      pass = 0;
    }
    else
    {
      Temp scratch = scratch_begin(0, 0);

      //- tec: connection A - create db/table, insert rows, then SELECT back without repeating 'use'
      U8 status = 0;
      String8 payload = {0};

      B32 ok = ncs_request(scratch.arena, conn_a,
                            str8_lit("CREATE DATABASE net_session_test; USE net_session_test; "
                                     "CREATE TABLE t (id u32, name string8); "
                                     "INSERT INTO t (id, name) VALUES (1, 'alpha'), (2, 'beta');"),
                            &status, &payload);
      if (!ok || status != 0)
      {
        log_error("net_client_server_session: setup request failed (ok=%d status=%u payload='%.*s')", ok, (U32)status, str8_varg(payload));
        pass = 0;
      }

      ok = ncs_request(scratch.arena, conn_a, str8_lit("SELECT id, name FROM t ORDER BY id ASC;"), &status, &payload);
      if (!ok || status != 0 || !ncs_payload_contains(payload, str8_lit("1 alpha")) || !ncs_payload_contains(payload, str8_lit("2 beta")))
      {
        log_error("net_client_server_session: FAIL - session did not persist 'use' across requests on the same connection (ok=%d status=%u payload='%.*s')",
                   ok, (U32)status, str8_varg(payload));
        pass = 0;
      }
      else
      {
        log_info("net_client_server_session: per-connection session persistence OK");
      }

      os_net_close(conn_a);

      //- tec: connection B
      OS_Handle conn_b = os_net_connect(str8_lit("127.0.0.1"), port);
      if (os_handle_match(conn_b, os_handle_zero()))
      {
        log_error("net_client_server_session: could not open second connection");
        pass = 0;
      }
      else
      {
        ok = ncs_request(scratch.arena, conn_b, str8_lit("SELECT id, name FROM t;"), &status, &payload);
        if (!ok || status == 0)
        {
          log_error("net_client_server_session: FAIL - fresh connection unexpectedly succeeded without 'use' (ok=%d status=%u)", ok, (U32)status);
          pass = 0;
        }
        else if (!ncs_payload_contains(payload, str8_lit("no database selected")))
        {
          log_error("net_client_server_session: FAIL - error payload didn't explain the problem: '%.*s'", str8_varg(payload));
          pass = 0;
        }
        else
        {
          log_info("net_client_server_session: per-connection session isolation OK, server survived + reported the error");
        }

        //- tec: the database created on connection A is loaded server wide,
        // so connection B can 'use' it directly without the server rereading it from disk
        ok = ncs_request(scratch.arena, conn_b, str8_lit("USE net_session_test; SELECT COUNT(*) FROM t;"), &status, &payload);
        if (!ok || status != 0 || !ncs_payload_contains(payload, str8_lit("2")))
        {
          log_error("net_client_server_session: FAIL - second connection couldn't see database created by the first (ok=%d status=%u payload='%.*s')",
                     ok, (U32)status, str8_varg(payload));
          pass = 0;
        }
        else
        {
          log_info("net_client_server_session: cross-connection shared database state OK");
        }

        os_net_close(conn_b);
      }

      scratch_end(scratch);
    }
  }

  if (pass)
  {
    log_info("net_client_server_session: PASS");
  }
  else
  {
    log_error("net_client_server_session: FAIL");
  }

  ProfEnd();
  ProfEndCapture();
  log_release();
}
