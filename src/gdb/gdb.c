internal void
gdb_init(void)
{
  ProfBeginFunction();
  
  Arena* arena = arena_alloc(.reserve_size=GDB_STATE_ARENA_RESERVE_SIZE, .commit_size=GDB_STATE_ARENA_COMMIT_SIZE);
  g_gdb_state = push_array(arena, GDB_State, 1);
  g_gdb_state->arena = arena;
  
  g_gdb_state->databases = NULL;
  g_gdb_state->rw_mutex = os_rw_mutex_alloc();
  
  ProfEnd();
}

internal void
gdb_release(void)
{
  os_mutex_release(g_gdb_state->rw_mutex);
  arena_release(g_gdb_state->arena);
}

internal void
gdb_add_database(GDB_Database* database)
{
  ProfBeginFunction();
  
  if (g_gdb_state->database_count == 0)
  {
    g_gdb_state->databases = push_array(g_gdb_state->arena, GDB_Database*, 2);
    g_gdb_state->database_capacity = 2;
  }
  else if (g_gdb_state->database_count >= g_gdb_state->database_capacity)
  {
    U64 new_capacity = g_gdb_state->database_capacity * 2;
    GDB_Database** new_databases = push_array(g_gdb_state->arena, GDB_Database*, new_capacity);
    MemoryCopy(new_databases, g_gdb_state->databases, sizeof(GDB_Database*) * g_gdb_state->database_count);
    g_gdb_state->databases = new_databases;
    g_gdb_state->database_capacity = new_capacity;
  }
  
  g_gdb_state->databases[g_gdb_state->database_count++] = database;
  ProfEnd();
}

//~ tec: database
internal GDB_Database*
gdb_database_alloc(String8 name)
{
  Arena* arena = arena_alloc(.reserve_size=GDB_DATABASE_ARENA_RESERVE_SIZE, .commit_size=GDB_DATABASE_ARENA_COMMIT_SIZE);
  GDB_Database* database = push_array(arena, GDB_Database, 1);
  
  database->name = name;
  database->tables = NULL;
  database->arena = arena;
  
  return database;
}

internal void
gdb_database_release(GDB_Database* database)
{
  arena_release(database->arena);
}

internal void
gdb_database_add_table(GDB_Database* database, GDB_Table* table)
{
  if (database->table_count == 0)
  {
    database->tables = push_array(database->arena, GDB_Table*, 2);
    database->table_capacity = 2;
  }
  else if (database->table_count >= database->table_capacity)
  {
    U64 new_capacity = database->table_capacity * 2;
    GDB_Table** new_tables = push_array(database->arena, GDB_Table*, new_capacity);
    MemoryCopy(new_tables, database->tables, sizeof(GDB_Table*) * database->table_count);
    database->tables = new_tables;
    database->table_capacity = new_capacity;
  }
  
  table->parent_database = database;
  database->tables[database->table_count++] = table;
}

internal void
gdb_database_replace_table(GDB_Database* database, GDB_Table* new_table)
{
  for (U64 i = 0; i < database->table_count; i++)
  {
    GDB_Table* old_table = database->tables[i];
    if (str8_match(old_table->name, new_table->name, 0))
    {
      for (U64 c = 0; c < old_table->column_count; c++)
      {
        gdb_column_close(old_table->columns[c]);
      }
      gdb_table_release(old_table);

      new_table->parent_database = database;
      database->tables[i] = new_table;
      return;
    }
  }

  gdb_database_add_table(database, new_table);
}

global String8 g_gdb_database_save_path = str8_lit_comp("gdb_data/");

internal B32
gdb_database_save(GDB_Database* database, String8 directory)
{
  ProfBeginFunction();
  
  Temp scratch = scratch_begin(0, 0);
  
  if (!os_make_directory(directory))
  {
    log_error("failed to create/open database directory: %s", directory.str);
    return 0;
  }
  
  for (U64 i = 0; i < database->table_count; i++)
  {
    GDB_Table* table = database->tables[i];
    String8 table_dir = push_str8f(scratch.arena, "%.*s/%.*s", str8_varg(directory), str8_varg(table->name));
    
    if (!os_make_directory(table_dir))
    {
      log_error("failed to create/open table directory: %.*s", str8_varg(table_dir));
      ProfEnd();
      return 0;
    }
    
    if (!gdb_table_save(table, table_dir))
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

internal GDB_Database*
gdb_database_load(String8 directory_path)
{
  ProfBeginFunction();
  
  GDB_Database* database = gdb_database_alloc(str8_lit("temp"));
  Temp scratch = scratch_begin(0, 0);
  
  // tec: check if the last character is a slash
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
  
  OS_FileIter* it = os_file_iter_begin(scratch.arena, directory_path, OS_FileIterFlag_SkipFiles);
  U64 idx = 0;
  for(OS_FileInfo info = {0}; idx < 16384 && os_file_iter_next(scratch.arena, it, &info); idx += 1)
  {
    String8 table_dir = push_str8_cat(scratch.arena, directory_path, info.name);
    String8 meta_path = push_str8f(scratch.arena, "%.*s/%.*s.meta", str8_varg(table_dir), str8_varg(info.name));
    GDB_Table* table = gdb_table_load(table_dir, meta_path);
    if (table)
    {
      gdb_database_add_table(database, table);
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
gdb_database_close(GDB_Database* database)
{
  
}

internal GDB_Table* 
gdb_database_find_table(GDB_Database* database, String8 table_name)
{
  ProfBeginFunction();
  for (U64 i = 0; i < database->table_count; i++)
  {
    GDB_Table* table = database->tables[i];
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
gdb_database_contains_table(GDB_Database* database, String8 table_name)
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
internal GDB_Table*
gdb_table_alloc(String8 name)
{
  Arena* arena = arena_alloc(.reserve_size=GDB_TABLE_ARENA_RESERVE_SIZE, .commit_size=GDB_TABLE_ARENA_COMMIT_SIZE);
  GDB_Table* table = push_array(arena, GDB_Table, 1);
  
  table->name = name;
  table->arena = arena;
  
  return table;
}

internal void
gdb_table_release(GDB_Table* table)
{
  arena_release(table->arena);
}

internal void
gdb_table_add_column(GDB_Table* table, GDB_ColumnSchema schema)
{
  ProfBeginFunction();
  
  if (table->column_count == 0)
  {
    table->columns = push_array(table->arena, GDB_Column*, 2);
    table->column_capacity = 2;
  }
  else if (table->column_count >= table->column_capacity)
  {
    U64 new_capacity = (U64)ceil_f64(table->column_capacity * GDB_TABLE_EXPAND_FACTOR);
    GDB_Column** new_columns = push_array(table->arena, GDB_Column*, new_capacity);
    MemoryCopy(new_columns, table->columns, sizeof(GDB_Column*) * table->column_count);
    table->columns = new_columns;
    table->column_capacity = new_capacity;
  }
  
  // tec: allocate and add the new column
  GDB_Column* column = gdb_column_alloc(schema.name, schema.type, schema.size);
  column->parent_table = table;
  table->columns[table->column_count++] = column;
  
  ProfEnd();
}

internal void
gdb_table_add_row(GDB_Table* table, void** row_data)
{
  for (U64 i = 0; i < table->column_count; ++i)
  {
    gdb_column_add_data(table->columns[i], row_data[i]);
  }
  table->row_count++;
}

internal void
gdb_table_remove_row(GDB_Table* table, U64 row_index)
{
  if (row_index >= table->row_count)
  {
    log_error("row index out of bounds: %llu", row_index);
    return;
  }
  
  for (U64 i = 0; i < table->column_count; ++i)
  {
    gdb_column_remove_data(table->columns[i], row_index);
  }
  
  table->row_count--;
}

internal B32
gdb_table_save(GDB_Table* table, String8 table_dir)
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
      GDB_Column* column = table->columns[i];
      meta_size += sizeof(GDB_ColumnType) + (sizeof(U64) * 3) + column->name.size;
    }
    
    U8* meta_buffer = push_array(scratch.arena, U8, meta_size);
    U8* meta_ptr = meta_buffer;
    
    *(U64*)meta_ptr = table->column_count; meta_ptr += sizeof(U64);
    *(U64*)meta_ptr = table->row_count; meta_ptr += sizeof(U64);
    
    for (U64 i = 0; i < table->column_count; i++)
    {
      GDB_Column* column = table->columns[i];
      
      *(GDB_ColumnType*)meta_ptr = column->type; meta_ptr += sizeof(GDB_ColumnType);
      *(U64*)meta_ptr = column->size; meta_ptr += sizeof(U64);
      *(U64*)meta_ptr = column->capacity; meta_ptr += sizeof(U64);
      *(U64*)meta_ptr = column->name.size; meta_ptr += sizeof(U64);
      MemoryCopy(meta_ptr, column->name.str, column->name.size);
      meta_ptr += column->name.size;
    }
    
    os_file_write(meta_file, r1u64(0, meta_size), meta_buffer);
    os_file_close(meta_file);
  }
  scratch_end(scratch);
  
  //- tec: column files
  scratch = scratch_begin(0, 0);
  for (U64 i = 0; i < table->column_count; i++)
  {
    GDB_Column* column = table->columns[i];
    
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
      
      if (column->type == GDB_ColumnType_String8)
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
  }
  scratch_end(scratch);
  
  ProfEnd();
  return 1;
}

internal B32
gdb_table_export_csv(GDB_Table* table, String8 path)
{
  ProfBeginFunction();
  
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if (os_handle_match(os_handle_zero(), file))
  {
    log_error("failed to open CSV file for writing: %s", path.str);
    return 0;
  }
  
  Temp scratch = temp_begin(g_gdb_state->arena);
  U64 chunk_cap = KB(8);
  String8 chunk = str8_from_memory_size(scratch.arena, chunk_cap);
  chunk.size = 0;
  
  // Save the original arena position
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
      GDB_Column* column = table->columns[col];
      String8 output = {0};
      
      if (column->type == GDB_ColumnType_U64)
      {
        U64* data = (U64*)gdb_column_get_data(column, row);
        output = str8_from_u64(scratch.arena, *data, 10, 0, 0);
      }
      else if (column->type == GDB_ColumnType_U32)
      {
        U32* data = (U32*)gdb_column_get_data(column, row);
        output = str8_from_u64(scratch.arena, *data, 10, 0, 0);
      }
      else if (column->type == GDB_ColumnType_F64)
      {
        F64* data = (F64*)gdb_column_get_data(column, row);
        output = str8_from_f64(scratch.arena, *data, 6);
      }
      else if (column->type == GDB_ColumnType_F32)
      {
        F32* data = (F32*)gdb_column_get_data(column, row);
        output = str8_from_f64(scratch.arena, *data, 6);
      }
      else if (column->type == GDB_ColumnType_String8)
      {
        output = gdb_column_get_string(scratch.arena, column, row);
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

internal GDB_Table*
gdb_table_load(String8 table_dir, String8 meta_path)
{
  ProfBeginFunction();
  
  GDB_Table* table = gdb_table_alloc(str8_lit("temp"));
  
  Temp scratch = temp_begin(g_gdb_state->arena);
  String8 meta_data = os_data_from_file_path(scratch.arena, meta_path);
  if (meta_data.size == 0)
  {
    log_error("Failed to read metadata: %.*s", str8_varg(meta_path));
    return NULL;
  }
  
  U8* read_ptr = meta_data.str;
  
  table->column_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
  table->row_count = *(U64*)read_ptr; read_ptr += sizeof(U64);
  
  table->columns = push_array(table->arena, GDB_Column*, table->column_count);
  
  for (U64 i = 0; i < table->column_count; i++)
  {
    GDB_Column* column = gdb_column_alloc(str8_lit("temp"), 0, 0);
    column->row_count = table->row_count;
    
    // tec: bind into the table immediately (rather than at the end of this loop body) so that
    // early 'continue'/'break' paths below (e.g. a freshly-created 0-row column whose .dat file
    // is legitimately empty) still leave table->columns[i] populated instead of NULL - a NULL
    // column here previously crashed any later INSERT into a table that had been saved with 0 rows.
    table->columns[i] = column;
    column->parent_table = table;
    
    column->type = *(GDB_ColumnType*)read_ptr; read_ptr += sizeof(GDB_ColumnType);
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
    
    if (props.size > GDB_DISK_BACKED_THRESHOLD_SIZE)
    {
      column->is_disk_backed = 1;
      column->disk_path = push_str8_copy(column->arena, column_path);
      if (column->type == GDB_ColumnType_String8)
      {
        OS_Handle map = os_file_map_open(OS_AccessFlag_Read, file);
        void* mapped_ptr = os_file_map_view_open(map, OS_AccessFlag_Read, r1u64(0, sizeof(U64)));
        column->variable_capacity = *(U64*)mapped_ptr;
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
      
      if (column->type == GDB_ColumnType_String8)
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
  while (i < len && count < max_fields)
  {
    B32 in_quote = 0;
    U64 start = i;
    if (input[i] == '"') { in_quote = 1; i++; start++; }
    
    U64 field_len = 0;
    for (; i < len; i++)
    {
      if (in_quote)
      {
        if (input[i] == '"' && (i + 1 == len || input[i + 1] == ',' || input[i + 1] == '\n'))
        {
          i++;
          break;
        }
        else if (input[i] == '"' && input[i + 1] == '"')
        {
          i++; // skip escaped quote
        }
      }
      else if (input[i] == ',') break;
    }
    
    U64 end = i;
    if (in_quote && end > start && input[end - 1] == '"') end--;
    fields[count++] = str8(input + start, end - start);
    
    // skip delimiter
    if (i < len && input[i] == ',') i++;
  }
  
  return count;
}

// tec: TODO make this function faster with multithreading
internal GDB_Table*
gdb_table_import_csv_streaming(GDB_Database *db, String8 table_name, String8 path)
{
  ProfBeginFunction();
  
  Temp scratch = temp_begin(g_gdb_state->arena);
  
  GDB_Table *table = gdb_table_alloc(table_name);
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
  
  GDB_ColumnType *types = 0;
  String8 *column_names = 0;
  U64 column_count = 0;
  
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
      while (at < chunk.size)
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
          types = push_array(scratch.arena, GDB_ColumnType, column_count);
          MemorySet(types, GDB_ColumnType_Invalid, column_count * sizeof(*types));
        }
        else
        {
          String8List values = str8_split_by_string_chars(parse_arena, line, str8_lit(","), StringSplitFlag_RespectQuotes | StringSplitFlag_KeepEmpties);
          U64 col_i = 0;
          for (String8Node *node = values.first; node && col_i < column_count; node = node->next, col_i++)
          {
            GDB_ColumnType type = gdb_infer_column_type(node->string);
            types[col_i] = gdb_promote_type(types[col_i], type);
          }
        }
        
        sample_rows++;
      }
    }
    
    for (U64 i = 0; i < column_count; i++)
    {
      if (types[i] == GDB_ColumnType_Invalid)
        types[i] = GDB_ColumnType_String8;
      
      GDB_ColumnSchema schema = gdb_column_schema_create(column_names[i], types[i]);
      gdb_table_add_column(table, schema);
      //log_info("column type: %.*s", str8_varg(string_from_gdb_column_type(types[i])));
    }
  }
  ProfEnd();
  
  {
    Arena *row_arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(32));
    U64 file_pos = 0;
    U8 *buffer = push_array(scratch.arena, U8, buffer_size);
    
    String8 leftover = {0};
    B32 skipped_header = 0;
    
    while (file_pos < file_size)
    {
      U64 read_size = Min(buffer_size, file_size - file_pos);
      os_file_read(file, r1u64(file_pos, file_pos + read_size), buffer);
      file_pos += read_size;
      
      String8 chunk = str8(buffer, read_size);
      if (leftover.size)
      {
        chunk = push_str8_cat(row_arena, leftover, chunk);
        leftover = (String8){0};
      }
      
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
        
        table->row_count++;
        if ((table->row_count % 1000000) == 0)
        {
          log_info("processing row %llu", table->row_count);
        }
        
        String8* values = push_array(row_arena, String8, column_count);
        U64 value_count = parse_csv_line(line.str, line.size, values, column_count);
        
        for (U64 col_i = 0; col_i < value_count; col_i++)
        {
          /*
          String8List values = str8_split_by_string_chars(row_arena, line, str8_lit(","), StringSplitFlag_RespectQuotes | StringSplitFlag_KeepEmpties);
          
          U64 col_i = 0;
          for (String8Node *node = values.first; node && col_i < column_count; node = node->next, col_i++)
          {
          String8 val = str8_skip_chop_whitespace(node->string);
            */
          String8 val = str8_skip_chop_whitespace(values[col_i]);
          GDB_Column *column = table->columns[col_i];
          
          if (val.size == 0)
          {
            gdb_column_add_data(column, NULL);
          }
          else
          {
            switch (column->type)
            {
              case GDB_ColumnType_U32:
              {
                U32 value = (U32)u64_from_str8(val, 10);
                gdb_column_add_data(column, &value);
              } break;
              
              case GDB_ColumnType_U64:
              {
                U64 value = u64_from_str8(val, 10);
                gdb_column_add_data(column, &value);
              } break;
              
              case GDB_ColumnType_F32:
              {
                F32 value = (F32)f64_from_str8(val);
                gdb_column_add_data(column, &value);
              } break;
              
              case GDB_ColumnType_F64:
              {
                F64 value = f64_from_str8(val);
                gdb_column_add_data(column, &value);
              } break;
              
              case GDB_ColumnType_String8:
              default:
              {
                gdb_column_add_data(column, &val);
              } break;
            }
          }
        }
        
        arena_clear(row_arena);
      }
    }
    
    // tec: process leftover if it's a valid line
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
          GDB_Column *column = table->columns[col_i];
          String8 val = str8_skip_chop_whitespace(node->string);
          
          if (val.size == 0)
          {
            gdb_column_add_data(column, NULL);
          }
          else
          {
            switch (column->type)
            {
              case GDB_ColumnType_U32: { U32 v = (U32)u64_from_str8(val, 10); gdb_column_add_data(column, &v); } break;
              case GDB_ColumnType_U64: { U64 v = u64_from_str8(val, 10); gdb_column_add_data(column, &v); } break;
              case GDB_ColumnType_F32: { F32 v = (F32)f64_from_str8(val); gdb_column_add_data(column, &v); } break;
              case GDB_ColumnType_F64: { F64 v = f64_from_str8(val); gdb_column_add_data(column, &v); } break;
              case GDB_ColumnType_String8:
              default: { gdb_column_add_data(column, &val); } break;
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
  ProfEnd();
  return table;
}

internal GDB_Column*
gdb_table_find_column(GDB_Table* table, String8 column_name)
{
  for (U64 i = 0; i < table->column_count; i++)
  {
    GDB_Column* column = table->columns[i];
    if (str8_match(column->name, column_name, 0))
    {
      return column;
    }
  }
  log_error("failed to find column '%.*s' in table '%.*s", column_name.size, column_name.str,
            table->name.size, table->name.str);
  return NULL;
}

//~ tec: column
internal GDB_Column*
gdb_column_alloc(String8 name, GDB_ColumnType type, U64 size)
{
  Arena* arena = arena_alloc(.reserve_size=GDB_COLUMN_ARENA_RESERVE_SIZE, .commit_size=GDB_COLUMN_ARENA_COMMIT_SIZE);
  GDB_Column* column = push_array(arena, GDB_Column, 1);
  
  column->name = name;
  column->type = type;
  column->size = size;
  column->arena = arena;
  
  return column;
}

internal void
gdb_column_release(GDB_Column* column)
{
  arena_release(column->arena);
}

internal void
gdb_column_open(GDB_Column* column)
{
  if (!column->is_disk_backed)
  {
    gdb_column_convert_to_disk_backed(column);
  }
}

internal void
gdb_column_close(GDB_Column* column)
{
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
gdb_column_add_data_disk_backed(GDB_Column* column, void* data)
{
  if (column->type == GDB_ColumnType_String8)
  {
    String8* str = (String8*)data;
    OS_Handle file = column->file;
    if (os_handle_match(os_handle_zero(), file))
    {
      // tec: cached in column->file for the column's whole lifetime
      file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_Write | OS_AccessFlag_Append | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column->disk_path);
      column->file = file;
    }

    U64 var_reserved = 0;
    os_file_read(file, r1u64(0, sizeof(U64)), &var_reserved);
    
    U64 offset_array_offset = sizeof(U64) + var_reserved;
    U64 offset_count = column->row_count;
    U64 total_offsets_size = (offset_count + 1) * sizeof(U64);
    
    if (offset_count == 0)
    {
      U64 zero = 0;
      os_file_write(file, r1u64(offset_array_offset, offset_array_offset + sizeof(U64)), &zero);
    }
    
    B32 needs_growth = (column->variable_capacity + str->size > var_reserved);
    if (needs_growth)
    {
      ProfBegin("gdb_column_add_data_disk_backed growth");
      U64 new_reserved = var_reserved * 2;
      if (new_reserved < column->variable_capacity + str->size)
      {
        new_reserved = AlignUp(column->variable_capacity + str->size + GDB_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE, 8);
      }
      
      U64 old_offset_pos = sizeof(U64) + var_reserved;
      U64 new_offset_pos = sizeof(U64) + new_reserved;
      
      Temp scratch = temp_begin(g_gdb_state->arena);
      void *buffer = push_array(scratch.arena, U8, total_offsets_size);
      
      os_file_read(file, r1u64(old_offset_pos, old_offset_pos + total_offsets_size), buffer);
      os_file_write(file, r1u64(new_offset_pos, new_offset_pos + total_offsets_size), buffer);
      
      os_file_write(file, r1u64(0, sizeof(U64)), &new_reserved);
      var_reserved = new_reserved;
      offset_array_offset = sizeof(U64) + var_reserved;
      
      U64 new_size = offset_array_offset + total_offsets_size;
      os_file_resize(file, new_size);
      
      U64 old_offset_array_size = total_offsets_size;
      void *zero_buf = push_array(scratch.arena, U8, old_offset_array_size);
      MemoryZero(zero_buf, old_offset_array_size);
      os_file_write(file, r1u64(old_offset_pos, old_offset_pos + old_offset_array_size), zero_buf);
      
      temp_end(scratch);
      ProfEnd();
    }
    U64 string_offset = column->variable_capacity;
    os_file_write(file, r1u64(sizeof(U64) + string_offset, sizeof(U64) + string_offset + str->size), str->str);
    
    U64 new_end_offset = string_offset + str->size;
    os_file_write(file,
                  r1u64(offset_array_offset + (offset_count + 0) * sizeof(U64),
                        offset_array_offset + (offset_count + 1) * sizeof(U64)),
                  &new_end_offset);
    
    column->variable_capacity += str->size;
  }
  else
  {
    OS_Handle file = column->file;
    if (os_handle_match(os_handle_zero(), file))
    {
      file = os_file_open(OS_AccessFlag_Write | OS_AccessFlag_Append, column->disk_path);
    }
    U64 offset = column->row_count * column->size;
    os_file_write(file, r1u64(offset, offset + column->size), data);
    
    if (os_handle_match(os_handle_zero(), column->file))
    {
      os_file_close(file);
    }
  }
}

internal void
gdb_column_add_data(GDB_Column* column, void* data)
{
  if (column->type == GDB_ColumnType_String8)
  {
    String8* str = (String8*)data;
    String8 empty_str = str8_lit("");
    if (str == NULL)
    {
      str = &empty_str;
    }
    
    if (column->is_disk_backed)
    {
      gdb_column_add_data_disk_backed(column, data);
    }
    else
    {
      //- tec: grow offsets array if needed
      if (column->row_count == column->capacity)
      {
        U64 new_capacity = (column->capacity > 0) ? column->capacity * 2 : GDB_COLUMN_EXPAND_COUNT;
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
        U64 new_variable_capacity = (column->variable_capacity > 0) ? column->variable_capacity * 2 : GDB_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE;
        while (new_variable_capacity < required_size)
        {
          new_variable_capacity *= 2;
        }
        
        if (new_variable_capacity > GDB_DISK_BACKED_THRESHOLD_SIZE)
        {
          if (!column->is_disk_backed)
          {
            gdb_column_convert_to_disk_backed(column);
            gdb_column_add_data(column, data);
            //gdb_column_add_data_disk_backed(column, data);
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
      gdb_column_add_data_disk_backed(column, data);
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
        U64 new_capacity = (column->capacity > 0) ? column->capacity * 2 : GDB_COLUMN_EXPAND_COUNT;
        if (new_capacity > column->capacity + GDB_COLUMN_MAX_GROW_BY_SIZE)
        {
          new_capacity = column->capacity + GDB_COLUMN_MAX_GROW_BY_SIZE;
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
      
      // tec: add data
      MemoryCopy(column->data + column->row_count * column->size, data, column->size);
      
      if ((column->row_count + 1) * column->size > GDB_DISK_BACKED_THRESHOLD_SIZE)
      {
        gdb_column_convert_to_disk_backed(column);
      }
    }
  }
  column->row_count++;
}

internal void
gdb_column_remove_data(GDB_Column* column, U64 row_index)
{
  if (row_index >= column->row_count)
  {
    log_error("column row index out of bounds: %llu", row_index);
    return;
  }
  
  if (column->is_disk_backed)
  {
    log_error("removing data from disk-backed column is not supported");
    return;
  }
  
  if (column->type == GDB_ColumnType_String8)
  {
    U64 start_offset = column->offsets[row_index];
    U64 end_offset = column->offsets[row_index + 1];
    U64 size_to_move = column->variable_capacity - end_offset;
    
    MemoryCopy(column->data + start_offset, column->data + end_offset, size_to_move);
    
    for (U64 i = row_index + 1; i < column->row_count; ++i)
    {
      column->offsets[i] = column->offsets[i + 1] - (end_offset - start_offset);
    }
  }
  else
  {
    U64 size_to_move = (column->row_count - row_index - 1) * column->size;
    MemoryCopy(column->data + row_index * column->size, column->data + (row_index + 1) * column->size, size_to_move);
  }
  
  column->row_count--;
}

internal void*
gdb_column_get_data(GDB_Column* column, U64 index)
{
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
gdb_column_get_string(Arena *arena, GDB_Column *column, U64 index)
{
  String8 result = {0};
  
  if (index >= column->row_count || column->type != GDB_ColumnType_String8)
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
    
    U64 offset_base = var_reserved + sizeof(U64);
    U64 offset_pos = offset_base + (index * sizeof(U64));
    
    U64 start = 0, end = 0;
    
    os_file_read(file, r1u64(offset_pos - sizeof(U64), offset_pos), &start);
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
gdb_column_get_total_size(GDB_Column* column)
{
  U64 total_size = 0;
  
  if (column->is_disk_backed)
  {
    FileProperties props = os_properties_from_file_path(column->disk_path);
    total_size = props.size;
    
    // Extra sanity check for string columns
    if (column->type == GDB_ColumnType_String8 && column->row_count > 0)
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
    if (column->type == GDB_ColumnType_String8)
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
gdb_column_get_data_range(Arena* arena, GDB_Column* column, Rng1U64 row_range, U64* out_size)
{
  ProfBeginFunction();
  
  if (column->type == GDB_ColumnType_String8)
  {
    log_error("gdb_column_get_data_range was called, but column type is string. Use gdb_column_get_string_chunk instead.");
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
  
  // Direct in-memory column
  if (!column->is_disk_backed)
  {
    void* data_ptr = column->data + (row_range.min * column->size);
    ProfEnd();
    return data_ptr;
  }
  
  // tec: create io handles
  if (os_handle_match(os_handle_zero(), column->file))
  {
    column->file = os_file_open(OS_AccessFlag_Read, column->disk_path);
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
  
  ProfEnd();
  return mapped_ptr;
}


internal GDB_StringDataChunk 
gdb_column_get_string_chunk(Arena* arena, GDB_Column* column, Rng1U64 row_range)
{
  ProfBeginFunction();
  
  GDB_StringDataChunk result = {0};
  
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
    
    // tec: on-disk string column layout (see gdb_table_save):
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
gdb_column_close_string_chunk(GDB_Column* column)
{
  OS_Handle file_map = column->file_map;
  os_file_map_view_close(file_map, column->mapped_ptr, column->current_mapped_range);
}

internal String8
gdb_generate_disk_path_for_column(Arena* arena, GDB_Column* column)
{
  GDB_Table* table = column->parent_table;
  GDB_Database* database = table->parent_database;
  
  // tec: check for valid paths
  {
    Temp scratch = scratch_begin(0, 0);
    
    String8 database_path = push_str8f(arena, "gdb_data/%.*s/", str8_varg(database->name));
    
    if (!os_file_path_exists(database_path))
    {
      os_make_directory(database_path);
    }
    
    String8 table_path = push_str8f(arena, "gdb_data/%.*s/%.*s/", str8_varg(database->name), str8_varg(table->name));
    if (!os_file_path_exists(table_path))
    {
      os_make_directory(table_path);
    }
    
    scratch_end(scratch);
  }
  
  String8 column_path = push_str8f(arena, "gdb_data/%.*s/%.*s/%.*s.dat", str8_varg(database->name), str8_varg(table->name), str8_varg(column->name));
  return column_path;
}

internal void
gdb_column_convert_to_disk_backed(GDB_Column* column)
{
  ProfBeginFunction();
  
  Temp scratch = scratch_begin(0, 0);
  String8 column_path = gdb_generate_disk_path_for_column(scratch.arena, column);
  // tec: this handle is cached in column->file and kept open for the column's whole lifetime
  OS_Handle file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_Write | OS_AccessFlag_Append | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column_path);
  
  if (column->type == GDB_ColumnType_String8)
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

//~ tec: utils
internal GDB_ColumnType
gdb_column_type_from_string(String8 str)
{
  if (str8_match(str, str8_lit("u32"), StringMatchFlag_CaseInsensitive))
  {
    return GDB_ColumnType_U32;
  }
  else if (str8_match(str, str8_lit("u64"), StringMatchFlag_CaseInsensitive))
  {
    return GDB_ColumnType_U64;
  }
  else if (str8_match(str, str8_lit("f32"), StringMatchFlag_CaseInsensitive))
  {
    return GDB_ColumnType_F32;
  }
  else if (str8_match(str, str8_lit("f64"), StringMatchFlag_CaseInsensitive))
  {
    return GDB_ColumnType_F64;
  }
  else if (str8_match(str, str8_lit("string8"), StringMatchFlag_CaseInsensitive))
  {
    return GDB_ColumnType_String8;
  }
  
  log_error("failed to find matching GDB_ColumnType for '%.*s'", str8_varg(str));
  return GDB_ColumnType_U64;
}

internal String8
string_from_gdb_column_type(GDB_ColumnType type)
{
  String8 result = str8_lit("");
  switch (type)
  {
    case GDB_ColumnType_U32: { result = str8_lit("GDB_ColumnType_U32"); } break;
    case GDB_ColumnType_U64: { result = str8_lit("GDB_ColumnType_U64"); } break;
    case GDB_ColumnType_F32: { result = str8_lit("GDB_ColumnType_F32"); } break;
    case GDB_ColumnType_F64: { result = str8_lit("GDB_ColumnType_F64"); } break;
    case GDB_ColumnType_String8: { result = str8_lit("GDB_ColumnType_String8"); } break;
  }
  return result;
}

internal GDB_ColumnSchema
gdb_column_schema_create(String8 name, GDB_ColumnType type)
{
  GDB_ColumnSchema schema = (GDB_ColumnSchema){ name, type, g_gdb_column_type_size[type] };
  return schema;
}

internal GDB_ColumnType
gdb_infer_column_type(String8 value)
{
  if (!value.str || value.size == 0 || str8_match(value, str8_lit("NULL"), StringMatchFlag_CaseInsensitive))
  {
    //log_error("could not infer column type, string is invalid");
    return GDB_ColumnType_Invalid;
  }
  
  B32 is_numeric = str8_is_numeric(value);
  if (is_numeric)
  {
    if (!str8_contains(value, '.'))
    {
      U64 u64_value = u64_from_str8(value, 10);
      if (u64_value <= max_U32)
      {
        return GDB_ColumnType_U32;
      }
      
      if (u64_value > max_U32)
      {
        return GDB_ColumnType_U64;
      }
    }
    
    F64 f64_value = f64_from_str8(value);
    if (f64_value == (F32)f64_value)
    {
      return GDB_ColumnType_F32;
    }
    return GDB_ColumnType_F64;
  }
  return GDB_ColumnType_String8;
}

internal GDB_ColumnType
gdb_promote_type(GDB_ColumnType existing, GDB_ColumnType new_type)
{
  if (existing == GDB_ColumnType_Invalid) return new_type;
  if (new_type == GDB_ColumnType_Invalid) return existing;
  
  if (existing == new_type) return existing;
  
  if ((existing == GDB_ColumnType_U32 && new_type == GDB_ColumnType_U64) ||
      (existing == GDB_ColumnType_U64 && new_type == GDB_ColumnType_U32))
  {
    return GDB_ColumnType_U64;
  }
  
  if ((existing == GDB_ColumnType_F32 && new_type == GDB_ColumnType_F64) ||
      (existing == GDB_ColumnType_F64 && new_type == GDB_ColumnType_F32))
  {
    return GDB_ColumnType_F64;
  }
  
  return GDB_ColumnType_String8;
}