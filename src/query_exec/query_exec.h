#ifndef QUERY_EXEC_H
#define QUERY_EXEC_H

//~ tec: fixed descriptor binding layout shared with scan_filter.comp
//
//  binding 0: bytecode buffer            (uint[])
//  binding 1: numeric constant pool      (uint[], 2 words per constant: lo, hi)
//  binding 2: string constant pool       (uint[], packed bytes, 4 per word)
//  binding 3: output row indices         (uint[], 2 words per index: lo, hi)
//  binding 4: output match count         (uint[1], atomicAdd on word 0)
//  binding 5: reserved
//  binding 6..15: dynamically bound column buffers (up to QE_MAX_COLUMN_BINDINGS slots)
//
//  push constant word 0 (U64 index 0): row_count

#define QE_BINDING_BYTECODE     0
#define QE_BINDING_NUM_CONSTS   1
#define QE_BINDING_STR_CONSTS   2
#define QE_BINDING_OUT_INDICES  3
#define QE_BINDING_OUT_COUNT    4
#define QE_BINDING_COLUMN_BASE  6

#define QE_MAX_COLUMN_BINDINGS  10

// tec: must match every kernel .comp's `layout(local_size_x = ...)`
#define QE_GPU_WORKGROUP_SIZE 256

#define QE_PUSH_CONSTANT_ROW_COUNT 0

// tec: must match scan_filter.comp's MAX_STACK exactly. settings configurable,
#define QE_SCAN_MAX_STACK 8

// tec: must match GDB_ColumnType's enum and scan_filter.comp's COLTYPE_* defines exactly
#define COLTYPE_BOOL      6
#define COLTYPE_I32       7
#define COLTYPE_I64       8
#define COLTYPE_DATE      9
#define COLTYPE_TIMESTAMP 10
#define COLTYPE_DECIMAL   11
#define COLTYPE_ENUM      12

internal B32 qe_resolve_date_literal_value(GDB_ColumnType col_type, IR_Node* literal_node, F64* out_value);
internal B32 qe_resolve_decimal_literal_value(GDB_Column* column, IR_Node* literal_node, F64* out_value);
internal B32 qe_resolve_enum_literal_value(GDB_Column* column, IR_Node* literal_node, F64* out_value);

typedef enum QE_Opcode
{
  QE_Opcode_PushTrue    = 0,
  QE_Opcode_LoadNumCol  = 1,
  QE_Opcode_PushConst   = 2,
  QE_Opcode_CmpEq       = 3,
  QE_Opcode_CmpNe       = 4,
  QE_Opcode_CmpLt       = 5,
  QE_Opcode_CmpGt       = 6,
  QE_Opcode_CmpLe       = 7,
  QE_Opcode_CmpGe       = 8,
  QE_Opcode_And         = 9,
  QE_Opcode_Or          = 10,
  QE_Opcode_StrEq       = 11,
  QE_Opcode_StrContains = 12,
  QE_Opcode_Halt        = 13,
} QE_Opcode;

typedef struct QE_ColumnBinding QE_ColumnBinding;
struct QE_ColumnBinding
{
  String8 name;
  GDB_Column* column;
  GDB_ColumnType type;
  U32 first_slot; // tec: relative slot (0..QE_MAX_COLUMN_BINDINGS-1), maps to descriptor binding QE_BINDING_COLUMN_BASE+first_slot
  U32 slot_count; // tec: 1 for numeric columns, 2 (data+offsets) for string columns
};

typedef struct QE_BytecodeProgram QE_BytecodeProgram;
struct QE_BytecodeProgram
{
  U32* words;
  U64 word_count;
  U64 words_cap;
  
  U32* consts;
  U64 const_count;
  U64 consts_cap;
  
  U8* str_const_pool;
  U64 str_const_pool_size;
  U64 str_const_pool_cap;
  
  QE_ColumnBinding bindings[QE_MAX_COLUMN_BINDINGS];
  U32 binding_count;
  U32 next_slot;
};

typedef struct QE_StringConstRef QE_StringConstRef;
struct QE_StringConstRef
{
  U32 word_offset;
  U32 byte_len;
};

internal void qe_bytecode_emit(QE_BytecodeProgram* prog, U32 word);
internal U32 qe_add_numeric_const(QE_BytecodeProgram* prog, F64 value);
internal QE_StringConstRef qe_add_string_const(QE_BytecodeProgram* prog, String8 str);
internal QE_ColumnBinding* qe_find_binding(QE_BytecodeProgram* prog, String8 column_name);
internal QE_ColumnBinding* qe_bind_column(QE_BytecodeProgram* prog, GDB_Table* table, String8 column_name);
internal QE_Opcode qe_opcode_from_comparison_operator(String8 op);
internal void qe_compile_load_value(QE_BytecodeProgram* prog, GDB_Table* table, IR_Node* node);
internal void qe_compile_condition(QE_BytecodeProgram* prog, GDB_Table* table, IR_Node* condition);

typedef struct QE_ScanResult QE_ScanResult;
struct QE_ScanResult
{
  U64* indices;
  U64 count;
};

internal void qe_bytecode_program_build(Arena* arena, QE_BytecodeProgram* prog, GDB_Database* database, GDB_Table* table, IR_Node* root_node, IR_Node* where_clause);
internal U32 qe_bytecode_program_max_stack_depth(QE_BytecodeProgram* prog);
internal QE_ScanResult qe_scan_filter(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* where_clause);
internal B32 qe_try_index_scan(Arena* arena, GDB_Table* table, IR_Node* where_clause, QE_ScanResult* out_result);
internal QE_ScanResult qe_cpu_scan_filter(Arena* arena, GDB_Table* table, IR_Node* where_clause);

//~ tec: shared query-result representation
#define PLAN_NULL_ROW max_U64 // tec: unmatched side of a LEFT JOIN - treat a column read against this as NULL

typedef struct PLAN_RowSet PLAN_RowSet;
struct PLAN_RowSet
{
  GDB_Table** tables;
  String8* aliases;
  U64 table_count;
  U64** row_indices;
  U64 count;
};

typedef struct PLAN_AggColumn PLAN_AggColumn;
struct PLAN_AggColumn
{
  String8 name;
  GDB_ColumnType type;
  U32 decimal_scale;
  GDB_EnumType* enum_type;
  F64* numeric_values;
  String8* string_values;
};

typedef struct PLAN_Materialized PLAN_Materialized;
struct PLAN_Materialized
{
  PLAN_AggColumn* columns;
  U64 column_count;
  U64 count; // tec: number of result rows (groups)
};

typedef struct PLAN_ExecResult PLAN_ExecResult;
struct PLAN_ExecResult
{
  B32 supported;        // tec: 0 if this plan (or a node beneath it) has no kernel to execute it yet
  B32 is_materialized;  // tec: 0 -> 'rows' is valid, 1 -> 'materialized' is valid
  PLAN_RowSet rows;
  PLAN_Materialized materialized;
};

internal String8 qe_column_list_item_display_name(Arena* arena, IR_Node* item);

//~ tec: multi-table column qualifier resolution
internal GDB_Table* qe_resolve_column_table(PLAN_RowSet* rows, String8 column_name, String8* out_bare_name, U64* out_slot);

internal U64 qe_rowset_table_slot(PLAN_RowSet* rows, GDB_Table* table);

internal F64* qe_gather_numeric_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column);
internal GDB_StringDataChunk qe_gather_string_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column);

//~ tec: sort

// tec: baked into bitonic_sort.comp/bitonic_sort_f32.comp's PAYLOAD_STRIDE and key indexing
// so they cant be configured via settings
#define QE_SORT_MAX_KEYS   4
#define QE_SORT_MAX_TABLES 4
internal PLAN_RowSet qe_sort_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* order_by_ir);
internal PLAN_Materialized qe_sort_materialized(Arena* arena, PLAN_Materialized* m, IR_Node* order_by_ir);

//~ tec: aggregate

// tec: baked into aggregate_assign.comp/aggregate_reduce.comp's fixed binding count
// so they cant be configured via settings
#define QE_AGG_MAX_GROUP_COLS 4
#define QE_AGG_MAX_EXPRS      8

internal PLAN_Materialized qe_aggregate(Arena* arena, GDB_Database* database, PLAN_RowSet* input, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir);
internal PLAN_Materialized qe_apply_having(Arena* arena, PLAN_Materialized* m, IR_Node* having_ir);

//~ tec: GPU build/probe equi-join
internal PLAN_RowSet qe_hash_join(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias, String8 join_type, IR_Node* condition);
internal B32 qe_column_belongs_to_table(GDB_Table* table, String8 alias, String8 column_name);
internal IR_Node* qe_find_equi_condition(PLAN_RowSet* left_rows, GDB_Table* right_table, String8 right_alias, IR_Node* condition);
internal PLAN_RowSet qe_filter_joined_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* condition);

#endif //QUERY_EXEC_H
