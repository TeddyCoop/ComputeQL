/* date = February 4th 2025 11:03 pm */

#ifndef IR_GEN_H
#define IR_GEN_H

typedef enum IR_NodeType
{
  IR_NodeType_Select,
  IR_NodeType_Column,
  IR_NodeType_Table,
  IR_NodeType_Database,
  IR_NodeType_Where,
  IR_NodeType_Create,
  IR_NodeType_Condition,
  IR_NodeType_Operator,
  IR_NodeType_Numeric,
  IR_NodeType_Literal,
  IR_NodeType_OrderBy,
  IR_NodeType_Ascending,
  IR_NodeType_Descending,
  IR_NodeType_Insert,
  IR_NodeType_Import,
  IR_NodeType_Value,
  IR_NodeType_ValueGroup,
  IR_NodeType_ColumnList,
  IR_NodeType_Delete,
  IR_NodeType_Alter,
  IR_NodeType_AddColumn,
  IR_NodeType_DropColumn,
  IR_NodeType_Rename,
  IR_NodeType_Type,
  IR_NodeType_Use,
  IR_NodeType_Join,
  IR_NodeType_Alias,
  IR_NodeType_GroupBy,
  IR_NodeType_Having,
  IR_NodeType_Limit,
  IR_NodeType_Offset,
  IR_NodeType_AggregateCall,
} IR_NodeType;

typedef struct IR_Node IR_Node;
struct IR_Node
{
  IR_Node* first;
  IR_Node* last;
  IR_Node* prev;
  IR_Node* next;
  IR_Node* parent;
  IR_NodeType type;
  String8 value;
};

typedef struct IR_Query IR_Query;
struct IR_Query
{
  /*
  IR_Node* select_nodes;
  U64 select_count;
  IR_Node* create_nodes;
  U64 create_count;
  IR_Node* insert_nodes;
  U64 insert_count;
  IR_Node* delete_nodes;
  U64 delete_count;
  IR_Node* alter_nodes;
  U64 alter_count;
  */
  IR_Node* execution_nodes;
  U64 count;
};

internal IR_Query* ir_generate_from_ast(Arena* arena, SQL_Node* ast_root);
internal IR_Node* ir_convert_expression(Arena* arena, SQL_Node *ast_expr);

internal IR_Node* ir_node_make(Arena* arena, IR_NodeType, String8 value);
internal void ir_node_add_child(IR_Node* parent, IR_Node* child);
internal IR_NodeType ir_type_from_sql_node_type(SQL_NodeType sql_type);
internal String8 ir_node_type_to_string(IR_NodeType type);
internal IR_Node* ir_node_find_child(IR_Node* parent, IR_NodeType type);
internal GDB_ColumnType ir_find_column_type(GDB_Database* database, IR_Node* select_ir_node, String8 column_name);
internal void ir_print_node(IR_Node *node, U64 depth);
internal void ir_print_query(IR_Query *query);

#endif //IR_GEN_H
