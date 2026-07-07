/* date = July 5th 2026 */

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
internal QE_ScanResult qe_scan_filter(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* root_node);

#endif //QUERY_EXEC_H
