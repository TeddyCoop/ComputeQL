internal IR_Node*
ir_node_make(Arena *arena, IR_NodeType type, String8 value)
{
  IR_Node *node = push_array(arena, IR_Node, 1);
  node->type = type;
  node->value = value;
  return node;
}

internal void
ir_node_add_child(IR_Node *parent, IR_Node *child)
{
  child->parent = parent;
  
  if (parent->last)
  {
    parent->last->next = child;
    child->prev = parent->last;
    parent->last = child;
  }
  else
  {
    parent->first = parent->last = child;
  }
}

internal IR_Node*
ir_generate_recursive(Arena* arena, SQL_Node* sql_node)
{
  if (!sql_node) return NULL;
  
  IR_Node *ir_node = push_array(arena, IR_Node, 1);
  ir_node->type = ir_type_from_sql_node_type(sql_node->type);
  ir_node->value = sql_node->value;
  
  // tec: build children via ir_node_add_child (not a hand-rolled ->next chain) so 
  // ->last/->prev/->parent all end up correctly populated too
  for (SQL_Node *child = sql_node->first; child; child = child->next)
  {
    IR_Node *ir_child = ir_generate_recursive(arena, child);
    ir_node_add_child(ir_node, ir_child);
  }
  
  return ir_node;
}

internal IR_Query*
ir_generate_from_ast(Arena* arena, SQL_Node* ast_root)
{
  ProfBeginFunction();
  
  IR_Query *ir_query = push_array(arena, IR_Query, 1);
  
  IR_Node **ir_node_next = &ir_query->execution_nodes;
  
  for (SQL_Node *node = ast_root; node; node = node->next)
  {
    IR_Node *ir_node = ir_generate_recursive(arena, node);
    
    *ir_node_next = ir_node;
    ir_node_next = &ir_node->next;
    ir_query->count++;
  }
  
  ProfEnd();
  return ir_query;
}

internal IR_NodeType
ir_type_from_sql_node_type(SQL_NodeType sql_type)
{
  switch (sql_type)
  {
    case SQL_NodeType_Use:           return IR_NodeType_Use;
    case SQL_NodeType_Describe:      return IR_NodeType_Describe;
    case SQL_NodeType_Select:        return IR_NodeType_Select;
    case SQL_NodeType_Column:        return IR_NodeType_Column;
    case SQL_NodeType_Table:         return IR_NodeType_Table;
    case SQL_NodeType_Where:         return IR_NodeType_Where;
    case SQL_NodeType_Create:        return IR_NodeType_Create;
    case SQL_NodeType_Operator:      return IR_NodeType_Operator;
    case SQL_NodeType_Literal:       return IR_NodeType_Literal;
    case SQL_NodeType_Numeric:       return IR_NodeType_Numeric;
    case SQL_NodeType_OrderBy:       return IR_NodeType_OrderBy;
    case SQL_NodeType_Ascending:     return IR_NodeType_Ascending;
    case SQL_NodeType_Descending:    return IR_NodeType_Descending;
    case SQL_NodeType_Insert:        return IR_NodeType_Insert;
    case SQL_NodeType_Import:        return IR_NodeType_Import;
    case SQL_NodeType_Value:         return IR_NodeType_Value;
    case SQL_NodeType_ValueGroup:    return IR_NodeType_ValueGroup;
    case SQL_NodeType_ColumnList:    return IR_NodeType_ColumnList; 
    case SQL_NodeType_Delete:        return IR_NodeType_Delete;
    case SQL_NodeType_Alter:         return IR_NodeType_Alter;
    case SQL_NodeType_Alter_AddColumn: return IR_NodeType_AddColumn;
    case SQL_NodeType_Alter_ColumnType: return IR_NodeType_Type;
    case SQL_NodeType_Alter_DropColumn: return IR_NodeType_DropColumn;
    case SQL_NodeType_Alter_Rename:  return IR_NodeType_Rename;
    case SQL_NodeType_Database:      return IR_NodeType_Database;
    case SQL_NodeType_Join:          return IR_NodeType_Join;
    case SQL_NodeType_Alias:         return IR_NodeType_Alias;
    case SQL_NodeType_GroupBy:       return IR_NodeType_GroupBy;
    case SQL_NodeType_Having:        return IR_NodeType_Having;
    case SQL_NodeType_Limit:         return IR_NodeType_Limit;
    case SQL_NodeType_Offset:        return IR_NodeType_Offset;
    case SQL_NodeType_AggregateCall: return IR_NodeType_AggregateCall;
    case SQL_NodeType_Index:        return IR_NodeType_Index;
    case SQL_NodeType_DropIndex:    return IR_NodeType_DropIndex;
    case SQL_NodeType_Null:         return IR_NodeType_Null;
    case SQL_NodeType_NotNull:      return IR_NodeType_NotNull;
    case SQL_NodeType_Unique:       return IR_NodeType_Unique;
    case SQL_NodeType_PrimaryKey:   return IR_NodeType_PrimaryKey;
    case SQL_NodeType_ForeignKey:   return IR_NodeType_ForeignKey;
    case SQL_NodeType_Check:        return IR_NodeType_Check;

    // special cases
    case SQL_NodeType_Row:           return IR_NodeType_ValueGroup;
    case SQL_NodeType_Type:          return IR_NodeType_Type;
    
    // nodes that may not have a direct IR mapping:     
    case SQL_NodeType_Drop:  
    return IR_NodeType_Condition; // placeholder
    
    default:
    return IR_NodeType_Condition; // fallback for unknown types
  }
}

internal String8
ir_node_type_to_string(IR_NodeType type)
{
  String8 result = str8_lit("no type");
  
  switch (type)
  {
    case IR_NodeType_Use: result = str8_lit("IR_NodeType_Use"); break;
    case IR_NodeType_Describe: result = str8_lit("IR_NodeType_Describe"); break;
    case IR_NodeType_Select: result = str8_lit("IR_NodeType_Select"); break;
    case IR_NodeType_Column: result = str8_lit("IR_NodeType_Column"); break;
    case IR_NodeType_Table: result = str8_lit("IR_NodeType_Table"); break;
    case IR_NodeType_Database: result = str8_lit("IR_NodeType_Database"); break;
    case IR_NodeType_Where: result = str8_lit("IR_NodeType_Where"); break;
    case IR_NodeType_Create: result = str8_lit("IR_NodeType_Create"); break;
    case IR_NodeType_Condition: result = str8_lit("IR_NodeType_Condition"); break;
    case IR_NodeType_Operator: result = str8_lit("IR_NodeType_Operator"); break;
    case IR_NodeType_Numeric: result = str8_lit("IR_NodeType_Numeric"); break;
    case IR_NodeType_Literal: result = str8_lit("IR_NodeType_Literal"); break;
    case IR_NodeType_OrderBy: result = str8_lit("IR_NodeType_OrderBy"); break;
    case IR_NodeType_Ascending: result = str8_lit("IR_NodeType_Ascending"); break;
    case IR_NodeType_Descending: result = str8_lit("IR_NodeType_Descending"); break;
    case IR_NodeType_Insert: result = str8_lit("IR_NodeType_Insert"); break;
    case IR_NodeType_Import: result = str8_lit("IR_NodeType_Import"); break;
    case IR_NodeType_Value: result = str8_lit("IR_NodeType_Value"); break;
    case IR_NodeType_ValueGroup: result = str8_lit("IR_NodeType_ValueGroup"); break;
    case IR_NodeType_ColumnList: result = str8_lit("IR_NodeType_ColumnList"); break;
    case IR_NodeType_Delete: result = str8_lit("IR_NodeType_Delete"); break;
    case IR_NodeType_Alter: result = str8_lit("IR_NodeType_Alter"); break;
    case IR_NodeType_AddColumn: result = str8_lit("IR_NodeType_AddColumn"); break;
    case IR_NodeType_Type: result = str8_lit("IR_NodeType_Type"); break;
    case IR_NodeType_DropColumn: result = str8_lit("IR_NodeType_DropColumn"); break;
    case IR_NodeType_Rename: result = str8_lit("IR_NodeType_Rename"); break;
    case IR_NodeType_Join: result = str8_lit("IR_NodeType_Join"); break;
    case IR_NodeType_Alias: result = str8_lit("IR_NodeType_Alias"); break;
    case IR_NodeType_GroupBy: result = str8_lit("IR_NodeType_GroupBy"); break;
    case IR_NodeType_Having: result = str8_lit("IR_NodeType_Having"); break;
    case IR_NodeType_Limit: result = str8_lit("IR_NodeType_Limit"); break;
    case IR_NodeType_Offset: result = str8_lit("IR_NodeType_Offset"); break;
    case IR_NodeType_AggregateCall: result = str8_lit("IR_NodeType_AggregateCall"); break;
    case IR_NodeType_Index: result = str8_lit("IR_NodeType_Index"); break;
    case IR_NodeType_DropIndex: result = str8_lit("IR_NodeType_DropIndex"); break;
    case IR_NodeType_Null: result = str8_lit("IR_NodeType_Null"); break;
    case IR_NodeType_NotNull: result = str8_lit("IR_NodeType_NotNull"); break;
    case IR_NodeType_Unique: result = str8_lit("IR_NodeType_Unique"); break;
    case IR_NodeType_PrimaryKey: result = str8_lit("IR_NodeType_PrimaryKey"); break;
    case IR_NodeType_ForeignKey: result = str8_lit("IR_NodeType_ForeignKey"); break;
    case IR_NodeType_Check: result = str8_lit("IR_NodeType_Check"); break;
  }
  
  return result;
}

internal IR_Node*
ir_node_find_child(IR_Node* parent, IR_NodeType type)
{
  if (!parent) return NULL;
  
  for (IR_Node* child = parent->first; child != NULL; child = child->next)
  {
    if (child->type == type)
    {
      return child;
    }
  }
  
  return NULL;
}

internal GDB_ColumnType
ir_find_column_type(GDB_Database* database, IR_Node* select_ir_node, String8 column_name)
{
  IR_Node* table_node = ir_node_find_child(select_ir_node, IR_NodeType_Table);
  if (!table_node) return GDB_ColumnType_Invalid;
  
  GDB_Table* table = gdb_database_find_table(database, table_node->value);
  if (!table) return GDB_ColumnType_Invalid;
  
  GDB_Column* column = gdb_table_find_column(table, column_name);
  if (!column) return GDB_ColumnType_Invalid;
  
  return column->type;
}

internal void
ir_create_active_column_list(Arena* arena, IR_Node* parent_node, String8List* used_columns)
{
  if (!parent_node) return;
  
  if (parent_node->type == IR_NodeType_Column)
  {
    B32 exists = 0;
    for (String8Node* node = used_columns->first; node != NULL; node = node->next)
    {
      if (str8_match(node->string, parent_node->value, 0))
      {
        exists = 1;
      }
    }
    
    if (!exists)
    {
      str8_list_push(arena, used_columns, parent_node->value);
    }
  }
  
  for (IR_Node* child = parent_node->first; child != NULL; child = child->next)
  {
    ir_create_active_column_list(arena, child, used_columns);
  }
}

internal void
ir_expand_star_to_columns(Arena *arena, GDB_Database *db, IR_Node *select_node)
{
  IR_Node *table_node = ir_node_find_child(select_node, IR_NodeType_Table);
  IR_Node *column_list = ir_node_find_child(select_node, IR_NodeType_ColumnList);
  
  // tec: only a bare 'SELECT *' needs expanding
  B32 is_bare_star = column_list && column_list->first && column_list->first == column_list->last &&
    column_list->first->type == IR_NodeType_Column && str8_match(column_list->first->value, str8_lit("*"), 0);
  if (!is_bare_star) return;
  
  column_list->first = column_list->last = NULL;
  
  GDB_Table *table = gdb_database_find_table_or_catalog(db, table_node->value);
  
  for (U64 i = 0; i < table->column_count; i++)
  {
    GDB_Column* col = table->columns[i];
    IR_Node *col_node = ir_node_make(arena, IR_NodeType_Column, col->name);
    ir_node_add_child(column_list, col_node);
  }
}

internal void
ir_print_node(IR_Node *node, U64 depth)
{
  if (!node) return;
  
  while (node)
  {
    for (int i = 0; i < depth; i++) printf("  ");
    String8 type = ir_node_type_to_string(node->type);
    printf("- [%.*s] %.*s\n", (int)type.size, type.str, (int)node->value.size, node->value.str);
    
    if (node->first) ir_print_node(node->first, depth + 1); 
    node = node->next;
  }
}

internal void
ir_print_query(IR_Query *query)
{
  printf("IR Query:\n");
  ir_print_node(query->execution_nodes, 1);
}
