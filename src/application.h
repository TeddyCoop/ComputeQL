/* date = February 9th 2025 8:43 pm */

#ifndef APPLICATION_H
#define APPLICATION_H

typedef struct APP_QueryResult APP_QueryResult;
struct APP_QueryResult
{
  B32 had_parse_error;
  String8 output_text;
};

// tec: structured (typed, per-cell) capture of a SELECT result
// populated only when that SELECT is the batch's final statement
typedef struct APP_ResultColumn APP_ResultColumn;
struct APP_ResultColumn
{
  String8 name;
  GDB_ColumnType type;
};

typedef struct APP_ResultSet APP_ResultSet;
struct APP_ResultSet
{
  B32 valid;
  APP_ResultColumn* columns;
  U64 column_count;
  U64 row_count;
  // [row * column_count + col], valid where cell_is_null is 0
  String8* cell_text;
  B32* cell_is_null;
  // raw value for a non-String8 column, undefined otherwise
  F64* cell_numeric;    
};

// tec: out_result_set may be NULL
internal APP_QueryResult app_execute_query_capture(Arena* arena, String8 sql_query, GDB_Database** io_database, APP_ResultSet* out_result_set);
internal void app_execute_query(String8 sql_query);
internal PLAN_ExecResult app_perform_kernel(Arena* arena, GDB_Database* database, IR_Node* root_node);

#endif //APPLICATION_H
