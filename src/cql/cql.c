internal void
cql_init(void)
{
  ProfBeginFunction();
  
  U64 state_arena_reserve_size = settings_u64(str8_lit("CQL_STATE_ARENA_RESERVE_SIZE"), CQL_STATE_ARENA_RESERVE_SIZE);
  U64 state_arena_commit_size = settings_u64(str8_lit("CQL_STATE_ARENA_COMMIT_SIZE"), CQL_STATE_ARENA_COMMIT_SIZE);
  Arena* arena = arena_alloc(.reserve_size=state_arena_reserve_size, .commit_size=state_arena_commit_size);
  g_cql_state = push_array(arena, CQL_State, 1);
  g_cql_state->arena = arena;
  
  g_cql_state->databases = NULL;
  g_cql_state->rw_mutex = os_rw_mutex_alloc();
  
  g_cql_state->disk_backed_threshold_size = settings_u64(str8_lit("CQL_DISK_BACKED_THRESHOLD_SIZE"), CQL_DISK_BACKED_THRESHOLD_SIZE);
  g_cql_state->column_expand_count = settings_u64(str8_lit("CQL_COLUMN_EXPAND_COUNT"), CQL_COLUMN_EXPAND_COUNT);
  g_cql_state->column_variable_capacity_alloc_size = settings_u64(str8_lit("CQL_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE"), CQL_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE);
  g_cql_state->column_max_grow_by_size = settings_u64(str8_lit("CQL_COLUMN_MAX_GROW_BY_SIZE"), CQL_COLUMN_MAX_GROW_BY_SIZE);
  g_cql_state->table_expand_factor = settings_f64(str8_lit("CQL_TABLE_EXPAND_FACTOR"), CQL_TABLE_EXPAND_FACTOR);
  g_cql_state->dict_encode_min_rows = settings_u64(str8_lit("CQL_DICT_ENCODE_MIN_ROWS"), CQL_DICT_ENCODE_MIN_ROWS);
  g_cql_state->dict_encode_max_distinct = settings_u64(str8_lit("CQL_DICT_ENCODE_MAX_DISTINCT"), CQL_DICT_ENCODE_MAX_DISTINCT);
  g_cql_state->dict_encode_max_cardinality_ratio = settings_f64(str8_lit("CQL_DICT_ENCODE_MAX_CARDINALITY_RATIO"), CQL_DICT_ENCODE_MAX_CARDINALITY_RATIO);
  
  U64 zonemap_chunk_rows = settings_u64(str8_lit("CQL_ZONEMAP_CHUNK_ROWS"), CQL_ZONEMAP_CHUNK_ROWS);
  if (!IsPow2(zonemap_chunk_rows))
  {
    log_error("settings: CQL_ZONEMAP_CHUNK_ROWS (%llu) must be a power of two, using default (%d)", zonemap_chunk_rows, CQL_ZONEMAP_CHUNK_ROWS);
    zonemap_chunk_rows = CQL_ZONEMAP_CHUNK_ROWS;
  }
  g_cql_state->zonemap_chunk_rows = zonemap_chunk_rows;
  g_cql_state->zonemap_chunk_rows_log2 = 0;
  while (((U64)1 << g_cql_state->zonemap_chunk_rows_log2) < zonemap_chunk_rows)
  {
    g_cql_state->zonemap_chunk_rows_log2++;
  }
  
  ProfEnd();
}

internal void
cql_release(void)
{
  os_mutex_release(g_cql_state->rw_mutex);
  arena_release(g_cql_state->arena);
}

internal void
cql_add_database(CQL_Database* database)
{
  ProfBeginFunction();
  
  OS_MutexScopeW(g_cql_state->rw_mutex)
  {
    if (g_cql_state->database_count == 0)
    {
      g_cql_state->databases = push_array(g_cql_state->arena, CQL_Database*, 2);
      g_cql_state->database_capacity = 2;
    }
    else if (g_cql_state->database_count >= g_cql_state->database_capacity)
    {
      U64 new_capacity = g_cql_state->database_capacity * 2;
      CQL_Database** new_databases = push_array(g_cql_state->arena, CQL_Database*, new_capacity);
      MemoryCopy(new_databases, g_cql_state->databases, sizeof(CQL_Database*) * g_cql_state->database_count);
      g_cql_state->databases = new_databases;
      g_cql_state->database_capacity = new_capacity;
    }
    
    g_cql_state->databases[g_cql_state->database_count++] = database;
  }
  
  ProfEnd();
}

internal CQL_Database*
cql_state_find_database_by_name(String8 name)
{
  CQL_Database* found = 0;
  
  OS_MutexScopeR(g_cql_state->rw_mutex)
  {
    for (U64 i = 0; i < g_cql_state->database_count; i++)
    {
      if (str8_match(g_cql_state->databases[i]->name, name, 0))
      {
        found = g_cql_state->databases[i];
        break;
      }
    }
  }
  
  return found;
}

//~ tec: database
internal CQL_Database*
cql_database_alloc(String8 name)
{
  Arena* arena = arena_alloc(
                             .reserve_size=settings_u64(str8_lit("CQL_DATABASE_ARENA_RESERVE_SIZE"), CQL_DATABASE_ARENA_RESERVE_SIZE),
                             .commit_size=settings_u64(str8_lit("CQL_DATABASE_ARENA_COMMIT_SIZE"), CQL_DATABASE_ARENA_COMMIT_SIZE));
  CQL_Database* database = push_array(arena, CQL_Database, 1);
  
  database->name = push_str8_copy(arena, name);
  database->tables = NULL;
  database->arena = arena;
  
  return database;
}

internal void
cql_database_release(CQL_Database* database)
{
  arena_release(database->arena);
}

internal void
cql_database_add_table(CQL_Database* database, CQL_Table* table)
{
  if (database->table_count == 0)
  {
    database->tables = push_array(database->arena, CQL_Table*, 2);
    database->table_capacity = 2;
  }
  else if (database->table_count >= database->table_capacity)
  {
    U64 new_capacity = database->table_capacity * 2;
    CQL_Table** new_tables = push_array(database->arena, CQL_Table*, new_capacity);
    MemoryCopy(new_tables, database->tables, sizeof(CQL_Table*) * database->table_count);
    database->tables = new_tables;
    database->table_capacity = new_capacity;
  }
  
  table->parent_database = database;
  database->tables[database->table_count++] = table;
}

internal void
cql_database_replace_table(CQL_Database* database, CQL_Table* new_table)
{
  for (U64 i = 0; i < database->table_count; i++)
  {
    CQL_Table* old_table = database->tables[i];
    if (str8_match(old_table->name, new_table->name, 0))
    {
      for (U64 c = 0; c < old_table->column_count; c++)
      {
        cql_column_close(old_table->columns[c]);
      }
      cql_table_release(old_table);
      
      new_table->parent_database = database;
      database->tables[i] = new_table;
      return;
    }
  }
  
  cql_database_add_table(database, new_table);
}

internal void
cql_database_add_enum_type(CQL_Database* database, CQL_EnumType* enum_type)
{
  if (database->enum_type_count == 0)
  {
    database->enum_types = push_array(database->arena, CQL_EnumType*, 2);
    database->enum_type_capacity = 2;
  }
  else if (database->enum_type_count >= database->enum_type_capacity)
  {
    U64 new_capacity = database->enum_type_capacity * 2;
    CQL_EnumType** new_enum_types = push_array(database->arena, CQL_EnumType*, new_capacity);
    MemoryCopy(new_enum_types, database->enum_types, sizeof(CQL_EnumType*) * database->enum_type_count);
    database->enum_types = new_enum_types;
    database->enum_type_capacity = new_capacity;
  }
  
  database->enum_types[database->enum_type_count++] = enum_type;
}

internal CQL_EnumType*
cql_database_find_enum_type(CQL_Database* database, String8 name)
{
  if (!database) return NULL;
  for (U64 i = 0; i < database->enum_type_count; i++)
  {
    if (str8_match(database->enum_types[i]->name, name, StringMatchFlag_CaseInsensitive))
    {
      return database->enum_types[i];
    }
  }
  return NULL;
}

internal B32
cql_enum_type_code_from_label(CQL_EnumType* enum_type, String8 label, U32* out_code)
{
  for (U32 i = 0; i < enum_type->value_count; i++)
  {
    if (str8_match(enum_type->value_labels[i], label, 0))
    {
      *out_code = i;
      return 1;
    }
  }
  return 0;
}

internal String8
cql_enum_type_label_from_code(CQL_EnumType* enum_type, U32 code)
{
  if (code >= enum_type->value_count) return str8_lit("?");
  return enum_type->value_labels[code];
}

internal B32
cql_database_save(CQL_Database* database, String8 directory)
{
  ProfBeginFunction();
  
  Temp scratch = scratch_begin(0, 0);
  
  if (!os_make_directory(directory))
  {
    log_error("failed to create/open database directory: %s", directory.str);
    return 0;
  }
  
  // tec: enums.meta
  {
    U64 meta_size = sizeof(U64);
    for (U64 i = 0; i < database->enum_type_count; i++)
    {
      CQL_EnumType* enum_type = database->enum_types[i];
      meta_size += sizeof(U64) + enum_type->name.size + sizeof(U32);
      for (U32 v = 0; v < enum_type->value_count; v++)
      {
        meta_size += sizeof(U64) + enum_type->value_labels[v].size;
      }
    }
    
    U8* meta_buffer = push_array(scratch.arena, U8, meta_size);
    U8* meta_ptr = meta_buffer;
    
    *(U64*)meta_ptr = database->enum_type_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < database->enum_type_count; i++)
    {
      CQL_EnumType* enum_type = database->enum_types[i];
      
      *(U64*)meta_ptr = enum_type->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, enum_type->name.str, enum_type->name.size);
      meta_ptr += enum_type->name.size;
      
      *(U32*)meta_ptr = enum_type->value_count; meta_ptr += sizeof(U32);
      for (U32 v = 0; v < enum_type->value_count; v++)
      {
        String8 label = enum_type->value_labels[v];
        *(U64*)meta_ptr = label.size; meta_ptr += sizeof(U64);
        MemoryCopy(meta_ptr, label.str, label.size);
        meta_ptr += label.size;
      }
    }
    
    String8 enums_meta_path = push_str8f(scratch.arena, "%.*s/enums.meta", str8_varg(directory));
    OS_Handle enums_meta_file = os_file_open(OS_AccessFlag_Write, enums_meta_path);
    if (os_handle_match(os_handle_zero(), enums_meta_file))
    {
      log_error("failed to open enums.meta for saving: %.*s", str8_varg(enums_meta_path));
      ProfEnd();
      return 0;
    }
    os_file_write(enums_meta_file, r1u64(0, meta_size), meta_buffer);
    os_file_close(enums_meta_file);
  }
  
  for (U64 i = 0; i < database->table_count; i++)
  {
    CQL_Table* table = database->tables[i];
    String8 table_dir = push_str8f(scratch.arena, "%.*s/%.*s", str8_varg(directory), str8_varg(table->name));
    
    if (!os_make_directory(table_dir))
    {
      log_error("failed to create/open table directory: %.*s", str8_varg(table_dir));
      ProfEnd();
      return 0;
    }
    
    if (!cql_table_save(table, table_dir))
    {
      log_error("failed to save table: %s", table->name.str);
      ProfEnd();
      return 0;
    }
  }
  scratch_end(scratch);
  
  ProfEnd();
  return 1;
}

internal CQL_Database*
cql_database_load(String8 directory_path)
{
  ProfBeginFunction();
  
  CQL_Database* database = cql_database_alloc(str8_lit("temp"));
  Temp scratch = scratch_begin(0, 0);
  
  {
    U8 last_char = directory_path.str[directory_path.size-1];
    if (last_char == '/' || last_char == '\\')
    {
    }
    else
    {
      directory_path = push_str8_cat(scratch.arena, directory_path, str8_lit("/"));
    }
  }
  
  database->name = push_str8_copy(database->arena, str8_skip_last_slash(str8_chop_last_slash(directory_path)));
  
  // tec: load enums.meta
  {
    String8 enums_meta_path = push_str8f(scratch.arena, "%.*senums.meta", str8_varg(directory_path));
    if (os_file_path_exists(enums_meta_path))
    {
      String8 meta_data = os_data_from_file_path(scratch.arena, enums_meta_path);
      U8* read_ptr = meta_data.str;
      U8* end_ptr = meta_data.str + meta_data.size;
      
      if (read_ptr + sizeof(U64) <= end_ptr)
      {
        U64 enum_type_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
        
        for (U64 i = 0; i < enum_type_count; i++)
        {
          if (read_ptr + sizeof(U64) > end_ptr) break;
          String8 name = {0};
          name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
          if (read_ptr + name.size > end_ptr) break;
          name.str = push_array(database->arena, U8, name.size);
          MemoryCopy(name.str, read_ptr, name.size);
          read_ptr += name.size;
          
          if (read_ptr + sizeof(U32) > end_ptr)
          {
            break;
          }
          U32 value_count = *(U32*)read_ptr; read_ptr += sizeof(U32);
          
          CQL_EnumType* enum_type = push_array(database->arena, CQL_EnumType, 1);
          enum_type->name = name;
          enum_type->value_count = value_count;
          enum_type->value_labels = push_array(database->arena, String8, Max(value_count, 1));
          
          B32 truncated = 0;
          for (U32 v = 0; v < value_count; v++)
          {
            if (read_ptr + sizeof(U64) > end_ptr)
            {
              truncated = 1;
              break;
            }
            String8 label = {0};
            label.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
            if (read_ptr + label.size > end_ptr)
            {
              truncated = 1;
              break;
            }
            label.str = push_array(database->arena, U8, label.size);
            MemoryCopy(label.str, read_ptr, label.size);
            read_ptr += label.size;
            enum_type->value_labels[v] = label;
          }
          
          cql_database_add_enum_type(database, enum_type);
          if (truncated) break;
        }
      }
    }
  }
  
  OS_FileIter* it = os_file_iter_begin(scratch.arena, directory_path, OS_FileIterFlag_SkipFiles);
  U64 idx = 0;
  for(OS_FileInfo info = {0}; idx < 16384 && os_file_iter_next(scratch.arena, it, &info); idx += 1)
  {
    String8 table_dir = push_str8_cat(scratch.arena, directory_path, info.name);
    String8 meta_path = push_str8f(scratch.arena, "%.*s/%.*s.meta", str8_varg(table_dir), str8_varg(info.name));
    CQL_Table* table = cql_table_load(database, table_dir, meta_path);
    if (table)
    {
      cql_database_add_table(database, table);
    }
    else
    {
      log_error("failed to load table: %.*s", str8_varg(table_dir));
    }
  }
  
  scratch_end(scratch);
  ProfEnd();
  return database;
}

internal void
cql_database_close(CQL_Database* database)
{
  
}

internal CQL_Table*
cql_database_find_table(CQL_Database* database, String8 table_name)
{
  ProfBeginFunction();
  for (U64 i = 0; i < database->table_count; i++)
  {
    CQL_Table* table = database->tables[i];
    if (str8_match(table->name, table_name, 0))
    {
      ProfEnd();
      return table;
    }
  }
  log_error("failed to find table '%.*s' in database '%.*s'", str8_varg(table_name), str8_varg(database->name));
  ProfEnd();
  return NULL;
}


internal B32
cql_database_contains_table(CQL_Database* database, String8 table_name)
{
  ProfBeginFunction();
  
  for (U64 i = 0; i < database->table_count; i++)
  {
    if (str8_match(database->tables[i]->name, table_name, 0))
    {
      ProfEnd();
      return 1;
    }
  }
  ProfEnd();
  return 0;
}


//~ tec: tables
internal CQL_Table*
cql_table_alloc(String8 name)
{
  Arena* arena = arena_alloc(.reserve_size=settings_u64(str8_lit("CQL_TABLE_ARENA_RESERVE_SIZE"), CQL_TABLE_ARENA_RESERVE_SIZE),
                             .commit_size=settings_u64(str8_lit("CQL_TABLE_ARENA_COMMIT_SIZE"), CQL_TABLE_ARENA_COMMIT_SIZE));
  CQL_Table* table = push_array(arena, CQL_Table, 1);
  
  table->name = push_str8_copy(arena, name);
  table->arena = arena;
  
  return table;
}

internal void
cql_table_release(CQL_Table* table)
{
  arena_release(table->arena);
}

internal void
cql_table_add_column(CQL_Table* table, CQL_ColumnSchema schema)
{
  ProfBeginFunction();
  
  if (table->column_count == 0)
  {
    table->columns = push_array(table->arena, CQL_Column*, 2);
    table->column_capacity = 2;
  }
  else if (table->column_count >= table->column_capacity)
  {
    U64 new_capacity = (U64)ceil_f64(table->column_capacity * g_cql_state->table_expand_factor);
    CQL_Column** new_columns = push_array(table->arena, CQL_Column*, new_capacity);
    MemoryCopy(new_columns, table->columns, sizeof(CQL_Column*) * table->column_count);
    table->columns = new_columns;
    table->column_capacity = new_capacity;
  }
  
  CQL_Column* column = cql_column_alloc(schema.name, schema.type, schema.size);
  column->parent_table = table;
  table->columns[table->column_count++] = column;
  
  ProfEnd();
}

internal void
cql_table_remove_column(CQL_Table* table, CQL_Column* column)
{
  ProfBeginFunction();
  
  U64 slot = 0;
  for (; slot < table->column_count; slot++)
  {
    if (table->columns[slot] == column) break;
  }
  
  if (slot >= table->column_count)
  {
    log_error("cql_table_remove_column: column '%.*s' does not belong to table '%.*s'",
              str8_varg(column->name), str8_varg(table->name));
    ProfEnd();
    return;
  }
  
  // tec: drop any index over this column first
  for (S64 i = (S64)table->index_count - 1; i >= 0; i--)
  {
    if (table->indexes[i]->column == column)
    {
      cql_table_drop_index(table, table->indexes[i]->name);
    }
  }
  
  cql_column_close(column);
  if (column->is_disk_backed)
  {
    os_delete_file_at_path(column->disk_path);
  }
  else if (table->parent_database)
  {
    // tec: is_disk_backed only reflects this process's in-memory state, it could be stale
    Temp scratch = scratch_begin(0, 0);
    String8 stale_path = cql_generate_disk_path_for_column(scratch.arena, column);
    os_delete_file_at_path(stale_path);
    scratch_end(scratch);
  }
  cql_column_release(column);
  
  for (U64 i = slot; i + 1 < table->column_count; i++)
  {
    table->columns[i] = table->columns[i + 1];
  }
  table->column_count--;
  
  ProfEnd();
}

internal void
cql_table_add_row(CQL_Table* table, void** row_data, B32* null_flags)
{
  for (U64 i = 0; i < table->column_count; ++i)
  {
    cql_column_add_data_maybe_null(table->columns[i], row_data[i], null_flags ? null_flags[i] : 0);
  }
  
  U64 new_row_index = table->row_count;
  table->row_count++;
  
  for (U64 i = 0; i < table->index_count; i++)
  {
    cql_index_insert_row(table->indexes[i], new_row_index);
  }
}

internal B32
cql_table_may_have_nulls(CQL_Table* table)
{
  for (U64 i = 0; i < table->column_count; i++)
  {
    if (table->columns[i]->null_flags) return 1;
  }
  return 0;
}

internal void
cql_table_remove_row(CQL_Table* table, U64 row_index)
{
  if (row_index >= table->row_count)
  {
    log_error("row index out of bounds: %llu", row_index);
    return;
  }
  
  for (U64 i = 0; i < table->column_count; ++i)
  {
    cql_column_remove_data(table->columns[i], row_index);
  }
  
  for (U64 i = 0; i < table->index_count; i++)
  {
    cql_index_remove_row(table->indexes[i], row_index);
  }
  
  table->row_count--;
}

internal B32
cql_table_save(CQL_Table* table, String8 table_dir)
{
  ProfBeginFunction();
  
  //- tec: table meta file
  Temp scratch = scratch_begin(0, 0);
  {
    String8 meta_path = push_str8f(scratch.arena, "%.*s/%.*s.meta", str8_varg(table_dir), str8_varg(table->name));
    OS_Handle meta_file = os_file_open(OS_AccessFlag_Write, meta_path);
    if (os_handle_match(os_handle_zero(), meta_file))
    {
      log_error("Failed to open metadata file: %.*s", str8_varg(meta_path));
      return 0;
    }
    
    U64 meta_size = sizeof(U64) * 2;
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      meta_size += sizeof(CQL_ColumnType) + (sizeof(U64) * 3) + column->name.size;
    }
    
    // tec: trailing, optional index declaration section
    meta_size += sizeof(U64);
    for (U64 i = 0; i < table->index_count; i++)
    {
      CQL_Index* index = table->indexes[i];
      meta_size += sizeof(U64) + index->name.size + sizeof(U64) + index->column_name.size;
    }
    
    // tec: trailing, optional list of columns that have a <column>.null
    meta_size += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (column->null_flags) meta_size += sizeof(U64) + column->name.size;
    }
    
    // tec: trailing, optional per column constraint section
    meta_size += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (!cql_column_has_any_constraint(column)) continue;
      
      meta_size += sizeof(U64) + column->name.size; // tec: column name
      meta_size += sizeof(U64);                     // tec: flags bitfield
      if (column->has_foreign_key)
      {
        meta_size += sizeof(U64) + column->fk_ref_table_name.size + sizeof(U64) + column->fk_ref_column_name.size;
      }
      if (column->has_check)
      {
        meta_size += sizeof(U64) + column->check_text.size;
      }
    }
    
    // tec: trailing, optional decimal precision/scale section
    meta_size += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (column->type != CQL_ColumnType_Decimal) continue;
      meta_size += sizeof(U64) + column->name.size + sizeof(U32) + sizeof(U32);
    }
    
    // tec: trailing, optional enum col reference section
    meta_size += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (column->type != CQL_ColumnType_Enum || !column->enum_type) continue;
      meta_size += sizeof(U64) + column->name.size + sizeof(U64) + column->enum_type->name.size;
    }
    
    // tec: trailing, optional per column zone map section
    meta_size += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (!column->has_zone_map) continue;
      meta_size += sizeof(U64) + column->name.size + sizeof(U64) +
        column->zone_map_chunk_count * (sizeof(F64) * 2 + sizeof(U32));
    }
    
    // tec: trailing, optional per column statistics section
    meta_size += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (!cql_column_stats_is_current(column))
      {
        continue;
      }
      meta_size += cql_column_stats_record_size(column);
    }
    
    U8* meta_buffer = push_array(scratch.arena, U8, meta_size);
    U8* meta_ptr = meta_buffer;
    
    *(U64*)meta_ptr = table->column_count; meta_ptr += sizeof(U64);
    *(U64*)meta_ptr = table->row_count; meta_ptr += sizeof(U64);
    
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      
      *(CQL_ColumnType*)meta_ptr = column->type; meta_ptr += sizeof(CQL_ColumnType);
      *(U64*)meta_ptr = column->size; meta_ptr += sizeof(U64);
      *(U64*)meta_ptr = column->capacity; meta_ptr += sizeof(U64);
      *(U64*)meta_ptr = column->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->name.str, column->name.size);
      meta_ptr += column->name.size;
    }
    
    *(U64*)meta_ptr = table->index_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < table->index_count; i++)
    {
      CQL_Index* index = table->indexes[i];
      
      *(U64*)meta_ptr = index->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, index->name.str, index->name.size);
      meta_ptr += index->name.size;
      
      *(U64*)meta_ptr = index->column_name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, index->column_name.str, index->column_name.size);
      meta_ptr += index->column_name.size;
    }
    
    U64 null_column_count = 0;
    for (U64 i = 0; i < table->column_count; i++)
    {
      if (table->columns[i]->null_flags)
      {
        null_column_count++;
      }
    }
    
    *(U64*)meta_ptr = null_column_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (!column->null_flags) continue;
      
      *(U64*)meta_ptr = column->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->name.str, column->name.size);
      meta_ptr += column->name.size;
    }
    
    U64 constraint_column_count = 0;
    for (U64 i = 0; i < table->column_count; i++)
    {
      if (cql_column_has_any_constraint(table->columns[i])) constraint_column_count++;
    }
    
    *(U64*)meta_ptr = constraint_column_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (!cql_column_has_any_constraint(column)) continue;
      
      *(U64*)meta_ptr = column->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->name.str, column->name.size);
      meta_ptr += column->name.size;
      
      U64 flags = 0;
      if (column->not_null) flags |= 0x1;
      if (column->is_unique) flags |= 0x2;
      if (column->is_primary_key) flags |= 0x4;
      if (column->has_foreign_key) flags |= 0x8;
      if (column->has_check) flags |= 0x10;
      *(U64*)meta_ptr = flags; meta_ptr += sizeof(U64);
      
      if (column->has_foreign_key)
      {
        *(U64*)meta_ptr = column->fk_ref_table_name.size; meta_ptr += sizeof(U64);
        MemoryCopy(meta_ptr, column->fk_ref_table_name.str, column->fk_ref_table_name.size);
        meta_ptr += column->fk_ref_table_name.size;
        
        *(U64*)meta_ptr = column->fk_ref_column_name.size; meta_ptr += sizeof(U64);
        MemoryCopy(meta_ptr, column->fk_ref_column_name.str, column->fk_ref_column_name.size);
        meta_ptr += column->fk_ref_column_name.size;
      }
      
      if (column->has_check)
      {
        *(U64*)meta_ptr = column->check_text.size; meta_ptr += sizeof(U64);
        MemoryCopy(meta_ptr, column->check_text.str, column->check_text.size);
        meta_ptr += column->check_text.size;
      }
    }
    
    U64 decimal_column_count = 0;
    for (U64 i = 0; i < table->column_count; i++)
    {
      if (table->columns[i]->type == CQL_ColumnType_Decimal) decimal_column_count++;
    }
    
    *(U64*)meta_ptr = decimal_column_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (column->type != CQL_ColumnType_Decimal) continue;
      
      *(U64*)meta_ptr = column->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->name.str, column->name.size);
      meta_ptr += column->name.size;
      
      *(U32*)meta_ptr = column->decimal_precision; meta_ptr += sizeof(U32);
      *(U32*)meta_ptr = column->decimal_scale; meta_ptr += sizeof(U32);
    }
    
    U64 enum_column_count = 0;
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (column->type == CQL_ColumnType_Enum && column->enum_type) enum_column_count++;
    }
    
    *(U64*)meta_ptr = enum_column_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (column->type != CQL_ColumnType_Enum || !column->enum_type) continue;
      
      *(U64*)meta_ptr = column->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->name.str, column->name.size);
      meta_ptr += column->name.size;
      
      *(U64*)meta_ptr = column->enum_type->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->enum_type->name.str, column->enum_type->name.size);
      meta_ptr += column->enum_type->name.size;
    }
    
    U64 zone_map_column_count = 0;
    for (U64 i = 0; i < table->column_count; i++)
    {
      if (table->columns[i]->has_zone_map) zone_map_column_count++;
    }
    
    *(U64*)meta_ptr = zone_map_column_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (!column->has_zone_map) continue;
      
      *(U64*)meta_ptr = column->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->name.str, column->name.size);
      meta_ptr += column->name.size;
      
      *(U64*)meta_ptr = column->zone_map_chunk_count; meta_ptr += sizeof(U64);
      for (U64 c = 0; c < column->zone_map_chunk_count; c++)
      {
        CQL_ZoneMapChunk* chunk = &column->zone_map[c];
        *(F64*)meta_ptr = chunk->min; meta_ptr += sizeof(F64);
        *(F64*)meta_ptr = chunk->max; meta_ptr += sizeof(F64);
        *(U32*)meta_ptr = chunk->has_values; meta_ptr += sizeof(U32);
      }
    }
    
    U64 stats_column_count = 0;
    for (U64 i = 0; i < table->column_count; i++)
    {
      if (cql_column_stats_is_current(table->columns[i]))
      {
        stats_column_count += 1;
      }
    }
    
    *(U64*)meta_ptr = stats_column_count; meta_ptr += sizeof(U64);
    for (U64 i = 0; i < table->column_count; i++)
    {
      CQL_Column* column = table->columns[i];
      if (!cql_column_stats_is_current(column))
      {
        continue;
      }
      meta_ptr = cql_column_stats_write_record(meta_ptr, column);
    }
    
    os_file_write(meta_file, r1u64(0, meta_size), meta_buffer);
    os_file_close(meta_file);
  }
  scratch_end(scratch);
  
  //- tec: column files
  scratch = scratch_begin(0, 0);
  for (U64 i = 0; i < table->column_count; i++)
  {
    CQL_Column* column = table->columns[i];
    
    cql_column_flush_pending(column);
    if (!column->is_disk_backed)
    {
      String8 column_path = push_str8f(scratch.arena, "%.*s/%.*s.dat", str8_varg(table_dir), str8_varg(column->name));
      OS_Handle file = os_file_open(OS_AccessFlag_Write, column_path);
      if (os_handle_match(os_handle_zero(), file))
      {
        log_error("Failed to open column file for saving: %.*s", str8_varg(column_path));
        break;
        //return 0;
      }
      
      if (column->type == CQL_ColumnType_String8)
      {
        U64 header_size = sizeof(U64);
        U64 data_size = column->variable_capacity;
        U64 offset_size = column->capacity * sizeof(U64);
        U64 total_size = header_size + data_size + offset_size;
        
        U8* buffer = push_array(scratch.arena, U8, total_size);
        U8* write_ptr = buffer;
        
        *(U64*)write_ptr = column->variable_capacity;
        write_ptr += sizeof(U64);
        MemoryCopy(write_ptr, column->data, column->variable_capacity);
        write_ptr += column->variable_capacity;
        MemoryCopy(write_ptr, column->offsets, column->capacity * sizeof(U64));
        
        os_file_write(file, r1u64(0, total_size), buffer);
      }
      else
      {
        U64 data_size = column->capacity * column->size;
        os_file_write(file, r1u64(0, data_size), column->data);
      }
      os_file_close(file);
    }
    
    if (column->null_flags)
    {
      String8 null_path = push_str8f(scratch.arena, "%.*s/%.*s.null", str8_varg(table_dir), str8_varg(column->name));
      OS_Handle null_file = os_file_open(OS_AccessFlag_Write, null_path);
      if (os_handle_match(os_handle_zero(), null_file))
      {
        log_error("Failed to open null-flags file for saving: %.*s", str8_varg(null_path));
      }
      else
      {
        os_file_write(null_file, r1u64(0, table->row_count), column->null_flags);
        os_file_close(null_file);
      }
    }
  }
  scratch_end(scratch);
  
  //- tec: index order files
  for (U64 i = 0; i < table->index_count; i++)
  {
    cql_index_save(table->indexes[i], table_dir);
  }
  
  ProfEnd();
  return 1;
}

internal B32
cql_table_export_csv(CQL_Table* table, String8 path)
{
  ProfBeginFunction();
  
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if (os_handle_match(os_handle_zero(), file))
  {
    log_error("failed to open CSV file for writing: %s", path.str);
    return 0;
  }
  
  Temp scratch = temp_begin(g_cql_state->arena);
  U64 chunk_cap = KB(8);
  String8 chunk = str8_from_memory_size(scratch.arena, chunk_cap);
  chunk.size = 0;
  
  U64 arena_restore_point = scratch.arena->pos;
  
  U64 file_off = 0;
  
#define FLUSH_CHUNK() do { \
if (chunk.size > 0) { \
os_file_write(file, r1u64(file_off, file_off + chunk.size), chunk.str); \
file_off += chunk.size; \
chunk.size = 0; \
scratch.arena->pos = arena_restore_point; \
} \
} while (0)
  
#define APPEND_TO_CHUNK(str8_expr) do { \
String8 __s = (str8_expr); \
if (chunk.size + __s.size > chunk_cap) FLUSH_CHUNK(); \
MemoryCopy(chunk.str + chunk.size, __s.str, __s.size); \
chunk.size += __s.size; \
} while (0)
  
  for (U64 i = 0; i < table->column_count; i++)
  {
    APPEND_TO_CHUNK(table->columns[i]->name);
    if (i < table->column_count - 1)
    {
      APPEND_TO_CHUNK(str8_lit(","));
    }
  }
  APPEND_TO_CHUNK(str8_lit("\n"));
  
  for (U64 row = 0; row < table->row_count; row++)
  {
    for (U64 col = 0; col < table->column_count; col++)
    {
      CQL_Column* column = table->columns[col];
      String8 output = {0};
      
      if (column->type == CQL_ColumnType_U64)
      {
        U64* data = (U64*)cql_column_get_data(column, row);
        output = str8_from_u64(scratch.arena, *data, 10, 0, 0);
      }
      else if (column->type == CQL_ColumnType_U32)
      {
        U32* data = (U32*)cql_column_get_data(column, row);
        output = str8_from_u64(scratch.arena, *data, 10, 0, 0);
      }
      else if (column->type == CQL_ColumnType_F64)
      {
        F64* data = (F64*)cql_column_get_data(column, row);
        output = str8_from_f64(scratch.arena, *data, 6);
      }
      else if (column->type == CQL_ColumnType_F32)
      {
        F32* data = (F32*)cql_column_get_data(column, row);
        output = str8_from_f64(scratch.arena, *data, 6);
      }
      else if (column->type == CQL_ColumnType_String8)
      {
        output = cql_column_get_string(scratch.arena, column, row);
      }
      else if (column->type == CQL_ColumnType_I64)
      {
        S64* data = (S64*)cql_column_get_data(column, row);
        output = str8_from_s64(scratch.arena, *data, 10, 0, 0);
      }
      else if (column->type == CQL_ColumnType_I32)
      {
        S32* data = (S32*)cql_column_get_data(column, row);
        output = str8_from_s64(scratch.arena, *data, 10, 0, 0);
      }
      else if (column->type == CQL_ColumnType_Bool)
      {
        U8* data = (U8*)cql_column_get_data(column, row);
        output = *data ? str8_lit("true") : str8_lit("false");
      }
      else if (column->type == CQL_ColumnType_Date)
      {
        S32* data = (S32*)cql_column_get_data(column, row);
        output = push_iso_date_string(scratch.arena, *data);
      }
      else if (column->type == CQL_ColumnType_Timestamp)
      {
        S64* data = (S64*)cql_column_get_data(column, row);
        output = push_iso_timestamp_string(scratch.arena, *data);
      }
      else if (column->type == CQL_ColumnType_Decimal)
      {
        S64* data = (S64*)cql_column_get_data(column, row);
        output = decimal_to_str8(scratch.arena, *data, column->decimal_scale);
      }
      else if (column->type == CQL_ColumnType_Enum && column->enum_type)
      {
        U32* data = (U32*)cql_column_get_data(column, row);
        output = cql_enum_type_label_from_code(column->enum_type, *data);
      }
      
      if (output.size && output.str)
      {
        APPEND_TO_CHUNK(output);
      }
      
      if (col < table->column_count - 1)
      {
        APPEND_TO_CHUNK(str8_lit(","));
      }
    }
    APPEND_TO_CHUNK(str8_lit("\n"));
  }
  
  FLUSH_CHUNK();
  
  os_file_close(file);
  temp_end(scratch);
  
  ProfEnd();
  return 1;
}

internal IR_Node*
cql_parse_check_expression(Arena* arena, String8 check_text)
{
  g_sql_parse_error = (SQL_ParseError){0};
  g_sql_source_text = check_text;
  
  SQL_TokenizeResult tok = sql_tokenize_from_text(arena, check_text);
  SQL_ParseCtx ctx = sql_parse_ctx_make(arena, tok.tokens, tok.count);
  SQL_Node* expr_sql = sql_parse_logical_expression(&ctx);
  
  if (!expr_sql || g_sql_parse_error.has_error)
  {
    log_error("cql_parse_check_expression: failed to re-parse CHECK expression '%.*s'", str8_varg(check_text));
    return NULL;
  }
  
  return ir_generate_recursive(arena, expr_sql);
}

internal CQL_Table*
cql_table_load(CQL_Database* database, String8 table_dir, String8 meta_path)
{
  ProfBeginFunction();
  
  CQL_Table* table = cql_table_alloc(str8_lit("temp"));
  
  Temp scratch = temp_begin(g_cql_state->arena);
  String8 meta_data = os_data_from_file_path(scratch.arena, meta_path);
  if (meta_data.size == 0)
  {
    log_error("Failed to read metadata: %.*s", str8_varg(meta_path));
    return NULL;
  }
  
  U8* read_ptr = meta_data.str;
  
  table->column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
  table->row_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
  
  table->columns = push_array(table->arena, CQL_Column*, table->column_count);
  
  for (U64 i = 0; i < table->column_count; i++)
  {
    CQL_Column* column = cql_column_alloc(str8_lit("temp"), 0, 0);
    column->row_count = table->row_count;
    
    // tec: bind into the table immediately (rather than at the end of this loop body) so that
    // early 'continue'/'break' paths below (e.g. a freshly-created 0-row column whose .dat file is legitimately empty)
    // still leave table->columns[i] populated instead of NULL
    table->columns[i] = column;
    column->parent_table = table;
    
    column->type = *(CQL_ColumnType*)read_ptr; read_ptr += sizeof(CQL_ColumnType);
    column->size = *(U64*)read_ptr; read_ptr += sizeof(U64);
    column->capacity = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    column->name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
    column->name.str = push_array(table->arena, U8, column->name.size);
    MemoryCopy(column->name.str, read_ptr, column->name.size);
    read_ptr += column->name.size;
    
    String8 column_path = push_str8f(scratch.arena, "%.*s/%.*s.dat", str8_varg(table_dir), str8_varg(column->name));
    OS_Handle file = os_file_open(OS_AccessFlag_Read, column_path);
    if (os_handle_match(os_handle_zero(), file))
    {
      log_error("Failed to open column file: %.*s", str8_varg(column_path));
      break;
      //return NULL;
    }
    
    FileProperties props = os_properties_from_file(file);
    
    if (props.size == 0)
    {
      log_error("%.*s contains no data", str8_varg(column_path));
      os_file_close(file);
      continue;
    }
    
    if (props.size > g_cql_state->disk_backed_threshold_size)
    {
      column->is_disk_backed = 1;
      column->disk_path = push_str8_copy(column->arena, column_path);
      if (column->type == CQL_ColumnType_String8)
      {
        // tec: peek the header for reserved capacity, then read the last offset to find how many of the bytes are actually used
        OS_Handle map = os_file_map_open(OS_AccessFlag_Read, file);
        void* mapped_ptr = os_file_map_view_open(map, OS_AccessFlag_Read, r1u64(0, sizeof(U64)));
        U64 reserved_capacity = *(U64*)mapped_ptr;
        os_file_map_view_close(map, mapped_ptr, r1u64(0, sizeof(U64)));
        os_file_map_close(map);
        
        column->variable_capacity = 0;
        if (column->row_count > 0)
        {
          U64 last_offset_pos = sizeof(U64) + reserved_capacity + (column->row_count - 1) * sizeof(U64);
          os_file_read(file, r1u64(last_offset_pos, last_offset_pos + sizeof(U64)), &column->variable_capacity);
        }
      }
      else
      {
        column->variable_capacity = props.size - (column->capacity * sizeof(U64));
      }
    }
    else
    {
      OS_Handle map = os_file_map_open(OS_AccessFlag_Read, file);
      void* mapped_ptr = os_file_map_view_open(map, OS_AccessFlag_Read, r1u64(0, props.size));
      
      if (column->type == CQL_ColumnType_String8)
      {
        column->variable_capacity = *(U64*)mapped_ptr;
        mapped_ptr = (U8*)mapped_ptr + sizeof(U64);
        
        column->data = push_array(table->arena, U8, column->variable_capacity);
        MemoryCopy(column->data, mapped_ptr, column->variable_capacity);
        mapped_ptr = (U8*)mapped_ptr + column->variable_capacity;
        
        column->offsets = push_array(table->arena, U64, column->capacity);
        MemoryCopy(column->offsets, mapped_ptr, column->capacity * sizeof(U64));
      }
      else
      {
        column->data = push_array(table->arena, U8, column->capacity * column->size);
        MemoryCopy(column->data, mapped_ptr, column->capacity * column->size);
      }
      
      os_file_map_view_close(map, mapped_ptr, r1u64(0, props.size));
      os_file_map_close(map);
    }
    os_file_close(file);
  }
  
  //- tec: optional trailing index declaration
  if (read_ptr + sizeof(U64) <= meta_data.str + meta_data.size)
  {
    U64 index_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    for (U64 i = 0; i < index_count; i++)
    {
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 index_name = {0};
      index_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + index_name.size > meta_data.str + meta_data.size) break;
      index_name.str = push_array(table->arena, U8, index_name.size);
      MemoryCopy(index_name.str, read_ptr, index_name.size);
      read_ptr += index_name.size;
      
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 column_name = {0};
      column_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + column_name.size > meta_data.str + meta_data.size) break;
      column_name.str = push_array(table->arena, U8, column_name.size);
      MemoryCopy(column_name.str, read_ptr, column_name.size);
      read_ptr += column_name.size;
      
      CQL_Column* column = cql_table_find_column(table, column_name);
      if (column)
      {
        CQL_Index* index = cql_table_register_index(table, index_name, column);
        if (!cql_index_load(index, table_dir))
        {
          cql_index_build_order(index);
        }
      }
      else
      {
        log_error("cql_table_load: index '%.*s' refers to missing column '%.*s' - dropping it",
                  str8_varg(index_name), str8_varg(column_name));
      }
    }
  }
  
  //- tec: optional trailing list of columns with a <column>.null sidecar file
  if (read_ptr + sizeof(U64) <= meta_data.str + meta_data.size)
  {
    U64 null_column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    for (U64 i = 0; i < null_column_count; i++)
    {
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 column_name = {0};
      column_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + column_name.size > meta_data.str + meta_data.size) break;
      column_name.str = push_array(table->arena, U8, column_name.size);
      MemoryCopy(column_name.str, read_ptr, column_name.size);
      read_ptr += column_name.size;
      
      CQL_Column* column = cql_table_find_column(table, column_name);
      if (!column)
      {
        log_error("cql_table_load: null-flags file refers to missing column '%.*s' - ignoring it", str8_varg(column_name));
        continue;
      }
      
      String8 null_path = push_str8f(scratch.arena, "%.*s/%.*s.null", str8_varg(table_dir), str8_varg(column_name));
      String8 null_data = os_data_from_file_path(scratch.arena, null_path);
      if (null_data.size < table->row_count)
      {
        log_error("cql_table_load: null-flags file '%.*s' is too small (%llu bytes, expected %llu) - ignoring it",
                  str8_varg(null_path), null_data.size, table->row_count);
        continue;
      }
      
      column->null_flags = push_array(table->arena, U8, table->row_count);
      column->null_flags_capacity = table->row_count;
      MemoryCopy(column->null_flags, null_data.str, table->row_count);
    }
  }
  
  //- tec: optional trailing per-column constraint section (see cql_table_save)
  if (read_ptr + sizeof(U64) <= meta_data.str + meta_data.size)
  {
    U64 constraint_column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    for (U64 i = 0; i < constraint_column_count; i++)
    {
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 column_name = {0};
      column_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + column_name.size > meta_data.str + meta_data.size) break;
      column_name.str = read_ptr;
      read_ptr += column_name.size;
      
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      U64 flags = *(U64*)read_ptr; read_ptr += sizeof(U64);
      
      String8 fk_ref_table_name = {0}, fk_ref_column_name = {0}, check_text = {0};
      B32 has_fk = (flags & 0x8) != 0;
      B32 has_check = (flags & 0x10) != 0;
      
      if (has_fk)
      {
        if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
        fk_ref_table_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
        if (read_ptr + fk_ref_table_name.size > meta_data.str + meta_data.size) break;
        fk_ref_table_name.str = read_ptr;
        read_ptr += fk_ref_table_name.size;
        
        if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
        fk_ref_column_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
        if (read_ptr + fk_ref_column_name.size > meta_data.str + meta_data.size) break;
        fk_ref_column_name.str = read_ptr;
        read_ptr += fk_ref_column_name.size;
      }
      
      if (has_check)
      {
        if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
        check_text.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
        if (read_ptr + check_text.size > meta_data.str + meta_data.size) break;
        check_text.str = read_ptr;
        read_ptr += check_text.size;
      }
      
      CQL_Column* column = cql_table_find_column(table, column_name);
      if (!column)
      {
        log_error("cql_table_load: constraint section refers to missing column '%.*s' - ignoring it", str8_varg(column_name));
        continue;
      }
      
      column->not_null = (flags & 0x1) != 0;
      column->is_unique = (flags & 0x2) != 0;
      column->is_primary_key = (flags & 0x4) != 0;
      
      if (has_fk)
      {
        column->has_foreign_key = 1;
        column->fk_ref_table_name = push_str8_copy(table->arena, fk_ref_table_name);
        column->fk_ref_column_name = push_str8_copy(table->arena, fk_ref_column_name);
      }
      
      if (has_check)
      {
        column->has_check = 1;
        column->check_text = push_str8_copy(table->arena, check_text);
        column->check_expr = cql_parse_check_expression(table->arena, column->check_text);
      }
    }
  }
  
  //- tec: optional trailing decimal precision/scale section
  if (read_ptr + sizeof(U64) <= meta_data.str + meta_data.size)
  {
    U64 decimal_column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    for (U64 i = 0; i < decimal_column_count; i++)
    {
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 column_name = {0};
      column_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + column_name.size > meta_data.str + meta_data.size) break;
      column_name.str = read_ptr;
      read_ptr += column_name.size;
      
      if (read_ptr + sizeof(U32) + sizeof(U32) > meta_data.str + meta_data.size) break;
      U32 precision = *(U32*)read_ptr; read_ptr += sizeof(U32);
      U32 scale = *(U32*)read_ptr; read_ptr += sizeof(U32);
      
      CQL_Column* column = cql_table_find_column(table, column_name);
      if (!column)
      {
        log_error("cql_table_load: decimal section refers to missing column '%.*s' - ignoring it", str8_varg(column_name));
        continue;
      }
      
      column->decimal_precision = precision;
      column->decimal_scale = scale;
    }
  }
  
  //- tec: optional trailing enum column reference section
  if (read_ptr + sizeof(U64) <= meta_data.str + meta_data.size)
  {
    U64 enum_column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    for (U64 i = 0; i < enum_column_count; i++)
    {
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 column_name = {0};
      column_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + column_name.size > meta_data.str + meta_data.size) break;
      column_name.str = read_ptr;
      read_ptr += column_name.size;
      
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 enum_type_name = {0};
      enum_type_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + enum_type_name.size > meta_data.str + meta_data.size) break;
      enum_type_name.str = read_ptr;
      read_ptr += enum_type_name.size;
      
      CQL_Column* column = cql_table_find_column(table, column_name);
      if (!column)
      {
        log_error("cql_table_load: enum section refers to missing column '%.*s' - ignoring it", str8_varg(column_name));
        continue;
      }
      
      CQL_EnumType* enum_type = cql_database_find_enum_type(database, enum_type_name);
      if (!enum_type)
      {
        log_error("cql_table_load: column '%.*s' references unknown enum type '%.*s'",
                  str8_varg(column_name), str8_varg(enum_type_name));
        continue;
      }
      
      column->enum_type = enum_type;
    }
  }
  
  //- tec: optional trailing zone map section
  if (read_ptr + sizeof(U64) <= meta_data.str + meta_data.size)
  {
    U64 zone_map_column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    for (U64 i = 0; i < zone_map_column_count; i++)
    {
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      String8 column_name = {0};
      column_name.size = *(U64*)read_ptr; read_ptr += sizeof(U64);
      if (read_ptr + column_name.size > meta_data.str + meta_data.size) break;
      column_name.str = read_ptr;
      read_ptr += column_name.size;
      
      if (read_ptr + sizeof(U64) > meta_data.str + meta_data.size) break;
      U64 chunk_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
      
      U64 chunks_size = chunk_count * (sizeof(F64) * 2 + sizeof(U32));
      if (read_ptr + chunks_size > meta_data.str + meta_data.size) break;
      
      CQL_Column* column = cql_table_find_column(table, column_name);
      // tec: a mismatched chunk count is not trusted
      U64 expected_chunk_count = column ? (column->row_count + g_cql_state->zonemap_chunk_rows - 1) / g_cql_state->zonemap_chunk_rows : 0;
      if (!column || chunk_count != expected_chunk_count)
      {
        if (column) log_debug("cql_table_load: zone map chunk count mismatch for column '%.*s' - will rebuild lazily", str8_varg(column_name));
        read_ptr += chunks_size;
        continue;
      }
      
      column->zone_map = push_array(column->arena, CQL_ZoneMapChunk, Max(chunk_count, 1));
      column->zone_map_capacity = chunk_count;
      column->zone_map_chunk_count = chunk_count;
      for (U64 c = 0; c < chunk_count; c++)
      {
        column->zone_map[c].min = *(F64*)read_ptr; read_ptr += sizeof(F64);
        column->zone_map[c].max = *(F64*)read_ptr; read_ptr += sizeof(F64);
        column->zone_map[c].has_values = *(U32*)read_ptr; read_ptr += sizeof(U32);
      }
      column->has_zone_map = 1;
      column->zonemap_checked_generation = column->write_generation;
    }
  }
  
  //- tec: optional trailing statistics section
  if (read_ptr + sizeof(U64) <= meta_data.str + meta_data.size)
  {
    U8* meta_end = meta_data.str + meta_data.size;
    U64 stats_column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
    
    for (U64 i = 0; i < stats_column_count; i++)
    {
      String8 column_name = {0};
      CQL_ColumnStats loaded_stats = {0};
      if (!cql_column_stats_read_record(&read_ptr, meta_end, &column_name, &loaded_stats))
      {
        break;
      }
      
      CQL_Column* column = cql_table_find_column(table, column_name);
      // tec: a row count mismatch means the stats predate the data on disk
      if (!column || loaded_stats.row_count != column->row_count)
      {
        continue;
      }
      
      column->stats = loaded_stats;
      column->stats.is_computed = 1;
      column->stats.computed_generation = column->write_generation;
    }
  }
  
  table->name = push_str8_copy(table->arena, str8_skip_last_slash(table_dir));
  temp_end(scratch);
  
  ProfEnd();
  
  return table;
}

internal U64
parse_csv_line(U8 *input, U64 len, String8 *fields, U64 max_fields)
{
  U64 count = 0;
  U64 i = 0;
  B32 need_field = len > 0;
  while (need_field && count < max_fields)
  {
    need_field = 0;
    B32 in_quote = 0;
    U64 start = i;
    if (i < len && input[i] == '"')
    {
      in_quote = 1;
      i++;
      start++;
    }
    
    for (; i < len; i++)
    {
      if (in_quote)
      {
        if (input[i] == '"' && (i + 1 == len || input[i + 1] == ',' || input[i + 1] == '\n'))
        {
          i++;
          break;
        }
        else if (input[i] == '"' && i + 1 < len && input[i + 1] == '"')
        {
          i++; // skip escaped quote
        }
      }
      else if (input[i] == ',')
      {
        break;
      }
    }
    
    U64 end = i;
    if (in_quote && end > start && input[end - 1] == '"')
    {
      end--;
    }
    fields[count++] = str8(input + start, end - start);
    
    if (i < len && input[i] == ',')
    {
      i++;
      need_field = 1;
    }
  }
  
  return count;
}

internal THREAD_POOL_TASK_FUNC(cql_csv_parse_task)
{
  CQL_CSV_ParseTask* task = (CQL_CSV_ParseTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  String8* fields = push_array(arena, String8, task->column_count);
  
  for (U64 row = range.min; row < range.max; row++)
  {
    String8 line = task->lines[row];
    U64 value_count = parse_csv_line(line.str, line.size, fields, task->column_count);
    
    CQL_CSV_ParsedField* row_out = task->parsed + row * task->column_count;
    for (U64 col_i = 0; col_i < value_count; col_i++)
    {
      CQL_Column* column = task->table->columns[col_i];
      String8 val = str8_skip_chop_whitespace(fields[col_i]);
      CQL_CSV_ParsedField* pf = &row_out[col_i];
      pf->present = 1;
      
      if (val.size == 0)
      {
        // tec: an empty CSV field is a NULL
        pf->is_null = 1;
      }
      else
      {
        switch (column->type)
        {
          case CQL_ColumnType_U32:
          {
            U32 v = (U32)u64_from_str8(val, 10);
            MemoryCopy(&pf->numeric_bits, &v, sizeof(v));
          } break;
          
          case CQL_ColumnType_U64:
          {
            U64 v = u64_from_str8(val, 10);
            MemoryCopy(&pf->numeric_bits, &v, sizeof(v));
          } break;
          
          case CQL_ColumnType_F32:
          {
            F32 v = (F32)f64_from_str8(val);
            MemoryCopy(&pf->numeric_bits, &v, sizeof(v));
          } break;
          
          case CQL_ColumnType_F64:
          {
            F64 v = f64_from_str8(val);
            MemoryCopy(&pf->numeric_bits, &v, sizeof(v));
          } break;
          
          case CQL_ColumnType_String8:
          default:
          {
            pf->str_value = val;
          } break;
        }
      }
    }
  }
}

internal void
cql_csv_append_parsed_row(CQL_Table* table, CQL_CSV_ParsedField* row_fields, U64 column_count)
{
  table->row_count++;
  if ((table->row_count % 1000000) == 0)
  {
    log_debug("processing row %llu", table->row_count);
  }
  
  for (U64 col_i = 0; col_i < column_count; col_i++)
  {
    CQL_CSV_ParsedField* pf = &row_fields[col_i];
    if (!pf->present) continue;
    
    CQL_Column* column = table->columns[col_i];
    if (pf->is_null)
    {
      cql_column_add_data_maybe_null(column, NULL, 1);
      continue;
    }
    
    switch (column->type)
    {
      case CQL_ColumnType_U32:
      {
        U32 v;
        MemoryCopy(&v, &pf->numeric_bits, sizeof(v));
        cql_column_add_data_maybe_null(column, &v, 0);
      } break;
      
      case CQL_ColumnType_U64:
      {
        U64 v;
        MemoryCopy(&v, &pf->numeric_bits, sizeof(v));
        cql_column_add_data_maybe_null(column, &v, 0);
      } break;
      
      case CQL_ColumnType_F32:
      {
        F32 v;
        MemoryCopy(&v, &pf->numeric_bits, sizeof(v));
        cql_column_add_data_maybe_null(column, &v, 0);
      } break;
      
      case CQL_ColumnType_F64:
      {
        F64 v; MemoryCopy(&v, &pf->numeric_bits, sizeof(v));
        cql_column_add_data_maybe_null(column, &v, 0);
      } break;
      
      case CQL_ColumnType_String8:
      default:
      {
        cql_column_add_data_maybe_null(column, &pf->str_value, 0);
      } break;
    }
  }
}

internal CQL_Table*
cql_table_import_csv_streaming(CQL_Database *db, String8 table_name, String8 path)
{
  ProfBeginFunction();
  U64 t_fn_start = os_now_microseconds();
  
  Temp scratch = temp_begin(g_cql_state->arena);
  
  CQL_Table *table = cql_table_alloc(table_name);
  table->parent_database = db;
  
  OS_Handle file = os_file_open(OS_AccessFlag_Read, path);
  if (os_handle_match(file, os_handle_zero()))
  {
    log_error("Failed to open CSV file: %.*s", str8_varg(path));
    return NULL;
  }
  
  log_info("starting import csv file %.*s", str8_varg(path));
  
  U64 file_size = os_properties_from_file(file).size;
  U64 buffer_size = MB(64);
  U8 *buffer = push_array(scratch.arena, U8, buffer_size);
  
  CQL_ColumnType *types = 0;
  String8 *column_names = 0;
  U64 column_count = 0;
  
  U64 t_typesample_start = os_now_microseconds();
  ProfBegin("column type parsing");
  {
    U64 file_pos = 0;
    U64 sample_rows = 0;
    Arena *parse_arena = arena_alloc(.reserve_size=MB(128), .commit_size=MB(4));
    
    String8 leftover = {0};
    
    while (sample_rows < 256 && file_pos < file_size)
    {
      U64 read_size = Min(buffer_size, file_size - file_pos);
      os_file_read(file, r1u64(file_pos, file_pos + read_size), buffer);
      file_pos += read_size;
      
      String8 chunk = str8(buffer, read_size);
      if (leftover.size)
      {
        chunk = push_str8_cat(parse_arena, leftover, chunk);
        leftover = (String8){0};
      }
      
      U64 at = 0;
      while (at < chunk.size && sample_rows < 256)
      {
        U64 line_start = at;
        while (at < chunk.size && chunk.str[at] != '\n') at++;
        if (at == chunk.size)
        {
          leftover = push_str8_copy(parse_arena, str8(chunk.str + line_start, at - line_start));
          break;
        }
        
        String8 line = str8(chunk.str + line_start, at - line_start);
        at++;
        if (line.size && line.str[line.size - 1] == '\r')
        {
          line.size -= 1;
        }
        
        if (sample_rows == 0)
        {
          String8List headers = str8_split_by_string_chars(parse_arena, line, str8_lit(","), StringSplitFlag_RespectQuotes);
          column_count = headers.node_count;
          column_names = push_array(scratch.arena, String8, column_count);
          
          U64 col_i = 0;
          for (String8Node *node = headers.first; node; node = node->next, col_i++)
          {
            column_names[col_i] = str8_skip_chop_whitespace(node->string);
          }
          types = push_array(scratch.arena, CQL_ColumnType, column_count);
          MemorySet(types, CQL_ColumnType_Invalid, column_count * sizeof(*types));
        }
        else
        {
          String8List values = str8_split_by_string_chars(parse_arena, line, str8_lit(","), StringSplitFlag_RespectQuotes | StringSplitFlag_KeepEmpties);
          U64 col_i = 0;
          for (String8Node *node = values.first; node && col_i < column_count; node = node->next, col_i++)
          {
            CQL_ColumnType type = cql_infer_column_type(node->string);
            types[col_i] = cql_promote_type(types[col_i], type);
          }
        }
        
        sample_rows++;
      }
    }
    
    for (U64 i = 0; i < column_count; i++)
    {
      if (types[i] == CQL_ColumnType_Invalid)
        types[i] = CQL_ColumnType_String8;
      
      String8 column_name = push_str8_copy(table->arena, column_names[i]);
      CQL_ColumnSchema schema = cql_column_schema_create(column_name, types[i]);
      cql_table_add_column(table, schema);
      //log_info("column type: %.*s", str8_varg(string_from_cql_column_type(types[i])));
    }
  }
  ProfEnd();
  U64 t_typesample = os_now_microseconds() - t_typesample_start;
  U64 t_read = 0, t_scan = 0, t_parse = 0, t_append = 0;
  
  {
    Arena *row_arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(32));
    U64 file_pos = 0;
    U8 *buffer = push_array(scratch.arena, U8, buffer_size);
    
    String8 leftover = {0};
    B32 skipped_header = 0;
    
    while (file_pos < file_size)
    {
      U64 p0 = os_now_microseconds();
      U64 read_size = Min(buffer_size, file_size - file_pos);
      os_file_read(file, r1u64(file_pos, file_pos + read_size), buffer);
      file_pos += read_size;
      t_read += os_now_microseconds() - p0;
      
      String8 chunk = str8(buffer, read_size);
      if (leftover.size)
      {
        chunk = push_str8_cat(row_arena, leftover, chunk);
        leftover = (String8){0};
      }
      
      // tec: pass 1. find all lines
      U64 p1 = os_now_microseconds();
      String8List chunk_lines = {0};
      U64 at = 0;
      while (at < chunk.size)
      {
        U64 line_start = at;
        while (at < chunk.size && chunk.str[at] != '\n') at++;
        
        B32 is_last_in_chunk = (at == chunk.size);
        
        String8 line = str8(chunk.str + line_start, at - line_start);
        if (!is_last_in_chunk) at++; // skip newline if not last
        
        if (is_last_in_chunk)
        {
          leftover = push_str8_copy(row_arena, line);
          break;
        }
        
        if (!skipped_header)
        {
          skipped_header = 1;
          continue;
        }
        
        str8_list_push(row_arena, &chunk_lines, line);
      }
      t_scan += os_now_microseconds() - p1;
      
      U64 chunk_row_count = chunk_lines.node_count;
      if (chunk_row_count > 0)
      {
        String8* lines_arr = push_array(row_arena, String8, chunk_row_count);
        U64 line_i = 0;
        for (String8Node *node = chunk_lines.first; node; node = node->next, line_i++)
        {
          lines_arr[line_i] = node->string;
        }
        
        // tec: pass 2. parse each line
        U64 p2 = os_now_microseconds();
        CQL_CSV_ParsedField* parsed = push_array(row_arena, CQL_CSV_ParsedField, chunk_row_count * column_count);
        
        CQL_CSV_ParseTask task = {0};
        task.lines = lines_arr;
        task.parsed = parsed;
        task.table = table;
        task.column_count = column_count;
        
        TP_Context* pool = app_thread_pool();
        U64 task_count = Max((U64)1, Min((U64)pool->worker_count, chunk_row_count));
        task.ranges = tp_divide_work(row_arena, chunk_row_count, (U32)task_count);
        
        TP_Arena* pool_arena = app_thread_pool_arena();
        TP_Temp temp = tp_temp_begin(pool_arena);
        tp_for_parallel(pool, pool_arena, task_count, cql_csv_parse_task, &task);
        tp_temp_end(temp);
        t_parse += os_now_microseconds() - p2;
        
        // tec: pass 3. append each line
        U64 p3 = os_now_microseconds();
        for (U64 row = 0; row < chunk_row_count; row++)
        {
          cql_csv_append_parsed_row(table, parsed + row * column_count, column_count);
        }
        t_append += os_now_microseconds() - p3;
      }
      
      arena_clear(row_arena);
    }
    
    if (leftover.size > 0)
    {
      if (!skipped_header)
      {
        skipped_header = 1;
      }
      else
      {
        table->row_count++;
        String8List values = str8_split_by_string_chars(row_arena, leftover, str8_lit(","), StringSplitFlag_RespectQuotes | StringSplitFlag_KeepEmpties);
        U64 col_i = 0;
        for (String8Node *node = values.first; node && col_i < column_count; node = node->next, col_i++)
        {
          CQL_Column *column = table->columns[col_i];
          String8 val = str8_skip_chop_whitespace(node->string);
          
          if (val.size == 0)
          {
            // tec: an empty CSV field is a NULL
            cql_column_add_data_maybe_null(column, NULL, 1);
          }
          else
          {
            switch (column->type)
            {
              case CQL_ColumnType_U32:
              {
                U32 v = (U32)u64_from_str8(val, 10);
                cql_column_add_data_maybe_null(column, &v, 0);
              } break;
              
              case CQL_ColumnType_U64:
              {
                U64 v = u64_from_str8(val, 10);
                cql_column_add_data_maybe_null(column, &v, 0);
              } break;
              
              case CQL_ColumnType_F32:
              {
                F32 v = (F32)f64_from_str8(val);
                cql_column_add_data_maybe_null(column, &v, 0);
              } break;
              
              case CQL_ColumnType_F64:
              {
                F64 v = f64_from_str8(val);
                cql_column_add_data_maybe_null(column, &v, 0);
              } break;
              
              case CQL_ColumnType_String8:
              default:
              {
                cql_column_add_data_maybe_null(column, &val, 0);
              } break;
            }
          }
        }
      }
      arena_clear(row_arena);
    }
  }
  os_file_close(file);
  temp_end(scratch);
  log_info("ending import csv file %.*s", str8_varg(path));
  log_debug("cql_table_import_csv_streaming: %llu rows, phases (us): type_sample=%llu read=%llu scan=%llu parse=%llu append=%llu total=%llu",
            table->row_count, t_typesample, t_read, t_scan, t_parse, t_append, os_now_microseconds() - t_fn_start);
  ProfEnd();
  return table;
}

internal CQL_Column*
cql_table_find_column(CQL_Table* table, String8 column_name)
{
  for (U64 i = 0; i < table->column_count; i++)
  {
    CQL_Column* column = table->columns[i];
    if (str8_match(column->name, column_name, 0))
    {
      return column;
    }
  }
  log_error("failed to find column '%.*s' in table '%.*s", column_name.size, column_name.str,
            table->name.size, table->name.str);
  return NULL;
}

//~ tec: indexes

internal CQL_Index*
cql_table_find_index(CQL_Table* table, String8 index_name)
{
  for (U64 i = 0; i < table->index_count; i++)
  {
    if (str8_match(table->indexes[i]->name, index_name, StringMatchFlag_CaseInsensitive))
    {
      return table->indexes[i];
    }
  }
  return NULL;
}

internal CQL_Index*
cql_table_find_index_on_column(CQL_Table* table, CQL_Column* column)
{
  for (U64 i = 0; i < table->index_count; i++)
  {
    if (table->indexes[i]->column == column)
    {
      return table->indexes[i];
    }
  }
  return NULL;
}

internal CQL_Index*
cql_table_register_index(CQL_Table* table, String8 index_name, CQL_Column* column)
{
  if (table->index_count == 0)
  {
    table->indexes = push_array(table->arena, CQL_Index*, 2);
    table->index_capacity = 2;
  }
  else if (table->index_count >= table->index_capacity)
  {
    U64 new_capacity = table->index_capacity * 2;
    CQL_Index** new_indexes = push_array(table->arena, CQL_Index*, new_capacity);
    MemoryCopy(new_indexes, table->indexes, sizeof(CQL_Index*) * table->index_count);
    table->indexes = new_indexes;
    table->index_capacity = new_capacity;
  }
  
  CQL_Index* index = push_array(table->arena, CQL_Index, 1);
  index->name = push_str8_copy(table->arena, index_name);
  index->column_name = push_str8_copy(table->arena, column->name);
  index->column = column;
  
  table->indexes[table->index_count++] = index;
  return index;
}

internal CQL_Index*
cql_table_create_index(CQL_Table* table, String8 index_name, CQL_Column* column)
{
  CQL_Index* index = cql_table_register_index(table, index_name, column);
  cql_index_build_order(index);
  return index;
}

internal void
cql_table_drop_index(CQL_Table* table, String8 index_name)
{
  U64 slot = 0;
  for (; slot < table->index_count; slot++)
  {
    if (str8_match(table->indexes[slot]->name, index_name, StringMatchFlag_CaseInsensitive)) break;
  }
  
  if (slot >= table->index_count)
  {
    log_error("cql_table_drop_index: no index '%.*s' on table '%.*s'", str8_varg(index_name), str8_varg(table->name));
    return;
  }
  
  if (table->parent_database)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 idx_path = cql_generate_disk_path_for_index(scratch.arena, table, table->indexes[slot]->name);
    os_delete_file_at_path(idx_path);
    scratch_end(scratch);
  }
  
  for (U64 i = slot; i + 1 < table->index_count; i++)
  {
    table->indexes[i] = table->indexes[i + 1];
  }
  table->index_count--;
}

//~ tec: index order maintenance

internal S32
cql_str8_compare(String8 a, String8 b)
{
  U64 min_size = Min(a.size, b.size);
  S32 cmp = min_size ? (S32)MemoryCompare(a.str, b.str, min_size) : 0;
  if (cmp != 0) return cmp;
  if (a.size < b.size) return -1;
  if (a.size > b.size) return 1;
  return 0;
}

internal F64
cql_index_numeric_value(CQL_Column* column, U64 row_index)
{
  void* data = cql_column_get_data(column, row_index);
  switch (column->type)
  {
    case CQL_ColumnType_U32: return (F64)(*(U32*)data);
    case CQL_ColumnType_U64: return (F64)(*(U64*)data);
    case CQL_ColumnType_F32: return (F64)(*(F32*)data);
    case CQL_ColumnType_F64: return *(F64*)data;
    case CQL_ColumnType_Bool: return (F64)(*(U8*)data);
    case CQL_ColumnType_I32: return (F64)(*(S32*)data);
    case CQL_ColumnType_I64: return (F64)(*(S64*)data);
    case CQL_ColumnType_Date: return (F64)(*(S32*)data);
    case CQL_ColumnType_Timestamp: return (F64)(*(S64*)data);
    case CQL_ColumnType_Decimal: return (F64)(*(S64*)data);
    case CQL_ColumnType_Enum: return (F64)(*(U32*)data);
    default: return 0.0;
  }
}

internal int
cql_index_build_compare(const void* a, const void* b)
{
  U64 ra = *(const U64*)a;
  U64 rb = *(const U64*)b;
  CQL_IndexBuildCtx* ctx = g_cql_index_build_ctx;
  
  if (ctx->is_string)
  {
    CQL_StringDataChunk* chunk = &ctx->string_keys;
    String8 sa = str8((U8*)chunk->data + chunk->offsets[ra], chunk->offsets[ra + 1] - chunk->offsets[ra]);
    String8 sb = str8((U8*)chunk->data + chunk->offsets[rb], chunk->offsets[rb + 1] - chunk->offsets[rb]);
    return cql_str8_compare(sa, sb);
  }
  
  F64 va = ctx->numeric_keys[ra];
  F64 vb = ctx->numeric_keys[rb];
  return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

internal void
cql_index_build_order(CQL_Index* index)
{
  CQL_Column* column = index->column;
  CQL_Table* table = column->parent_table;
  U64 row_count = table->row_count;
  
  index->order = row_count ? push_array(table->arena, U64, row_count) : NULL;
  index->order_count = row_count;
  index->order_capacity = row_count;
  
  if (row_count == 0) return;
  
  Temp scratch = scratch_begin(0, 0);
  
  CQL_IndexBuildCtx ctx = {0};
  ctx.is_string = (column->type == CQL_ColumnType_String8);
  
  if (ctx.is_string)
  {
    ctx.string_keys = cql_column_get_string_chunk(scratch.arena, column, r1u64(0, row_count));
  }
  else
  {
    U64 range_size = 0;
    void* base_ptr = cql_column_get_data_range(scratch.arena, column, r1u64(0, row_count), &range_size);
    F64* numeric_keys = push_array(scratch.arena, F64, row_count);
    for (U64 i = 0; i < row_count; i++)
    {
      void* data = (U8*)base_ptr + i * column->size;
      switch (column->type)
      {
        case CQL_ColumnType_U32: numeric_keys[i] = (F64)(*(U32*)data); break;
        case CQL_ColumnType_U64: numeric_keys[i] = (F64)(*(U64*)data); break;
        case CQL_ColumnType_F32: numeric_keys[i] = (F64)(*(F32*)data); break;
        case CQL_ColumnType_F64: numeric_keys[i] = *(F64*)data; break;
        case CQL_ColumnType_Bool: numeric_keys[i] = (F64)(*(U8*)data); break;
        case CQL_ColumnType_I32: numeric_keys[i] = (F64)(*(S32*)data); break;
        case CQL_ColumnType_I64: numeric_keys[i] = (F64)(*(S64*)data); break;
        case CQL_ColumnType_Date: numeric_keys[i] = (F64)(*(S32*)data); break;
        case CQL_ColumnType_Timestamp: numeric_keys[i] = (F64)(*(S64*)data); break;
        case CQL_ColumnType_Decimal: numeric_keys[i] = (F64)(*(S64*)data); break;
        case CQL_ColumnType_Enum: numeric_keys[i] = (F64)(*(U32*)data); break;
        default: numeric_keys[i] = 0.0; break;
      }
    }
    ctx.numeric_keys = numeric_keys;
  }
  
  for (U64 i = 0; i < row_count; i++) index->order[i] = i;
  
  CQL_IndexBuildCtx* prev_ctx = g_cql_index_build_ctx;
  g_cql_index_build_ctx = &ctx;
  quick_sort(index->order, row_count, sizeof(U64), cql_index_build_compare);
  g_cql_index_build_ctx = prev_ctx;
  
  scratch_end(scratch);
}

internal void
cql_index_ensure_order_capacity(CQL_Index* index, CQL_Table* table)
{
  if (index->order_count < index->order_capacity) return;
  
  U64 new_capacity = (index->order_capacity > 0) ? index->order_capacity * 2 : g_cql_state->column_expand_count;
  U64* new_order = push_array(table->arena, U64, new_capacity);
  if (index->order)
  {
    MemoryCopy(new_order, index->order, index->order_count * sizeof(U64));
  }
  index->order = new_order;
  index->order_capacity = new_capacity;
}

internal void
cql_index_insert_row(CQL_Index* index, U64 new_row_index)
{
  CQL_Column* column = index->column;
  CQL_Table* table = column->parent_table;
  
  cql_index_ensure_order_capacity(index, table);
  
  Temp scratch = scratch_begin(0, 0);
  
  U64 lo = 0, hi = index->order_count;
  if (column->type == CQL_ColumnType_String8)
  {
    String8 new_key = cql_column_get_string(scratch.arena, column, new_row_index);
    while (lo < hi)
    {
      U64 mid = lo + (hi - lo) / 2;
      String8 mid_key = cql_column_get_string(scratch.arena, column, index->order[mid]);
      if (cql_str8_compare(mid_key, new_key) < 0) lo = mid + 1;
      else hi = mid;
    }
  }
  else
  {
    F64 new_key = cql_index_numeric_value(column, new_row_index);
    while (lo < hi)
    {
      U64 mid = lo + (hi - lo) / 2;
      F64 mid_key = cql_index_numeric_value(column, index->order[mid]);
      if (mid_key < new_key) lo = mid + 1;
      else hi = mid;
    }
  }
  
  MemoryCopy(index->order + lo + 1, index->order + lo, (index->order_count - lo) * sizeof(U64));
  index->order[lo] = new_row_index;
  index->order_count++;
  
  scratch_end(scratch);
}

internal void
cql_index_remove_row(CQL_Index* index, U64 removed_row_index)
{
  U64 slot = index->order_count;
  for (U64 i = 0; i < index->order_count; i++)
  {
    if (index->order[i] == removed_row_index) { slot = i; break; }
  }
  
  if (slot < index->order_count)
  {
    MemoryCopy(index->order + slot, index->order + slot + 1, (index->order_count - slot - 1) * sizeof(U64));
    index->order_count--;
  }
  
  for (U64 i = 0; i < index->order_count; i++)
  {
    if (index->order[i] > removed_row_index) index->order[i]--;
  }
}

internal String8
cql_generate_disk_path_for_index(Arena* arena, CQL_Table* table, String8 index_name)
{
  CQL_Database* database = table->parent_database;
  
  {
    Temp scratch = scratch_begin(0, 0);
    
    if (!os_file_path_exists(str8_lit("cql_data/")))
    {
      os_make_directory(str8_lit("cql_data/"));
    }
    
    String8 database_path = push_str8f(arena, "cql_data/%.*s/", str8_varg(database->name));
    if (!os_file_path_exists(database_path))
    {
      os_make_directory(database_path);
    }
    
    String8 table_path = push_str8f(arena, "cql_data/%.*s/%.*s/", str8_varg(database->name), str8_varg(table->name));
    if (!os_file_path_exists(table_path))
    {
      os_make_directory(table_path);
    }
    
    scratch_end(scratch);
  }
  
  return push_str8f(arena, "cql_data/%.*s/%.*s/%.*s.idx", str8_varg(database->name), str8_varg(table->name), str8_varg(index_name));
}

internal void
cql_index_save(CQL_Index* index, String8 table_dir)
{
  Temp scratch = scratch_begin(0, 0);
  
  String8 idx_path = push_str8f(scratch.arena, "%.*s/%.*s.idx", str8_varg(table_dir), str8_varg(index->name));
  OS_Handle file = os_file_open(OS_AccessFlag_Write, idx_path);
  if (os_handle_match(os_handle_zero(), file))
  {
    log_error("Failed to open index file for saving: %.*s", str8_varg(idx_path));
    scratch_end(scratch);
    return;
  }
  
  U64 data_size = index->order_count * sizeof(U64);
  U64 total_size = sizeof(U64) + data_size;
  U8* buffer = push_array(scratch.arena, U8, total_size);
  *(U64*)buffer = index->order_count;
  if (data_size)
  {
    MemoryCopy(buffer + sizeof(U64), index->order, data_size);
  }
  
  os_file_write(file, r1u64(0, total_size), buffer);
  os_file_close(file);
  scratch_end(scratch);
}

internal B32
cql_index_load(CQL_Index* index, String8 table_dir)
{
  Temp scratch = scratch_begin(0, 0);
  
  String8 idx_path = push_str8f(scratch.arena, "%.*s/%.*s.idx", str8_varg(table_dir), str8_varg(index->name));
  String8 idx_data = os_data_from_file_path(scratch.arena, idx_path);
  
  B32 ok = 0;
  if (idx_data.size >= sizeof(U64))
  {
    U64 count = *(U64*)idx_data.str;
    U64 expected_size = sizeof(U64) + count * sizeof(U64);
    CQL_Table* table = index->column->parent_table;
    if (idx_data.size == expected_size && count == table->row_count)
    {
      index->order = count ? push_array(table->arena, U64, count) : NULL;
      if (count) MemoryCopy(index->order, idx_data.str + sizeof(U64), count * sizeof(U64));
      index->order_count = count;
      index->order_capacity = count;
      ok = 1;
    }
  }
  
  scratch_end(scratch);
  return ok;
}

//~ tec: constraints

internal B32
cql_column_has_any_constraint(CQL_Column* column)
{
  return column->not_null || column->is_unique || column->is_primary_key ||
    column->has_foreign_key || column->has_check;
}

//~ tec: column
internal B32
cql_column_is_ephemeral(CQL_Column* column)
{
  return column->parent_table && column->parent_table->is_ephemeral;
}

global U64 g_cql_column_uid_counter = 0;

internal CQL_Column*
cql_column_alloc(String8 name, CQL_ColumnType type, U64 size)
{
  Arena* arena = arena_alloc(.reserve_size=settings_u64(str8_lit("CQL_COLUMN_ARENA_RESERVE_SIZE"), CQL_COLUMN_ARENA_RESERVE_SIZE),
                             .commit_size=settings_u64(str8_lit("CQL_COLUMN_ARENA_COMMIT_SIZE"), CQL_COLUMN_ARENA_COMMIT_SIZE));
  CQL_Column* column = push_array(arena, CQL_Column, 1);
  
  column->name = push_str8_copy(arena, name);
  column->type = type;
  column->size = size;
  column->arena = arena;
  column->write_generation = 1;
  g_cql_column_uid_counter += 1;
  column->uid = g_cql_column_uid_counter;
  
  return column;
}

internal void
cql_column_release(CQL_Column* column)
{
  if (column->dict)
  {
    arena_release(column->dict->arena);
  }
  if (column->agg_str_arena)
  {
    arena_release(column->agg_str_arena);
  }
  arena_release(column->arena);
}

internal void
cql_column_open(CQL_Column* column)
{
  if (!column->is_disk_backed)
  {
    cql_column_convert_to_disk_backed(column);
  }
}

internal void
cql_column_close(CQL_Column* column)
{
  cql_column_flush_pending(column);
  if (column->is_disk_backed)
  {
    os_file_close(column->file);
    if (!os_handle_match(os_handle_zero(), column->file_map_backing_file))
    {
      os_file_close(column->file_map_backing_file);
    }
  }
}

internal void
cql_column_drop_mapping(CQL_Column* column)
{
  if (column->mapped_ptr)
  {
    os_file_map_view_close(column->file_map, column->mapped_ptr, column->current_mapped_range);
    column->mapped_ptr = 0;
  }
  if (!os_handle_match(os_handle_zero(), column->file_map))
  {
    os_file_map_close(column->file_map);
    column->file_map = os_handle_zero();
  }
  if (!os_handle_match(os_handle_zero(), column->file_map_backing_file))
  {
    os_file_close(column->file_map_backing_file);
    column->file_map_backing_file = os_handle_zero();
  }
}

// tec: writes the collected appends to the file. row_count already counts the pending rows
internal void
cql_column_flush_pending(CQL_Column* column)
{
  if (column->pending_rows == 0)
  {
    return;
  }

  OS_Handle file = column->file;
  if (os_handle_match(os_handle_zero(), file))
  {
    // tec: cached in column->file for the column's whole lifetime
    file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_Write | OS_AccessFlag_Append | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column->disk_path);
    column->file = file;
  }

  U64 flushed_rows = column->row_count - column->pending_rows;

  if (column->type == CQL_ColumnType_String8)
  {
    U64 flushed_var = column->variable_capacity - column->pending_data_size;
    U64 var_reserved = 0;
    os_file_read(file, r1u64(0, sizeof(U64)), &var_reserved);

    U64 offset_array_offset = sizeof(U64) + var_reserved;
    U64 total_offsets_size = (flushed_rows + 1) * sizeof(U64);

    B32 needs_growth = (column->variable_capacity > var_reserved);
    if (needs_growth)
    {
      ProfBegin("cql_column_flush_pending growth");
      U64 new_reserved = var_reserved * 2;
      if (new_reserved < column->variable_capacity)
      {
        new_reserved = AlignUp(column->variable_capacity + g_cql_state->column_variable_capacity_alloc_size, 8);
      }

      U64 old_offset_pos = sizeof(U64) + var_reserved;
      U64 new_offset_pos = sizeof(U64) + new_reserved;

      Temp scratch = temp_begin(g_cql_state->arena);
      void *buffer = push_array(scratch.arena, U8, total_offsets_size);

      os_file_read(file, r1u64(old_offset_pos, old_offset_pos + total_offsets_size), buffer);
      os_file_write(file, r1u64(new_offset_pos, new_offset_pos + total_offsets_size), buffer);

      os_file_write(file, r1u64(0, sizeof(U64)), &new_reserved);
      var_reserved = new_reserved;
      offset_array_offset = sizeof(U64) + var_reserved;

      U64 new_size = offset_array_offset + total_offsets_size;
      os_file_resize(file, new_size);

      // tec: clear the old offsets location
      U64 old_offset_array_end = old_offset_pos + total_offsets_size;
      if (old_offset_array_end > new_offset_pos)
      {
        old_offset_array_end = new_offset_pos;
      }
      if (old_offset_array_end > old_offset_pos)
      {
        U64 old_offset_array_size = old_offset_array_end - old_offset_pos;
        void *zero_buf = push_array(scratch.arena, U8, old_offset_array_size);
        MemoryZero(zero_buf, old_offset_array_size);
        os_file_write(file, r1u64(old_offset_pos, old_offset_array_end), zero_buf);
      }

      temp_end(scratch);
      ProfEnd();
    }

    if (column->pending_data_size > 0)
    {
      os_file_write(file, r1u64(sizeof(U64) + flushed_var, sizeof(U64) + flushed_var + column->pending_data_size), column->pending_data);
    }
    os_file_write(file,
                  r1u64(offset_array_offset + flushed_rows * sizeof(U64),
                        offset_array_offset + (flushed_rows + column->pending_rows) * sizeof(U64)),
                  column->pending_offsets);
  }
  else
  {
    U64 offset = flushed_rows * column->size;
    os_file_write(file, r1u64(offset, offset + column->pending_data_size), column->pending_data);
  }

  column->pending_rows = 0;
  column->pending_data_size = 0;

  // tec: a cached mapping bakes in the file size, the flush just changed it
  cql_column_drop_mapping(column);
}

internal void
cql_column_add_data_disk_backed(CQL_Column* column, void* data)
{
  if (!column->pending_data)
  {
    column->pending_data_capacity = CQL_PENDING_APPEND_BYTES;
    column->pending_data = push_array(column->arena, U8, column->pending_data_capacity);
    if (column->type == CQL_ColumnType_String8)
    {
      column->pending_offsets = push_array(column->arena, U64, CQL_PENDING_APPEND_ROWS);
    }
  }

  if (column->type == CQL_ColumnType_String8)
  {
    String8* str = (String8*)data;

    if (column->pending_rows == CQL_PENDING_APPEND_ROWS || column->pending_data_size + str->size > column->pending_data_capacity)
    {
      cql_column_flush_pending(column);
    }
    if (str->size > column->pending_data_capacity)
    {
      column->pending_data_capacity = AlignUp(str->size, KB(64));
      column->pending_data = push_array(column->arena, U8, column->pending_data_capacity);
    }

    MemoryCopy(column->pending_data + column->pending_data_size, str->str, str->size);
    column->pending_data_size += str->size;
    column->variable_capacity += str->size;
    column->pending_offsets[column->pending_rows] = column->variable_capacity;
    column->pending_rows += 1;
  }
  else
  {
    if (column->pending_data_size + column->size > column->pending_data_capacity)
    {
      cql_column_flush_pending(column);
    }

    U8* dest = column->pending_data + column->pending_data_size;
    if (data)
    {
      MemoryCopy(dest, data, column->size);
    }
    else
    {
      MemoryZero(dest, column->size);
    }
    column->pending_data_size += column->size;
    column->pending_rows += 1;
  }
}

internal void
cql_column_add_data(CQL_Column* column, void* data)
{
  if (column->type == CQL_ColumnType_String8)
  {
    String8* str = (String8*)data;
    String8 empty_str = str8_lit("");
    if (str == NULL)
    {
      str = &empty_str;
    }
    
    if (column->is_disk_backed)
    {
      cql_column_add_data_disk_backed(column, str);
    }
    else
    {
      //- tec: grow offsets array if needed
      if (column->row_count == column->capacity)
      {
        U64 new_capacity = (column->capacity > 0) ? column->capacity * 2 : g_cql_state->column_expand_count;
        U64* new_offsets = push_array(column->arena, U64, new_capacity);
        if (column->offsets)
        {
          MemoryCopy(new_offsets, column->offsets, column->row_count * sizeof(U64));
        }
        column->offsets = new_offsets;
        column->capacity = new_capacity;
      }
      
      //- tec: grow variable data if needed
      U64 previous_offset = (column->row_count > 0) ? column->offsets[column->row_count - 1] : 0;
      U64 required_size = previous_offset + str->size;
      if (required_size > column->variable_capacity)
      {
        U64 new_variable_capacity = (column->variable_capacity > 0) ? column->variable_capacity * 2 : g_cql_state->column_variable_capacity_alloc_size;
        while (new_variable_capacity < required_size)
        {
          new_variable_capacity *= 2;
        }
        
        if (new_variable_capacity > g_cql_state->disk_backed_threshold_size && !cql_column_is_ephemeral(column))
        {
          if (!column->is_disk_backed)
          {
            cql_column_convert_to_disk_backed(column);
            cql_column_add_data(column, data);
            //cql_column_add_data_disk_backed(column, data);
            return;
          }
        }
        
        U8* new_data = push_array(column->arena, U8, new_variable_capacity);
        if (column->data)
        {
          MemoryCopy(new_data, column->data, column->variable_capacity);
        }
        column->data = new_data;
        column->variable_capacity = new_variable_capacity;
      }
      
      if (column->row_count >= column->capacity)
      {
        log_error("offset array out of bounds: row_count=%llu capacity=%llu", column->row_count, column->capacity);
      }
      
      U64 current_offset = (column->row_count > 0) ? column->offsets[column->row_count - 1] : 0;
      MemoryCopy(column->data + current_offset, str->str, str->size);
      column->offsets[column->row_count] = current_offset + str->size;
    }
  }
  else
  {
    if (column->is_disk_backed)
    {
      cql_column_add_data_disk_backed(column, data);
    }
    else
    {
      // tec: grow if needed
      if (column->row_count == column->capacity)
      {
        // tec: check that an absurdly large buffer is not being allocated
        if (column->capacity > (max_U64 / 2))
        {
          log_error("column capacity too large, can not allocate");
          return;
        }
        
        // tec: cap capacity
        U64 new_capacity = (column->capacity > 0) ? column->capacity * 2 : g_cql_state->column_expand_count;
        if (new_capacity > column->capacity + g_cql_state->column_max_grow_by_size)
        {
          new_capacity = column->capacity + g_cql_state->column_max_grow_by_size;
        }
        //log_debug("growing column: old_capacity=%llu, new_capacity=%llu, size=%llu", column->capacity, new_capacity, column->size);
        
        U8* new_data = arena_push(column->arena, new_capacity * column->size, 8);
        if (new_data == 0)
        {
          log_error("failed to allocate memory in arena");
          return;
        }
        
        if (column->capacity > 0 && column->data)
        {
          MemoryCopy(new_data, column->data, column->capacity * column->size);
        }
        column->data = new_data;
        column->capacity = new_capacity;
      }
      
      // tec: add data. which may or may not be null
      if (data)
      {
        MemoryCopy(column->data + column->row_count * column->size, data, column->size);
      }
      else
      {
        MemoryZero(column->data + column->row_count * column->size, column->size);
      }
      
      if ((column->row_count + 1) * column->size > g_cql_state->disk_backed_threshold_size && !cql_column_is_ephemeral(column))
      {
        cql_column_convert_to_disk_backed(column);
      }
    }
  }
  column->row_count++;
  column->write_generation++;
  
  // tec: drop the cached mapping
  // file mapping bakes in the file size at open time, so a stale mapping cant see bytes this write just appended
  if (column->is_disk_backed)
  {
    cql_column_drop_mapping(column);
  }
}

internal B32
cql_column_fill_from_f64(CQL_Column* column, F64* values, U8* is_null, U64 count)
{
  if (column->type == CQL_ColumnType_String8 || column->type == CQL_ColumnType_Invalid || 
      column->row_count != 0 || column->is_disk_backed)
  {
    return 0;
  }
  if (count == 0)
  {
    return 1;
  }
  
  U8* data = arena_push(column->arena, count * column->size, 8);
  if (data == 0)
  {
    return 0;
  }
  
  switch (column->type)
  {
    case CQL_ColumnType_U32:
    case CQL_ColumnType_Enum:
    { 
      U32* out = (U32*)data; 
      for (U64 i = 0; i < count; i++) 
      {
        out[i] = (U32)values[i];
      }
    } break;
    case CQL_ColumnType_U64:
    { 
      U64* out = (U64*)data; 
      for (U64 i = 0; i < count; i++) 
      {
        out[i] = (U64)values[i];
      }
    } break;
    case CQL_ColumnType_F32: 
    { 
      F32* out = (F32*)data; 
      for (U64 i = 0; i < count; i++) 
      {
        out[i] = (F32)values[i];
      }
    } break;
    case CQL_ColumnType_F64: 
    { 
      MemoryCopy(data, values, count * sizeof(F64)); 
    } break;
    case CQL_ColumnType_Bool: 
    { 
      U8* out = data; 
      for (U64 i = 0; i < count; i++) 
      {
        out[i] = (U8)values[i]; 
      }
    } break;
    case CQL_ColumnType_I32:
    case CQL_ColumnType_Date: 
    { 
      S32* out = (S32*)data; 
      for (U64 i = 0; i < count; i++) 
      {
        out[i] = (S32)values[i]; 
      }
    } break;
    case CQL_ColumnType_I64:
    case CQL_ColumnType_Timestamp:
    case CQL_ColumnType_Decimal: 
    { 
      S64* out = (S64*)data; 
      for (U64 i = 0; i < count; i++) 
      {
        out[i] = (S64)values[i];
      }
    } break;
    default: return 0;
  }
  
  column->data = data;
  column->capacity = count;
  column->row_count = count;
  column->write_generation++;
  
  if (is_null)
  {
    B32 any_null = 0;
    for (U64 i = 0; i < count && !any_null; i++)
    {
      any_null = is_null[i] != 0;
    }
    if (any_null)
    {
      cql_column_ensure_null_flags_capacity(column, count);
      MemoryCopy(column->null_flags, is_null, count);
    }
  }
  return 1;
}

internal void*
cql_column_alloc_fixed_rows(CQL_Column* column, U64 count)
{
  if (column->type == CQL_ColumnType_String8 || column->type == CQL_ColumnType_Invalid ||
      column->row_count != 0 || column->is_disk_backed || count == 0)
  {
    return 0;
  }

  U8* data = arena_push(column->arena, count * column->size, 8);
  if (data == 0)
  {
    return 0;
  }

  column->data = data;
  column->capacity = count;
  column->row_count = count;
  column->write_generation++;
  return data;
}

internal B32
cql_column_fill_from_strings(CQL_Column* column, String8* values, U8* is_null, U64 count)
{
  if (column->type != CQL_ColumnType_String8 || column->row_count != 0 || 
      column->is_disk_backed || column->offsets != 0)
  {
    return 0;
  }
  if (count == 0)
  {
    return 1;
  }
  
  U64 total_size = 0;
  for (U64 i = 0; i < count; i++)
  {
    total_size += values[i].size;
  }
  
  U64* offsets = push_array_no_zero(column->arena, U64, count);
  U8* data = push_array_no_zero(column->arena, U8, Max(total_size, 8));
  if (offsets == 0 || data == 0)
  {
    return 0;
  }
  
  U64 cursor = 0;
  for (U64 i = 0; i < count; i++)
  {
    U64 size = values[i].size;
    if (size > 0)
    {
      MemoryCopy(data + cursor, values[i].str, size);
    }
    cursor += size;
    offsets[i] = cursor;
  }
  
  column->data = data;
  column->offsets = offsets;
  column->capacity = count;
  column->variable_capacity = Max(total_size, 8);
  column->row_count = count;
  column->write_generation++;
  
  if (is_null)
  {
    B32 any_null = 0;
    for (U64 i = 0; i < count && !any_null; i++)
    {
      any_null = is_null[i] != 0;
    }
    if (any_null)
    {
      cql_column_ensure_null_flags_capacity(column, count);
      MemoryCopy(column->null_flags, is_null, count);
    }
  }
  return 1;
}

internal void
cql_column_ensure_null_flags_capacity(CQL_Column* column, U64 needed_count)
{
  if (needed_count <= column->null_flags_capacity)
  {
    return;
  }
  
  U64 new_capacity = (column->null_flags_capacity > 0) ? column->null_flags_capacity * 2 : g_cql_state->column_expand_count;
  while (new_capacity < needed_count) new_capacity *= 2;
  
  U8* new_flags = push_array(column->arena, U8, new_capacity); // tec: push_array auto-zeroes
  if (column->null_flags)
  {
    MemoryCopy(new_flags, column->null_flags, column->null_flags_capacity);
  }
  column->null_flags = new_flags;
  column->null_flags_capacity = new_capacity;
}

internal F64
cql_numeric_value_as_f64(CQL_ColumnType type, void* data)
{
  switch (type)
  {
    case CQL_ColumnType_U32: return (F64)(*(U32*)data);
    case CQL_ColumnType_U64: return (F64)(*(U64*)data);
    case CQL_ColumnType_F32: return (F64)(*(F32*)data);
    case CQL_ColumnType_F64: return *(F64*)data;
    case CQL_ColumnType_Bool: return (F64)(*(U8*)data);
    case CQL_ColumnType_I32: return (F64)(*(S32*)data);
    case CQL_ColumnType_I64: return (F64)(*(S64*)data);
    case CQL_ColumnType_Date: return (F64)(*(S32*)data);
    case CQL_ColumnType_Timestamp: return (F64)(*(S64*)data);
    case CQL_ColumnType_Decimal: return (F64)(*(S64*)data);
    case CQL_ColumnType_Enum: return (F64)(*(U32*)data);
    default: return 0.0;
  }
}

internal B32
cql_column_type_is_zone_map_eligible(CQL_ColumnType type)
{
  return type != CQL_ColumnType_Invalid && type != CQL_ColumnType_String8;
}

internal void
cql_column_zone_map_grow(CQL_Column* column, U64 needed_count)
{
  if (needed_count <= column->zone_map_capacity) return;
  
  U64 new_capacity = (column->zone_map_capacity > 0) ? column->zone_map_capacity * 2 : g_cql_state->column_expand_count;
  while (new_capacity < needed_count) new_capacity *= 2;
  
  CQL_ZoneMapChunk* new_zone_map = push_array(column->arena, CQL_ZoneMapChunk, new_capacity); // tec: push_array auto-zeroes -> has_values=0
  if (column->zone_map)
  {
    MemoryCopy(new_zone_map, column->zone_map, column->zone_map_chunk_count * sizeof(CQL_ZoneMapChunk));
  }
  column->zone_map = new_zone_map;
  column->zone_map_capacity = new_capacity;
}

internal void
cql_column_zone_map_note_append(CQL_Column* column, void* data)
{
  if (!cql_column_type_is_zone_map_eligible(column->type)) return;
  
  U64 new_row_index = column->row_count - 1; // tec: caller already incremented row_count
  U64 chunk_index = new_row_index >> g_cql_state->zonemap_chunk_rows_log2;
  
  if (chunk_index >= column->zone_map_chunk_count)
  {
    cql_column_zone_map_grow(column, chunk_index + 1);
    column->zone_map_chunk_count = chunk_index + 1;
  }
  
  F64 value = data ? cql_numeric_value_as_f64(column->type, data) : 0.0;
  CQL_ZoneMapChunk* chunk = &column->zone_map[chunk_index];
  if (!chunk->has_values)
  {
    chunk->min = value;
    chunk->max = value;
    chunk->has_values = 1;
  }
  else
  {
    if (value < chunk->min) chunk->min = value;
    if (value > chunk->max) chunk->max = value;
  }
  
  column->has_zone_map = 1;
  column->zonemap_checked_generation = column->write_generation;
}

internal void
cql_column_add_data_maybe_null(CQL_Column* column, void* data, B32 is_null)
{
  // tec: append a placeholder value through the existing storage path, NULL tracking is a side channel layered on top
  cql_column_add_data(column, data);
  
  // tec: null_flags is only touched once the column has seen a NULL, so a column without NULLs pays nothing
  if (is_null || column->null_flags)
  {
    cql_column_ensure_null_flags_capacity(column, column->row_count);
    column->null_flags[column->row_count - 1] = is_null ? 1 : 0;
  }
  
  if (!is_null)
  {
    cql_column_zone_map_note_append(column, data);
  }
}

internal B32
cql_column_is_null(CQL_Column* column, U64 row_index)
{
  return column->null_flags && row_index < column->null_flags_capacity && column->null_flags[row_index];
}

internal void
cql_column_remove_data(CQL_Column* column, U64 row_index)
{
  if (row_index >= column->row_count)
  {
    log_error("column row index out of bounds: %llu", row_index);
    return;
  }
  
  if (column->is_disk_backed)
  {
    cql_column_materialize_to_memory(column);
  }
  
  if (column->type == CQL_ColumnType_String8)
  {
    // tec: offsets[k] is the end offset of row k (no leading zero entry)
    // the removed row's byte range is [offsets[row_index-1] or 0, offsets[row_index]), not [offsets[row_index], offsets[row_index+1])
    U64 start_offset = (row_index > 0) ? column->offsets[row_index - 1] : 0;
    U64 end_offset = column->offsets[row_index];
    U64 removed_size = end_offset - start_offset;
    U64 size_to_move = column->variable_capacity - end_offset;
    
    MemoryCopy(column->data + start_offset, column->data + end_offset, size_to_move);
    
    for (U64 i = row_index; i < column->row_count - 1; ++i)
    {
      column->offsets[i] = column->offsets[i + 1] - removed_size;
    }
    
    column->variable_capacity -= removed_size;
  }
  else
  {
    U64 size_to_move = (column->row_count - row_index - 1) * column->size;
    MemoryCopy(column->data + row_index * column->size, column->data + (row_index + 1) * column->size, size_to_move);
  }
  
  if (column->null_flags)
  {
    U64 flags_to_move = column->row_count - row_index - 1;
    MemoryCopy(column->null_flags + row_index, column->null_flags + row_index + 1, flags_to_move);
  }
  
  column->row_count--;
  column->write_generation++;
}

internal void*
cql_column_get_data(CQL_Column* column, U64 index)
{
  cql_column_flush_pending(column);
  if (index >= column->row_count)
  {
    log_error("index %llu out of bounds %llu", index, column->row_count);
    return NULL;
  }
  
  if (column->is_disk_backed)
  {
    U64 offset = index * column->size;
    OS_Handle file = column->file;
    B32 temp_opened = 0;
    if (os_handle_match(os_handle_zero(), file))
    {
      file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_ShareRead, column->disk_path);
      temp_opened = 1;
    }
    void* data = arena_push(column->arena, column->size, 8);
    os_file_read(file, r1u64(offset, offset + column->size), data);
    if (temp_opened)
    {
      os_file_close(file);
    }
    return data;
  }
  else
  {
    return (void*)(column->data + index * column->size);
  }
}

internal String8
cql_column_get_string(Arena *arena, CQL_Column *column, U64 index)
{
  cql_column_flush_pending(column);
  String8 result = {0};
  
  if (index >= column->row_count || column->type != CQL_ColumnType_String8)
    return result;
  
  if (column->is_disk_backed)
  {
    OS_Handle file = column->file;
    B32 temp_opened = 0;
    if (os_handle_match(os_handle_zero(), file))
    {
      file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_ShareRead, column->disk_path);
      temp_opened = 1;
    }
    
    U64 var_reserved = 0;
    os_file_read(file, r1u64(0, sizeof(U64)), &var_reserved);
    
    // tec: offsets[k] = end-of-row-k byte position (0-indexed, no leading zero stored)
    U64 offset_base = var_reserved + sizeof(U64);
    U64 offset_pos = offset_base + (index * sizeof(U64));
    
    U64 start = 0, end = 0;
    
    if (index == 0)
    {
      start = 0;
    }
    else
    {
      os_file_read(file, r1u64(offset_pos - sizeof(U64), offset_pos), &start);
    }
    os_file_read(file, r1u64(offset_pos, offset_pos + sizeof(U64)), &end);
    
    if (end < start)
    {
      result = str8_lit("invalid string");
    }
    else
    {
      U64 size = end - start;
      result.str = arena_push(arena, size, 8);
      U64 data_pos = sizeof(U64) + start;
      os_file_read(file, r1u64(data_pos, data_pos + size), result.str);
      result.size = size;
    }
    
    if (temp_opened)
    {
      os_file_close(file);
    }
  }
  else
  {
    U64 start = (index > 0) ? column->offsets[index - 1] : 0;
    U64 end = column->offsets[index];
    
    if (end >= start && end <= column->variable_capacity)
    {
      result.str = column->data + start;
      result.size = end - start;
    }
  }
  
  return result;
}

// tec: TODO i think this doesnt accurately reflect the column sizes
// because for strings the file size may be different than the actual size
internal U64
cql_column_get_total_size(CQL_Column* column)
{
  cql_column_flush_pending(column);
  U64 total_size = 0;
  
  if (column->is_disk_backed)
  {
    FileProperties props = os_properties_from_file_path(column->disk_path);
    total_size = props.size;
    
    if (column->type == CQL_ColumnType_String8 && column->row_count > 0)
    {
      U64 expected_minimum_size = (column->row_count + 1) * sizeof(U64);
      if (total_size < expected_minimum_size)
      {
        log_error("String column file too small: %.*s", str8_varg(column->name));
      }
    }
  }
  else
  {
    if (column->type == CQL_ColumnType_String8)
    {
      total_size += sizeof(U64);
      total_size += column->variable_capacity;
      total_size = column->variable_capacity + (column->row_count + 1) * sizeof(U64);
      //total_size += column->row_count * sizeof(U64);
    }
    else
    {
      total_size = column->row_count * column->size;
    }
  }
  
  return total_size;
}

internal void*
cql_column_get_data_range(Arena* arena, CQL_Column* column, Rng1U64 row_range, U64* out_size)
{
  cql_column_flush_pending(column);
  ProfBeginFunction();
  
  if (column->type == CQL_ColumnType_String8)
  {
    log_error("cql_column_get_data_range was called, but column type is string. Use cql_column_get_string_chunk instead.");
    *out_size = 0;
    ProfEnd();
    return NULL;
  }
  
  if (row_range.max < row_range.min || row_range.max > column->row_count)
  {
    log_error("invalid row range [%llu - %llu] for column with %llu rows", row_range.min, row_range.max, column->row_count);
    *out_size = 0;
    ProfEnd();
    return NULL;
  }
  
  U64 row_count = row_range.max - row_range.min;
  U64 size = row_count * column->size;
  *out_size = size;
  
  if (!column->is_disk_backed)
  {
    void* data_ptr = column->data + (row_range.min * column->size);
    ProfEnd();
    return data_ptr;
  }
  
  if (os_handle_match(os_handle_zero(), column->file))
  {
    column->file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column->disk_path);
    if (os_handle_match(os_handle_zero(), column->file))
    {
      log_error("failed to open disk-backed column: %.*s", str8_varg(column->disk_path));
      *out_size = 0;
      ProfEnd();
      return NULL;
    }
  }
  if (os_handle_match(os_handle_zero(), column->file_map))
  {
    column->file_map = os_file_map_open(OS_AccessFlag_Read, column->file);
    if (os_handle_match(os_handle_zero(), column->file_map))
    {
      log_error("failed to open file mapping for column: %.*s", str8_varg(column->disk_path));
      *out_size = 0;
      ProfEnd();
      return NULL;
    }
  }
  
  U64 byte_offset = row_range.min * column->size;
  Rng1U64 byte_range = r1u64(byte_offset, byte_offset + size);
  
  if (byte_range.max > os_properties_from_file(column->file).size)
  {
    byte_range.max = os_properties_from_file(column->file).size;
  }
  
  // tec: reuse the mapped view if it covers this range and no write has landed since
  // avoids a fresh os_file_map_view_open on repeated scans of the same range
  if (column->mapped_ptr
      && column->mapped_generation == column->write_generation
      && byte_range.min >= column->current_mapped_range.min
      && byte_range.max <= column->current_mapped_range.max)
  {
    ProfEnd();
    return (U8*)column->mapped_ptr + (byte_offset - column->current_mapped_range.min);
  }
  
  void* mapped_ptr = os_file_map_view_open(column->file_map, OS_AccessFlag_Read, byte_range);
  if (!mapped_ptr)
  {
    log_error("failed to memory map column range for %.*s", str8_varg(column->name));
    *out_size = 0;
    ProfEnd();
    return NULL;
  }
  
  column->mapped_ptr = mapped_ptr;
  column->current_mapped_range = byte_range;
  column->mapped_generation = column->write_generation;
  
  ProfEnd();
  return mapped_ptr;
}


internal CQL_StringDataChunk
cql_column_get_string_chunk(Arena* arena, CQL_Column* column, Rng1U64 row_range)
{
  cql_column_flush_pending(column);
  ProfBeginFunction();
  
  CQL_StringDataChunk result = {0};
  
  U64 row_count = row_range.max - row_range.min;
  if (row_count == 0)
  {
    result.data = NULL;
    result.offsets = NULL;
    result.size = 0;
    result.row_count = 0;
    return result;
  }
  
  if (column->is_disk_backed)
  {
    // tec: ShareRead+ShareWrite
    OS_Handle file = column->file;
    if (os_handle_match(os_handle_zero(), file))
    {
      file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column->disk_path);
    }
    OS_Handle file_map = column->file_map;
    if (os_handle_match(os_handle_zero(), file_map))
    {
      OS_Handle map_backing_file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column->disk_path);
      file_map = os_file_map_open(OS_AccessFlag_Read, map_backing_file);
      column->file_map = file_map;
      column->file_map_backing_file = map_backing_file;
    }
    
    // tec: on-disk string column layout (see cql_table_save):
    //   [0, 8)                          : U64 variable_capacity (size of the raw string data blob)
    //   [8, 8+variable_capacity)        : raw string bytes (all rows, contiguous)
    //   [8+variable_capacity, ...)      : U64 offsets[capacity], offsets[k] = end-of-row-k byte
    //                                     position within the data blob (0-indexed, no leading zero)
    U64 variable_capacity = 0;
    os_file_read(file, r1u64(0, sizeof(U64)), &variable_capacity);
    
    U64 offsets_base = sizeof(U64) + variable_capacity;
    
    U64 start_offset = 0;
    U64 end_offset = 0;
    U64 offset_position_start = offsets_base + (row_range.min * sizeof(U64));
    U64 offset_position_end = offsets_base + ((row_range.max - 1) * sizeof(U64));
    
    if (row_range.min == 0)
    {
      start_offset = 0;
    }
    else
    {
      os_file_read(file, r1u64(offset_position_start - sizeof(U64), offset_position_start), &start_offset);
    }
    
    os_file_read(file, r1u64(offset_position_end, offset_position_end + sizeof(U64)), &end_offset);
    
    ProfBegin("read string data");
    U64 size = end_offset - start_offset;
    Rng1U64 str_data_range = r1u64(start_offset + sizeof(U64), start_offset + size + sizeof(U64));
    result.data = push_array(arena, U8, size);
    /*
    if (os_file_read(file, , result.data) != size)
    {
      log_error("Failed to read string data for rows [%llu - %llu]", row_range.min, row_range.max);
    }
    */
    column->mapped_ptr = os_file_map_view_open(file_map, OS_AccessFlag_Read, str_data_range);
    result.data = column->mapped_ptr;
    column->current_mapped_range = str_data_range;
    if (!column->mapped_ptr)
    {
      log_error("failed to map file for string data");
    }
    ProfEnd();
    
    // tec: NOTE add 1 to the row count to include the trailing offset (end of last row),
    // matching the in-memory (non-disk-backed) path below and what consumers expect.
    result.offsets = push_array(arena, U64, row_count + 1);
    ProfBegin("read string offsets");
    {
      U64 *raw_offsets = push_array(arena, U64, row_count);
      os_file_read(file, r1u64(offset_position_start, offset_position_start + row_count * sizeof(U64)), raw_offsets);
      
      result.offsets[0] = 0;
      for (U64 i = 0; i < row_count; i++)
      {
        result.offsets[i + 1] = raw_offsets[i] - start_offset;
      }
    }
    ProfEnd();
    
    if (os_handle_match(os_handle_zero(), column->file))
    {
      os_file_close(file);
    }
    
    result.size = size;
    result.row_count = row_count;
  }
  else
  {
    U64 start_offset = (row_range.min > 0) ? column->offsets[row_range.min - 1] : 0;
    U64 end_offset = column->offsets[row_range.max - 1];
    U64 size = end_offset - start_offset;
    
    result.data = column->data + start_offset;
    result.size = size;
    result.row_count = row_count;
    
    // tec: NOTE add 1 to the row count to include the last offset
    result.offsets = push_array(arena, U64, row_count+1);
    for (U64 i = 0; i < row_count+1; i++)
    {
      result.offsets[i] = (row_range.min + i > 0) ? column->offsets[row_range.min + i - 1] - start_offset : 0;
    }
  }
  
  ProfEnd();
  return result;
}

internal void
cql_column_close_string_chunk(CQL_Column* column)
{
  OS_Handle file_map = column->file_map;
  os_file_map_view_close(file_map, column->mapped_ptr, column->current_mapped_range);
}

internal B32
cql_string_dict_code_from_value(CQL_StringDict* dict, String8 value, U32* out_code)
{
  U64 mask = dict->index_capacity - 1;
  U64 slot = u64_hash_from_str8(value) & mask;
  for (U32 probe = 0; probe < CQL_DICT_MAX_PROBE; probe++)
  {
    U32 code = dict->index_codes[slot];
    if (code == CQL_DICT_EMPTY_SLOT)
    {
      return 0;
    }
    if (str8_match(dict->values[code], value, 0))
    {
      *out_code = code;
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

internal U32
cql_string_dict_code_or_sentinel(CQL_StringDict* dict, String8 value)
{
  U32 code = 0;
  return cql_string_dict_code_from_value(dict, value, &code) ? code : CQL_DICT_NOT_FOUND;
}

internal String8
cql_dict_row_string(CQL_StringDataChunk* chunk, U64 row)
{
  if (!chunk->data)
  {
    return g_cql_dict_empty_str8;
  }
  U64 start = chunk->offsets[row];
  U64 len = chunk->offsets[row + 1] - start;
  return str8((U8*)chunk->data + start, len);
}

internal THREAD_POOL_TASK_FUNC(cql_dict_claim_task)
{
  CQL_DictBuildCtx* ctx = (CQL_DictBuildCtx*)raw_task;
  Rng1U64 range = ctx->ranges[task_id];
  U64 mask = ctx->capacity - 1;
  
  for (U64 row = range.min; row < range.max; row++)
  {
    if (ctx->overflow_flag) break;
    
    String8 s = cql_dict_row_string(&ctx->chunk, row);
    U64 slot = u64_hash_from_str8(s) & mask;
    B32 placed = 0;
    
    for (U32 probe = 0; probe < CQL_DICT_MAX_PROBE; probe++)
    {
      U32 existing = ins_atomic_u32_eval_cond_assign(&ctx->owner_row[slot], (U32)row, CQL_DICT_EMPTY_SLOT);
      if (existing == CQL_DICT_EMPTY_SLOT)
      {
        placed = 1;
        break;
      }
      
      String8 existing_str = cql_dict_row_string(&ctx->chunk, existing);
      if (MemoryMatch(existing_str.str, s.str, s.size) && existing_str.size == s.size)
      {
        placed = 1;
        break;
      }
      
      slot = (slot + 1) & mask;
    }
    
    if (!placed)
    {
      ins_atomic_u32_eval_assign(&ctx->overflow_flag, 1);
      break;
    }
  }
}

internal THREAD_POOL_TASK_FUNC(cql_dict_fill_codes_task)
{
  CQL_DictFillCtx* ctx = (CQL_DictFillCtx*)raw_task;
  Rng1U64 range = ctx->ranges[task_id];
  
  for (U64 row = range.min; row < range.max; row++)
  {
    String8 s = cql_dict_row_string(&ctx->chunk, row);
    U32 code = 0;
    B32 found = cql_string_dict_code_from_value(ctx->dict, s, &code);
    ctx->dict_codes[row] = found ? code : 0;
  }
}

internal void
cql_column_ensure_string_dict(CQL_Column* column)
{
  ProfBeginFunction();
  
  if (column->type != CQL_ColumnType_String8)
  {
    ProfEnd();
    return;
  }
  
  if (column->dict_checked_generation == column->write_generation)
  {
    ProfEnd();
    return;
  }
  
  if (column->dict)
  {
    arena_release(column->dict->arena);
  }
  column->has_dict = 0;
  column->dict = 0;
  column->dict_codes = 0;
  
  if (column->row_count < g_cql_state->dict_encode_min_rows)
  {
    column->dict_checked_generation = column->write_generation;
    ProfEnd();
    return;
  }
  
  Temp scratch = scratch_begin(0, 0);
  
  CQL_DictBuildCtx build_ctx = {0};
  build_ctx.chunk = cql_column_get_string_chunk(scratch.arena, column, r1u64(0, column->row_count));
  build_ctx.capacity = u64_up_to_pow2(g_cql_state->dict_encode_max_distinct * CQL_DICT_TABLE_CAPACITY_FACTOR);
  build_ctx.owner_row = push_array(scratch.arena, U32, build_ctx.capacity);
  for (U64 i = 0; i < build_ctx.capacity; i++) build_ctx.owner_row[i] = CQL_DICT_EMPTY_SLOT;
  
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, column->row_count));
  build_ctx.ranges = tp_divide_work(scratch.arena, column->row_count, (U32)task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, cql_dict_claim_task, &build_ctx);
  tp_temp_end(temp);
  
  B32 eligible = !build_ctx.overflow_flag;
  U32 distinct_count = 0;
  
  if (eligible)
  {
    for (U64 slot = 0; slot < build_ctx.capacity; slot++)
    {
      if (build_ctx.owner_row[slot] != CQL_DICT_EMPTY_SLOT)
      {
        if (distinct_count >= g_cql_state->dict_encode_max_distinct)
        {
          eligible = 0;
          break;
        }
        distinct_count++;
      }
    }
  }
  
  if (eligible && (F64)distinct_count / (F64)column->row_count > g_cql_state->dict_encode_max_cardinality_ratio)
  {
    eligible = 0;
  }
  
  if (eligible)
  {
    Arena* dict_arena = arena_alloc();
    CQL_StringDict* dict = push_array(dict_arena, CQL_StringDict, 1);
    dict->arena = dict_arena;
    dict->value_count = distinct_count;
    dict->values = push_array(dict_arena, String8, Max(distinct_count, 1));
    dict->index_capacity = build_ctx.capacity;
    dict->index_codes = push_array(dict_arena, U32, build_ctx.capacity);
    
    U32 next_code = 0;
    for (U64 slot = 0; slot < build_ctx.capacity; slot++)
    {
      U32 owner = build_ctx.owner_row[slot];
      if (owner == CQL_DICT_EMPTY_SLOT)
      {
        dict->index_codes[slot] = CQL_DICT_EMPTY_SLOT;
      }
      else
      {
        U32 code = next_code++;
        dict->values[code] = push_str8_copy(dict_arena, cql_dict_row_string(&build_ctx.chunk, owner));
        dict->index_codes[slot] = code;
      }
    }
    
    U32* dict_codes = push_array(column->arena, U32, Max(column->row_count, 1));
    
    CQL_DictFillCtx fill_ctx = {0};
    fill_ctx.chunk = build_ctx.chunk;
    fill_ctx.ranges = build_ctx.ranges;
    fill_ctx.dict = dict;
    fill_ctx.dict_codes = dict_codes;
    
    temp = tp_temp_begin(pool_arena);
    tp_for_parallel(pool, pool_arena, task_count, cql_dict_fill_codes_task, &fill_ctx);
    tp_temp_end(temp);
    
    column->dict = dict;
    column->dict_codes = dict_codes;
    column->has_dict = 1;
  }
  
  if (build_ctx.chunk.data)
  {
    cql_column_close_string_chunk(column);
  }
  
  column->dict_checked_generation = column->write_generation;
  
  scratch_end(scratch);
  ProfEnd();
}

internal THREAD_POOL_TASK_FUNC(cql_zone_map_build_task)
{
  CQL_ZoneMapBuildCtx* ctx = (CQL_ZoneMapBuildCtx*)raw_task;
  Rng1U64 chunk_range = ctx->chunk_ranges[task_id];
  CQL_Column* column = ctx->column;
  
  for (U64 c = chunk_range.min; c < chunk_range.max; c++)
  {
    U64 row_start = c * ctx->chunk_rows;
    U64 row_end = Min(row_start + ctx->chunk_rows, column->row_count);
    if (row_start >= row_end) break;
    
    CQL_ZoneMapChunk* out = &ctx->zone_map[c];
    
    // tec: a NULL free column of a plain 32 bit type is folded in its own type, several times faster than converting every row to a double
    if (ctx->base_ptr && !column->null_flags && column->type == CQL_ColumnType_U32)
    {
      U32* values = (U32*)ctx->base_ptr + row_start;
      U32 low = values[0];
      U32 high = values[0];
      for (U64 i = 1; i < row_end - row_start; i++)
      {
        low = Min(low, values[i]);
        high = Max(high, values[i]);
      }
      out->min = (F64)low;
      out->max = (F64)high;
      out->has_values = 1;
      continue;
    }
    if (ctx->base_ptr && !column->null_flags && column->type == CQL_ColumnType_I32)
    {
      S32* values = (S32*)ctx->base_ptr + row_start;
      S32 low = values[0];
      S32 high = values[0];
      for (U64 i = 1; i < row_end - row_start; i++)
      {
        low = Min(low, values[i]);
        high = Max(high, values[i]);
      }
      out->min = (F64)low;
      out->max = (F64)high;
      out->has_values = 1;
      continue;
    }
    
    for (U64 row = row_start; row < row_end; row++)
    {
      if (cql_column_is_null(column, row)) continue;
      if (!ctx->base_ptr) continue;
      
      void* data = (U8*)ctx->base_ptr + row * column->size;
      F64 value = cql_numeric_value_as_f64(column->type, data);
      if (!out->has_values)
      {
        out->min = value;
        out->max = value;
        out->has_values = 1;
      }
      else
      {
        if (value < out->min) out->min = value;
        if (value > out->max) out->max = value;
      }
    }
  }
}

internal void
cql_column_ensure_zone_map(CQL_Column* column)
{
  ProfBeginFunction();
  
  if (!cql_column_type_is_zone_map_eligible(column->type))
  {
    ProfEnd();
    return;
  }
  
  if (column->zonemap_checked_generation == column->write_generation)
  {
    ProfEnd();
    return;
  }
  
  column->zone_map = 0;
  column->zone_map_capacity = 0;
  column->zone_map_chunk_count = 0;
  column->has_zone_map = 0;
  
  U64 chunk_rows = g_cql_state->zonemap_chunk_rows;
  U64 chunk_count = (column->row_count + chunk_rows - 1) / chunk_rows;
  
  if (chunk_count > 0)
  {
    CQL_ZoneMapChunk* zone_map = push_array(column->arena, CQL_ZoneMapChunk, chunk_count);
    
    Temp scratch = scratch_begin(0, 0);
    U64 range_size = 0;
    void* base_ptr = cql_column_get_data_range(scratch.arena, column, r1u64(0, column->row_count), &range_size);
    
    TP_Context* pool = app_thread_pool();
    U64 task_count = Max((U64)1, Min((U64)pool->worker_count, chunk_count));
    Rng1U64* chunk_ranges = tp_divide_work(scratch.arena, chunk_count, (U32)task_count);
    
    CQL_ZoneMapBuildCtx ctx = {0};
    ctx.column = column;
    ctx.zone_map = zone_map;
    ctx.chunk_ranges = chunk_ranges;
    ctx.chunk_rows = chunk_rows;
    ctx.base_ptr = base_ptr;
    
    TP_Arena* pool_arena = app_thread_pool_arena();
    TP_Temp temp = tp_temp_begin(pool_arena);
    tp_for_parallel(pool, pool_arena, task_count, cql_zone_map_build_task, &ctx);
    tp_temp_end(temp);
    
    scratch_end(scratch);
    
    column->zone_map = zone_map;
    column->zone_map_capacity = chunk_count;
    column->zone_map_chunk_count = chunk_count;
    column->has_zone_map = 1;
  }
  
  column->zonemap_checked_generation = column->write_generation;
  
  ProfEnd();
}

internal String8
cql_generate_disk_path_for_column(Arena* arena, CQL_Column* column)
{
  CQL_Table* table = column->parent_table;
  CQL_Database* database = table->parent_database;
  
  {
    Temp scratch = scratch_begin(0, 0);
    
    if (!os_file_path_exists(str8_lit("cql_data/")))
    {
      os_make_directory(str8_lit("cql_data/"));
    }
    
    String8 database_path = push_str8f(arena, "cql_data/%.*s/", str8_varg(database->name));
    
    if (!os_file_path_exists(database_path))
    {
      os_make_directory(database_path);
    }
    
    String8 table_path = push_str8f(arena, "cql_data/%.*s/%.*s/", str8_varg(database->name), str8_varg(table->name));
    if (!os_file_path_exists(table_path))
    {
      os_make_directory(table_path);
    }
    
    scratch_end(scratch);
  }
  
  String8 column_path = push_str8f(arena, "cql_data/%.*s/%.*s/%.*s.dat", str8_varg(database->name), str8_varg(table->name), str8_varg(column->name));
  return column_path;
}

internal void
cql_column_convert_to_disk_backed(CQL_Column* column)
{
  ProfBeginFunction();
  
  Temp scratch = scratch_begin(0, 0);
  String8 column_path = cql_generate_disk_path_for_column(scratch.arena, column);
  // tec: this handle is cached in column->file and kept open for the column's whole lifetime
  OS_Handle file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_Write | OS_AccessFlag_Append | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column_path);
  
  if (column->type == CQL_ColumnType_String8)
  {
    os_file_write(file, r1u64(0, sizeof(U64)), &column->variable_capacity);
    os_file_write(file, r1u64(sizeof(U64), sizeof(U64) + column->variable_capacity), column->data);
    os_file_write(file, r1u64(sizeof(U64) + column->variable_capacity,
                              sizeof(U64) + column->variable_capacity +
                              column->row_count * sizeof(U64)),
                  column->offsets);
    column->variable_capacity = column->offsets[column->row_count - 1];
  }
  else
  {
    os_file_write(file, r1u64(0, (column->row_count + 1) * column->size), column->data);
  }
  
  column->is_disk_backed = 1;
  column->disk_path = push_str8_copy(column->arena, column_path);
  column->file = file;
  
  column->data = NULL;
  column->offsets = NULL;
  column->capacity = 0;
  
  scratch_end(scratch);
  
  ProfEnd();
}

internal void
cql_column_materialize_to_memory(CQL_Column* column)
{
  cql_column_flush_pending(column);
  if (!column->is_disk_backed) return;
  
  ProfBeginFunction();
  
  U64 row_count = column->row_count;
  
  if (column->type == CQL_ColumnType_String8)
  {
    CQL_StringDataChunk chunk = cql_column_get_string_chunk(column->arena, column, r1u64(0, row_count));
    
    // tec: copy chunk.data before closing the mapping
    U8* owned_data = push_array(column->arena, U8, Max(chunk.size, 1));
    if (chunk.size > 0) MemoryCopy(owned_data, chunk.data, chunk.size);
    cql_column_close_string_chunk(column);
    column->mapped_ptr = 0;
    
    // tec: the chunk's offsets carry a leading 0 (row_count+1 entries);
    // this column's own inmemory convention has no leading entry (offsets[k] = end offset of row k)
    U64* owned_offsets = push_array(column->arena, U64, Max(row_count, 1));
    for (U64 i = 0; i < row_count; i++)
    {
      owned_offsets[i] = chunk.offsets[i + 1];
    }
    
    column->data = owned_data;
    column->offsets = owned_offsets;
    column->variable_capacity = chunk.size;
    column->capacity = Max(row_count, 1);
  }
  else
  {
    U64 size = 0;
    void* data_ptr = cql_column_get_data_range(column->arena, column, r1u64(0, row_count), &size);
    U8* owned_data = push_array(column->arena, U8, Max(size, column->size));
    if (size > 0) MemoryCopy(owned_data, data_ptr, size);
    
    if (column->mapped_ptr)
    {
      os_file_map_view_close(column->file_map, column->mapped_ptr, column->current_mapped_range);
      column->mapped_ptr = 0;
    }
    
    column->data = owned_data;
    column->capacity = Max(row_count, 1);
  }
  
  if (!os_handle_match(os_handle_zero(), column->file_map))
  {
    os_file_map_close(column->file_map);
    column->file_map = os_handle_zero();
  }
  if (!os_handle_match(os_handle_zero(), column->file_map_backing_file))
  {
    os_file_close(column->file_map_backing_file);
    column->file_map_backing_file = os_handle_zero();
  }
  if (!os_handle_match(os_handle_zero(), column->file))
  {
    os_file_close(column->file);
    column->file = os_handle_zero();
  }
  
  column->is_disk_backed = 0;
  
  ProfEnd();
}

//~ tec: utils
internal CQL_ColumnType
cql_column_type_from_string(CQL_Database* database, String8 str)
{
  if (str8_match(str, str8_lit("u32"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_U32;
  }
  else if (str8_match(str, str8_lit("u64"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_U64;
  }
  else if (str8_match(str, str8_lit("f32"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_F32;
  }
  else if (str8_match(str, str8_lit("f64"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_F64;
  }
  else if (str8_match(str, str8_lit("string8"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_String8;
  }
  else if (str8_match(str, str8_lit("bool"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_Bool;
  }
  else if (str8_match(str, str8_lit("i32"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_I32;
  }
  else if (str8_match(str, str8_lit("i64"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_I64;
  }
  else if (str8_match(str, str8_lit("date"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_Date;
  }
  else if (str8_match(str, str8_lit("timestamp"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_Timestamp;
  }
  else if (str8_match(str, str8_lit("decimal"), StringMatchFlag_CaseInsensitive) ||
           str8_match(str, str8_lit("numeric"), StringMatchFlag_CaseInsensitive))
  {
    return CQL_ColumnType_Decimal;
  }
  else if (cql_database_find_enum_type(database, str))
  {
    return CQL_ColumnType_Enum;
  }
  
  log_error("failed to find matching CQL_ColumnType for '%.*s'", str8_varg(str));
  return CQL_ColumnType_Invalid;
}

internal String8
string_from_cql_column_type(CQL_ColumnType type)
{
  String8 result = str8_lit("");
  switch (type)
  {
    case CQL_ColumnType_U32: { result = str8_lit("CQL_ColumnType_U32"); } break;
    case CQL_ColumnType_U64: { result = str8_lit("CQL_ColumnType_U64"); } break;
    case CQL_ColumnType_F32: { result = str8_lit("CQL_ColumnType_F32"); } break;
    case CQL_ColumnType_F64: { result = str8_lit("CQL_ColumnType_F64"); } break;
    case CQL_ColumnType_String8: { result = str8_lit("CQL_ColumnType_String8"); } break;
    case CQL_ColumnType_Bool: { result = str8_lit("CQL_ColumnType_Bool"); } break;
    case CQL_ColumnType_I32: { result = str8_lit("CQL_ColumnType_I32"); } break;
    case CQL_ColumnType_I64: { result = str8_lit("CQL_ColumnType_I64"); } break;
    case CQL_ColumnType_Date: { result = str8_lit("CQL_ColumnType_Date"); } break;
    case CQL_ColumnType_Timestamp: { result = str8_lit("CQL_ColumnType_Timestamp"); } break;
    case CQL_ColumnType_Decimal: { result = str8_lit("CQL_ColumnType_Decimal"); } break;
    case CQL_ColumnType_Enum: { result = str8_lit("CQL_ColumnType_Enum"); } break;
  }
  return result;
}

internal CQL_ColumnSchema
cql_column_schema_create(String8 name, CQL_ColumnType type)
{
  CQL_ColumnSchema schema = (CQL_ColumnSchema){ name, type, g_cql_column_type_size[type] };
  return schema;
}

internal String8
cql_column_type_display_name(Arena* arena, CQL_Column* column)
{
  String8 result = str8_lit("");
  switch (column->type)
  {
    case CQL_ColumnType_U32:     result = str8_lit("u32");     break;
    case CQL_ColumnType_U64:     result = str8_lit("u64");     break;
    case CQL_ColumnType_F32:     result = str8_lit("f32");     break;
    case CQL_ColumnType_F64:     result = str8_lit("f64");     break;
    case CQL_ColumnType_String8: result = str8_lit("string8"); break;
    case CQL_ColumnType_Bool:    result = str8_lit("bool");    break;
    case CQL_ColumnType_I32:     result = str8_lit("i32");     break;
    case CQL_ColumnType_I64:     result = str8_lit("i64");     break;
    case CQL_ColumnType_Date:      result = str8_lit("date");      break;
    case CQL_ColumnType_Timestamp: result = str8_lit("timestamp"); break;
    case CQL_ColumnType_Decimal:
    result = push_str8f(arena, "decimal(%u,%u)", column->decimal_precision, column->decimal_scale);
    break;
    case CQL_ColumnType_Enum:
    result = column->enum_type ? push_str8_copy(arena, column->enum_type->name) : str8_lit("enum");
    break;
    default: log_error("cql_column_type_display_name: unhandled CQL_ColumnType %u", column->type); break;
  }
  return result;
}

// tec: this and decimal to str8 should probably go into the base layer
internal B32
decimal_from_str8(String8 str, U32 scale, S64* out_raw)
{
  S64 sign = 1;
  String8 rest = str;
  if (rest.size > 0 && (rest.str[0] == '-' || rest.str[0] == '+'))
  {
    if (rest.str[0] == '-') sign = -1;
    rest = str8_skip(rest, 1);
  }
  
  U64 dot = str8_find_needle(rest, 0, str8_lit("."), 0);
  String8 int_part = (dot < rest.size) ? str8_prefix(rest, dot) : rest;
  String8 frac_part = (dot < rest.size) ? str8_skip(rest, dot + 1) : str8_zero();
  
  if (int_part.size == 0 && frac_part.size == 0) return 0;
  if (int_part.size > 0 && !str8_is_integer(int_part, 10)) return 0;
  if (frac_part.size > 0 && !str8_is_integer(frac_part, 10)) return 0;
  
  U64 int_value = int_part.size ? u64_from_str8(int_part, 10) : 0;
  
  U64 frac_len = Min(frac_part.size, (U64)scale);
  String8 frac_kept = str8_prefix(frac_part, frac_len);
  U64 frac_value = frac_kept.size ? u64_from_str8(frac_kept, 10) : 0;
  for (U64 k = frac_len; k < scale; k++) frac_value *= 10;
  
  B32 round_up = 0;
  if (frac_part.size > scale)
  {
    U8 round_digit = frac_part.str[scale];
    round_up = (round_digit >= '5' && round_digit <= '9');
  }
  
  U64 pow10_scale = 1;
  for (U32 i = 0; i < scale; i++) pow10_scale *= 10;
  
  U64 magnitude = int_value * pow10_scale + frac_value;
  if (round_up) magnitude += 1;
  
  *out_raw = sign * (S64)magnitude;
  return 1;
}

internal String8
decimal_to_str8(Arena* arena, S64 raw, U32 scale)
{
  if (scale == 0) return str8_from_s64(arena, raw, 10, 0, 0);
  
  B32 negative = raw < 0;
  U64 magnitude = negative ? (U64)(-raw) : (U64)raw;
  
  U64 pow10_scale = 1;
  for (U32 i = 0; i < scale; i++) pow10_scale *= 10;
  
  U64 int_part = magnitude / pow10_scale;
  U64 frac_part = magnitude % pow10_scale;
  
  return push_str8f(arena, "%s%llu.%0*llu", negative ? "-" : "", int_part, (int)scale, frac_part);
}

//~ tec: column catalog
internal CQL_Table*
cql_database_build_column_catalog(CQL_Database* database)
{
  CQL_Table* catalog = cql_table_alloc(CQL_COLUMN_CATALOG_TABLE_NAME);
  
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("table_name"), CQL_ColumnType_String8));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("column_name"), CQL_ColumnType_String8));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("type"), CQL_ColumnType_String8));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("ordinal"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("nullable"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("is_unique"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("is_primary_key"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("is_foreign_key"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("fk_ref_table"), CQL_ColumnType_String8));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("fk_ref_column"), CQL_ColumnType_String8));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("has_check"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("check_text"), CQL_ColumnType_String8));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("is_indexed"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("index_name"), CQL_ColumnType_String8));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("is_disk_backed"), CQL_ColumnType_U32));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("row_count"), CQL_ColumnType_U64));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("distinct_count"), CQL_ColumnType_U64));
  cql_table_add_column(catalog, cql_column_schema_create(str8_lit("null_count"), CQL_ColumnType_U64));
  
  String8 empty = str8_lit("");
  
  for (U64 t = 0; t < database->table_count; t++)
  {
    CQL_Table* table = database->tables[t];
    
    for (U64 c = 0; c < table->column_count; c++)
    {
      CQL_Column* column = table->columns[c];
      CQL_Index* index_on_column = cql_table_find_index_on_column(table, column);
      
      String8 type_name = cql_column_type_display_name(catalog->arena, column);
      U32 ordinal = (U32)c;
      U32 nullable = column->not_null ? 0 : 1;
      U32 is_unique = column->is_unique ? 1 : 0;
      U32 is_primary_key = column->is_primary_key ? 1 : 0;
      U32 is_foreign_key = column->has_foreign_key ? 1 : 0;
      U32 has_check = column->has_check ? 1 : 0;
      U32 is_indexed = index_on_column ? 1 : 0;
      U32 is_disk_backed = column->is_disk_backed ? 1 : 0;
      U64 row_count = table->row_count;
      
      String8 fk_ref_table = column->has_foreign_key ? column->fk_ref_table_name : empty;
      String8 fk_ref_column = column->has_foreign_key ? column->fk_ref_column_name : empty;
      String8 check_text = column->has_check ? column->check_text : empty;
      String8 index_name = index_on_column ? index_on_column->name : empty;
      
      // tec: stats only show up after ANALYZE or a query that needed them, this never computes them
      B32 has_stats = cql_column_stats_is_current(column);
      U64 distinct_count = column->stats.distinct_count;
      U64 null_count = column->stats.null_count;
      
      void* row_data[18] = {
        &table->name, &column->name, &type_name, &ordinal, &nullable, &is_unique,
        &is_primary_key, &is_foreign_key, &fk_ref_table, &fk_ref_column, &has_check,
        &check_text, &is_indexed, &index_name, &is_disk_backed, &row_count,
        &distinct_count, &null_count,
      };
      B32 null_flags[18] = {0};
      null_flags[16] = !has_stats;
      null_flags[17] = !has_stats;
      
      cql_table_add_row(catalog, row_data, null_flags);
    }
  }
  
  return catalog;
}

internal void
cql_database_add_temp_table(CQL_Database* database, CQL_Table* table)
{
  if (database->temp_table_count >= database->temp_table_capacity)
  {
    U64 new_capacity = Max((U64)8, database->temp_table_capacity * 2);
    CQL_Table** new_tables = push_array(database->arena, CQL_Table*, new_capacity);
    if (database->temp_table_count) MemoryCopy(new_tables, database->temp_tables, sizeof(CQL_Table*) * database->temp_table_count);
    database->temp_tables = new_tables;
    database->temp_table_capacity = new_capacity;
  }
  
  table->is_ephemeral = 1;
  database->temp_tables[database->temp_table_count++] = table;
}

internal void
cql_database_release_temp_tables_from(CQL_Database* database, U64 first_temp_index)
{
  while (database->temp_table_count > first_temp_index)
  {
    CQL_Table* table = database->temp_tables[--database->temp_table_count];
    for (U64 c = 0; c < table->column_count; c++)
    {
      cql_column_close(table->columns[c]);
      cql_column_release(table->columns[c]);
    }
    cql_table_release(table);
  }
}

internal CQL_Table*
cql_database_find_table_or_catalog(CQL_Database* database, String8 name)
{
  // tec: newest first, so nested temp tables and CTEs shadow base tables
  for (U64 i = database->temp_table_count; i > 0; i--)
  {
    if (str8_match(database->temp_tables[i - 1]->name, name, 0))
    {
      return database->temp_tables[i - 1];
    }
  }
  
  if (str8_match(name, CQL_COLUMN_CATALOG_TABLE_NAME, StringMatchFlag_CaseInsensitive))
  {
    return cql_database_build_column_catalog(database);
  }
  return cql_database_find_table(database, name);
}

internal CQL_ColumnType
cql_infer_column_type(String8 value)
{
  if (!value.str || value.size == 0 || str8_match(value, str8_lit("NULL"), StringMatchFlag_CaseInsensitive))
  {
    //log_error("could not infer column type, string is invalid");
    return CQL_ColumnType_Invalid;
  }
  
  B32 is_numeric = str8_is_numeric(value);
  if (is_numeric)
  {
    if (!str8_contains(value, '.'))
    {
      U64 u64_value = u64_from_str8(value, 10);
      if (u64_value <= max_U32)
      {
        return CQL_ColumnType_U32;
      }
      
      if (u64_value > max_U32)
      {
        return CQL_ColumnType_U64;
      }
    }
    
    F64 f64_value = f64_from_str8(value);
    if (f64_value == (F32)f64_value)
    {
      return CQL_ColumnType_F32;
    }
    return CQL_ColumnType_F64;
  }
  return CQL_ColumnType_String8;
}

internal CQL_ColumnType
cql_promote_type(CQL_ColumnType existing, CQL_ColumnType new_type)
{
  if (existing == CQL_ColumnType_Invalid) return new_type;
  if (new_type == CQL_ColumnType_Invalid) return existing;
  
  if (existing == new_type) return existing;
  
  if ((existing == CQL_ColumnType_U32 && new_type == CQL_ColumnType_U64) ||
      (existing == CQL_ColumnType_U64 && new_type == CQL_ColumnType_U32))
  {
    return CQL_ColumnType_U64;
  }
  
  if ((existing == CQL_ColumnType_F32 && new_type == CQL_ColumnType_F64) ||
      (existing == CQL_ColumnType_F64 && new_type == CQL_ColumnType_F32))
  {
    return CQL_ColumnType_F64;
  }
  
  return CQL_ColumnType_String8;
}