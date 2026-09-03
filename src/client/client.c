internal B32
client_send_query_and_print(OS_Handle conn, String8 sql_query)
{
  U64 sql_len = sql_query.size;
  B32 ok = os_net_send_exact(conn, &sql_len, sizeof(sql_len)) &&
    os_net_send_exact(conn, sql_query.str, sql_len);
  
  if (!ok)
  {
    log_error("client: failed to send request");
    return 0;
  }
  
  U8 status = 0;
  U64 payload_len = 0;
  ok = os_net_recv_exact(conn, &status, sizeof(status)) &&
    os_net_recv_exact(conn, &payload_len, sizeof(payload_len));
  
  if (!ok)
  {
    log_error("client: failed to read response header");
    return 0;
  }
  
  if (payload_len > 0)
  {
    Temp scratch = scratch_begin(0, 0);
    U8 *payload_bytes = push_array_no_zero(scratch.arena, U8, payload_len);
    
    ok = os_net_recv_exact(conn, payload_bytes, payload_len);
    if (ok)
    {
      String8 payload = str8(payload_bytes, payload_len);
      if (status == 0)
      { 
        printf("%.*s", str8_varg(payload)); 
      }
      else  
      { 
        fprintf(stderr, "%.*s\n", str8_varg(payload)); 
      }
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
  
  return ok;
}

internal void
client_run_one_shot(String8 host, U16 port, String8 sql_query)
{
  OS_Handle conn = os_net_connect(host, port);
  if (os_handle_match(conn, os_handle_zero()))
  {
    log_error("client: failed to connect to %.*s:%u", str8_varg(host), (U32)port);
    return;
  }
  
  client_send_query_and_print(conn, sql_query);
  
  os_net_close(conn);
}

internal B32
client_line_is_quit_command(String8 line)
{
  return str8_match(line, str8_lit("quit"), StringMatchFlag_CaseInsensitive) ||
    str8_match(line, str8_lit("quit;"), StringMatchFlag_CaseInsensitive) ||
    str8_match(line, str8_lit("exit"), StringMatchFlag_CaseInsensitive) ||
    str8_match(line, str8_lit("exit;"), StringMatchFlag_CaseInsensitive) ||
    str8_match(line, str8_lit("\\q"), StringMatchFlag_CaseInsensitive);
}

internal void
client_run_interactive(String8 host, U16 port)
{
  OS_Handle conn = os_net_connect(host, port);
  if (os_handle_match(conn, os_handle_zero()))
  {
    log_error("client: failed to connect to %.*s:%u", str8_varg(host), (U32)port);
    return;
  }
  
  printf("connected to %.*s:%u - one SQL statement (or ';'-separated statements) per line, 'quit' to exit\n",
         str8_varg(host), (U32)port);
  
  char line_buf[64 * 1024];
  for (;;)
  {
    printf("gdb> ");
    fflush(stdout);
    
    if (!fgets(line_buf, sizeof(line_buf), stdin))
    {
      printf("\n");
      // tec: EOF ends the session
      break; 
    }
    
    String8 line = str8_skip_chop_whitespace(str8_cstring(line_buf));
    if (line.size == 0) continue;
    if (client_line_is_quit_command(line)) break;
    
    if (!client_send_query_and_print(conn, line))
    {
      log_error("client: connection lost - ending session");
      break;
    }
  }
  
  os_net_close(conn);
}
