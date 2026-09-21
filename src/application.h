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

global OS_Handle g_query_exec_mutex = {0};

global TP_Context* g_app_thread_pool = 0;
global TP_Arena* g_app_thread_pool_arena = 0;

// tec: out_result_set may be NULL
internal APP_QueryResult app_execute_query_capture(Arena* arena, String8 sql_query, GDB_Database** io_database, APP_ResultSet* out_result_set);
internal void app_execute_query(String8 sql_query);
internal PLAN_ExecResult app_perform_kernel(Arena* arena, GDB_Database* database, IR_Node* root_node);

internal TP_Context* app_thread_pool(void);
internal TP_Arena* app_thread_pool_arena(void);

//~ tec: row constraints
internal int delete_row_index_compare_descending(const void* a, const void* b);
internal void* gdb_zero_value_for_type(Arena* arena, GDB_ColumnType type);
internal B32 gdb_candidate_value_equals_row(Arena* arena, GDB_Column* column, void* candidate, U64 existing_row);
internal B32 gdb_stored_values_equal(Arena* arena, GDB_Column* col_a, U64 row_a, GDB_Column* col_b, U64 row_b);
internal F64 gdb_check_load_value(GDB_Table* table, void** row_data, B32* row_null, IR_Node* node, B32* out_is_string, String8* out_string, B32* out_is_null);
internal B32 gdb_check_eval(GDB_Table* table, void** row_data, B32* row_null, IR_Node* condition);
internal B32 gdb_table_validate_row_constraints(Arena* arena, GDB_Database* database, GDB_Table* table, void** row_data, B32* row_null);
internal B32 gdb_row_has_referencing_children(Arena* arena, GDB_Database* database, GDB_Table* table, U64 row_index);

//~ tec: select formatting
typedef struct SelectColGather SelectColGather;
struct SelectColGather
{
  B32 resolved;
  B32 is_score;
  GDB_Table* col_table;
  GDB_Column* column;
  U64 table_slot;
  GDB_ColumnType type;
  F64* numeric_values;
  GDB_StringDataChunk strings;
};

typedef struct APP_SelectFormatTask APP_SelectFormatTask;
struct APP_SelectFormatTask
{
  Rng1U64* ranges;
  IR_Node* select_output_columns;
  SelectColGather* gathered;
  U64 column_count;
  PLAN_RowSet* rows;
  B32 capture_structured;
  APP_ResultSet* out_result_set;
  String8List* worker_lists;
};

internal String8 app_format_cell_text(Arena* arena, GDB_ColumnType type, F64 numeric_value, String8 string_value, U32 decimal_scale, GDB_EnumType* enum_type);
internal THREAD_POOL_TASK_FUNC(app_select_format_task);
internal void app_select_format_dispatch(Arena* arena, String8List* out, IR_Node* select_output_columns, SelectColGather* gathered, U64 column_count, PLAN_RowSet* rows, U64 result_count, B32 capture_structured, APP_ResultSet* out_result_set);

#endif //APPLICATION_H
