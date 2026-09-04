global Arena *g_pg_server_arena = 0;

typedef struct PG_ConnCtx PG_ConnCtx;
struct PG_ConnCtx
{
  OS_Handle socket;
};

//~ tec: extended query protocol session state
// tec: a prepared statement holds unsubstituted '$N' text; a portal holds it with values filled in
#define PG_MAX_PREPARED_STATEMENTS 64
#define PG_MAX_PORTALS 64
#define PG_MAX_BIND_PARAMS 100

typedef struct PG_PreparedStatement PG_PreparedStatement;
struct PG_PreparedStatement
{
  B32 in_use;
  String8 name;
  String8 sql_text; // unsubstituted '$1'/'$2'/... placeholders
  U32 param_count;
  U32 param_oids[PG_MAX_BIND_PARAMS]; // 0 = unknown
};

typedef struct PG_Portal PG_Portal;
struct PG_Portal
{
  B32 in_use;
  String8 name;
  String8 bound_sql_text;
  B32 looks_like_select;
  // pe column, reuses the bind-param cap
  U16 result_format_codes[PG_MAX_BIND_PARAMS];
  U16 result_format_code_count;

  // tec: run lazily by Describe or Execute (whichever comes first) and cached
  B32 executed;
  B32 had_error;
  String8 error_message;
  APP_ResultSet result_set;
  U64 rows_sent; // for a max_rows limited Execute / PortalSuspended
};

typedef struct PG_Session PG_Session;
struct PG_Session
{
  Arena *arena;
  
  GDB_Database *current_database;

  // tec: after an error, Parse/Bind/Describe/Execute/Close are discarded until the next Sync
  B32 in_failed_pipeline;

  PG_PreparedStatement statements[PG_MAX_PREPARED_STATEMENTS];
  PG_Portal portals[PG_MAX_PORTALS];
};

internal PG_PreparedStatement*
pg_session_find_statement(PG_Session *session, String8 name)
{
  for (U32 i = 0; i < PG_MAX_PREPARED_STATEMENTS; i++)
  {
    if (session->statements[i].in_use && str8_match(session->statements[i].name, name, 0))
    {
      return &session->statements[i];
    }
  }
  return 0;
}

internal PG_PreparedStatement*
pg_session_alloc_statement(PG_Session *session, String8 name)
{
  // tec: reparse under an existing name just replaces it
  // this is more lenient than Postgres, should this be more strict?
  PG_PreparedStatement *existing = pg_session_find_statement(session, name);
  if (existing) { existing->in_use = 0; }

  for (U32 i = 0; i < PG_MAX_PREPARED_STATEMENTS; i++)
  {
    if (!session->statements[i].in_use)
    {
      MemoryZeroStruct(&session->statements[i]);
      session->statements[i].in_use = 1;
      return &session->statements[i];
    }
  }
  return 0;
}

internal void
pg_session_close_statement(PG_Session *session, String8 name)
{
  PG_PreparedStatement *s = pg_session_find_statement(session, name);
  if (s) { s->in_use = 0; }
}

internal PG_Portal*
pg_session_find_portal(PG_Session *session, String8 name)
{
  for (U32 i = 0; i < PG_MAX_PORTALS; i++)
  {
    if (session->portals[i].in_use && str8_match(session->portals[i].name, name, 0))
    {
      return &session->portals[i];
    }
  }
  return 0;
}

internal PG_Portal*
pg_session_alloc_portal(PG_Session *session, String8 name)
{
  PG_Portal *existing = pg_session_find_portal(session, name);
  if (existing) { existing->in_use = 0; }

  for (U32 i = 0; i < PG_MAX_PORTALS; i++)
  {
    if (!session->portals[i].in_use)
    {
      MemoryZeroStruct(&session->portals[i]);
      session->portals[i].in_use = 1;
      return &session->portals[i];
    }
  }
  return 0;
}

internal void
pg_session_close_portal(PG_Session *session, String8 name)
{
  PG_Portal *p = pg_session_find_portal(session, name);
  if (p) { p->in_use = 0; }
}

internal void
pg_portal_ensure_executed(PG_Session *session, PG_Portal *portal)
{
  if (portal->executed) { return; }

  U64 error_count_before = log_error_count();
  APP_QueryResult query_result = {0};
  GDB_Database *db = session->current_database;
  OS_MutexScope(g_query_exec_mutex)
  {
    query_result = app_execute_query_capture(session->arena, portal->bound_sql_text, &db, &portal->result_set);
  }
  B32 had_runtime_error = log_error_count() > error_count_before;

  if (query_result.had_parse_error || had_runtime_error)
  {
    portal->had_error = 1;
    String8 msg = log_last_error(session->arena);
    portal->error_message = msg.size ? msg : str8_lit("query failed - see server log for details");
  }
  else
  {
    session->current_database = db;
  }

  portal->executed = 1;
}

//~ tec: cursor reader for extended-protocol message bodies
typedef struct PG_Reader PG_Reader;
struct PG_Reader
{
  U8 *data;
  U64 size;
  U64 pos;
  B32 error;
};

internal PG_Reader
pg_reader_make(String8 body)
{
  PG_Reader r = {body.str, body.size, 0, 0};
  return r;
}

internal U8
pg_reader_u8(PG_Reader *r)
{
  if (r->error || r->pos + 1 > r->size) { r->error = 1; return 0; }
  U8 v = r->data[r->pos];
  r->pos += 1;
  return v;
}

internal U16
pg_reader_u16(PG_Reader *r)
{
  if (r->error || r->pos + 2 > r->size) { r->error = 1; return 0; }
  U16 v = ((U16)r->data[r->pos] << 8) | (U16)r->data[r->pos + 1];
  r->pos += 2;
  return v;
}

internal U32
pg_reader_u32(PG_Reader *r)
{
  if (r->error || r->pos + 4 > r->size) { r->error = 1; return 0; }
  U32 v = pg_read_be32(r->data + r->pos);
  r->pos += 4;
  return v;
}

internal S32
pg_reader_s32(PG_Reader *r)
{
  return (S32)pg_reader_u32(r);
}

internal String8
pg_reader_cstr(PG_Reader *r)
{
  if (r->error || r->pos >= r->size) { r->error = 1; return (String8){0}; }
  U64 start = r->pos;
  while (r->pos < r->size && r->data[r->pos] != 0) { r->pos++; }
  if (r->pos >= r->size) { r->error = 1; return (String8){0}; }
  String8 s = str8(r->data + start, r->pos - start);
  r->pos += 1;
  return s;
}

internal String8
pg_reader_bytes(PG_Reader *r, U64 n)
{
  if (r->error || r->pos + n > r->size) { r->error = 1; return (String8){0}; }
  String8 s = str8(r->data + r->pos, n);
  r->pos += n;
  return s;
}

// tec: NULL isnt valid outside 'IS [NOT] NULL' in this grammar, so use 0/'' instead
internal String8
pg_dummy_literal_for_oid(U32 oid)
{
  switch (oid)
  {
    case PG_OID_INT2: case PG_OID_INT4: case PG_OID_INT8:
    case PG_OID_FLOAT4: case PG_OID_FLOAT8: case PG_OID_BOOL:
      return str8_lit("0");
    default:
      return str8_lit("''");
  }
}

// tec: validates the startup "database" param before it's spliced into a server-built "USE ...;"
internal B32
pg_is_valid_identifier(String8 s)
{
  if (s.size == 0 || s.size > 128) { return 0; }
  if (!(char_is_alpha(s.str[0]) || s.str[0] == '_')) { return 0; }
  for (U64 i = 1; i < s.size; i++)
  {
    U8 c = s.str[i];
    if (!(char_is_alpha(c) || char_is_digit(c, 10) || c == '_')) { return 0; }
  }
  return 1;
}

internal void
pg_handle_simple_query(Arena *arena, OS_Handle socket, GDB_Database **current_database, String8 sql)
{
  String8 trimmed = str8_skip_chop_whitespace(sql);
  if (trimmed.size == 0)
  {
    pg_send_empty_query_response(socket, arena);
    pg_send_ready_for_query(socket, arena, 'I');
    return;
  }

  APP_ResultSet result_set = {0};
  APP_QueryResult result = {0};
  U64 error_count_before = log_error_count();
  OS_MutexScope(g_query_exec_mutex) { result = app_execute_query_capture(arena, sql, current_database, &result_set); }
  B32 had_runtime_error = log_error_count() > error_count_before;
  B32 ok = !result.had_parse_error && !had_runtime_error;

  if (!ok)
  {
    String8 message = log_last_error(arena);
    if (message.size == 0) { message = str8_lit("query failed - see server log for details"); }
    pg_send_error_response(socket, arena, str8_lit("ERROR"), str8_lit("XX000"), message);
    pg_send_ready_for_query(socket, arena, 'I');
    return;
  }

  if (result_set.valid)
  {
    pg_send_row_description_from_result_set(socket, arena, &result_set);
    pg_send_data_rows_from_result_set(socket, arena, &result_set);
    pg_send_command_complete(socket, arena, push_str8f(arena, "SELECT %llu", result_set.row_count));
    pg_send_ready_for_query(socket, arena, 'I');
    return;
  }

  if (result.output_text.size > 0)
  {
    // tec: DESCRIBE/EXPLAIN have no typed columns. present as one "result" column, one row per line
    pg_send_row_description_single_text_column(socket, arena, str8_lit("result"));

    String8List lines = str8_split_by_string_chars(arena, result.output_text, str8_lit("\n"), 0);
    for (String8Node *n = lines.first; n != 0; n = n->next)
    {
      pg_send_data_row_single_text_column(socket, arena, n->string);
    }

    pg_send_command_complete(socket, arena, pg_command_tag_from_sql(arena, sql));
    pg_send_ready_for_query(socket, arena, 'I');
    return;
  }

  pg_send_command_complete(socket, arena, pg_command_tag_from_sql(arena, sql));
  pg_send_ready_for_query(socket, arena, 'I');
}

internal void
pg_handle_parse(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body)
{
  PG_Reader r = pg_reader_make(msg_body);
  String8 stmt_name = pg_reader_cstr(&r);
  String8 query = pg_reader_cstr(&r);
  U16 num_param_types = pg_reader_u16(&r);

  U32 param_oids[PG_MAX_BIND_PARAMS];
  U32 param_count = Min((U32)num_param_types, (U32)PG_MAX_BIND_PARAMS);
  for (U32 i = 0; i < param_count; i++) { param_oids[i] = pg_reader_u32(&r); }
  // tec: drain any beyond cap
  for (U32 i = param_count; i < num_param_types; i++) { pg_reader_u32(&r); } 

  if (r.error)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("08P01"), str8_lit("malformed Parse message"));
    session->in_failed_pipeline = 1;
    return;
  }

  PG_PreparedStatement *stmt = pg_session_alloc_statement(session, stmt_name);
  if (!stmt)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("53000"), str8_lit("too many prepared statements on this connection"));
    session->in_failed_pipeline = 1;
    return;
  }

  stmt->name = push_str8_copy(session->arena, stmt_name);
  stmt->sql_text = push_str8_copy(session->arena, query);
  stmt->param_count = param_count;
  MemoryCopy(stmt->param_oids, param_oids, sizeof(U32) * param_count);

  pg_send_parse_complete(socket, msg_arena);
}

internal void
pg_handle_bind(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body)
{
  PG_Reader r = pg_reader_make(msg_body);
  String8 portal_name = pg_reader_cstr(&r);
  String8 stmt_name = pg_reader_cstr(&r);

  U16 num_format_codes = pg_reader_u16(&r);
  U16 format_codes[PG_MAX_BIND_PARAMS];
  U16 format_code_count = Min(num_format_codes, (U16)PG_MAX_BIND_PARAMS);
  for (U16 i = 0; i < format_code_count; i++) { format_codes[i] = pg_reader_u16(&r); }
  for (U16 i = format_code_count; i < num_format_codes; i++) { pg_reader_u16(&r); }

  U16 num_param_values = pg_reader_u16(&r);

  if (r.error)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("08P01"), str8_lit("malformed Bind message"));
    session->in_failed_pipeline = 1;
    return;
  }

  PG_PreparedStatement *stmt = pg_session_find_statement(session, stmt_name);
  if (!stmt)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("26000"),
                            push_str8f(msg_arena, "prepared statement \"%.*s\" does not exist", str8_varg(stmt_name)));
    session->in_failed_pipeline = 1;
    return;
  }

  String8 literals[PG_MAX_BIND_PARAMS];
  U32 literal_count = 0;
  B32 bind_ok = 1;
  String8 bind_error = {0};

  for (U32 i = 0; i < (U32)num_param_values; i++)
  {
    S32 len = pg_reader_s32(&r);
    if (r.error) { bind_ok = 0; bind_error = str8_lit("malformed Bind message"); break; }

    B32 is_null = (len == -1);
    String8 raw = {0};
    if (!is_null)
    {
      if (len < 0) { bind_ok = 0; bind_error = str8_lit("malformed Bind message (invalid parameter length)"); break; }
      raw = pg_reader_bytes(&r, (U64)len);
      if (r.error) { bind_ok = 0; bind_error = str8_lit("malformed Bind message (truncated parameter value)"); break; }
    }

    if (i >= PG_MAX_BIND_PARAMS)
    {
      bind_ok = 0;
      bind_error = str8_lit("too many bind parameters");
      break;
    }

    U16 fmt = (format_code_count == 0) ? 0 : (format_code_count == 1 ? format_codes[0] : format_codes[i < format_code_count ? i : 0]);
    U32 declared_oid = (i < stmt->param_count) ? stmt->param_oids[i] : 0;

    String8 literal = {0};
    String8 err = {0};
    if (!pg_param_to_sql_literal(session->arena, declared_oid, fmt, raw, is_null, &literal, &err))
    {
      bind_ok = 0;
      bind_error = err;
      break;
    }
    literals[i] = literal;
    literal_count = i + 1;
  }

  U16 result_format_code_count = 0;
  U16 result_format_codes[PG_MAX_BIND_PARAMS];
  if (bind_ok)
  {
    U16 num_result_format_codes = pg_reader_u16(&r);
    result_format_code_count = Min(num_result_format_codes, (U16)PG_MAX_BIND_PARAMS);
    for (U16 i = 0; i < result_format_code_count; i++) { result_format_codes[i] = pg_reader_u16(&r); }
    for (U16 i = result_format_code_count; i < num_result_format_codes; i++) { pg_reader_u16(&r); }

    if (r.error) { bind_ok = 0; bind_error = str8_lit("malformed Bind message"); }
  }

  if (!bind_ok)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("22P02"), bind_error.size ? bind_error : str8_lit("bind parameter error"));
    session->in_failed_pipeline = 1;
    return;
  }

  PG_Portal *portal = pg_session_alloc_portal(session, portal_name);
  if (!portal)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("53000"), str8_lit("too many portals on this connection"));
    session->in_failed_pipeline = 1;
    return;
  }

  portal->name = push_str8_copy(session->arena, portal_name);
  portal->bound_sql_text = pg_substitute_params(session->arena, stmt->sql_text, literals, literal_count);
  portal->looks_like_select = pg_looks_like_select(stmt->sql_text);
  portal->result_format_code_count = result_format_code_count;
  MemoryCopy(portal->result_format_codes, result_format_codes, sizeof(U16) * result_format_code_count);

  pg_send_bind_complete(socket, msg_arena);
}

internal void
pg_handle_describe(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body)
{
  PG_Reader r = pg_reader_make(msg_body);
  U8 kind = pg_reader_u8(&r);
  String8 name = pg_reader_cstr(&r);

  if (r.error)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("08P01"), str8_lit("malformed Describe message"));
    session->in_failed_pipeline = 1;
    return;
  }

  if (kind == 'S')
  {
    PG_PreparedStatement *stmt = pg_session_find_statement(session, name);
    if (!stmt)
    {
      pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("26000"),
                              push_str8f(msg_arena, "prepared statement \"%.*s\" does not exist", str8_varg(name)));
      session->in_failed_pipeline = 1;
      return;
    }

    pg_send_parameter_description(socket, msg_arena, stmt->param_oids, stmt->param_count);

    if (pg_looks_like_select(stmt->sql_text))
    {
      // tec: dummy values just to learn the result shape before any real Bind happens
      // tec: must be a real arena. a scratch one here can alias and corrupt out_result_set
      Arena *shape_arena = arena_alloc(.reserve_size=MB(16), .commit_size=KB(64));

      U32 dummy_count = stmt->param_count;
      String8 dummy_literals[PG_MAX_BIND_PARAMS];
      for (U32 i = 0; i < dummy_count; i++) { dummy_literals[i] = pg_dummy_literal_for_oid(stmt->param_oids[i]); }
      String8 shape_sql = pg_substitute_params(shape_arena, stmt->sql_text, dummy_literals, dummy_count);

      APP_ResultSet shape_result_set = {0};
      GDB_Database *shape_db = session->current_database;
      OS_MutexScope(g_query_exec_mutex) { app_execute_query_capture(shape_arena, shape_sql, &shape_db, &shape_result_set); }

      if (shape_result_set.valid) { pg_send_row_description_from_result_set_ex(socket, msg_arena, &shape_result_set, 0, 0); }
      else                        { pg_send_no_data(socket, msg_arena); }

      arena_release(shape_arena);
    }
    else
    {
      pg_send_no_data(socket, msg_arena);
    }
  }
  else if (kind == 'P')
  {
    PG_Portal *portal = pg_session_find_portal(session, name);
    if (!portal)
    {
      pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("34000"),
                              push_str8f(msg_arena, "portal \"%.*s\" does not exist", str8_varg(name)));
      session->in_failed_pipeline = 1;
      return;
    }

    if (!portal->looks_like_select)
    {
      pg_send_no_data(socket, msg_arena);
      return;
    }

    pg_portal_ensure_executed(session, portal);

    if (portal->had_error)
    {
      pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("XX000"), portal->error_message);
      session->in_failed_pipeline = 1;
    }
    else if (portal->result_set.valid)
    {
      pg_send_row_description_from_result_set_ex(socket, msg_arena, &portal->result_set, portal->result_format_codes, portal->result_format_code_count);
    }
    else
    {
      pg_send_no_data(socket, msg_arena);
    }
  }
  else
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("08P01"),
                            push_str8f(msg_arena, "invalid Describe target type '%c'", (char)kind));
    session->in_failed_pipeline = 1;
  }
}

internal void
pg_handle_execute(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body)
{
  PG_Reader r = pg_reader_make(msg_body);
  String8 portal_name = pg_reader_cstr(&r);
  S32 max_rows = pg_reader_s32(&r);

  if (r.error)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("08P01"), str8_lit("malformed Execute message"));
    session->in_failed_pipeline = 1;
    return;
  }

  PG_Portal *portal = pg_session_find_portal(session, portal_name);
  if (!portal)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("34000"),
                            push_str8f(msg_arena, "portal \"%.*s\" does not exist", str8_varg(portal_name)));
    session->in_failed_pipeline = 1;
    return;
  }

  pg_portal_ensure_executed(session, portal);

  if (portal->had_error)
  {
    pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("XX000"), portal->error_message);
    session->in_failed_pipeline = 1;
    return;
  }

  if (portal->result_set.valid)
  {
    U64 remaining = portal->result_set.row_count - portal->rows_sent;
    U64 to_send = (max_rows > 0) ? Min((U64)max_rows, remaining) : remaining;

    pg_send_data_rows_from_result_set_range(socket, msg_arena, &portal->result_set,
                                             portal->result_format_codes, portal->result_format_code_count,
                                             portal->rows_sent, to_send);
    portal->rows_sent += to_send;

    if (max_rows > 0 && portal->rows_sent < portal->result_set.row_count)
    {
      pg_send_portal_suspended(socket, msg_arena);
    }
    else
    {
      pg_send_command_complete(socket, msg_arena, push_str8f(msg_arena, "SELECT %llu", portal->rows_sent));
    }
  }
  else
  {
    pg_send_command_complete(socket, msg_arena, pg_command_tag_from_sql(msg_arena, portal->bound_sql_text));
  }
}

internal void
pg_handle_close(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body)
{
  PG_Reader r = pg_reader_make(msg_body);
  U8 kind = pg_reader_u8(&r);
  String8 name = pg_reader_cstr(&r);

  if (!r.error)
  {
    if (kind == 'S')      { pg_session_close_statement(session, name); }
    else if (kind == 'P') { pg_session_close_portal(session, name); }
  }

  pg_send_close_complete(socket, msg_arena);
}

// tec: returns 0 if the connection should just be closed (handshake failed or cancel request)
internal B32
pg_do_startup_handshake(Arena *arena, OS_Handle socket, GDB_Database **out_current_database)
{
  String8 startup_params = {0};

  for (U64 attempt = 0; attempt < 8; attempt++)
  {
    String8 body = {0};
    if (!pg_recv_untyped(arena, socket, &body)) { return 0; }
    if (body.size < 4) { return 0; }

    U32 code = pg_read_be32(body.str);

    if (code == PG_SSL_REQUEST_CODE || code == PG_GSSENC_REQUEST_CODE)
    {
	  // no encryption
      if (!pg_send_raw_byte(socket, 'N')) { return 0; } 
      continue;
    }

    if (code == PG_CANCEL_REQUEST_CODE)
    {
	  // no response is sent for this per protocol; dont cancel in flight queries
      return 0; 
    }

	// protocol 3.x - the real StartupMessage
    if ((code >> 16) == 3) 
    {
      startup_params = str8_skip(body, 4);
      break;
    }

    pg_send_error_response(socket, arena, str8_lit("FATAL"), str8_lit("08004"),
                            push_str8f(arena, "unsupported protocol version 0x%08x - this server speaks protocol 3.0", code));
    return 0;
  }

  if (startup_params.str == 0) { return 0; }

  String8 database_param = pg_startup_param_find(startup_params, str8_lit("database"));

  if (database_param.size > 0 && !pg_is_valid_identifier(database_param))
  {
    pg_send_error_response(socket, arena, str8_lit("FATAL"), str8_lit("3D000"), str8_lit("invalid database name"));
    return 0;
  }

  if (!pg_send_authentication_ok(socket, arena)) { return 0; }

  pg_send_parameter_status(socket, arena, str8_lit("server_version"), str8_lit("14.0"));
  pg_send_parameter_status(socket, arena, str8_lit("client_encoding"), str8_lit("UTF8"));
  pg_send_parameter_status(socket, arena, str8_lit("server_encoding"), str8_lit("UTF8"));
  pg_send_parameter_status(socket, arena, str8_lit("DateStyle"), str8_lit("ISO, MDY"));
  pg_send_parameter_status(socket, arena, str8_lit("integer_datetimes"), str8_lit("on"));
  pg_send_parameter_status(socket, arena, str8_lit("standard_conforming_strings"), str8_lit("on"));

  U32 fake_pid = (U32)os_now_microseconds();
  U32 fake_secret = (U32)(os_now_microseconds() ^ 0x5bd1e995u);
  if (!pg_send_backend_key_data(socket, arena, fake_pid, fake_secret)) { return 0; }

  *out_current_database = 0;
  if (database_param.size > 0)
  {
    String8 use_sql = push_str8f(arena, "USE %.*s;", str8_varg(database_param));
    APP_QueryResult use_result = {0};
    OS_MutexScope(g_query_exec_mutex) { use_result = app_execute_query_capture(arena, use_sql, out_current_database, 0); }
    (void)use_result;
  }

  if (!pg_send_ready_for_query(socket, arena, 'I')) { return 0; }

  return 1;
}

internal void
pg_connection_thread_proc(void *ptr)
{
  PG_ConnCtx *ctx = (PG_ConnCtx*)ptr;
  OS_Handle socket = ctx->socket;

  log_info("pg server: connection opened");

  Arena *handshake_arena = arena_alloc(.reserve_size=MB(16), .commit_size=KB(64));
  GDB_Database *current_database = 0;
  B32 handshake_ok = pg_do_startup_handshake(handshake_arena, socket, &current_database);
  arena_release(handshake_arena);

  if (handshake_ok)
  {
    Arena *session_arena = arena_alloc(.reserve_size=GB(1), .commit_size=MB(16));
    PG_Session *session = push_array(session_arena, PG_Session, 1);
    session->arena = session_arena;
    session->current_database = current_database;

    for (;;)
    {
      Arena *msg_arena = arena_alloc(.reserve_size=Max(GB(1), GPU_MAX_BUFFER_SIZE), .commit_size=MB(64));

      U8 msg_type = 0;
      String8 msg_body = {0};
      if (!pg_recv_typed(msg_arena, socket, &msg_type, &msg_body))
      {
        arena_release(msg_arena);
        break;
      }

      B32 keep_going = 1;

      switch (msg_type)
      {
		// Simple Query. implicitly syncs
        case 'Q': 
        {
          session->in_failed_pipeline = 0;
          String8 sql = msg_body;
          if (sql.size > 0 && sql.str[sql.size - 1] == 0) { sql.size -= 1; }
          pg_handle_simple_query(msg_arena, socket, &session->current_database, sql);
        } break;

		// Parse
        case 'P': 
        {
          if (!session->in_failed_pipeline) { pg_handle_parse(session, socket, msg_arena, msg_body); }
        } break;

		// Bind
        case 'B': 
        {
          if (!session->in_failed_pipeline) { pg_handle_bind(session, socket, msg_arena, msg_body); }
        } break;

		// Describe
        case 'D': 
        {
          if (!session->in_failed_pipeline) { pg_handle_describe(session, socket, msg_arena, msg_body); }
        } break;

		// Execute
        case 'E': 
        {
          if (!session->in_failed_pipeline) { pg_handle_execute(session, socket, msg_arena, msg_body); }
        } break;

		// Close
        case 'C': 
        {
          if (!session->in_failed_pipeline) { pg_handle_close(session, socket, msg_arena, msg_body); }
        } break;

        case 'H':
		{
			// Flush. responses are already sent synchronously
		} break; 

		// Sync
        case 'S': 
        {
          session->in_failed_pipeline = 0;
          pg_send_ready_for_query(socket, msg_arena, 'I');
        } break;

		// Terminate
        case 'X': 
        {
          keep_going = 0;
        } break;

		// CopyData/CopyDone/CopyFail
        case 'd': case 'c': case 'f': 
        {
          pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("0A000"), str8_lit("COPY is not supported"));
          session->in_failed_pipeline = 1;
        } break;

        default:
        {
          pg_send_error_response(socket, msg_arena, str8_lit("ERROR"), str8_lit("08P01"),
                                  push_str8f(msg_arena, "unsupported frontend message type '%c' (0x%02x)", (char)msg_type, (U32)msg_type));
          session->in_failed_pipeline = 1;
        } break;
      }

      arena_release(msg_arena);
      if (!keep_going) { break; }
    }

    arena_release(session->arena);
  }

  log_info("pg server: connection closed");
  os_net_close(socket);
}

internal void
server_run_pg(U16 port)
{
  g_pg_server_arena = arena_alloc(.reserve_size=MB(64), .commit_size=KB(64));

  OS_Handle listen_socket = os_net_listen(port, 16);
  if (os_handle_match(listen_socket, os_handle_zero()))
  {
    log_error("server_run_pg: failed to listen on port %u", (U32)port);
    return;
  }

  log_info("postgres wire protocol server listening on port %u (trust auth only - no passwords, no TLS)", (U32)port);

  for (;;)
  {
    OS_Handle conn = os_net_accept(listen_socket, 0, 0);
    if (os_handle_match(conn, os_handle_zero())) { continue; }

    PG_ConnCtx *ctx = push_array(g_pg_server_arena, PG_ConnCtx, 1);
    ctx->socket = conn;

    OS_Handle thread = os_thread_launch(pg_connection_thread_proc, ctx, 0);
    os_thread_detach(thread);
  }
}
