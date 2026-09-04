#ifndef PG_PROTOCOL_H
#define PG_PROTOCOL_H

// tec: Postgres wire protocol
// tec: not implemented: real auth, COPY, LISTEN/NOTIFY, query cancellation
// tec: pg_param_to_sql_literal/pg_substitute_params splice params into SQL text as literals

#define PG_PROTOCOL_VERSION_3_0  0x00030000u
#define PG_SSL_REQUEST_CODE      80877103u
#define PG_GSSENC_REQUEST_CODE   80877104u
#define PG_CANCEL_REQUEST_CODE   80877102u

//~ tec: type OIDs
// tec: GDB_ColumnType_U64 maps to signed int8. values above INT64_MAX round trip as text but not as a binary int8
internal U32 pg_oid_for_column_type(GDB_ColumnType type);
internal S16 pg_typlen_for_column_type(GDB_ColumnType type);

//~ tec: wire I/O
// tec: startup-phase messages have no leading type byte; every message after startup does
internal B32 pg_recv_untyped(Arena* arena, OS_Handle conn, String8* out_body);
internal B32 pg_recv_typed(Arena* arena, OS_Handle conn, U8* out_type, String8* out_body);

internal B32 pg_send_raw_byte(OS_Handle conn, U8 b);
internal B32 pg_send_msg(OS_Handle conn, Arena* arena, U8 type, String8List* body_parts);

//~ tec: backend messages
internal B32 pg_send_authentication_ok(OS_Handle conn, Arena* arena);
internal B32 pg_send_parameter_status(OS_Handle conn, Arena* arena, String8 name, String8 value);
internal B32 pg_send_backend_key_data(OS_Handle conn, Arena* arena, U32 pid, U32 secret);
internal B32 pg_send_ready_for_query(OS_Handle conn, Arena* arena, U8 status); // 'I'/'T'/'E'
internal B32 pg_send_error_response(OS_Handle conn, Arena* arena, String8 severity, String8 sqlstate, String8 message);
internal B32 pg_send_empty_query_response(OS_Handle conn, Arena* arena);
internal B32 pg_send_command_complete(OS_Handle conn, Arena* arena, String8 tag);
internal B32 pg_send_row_description_from_result_set(OS_Handle conn, Arena* arena, APP_ResultSet* result_set);
internal B32 pg_send_row_description_single_text_column(OS_Handle conn, Arena* arena, String8 column_name);
internal B32 pg_send_data_rows_from_result_set(OS_Handle conn, Arena* arena, APP_ResultSet* result_set);
internal B32 pg_send_data_row_single_text_column(OS_Handle conn, Arena* arena, String8 value);

//~ tec: extended protocol
// tec: result_format_codes: 0 entries = all text, 1 entry = applies to every column, else one per column
internal B32 pg_send_row_description_from_result_set_ex(OS_Handle conn, Arena* arena, APP_ResultSet* result_set, U16* result_format_codes, U16 format_code_count);
internal B32 pg_send_data_rows_from_result_set_range(OS_Handle conn, Arena* arena, APP_ResultSet* result_set, U16* result_format_codes, U16 format_code_count, U64 first_row, U64 row_count);
internal B32 pg_send_no_data(OS_Handle conn, Arena* arena);
internal B32 pg_send_parse_complete(OS_Handle conn, Arena* arena);
internal B32 pg_send_bind_complete(OS_Handle conn, Arena* arena);
internal B32 pg_send_close_complete(OS_Handle conn, Arena* arena);
internal B32 pg_send_portal_suspended(OS_Handle conn, Arena* arena);
internal B32 pg_send_parameter_description(OS_Handle conn, Arena* arena, U32* param_oids, U32 param_count);

internal String8 pg_startup_param_find(String8 startup_body_after_version, String8 key);

// tec: cosmetic tag only. INSERT/DELETE row counts are always reported as 0
internal String8 pg_command_tag_from_sql(Arena* arena, String8 sql);

internal B32 pg_looks_like_select(String8 sql);

//~ tec: parameter handling
#define PG_OID_BOOL    16
#define PG_OID_INT8    20
#define PG_OID_INT2    21
#define PG_OID_INT4    23
#define PG_OID_TEXT    25
#define PG_OID_FLOAT4  700
#define PG_OID_FLOAT8  701
#define PG_OID_UNKNOWN 705
#define PG_OID_BPCHAR  1042
#define PG_OID_VARCHAR 1043

// tec: returns 0 (with *out_error set) if the value cant be safely represented
internal B32 pg_param_to_sql_literal(Arena* arena, U32 declared_oid, U16 format, String8 raw_value, B32 is_null, String8* out_literal, String8* out_error);

// tec: replaces '$N' (1-based) outside quoted strings with literals[N-1]; out-of-range is left as is
internal String8 pg_substitute_params(Arena* arena, String8 sql_text, String8* literals, U32 literal_count);

#endif //PG_PROTOCOL_H
