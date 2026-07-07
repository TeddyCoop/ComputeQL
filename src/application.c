
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
              // tec: NOTE - these scalar values must be arena-allocated (not stack locals) since
              // value_ptr is stashed into row_data[] and only actually consumed later by
              // gdb_table_add_row() once every column in the row has been visited. A stack local
              // scoped to this switch-case's block would get silently clobbered by the next
              // loop iteration's locals before gdb_table_add_row() ever reads it (this previously
              // corrupted every column except the last one in each row).
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
        
        //if (gdb_database_contains_table(database, table_node->value))
        {
          
        }
        //else
        {
          Temp scratch = scratch_begin(0, 0);
          String8 filepath = push_str8f(scratch.arena, "%.*s", 
                                        str8_varg(import_file_node->value));
          //GDB_Table* table = gdb_table_import_csv(database, filepath);
          GDB_Table* table = gdb_table_import_csv_streaming(database, table_node->value, filepath);
          //table->name = push_str8_copy(table->arena, table_node->value);
          //table->name = push_str8_copy(table->arena, table_node->value);
          scratch_end(scratch);
          gdb_database_add_table(database, table);
        }
        
        ProfEnd();
      } break;
      
      //~ tec: gpu
      case IR_NodeType_Select:
      {
        ProfBegin("SQL: Select");
        U64 start_time = os_now_microseconds();
        
        ir_expand_star_to_columns(arena, database, ir_execution_node);
        
        APP_KernelResult result = app_perform_kernel(arena, database, ir_execution_node);
        
        IR_Node* select_output_columns = ir_node_find_child(ir_execution_node, IR_NodeType_ColumnList);
        GDB_Table* table = gdb_database_find_table(database, ir_node_find_child(ir_execution_node, IR_NodeType_Table)->value);
        
        log_info("result count %llu", result.count);
#if PRINT_SELECT_OUTPUT
        Temp scratch = scratch_begin(0, 0);
        for (U64 i = 0; i < result.count; i++)
        {
          U64 row_index = result.indices[i];
          for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next)
          {
            GDB_Column* column = gdb_table_find_column(table, column_node->value);
            void* data = gdb_column_get_data(column, row_index);
            
            switch (column->type)
            {
              case GDB_ColumnType_U32:
              printf("%u ", *(U32*)data);
              break;
              case GDB_ColumnType_U64:
              printf("%llu ", *(U64*)data);
              break;
              case GDB_ColumnType_F32:
              printf("%f ", *(F32*)data);
              break;
              case GDB_ColumnType_F64:
              printf("%lf ", *(F64*)data);
              break;
              case GDB_ColumnType_String8: 
              {
                String8 str = gdb_column_get_string(scratch.arena, column, row_index);
                printf("%.*s ", str8_varg(str));
              } break;
              default:
              printf("UNKNOWN ");
              break;
            }
          }
          printf("\n");
          scratch_end(scratch);
        }
#endif
        
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

internal APP_KernelResult
app_perform_kernel(Arena* arena, GDB_Database* database, IR_Node* root_node)
{
  ProfBeginFunction();

  APP_KernelResult result = { 0 };

  GDB_Table* table = gdb_database_find_table(database, ir_node_find_child(root_node, IR_NodeType_Table)->value);
  if (!table)
  {
    log_error("app_perform_kernel: table not found");
    ProfEnd();
    return result;
  }

  QE_ScanResult scan_result = qe_scan_filter(arena, database, table, root_node);
  result.indices = scan_result.indices;
  result.count = scan_result.count;
  result.cap = scan_result.count;

  ProfEnd();
  return result;
}