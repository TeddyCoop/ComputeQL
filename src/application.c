
internal int
delete_row_index_compare_descending(const void* a, const void* b)
{
  U64 lhs = *(const U64*)a;
  U64 rhs = *(const U64*)b;
  if (lhs < rhs) return 1;
  if (lhs > rhs) return -1;
  return 0;
}

internal void*
gdb_zero_value_for_type(Arena* arena, GDB_ColumnType type)
{
  switch (type)
  {
    case GDB_ColumnType_U32: return push_array(arena, U32, 1);
    case GDB_ColumnType_U64: return push_array(arena, U64, 1);
    case GDB_ColumnType_F32: return push_array(arena, F32, 1);
    case GDB_ColumnType_F64: return push_array(arena, F64, 1);
    case GDB_ColumnType_String8: return push_array(arena, String8, 1);
    default: return NULL;
  }
}

//~ tec: constraint enforcement (PK/UNIQUE/NOT NULL/FOREIGN KEY/CHECK)

internal B32
gdb_candidate_value_equals_row(Arena* arena, GDB_Column* column, void* candidate, U64 existing_row)
{
  if (column->type == GDB_ColumnType_String8)
  {
    String8 existing = gdb_column_get_string(arena, column, existing_row);
    return str8_match(*(String8*)candidate, existing, 0);
  }

  void* existing_data = gdb_column_get_data(column, existing_row);
  if (!existing_data) return 0;

  switch (column->type)
  {
    case GDB_ColumnType_U32: return *(U32*)candidate == *(U32*)existing_data;
    case GDB_ColumnType_U64: return *(U64*)candidate == *(U64*)existing_data;
    case GDB_ColumnType_F32: return *(F32*)candidate == *(F32*)existing_data;
    case GDB_ColumnType_F64: return *(F64*)candidate == *(F64*)existing_data;
    default: return 0;
  }
}

internal B32
gdb_stored_values_equal(Arena* arena, GDB_Column* col_a, U64 row_a, GDB_Column* col_b, U64 row_b)
{
  if (col_a->type == GDB_ColumnType_String8 || col_b->type == GDB_ColumnType_String8)
  {
    if (col_a->type != col_b->type) return 0;
    String8 sa = gdb_column_get_string(arena, col_a, row_a);
    String8 sb = gdb_column_get_string(arena, col_b, row_b);
    return str8_match(sa, sb, 0);
  }
  return qe_read_numeric_as_f64(col_a, row_a) == qe_read_numeric_as_f64(col_b, row_b);
}

// tec: CHECK evaluation against an in flight candidate row
internal F64
gdb_check_load_value(GDB_Table* table, void** row_data, B32* row_null, IR_Node* node, B32* out_is_string, String8* out_string, B32* out_is_null)
{
  *out_is_null = 0;
  *out_is_string = 0;

  if (node->type == IR_NodeType_Column)
  {
    GDB_Column* column = gdb_table_find_column(table, node->value);
    if (!column) return 0.0;

    U64 slot = 0;
    for (; slot < table->column_count; slot++) if (table->columns[slot] == column) break;
    if (slot >= table->column_count) return 0.0;

    if (row_null[slot]) { *out_is_null = 1; return 0.0; }

    if (column->type == GDB_ColumnType_String8)
    {
      *out_is_string = 1;
      *out_string = *(String8*)row_data[slot];
      return 0.0;
    }

    switch (column->type)
    {
      case GDB_ColumnType_U32: return (F64)(*(U32*)row_data[slot]);
      case GDB_ColumnType_U64: return (F64)(*(U64*)row_data[slot]);
      case GDB_ColumnType_F32: return (F64)(*(F32*)row_data[slot]);
      case GDB_ColumnType_F64: return *(F64*)row_data[slot];
      default: return 0.0;
    }
  }
  else if (node->type == IR_NodeType_Literal)
  {
    *out_is_string = 1;
    *out_string = node->value;
    return 0.0;
  }

  return f64_from_str8(node->value);
}

internal B32
gdb_check_eval(GDB_Table* table, void** row_data, B32* row_null, IR_Node* condition)
{
  if (!condition) return 1;

  if (condition->type != IR_NodeType_Operator)
  {
    B32 is_str = 0, is_null = 0;
    String8 s = {0};
    F64 v = gdb_check_load_value(table, row_data, row_null, condition, &is_str, &s, &is_null);
    if (is_null) return 0;
    return is_str ? (s.size > 0) : (v != 0.0);
  }

  String8 op = condition->value;
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;

  if (str8_match(op, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    return gdb_check_eval(table, row_data, row_null, left) && gdb_check_eval(table, row_data, row_null, right);
  }
  if (str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    return gdb_check_eval(table, row_data, row_null, left) || gdb_check_eval(table, row_data, row_null, right);
  }

  if (str8_match(op, str8_lit("is null"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("is not null"), StringMatchFlag_CaseInsensitive))
  {
    B32 is_str = 0, is_null = 0;
    String8 s = {0};
    gdb_check_load_value(table, row_data, row_null, left, &is_str, &s, &is_null);
    B32 is_not = str8_match(op, str8_lit("is not null"), StringMatchFlag_CaseInsensitive);
    return is_not ? !is_null : is_null;
  }

  if (!left || !right)
  {
    log_error("gdb_check_eval: malformed CHECK expression, missing operand(s)");
    return 1;
  }

  B32 lstr = 0, rstr = 0, lnull = 0, rnull = 0;
  String8 ls = {0}, rs = {0};
  F64 lv = gdb_check_load_value(table, row_data, row_null, left, &lstr, &ls, &lnull);
  F64 rv = gdb_check_load_value(table, row_data, row_null, right, &rstr, &rs, &rnull);

  if (lnull || rnull) return 0; // tec: three-valued logic - a NULL operand makes CHECK neither true nor false, so the row is rejected just like a direct comparison would be

  if (lstr || rstr)
  {
    if (str8_match(op, str8_lit("contains"), StringMatchFlag_CaseInsensitive)) return qe_str8_contains(ls, rs);
    B32 eq = qe_str8_compare(ls, rs) == 0;
    if (str8_match(op, str8_lit("!="), 0)) return !eq;
    return eq;
  }

  if (str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0)) return lv == rv;
  if (str8_match(op, str8_lit("!="), 0)) return lv != rv;
  if (str8_match(op, str8_lit("<="), 0)) return lv <= rv;
  if (str8_match(op, str8_lit(">="), 0)) return lv >= rv;
  if (str8_match(op, str8_lit("<"), 0)) return lv < rv;
  if (str8_match(op, str8_lit(">"), 0)) return lv > rv;

  log_error("gdb_check_eval: unsupported operator '%.*s'", str8_varg(op));
  return 1;
}

// tec: validates NOT NULL/UNIQUE/PRIMARY KEY/FOREIGN KEY/CHECK for one candidate row before itss inserted
internal B32
gdb_table_validate_row_constraints(Arena* arena, GDB_Database* database, GDB_Table* table, void** row_data, B32* row_null)
{
  for (U64 i = 0; i < table->column_count; i++)
  {
    GDB_Column* column = table->columns[i];
    if (!gdb_column_has_any_constraint(column)) continue;

    B32 is_null = row_null[i];

    if (column->not_null && is_null)
    {
      log_error("insert: NOT NULL constraint violated on column '%.*s'", str8_varg(column->name));
      return 0;
    }

    if (!is_null && (column->is_unique || column->is_primary_key))
    {
      for (U64 r = 0; r < column->row_count; r++)
      {
        if (gdb_column_is_null(column, r)) continue;
        if (gdb_candidate_value_equals_row(arena, column, row_data[i], r))
        {
          log_error("insert: UNIQUE constraint violated on column '%.*s'", str8_varg(column->name));
          return 0;
        }
      }
    }

    if (!is_null && column->has_foreign_key)
    {
      GDB_Table* ref_table = gdb_database_find_table(database, column->fk_ref_table_name);
      GDB_Column* ref_column = ref_table ? gdb_table_find_column(ref_table, column->fk_ref_column_name) : NULL;

      B32 found = 0;
      if (ref_column)
      {
        for (U64 r = 0; r < ref_column->row_count; r++)
        {
          if (gdb_column_is_null(ref_column, r)) continue;
          if (gdb_candidate_value_equals_row(arena, ref_column, row_data[i], r)) { found = 1; break; }
        }
      }

      if (!found)
      {
        log_error("insert: FOREIGN KEY constraint violated on column '%.*s' - no matching row in '%.*s(%.*s)'",
                   str8_varg(column->name), str8_varg(column->fk_ref_table_name), str8_varg(column->fk_ref_column_name));
        return 0;
      }
    }

    if (column->has_check && column->check_expr)
    {
      if (!gdb_check_eval(table, row_data, row_null, column->check_expr))
      {
        log_error("insert: CHECK constraint violated on column '%.*s'", str8_varg(column->name));
        return 0;
      }
    }
  }

  return 1;
}

internal B32
gdb_row_has_referencing_children(Arena* arena, GDB_Database* database, GDB_Table* table, U64 row_index)
{
  for (U64 t = 0; t < database->table_count; t++)
  {
    GDB_Table* other = database->tables[t];
    if (other == table) continue;

    for (U64 c = 0; c < other->column_count; c++)
    {
      GDB_Column* fk_column = other->columns[c];
      if (!fk_column->has_foreign_key) continue;
      if (!str8_match(fk_column->fk_ref_table_name, table->name, 0)) continue;

      GDB_Column* ref_column = gdb_table_find_column(table, fk_column->fk_ref_column_name);
      if (!ref_column) continue;
      if (gdb_column_is_null(ref_column, row_index)) continue; // tec: NULL is never referenced

      for (U64 r = 0; r < fk_column->row_count; r++)
      {
        if (gdb_column_is_null(fk_column, r)) continue;
        if (gdb_stored_values_equal(arena, ref_column, row_index, fk_column, r)) return 1;
      }
    }
  }
  return 0;
}

internal void
app_execute_query(String8 sql_query)
{
  ProfBeginFunction();
  
  Arena* arena = arena_alloc(.reserve_size=Max(GB(1), GPU_MAX_BUFFER_SIZE), .commit_size=MB(64));
  
  SQL_TokenizeResult tokenize_result = sql_tokenize_from_text(arena, sql_query);
  SQL_Node* sql_root = sql_parse(arena, tokenize_result.tokens, tokenize_result.count, sql_query);
  
  if (sql_root == NULL || g_sql_parse_error.has_error)
  {
    log_error("failed to parse query, aborting");
    arena_release(arena);
    ProfEnd();
    return;
  }
  
  IR_Query* ir_query = ir_generate_from_ast(arena, sql_root);
  //sql_tokens_print(tokenize_result);
  //sql_print_ast(sql_root);
  //ir_print_query(ir_query);
  
  
  GDB_Database* database = NULL;
  
  for (IR_Node* ir_execution_node = ir_query->execution_nodes; ir_execution_node != NULL;
       ir_execution_node = ir_execution_node->next)
  {
    switch (ir_execution_node->type)
    {
      //~ tec: cpu
      
      case IR_NodeType_Use:
      {
        IR_Node* use_ir_node = ir_node_find_child(ir_execution_node, IR_NodeType_Database);
        
        if (use_ir_node->type == IR_NodeType_Database)
        {
          String8 database_path = push_str8f(arena, "gdb_data/%.*s", str8_varg(use_ir_node->value));
          database = gdb_database_load(database_path);
          gdb_add_database(database);
        }
      } break;
      
      case IR_NodeType_Describe:
      {
        IR_Node* table_ir_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);

        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
        }
        else
        {
          GDB_Table* table = gdb_database_find_table(database, table_ir_node->value);

          if (!table)
          {
            log_error("table '%.*s' does not exist", str8_varg(table_ir_node->value));
          }
          else
          {
            printf("Table: %.*s (%llu row%s)\n\n", str8_varg(table->name), table->row_count,
                   table->row_count == 1 ? "" : "s");
            printf("%-24s %-10s %-5s %-5s %s\n", "Column", "Type", "Null", "Key", "Extra");
            printf("%-24s %-10s %-5s %-5s %s\n", "------", "----", "----", "---", "-----");

            for (U64 i = 0; i < table->column_count; i++)
            {
              GDB_Column* column = table->columns[i];

              String8 type_name = gdb_column_type_display_name(column->type);

              String8 key = str8_lit("");
              if (column->is_primary_key) key = str8_lit("PRI");
              else if (column->is_unique) key = str8_lit("UNI");

              GDB_Index* index_on_column = gdb_table_find_index_on_column(table, column);

              Temp scratch = scratch_begin(&arena, 1);
              String8List extra_parts = {0};
              if (column->is_disk_backed) str8_list_push(scratch.arena, &extra_parts, str8_lit("disk-backed"));
              if (index_on_column) str8_list_push(scratch.arena, &extra_parts, push_str8f(scratch.arena, "indexed(%.*s)", str8_varg(index_on_column->name)));
              if (column->has_foreign_key) str8_list_push(scratch.arena, &extra_parts, push_str8f(scratch.arena, "references %.*s(%.*s)", str8_varg(column->fk_ref_table_name), str8_varg(column->fk_ref_column_name)));
              if (column->has_check) str8_list_push(scratch.arena, &extra_parts, push_str8f(scratch.arena, "check(%.*s)", str8_varg(column->check_text)));
              String8 extra = str8_list_join(scratch.arena, &extra_parts, &(StringJoin){.sep = str8_lit(", ")});

              printf("%-24.*s %-10.*s %-5s %-5.*s %.*s\n",
                     str8_varg(column->name),
                     str8_varg(type_name),
                     column->not_null ? "NO" : "YES",
                     str8_varg(key),
                     str8_varg(extra));

              scratch_end(scratch);
            }
          }
        }
      } break;

      case IR_NodeType_Create:
      {
        IR_Node* create_ir_node = ir_execution_node->first;
        
        if (create_ir_node->type == IR_NodeType_Database)
        {
          database = gdb_database_alloc(create_ir_node->value);
          gdb_add_database(database);
        }
        else if (create_ir_node->type == IR_NodeType_Table)
        {
          if (gdb_database_contains_table(database, create_ir_node->value))
          {
            log_error("table '%.*s' already exists", str8_varg(create_ir_node->value));
          }
          else
          {
            GDB_Table* table = gdb_table_alloc(create_ir_node->value);

            for (IR_Node* column_node = create_ir_node->first; column_node != 0; column_node = column_node->next)
            {
              GDB_ColumnType column_type = gdb_column_type_from_string(column_node->first->value);
              GDB_ColumnSchema column_schema = gdb_column_schema_create(column_node->value, column_type);
              gdb_table_add_column(table, column_schema);

              GDB_Column* new_column = table->columns[table->column_count - 1];

              // tec: column_node->first is the Type node
			  // any constraint clauses (NOT NULL, UNIQUE, PRIMARY KEY, REFERENCES, CHECK) 
			  // were appended as its siblings by the parser, in whatever order they appeared
              for (IR_Node* c = column_node->first->next; c != 0; c = c->next)
              {
                switch (c->type)
                {
                  case IR_NodeType_NotNull:
                  {
                    new_column->not_null = 1;
                  } break;
                  case IR_NodeType_Unique:
                  {
                    new_column->is_unique = 1;
                  } break;
                  case IR_NodeType_PrimaryKey:
                  {
                    // tec: single column PRIMARY KEY only
					// ted: TODO support composite keys
                    new_column->is_primary_key = 1;
                    new_column->not_null = 1;
                    new_column->is_unique = 1;
                  } break;
                  case IR_NodeType_ForeignKey:
                  {
                    GDB_Table* ref_table = gdb_database_find_table(database, c->value);
                    GDB_Column* ref_column = ref_table ? gdb_table_find_column(ref_table, c->first->value) : NULL;
                    if (!ref_table || !ref_column)
                    {
                      log_error("create table: foreign key on column '%.*s' references unknown '%.*s(%.*s)' - ignoring it",
                                 str8_varg(new_column->name), str8_varg(c->value), str8_varg(c->first->value));
                    }
                    else
                    {
                      new_column->has_foreign_key = 1;
                      new_column->fk_ref_table_name = push_str8_copy(table->arena, c->value);
                      new_column->fk_ref_column_name = push_str8_copy(table->arena, c->first->value);
                    }
                  } break;
                  case IR_NodeType_Check:
                  {
                    new_column->has_check = 1;
                    new_column->check_text = push_str8_copy(table->arena, c->value);
                    new_column->check_expr = c->first;
                  } break;
                  default: break;
                }
              }
            }

            gdb_database_add_table(database, table);
          }
        }
        else if (create_ir_node->type == IR_NodeType_Index)
        {
          IR_Node* table_node = create_ir_node->first;
          IR_Node* column_node = table_node ? table_node->next : NULL;

          GDB_Table* table = gdb_database_find_table(database, table_node->value);
          if (!table)
          {
            log_error("create index: unknown table '%.*s'", str8_varg(table_node->value));
          }
          else if (gdb_table_find_index(table, create_ir_node->value))
          {
            log_error("create index: index '%.*s' already exists on table '%.*s'",
                       str8_varg(create_ir_node->value), str8_varg(table->name));
          }
          else
          {
            GDB_Column* column = gdb_table_find_column(table, column_node->value);
            if (column)
            {
              gdb_table_create_index(table, create_ir_node->value, column);
            }
          }
        }

      } break;
      case IR_NodeType_DropIndex:
      {
        IR_Node* table_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        GDB_Table* table = gdb_database_find_table(database, table_node->value);
        if (table)
        {
          gdb_table_drop_index(table, ir_execution_node->value);
        }
      } break;
      case IR_NodeType_Insert:
      {
        // tec: table
        IR_Node* table_object = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        GDB_Table* table = gdb_database_find_table(database, table_object->value);

        // tec: optional explicit column list
		// if omitted, value positions map to all table columns in schema order
        IR_Node* next_object = table_object->next;
        IR_Node* columns_object = (next_object && next_object->type == IR_NodeType_ColumnList) ? next_object : NULL;
        IR_Node* values_object = columns_object ? columns_object->next : next_object;

        if (!values_object)
        {
          log_error("missing 'values' clause in 'insert' statement");
          return;
        }

        //- tec: value group
        Temp scratch = scratch_begin(0, 0);

        // tec: maps value-group position -> table->columns[] slot
        U64* column_slots = push_array(scratch.arena, U64, Max(table->column_count, 1));

        U64 listed_count = table->column_count;

        if (columns_object)
        {
          listed_count = 0;
          for (IR_Node* c = columns_object->first; c != 0; c = c->next) listed_count++;

          if (listed_count > table->column_count)
          {
            log_error("insert column list names more columns than table '%.*s' has", str8_varg(table->name));
            return;
          }

          U64 ci = 0;
          for (IR_Node* c = columns_object->first; c != 0; c = c->next, ci++)
          {
            GDB_Column* column = gdb_table_find_column(table, c->value);
            if (!column)
            {
              log_error("unknown column '%.*s' in 'insert' statement", str8_varg(c->value));
              return;
            }

            U64 slot = 0;
            for (; slot < table->column_count; slot++)
            {
              if (table->columns[slot] == column) break;
            }
            column_slots[ci] = slot;
          }
        }
        else
        {
          for (U64 i = 0; i < table->column_count; i++) column_slots[i] = i;
        }

        void** row_data = push_array(scratch.arena, void*, table->column_count);
        B32* row_null = push_array(scratch.arena, B32, table->column_count);
        B32* slot_was_set = push_array(scratch.arena, B32, table->column_count);

        for (IR_Node* value_group_node = values_object->first; value_group_node != 0; value_group_node = value_group_node->next)
        {
          MemoryZero(slot_was_set, sizeof(B32) * table->column_count);
          U64 column_index = 0;

          for (IR_Node* data_node = value_group_node->first; data_node != 0; data_node = data_node->next)
          {
            if (column_index >= listed_count)
            {
              log_error("too many values in 'insert' statement");
              return;
            }

            U64 slot = column_slots[column_index];
            GDB_Column* column = table->columns[slot];
            B32 is_null = (data_node->type == IR_NodeType_Null);
            String8 value_str = data_node->value;

            void* value_ptr = 0;

            if (is_null)
            {
              value_ptr = gdb_zero_value_for_type(scratch.arena, column->type);
            }
            else switch (column->type)
            {
              case GDB_ColumnType_U32:
              {
                U32* value = push_array(scratch.arena, U32, 1);
                *value = (U32)u64_from_str8(value_str, 10);
                value_ptr = value;
              } break;
              case GDB_ColumnType_U64:
              {
                U64* value = push_array(scratch.arena, U64, 1);
                *value = u64_from_str8(value_str, 10);
                value_ptr = value;
              } break;
              case GDB_ColumnType_F32:
              {
                F32* value = push_array(scratch.arena, F32, 1);
                *value = (F32)f64_from_str8(value_str);
                value_ptr = value;
              } break;
              case GDB_ColumnType_F64:
              {
                F64* value = push_array(scratch.arena, F64, 1);
                *value = f64_from_str8(value_str);
                value_ptr = value;
              } break;
              case GDB_ColumnType_String8:
              {
                String8* value = push_array(arena, String8, 1);
                *value = value_str;
                value_ptr = value;
              } break;
              default:
              log_error("unknown column type");
              return;
            }

            row_data[slot] = value_ptr;
            row_null[slot] = is_null;
            slot_was_set[slot] = 1;
            column_index++;
          }

          if (column_index != listed_count)
          {
            log_error("mismatch in column count and value count in 'insert' statement");
            return;
          }

          // tec: any table column not named by a partial column list defaults to NULL
          for (U64 slot = 0; slot < table->column_count; slot++)
          {
            if (slot_was_set[slot]) continue;
            row_data[slot] = gdb_zero_value_for_type(scratch.arena, table->columns[slot]->type);
            row_null[slot] = 1;
          }

          if (!gdb_table_validate_row_constraints(scratch.arena, database, table, row_data, row_null))
          {
            scratch_end(scratch);
            return;
          }

          gdb_table_add_row(table, row_data, row_null);
        }

        scratch_end(scratch);

      } break;
      case IR_NodeType_Alter:
      {
        IR_Node* table_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        GDB_Table* table = gdb_database_find_table(database, table_node->value);

        if (!table)
        {
          log_error("alter table: unknown table '%.*s'", str8_varg(table_node->value));
          break;
        }

        IR_Node* operation_node = table_node->next;

        if (operation_node && operation_node->type == IR_NodeType_AddColumn)
        {
          IR_Node* column_name_node = operation_node->first;
          IR_Node* type_node = column_name_node ? column_name_node->next : NULL;

          B32 column_exists = 0;
          for (U64 i = 0; i < table->column_count; i++)
          {
            if (str8_match(table->columns[i]->name, column_name_node->value, StringMatchFlag_CaseInsensitive))
            {
              column_exists = 1;
              break;
            }
          }

          if (column_exists)
          {
            log_error("alter table: column '%.*s' already exists on table '%.*s'",
                       str8_varg(column_name_node->value), str8_varg(table->name));
            break;
          }

          if (!type_node)
          {
            log_error("alter table: 'add column' requires a column type");
            break;
          }

          GDB_ColumnType column_type = gdb_column_type_from_string(type_node->value);
          GDB_ColumnSchema schema = gdb_column_schema_create(column_name_node->value, column_type);
          gdb_table_add_column(table, schema);

          // tec: backfill existing rows as NULL so the new column's row count stays in sync
          // with the table (matches standard SQL ALTER TABLE ADD COLUMN semantics)
          GDB_Column* new_column = table->columns[table->column_count - 1];
          U64 existing_row_count = table->row_count;

          Temp backfill_scratch = scratch_begin(0, 0);
          for (U64 i = 0; i < existing_row_count; i++)
          {
            void* default_value = gdb_zero_value_for_type(backfill_scratch.arena, column_type);
            gdb_column_add_data_maybe_null(new_column, default_value, 1);
          }
          scratch_end(backfill_scratch);
        }
        else if (operation_node && operation_node->type == IR_NodeType_DropColumn)
        {
          GDB_Column* column = gdb_table_find_column(table, operation_node->value);
          if (!column)
          {
            log_error("alter table: unknown column '%.*s' on table '%.*s'",
                       str8_varg(operation_node->value), str8_varg(table->name));
            break;
          }

          gdb_table_remove_column(table, column);
        }
        else if (operation_node && operation_node->type == IR_NodeType_Rename)
        {
          if (gdb_database_contains_table(database, operation_node->value))
          {
            log_error("alter table: table '%.*s' already exists", str8_varg(operation_node->value));
            break;
          }

          log_info("alter table: renaming '%.*s' to '%.*s' - any already-saved on-disk directory for the old name is not removed",
                    str8_varg(table->name), str8_varg(operation_node->value));

          table->name = operation_node->value;
        }
        else
        {
          log_error("alter table: unsupported or missing operation");
        }
      } break;
      case IR_NodeType_Delete:
      {
        ProfBegin("SQL: Delete");

        IR_Node* table_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        GDB_Table* table = gdb_database_find_table(database, table_node->value);

        if (!table)
        {
          log_error("delete: unknown table '%.*s'", str8_varg(table_node->value));
          ProfEnd();
          break;
        }

        B32 has_disk_backed_column = 0;
        for (U64 i = 0; i < table->column_count; i++)
        {
          if (table->columns[i]->is_disk_backed)
          {
            has_disk_backed_column = 1;
            break;
          }
        }

        if (has_disk_backed_column)
        {
          log_error("delete: table '%.*s' has disk-backed columns - row removal is not yet supported for disk-backed tables",
                     str8_varg(table->name));
          ProfEnd();
          break;
        }

        IR_Node* where_clause = ir_node_find_child(ir_execution_node, IR_NodeType_Where);

        QE_ScanResult scan = {0};
        if (qe_try_index_scan(arena, table, where_clause, &scan))
        {
          // tec: index hit
        }
        else if (gdb_table_may_have_nulls(table))
        {
          scan = qe_cpu_scan_filter(arena, table, where_clause);
        }
        else
        {
          scan = qe_scan_filter(arena, database, table, where_clause);
        }

        // tec: FOREIGN KEY RESTRICT 
		// check every row before removing any of them, so a violation partway through doesnt leave the delete half applied
        B32 restricted = 0;
        for (U64 i = 0; i < scan.count; i++)
        {
          if (gdb_row_has_referencing_children(arena, database, table, scan.indices[i]))
          {
            log_error("delete: row %llu of '%.*s' is still referenced by a FOREIGN KEY in another table - refusing to delete",
                      scan.indices[i], str8_varg(table->name));
            restricted = 1;
            break;
          }
        }

        if (restricted)
        {
          ProfEnd();
          break;
        }

        quick_sort(scan.indices, scan.count, sizeof(U64), delete_row_index_compare_descending);

        for (U64 i = 0; i < scan.count; i++)
        {
          gdb_table_remove_row(table, scan.indices[i]);
        }

        log_info("deleted %llu row(s) from '%.*s'", scan.count, str8_varg(table->name));

        ProfEnd();
      } break;
      
      case IR_NodeType_Import:
      {
        ProfBegin("SQL: Import");
        
        IR_Node* table_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        IR_Node* import_file_node = ir_node_find_child(ir_execution_node, IR_NodeType_Literal);
        
        Temp scratch = scratch_begin(0, 0);
        String8 filepath = push_str8f(scratch.arena, "%.*s",
                                      str8_varg(import_file_node->value));
        GDB_Table* table = gdb_table_import_csv_streaming(database, table_node->value, filepath);
        scratch_end(scratch);
        
        // tec: a table with this name may already exist, replace it
        gdb_database_replace_table(database, table);
        
        ProfEnd();
      } break;
      
      //~ tec: gpu
      case IR_NodeType_Select:
      {
        ProfBegin("SQL: Select");
        U64 start_time = os_now_microseconds();
        
        ir_expand_star_to_columns(arena, database, ir_execution_node);
        
        PLAN_ExecResult result = app_perform_kernel(arena, database, ir_execution_node);
        
        IR_Node* select_output_columns = ir_node_find_child(ir_execution_node, IR_NodeType_ColumnList);
        U64 result_count = result.is_materialized ? result.materialized.count : result.rows.count;
        
        log_info("result count %llu", result_count);
        
        if (result.supported && select_output_columns)
        {
          Temp scratch = scratch_begin(0, 0);

          if (result.is_materialized)
          {
            for (U64 i = 0; i < result_count; i++)
            {
              for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next)
              {
                String8 name = qe_column_list_item_display_name(scratch.arena, column_node);
                PLAN_AggColumn* col = NULL;
                for (U64 c = 0; c < result.materialized.column_count; c++)
                {
                  if (str8_match(result.materialized.columns[c].name, name, 0))
                  {
                    col = &result.materialized.columns[c];
                    break;
                  }
                }

                if (!col)
                {
                  printf("? ");
                }
                else if (col->type == GDB_ColumnType_String8)
                {
                  printf("%.*s ", str8_varg(col->string_values[i]));
                }
                else if (col->type == GDB_ColumnType_U32 || col->type == GDB_ColumnType_U64)
                {
                  printf("%llu ", (U64)col->numeric_values[i]);
                }
                else
                {
                  printf("%lf ", col->numeric_values[i]);
                }
              }
              printf("\n");
            }
          }
          else
          {
            U64 column_count = 0;
            for (IR_Node* c = select_output_columns->first; c != NULL; c = c->next) column_count++;

            typedef struct SelectColGather SelectColGather;
            struct SelectColGather
            {
              B32 resolved;
              GDB_Table* col_table;
              GDB_Column* column;
              U64 table_slot;
              GDB_ColumnType type;
              F64* numeric_values;
              GDB_StringDataChunk strings;
            };

            SelectColGather* gathered = push_array(scratch.arena, SelectColGather, Max(column_count, 1));

            U64 ci = 0;
            for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next, ci++)
            {
              String8 bare_name = {0};
              U64 table_slot = max_U64;
              GDB_Table* col_table = qe_resolve_column_table(&result.rows, column_node->value, &bare_name, &table_slot);
              if (!col_table) continue;

              GDB_Column* column = gdb_table_find_column(col_table, bare_name);
              if (!column) continue;

              gathered[ci].resolved = 1;
              gathered[ci].col_table = col_table;
              gathered[ci].column = column;
              gathered[ci].table_slot = table_slot;
              gathered[ci].type = column->type;

              if (column->type == GDB_ColumnType_String8)
              {
                gathered[ci].strings = qe_gather_string_column(scratch.arena, &result.rows, table_slot, column);
              }
              else
              {
                gathered[ci].numeric_values = qe_gather_numeric_column(scratch.arena, &result.rows, table_slot, column);
              }
            }

            for (U64 i = 0; i < result_count; i++)
            {
              ci = 0;
              for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next, ci++)
              {
                if (!gathered[ci].resolved)
                {
                  printf("? ");
                  continue;
                }

                U64 row_index = result.rows.row_indices[gathered[ci].table_slot][i];
                if (row_index == PLAN_NULL_ROW || gdb_column_is_null(gathered[ci].column, row_index))
                {
                  printf("NULL ");
                  continue;
                }

                switch (gathered[ci].type)
                {
                  case GDB_ColumnType_U32:
                  printf("%u ", (U32)gathered[ci].numeric_values[i]);
                  break;
                  case GDB_ColumnType_U64:
                  printf("%llu ", (U64)gathered[ci].numeric_values[i]);
                  break;
                  case GDB_ColumnType_F32:
                  printf("%f ", (F32)gathered[ci].numeric_values[i]);
                  break;
                  case GDB_ColumnType_F64:
                  printf("%lf ", gathered[ci].numeric_values[i]);
                  break;
                  case GDB_ColumnType_String8:
                  {
                    GDB_StringDataChunk* chunk = &gathered[ci].strings;
                    U64 start = chunk->offsets[i];
                    U64 end = chunk->offsets[i + 1];
                    String8 str = str8((U8*)chunk->data + start, end - start);
                    printf("%.*s ", str8_varg(str));
                  } break;
                  default:
                  printf("UNKNOWN ");
                  break;
                }
              }
              printf("\n");
            }
          }

          scratch_end(scratch);
        }
        
        log_info("total 'SELECT' query time: %.4f ms", (os_now_microseconds() - start_time) / 1000.0f);
        
        ProfEnd();
      } break;
      
    }
  }
  
  if (database)
  {
    String8 database_filepath = push_str8f(arena, "gdb_data/%.*s", (U32)database->name.size, database->name.str);
    gdb_database_save(database, database_filepath);
  }
  
  //String8 table_filepath = push_str8f(arena, "gdb_data/benchmark/%.*s/", str8_varg(database->tables[0]->name));
  //gdb_table_save(database->tables[0], table_filepath);
  
  //gdb_table_export_csv(database->tables[0], str8_lit("data/output.csv"));
  
  //test_print_database(database);
  
  arena_release(arena);
  ProfEnd();
}

internal PLAN_ExecResult
app_perform_kernel(Arena* arena, GDB_Database* database, IR_Node* root_node)
{
  ProfBeginFunction();
  
  PLAN_Node* plan = plan_build_from_select(arena, database, root_node);
  PLAN_ExecResult result = plan_execute(arena, database, plan, root_node);
  
  if (!result.supported)
  {
    log_error("app_perform_kernel: query has no supported execution path yet, returning no rows");
  }
  
  ProfEnd();
  return result;
}