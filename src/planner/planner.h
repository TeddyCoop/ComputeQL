#ifndef PLANNER_H
#define PLANNER_H

internal PLAN_Node* plan_node_make(Arena* arena, PLAN_NodeType type);
internal PLAN_Node* plan_build_from_select(Arena* arena, GDB_Database* database, IR_Node* select_ir_node);
internal B32 plan_ir_contains_aggregate(IR_Node* node);

internal PLAN_ExecResult plan_execute(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node, QE_TraceCtx* trace);
internal PLAN_ExecResult plan_execute_node(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node, QE_TraceCtx* trace);
internal PLAN_ExecResult plan_execute_top_n(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node, QE_TraceCtx* trace);
internal PLAN_Node* plan_add_semi_join(Arena* arena, GDB_Database* database, PLAN_Node* input, IR_Node* semi_ir);
internal B32 plan_node_is_join(PLAN_Node* plan);
internal void plan_apply_limit(PLAN_ExecResult* result, IR_Node* offset_node, IR_Node* limit_node);
internal PLAN_RowSet plan_reverse_rowset(Arena* arena, PLAN_RowSet* rows);
internal U64 plan_result_row_count(PLAN_ExecResult* result);
internal B32 plan_node_records_rows_only(PLAN_Node* plan);

// tec: leaves temp tables registered on `database`. release them with gdb_database_release_temp_tables_from()
internal PLAN_ExecResult plan_run_select(Arena* arena, GDB_Database* database, IR_Node* select_ir, QE_TraceCtx* trace);
internal PLAN_ExecResult plan_run_select_with_plan(Arena* arena, GDB_Database* database, IR_Node* select_ir, QE_TraceCtx* trace, PLAN_Node** out_plan);
internal PLAN_Node* plan_build_for_explain(Arena* arena, GDB_Database* database, IR_Node* select_ir, OPT_SourceEstimates* inherited);

internal String8 plan_node_type_to_string(PLAN_NodeType type);
internal void plan_print(Arena* arena, String8List* out, PLAN_Node* plan, U64 depth);
internal void plan_print_analyzed(Arena* arena, String8List* out, PLAN_Node* plan, QE_TraceCtx* trace, U64 depth);
internal String8 plan_format_rows(Arena* arena, F64 rows);
internal F64 plan_q_error(F64 estimated, F64 actual);
internal String8 plan_estimate_suffix(Arena* arena, PLAN_Node* plan, B32 has_actual, U64 actual_rows);
internal String8 plan_scan_strategy_suffix(Arena* arena, PLAN_Node* plan);
internal String8 plan_sort_strategy_suffix(Arena* arena, PLAN_Node* plan);
internal String8 plan_indent_string(Arena* arena, U64 depth);
internal void plan_print_plain_line(Arena* arena, String8List* out, PLAN_Node* plan, String8 indent, String8 estimate_suffix);
internal void plan_print_scan_trace(Arena* arena, String8List* out, PLAN_Node* plan, QE_NodeTrace* node_trace, String8 indent);

//~ tec: join execution
#define PLAN_JOIN_MAX_ON_CONDITIONS 16
internal String8 plan_alias_from_table_ir(IR_Node* table_ir);
internal PLAN_ExecResult plan_wrap_scan_result(Arena* arena, GDB_Table* table, String8 alias, QE_ScanResult scan_result);
internal PLAN_ExecResult plan_execute_join(Arena* arena, GDB_Database* database, PLAN_Node* join_plan, IR_Node* select_ir_node, IR_Node* residual_where_root, QE_TraceCtx* trace);
internal PLAN_RowSet plan_make_empty_join_rowset(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias);
internal PLAN_ExecResult plan_execute_identity_scan(Arena* arena, PLAN_Node* scan, QE_ScanTrace* trace);
internal F64 plan_us_to_ms(U64 us);

//~ tec: materialization
internal String8 plan_output_column_name(Arena* arena, IR_Node* item);
internal B32 plan_materialize_rowset_item(Arena* arena, PLAN_RowSet* rows, IR_Node* item, U64 count, PLAN_AggColumn* out_col);
internal B32 plan_materialize_result(Arena* arena, PLAN_ExecResult* result, IR_Node* column_list_ir, PLAN_Materialized* out);
internal void plan_temp_column_append(GDB_Column* column, PLAN_AggColumn* src, U64 row);
internal GDB_Table* plan_materialized_to_temp_table(GDB_Database* database, String8 name, PLAN_Materialized* m);
internal B32 plan_value_to_ir_node(Arena* arena, PLAN_AggColumn* col, U64 row, IR_Node* out);
internal B32 plan_run_select_materialized(Arena* arena, GDB_Database* database, IR_Node* select_ir, PLAN_Materialized* out);

//~ tec: subqueries and derived tables
typedef struct PLAN_SubqueryTable PLAN_SubqueryTable;
struct PLAN_SubqueryTable
{
  GDB_Table* table;
  String8 alias;
};

internal B32 plan_bind_derived_table(Arena* arena, GDB_Database* database, IR_Node* table_ir);
internal void plan_make_constant_condition(Arena* arena, IR_Node* node, B32 value);
internal GDB_Table* plan_find_table_quiet(GDB_Database* database, String8 name);
internal B32 plan_column_resolves(PLAN_SubqueryTable* tables, U64 table_count, String8 name);
internal B32 plan_check_uncorrelated(Arena* arena, GDB_Database* database, IR_Node* select_ir);
internal B32 plan_run_subquery_values(Arena* arena, GDB_Database* database, IR_Node* subquery_ir, PLAN_Materialized* out);
internal B32 plan_rewrite_predicates(Arena* arena, GDB_Database* database, IR_Node* node);
internal void plan_apply_subquery_in_list(Arena* arena, IR_Node* node, IR_Node* subquery_node, PLAN_Materialized* values, B32 is_not_in);

//~ tec: window functions
typedef struct PLAN_WindowKey PLAN_WindowKey;
struct PLAN_WindowKey
{
  B32 is_string;
  B32 descending;
  F64* nums;
  String8* strs;
  U8* is_null;
};

typedef struct PLAN_WindowSortCtx PLAN_WindowSortCtx;
struct PLAN_WindowSortCtx
{
  PLAN_WindowKey* keys;
  U64 key_count;
};

// tec: qsort has no user pointer
global PLAN_WindowSortCtx* g_plan_window_sort_ctx = 0;

#define PLAN_WINDOW_MAX_KEYS 8

typedef enum PLAN_WindowFunc
{
  PLAN_WindowFunc_RowNumber,
  PLAN_WindowFunc_Rank,
  PLAN_WindowFunc_DenseRank,
  PLAN_WindowFunc_Lag,
  PLAN_WindowFunc_Lead,
  PLAN_WindowFunc_Agg,
} PLAN_WindowFunc;

internal B32 plan_ir_is_window_call(IR_Node* node);
internal S32 plan_window_key_compare(PLAN_WindowKey* key, U64 a, U64 b);
internal S32 plan_window_row_compare(const void* pa, const void* pb);
internal B32 plan_window_keys_equal(PLAN_WindowKey* keys, U64 first, U64 count, U64 a, U64 b);
internal B32 plan_window_gather_key(Arena* arena, PLAN_RowSet* rows, String8 column_name, B32 descending, PLAN_WindowKey* out);
internal B32 plan_window_compute(Arena* arena, PLAN_RowSet* rows, IR_Node* call, PLAN_AggColumn* out_col);
internal B32 plan_window_apply(Arena* arena, PLAN_RowSet* rows, IR_Node* column_list_ir, IR_Node* order_by_ir, PLAN_Materialized* out);

#endif //PLANNER_H
