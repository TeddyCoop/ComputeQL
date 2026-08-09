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
#define QE_BYTECODE_MAX_WORDS   4096
#define QE_MAX_NUMERIC_CONSTS   256
#define QE_STRING_CONST_POOL_SIZE KB(64)

// tec: must match every kernel .comp's `layout(local_size_x = ...)` - all of them use this same workgroup size
#define QE_GPU_WORKGROUP_SIZE 256

#define QE_PUSH_CONSTANT_ROW_COUNT 0

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
  U32 words[QE_BYTECODE_MAX_WORDS];
  U64 word_count;
  
  U32 consts[QE_MAX_NUMERIC_CONSTS * 2]; // tec: pairs of (lo, hi) per constant
  U64 const_count;
  
  U8 str_const_pool[QE_STRING_CONST_POOL_SIZE];
  U64 str_const_pool_size;
  
  QE_ColumnBinding bindings[QE_MAX_COLUMN_BINDINGS];
  U32 binding_count;
  U32 next_slot;
};

typedef struct QE_ScanResult QE_ScanResult;
struct QE_ScanResult
{
  U64* indices;
  U64 count;
};

internal void qe_bytecode_program_build(QE_BytecodeProgram* prog, GDB_Database* database, GDB_Table* table, IR_Node* root_node, IR_Node* where_clause);
// tec: where_clause is the IR_NodeType_Where wrapper node (its->first is the actual condition tree) pass NULL for an unfiltered scan (eg a Scan that's just one side of a Join, where the
// query's WHERE, if any, must NOT be applied here. see plan_execute's Scan/Filter cases)
internal QE_ScanResult qe_scan_filter(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* where_clause);

//~ tec: shared query-result representation
// lives here because the unity build
#define PLAN_NULL_ROW max_U64 // tec: unmatched side of a LEFT JOIN - treat a column read against this as NULL

typedef struct PLAN_RowSet PLAN_RowSet;
struct PLAN_RowSet
{
  GDB_Table** tables;  // tec: contributing tables, stable left-to-right order (as folded by the FROM/JOIN tree)
  String8* aliases;
  U64 table_count;
  U64** row_indices;   // tec: row_indices[t][i] = row index of tables[t] contributing to output row i (or PLAN_NULL_ROW)
  U64 count;           // tec: number of output rows (same for every row_indices[t])
};

typedef struct PLAN_AggColumn PLAN_AggColumn;
struct PLAN_AggColumn
{
  String8 name;             // tec: display name - group column's bare name, alias, or the aggregate's signature text (eg "SUM(price)")
  GDB_ColumnType type;       // tec: String8 -> string_values valid, anything else -> numeric_values valid
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

//~ tec: display/lookup name for one SELECT-list (or HAVING) item - an explicit alias wins,
// otherwise a plain Column uses its own (possibly qualified) text and an AggregateCall renders
// as "FUNC(arg)". single source of truth shared by qe_aggregate (which builds materialized
// column names) and the output printer (which looks columns up by this same name)
internal String8 qe_column_list_item_display_name(Arena* arena, IR_Node* item);

//~ tec: multi-table column qualifier resolution
// shared by Join/Filter-over-Join/output printing.
// splits an optional 'table.column' qualifier out of column_name and finds which of rows->tables[]
// it belongs to (searches all tables if unqualified; errors on ambiguity/not-found). a qualifier is
// matched against that slot's alias first (if it has one), falling back to the real table name -
// this is what lets a self-join's two "AS" aliases resolve to distinct slots even though both
// slots point at the same GDB_Table*. out_slot (may be NULL) receives the resolved slot index,
// which callers should use directly instead of re-deriving it via qe_rowset_table_slot(), since
// table-pointer identity alone can't distinguish a self-join's two sides
internal GDB_Table* qe_resolve_column_table(PLAN_RowSet* rows, String8 column_name, String8* out_bare_name, U64* out_slot);

// tec: which of rows->tables[] a table pointer is, or max_U64 if it's not part of this row set.
// NOTE: ambiguous for a self-join (returns the first matching slot) - prefer the out_slot from
// qe_resolve_column_table wherever a specific column reference is available
internal U64 qe_rowset_table_slot(PLAN_RowSet* rows, GDB_Table* table);

//~ tec: gather a column's values for an arbitrary (possibly multi-table) row set into a dense,
// arena-allocated array sized rows->count - used by Sort/Aggregate/chained-Join so those kernels
// never need to know whether their input was a bare table scan or a join's output. takes the
// already-resolved row-set slot (not a table pointer) so self-joins address the correct side
internal F64* qe_gather_numeric_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column);
internal GDB_StringDataChunk qe_gather_string_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column);

//~ tec: sort
#define QE_SORT_MAX_KEYS   4
#define QE_SORT_MAX_TABLES 4
internal PLAN_RowSet qe_sort_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* order_by_ir);
internal PLAN_Materialized qe_sort_materialized(Arena* arena, PLAN_Materialized* m, IR_Node* order_by_ir);

//~ tec: aggregate

#define QE_AGG_MAX_GROUP_COLS 4
#define QE_AGG_MAX_EXPRS      8
internal PLAN_Materialized qe_aggregate(Arena* arena, GDB_Database* database, PLAN_RowSet* input, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir);
internal PLAN_Materialized qe_apply_having(Arena* arena, PLAN_Materialized* m, IR_Node* having_ir);

//~ tec: GPU build/probe equi-join. 'right_table' must be a bare table 'left' may be any prior PLAN_RowSet.
// 'right_alias' is the "AS x" alias the FROM/JOIN clause gave right_table (or {0} if unaliased) -
// needed so a self-join's two sides (same GDB_Table*, different aliases) resolve distinctly.
internal PLAN_RowSet qe_hash_join(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias, String8 join_type, IR_Node* condition);

// tec: does `column_name` (bare or qualified) belong to 'table'? a qualifier is matched against
// 'alias' first (if non-empty), falling back to table->name
internal B32 qe_column_belongs_to_table(GDB_Table* table, String8 alias, String8 column_name);

// tec: finds the first 'a = b' clause (top-level, or AND-chained) where one side resolves to
// right_table (addressed as right_alias, or its real name if unaliased) and the other to some
// table in left_rows - this is what turns an explicit ON condition (already this shape) and a
// comma-join's WHERE clause (searched for this shape) into the same equi-join condition
// qe_hash_join needs. Returns NULL if no such clause exists.
internal IR_Node* qe_find_equi_condition(PLAN_RowSet* left_rows, GDB_Table* right_table, String8 right_alias, IR_Node* condition);

// tec: residual WHERE filter evaluated directly in C against an already-joined row set
// reuses the WHERE-clause condition tree verbatim, including any join-equality clause it contains
internal PLAN_RowSet qe_filter_joined_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* condition);

#endif //QUERY_EXEC_H
