internal U32
pg_oid_for_column_type(GDB_ColumnType type)
{
  switch (type)
  {
    case GDB_ColumnType_U32:     return 23;  // int4
    case GDB_ColumnType_U64:     return 20;  // int8
    case GDB_ColumnType_F32:     return 700; // float4
    case GDB_ColumnType_F64:     return 701; // float8
    case GDB_ColumnType_String8: return 25;  // text
    default:                     return 25;  // text
  }
}

internal S16
pg_typlen_for_column_type(GDB_ColumnType type)
{
  switch (type)
  {
    case GDB_ColumnType_U32: return 4;
    case GDB_ColumnType_U64: return 8;
    case GDB_ColumnType_F32: return 4;
    case GDB_ColumnType_F64: return 8;
    default:                 return -1; // tec: variable-length (text)
  }
}

//~ tec: big-endian encoding (wire protocol is network byte order, x64 is little-endian)
internal String8
pg_be32(Arena* arena, U32 v)
{
  U8* b = push_array_no_zero(arena, U8, 4);
  b[0] = (U8)(v >> 24); b[1] = (U8)(v >> 16); b[2] = (U8)(v >> 8); b[3] = (U8)v;
  return str8(b, 4);
}

internal String8
pg_be16(Arena* arena, U16 v)
{
  U8* b = push_array_no_zero(arena, U8, 2);
  b[0] = (U8)(v >> 8); b[1] = (U8)v;
  return str8(b, 2);
}

internal U32
pg_read_be32(U8* bytes)
{
  return ((U32)bytes[0] << 24) | ((U32)bytes[1] << 16) | ((U32)bytes[2] << 8) | (U32)bytes[3];
}

// tec: the wire format's "cstring": a NUL-terminated string
internal String8
pg_cstr(Arena* arena, String8 s)
{
  U8* b = push_array_no_zero(arena, U8, s.size + 1);
  MemoryCopy(b, s.str, s.size);
  b[s.size] = 0;
  return str8(b, s.size + 1);
}

internal B32
pg_recv_untyped(Arena* arena, OS_Handle conn, String8* out_body)
{
  U8 len_bytes[4];
  if (!os_net_recv_exact(conn, len_bytes, 4)) { return 0; }

  U32 len = pg_read_be32(len_bytes);
  if (len < 4 || len > MB(1)) { return 0; } // tec: cap against a hostile length

  U64 body_size = len - 4;
  U8* body = push_array_no_zero(arena, U8, Max(body_size, 1));
  if (body_size > 0 && !os_net_recv_exact(conn, body, body_size)) { return 0; }

  *out_body = str8(body, body_size);
  return 1;
}

internal B32
pg_recv_typed(Arena* arena, OS_Handle conn, U8* out_type, String8* out_body)
{
  U8 type = 0;
  if (!os_net_recv_exact(conn, &type, 1)) { return 0; }

  String8 body = {0};
  if (!pg_recv_untyped(arena, conn, &body)) { return 0; }

  *out_type = type;
  *out_body = body;
  return 1;
}

internal B32
pg_send_raw_byte(OS_Handle conn, U8 b)
{
  return os_net_send_exact(conn, &b, 1);
}

internal B32
pg_send_msg(OS_Handle conn, Arena* arena, U8 type, String8List* body_parts)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 body = body_parts ? str8_list_join(scratch.arena, body_parts, 0) : (String8){0};

  U8 header[5];
  header[0] = type;
  U32 len = (U32)(body.size + 4);
  header[1] = (U8)(len >> 24); header[2] = (U8)(len >> 16); header[3] = (U8)(len >> 8); header[4] = (U8)len;

  B32 ok = os_net_send_exact(conn, header, sizeof(header));
  if (ok && body.size > 0)
  {
    ok = os_net_send_exact(conn, body.str, body.size);
  }

  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_authentication_ok(OS_Handle conn, Arena* arena)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};
  str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, 0));
  B32 ok = pg_send_msg(conn, scratch.arena, 'R', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_parameter_status(OS_Handle conn, Arena* arena, String8 name, String8 value)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, name));
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, value));
  B32 ok = pg_send_msg(conn, scratch.arena, 'S', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_backend_key_data(OS_Handle conn, Arena* arena, U32 pid, U32 secret)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};
  str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, pid));
  str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, secret));
  B32 ok = pg_send_msg(conn, scratch.arena, 'K', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_ready_for_query(OS_Handle conn, Arena* arena, U8 status)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};
  U8* b = push_array_no_zero(scratch.arena, U8, 1);
  b[0] = status;
  str8_list_push(scratch.arena, &parts, str8(b, 1));
  B32 ok = pg_send_msg(conn, scratch.arena, 'Z', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_error_response(OS_Handle conn, Arena* arena, String8 severity, String8 sqlstate, String8 message)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};

  U8 severity_code = 'S';
  str8_list_push(scratch.arena, &parts, str8(&severity_code, 1));
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, severity));

  U8 sqlstate_code = 'C';
  str8_list_push(scratch.arena, &parts, str8(&sqlstate_code, 1));
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, sqlstate));

  U8 message_code = 'M';
  str8_list_push(scratch.arena, &parts, str8(&message_code, 1));
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, message));

  U8 terminator = 0;
  str8_list_push(scratch.arena, &parts, str8(&terminator, 1));

  B32 ok = pg_send_msg(conn, scratch.arena, 'E', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_empty_query_response(OS_Handle conn, Arena* arena)
{
  return pg_send_msg(conn, arena, 'I', 0);
}

internal B32
pg_send_command_complete(OS_Handle conn, Arena* arena, String8 tag)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, tag));
  B32 ok = pg_send_msg(conn, scratch.arena, 'C', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_row_description_from_result_set(OS_Handle conn, Arena* arena, APP_ResultSet* result_set)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};

  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)result_set->column_count));

  for (U64 c = 0; c < result_set->column_count; c++)
  {
    APP_ResultColumn* col = &result_set->columns[c];
    str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, col->name));
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, 0));  // table OID
    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, 0));  // column attribute number
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, pg_oid_for_column_type(col->type)));
    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)pg_typlen_for_column_type(col->type)));
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)-1)); // type modifier
    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, 0));  // format code: text
  }

  B32 ok = pg_send_msg(conn, scratch.arena, 'T', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_row_description_single_text_column(OS_Handle conn, Arena* arena, String8 column_name)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};

  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, 1));
  str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, column_name));
  str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, 0));
  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, 0));
  str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, 25)); // text
  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)(S16)-1));
  str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)-1));
  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, 0));

  B32 ok = pg_send_msg(conn, scratch.arena, 'T', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_data_rows_from_result_set(OS_Handle conn, Arena* arena, APP_ResultSet* result_set)
{
  for (U64 r = 0; r < result_set->row_count; r++)
  {
    Temp scratch = scratch_begin(&arena, 1);
    String8List parts = {0};

    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)result_set->column_count));

    for (U64 c = 0; c < result_set->column_count; c++)
    {
      U64 cell_i = r * result_set->column_count + c;
      if (result_set->cell_is_null[cell_i])
      {
        str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)-1)); // -1 length == NULL
      }
      else
      {
        String8 cell = result_set->cell_text[cell_i];
        str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)cell.size));
        str8_list_push(scratch.arena, &parts, cell);
      }
    }

    B32 ok = pg_send_msg(conn, scratch.arena, 'D', &parts);
    scratch_end(scratch);
    if (!ok) { return 0; }
  }

  return 1;
}

internal B32
pg_send_data_row_single_text_column(OS_Handle conn, Arena* arena, String8 value)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};

  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, 1));
  str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)value.size));
  str8_list_push(scratch.arena, &parts, value);

  B32 ok = pg_send_msg(conn, scratch.arena, 'D', &parts);
  scratch_end(scratch);
  return ok;
}

internal String8
pg_startup_param_find(String8 body, String8 key)
{
  // tec: "key\0value\0" pairs, terminated by an empty key
  U64 pos = 0;
  for (;;)
  {
    if (pos >= body.size) { break; }

    U64 key_start = pos;
    U64 key_end = str8_find_needle(body, pos, str8_lit("\x00"), 0);
    if (key_end >= body.size) { break; }
    String8 this_key = str8_substr(body, r1u64(key_start, key_end));
    if (this_key.size == 0) { break; }

    U64 value_start = key_end + 1;
    U64 value_end = str8_find_needle(body, value_start, str8_lit("\x00"), 0);
    if (value_end > body.size) { break; }
    String8 this_value = str8_substr(body, r1u64(value_start, value_end));

    if (str8_match(this_key, key, 0)) { return this_value; }

    pos = value_end + 1;
  }

  return str8_lit("");
}

internal B32
pg_looks_like_select(String8 sql)
{
  Temp scratch = scratch_begin(0, 0);
  String8 trimmed = str8_skip_chop_whitespace(sql);
  String8List words = str8_split_by_string_chars(scratch.arena, trimmed, str8_lit(" \t\r\n"), 0);
  String8 first = words.first ? words.first->string : str8_lit("");
  B32 result = str8_match(first, str8_lit("select"), StringMatchFlag_CaseInsensitive) ||
               str8_match(first, str8_lit("explain"), StringMatchFlag_CaseInsensitive) ||
               str8_match(first, str8_lit("describe"), StringMatchFlag_CaseInsensitive);
  scratch_end(scratch);
  return result;
}

internal U16
pg_result_format_code_for_column(U16* format_codes, U16 format_code_count, U64 column_index)
{
  if (format_code_count == 0) { return 0; }
  if (format_code_count == 1) { return format_codes[0]; }
  if (column_index < format_code_count) { return format_codes[column_index]; }
  return 0;
}

internal String8
pg_encode_binary_numeric(Arena* arena, GDB_ColumnType type, F64 value)
{
  switch (type)
  {
    case GDB_ColumnType_U32:
    {
      U8* b = push_array_no_zero(arena, U8, 4);
      U32 v = (U32)value;
      b[0] = (U8)(v >> 24); b[1] = (U8)(v >> 16); b[2] = (U8)(v >> 8); b[3] = (U8)v;
      return str8(b, 4);
    }
    case GDB_ColumnType_U64:
    {
      U8* b = push_array_no_zero(arena, U8, 8);
      U64 v = (U64)value;
      b[0] = (U8)(v >> 56); b[1] = (U8)(v >> 48); b[2] = (U8)(v >> 40); b[3] = (U8)(v >> 32);
      b[4] = (U8)(v >> 24); b[5] = (U8)(v >> 16); b[6] = (U8)(v >> 8);  b[7] = (U8)v;
      return str8(b, 8);
    }
    case GDB_ColumnType_F32:
    {
      F32 f = (F32)value;
      U32 bits; MemoryCopy(&bits, &f, sizeof(bits));
      U8* b = push_array_no_zero(arena, U8, 4);
      b[0] = (U8)(bits >> 24); b[1] = (U8)(bits >> 16); b[2] = (U8)(bits >> 8); b[3] = (U8)bits;
      return str8(b, 4);
    }
    case GDB_ColumnType_F64:
    {
      U64 bits; MemoryCopy(&bits, &value, sizeof(bits));
      U8* b = push_array_no_zero(arena, U8, 8);
      b[0] = (U8)(bits >> 56); b[1] = (U8)(bits >> 48); b[2] = (U8)(bits >> 40); b[3] = (U8)(bits >> 32);
      b[4] = (U8)(bits >> 24); b[5] = (U8)(bits >> 16); b[6] = (U8)(bits >> 8);  b[7] = (U8)bits;
      return str8(b, 8);
    }
    default: return str8_lit(""); // unreachable: only called for a numeric column
  }
}

internal B32
pg_send_row_description_from_result_set_ex(OS_Handle conn, Arena* arena, APP_ResultSet* result_set, U16* result_format_codes, U16 format_code_count)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};

  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)result_set->column_count));

  for (U64 c = 0; c < result_set->column_count; c++)
  {
    APP_ResultColumn* col = &result_set->columns[c];
    U16 fmt = pg_result_format_code_for_column(result_format_codes, format_code_count, c);
    str8_list_push(scratch.arena, &parts, pg_cstr(scratch.arena, col->name));
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, 0));
    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, 0));
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, pg_oid_for_column_type(col->type)));
    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)pg_typlen_for_column_type(col->type)));
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)-1));
    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, fmt));
  }

  B32 ok = pg_send_msg(conn, scratch.arena, 'T', &parts);
  scratch_end(scratch);
  return ok;
}

internal B32
pg_send_data_rows_from_result_set_range(OS_Handle conn, Arena* arena, APP_ResultSet* result_set, U16* result_format_codes, U16 format_code_count, U64 first_row, U64 row_count)
{
  U64 end_row = Min(first_row + row_count, result_set->row_count);

  for (U64 r = first_row; r < end_row; r++)
  {
    Temp scratch = scratch_begin(&arena, 1);
    String8List parts = {0};

    str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)result_set->column_count));

    for (U64 c = 0; c < result_set->column_count; c++)
    {
      U64 cell_i = r * result_set->column_count + c;

      if (result_set->cell_is_null[cell_i])
      {
        str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)-1));
        continue;
      }

      GDB_ColumnType type = result_set->columns[c].type;
      U16 fmt = pg_result_format_code_for_column(result_format_codes, format_code_count, c);

      if (fmt == 1 && type != GDB_ColumnType_String8)
      {
        String8 bin = pg_encode_binary_numeric(scratch.arena, type, result_set->cell_numeric[cell_i]);
        str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)bin.size));
        str8_list_push(scratch.arena, &parts, bin);
      }
      else
      {
        String8 cell = result_set->cell_text[cell_i];
        str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, (U32)cell.size));
        str8_list_push(scratch.arena, &parts, cell);
      }
    }

    B32 ok = pg_send_msg(conn, scratch.arena, 'D', &parts);
    scratch_end(scratch);
    if (!ok) { return 0; }
  }

  return 1;
}

internal B32 pg_send_no_data(OS_Handle conn, Arena* arena)         { return pg_send_msg(conn, arena, 'n', 0); }
internal B32 pg_send_parse_complete(OS_Handle conn, Arena* arena)  { return pg_send_msg(conn, arena, '1', 0); }
internal B32 pg_send_bind_complete(OS_Handle conn, Arena* arena)   { return pg_send_msg(conn, arena, '2', 0); }
internal B32 pg_send_close_complete(OS_Handle conn, Arena* arena)  { return pg_send_msg(conn, arena, '3', 0); }
internal B32 pg_send_portal_suspended(OS_Handle conn, Arena* arena) { return pg_send_msg(conn, arena, 's', 0); }

internal B32
pg_send_parameter_description(OS_Handle conn, Arena* arena, U32* param_oids, U32 param_count)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};

  str8_list_push(scratch.arena, &parts, pg_be16(scratch.arena, (U16)param_count));
  for (U32 i = 0; i < param_count; i++)
  {
    str8_list_push(scratch.arena, &parts, pg_be32(scratch.arena, param_oids[i]));
  }

  B32 ok = pg_send_msg(conn, scratch.arena, 't', &parts);
  scratch_end(scratch);
  return ok;
}

//~ tec: parameter handling

internal String8
pg_quote_sql_string_literal(Arena* arena, String8 value)
{
  U64 extra = 0;
  for (U64 i = 0; i < value.size; i++) { if (value.str[i] == '\'') { extra++; } }

  U8* buf = push_array_no_zero(arena, U8, value.size + extra + 2);
  U64 o = 0;
  buf[o++] = '\'';
  for (U64 i = 0; i < value.size; i++)
  {
    if (value.str[i] == '\'') { buf[o++] = '\''; buf[o++] = '\''; }
    else                      { buf[o++] = value.str[i]; }
  }
  buf[o++] = '\'';

  return str8(buf, o);
}

// tec: strict [+-]?digits(.digits)? - anything else gets quoted as a string instead
internal B32
pg_text_looks_numeric(String8 s)
{
  if (s.size == 0) { return 0; }

  U64 i = 0;
  if (s.str[i] == '+' || s.str[i] == '-') { i++; }

  B32 has_digit = 0;
  B32 has_dot = 0;
  for (; i < s.size; i++)
  {
    U8 c = s.str[i];
    if (c >= '0' && c <= '9') { has_digit = 1; continue; }
    if (c == '.' && !has_dot) { has_dot = 1; continue; }
    return 0;
  }
  return has_digit;
}

internal B32
pg_param_to_sql_literal(Arena* arena, U32 declared_oid, U16 format, String8 raw_value, B32 is_null, String8* out_literal, String8* out_error)
{
  if (is_null)
  {
    *out_literal = str8_lit("NULL");
    return 1;
  }

  if (format == 1)
  {
    switch (declared_oid)
    {
      case PG_OID_INT2:
      {
        if (raw_value.size != 2) { *out_error = str8_lit("malformed binary int2 parameter"); return 0; }
        S16 v = (S16)(((U16)raw_value.str[0] << 8) | (U16)raw_value.str[1]);
        *out_literal = push_str8f(arena, "%d", (int)v);
        return 1;
      }
      case PG_OID_INT4:
      {
        if (raw_value.size != 4) { *out_error = str8_lit("malformed binary int4 parameter"); return 0; }
        S32 v = (S32)pg_read_be32(raw_value.str);
        *out_literal = push_str8f(arena, "%d", v);
        return 1;
      }
      case PG_OID_INT8:
      {
        if (raw_value.size != 8) { *out_error = str8_lit("malformed binary int8 parameter"); return 0; }
        U64 bits = ((U64)pg_read_be32(raw_value.str) << 32) | (U64)pg_read_be32(raw_value.str + 4);
        S64 v = (S64)bits;
        *out_literal = push_str8f(arena, "%lld", v);
        return 1;
      }
      case PG_OID_FLOAT4:
      {
        if (raw_value.size != 4) { *out_error = str8_lit("malformed binary float4 parameter"); return 0; }
        U32 bits = pg_read_be32(raw_value.str);
        F32 v; MemoryCopy(&v, &bits, sizeof(v));
        *out_literal = push_str8f(arena, "%f", v);
        return 1;
      }
      case PG_OID_FLOAT8:
      {
        if (raw_value.size != 8) { *out_error = str8_lit("malformed binary float8 parameter"); return 0; }
        U64 bits = ((U64)pg_read_be32(raw_value.str) << 32) | (U64)pg_read_be32(raw_value.str + 4);
        F64 v; MemoryCopy(&v, &bits, sizeof(v));
        *out_literal = push_str8f(arena, "%lf", v);
        return 1;
      }
      case PG_OID_BOOL:
      {
        if (raw_value.size != 1) { *out_error = str8_lit("malformed binary bool parameter"); return 0; }
        *out_literal = raw_value.str[0] ? str8_lit("1") : str8_lit("0");
        return 1;
      }
      case PG_OID_TEXT: case PG_OID_VARCHAR: case PG_OID_BPCHAR: case PG_OID_UNKNOWN: case 0:
      {
        for (U64 i = 0; i < raw_value.size; i++)
        {
          if (raw_value.str[i] == 0) { *out_error = str8_lit("NUL bytes are not supported in parameter values"); return 0; }
        }
        *out_literal = pg_quote_sql_string_literal(arena, raw_value);
        return 1;
      }
      default:
        *out_error = push_str8f(arena, "binary-format parameter with OID %u is not supported", declared_oid);
        return 0;
    }
  }

  // tec: text format - raw_value is already the value's text representation
  for (U64 i = 0; i < raw_value.size; i++)
  {
    if (raw_value.str[i] == 0) { *out_error = str8_lit("NUL bytes are not supported in parameter values"); return 0; }
  }

  switch (declared_oid)
  {
    case PG_OID_INT2: case PG_OID_INT4: case PG_OID_INT8: case PG_OID_FLOAT4: case PG_OID_FLOAT8:
    {
      if (!pg_text_looks_numeric(raw_value))
      {
        *out_error = push_str8f(arena, "invalid input syntax for numeric parameter: '%.*s'", str8_varg(raw_value));
        return 0;
      }
      *out_literal = push_str8_copy(arena, raw_value);
      return 1;
    }
    case PG_OID_BOOL:
    {
      if (str8_match(raw_value, str8_lit("t"), StringMatchFlag_CaseInsensitive) ||
          str8_match(raw_value, str8_lit("true"), StringMatchFlag_CaseInsensitive) ||
          str8_match(raw_value, str8_lit("1"), 0))
      {
        *out_literal = str8_lit("1");
        return 1;
      }
      if (str8_match(raw_value, str8_lit("f"), StringMatchFlag_CaseInsensitive) ||
          str8_match(raw_value, str8_lit("false"), StringMatchFlag_CaseInsensitive) ||
          str8_match(raw_value, str8_lit("0"), 0))
      {
        *out_literal = str8_lit("0");
        return 1;
      }
      *out_error = push_str8f(arena, "invalid input syntax for boolean parameter: '%.*s'", str8_varg(raw_value));
      return 0;
    }
    case PG_OID_TEXT: case PG_OID_VARCHAR: case PG_OID_BPCHAR:
    {
      *out_literal = pg_quote_sql_string_literal(arena, raw_value);
      return 1;
    }
    default: // undeclared, or an OID this engine has no mapping for - duck-type from the text's shape
    {
      if (pg_text_looks_numeric(raw_value))
      {
        *out_literal = push_str8_copy(arena, raw_value);
      }
      else
      {
        *out_literal = pg_quote_sql_string_literal(arena, raw_value);
      }
      return 1;
    }
  }
}

internal String8
pg_substitute_params(Arena* arena, String8 sql_text, String8* literals, U32 literal_count)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};

  U64 i = 0;
  U64 run_start = 0;
  B32 in_string = 0;

  while (i < sql_text.size)
  {
    U8 c = sql_text.str[i];

    if (in_string)
    {
      if (c == '\'')
      {
        if (i + 1 < sql_text.size && sql_text.str[i + 1] == '\'')
        {
          i += 2; // '' escaped quote, still inside the string
          continue;
        }
        in_string = 0;
      }
      i++;
      continue;
    }

    if (c == '\'')
    {
      in_string = 1;
      i++;
      continue;
    }

    if (c == '$' && i + 1 < sql_text.size && char_is_digit(sql_text.str[i + 1], 10))
    {
      if (i > run_start)
      {
        str8_list_push(scratch.arena, &parts, str8_substr(sql_text, r1u64(run_start, i)));
      }

      U64 num_start = i + 1;
      U64 num_end = num_start;
      while (num_end < sql_text.size && char_is_digit(sql_text.str[num_end], 10)) { num_end++; }

      String8 num_str = str8_substr(sql_text, r1u64(num_start, num_end));
      U64 param_index = u64_from_str8(num_str, 10); // 1-based

      if (param_index >= 1 && param_index <= literal_count)
      {
        str8_list_push(scratch.arena, &parts, literals[param_index - 1]);
      }
      else
      {
        str8_list_push(scratch.arena, &parts, str8_substr(sql_text, r1u64(i, num_end)));
      }

      i = num_end;
      run_start = i;
      continue;
    }

    i++;
  }

  if (i > run_start)
  {
    str8_list_push(scratch.arena, &parts, str8_substr(sql_text, r1u64(run_start, i)));
  }

  String8 result = str8_list_join(arena, &parts, 0);
  scratch_end(scratch);
  return result;
}

internal String8
pg_command_tag_from_sql(Arena* arena, String8 sql)
{
  String8 trimmed = str8_skip_chop_whitespace(sql);

  String8List words = str8_split_by_string_chars(arena, trimmed, str8_lit(" \t\r\n"), 0);
  String8 first = words.first ? words.first->string : str8_lit("");
  String8 second = (words.first && words.first->next) ? words.first->next->string : str8_lit("");

  if (str8_match(first, str8_lit("select"), StringMatchFlag_CaseInsensitive)) { return str8_lit("SELECT 0"); }
  if (str8_match(first, str8_lit("insert"), StringMatchFlag_CaseInsensitive)) { return str8_lit("INSERT 0 0"); }
  if (str8_match(first, str8_lit("delete"), StringMatchFlag_CaseInsensitive)) { return str8_lit("DELETE 0"); }
  if (str8_match(first, str8_lit("alter"), StringMatchFlag_CaseInsensitive)) { return str8_lit("ALTER TABLE"); }
  if (str8_match(first, str8_lit("drop"), StringMatchFlag_CaseInsensitive)) { return str8_lit("DROP INDEX"); }
  if (str8_match(first, str8_lit("describe"), StringMatchFlag_CaseInsensitive)) { return str8_lit("DESCRIBE"); }
  if (str8_match(first, str8_lit("explain"), StringMatchFlag_CaseInsensitive)) { return str8_lit("EXPLAIN"); }
  if (str8_match(first, str8_lit("import"), StringMatchFlag_CaseInsensitive)) { return str8_lit("IMPORT"); }
  if (str8_match(first, str8_lit("use"), StringMatchFlag_CaseInsensitive)) { return str8_lit("SET"); }
  if (str8_match(first, str8_lit("create"), StringMatchFlag_CaseInsensitive))
  {
    if (str8_match(second, str8_lit("database"), StringMatchFlag_CaseInsensitive)) { return str8_lit("CREATE DATABASE"); }
    if (str8_match(second, str8_lit("index"), StringMatchFlag_CaseInsensitive)) { return str8_lit("CREATE INDEX"); }
    return str8_lit("CREATE TABLE");
  }

  return str8_lit("OK");
}
