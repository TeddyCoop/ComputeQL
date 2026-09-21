#ifndef PG_SERVER_H
#define PG_SERVER_H

internal void server_run_pg(U16 port);

//~ tec: connection
global Arena *g_pg_server_arena = 0;

typedef struct PG_ConnCtx PG_ConnCtx;
struct PG_ConnCtx
{
  OS_Handle socket;
};

internal B32 pg_do_startup_handshake(Arena *arena, OS_Handle socket, GDB_Database **out_current_database);
internal void pg_connection_thread_proc(void *ptr);

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

internal PG_PreparedStatement* pg_session_find_statement(PG_Session *session, String8 name);
internal PG_PreparedStatement* pg_session_alloc_statement(PG_Session *session, String8 name);
internal void pg_session_close_statement(PG_Session *session, String8 name);
internal PG_Portal* pg_session_find_portal(PG_Session *session, String8 name);
internal PG_Portal* pg_session_alloc_portal(PG_Session *session, String8 name);
internal void pg_session_close_portal(PG_Session *session, String8 name);
internal void pg_portal_ensure_executed(PG_Session *session, PG_Portal *portal);

//~ tec: message reader
typedef struct PG_Reader PG_Reader;
struct PG_Reader
{
  U8 *data;
  U64 size;
  U64 pos;
  B32 error;
};

internal PG_Reader pg_reader_make(String8 body);
internal U8 pg_reader_u8(PG_Reader *r);
internal U16 pg_reader_u16(PG_Reader *r);
internal U32 pg_reader_u32(PG_Reader *r);
internal S32 pg_reader_s32(PG_Reader *r);
internal String8 pg_reader_cstr(PG_Reader *r);
internal String8 pg_reader_bytes(PG_Reader *r, U64 n);

//~ tec: message handlers
internal String8 pg_dummy_literal_for_oid(U32 oid);
internal B32 pg_is_valid_identifier(String8 s);
internal void pg_handle_simple_query(Arena *arena, OS_Handle socket, GDB_Database **current_database, String8 sql);
internal void pg_handle_parse(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body);
internal void pg_handle_bind(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body);
internal void pg_handle_describe(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body);
internal void pg_handle_execute(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body);
internal void pg_handle_close(PG_Session *session, OS_Handle socket, Arena *msg_arena, String8 msg_body);

#endif //PG_SERVER_H
