
internal TP_Context*
app_thread_pool(void)
{
  if (!g_app_thread_pool)
  {
    Arena* arena = arena_alloc();
    // tec: 0 (default) means auto-detect from the logical processor count
    U32 worker_count = (U32)settings_u64(str8_lit("APP_THREAD_POOL_WORKER_COUNT"), 0);
    if (worker_count == 0)
    {
      worker_count = Max(1, os_get_system_info()->logical_processor_count);
    }
    g_app_thread_pool = tp_alloc(arena, worker_count, 0, str8_zero());
    g_app_thread_pool_arena = tp_arena_alloc(g_app_thread_pool);
  }
  return g_app_thread_pool;
}

internal TP_Arena*
app_thread_pool_arena(void)
{
  app_thread_pool();
  return g_app_thread_pool_arena;
}

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
    case GDB_ColumnType_Bool: return push_array(arena, U8, 1);
    case GDB_ColumnType_I32: return push_array(arena, S32, 1);
    case GDB_ColumnType_I64: return push_array(arena, S64, 1);
    case GDB_ColumnType_Date: return push_array(arena, S32, 1);
    case GDB_ColumnType_Timestamp: return push_array(arena, S64, 1);
    case GDB_ColumnType_Decimal: return push_array(arena, S64, 1);
    case GDB_ColumnType_Enum: return push_array(arena, U32, 1);
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
    case GDB_ColumnType_Bool: return *(U8*)candidate == *(U8*)existing_data;
    case GDB_ColumnType_I32: return *(S32*)candidate == *(S32*)existing_data;
    case GDB_ColumnType_I64: return *(S64*)candidate == *(S64*)existing_data;
    case GDB_ColumnType_Date: return *(S32*)candidate == *(S32*)existing_data;
    case GDB_ColumnType_Timestamp: return *(S64*)candidate == *(S64*)existing_data;
    case GDB_ColumnType_Decimal: return *(S64*)candidate == *(S64*)existing_data;
    case GDB_ColumnType_Enum: return *(U32*)candidate == *(U32*)existing_data;
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
    for (; slot < table->column_count; slot++)
    {
      if (table->columns[slot] == column) break;
    }
    if (slot >= table->column_count) return 0.0;
    
    if (row_null[slot]) 
    { 
      *out_is_null = 1;
      return 0.0; 
    }
    
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
      case GDB_ColumnType_Bool: return (F64)(*(U8*)row_data[slot]);
      case GDB_ColumnType_I32: return (F64)(*(S32*)row_data[slot]);
      case GDB_ColumnType_I64: return (F64)(*(S64*)row_data[slot]);
      case GDB_ColumnType_Date: return (F64)(*(S32*)row_data[slot]);
      case GDB_ColumnType_Timestamp: return (F64)(*(S64*)row_data[slot]);
      case GDB_ColumnType_Decimal: return (F64)(*(S64*)row_data[slot]);
      case GDB_ColumnType_Enum: return (F64)(*(U32*)row_data[slot]);
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
  
  F64 rv = 0.0;
  B32 right_resolved = 0;
  if (left->type == IR_NodeType_Column && (right->type == IR_NodeType_Literal || right->type == IR_NodeType_Numeric))
  {
    GDB_Column* column = gdb_table_find_column(table, left->value);
    if (column && right->type == IR_NodeType_Literal &&
        (column->type == GDB_ColumnType_Date || column->type == GDB_ColumnType_Timestamp))
    {
      right_resolved = 1;
      if (!qe_resolve_date_literal_value(column->type, right, &rv)) return 0; // unparsable literal never matches
    }
    else if (column && right->type == IR_NodeType_Literal && column->type == GDB_ColumnType_Enum)
    {
      right_resolved = 1;
      if (!qe_resolve_enum_literal_value(column, right, &rv)) return 0; // unparsable literal never matches
    }
    else if (column && right->type == IR_NodeType_Numeric && column->type == GDB_ColumnType_Decimal)
    {
      right_resolved = 1;
      if (!qe_resolve_decimal_literal_value(column, right, &rv)) return 0; // unparsable literal never matches
    }
  }
  if (!right_resolved)
  {
    rv = gdb_check_load_value(table, row_data, row_null, right, &rstr, &rs, &rnull);
  }
  
  // tec: three-valued logic. a NULL operand makes CHECK neither true nor false, so the row is rejected
  if (lnull || rnull) return 0;
  
  if (lstr || rstr)
  {
    if (str8_match(op, str8_lit("contains"), StringMatchFlag_CaseInsensitive)) 
      return qe_str8_contains(ls, rs)
      ;
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
          if (gdb_candidate_value_equals_row(arena, ref_column, row_data[i], r))
          { 
            found = 1; break;
          }
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

#define APP_EMIT(...) str8_list_push(arena, &out, push_str8f(arena, __VA_ARGS__))

internal String8
app_format_cell_text(Arena* arena, GDB_ColumnType type, F64 numeric_value, String8 string_value, U32 decimal_scale, GDB_EnumType* enum_type)
{
  switch (type)
  {
    case GDB_ColumnType_U32: return push_str8f(arena, "%u", (U32)numeric_value);
    case GDB_ColumnType_U64: return push_str8f(arena, "%llu", (U64)numeric_value);
    case GDB_ColumnType_F32: return push_str8f(arena, "%f", (F32)numeric_value);
    case GDB_ColumnType_F64: return push_str8f(arena, "%lf", numeric_value);
    case GDB_ColumnType_String8: return push_str8_copy(arena, string_value);
    case GDB_ColumnType_Bool: return (numeric_value != 0.0) ? str8_lit("true") : str8_lit("false");
    case GDB_ColumnType_I32: return push_str8f(arena, "%d", (S32)numeric_value);
    case GDB_ColumnType_I64: return push_str8f(arena, "%lld", (S64)numeric_value);
    case GDB_ColumnType_Date: return push_iso_date_string(arena, (S32)numeric_value);
    case GDB_ColumnType_Timestamp: return push_iso_timestamp_string(arena, (S64)numeric_value);
    case GDB_ColumnType_Decimal: return decimal_to_str8(arena, (S64)numeric_value, decimal_scale);
    case GDB_ColumnType_Enum:
    return enum_type ? push_str8_copy(arena, gdb_enum_type_label_from_code(enum_type, (U32)numeric_value)) : str8_lit("?");
    default: return str8_lit("UNKNOWN");
  }
}

internal THREAD_POOL_TASK_FUNC(app_select_format_task)
{
  APP_SelectFormatTask* task = (APP_SelectFormatTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  B32 capture_structured = task->capture_structured;
  APP_ResultSet* out_result_set = task->out_result_set;
  String8List local_out = {0};
  
  for (U64 i = range.min; i < range.max; i++)
  {
    U64 ci = 0;
    for (IR_Node* column_node = task->select_output_columns->first; column_node != NULL; column_node = column_node->next, ci++)
    {
      U64 cell_i = i * task->column_count + ci;
      
      if (!task->gathered[ci].resolved)
      {
        if (!capture_structured) { str8_list_push(arena, &local_out, str8_lit("? ")); }
        if (capture_structured) { out_result_set->cell_is_null[cell_i] = 1; }
        continue;
      }
      
      U64 row_index = task->rows->row_indices[task->gathered[ci].table_slot][i];
      if (row_index == PLAN_NULL_ROW ||
          (!task->gathered[ci].is_score && gdb_column_is_null(task->gathered[ci].column, row_index)))
      {
        if (!capture_structured) { str8_list_push(arena, &local_out, str8_lit("NULL ")); }
        if (capture_structured) { out_result_set->cell_is_null[cell_i] = 1; }
        continue;
      }
      
      switch (task->gathered[ci].type)
      {
        case GDB_ColumnType_U32:
        if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%u ", (U32)task->gathered[ci].numeric_values[i])); }
        if (capture_structured)
        {
          out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type);
          out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
        }
        break;
        case GDB_ColumnType_U64:
        if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%llu ", (U64)task->gathered[ci].numeric_values[i])); }
        if (capture_structured)
        {
          out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type);
          out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
        }
        break;
        case GDB_ColumnType_F32:
        if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%f ", (F32)task->gathered[ci].numeric_values[i])); }
        if (capture_structured)
        {
          out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type);
          out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
        }
        break;
        case GDB_ColumnType_F64:
        if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%lf ", task->gathered[ci].numeric_values[i])); }
        if (capture_structured)
        {
          U32 decimal_scale = task->gathered[ci].column ? task->gathered[ci].column->decimal_scale : 0;
          GDB_EnumType* enum_type = task->gathered[ci].column ? task->gathered[ci].column->enum_type : 0;
          out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, decimal_scale, enum_type);
          out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
        }
        break;
        case GDB_ColumnType_Bool:
        if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%s ", task->gathered[ci].numeric_values[i] != 0.0 ? "true" : "false")); }
        if (capture_structured)
        {
          out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type);
          out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
        }
        break;
        case GDB_ColumnType_I32:
        if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%d ", (S32)task->gathered[ci].numeric_values[i])); }
        if (capture_structured)
        {
          out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type);
          out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
        }
        break;
        case GDB_ColumnType_I64:
        if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%lld ", (S64)task->gathered[ci].numeric_values[i])); }
        if (capture_structured)
        {
          out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type);
          out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
        }
        break;
        case GDB_ColumnType_Date:
        case GDB_ColumnType_Timestamp:
        case GDB_ColumnType_Decimal:
        case GDB_ColumnType_Enum:
        {
          String8 formatted = app_format_cell_text(arena, task->gathered[ci].type, task->gathered[ci].numeric_values[i], (String8){0}, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type);
          if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%.*s ", str8_varg(formatted))); }
          if (capture_structured)
          {
            out_result_set->cell_text[cell_i] = formatted;
            out_result_set->cell_numeric[cell_i] = task->gathered[ci].numeric_values[i];
          }
        } break;
        case GDB_ColumnType_String8:
        {
          GDB_StringDataChunk* chunk = &task->gathered[ci].strings;
          U64 start = chunk->offsets[i];
          U64 end = chunk->offsets[i + 1];
          String8 str = str8((U8*)chunk->data + start, end - start);
          if (!capture_structured) { str8_list_push(arena, &local_out, push_str8f(arena, "%.*s ", str8_varg(str))); }
          if (capture_structured) { out_result_set->cell_text[cell_i] = app_format_cell_text(arena, task->gathered[ci].type, 0, str, task->gathered[ci].column->decimal_scale, task->gathered[ci].column->enum_type); }
        } break;
        default:
        if (!capture_structured) { str8_list_push(arena, &local_out, str8_lit("UNKNOWN ")); }
        break;
      }
    }
    if (!capture_structured) { str8_list_push(arena, &local_out, str8_lit("\n")); }
  }
  
  if (!capture_structured) { task->worker_lists[task_id] = local_out; }
}

internal void
app_select_format_dispatch(Arena* arena, String8List* out, IR_Node* select_output_columns, SelectColGather* gathered,
                           U64 column_count, PLAN_RowSet* rows, U64 result_count, B32 capture_structured, APP_ResultSet* out_result_set)
{
  Temp scratch = scratch_begin(&arena, 1);
  TP_Context* pool = app_thread_pool();
  U64 task_count = (result_count > 1) ? Min((U64)pool->worker_count, result_count) : 1;
  
  APP_SelectFormatTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, result_count, (U32)task_count);
  task.select_output_columns = select_output_columns;
  task.gathered = gathered;
  task.column_count = column_count;
  task.rows = rows;
  task.capture_structured = capture_structured;
  task.out_result_set = out_result_set;
  task.worker_lists = push_array(scratch.arena, String8List, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, app_select_format_task, &task);
  
  if (capture_structured)
  {
    // tec: copy cell_text into caller's arena
    for (U64 idx = 0; idx < column_count * Max(result_count, 1); idx++)
    {
      if (out_result_set->cell_text[idx].size)
      {
        out_result_set->cell_text[idx] = push_str8_copy(arena, out_result_set->cell_text[idx]);
      }
    }
  }
  else
  {
    for (U64 w = 0; w < task_count; w++)
    {
      String8 joined = str8_list_join(arena, &task.worker_lists[w], &(StringJoin){0});
      str8_list_push(arena, out, joined);
    }
  }
  tp_temp_end(temp);
  
  scratch_end(scratch);
}

internal APP_QueryResult
app_execute_query_capture(Arena* arena, String8 sql_query, GDB_Database** io_database, APP_ResultSet* out_result_set)
{
  ProfBeginFunction();
  
  if (out_result_set) { MemoryZeroStruct(out_result_set); }
  
  APP_QueryResult result = {0};
  String8List out = {0};
  
  SQL_TokenizeResult tokenize_result = sql_tokenize_from_text(arena, sql_query);
  SQL_Node* sql_root = sql_parse(arena, tokenize_result.tokens, tokenize_result.count, sql_query);
  
  if (sql_root == NULL || g_sql_parse_error.has_error)
  {
    log_error("failed to parse query, aborting");
    result.had_parse_error = 1;
    ProfEnd();
    return result;
  }
  
  IR_Query* ir_query = ir_generate_from_ast(arena, sql_root);
  //sql_tokens_print(tokenize_result);
  //sql_print_ast(sql_root);
  //ir_print_query(ir_query);
  
  
  GDB_Database* database = io_database ? *io_database : NULL;
  
  for (IR_Node* ir_execution_node = ir_query->execution_nodes; 
       ir_execution_node != NULL;
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
          database = gdb_state_find_database_by_name(use_ir_node->value);
          if (!database)
          {
            String8 database_path = push_str8f(arena, "gdb_data/%.*s", str8_varg(use_ir_node->value));
            database = gdb_database_load(database_path);
            gdb_add_database(database);
          }
        }
        
        if (io_database) { *io_database = database; }
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
            APP_EMIT("Table: %.*s (%llu row%s)\n\n", str8_varg(table->name), table->row_count,
                     table->row_count == 1 ? "" : "s");
            APP_EMIT("%-24s %-10s %-5s %-5s %s\n", "Column", "Type", "Null", "Key", "Extra");
            APP_EMIT("%-24s %-10s %-5s %-5s %s\n", "------", "----", "----", "---", "-----");
            
            for (U64 i = 0; i < table->column_count; i++)
            {
              GDB_Column* column = table->columns[i];
              
              String8 type_name = gdb_column_type_display_name(arena, column);
              
              String8 key = str8_lit("");
              if (column->is_primary_key) key = str8_lit("PRI");
              else if (column->is_unique) key = str8_lit("UNI");
              
              GDB_Index* index_on_column = gdb_table_find_index_on_column(table, column);
              
              Temp scratch = scratch_begin(&arena, 1);
              String8List extra_parts = {0};
              if (column->is_disk_backed)
              {
                str8_list_push(scratch.arena, &extra_parts, str8_lit("disk-backed"));
              }
              if (index_on_column) 
              {
                str8_list_push(scratch.arena, &extra_parts, 
                               push_str8f(scratch.arena, "indexed(%.*s)", str8_varg(index_on_column->name)));
              }
              if (column->has_foreign_key) 
              {
                str8_list_push(scratch.arena, &extra_parts, 
                               push_str8f(scratch.arena, "references %.*s(%.*s)", str8_varg(column->fk_ref_table_name), str8_varg(column->fk_ref_column_name)));
              }
              if (column->has_check) 
              {
                str8_list_push(scratch.arena, &extra_parts, 
                               push_str8f(scratch.arena, "check(%.*s)", str8_varg(column->check_text)));
              }
              String8 extra = str8_list_join(scratch.arena, &extra_parts, &(StringJoin){.sep = str8_lit(", ")});
              
              APP_EMIT("%-24.*s %-10.*s %-5s %-5.*s %.*s\n",
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
      
      case IR_NodeType_Explain:
      {
        IR_Node* select_ir_node = ir_node_find_child(ir_execution_node, IR_NodeType_Select);
        B32 is_analyze = str8_match(ir_execution_node->value, str8_lit("analyze"), 0);
        
        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
        }
        else if (!select_ir_node)
        {
          log_error("explain: only 'explain [analyze] select ...' is supported");
        }
        else
        {
          ir_expand_star_to_columns(arena, database, select_ir_node);
          
          PLAN_Node* plan = plan_build_from_select(arena, database, select_ir_node);
          
          if (is_analyze)
          {
            QE_TraceCtx* trace = qe_trace_ctx_alloc(arena);
            // tec: EXPLAIN ANALYZE reports plan+stats text
            PLAN_ExecResult result = plan_execute(arena, database, plan, select_ir_node, trace);
            APP_EMIT("Query plan (analyzed):\n");
            plan_print_analyzed(arena, &out, plan, trace, 0);
            if (!result.supported)
            {
              APP_EMIT("\n(note: query has no supported execution path - stats above reflect only the portion that ran)\n");
            }
            
            B32 any_warm_cache = 0;
            for (QE_NodeTrace* record = trace->records; record != 0 && !any_warm_cache; record = record->next)
            {
              if (record->node_type == PLAN_NodeType_Scan || record->node_type == PLAN_NodeType_Filter)
              {
                any_warm_cache = record->scan.gpu_cache_hit_count > 0;
              }
            }
            if (any_warm_cache)
            {
              APP_EMIT("\n(note: one or more scans reused a warm GPU-resident buffer from a prior query - "
                       "re-run after DDL/reload to see cold-cache timings)\n");
            }
          }
          else
          {
            APP_EMIT("Query plan:\n");
            plan_print(arena, &out, plan, 0);
          }
        }
      } break;
      
      case IR_NodeType_Create:
      {
        IR_Node* create_ir_node = ir_execution_node->first;
        
        if (create_ir_node->type == IR_NodeType_Database)
        {
          if (gdb_state_find_database_by_name(create_ir_node->value))
          {
            log_error("create database: '%.*s' is already loaded", str8_varg(create_ir_node->value));
          }
          else
          {
            database = gdb_database_alloc(create_ir_node->value);
            gdb_add_database(database);
            
            if (io_database) { *io_database = database; }
          }
        }
        else if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
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
              GDB_ColumnType column_type = gdb_column_type_from_string(database, column_node->first->value);
              if (column_type == GDB_ColumnType_Invalid)
              {
                log_error("create table: unknown column type '%.*s' for column '%.*s'",
                          str8_varg(column_node->first->value), str8_varg(column_node->value));
                goto done;
              }
              GDB_ColumnSchema column_schema = gdb_column_schema_create(column_node->value, column_type);
              gdb_table_add_column(table, column_schema);
              
              GDB_Column* new_column = table->columns[table->column_count - 1];
              
              if (column_type == GDB_ColumnType_Enum)
              {
                new_column->enum_type = gdb_database_find_enum_type(database, column_node->first->value);
              }
              
              if (column_type == GDB_ColumnType_Decimal)
              {
                IR_Node* precision_node = column_node->first->first;
                IR_Node* scale_node = precision_node ? precision_node->next : NULL;
                if (!precision_node || !scale_node)
                {
                  log_error("create table: decimal column '%.*s' requires both precision and scale, e.g. decimal(10,2)",
                            str8_varg(column_node->value));
                  goto done;
                }
                U64 precision = u64_from_str8(precision_node->value, 10);
                U64 scale = u64_from_str8(scale_node->value, 10);
                if (precision == 0 || precision > 18 || scale > precision)
                {
                  log_error("create table: decimal column '%.*s' has invalid precision/scale (%llu,%llu) - precision must be 1-18 and scale <= precision",
                            str8_varg(column_node->value), precision, scale);
                  goto done;
                }
                new_column->decimal_precision = (U32)precision;
                new_column->decimal_scale = (U32)scale;
              }
              
              // tec: column_node->first is the Type node
              // any constraint clauses were appended as its siblings by the parser, in whatever order they appeared
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
                    // tec: TODO support composite keys
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
        else if (create_ir_node->type == IR_NodeType_EnumDef)
        {
          if (gdb_database_find_enum_type(database, create_ir_node->value))
          {
            log_error("create type: '%.*s' already exists", str8_varg(create_ir_node->value));
          }
          else
          {
            U32 value_count = 0;
            for (IR_Node* v = create_ir_node->first; v != 0; v = v->next) value_count++;
            
            GDB_EnumType* enum_type = push_array(database->arena, GDB_EnumType, 1);
            enum_type->name = push_str8_copy(database->arena, create_ir_node->value);
            enum_type->value_count = value_count;
            enum_type->value_labels = push_array(database->arena, String8, Max(value_count, 1));
            
            U32 vi = 0;
            for (IR_Node* v = create_ir_node->first; v != 0; v = v->next, vi++)
            {
              enum_type->value_labels[vi] = push_str8_copy(database->arena, v->value);
            }
            
            gdb_database_add_enum_type(database, enum_type);
          }
        }
        
      } break;
      case IR_NodeType_DropIndex:
      {
        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
          break;
        }
        
        IR_Node* table_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        GDB_Table* table = gdb_database_find_table(database, table_node->value);
        if (table)
        {
          gdb_table_drop_index(table, ir_execution_node->value);
        }
      } break;
      case IR_NodeType_Insert:
      {
        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
          break;
        }
        
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
          goto done;
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
            goto done;
          }
          
          U64 ci = 0;
          for (IR_Node* c = columns_object->first; c != 0; c = c->next, ci++)
          {
            GDB_Column* column = gdb_table_find_column(table, c->value);
            if (!column)
            {
              log_error("unknown column '%.*s' in 'insert' statement", str8_varg(c->value));
              goto done;
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
        
        for (IR_Node* value_group_node = values_object->first; 
             value_group_node != 0;
             value_group_node = value_group_node->next)
        {
          MemoryZero(slot_was_set, sizeof(B32) * table->column_count);
          U64 column_index = 0;
          
          for (IR_Node* data_node = value_group_node->first; data_node != 0; data_node = data_node->next)
          {
            if (column_index >= listed_count)
            {
              log_error("too many values in 'insert' statement");
              goto done;
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
              case GDB_ColumnType_Bool:
              {
                U8* value = push_array(scratch.arena, U8, 1);
                *value = (U8)u64_from_str8(value_str, 10);
                value_ptr = value;
              } break;
              case GDB_ColumnType_I32:
              {
                S32* value = push_array(scratch.arena, S32, 1);
                *value = (S32)s64_from_str8(value_str, 10);
                value_ptr = value;
              } break;
              case GDB_ColumnType_I64:
              {
                S64* value = push_array(scratch.arena, S64, 1);
                *value = s64_from_str8(value_str, 10);
                value_ptr = value;
              } break;
              case GDB_ColumnType_Date:
              {
                S32* value = push_array(scratch.arena, S32, 1);
                if (!parse_iso_date(value_str, value))
                {
                  log_error("invalid date literal '%.*s' for column '%.*s'", str8_varg(value_str), str8_varg(column->name));
                  goto done;
                }
                value_ptr = value;
              } break;
              case GDB_ColumnType_Timestamp:
              {
                S64* value = push_array(scratch.arena, S64, 1);
                if (!parse_iso_timestamp(value_str, value))
                {
                  log_error("invalid timestamp literal '%.*s' for column '%.*s'", str8_varg(value_str), str8_varg(column->name));
                  goto done;
                }
                value_ptr = value;
              } break;
              case GDB_ColumnType_Decimal:
              {
                S64* value = push_array(scratch.arena, S64, 1);
                if (!decimal_from_str8(value_str, column->decimal_scale, value))
                {
                  log_error("invalid decimal literal '%.*s' for column '%.*s'", str8_varg(value_str), str8_varg(column->name));
                  goto done;
                }
                
                U64 pow10_precision = 1;
                for (U32 p = 0; p < column->decimal_precision; p++) pow10_precision *= 10;
                S64 magnitude = (*value < 0) ? -(*value) : *value;
                if ((U64)magnitude >= pow10_precision)
                {
                  log_error("decimal literal '%.*s' overflows %.*s's precision(%u) for column '%.*s'",
                            str8_varg(value_str), str8_varg(gdb_column_type_display_name(scratch.arena, column)),
                            column->decimal_precision, str8_varg(column->name));
                  goto done;
                }
                
                value_ptr = value;
              } break;
              case GDB_ColumnType_Enum:
              {
                U32* value = push_array(scratch.arena, U32, 1);
                if (!column->enum_type || !gdb_enum_type_code_from_label(column->enum_type, value_str, value))
                {
                  log_error("'%.*s' is not a valid label for enum type '%.*s' on column '%.*s'",
                            str8_varg(value_str),
                            str8_varg(column->enum_type ? column->enum_type->name : str8_lit("?")),
                            str8_varg(column->name));
                  goto done;
                }
                value_ptr = value;
              } break;
              default:
              log_error("unknown column type");
              goto done;
            }
            
            row_data[slot] = value_ptr;
            row_null[slot] = is_null;
            slot_was_set[slot] = 1;
            column_index++;
          }
          
          if (column_index != listed_count)
          {
            log_error("mismatch in column count and value count in 'insert' statement");
            goto done;
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
            goto done;
          }
          
          gdb_table_add_row(table, row_data, row_null);
        }
        
        scratch_end(scratch);
        
      } break;
      case IR_NodeType_Alter:
      {
        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
          break;
        }
        
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
          
          GDB_ColumnType column_type = gdb_column_type_from_string(database, type_node->value);
          if (column_type == GDB_ColumnType_Invalid)
          {
            log_error("alter table: unknown column type '%.*s'", str8_varg(type_node->value));
            break;
          }
          // tec: resolve/validate BEFORE adding the colum
          GDB_EnumType* enum_type = (column_type == GDB_ColumnType_Enum) ? gdb_database_find_enum_type(database, type_node->value) : NULL;
          U32 decimal_precision = 0, decimal_scale = 0;
          if (column_type == GDB_ColumnType_Decimal)
          {
            IR_Node* precision_node = type_node->first;
            IR_Node* scale_node = precision_node ? precision_node->next : NULL;
            if (!precision_node || !scale_node)
            {
              log_error("alter table: decimal column '%.*s' requires both precision and scale, e.g. decimal(10,2)",
                        str8_varg(column_name_node->value));
              break;
            }
            U64 precision = u64_from_str8(precision_node->value, 10);
            U64 scale = u64_from_str8(scale_node->value, 10);
            if (precision == 0 || precision > 18 || scale > precision)
            {
              log_error("alter table: decimal column '%.*s' has invalid precision/scale (%llu,%llu) - precision must be 1-18 and scale <= precision",
                        str8_varg(column_name_node->value), precision, scale);
              break;
            }
            decimal_precision = (U32)precision;
            decimal_scale = (U32)scale;
          }
          
          GDB_ColumnSchema schema = gdb_column_schema_create(column_name_node->value, column_type);
          gdb_table_add_column(table, schema);
          
          // tec: backfill existing rows as NULL so the new column's row count stays in sync
          GDB_Column* new_column = table->columns[table->column_count - 1];
          U64 existing_row_count = table->row_count;
          new_column->decimal_precision = decimal_precision;
          new_column->decimal_scale = decimal_scale;
          new_column->enum_type = enum_type;
          
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
          
          for (U64 i = 0; i < table->column_count; i++)
          {
            gdb_column_materialize_to_memory(table->columns[i]);
          }
          
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
        
        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
          ProfEnd();
          break;
        }
        
        IR_Node* table_node = ir_node_find_child(ir_execution_node, IR_NodeType_Table);
        GDB_Table* table = gdb_database_find_table(database, table_node->value);
        
        if (!table)
        {
          log_error("delete: unknown table '%.*s'", str8_varg(table_node->value));
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
          scan = qe_cpu_scan_filter(arena, table, where_clause, NULL);
        }
        else
        {
          scan = qe_scan_filter(arena, database, table, where_clause, NULL);
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
        
        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
          ProfEnd();
          break;
        }
        
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
        
        if (!database)
        {
          log_error("no database selected - run 'use <database>' first");
          ProfEnd();
          break;
        }
        
        // tec: a row set result points into the temp tables, so release them after formatting
        U64 temp_table_mark = database->temp_table_count;
        PLAN_ExecResult result = app_perform_kernel(arena, database, ir_execution_node);
        
        IR_Node* select_output_columns = ir_node_find_child(ir_execution_node, IR_NodeType_ColumnList);
        U64 result_count = result.is_materialized ? result.materialized.count : result.rows.count;
        
        log_info("result count %llu", result_count);
        
        B32 capture_structured = (out_result_set != 0) && (ir_execution_node->next == NULL);
        
        if (result.supported && select_output_columns)
        {
          Temp scratch = scratch_begin(0, 0);
          
          U64 out_column_count = 0;
          if (capture_structured)
          {
            for (IR_Node* c = select_output_columns->first; c != NULL; c = c->next) out_column_count++;
            
            out_result_set->valid = 1;
            out_result_set->column_count = out_column_count;
            out_result_set->row_count = result_count;
            out_result_set->columns = push_array(arena, APP_ResultColumn, Max(out_column_count, 1));
            out_result_set->cell_text = push_array(arena, String8, Max(out_column_count * Max(result_count, 1), 1));
            out_result_set->cell_is_null = push_array(arena, B32, Max(out_column_count * Max(result_count, 1), 1));
            out_result_set->cell_numeric = push_array(arena, F64, Max(out_column_count * Max(result_count, 1), 1));
            
            U64 name_i = 0;
            for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next, name_i++)
            {
              out_result_set->columns[name_i].name = push_str8_copy(arena, qe_column_list_item_display_name(arena, column_node));
            }
          }
          
          if (result.is_materialized)
          {
            if (capture_structured)
            {
              U64 col_i = 0;
              for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next, col_i++)
              {
                String8 name = qe_column_list_item_display_name(scratch.arena, column_node);
                GDB_ColumnType col_type = GDB_ColumnType_String8; // tec: fallback for an unresolved ("?") column
                for (U64 c = 0; c < result.materialized.column_count; c++)
                {
                  if (str8_match(result.materialized.columns[c].name, name, 0)) { col_type = result.materialized.columns[c].type; break; }
                }
                out_result_set->columns[col_i].type = col_type;
              }
            }
            
            for (U64 i = 0; i < result_count; i++)
            {
              U64 col_i = 0;
              for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next, col_i++)
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
                
                U64 cell_i = i * out_column_count + col_i;
                
                if (!col)
                {
                  if (!capture_structured) { APP_EMIT("? "); }
                  if (capture_structured) { out_result_set->cell_is_null[cell_i] = 1; }
                }
                else if (col->type == GDB_ColumnType_String8)
                {
                  if (!capture_structured) { APP_EMIT("%.*s ", str8_varg(col->string_values[i])); }
                  if (capture_structured) { out_result_set->cell_text[cell_i] = app_format_cell_text(arena, col->type, 0, col->string_values[i], col->decimal_scale, col->enum_type); }
                }
                else if (col->type == GDB_ColumnType_U32 || col->type == GDB_ColumnType_U64)
                {
                  if (!capture_structured) { APP_EMIT("%llu ", (U64)col->numeric_values[i]); }
                  if (capture_structured)
                  {
                    out_result_set->cell_text[cell_i] = app_format_cell_text(arena, col->type, col->numeric_values[i], (String8){0}, col->decimal_scale, col->enum_type);
                    out_result_set->cell_numeric[cell_i] = col->numeric_values[i];
                  }
                }
                else if (col->type == GDB_ColumnType_I32 || col->type == GDB_ColumnType_I64)
                {
                  if (!capture_structured) { APP_EMIT("%lld ", (S64)col->numeric_values[i]); }
                  if (capture_structured)
                  {
                    out_result_set->cell_text[cell_i] = app_format_cell_text(arena, col->type, col->numeric_values[i], (String8){0}, col->decimal_scale, col->enum_type);
                    out_result_set->cell_numeric[cell_i] = col->numeric_values[i];
                  }
                }
                else if (col->type == GDB_ColumnType_Bool)
                {
                  if (!capture_structured) { APP_EMIT("%s ", col->numeric_values[i] != 0.0 ? "true" : "false"); }
                  if (capture_structured)
                  {
                    out_result_set->cell_text[cell_i] = app_format_cell_text(arena, col->type, col->numeric_values[i], (String8){0}, col->decimal_scale, col->enum_type);
                    out_result_set->cell_numeric[cell_i] = col->numeric_values[i];
                  }
                }
                else if (col->type == GDB_ColumnType_Date || col->type == GDB_ColumnType_Timestamp || col->type == GDB_ColumnType_Decimal || col->type == GDB_ColumnType_Enum)
                {
                  if (capture_structured)
                  {
                    String8 formatted = app_format_cell_text(arena, col->type, col->numeric_values[i], (String8){0}, col->decimal_scale, col->enum_type);
                    out_result_set->cell_text[cell_i] = formatted;
                    out_result_set->cell_numeric[cell_i] = col->numeric_values[i];
                  }
                  else
                  {
                    String8 formatted = app_format_cell_text(arena, col->type, col->numeric_values[i], (String8){0}, col->decimal_scale, col->enum_type);
                    APP_EMIT("%.*s ", str8_varg(formatted));
                  }
                }
                else
                {
                  if (!capture_structured) { APP_EMIT("%lf ", col->numeric_values[i]); }
                  if (capture_structured)
                  {
                    out_result_set->cell_text[cell_i] = app_format_cell_text(arena, col->type, col->numeric_values[i], (String8){0}, col->decimal_scale, col->enum_type);
                    out_result_set->cell_numeric[cell_i] = col->numeric_values[i];
                  }
                }
              }
              if (!capture_structured) { APP_EMIT("\n"); }
            }
          }
          else
          {
            U64 column_count = 0;
            for (IR_Node* c = select_output_columns->first; c != NULL; c = c->next) column_count++;
            
            SelectColGather* gathered = push_array(scratch.arena, SelectColGather, Max(column_count, 1));
            
            U64 gather_start = os_now_microseconds();
            
            U64 ci = 0;
            for (IR_Node* column_node = select_output_columns->first;
                 column_node != NULL;
                 column_node = column_node->next, ci++)
            {
              B32 is_distance = 0;
              if (qe_ir_is_fuzzy_call(column_node, &is_distance))
              {
                if (result.rows.table_count != 1)
                {
                  log_error("SELECT %.*s(): fuzzy search is only supported over a single base table", str8_varg(column_node->value));
                  continue;
                }
                
                IR_Node* col_arg = column_node->first;
                IR_Node* needle_arg = col_arg ? col_arg->next : 0;
                
                B32 scores_match = result.rows.scores && col_arg && needle_arg &&
                  result.rows.score_is_distance == is_distance &&
                  str8_match(result.rows.score_column_name, col_arg->value, 0) &&
                  str8_match(result.rows.score_needle, needle_arg->value, 0);
                
                F64* score_values;
                if (scores_match)
                {
                  score_values = result.rows.scores;
                }
                else
                {
                  score_values = push_array(scratch.arena, F64, Max(result_count, 1));
                  for (U64 i = 0; i < result_count; i++)
                  {
                    score_values[i] = qe_row_eval_fuzzy_call(scratch.arena, &result.rows, column_node, i, is_distance);
                  }
                }
                
                gathered[ci].resolved = 1;
                gathered[ci].is_score = 1;
                gathered[ci].table_slot = 0;
                gathered[ci].type = GDB_ColumnType_F64;
                gathered[ci].numeric_values = score_values;
                continue;
              }
              
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
            
            if (capture_structured)
            {
              for (U64 c = 0; c < column_count; c++)
              {
                out_result_set->columns[c].type = gathered[c].resolved ? gathered[c].type : GDB_ColumnType_String8;
              }
            }
            
            log_info("select column gather total time: %llu microseconds", os_now_microseconds() - gather_start);
            U64 format_start = os_now_microseconds();
            
            app_select_format_dispatch(arena, &out, select_output_columns, gathered, column_count,
                                       &result.rows, result_count, capture_structured, out_result_set);
            log_info("select cell format/emit total time: %llu microseconds", os_now_microseconds() - format_start);
          }
          
          scratch_end(scratch);
        }
        
        gdb_database_release_temp_tables_from(database, temp_table_mark);
        
        log_info("total 'SELECT' query time: %.4f ms", (os_now_microseconds() - start_time) / 1000.0f);
        
        ProfEnd();
      } break;
      
    }
  }
  
  done:;
  if (database)
  {
    String8 database_filepath = push_str8f(arena, "gdb_data/%.*s", (U32)database->name.size, database->name.str);
    gdb_database_save(database, database_filepath);
  }
  
  result.output_text = str8_list_join(arena, &out, &(StringJoin){0});
  
  ProfEnd();
  return result;
}
#undef APP_EMIT

internal void
app_execute_query(String8 sql_query)
{
  Arena* arena = arena_alloc(.reserve_size=Max(GB(1), settings_u64(str8_lit("GPU_MAX_BUFFER_SIZE"), GPU_MAX_BUFFER_SIZE)), .commit_size=MB(64));
  
  APP_QueryResult result = {0};
  GDB_Database* database = NULL;
  OS_MutexScope(g_query_exec_mutex) { result = app_execute_query_capture(arena, sql_query, &database, 0); }
  
  if (result.output_text.size)
  {
    printf("%.*s", str8_varg(result.output_text));
  }
  arena_release(arena);
}

internal PLAN_ExecResult
app_perform_kernel(Arena* arena, GDB_Database* database, IR_Node* root_node)
{
  ProfBeginFunction();
  
  PLAN_ExecResult result = plan_run_select(arena, database, root_node, NULL);
  
  if (!result.supported)
  {
    log_error("app_perform_kernel: query has no supported execution path yet, returning no rows");
  }
  
  ProfEnd();
  return result;
}