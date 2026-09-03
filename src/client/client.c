internal void
client_run_one_shot(String8 host, U16 port, String8 sql_query)
{
  OS_Handle conn = os_net_connect(host, port);
  if (os_handle_match(conn, os_handle_zero()))
  {
    log_error("client: failed to connect to %.*s:%u", str8_varg(host), (U32)port);
    return;
  }

  U64 sql_len = sql_query.size;
  B32 ok = os_net_send_exact(conn, &sql_len, sizeof(sql_len)) &&
           os_net_send_exact(conn, sql_query.str, sql_len);

  if (!ok)
  {
    log_error("client: failed to send request");
    os_net_close(conn);
    return;
  }

  U8 status = 0;
  U64 payload_len = 0;
  ok = os_net_recv_exact(conn, &status, sizeof(status)) &&
       os_net_recv_exact(conn, &payload_len, sizeof(payload_len));

  if (!ok)
  {
    log_error("client: failed to read response header");
    os_net_close(conn);
    return;
  }

  if (payload_len > 0)
  {
    Temp scratch = scratch_begin(0, 0);
    U8 *payload_bytes = push_array_no_zero(scratch.arena, U8, payload_len);

    if (os_net_recv_exact(conn, payload_bytes, payload_len))
    {
      String8 payload = str8(payload_bytes, payload_len);
      if (status == 0) { printf("%.*s", str8_varg(payload)); }
      else              { fprintf(stderr, "%.*s\n", str8_varg(payload)); }
    }
    else
    {
      log_error("client: failed to read response payload");
    }

    scratch_end(scratch);
  }
  else if (status != 0)
  {
    log_error("client: query failed");
  }

  os_net_close(conn);
}
