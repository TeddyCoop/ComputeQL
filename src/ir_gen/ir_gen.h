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
  IR_NodeType_Index,
  IR_NodeType_DropIndex,
  IR_NodeType_Null,
  IR_NodeType_NotNull,
  IR_NodeType_Unique,
  IR_NodeType_PrimaryKey,
  IR_NodeType_ForeignKey,
  IR_NodeType_Check,
  IR_NodeType_Describe,
  IR_NodeType_Explain,
  IR_NodeType_Analyze,
  IR_NodeType_EnumDef,
  IR_NodeType_EnumValue,
  IR_NodeType_CteList,
  IR_NodeType_Cte,
  IR_NodeType_Subquery,
  IR_NodeType_InList,
  IR_NodeType_Exists,
  IR_NodeType_Window,
  IR_NodeType_PartitionBy,
  
  // tec: planner only, value is "semi" or "anti"
  // children are the outer key, the inner key, the inner Table and an optional Where
  IR_NodeType_SemiJoin, 
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

//~ tec: generation helpers
internal IR_Node* ir_generate_recursive(Arena* arena, SQL_Node* sql_node);
internal void ir_create_active_column_list(Arena* arena, IR_Node* parent_node, String8List* used_columns);
internal void ir_expand_star_to_columns(Arena *arena, GDB_Database *db, IR_Node *select_node);

#endif //IR_GEN_H
