global Arena *g_server_arena = 0;

typedef struct SRV_ConnCtx SRV_ConnCtx;
struct SRV_ConnCtx
{
  OS_Handle socket;
};

internal void
server_connection_thread_proc(void *ptr)
{
  SRV_ConnCtx *ctx = (SRV_ConnCtx*)ptr;
  OS_Handle socket = ctx->socket;

  Arena *arena = arena_alloc(.reserve_size=Max(GB(1), GPU_MAX_BUFFER_SIZE), .commit_size=MB(64));

  APP_QueryResult result = {0};
  B32 ok = 0;

  U64 sql_len = 0;
  if (os_net_recv_exact(socket, &sql_len, sizeof(sql_len)) && sql_len > 0 && sql_len < GB(1))
  {
    U8 *sql_bytes = push_array_no_zero(arena, U8, sql_len);
    if (os_net_recv_exact(socket, sql_bytes, sql_len))
    {
      String8 sql_query = str8(sql_bytes, sql_len);
      OS_MutexScope(g_query_exec_mutex) { result = app_execute_query_capture(arena, sql_query); }
      ok = !result.had_parse_error;
    }
  }

  U8 status = ok ? 0 : 1;
  String8 payload = result.output_text;
  if (!ok && payload.size == 0)
  {
    payload = str8_lit("query failed - see server log for details");
  }

  if (os_net_send_exact(socket, &status, sizeof(status)))
  {
    U64 payload_len = payload.size;
    if (os_net_send_exact(socket, &payload_len, sizeof(payload_len)) && payload_len > 0)
    {
      os_net_send_exact(socket, payload.str, payload_len);
    }
  }

  os_net_close(socket);
  arena_release(arena);
}

internal void
server_run(U16 port)
{
  g_server_arena = arena_alloc(.reserve_size=MB(64), .commit_size=KB(64));

  OS_Handle listen_socket = os_net_listen(port, 16);
  if (os_handle_match(listen_socket, os_handle_zero()))
  {
    log_error("server_run: failed to listen on port %u", (U32)port);
    return;
  }

  log_info("server listening on port %u", (U32)port);

  for (;;)
  {
    OS_Handle conn = os_net_accept(listen_socket, 0, 0);
    if (os_handle_match(conn, os_handle_zero())) { continue; }

    SRV_ConnCtx *ctx = push_array(g_server_arena, SRV_ConnCtx, 1);
    ctx->socket = conn;

    OS_Handle thread = os_thread_launch(server_connection_thread_proc, ctx, 0);
    os_thread_detach(thread);
  }
}
