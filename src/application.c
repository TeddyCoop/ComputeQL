
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
          GDB_Table* table = gdb_table_alloc(create_ir_node->value);
          
          for (IR_Node* column_node = create_ir_node->first; column_node != 0; column_node = column_node->next)
          {
            GDB_ColumnType column_type = gdb_column_type_from_string(column_node->first->value);
            GDB_ColumnSchema column_schema = gdb_column_schema_create(column_node->value, column_type);
            gdb_table_add_column(table, column_schema);
          }
          
          gdb_database_add_table(database, table);
        }
        
      } break;
      case IR_NodeType_Insert:
      {
        // tec: table
        IR_Node* table_object = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        GDB_Table* table = gdb_database_find_table(database, table_object->value);
        
        // tec: skip column defs
        IR_Node* columns_object = table_object->next;
        
        // tec: values
        IR_Node* values_object = columns_object->next;
        
        //- tec: value group
        Temp scratch = scratch_begin(0, 0);
        
        void** row_data = push_array(scratch.arena, void*, table->column_count);
        
        for (IR_Node* value_group_node = values_object->first; value_group_node != 0; value_group_node = value_group_node->next)
        {
          U64 column_index = 0;
          
          for (IR_Node* data_node = value_group_node->first; data_node != 0; data_node = data_node->next)
          {
            if (column_index >= table->column_count)
            {
              log_error("too many values in 'insert' statement");
              return;
            }
            
            GDB_Column* column = table->columns[column_index];
            String8 value_str = data_node->value;
            
            void* value_ptr = 0;
            
            switch (column->type)
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
            
            row_data[column_index] = value_ptr;
            column_index++;
          }
          
          if (column_index != table->column_count)
          {
            log_error("mismatch in column count and value count in 'insert' statement");
            return;
          }
          
          gdb_table_add_row(table, row_data);
        }
        
        scratch_end(scratch);
        
      } break;
      case IR_NodeType_Alter:
      {
        IR_Node* table_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        
      } break;
      case IR_NodeType_Delete:
      {
        /*
        String8 kernel_name = str8_lit("delete_query");
        IR_Node* where_clause = ir_node_find_child(ir_execution_node, IR_NodeType_Where);
        String8List active_columns = { 0 };
        ir_create_active_column_list(arena, where_clause, &active_columns);
        
        //~ tec: output
        IR_Node* select_output_columns = ir_node_find_child(ir_execution_node, IR_NodeType_ColumnList);
        for (U64 i = 0; i < filtered_count; i++)
        {
          B32 should_delete = filtered_results[i];
          if (should_delete == 1)
          {
            gdb_table_remove_row(table, i);
          }
        }
        */
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
              GDB_Table* col_table = qe_resolve_column_table(&result.rows, column_node->value, &bare_name);
              if (!col_table) continue;

              GDB_Column* column = gdb_table_find_column(col_table, bare_name);
              if (!column) continue;

              gathered[ci].resolved = 1;
              gathered[ci].col_table = col_table;
              gathered[ci].table_slot = qe_rowset_table_slot(&result.rows, col_table);
              gathered[ci].type = column->type;

              if (column->type == GDB_ColumnType_String8)
              {
                gathered[ci].strings = qe_gather_string_column(scratch.arena, &result.rows, col_table, column);
              }
              else
              {
                gathered[ci].numeric_values = qe_gather_numeric_column(scratch.arena, &result.rows, col_table, column);
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
                if (row_index == PLAN_NULL_ROW)
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