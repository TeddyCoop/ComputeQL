internal void
qe_bytecode_emit(QE_BytecodeProgram* prog, U32 word)
{
  if (prog->word_count >= prog->words_cap)
  {
    log_error("qe_bytecode_emit: bytecode program exceeded max word count (%llu)", prog->words_cap);
    return;
  }
  prog->words[prog->word_count++] = word;
}

internal U32
qe_add_numeric_const(QE_BytecodeProgram* prog, F64 value)
{
  if (prog->const_count >= prog->consts_cap)
  {
    log_error("qe_add_numeric_const: exceeded max numeric const count (%llu)", prog->consts_cap);
    return 0;
  }
  
  U64 bits = 0;
  MemoryCopy(&bits, &value, sizeof(bits));
  
  U32 index = (U32)prog->const_count;
  prog->consts[index * 2 + 0] = (U32)(bits & max_U64);
  prog->consts[index * 2 + 1] = (U32)(bits >> 32);
  prog->const_count++;
  
  return index;
}

internal QE_StringConstRef
qe_add_string_const(QE_BytecodeProgram* prog, String8 str)
{
  QE_StringConstRef ref = {0};
  
  // tec: keep every string constant on a 4-byte boundary so word_offset*4 == byte offset
  U64 aligned_offset = AlignPow2(prog->str_const_pool_size, 4);
  
  if (aligned_offset + str.size > prog->str_const_pool_cap)
  {
    log_error("qe_add_string_const: string constant pool exhausted");
    return ref;
  }
  
  MemoryCopy(prog->str_const_pool + aligned_offset, str.str, str.size);
  prog->str_const_pool_size = aligned_offset + str.size;
  
  ref.word_offset = (U32)(aligned_offset / 4);
  ref.byte_len = (U32)str.size;
  return ref;
}

internal QE_ColumnBinding*
qe_find_binding(QE_BytecodeProgram* prog, String8 column_name)
{
  for (U32 i = 0; i < prog->binding_count; i++)
  {
    if (str8_match(prog->bindings[i].name, column_name, 0))
    {
      return &prog->bindings[i];
    }
  }
  return 0;
}

internal QE_ColumnBinding*
qe_bind_column(QE_BytecodeProgram* prog, GDB_Table* table, String8 column_name)
{
  QE_ColumnBinding* existing = qe_find_binding(prog, column_name);
  if (existing) return existing;
  
  if (prog->binding_count >= QE_MAX_COLUMN_BINDINGS)
  {
    log_error("qe_bind_column: exceeded max column bindings (%u)", (U32)QE_MAX_COLUMN_BINDINGS);
    return 0;
  }
  
  GDB_Column* column = gdb_table_find_column(table, column_name);
  if (!column)
  {
    log_error("qe_bind_column: unknown column '%.*s'", str8_varg(column_name));
    return 0;
  }
  
  U32 slot_count = (column->type == GDB_ColumnType_String8) ? 2 : 1;
  if (prog->next_slot + slot_count > QE_MAX_COLUMN_BINDINGS)
  {
    log_error("qe_bind_column: exceeded available descriptor column slots for '%.*s'", str8_varg(column_name));
    return 0;
  }
  
  QE_ColumnBinding* binding = &prog->bindings[prog->binding_count++];
  binding->name = column_name;
  binding->column = column;
  binding->type = column->type;
  binding->first_slot = prog->next_slot;
  binding->slot_count = slot_count;
  prog->next_slot += slot_count;
  
  return binding;
}

internal QE_ColumnBinding*
qe_bind_column_dict_codes(QE_BytecodeProgram* prog, GDB_Table* table, String8 column_name)
{
  String8 synthetic_name = push_str8f(prog->arena, "%.*s$dict", str8_varg(column_name));
  
  QE_ColumnBinding* existing = qe_find_binding(prog, synthetic_name);
  if (existing) return existing;
  
  if (prog->binding_count >= QE_MAX_COLUMN_BINDINGS)
  {
    log_error("qe_bind_column_dict_codes: exceeded max column bindings (%u)", (U32)QE_MAX_COLUMN_BINDINGS);
    return 0;
  }
  
  GDB_Column* column = gdb_table_find_column(table, column_name);
  if (!column || !column->has_dict)
  {
    log_error("qe_bind_column_dict_codes: '%.*s' has no dictionary", str8_varg(column_name));
    return 0;
  }
  
  if (prog->next_slot + 1 > QE_MAX_COLUMN_BINDINGS)
  {
    log_error("qe_bind_column_dict_codes: exceeded available descriptor column slots for '%.*s'", str8_varg(column_name));
    return 0;
  }
  
  QE_ColumnBinding* binding = &prog->bindings[prog->binding_count++];
  binding->name = synthetic_name;
  binding->column = column;
  binding->type = GDB_ColumnType_U32;
  binding->first_slot = prog->next_slot;
  binding->slot_count = 1;
  binding->use_dict_codes = 1;
  prog->next_slot += 1;
  
  return binding;
}

internal QE_Opcode
qe_opcode_from_comparison_operator(String8 op)
{
  if (str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0) ||
      str8_match(op, str8_lit("equals"), StringMatchFlag_CaseInsensitive))
  {
    return QE_Opcode_CmpEq;
  }
  else if (str8_match(op, str8_lit("!="), 0))
  {
    return QE_Opcode_CmpNe;
  }
  else if (str8_match(op, str8_lit("<="), 0))
  {
    return QE_Opcode_CmpLe;
  }
  else if (str8_match(op, str8_lit(">="), 0))
  {
    return QE_Opcode_CmpGe;
  }
  else if (str8_match(op, str8_lit("<"), 0))
  {
    return QE_Opcode_CmpLt;
  }
  else if (str8_match(op, str8_lit(">"), 0))
  {
    return QE_Opcode_CmpGt;
  }
  
  log_error("qe_opcode_from_comparison_operator: unsupported operator '%.*s', defaulting to '='", str8_varg(op));
  return QE_Opcode_CmpEq;
}

internal void
qe_compile_load_value(QE_BytecodeProgram* prog, GDB_Table* table, IR_Node* node)
{
  if (node->type == IR_NodeType_Column)
  {
    QE_ColumnBinding* binding = qe_bind_column(prog, table, node->value);
    if (!binding)
    {
      qe_bytecode_emit(prog, QE_Opcode_PushConst);
      qe_bytecode_emit(prog, qe_add_numeric_const(prog, 0.0));
      return;
    }
    
    U32 operand = ((U32)binding->type << 8) | (binding->first_slot & 0xff);
    qe_bytecode_emit(prog, QE_Opcode_LoadNumCol);
    qe_bytecode_emit(prog, operand);
  }
  else // tec: IR_NodeType_Numeric (or any other leaf) -> constant
  {
    F64 value = f64_from_str8(node->value);
    U32 const_index = qe_add_numeric_const(prog, value);
    qe_bytecode_emit(prog, QE_Opcode_PushConst);
    qe_bytecode_emit(prog, const_index);
  }
}

internal void
qe_compile_condition(QE_BytecodeProgram* prog, GDB_Table* table, IR_Node* condition, QE_ScanTrace* out_trace)
{
  if (!condition) return;
  
  if (condition->type != IR_NodeType_Operator)
  {
    // tec: bare column/literal used as a boolean predicate on its own, not really valid SQL,
    // but load it as a numeric value and let 'nonzero' mean true
    qe_compile_load_value(prog, table, condition);
    return;
  }
  
  String8 op = condition->value;
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : 0;
  
  if (str8_match(op, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    qe_compile_condition(prog, table, left, out_trace);
    qe_compile_condition(prog, table, right, out_trace);
    qe_bytecode_emit(prog, QE_Opcode_And);
    return;
  }
  else if (str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    qe_compile_condition(prog, table, left, out_trace);
    qe_compile_condition(prog, table, right, out_trace);
    qe_bytecode_emit(prog, QE_Opcode_Or);
    return;
  }
  
  // tec: because null checking is weird, theres no right side operand. so this needs to be checked now
  if (str8_match(op, str8_lit("is null"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("is not null"), StringMatchFlag_CaseInsensitive))
  {
    B32 is_not = str8_match(op, str8_lit("is not null"), StringMatchFlag_CaseInsensitive);
    if (is_not)
    {
      qe_bytecode_emit(prog, QE_Opcode_PushTrue);
    }
    else
    {
      qe_bytecode_emit(prog, QE_Opcode_PushConst);
      qe_bytecode_emit(prog, qe_add_numeric_const(prog, 0.0));
    }
    return;
  }
  
  // tec: IN expands to an OR chain of '=' (AND of '!=' for NOT IN)
  // a list too long for the bytecode sets requires_cpu_scan
  if (str8_match(op, str8_lit("in"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive))
  {
    B32 is_not = str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive);
    
    if (!left || !right || right->type != IR_NodeType_InList)
    {
      log_error("qe_compile_condition: 'in' needs a value list on its right side");
      qe_bytecode_emit(prog, QE_Opcode_PushFalse);
      return;
    }
    
    // tec: an empty list matches nothing, so 'not in' matches everything
    if (!right->first)
    {
      qe_bytecode_emit(prog, is_not ? QE_Opcode_PushTrue : QE_Opcode_PushFalse);
      return;
    }
    
    U64 item_count = 0;
    for (IR_Node* item = right->first; item != NULL; item = item->next) 
    {
      item_count++;
    }
    
    if (left->type != IR_NodeType_Column || item_count > settings_u64(str8_lit("QE_IN_LIST_GPU_MAX_ITEMS"), 32))
    {
      prog->requires_cpu_scan = 1;
      prog->cpu_scan_reason = str8_lit("IN list too long for GPU bytecode");
      qe_bytecode_emit(prog, QE_Opcode_PushTrue);
      return;
    }
    
    B32 first_item = 1;
    for (IR_Node* item = right->first; item != NULL; item = item->next)
    {
      // tec: fresh operand copies, since this function reads the right operand as left->next
      IR_Node* lhs = ir_node_make(prog->arena, left->type, left->value);
      IR_Node* rhs = ir_node_make(prog->arena, item->type, item->value);
      lhs->next = rhs;
      IR_Node* cmp = ir_node_make(prog->arena, IR_NodeType_Operator, is_not ? str8_lit("!=") : str8_lit("="));
      cmp->first = lhs;
      cmp->last = rhs;
      
      qe_compile_condition(prog, table, cmp, out_trace);
      if (!first_item) 
      {
        qe_bytecode_emit(prog, is_not ? QE_Opcode_And : QE_Opcode_Or);
      }
      first_item = 0;
    }
    return;
  }
  
  // tec: fuzzy search. SIMILARITY(col,'term')/EDIT_DISTANCE(col,'term') op threshold
  if (left && qe_ir_is_fuzzy_call(left, 0))
  {
    B32 is_distance = 0;
    qe_ir_is_fuzzy_call(left, &is_distance);
    
    IR_Node* col_arg = left->first;
    IR_Node* needle_arg = col_arg ? col_arg->next : 0;
    QE_ColumnBinding* binding = (col_arg && col_arg->type == IR_NodeType_Column) ? qe_bind_column(prog, table, col_arg->value) : 0;
    
    if (!right || !col_arg || !needle_arg || !binding || binding->type != GDB_ColumnType_String8)
    {
      log_error("qe_compile_condition: %.*s() requires (string column, string literal) and a comparison", str8_varg(left->value));
      qe_bytecode_emit(prog, QE_Opcode_PushTrue);
      return;
    }
    
    U64 max_needle_len = is_distance
      ? settings_u64(str8_lit("QE_EDIT_DISTANCE_MAX_NEEDLE_LEN"), 64)
      : settings_u64(str8_lit("QE_TRIGRAM_MAX_NEEDLE_LEN"), 64);
    String8 needle = needle_arg->value;
    if (needle.size > max_needle_len)
    {
      log_error("qe_compile_condition: %.*s() needle longer than max %llu bytes, truncating", str8_varg(left->value), max_needle_len);
      needle = str8_prefix(needle, max_needle_len);
    }
    
    QE_StringConstRef ref = qe_add_string_const(prog, needle);
    
    qe_bytecode_emit(prog, is_distance ? QE_Opcode_EditDistance : QE_Opcode_TrigramSim);
    qe_bytecode_emit(prog, binding->first_slot);
    qe_bytecode_emit(prog, ref.word_offset);
    qe_bytecode_emit(prog, ref.byte_len);
    
    prog->has_score_output = 1;
    prog->score_is_distance = is_distance;
    prog->score_column_name = col_arg->value;
    prog->score_needle = needle_arg->value;
    
    qe_compile_load_value(prog, table, right);
    qe_bytecode_emit(prog, qe_opcode_from_comparison_operator(op));
    return;
  }
  
  if (!left || !right)
  {
    log_error("qe_compile_condition: malformed comparison, missing operand(s)");
    qe_bytecode_emit(prog, QE_Opcode_PushTrue);
    return;
  }
  
  // tec: string comparisons - column op 'literal', where the column is a String8 column
  if (left->type == IR_NodeType_Column && right->type == IR_NodeType_Literal)
  {
    U32 bindings_before = prog->binding_count;
    U32 slots_before = prog->next_slot;
    QE_ColumnBinding* binding = qe_bind_column(prog, table, left->value);
    B32 bound_here = prog->binding_count > bindings_before;
    if (binding && binding->type == GDB_ColumnType_String8)
    {
      B32 is_contains = str8_match(op, str8_lit("contains"), StringMatchFlag_CaseInsensitive);
      B32 is_eq = str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0);
      B32 is_ne = str8_match(op, str8_lit("!="), 0);
      B32 is_lt = str8_match(op, str8_lit("<"), 0);
      B32 is_gt = str8_match(op, str8_lit(">"), 0);
      B32 is_le = str8_match(op, str8_lit("<="), 0);
      B32 is_ge = str8_match(op, str8_lit(">="), 0);
      
      if (!is_contains && !is_eq && !is_ne && !is_lt && !is_gt && !is_le && !is_ge)
      {
        log_error("qe_compile_condition: unsupported string operator '%.*s'", str8_varg(op));
        qe_bytecode_emit(prog, QE_Opcode_PushTrue);
        return;
      }
      
      // tec: dict codes are assigned by insert order during dict build, not sorted.
      // so only equality/inequality can use them, relational ops must always compare the raw bytes
      if (is_eq || is_ne)
      {
        gdb_column_ensure_string_dict(binding->column);
        if (binding->column->has_dict)
        {
          if (out_trace)
          {
            out_trace->dict_decision_made = 1;
            out_trace->dict_size = binding->column->dict->value_count;
          }
          
          U32 code = 0;
          if (!gdb_string_dict_code_from_value(binding->column->dict, right->value, &code))
          {
            if (out_trace) out_trace->dict_hit = 0;
            if (bound_here)
            {
              prog->binding_count = bindings_before;
              prog->next_slot = slots_before;
            }
            // tec: literal absent from the dictionary matches no row for '=', every row for '!='
            qe_bytecode_emit(prog, is_ne ? QE_Opcode_PushTrue : QE_Opcode_PushFalse);
            return;
          }
          if (out_trace) out_trace->dict_hit = 1;
          
          // tec: the dict codes replace the raw string data, so drop the binding made above unless something else already needed it
          if (bound_here)
          {
            prog->binding_count = bindings_before;
            prog->next_slot = slots_before;
          }
          
          QE_ColumnBinding* dict_binding = qe_bind_column_dict_codes(prog, table, left->value);
          if (dict_binding)
          {
            U32 operand = ((U32)dict_binding->type << 8) | (dict_binding->first_slot & 0xff);
            qe_bytecode_emit(prog, QE_Opcode_LoadNumCol);
            qe_bytecode_emit(prog, operand);
            qe_bytecode_emit(prog, QE_Opcode_PushConst);
            qe_bytecode_emit(prog, qe_add_numeric_const(prog, (F64)code));
            qe_bytecode_emit(prog, is_ne ? QE_Opcode_CmpNe : QE_Opcode_CmpEq);
            return;
          }
          
          binding = qe_bind_column(prog, table, left->value);
          if (!binding)
          {
            qe_bytecode_emit(prog, QE_Opcode_PushTrue);
            return;
          }
        }
      }
      
      QE_Opcode string_opcode = QE_Opcode_StrEq;
      if (is_contains) string_opcode = QE_Opcode_StrContains;
      else if (is_ne) string_opcode = QE_Opcode_StrNe;
      else if (is_lt) string_opcode = QE_Opcode_StrLt;
      else if (is_gt) string_opcode = QE_Opcode_StrGt;
      else if (is_le) string_opcode = QE_Opcode_StrLe;
      else if (is_ge) string_opcode = QE_Opcode_StrGe;
      
      QE_StringConstRef ref = qe_add_string_const(prog, right->value);
      
      qe_bytecode_emit(prog, string_opcode);
      qe_bytecode_emit(prog, binding->first_slot);
      qe_bytecode_emit(prog, ref.word_offset);
      qe_bytecode_emit(prog, ref.byte_len);
      return;
    }
    
    // tec: DATE/TIMESTAMP comparison
    if (binding && (binding->type == GDB_ColumnType_Date || binding->type == GDB_ColumnType_Timestamp))
    {
      F64 value = 0.0;
      if (!qe_resolve_date_literal_value(binding->type, right, &value))
      {
        qe_bytecode_emit(prog, QE_Opcode_PushTrue);
        return;
      }
      
      U32 operand = ((U32)binding->type << 8) | (binding->first_slot & 0xff);
      qe_bytecode_emit(prog, QE_Opcode_LoadNumCol);
      qe_bytecode_emit(prog, operand);
      qe_bytecode_emit(prog, QE_Opcode_PushConst);
      qe_bytecode_emit(prog, qe_add_numeric_const(prog, value));
      qe_bytecode_emit(prog, qe_opcode_from_comparison_operator(op));
      return;
    }
    
    // tec: ENUM comparisons
    if (binding && binding->type == GDB_ColumnType_Enum)
    {
      F64 value = 0.0;
      if (!qe_resolve_enum_literal_value(binding->column, right, &value))
      {
        qe_bytecode_emit(prog, QE_Opcode_PushTrue);
        return;
      }
      
      U32 operand = ((U32)binding->type << 8) | (binding->first_slot & 0xff);
      qe_bytecode_emit(prog, QE_Opcode_LoadNumCol);
      qe_bytecode_emit(prog, operand);
      qe_bytecode_emit(prog, QE_Opcode_PushConst);
      qe_bytecode_emit(prog, qe_add_numeric_const(prog, value));
      qe_bytecode_emit(prog, qe_opcode_from_comparison_operator(op));
      return;
    }
  }
  
  // tec: DECIMAL comparisons
  if (left->type == IR_NodeType_Column && right->type == IR_NodeType_Numeric)
  {
    QE_ColumnBinding* binding = qe_bind_column(prog, table, left->value);
    if (binding && binding->type == GDB_ColumnType_Decimal)
    {
      F64 value = 0.0;
      if (!qe_resolve_decimal_literal_value(binding->column, right, &value))
      {
        qe_bytecode_emit(prog, QE_Opcode_PushTrue);
        return;
      }
      
      U32 operand = ((U32)binding->type << 8) | (binding->first_slot & 0xff);
      qe_bytecode_emit(prog, QE_Opcode_LoadNumCol);
      qe_bytecode_emit(prog, operand);
      qe_bytecode_emit(prog, QE_Opcode_PushConst);
      qe_bytecode_emit(prog, qe_add_numeric_const(prog, value));
      qe_bytecode_emit(prog, qe_opcode_from_comparison_operator(op));
      return;
    }
  }
  
  // tec: generic numeric comparison
  qe_compile_load_value(prog, table, left);
  qe_compile_load_value(prog, table, right);
  qe_bytecode_emit(prog, qe_opcode_from_comparison_operator(op));
}

internal void
qe_bytecode_program_build(Arena* arena, QE_BytecodeProgram* prog, GDB_Database* database, GDB_Table* table, IR_Node* root_node, IR_Node* where_clause, QE_ScanTrace* out_trace)
{
  MemoryZeroStruct(prog);
  prog->arena = arena;
  prog->words_cap = settings_u64(str8_lit("QE_BYTECODE_MAX_WORDS"), 4096);
  prog->consts_cap = settings_u64(str8_lit("QE_MAX_NUMERIC_CONSTS"), 256);
  prog->str_const_pool_cap = settings_u64(str8_lit("QE_STRING_CONST_POOL_SIZE"), KB(64));
  prog->words = push_array(arena, U32, prog->words_cap);
  prog->consts = push_array(arena, U32, prog->consts_cap * 2);
  prog->str_const_pool = push_array(arena, U8, prog->str_const_pool_cap);
  
  if (where_clause && where_clause->first)
  {
    qe_compile_condition(prog, table, where_clause->first, out_trace);
  }
  else
  {
    qe_bytecode_emit(prog, QE_Opcode_PushTrue);
  }
  
  qe_bytecode_emit(prog, QE_Opcode_Halt);
}

internal U32
qe_bytecode_program_max_stack_depth(QE_BytecodeProgram* prog)
{
  U32 depth = 0;
  U32 max_depth = 0;
  U64 ip = 0;
  
  while (ip < prog->word_count)
  {
    U32 opcode = prog->words[ip++];
    switch (opcode)
    {
      case QE_Opcode_PushTrue:
      case QE_Opcode_PushFalse:
      {
        depth += 1;
      } break;
      
      case QE_Opcode_LoadNumCol:
      { 
        ip += 1; 
        depth += 1; } 
      break;
      
      case QE_Opcode_PushConst:
      { 
        ip += 1;
        depth += 1;
      } break;
      
      case QE_Opcode_CmpEq:
      case QE_Opcode_CmpNe:
      case QE_Opcode_CmpLt:
      case QE_Opcode_CmpGt:
      case QE_Opcode_CmpLe: 
      case QE_Opcode_CmpGe:
      case QE_Opcode_And: 
      case QE_Opcode_Or:
      { 
        depth -= 1; 
      } break;
      
      case QE_Opcode_StrEq:
      case QE_Opcode_StrContains:
      case QE_Opcode_TrigramSim:
      case QE_Opcode_EditDistance:
      case QE_Opcode_StrNe:
      case QE_Opcode_StrLt:
      case QE_Opcode_StrGt:
      case QE_Opcode_StrLe:
      case QE_Opcode_StrGe:
      {
        ip += 3;
        depth += 1;
      } break;
      
      case QE_Opcode_Halt:
      {
        ip = prog->word_count; 
      } break;
      
      default:
      {
        ip = prog->word_count; 
      } break;
    }
    max_depth = Max(max_depth, depth);
  }
  
  return max_depth;
}

//~ tec: double-buffered chunk prefetch for qe_scan_filter
/*
  abackground thread loads the next chunk from disk while the main thread uploads, dispatches, and reads back the current chunk, overlapping disk IO with GPU work

the worker only performs gdb_column_* reads into host memory. all GPU work remains on the
main thread because the dispatch path uses shared sync stuff

the next chunk is not prefetched until the current chunk has finished uploading and any mapped string data has been closed
*/

internal void
qe_prefetch_read_slot(QE_PrefetchSlot* slot, QE_BytecodeProgram* prog, Rng1U64 range, U64 rows)
{
  slot->chunk_range = range;
  slot->chunk_rows = rows;
  
  for (U32 i = 0; i < prog->binding_count; i++)
  {
    QE_ColumnBinding* binding = &prog->bindings[i];
    QE_PrefetchBindingResult* out = &slot->bindings[i];
    MemoryZeroStruct(out);
    
    B32 is_whole_column = (range.min == 0 && range.max == binding->column->row_count);
    if (binding->type != GDB_ColumnType_String8 && is_whole_column &&
        binding->column->gpu_upload_generation == binding->column->write_generation)
    {
      out->valid = 1;
      out->cached = 1;
    }
    // tec: non dict string column already resident from a prior whole col upload. skip the chunk read entirely
    else if (binding->type == GDB_ColumnType_String8 && !binding->use_dict_codes && is_whole_column &&
             binding->column->gpu_str_upload_generation == binding->column->write_generation)
    {
      out->is_string = 1;
      out->valid = 1;
      out->cached = 1;
    }
    else if (binding->use_dict_codes)
    {
      out->data_ptr = binding->column->dict_codes + range.min;
      out->size = (range.max - range.min) * sizeof(U32);
      out->valid = (binding->column->dict_codes != 0);
    }
    else if (binding->type == GDB_ColumnType_String8)
    {
      out->is_string = 1;
      out->str_chunk = gdb_column_get_string_chunk(slot->arena, binding->column, range);
      out->valid = (out->str_chunk.data != 0 && out->str_chunk.offsets != 0);
    }
    else
    {
      out->data_ptr = gdb_column_get_data_range(slot->arena, binding->column, range, &out->size);
      out->valid = (out->data_ptr != 0);
    }
  }
}

internal void
qe_prefetch_worker_main(void* raw_ctx)
{
  TCTX tctx_;
  tctx_init_and_equip(&tctx_);
  
  QE_PrefetchCtx* ctx = (QE_PrefetchCtx*)raw_ctx;
  for (;;)
  {
    os_semaphore_take(ctx->request_sem, max_U64);
    if (ctx->stop)
    {
      break;
    }
    
    arena_clear(ctx->slots[ctx->pending_slot].arena);
    qe_prefetch_read_slot(&ctx->slots[ctx->pending_slot], ctx->prog, ctx->pending_range, ctx->pending_rows);
    os_semaphore_drop(ctx->ready_sem);
  }
}

internal QE_PrefetchCtx*
qe_prefetch_start(Arena* arena, QE_BytecodeProgram* prog)
{
  QE_PrefetchCtx* ctx = push_array(arena, QE_PrefetchCtx, 1);
  ctx->prog = prog;
  ctx->slots[0].arena = arena_alloc();
  ctx->slots[1].arena = arena_alloc();
  ctx->request_sem = os_semaphore_alloc(0, 1, str8_zero());
  ctx->ready_sem = os_semaphore_alloc(0, 1, str8_zero());
  ctx->worker = os_thread_launch(qe_prefetch_worker_main, ctx, 0);
  return ctx;
}

// tec: asks the background thread to read `range` into slot `slot_index` (0 or 1)
internal void
qe_prefetch_request(QE_PrefetchCtx* ctx, U32 slot_index, Rng1U64 range, U64 rows)
{
  ctx->pending_slot = slot_index;
  ctx->pending_range = range;
  ctx->pending_rows = rows;
  os_semaphore_drop(ctx->request_sem);
}

// tec: blocks until the most recently requested slot is ready, then returns it
internal QE_PrefetchSlot*
qe_prefetch_wait(QE_PrefetchCtx* ctx, U32 slot_index)
{
  os_semaphore_take(ctx->ready_sem, max_U64);
  return &ctx->slots[slot_index];
}

internal void
qe_prefetch_stop(QE_PrefetchCtx* ctx)
{
  ctx->stop = 1;
  os_semaphore_drop(ctx->request_sem);
  os_thread_join(ctx->worker, max_U64);
  os_semaphore_release(ctx->request_sem);
  os_semaphore_release(ctx->ready_sem);
  arena_release(ctx->slots[0].arena);
  arena_release(ctx->slots[1].arena);
}

//~ tec: EXPLAIN ANALYZE trace/stats

internal QE_TraceCtx*
qe_trace_ctx_alloc(Arena* arena)
{
  QE_TraceCtx* trace = push_array(arena, QE_TraceCtx, 1);
  trace->arena = arena;
  return trace;
}

internal QE_NodeTrace*
qe_trace_record_begin(QE_TraceCtx* trace, PLAN_Node* plan_node, PLAN_NodeType node_type)
{
  if (!trace) return 0;
  
  QE_NodeTrace* record = push_array(trace->arena, QE_NodeTrace, 1);
  record->plan_node = plan_node;
  record->node_type = node_type;
  
  if (trace->records_last)
  {
    trace->records_last->next = record;
    trace->records_last = record;
  }
  else
  {
    trace->records = trace->records_last = record;
  }
  
  return record;
}

internal QE_NodeTrace*
qe_trace_find(QE_TraceCtx* trace, PLAN_Node* plan_node)
{
  if (!trace) return 0;
  for (QE_NodeTrace* record = trace->records; record != 0; record = record->next)
  {
    if (record->plan_node == plan_node) return record;
  }
  return 0;
}

internal QE_ScanResult
qe_scan_filter(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* where_clause, QE_ScanTrace* out_trace)
{
  return qe_scan_filter_selected(arena, database, table, where_clause, out_trace, 0, 0);
}

// tec: when out_selection is given and the scan is a single dispatch, the selected rows stay on the GPU and result.indices stays empty
internal QE_ScanResult
qe_scan_filter_selected(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* where_clause, QE_ScanTrace* out_trace, QE_DeviceSelection* out_selection, U64 expected_rows)
{
  ProfBeginFunction();
  
  if (out_selection)
  {
    MemoryZeroStruct(out_selection);
  }
  QE_ScanResult result = {0};
  
  QE_BytecodeProgram* prog = push_array(arena, QE_BytecodeProgram, 1);
  qe_bytecode_program_build(arena, prog, database, table, NULL, where_clause, out_trace);
  
  U32 stack_depth = qe_bytecode_program_max_stack_depth(prog);
  log_debug("scan_filter bytecode peak operand-stack depth: %u (of MAX_STACK=%u)", stack_depth, (U32)QE_SCAN_MAX_STACK);
  
  if (prog->requires_cpu_scan)
  {
    log_debug("qe_scan_filter: %.*s - running the scan on the CPU", str8_varg(prog->cpu_scan_reason));
    if (out_trace)
    {
      out_trace->strategy = QE_TraceStrategy_CpuScan;
      out_trace->strategy_reason = prog->cpu_scan_reason;
    }
    ProfEnd();
    return qe_cpu_scan_filter(arena, table, where_clause, out_trace);
  }
  
  if (stack_depth > QE_SCAN_MAX_STACK)
  {
    log_error("qe_scan_filter: WHERE clause needs operand-stack depth %u, exceeding scan_filter.comp's MAX_STACK (%u) - falling back to CPU scan to avoid a GPU stack overflow",
              stack_depth, (U32)QE_SCAN_MAX_STACK);
    if (out_trace)
    {
      out_trace->strategy = QE_TraceStrategy_CpuScan;
      out_trace->strategy_reason = str8_lit("WHERE clause exceeds GPU operand-stack depth");
    }
    ProfEnd();
    return qe_cpu_scan_filter(arena, table, where_clause, out_trace);
  }
  
  GPU_Kernel* kernel = gpu_kernel_alloc(str8_lit("scan_filter"));
  if (!kernel)
  {
    log_error("qe_scan_filter: failed to alloc 'scan_filter' kernel");
    ProfEnd();
    return result;
  }
  
  //- tec: upload the (query invariant) bytecode + constant pool buffers
  GPU_Buffer* bytecode_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_bytecode"), Max(prog->word_count, 1) * sizeof(U32), GPU_BufferFlag_Write, 0);
  GPU_Buffer* num_consts_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_num_consts"), Max(prog->const_count * 2, 1) * sizeof(U32), GPU_BufferFlag_Write, 0);
  GPU_Buffer* str_consts_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_str_consts"), Max(prog->str_const_pool_size, 4), GPU_BufferFlag_Write, 0);
  
  {
    U64 word_bytes = prog->word_count * sizeof(U32);
    U64 num_consts_bytes = prog->const_count * 2 * sizeof(U32);
    U64 str_bytes = prog->str_const_pool_size;
    
    GPU_Batch* prog_batch = gpu_batch_begin(word_bytes + num_consts_bytes + str_bytes, 0);
    gpu_batch_buffer_write(prog_batch, bytecode_buffer, prog->words, word_bytes);
    if (prog->const_count > 0)
    {
      gpu_batch_buffer_write(prog_batch, num_consts_buffer, prog->consts, num_consts_bytes);
    }
    if (prog->str_const_pool_size > 0)
    {
      gpu_batch_buffer_write(prog_batch, str_consts_buffer, prog->str_const_pool, str_bytes);
    }
    gpu_batch_end(prog_batch);
  }
  
  gpu_kernel_set_arg_buffer(kernel, QE_BINDING_BYTECODE, bytecode_buffer);
  gpu_kernel_set_arg_buffer(kernel, QE_BINDING_NUM_CONSTS, num_consts_buffer);
  gpu_kernel_set_arg_buffer(kernel, QE_BINDING_STR_CONSTS, str_consts_buffer);
  
  // tec: query invariant fuzzy search push constants
  U64 score_flags = prog->has_score_output ? (1u | (prog->score_is_distance ? 2u : 0u)) : 0u;
  gpu_kernel_set_arg_u64(kernel, QE_PUSH_CONSTANT_SCORE_FLAGS, score_flags);
  gpu_kernel_set_arg_u64(kernel, QE_PUSH_CONSTANT_TRIGRAM_N, settings_u64(str8_lit("QE_TRIGRAM_N"), 3));
  
  QE_ResultChunk* result_chunks = 0;
  QE_ResultChunk** tail = &result_chunks;
  
  U64 largest_column_size = 0;
  for (U32 i = 0; i < prog->binding_count; i++)
  {
    largest_column_size = Max(gdb_column_get_total_size(prog->bindings[i].column), largest_column_size);
  }
  
  U64 gpu_kernel_execution_time = 0;
  U64 load_data_from_disk_time = 0;
  U64 prefetch_stall_time = 0;
  U64 buffer_alloc_time = 0;
  U64 submit_wait_time = 0;
  U64 gpu_cache_hit_bytes = 0, gpu_cache_hit_count = 0;
  U64 gpu_cache_miss_bytes = 0, gpu_cache_miss_count = 0;
  
  U64 gpu_max_buffer_size = settings_u64(str8_lit("GPU_MAX_BUFFER_SIZE"), GPU_MAX_BUFFER_SIZE);
  B32 needs_chunking = largest_column_size > gpu_max_buffer_size;
  
  U64 rows_per_chunk = table->row_count;
  
  if (needs_chunking)
  {
    U64 row_size = 0;
    for (U32 i = 0; i < prog->binding_count; i++)
    {
      row_size += table->row_count ? (gdb_column_get_total_size(prog->bindings[i].column) / table->row_count) : 0;
    }
    if (row_size == 0) row_size = 1;
    
    rows_per_chunk = gpu_max_buffer_size / row_size;
    if (rows_per_chunk == 0) rows_per_chunk = 1;
  }
  if (rows_per_chunk == 0) rows_per_chunk = 1; // tec: table->row_count == 0 case
  
  // tec: zone map pruning may skip some of these ranges' wroth of rows completely and merge the rest into
  // fewer, differently sized dispatches. fall back to chunk_index*rows_per_chunk if no leaf of the where clause is prunable
  U64 chunk_count = 0;
  U64 pruned_rows = 0;
  String8 pruned_column_name = {0};
  Rng1U64* dispatch_ranges = qe_scan_build_dispatch_ranges(arena, table, where_clause, rows_per_chunk, &chunk_count, &pruned_rows, &pruned_column_name);
  if (pruned_rows > 0)
  {
    log_debug("qe_scan_filter: zone-map pruning skipped %llu of %llu rows, %llu dispatch range(s) remain",
              pruned_rows, table->row_count, chunk_count);
  }
  if (out_trace)
  {
    out_trace->rows_before = table->row_count;
    out_trace->zonemap_pruned_rows = pruned_rows;
    out_trace->zonemap_chunk_count = chunk_count;
    out_trace->zonemap_column_name = pruned_column_name;
    out_trace->chunk_count = chunk_count;
  }
  
  B32 keep_on_device = out_selection && chunk_count == 1 && !prog->has_score_output;
  
  // tec: only worth a background thread + two extra arenas when there's a next chunk to hide IO for the common single-chunk case
  QE_PrefetchCtx* prefetch = (chunk_count > 1) ? qe_prefetch_start(arena, prog) : 0;
  
  for (U64 chunk_index = 0; chunk_index < chunk_count; chunk_index++)
  {
    Temp fallback_arena = {0};
    B32 using_prefetch = (prefetch != 0);
    
    Rng1U64 chunk_range = dispatch_ranges[chunk_index];
    U64 chunk_row_start = chunk_range.min;
    U64 chunk_rows = chunk_range.max - chunk_range.min;
    B32 chunk_is_whole_column = (chunk_range.min == 0 && chunk_range.max == table->row_count);
    
    log_debug("filtering rows %llu-%llu", chunk_range.min, chunk_range.max);
    
    QE_PrefetchSlot* slot;
    if (using_prefetch)
    {
      if (chunk_index == 0)
      {
        // tec: nothing to overlap chunk 0s read with yet, so do it synchronously on the main thread, straight into slot 0.
        U64 start_read_time = os_now_microseconds();
        arena_clear(prefetch->slots[0].arena);
        qe_prefetch_read_slot(&prefetch->slots[0], prog, chunk_range, chunk_rows);
        load_data_from_disk_time += os_now_microseconds() - start_read_time;
        slot = &prefetch->slots[0];
      }
      else
      {
        // tec: this chunk's read was kicked off during the previous chunk's dispatch below
        // usually already done by now, so this should stall near zero if IO is well hidden
        U64 wait_start = os_now_microseconds();
        slot = qe_prefetch_wait(prefetch, (U32)(chunk_index % 2));
        prefetch_stall_time += os_now_microseconds() - wait_start;
      }
    }
    else
    {
      fallback_arena = temp_begin(arena);
      slot = push_array(fallback_arena.arena, QE_PrefetchSlot, 1);
      slot->arena = fallback_arena.arena;
      U64 start_read_time = os_now_microseconds();
      qe_prefetch_read_slot(slot, prog, chunk_range, chunk_rows);
      load_data_from_disk_time += os_now_microseconds() - start_read_time;
    }
    
    ProfBegin("allocating column GPU buffers");
    U64 buffer_alloc_start = os_now_microseconds();
    
    B32 column_slot_used[QE_MAX_COLUMN_BINDINGS] = {0};
    
    for (U32 i = 0; i < prog->binding_count; i++)
    {
      QE_ColumnBinding* binding = &prog->bindings[i];
      QE_PrefetchBindingResult* in = &slot->bindings[i];
      U32 descriptor_binding = QE_BINDING_COLUMN_BASE + binding->first_slot;
      
      String8 col_pool_key = push_str8f(gpu_scratch_arena(), "scan_col:%.*s.%.*s", str8_varg(table->name), str8_varg(binding->name));
      // tec: a distinct key from col_pool_key, used only for whole column dispatches
      String8 col_pool_key_full = push_str8f(gpu_scratch_arena(), "%.*s.whole", str8_varg(col_pool_key));
      
      if (in->cached)
      {
        log_debug("qe_scan_filter: reusing GPU-resident buffer for column '%.*s' (generation %llu, %llu bytes) - no read, no re-upload",
                  str8_varg(binding->name), binding->column->gpu_upload_generation, binding->column->gpu_upload_data_size);
        gpu_cache_hit_count++;
        gpu_cache_hit_bytes += binding->column->gpu_upload_data_size;
      }
      
      if (binding->use_dict_codes)
      {
        if (in->cached)
        {
          GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(col_pool_key_full, binding->column->gpu_upload_data_size, GPU_BufferFlag_Write, 0);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding, data_buf);
          column_slot_used[binding->first_slot] = 1;
        }
        else if (in->valid)
        {
          String8 key = chunk_is_whole_column ? col_pool_key_full : col_pool_key;
          GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(key, in->size, GPU_BufferFlag_Write, in->data_ptr);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding, data_buf);
          column_slot_used[binding->first_slot] = 1;
          if (chunk_is_whole_column)
          {
            binding->column->gpu_upload_generation = binding->column->write_generation;
            binding->column->gpu_upload_data_size = in->size;
            gpu_cache_miss_count++;
            gpu_cache_miss_bytes += in->size;
          }
        }
        else
        {
          log_error("qe_scan_filter: failed to load dictionary codes for column '%.*s'", str8_varg(binding->name));
        }
      }
      else if (binding->type == GDB_ColumnType_String8)
      {
        if (in->cached)
        {
          log_debug("qe_scan_filter: reusing GPU-resident string buffer for column '%.*s' (generation %llu, %llu+%llu bytes) - no read, no re-upload",
                    str8_varg(binding->name), binding->column->gpu_str_upload_generation,
                    binding->column->gpu_str_upload_data_size, binding->column->gpu_str_upload_offsets_size);
          gpu_cache_hit_count++;
          gpu_cache_hit_bytes += binding->column->gpu_str_upload_data_size + binding->column->gpu_str_upload_offsets_size;
          
          String8 data_key = push_str8f(gpu_scratch_arena(), "%.*s.data", str8_varg(col_pool_key_full));
          GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(data_key, binding->column->gpu_str_upload_data_size, GPU_BufferFlag_Write | GPU_BufferFlag_HostVisible, 0);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 0, data_buf);
          
          String8 offsets_key = push_str8f(gpu_scratch_arena(), "%.*s.offsets", str8_varg(col_pool_key_full));
          GPU_Buffer* offsets_buf = gpu_buffer_alloc_pooled(offsets_key, binding->column->gpu_str_upload_offsets_size, GPU_BufferFlag_Write | GPU_BufferFlag_CopyHostPointer, 0);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 1, offsets_buf);
          
          column_slot_used[binding->first_slot] = 1;
          column_slot_used[binding->first_slot + 1] = 1;
        }
        else if (in->valid)
        {
          String8 key_prefix = chunk_is_whole_column ? col_pool_key_full : col_pool_key;
          
          String8 data_key = push_str8f(gpu_scratch_arena(), "%.*s.data", str8_varg(key_prefix));
          GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(data_key, in->str_chunk.size, GPU_BufferFlag_Write | GPU_BufferFlag_HostVisible, in->str_chunk.data);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 0, data_buf);
          
          // tec: +1 row for the trailing offset used to compute the last strings size
          U64 offsets_size = (in->str_chunk.row_count + 1) * sizeof(U64);
          String8 offsets_key = push_str8f(gpu_scratch_arena(), "%.*s.offsets", str8_varg(key_prefix));
          GPU_Buffer* offsets_buf = gpu_buffer_alloc_pooled(offsets_key, offsets_size, GPU_BufferFlag_Write | GPU_BufferFlag_CopyHostPointer, in->str_chunk.offsets);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 1, offsets_buf);
          
          column_slot_used[binding->first_slot] = 1;
          column_slot_used[binding->first_slot + 1] = 1;
          
          if (chunk_is_whole_column)
          {
            binding->column->gpu_str_upload_generation = binding->column->write_generation;
            binding->column->gpu_str_upload_data_size = in->str_chunk.size;
            binding->column->gpu_str_upload_offsets_size = offsets_size;
            gpu_cache_miss_count++;
            gpu_cache_miss_bytes += in->str_chunk.size + offsets_size;
          }
          
          // tec: safe to close now, the data's already been copied into the GPU buffer above
          gdb_column_close_string_chunk(binding->column);
        }
        else
        {
          log_error("qe_scan_filter: failed to load string data/offsets for column '%.*s'", str8_varg(binding->name));
        }
      }
      else if (in->cached)
      {
        GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(col_pool_key_full, binding->column->gpu_upload_data_size, GPU_BufferFlag_Write, 0);
        gpu_kernel_set_arg_buffer(kernel, descriptor_binding, data_buf);
        column_slot_used[binding->first_slot] = 1;
      }
      else if (in->valid && chunk_is_whole_column)
      {
        GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(col_pool_key_full, in->size, GPU_BufferFlag_Write, in->data_ptr);
        gpu_kernel_set_arg_buffer(kernel, descriptor_binding, data_buf);
        
        column_slot_used[binding->first_slot] = 1;
        binding->column->gpu_upload_generation = binding->column->write_generation;
        binding->column->gpu_upload_data_size = in->size;
        gpu_cache_miss_count++;
        gpu_cache_miss_bytes += in->size;
      }
      else if (in->valid)
      {
        GPU_Buffer* data_buf = 0;
        if (binding->column->is_disk_backed)
        {
          data_buf = gpu_buffer_import_host_readonly_pooled(col_pool_key, in->data_ptr, in->size);
        }
        if (!data_buf)
        {
          // tec: distinct key from the import path above
          String8 fallback_key = push_str8f(gpu_scratch_arena(), "%.*s.alloc_fallback", str8_varg(col_pool_key));
          data_buf = gpu_buffer_alloc_pooled(fallback_key, in->size, GPU_BufferFlag_Write, in->data_ptr);
        }
        gpu_kernel_set_arg_buffer(kernel, descriptor_binding, data_buf);
        
        column_slot_used[binding->first_slot] = 1;
      }
    }
    
    for (U32 col_slot = 0; col_slot < QE_MAX_COLUMN_BINDINGS; col_slot++)
    {
      if (!column_slot_used[col_slot])
      {
        gpu_kernel_set_arg_buffer(kernel, QE_BINDING_COLUMN_BASE + col_slot, bytecode_buffer);
      }
    }
    
    // tec: sized to a default cap rather than chunk_rows
    U64 output_cap_rows = Max(Min(chunk_rows, settings_u64(str8_lit("QE_SCAN_OUTPUT_DEFAULT_CAP_ROWS"), 65536)), 1);
    // tec: a result the optimizer expects to be large gets its buffer up front, saving the second dispatch
    if (expected_rows > output_cap_rows)
    {
      output_cap_rows = Min(chunk_rows, expected_rows + expected_rows / 8 + 1024);
    }
    GPU_Buffer* output_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_output"), output_cap_rows * 2 * sizeof(U32), GPU_BufferFlag_Read, 0);
    GPU_Buffer* result_counter_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_result_counter"), 2 * sizeof(U32), GPU_BufferFlag_ReadWrite | GPU_BufferFlag_HostCached, 0);
    buffer_alloc_time += os_now_microseconds() - buffer_alloc_start;
    ProfEnd();
    
    gpu_kernel_set_arg_buffer(kernel, QE_BINDING_OUT_INDICES, output_buffer);
    gpu_kernel_set_arg_buffer(kernel, QE_BINDING_OUT_COUNT, result_counter_buffer);
    gpu_kernel_set_arg_u64(kernel, QE_PUSH_CONSTANT_ROW_COUNT, chunk_rows);
    
    // tec: this chunk's columns are now fully read+uploaded+closed
    // so its safe to kick off the next chunk's read in the background
    if (using_prefetch && chunk_index + 1 < chunk_count)
    {
      Rng1U64 next_range = dispatch_ranges[chunk_index + 1];
      U64 next_rows = next_range.max - next_range.min;
      qe_prefetch_request(prefetch, (U32)((chunk_index + 1) % 2), next_range, next_rows);
    }
    
    // tec: the first rows of the output come back in the same submit as the count, so a selective scan needs no second download
    // tec: the chunk's temp arena is rewound below, so the speculative rows live in a scratch arena until they are copied out
    Temp speculative_scratch = scratch_begin(&arena, 1);
    U64 speculative_rows = 0;
    U32* speculative_indices = 0;
    if (optimizer_fusion_enabled() && !keep_on_device)
    {
      speculative_rows = Min(settings_u64(str8_lit("QE_SCAN_SPECULATIVE_ROWS"), 4096), output_cap_rows);
      speculative_indices = push_array(speculative_scratch.arena, U32, Max(speculative_rows, (U64)1) * 2);
    }
    
    // tec: at most 2 attempts
    U32 result_count32[2] = {0, 0};
    U64 result_count = 0;
    for (U32 attempt = 0; attempt < 2; attempt++)
    {
      U64 submit_wait_start = os_now_microseconds();
      GPU_Batch* dispatch_batch = gpu_batch_begin(0, sizeof(result_count32) + speculative_rows * 2 * sizeof(U32));
      gpu_batch_buffer_zero(dispatch_batch, result_counter_buffer, 2 * sizeof(U32));
      gpu_batch_kernel_execute(dispatch_batch, kernel, (U32)chunk_rows, QE_GPU_WORKGROUP_SIZE);
      gpu_batch_buffer_read(dispatch_batch, result_counter_buffer, result_count32, sizeof(result_count32));
      if (speculative_rows > 0)
      {
        gpu_batch_buffer_read(dispatch_batch, output_buffer, speculative_indices, speculative_rows * 2 * sizeof(U32));
      }
      gpu_batch_end(dispatch_batch);
      gpu_kernel_execution_time += gpu_get_executed_kernel_time_microseconds();
      submit_wait_time += os_now_microseconds() - submit_wait_start;
      
      result_count = result_count32[0];
      if (result_count <= output_cap_rows)
      {
        break;
      }
      
      U64 grow_start = os_now_microseconds();
      output_cap_rows = result_count;
      output_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_output"), output_cap_rows * 2 * sizeof(U32), GPU_BufferFlag_Read, 0);
      gpu_kernel_set_arg_buffer(kernel, QE_BINDING_OUT_INDICES, output_buffer);
      buffer_alloc_time += os_now_microseconds() - grow_start;
    }
    
    if (!using_prefetch)
    {
      temp_end(fallback_arena);
    }
    
    if (keep_on_device)
    {
      out_selection->valid = 1;
      out_selection->table = table;
      out_selection->rows = output_buffer;
      out_selection->count = result_count;
      out_selection->row_offset = chunk_row_start;
      out_selection->stride_words = 2;
    }
    else if (result_count != 0)
    {
      U32* raw_indices = speculative_indices;
      if (result_count > speculative_rows)
      {
        raw_indices = push_array(arena, U32, result_count * 2);
        gpu_buffer_read(output_buffer, raw_indices, result_count * 2 * sizeof(U32));
      }
      
      U64* chunk_data = push_array(arena, U64, result_count);
      F64* chunk_scores = prog->has_score_output ? push_array(arena, F64, result_count) : 0;
      for (U64 i = 0; i < result_count; i++)
      {
        chunk_data[i] = chunk_row_start + raw_indices[i * 2 + 0];
        if (chunk_scores)
        {
          U32 bits = raw_indices[i * 2 + 1];
          if (prog->score_is_distance)
          {
            chunk_scores[i] = (F64)bits;
          }
          else
          {
            F32 f; MemoryCopy(&f, &bits, sizeof(f));
            chunk_scores[i] = (F64)f;
          }
        }
      }
      
      QE_ResultChunk* rc = push_array(arena, QE_ResultChunk, 1);
      rc->indices = chunk_data;
      rc->scores = chunk_scores;
      rc->count = result_count;
      rc->next = 0;
      *tail = rc;
      tail = &rc->next;
    }
    scratch_end(speculative_scratch);
  }
  
  if (prefetch)
  {
    qe_prefetch_stop(prefetch);
  }
  
  gpu_kernel_release(kernel);
  
  log_debug("gpu kernel total execution time: %llu microseconds", gpu_kernel_execution_time);
  log_debug("load from disk total time: %llu microseconds", load_data_from_disk_time);
  log_debug("buffer alloc total time: %llu microseconds", buffer_alloc_time);
  log_debug("submit+wait (gpu_kernel_execute) total time: %llu microseconds", submit_wait_time);
  if (chunk_count > 1)
  {
    log_debug("prefetch stall time (time the GPU sat idle waiting on disk I/O the pipeline failed to hide): %llu microseconds", prefetch_stall_time);
  }
  if (out_trace)
  {
    out_trace->gpu_kernel_time_us = gpu_kernel_execution_time;
    out_trace->load_from_disk_time_us = load_data_from_disk_time;
    out_trace->buffer_alloc_time_us = buffer_alloc_time;
    out_trace->submit_wait_time_us = submit_wait_time;
    out_trace->prefetch_stall_time_us = prefetch_stall_time;
    out_trace->gpu_cache_hit_bytes = gpu_cache_hit_bytes;
    out_trace->gpu_cache_hit_count = gpu_cache_hit_count;
    out_trace->gpu_cache_miss_bytes = gpu_cache_miss_bytes;
    out_trace->gpu_cache_miss_count = gpu_cache_miss_count;
  }
  
  ProfBegin("flatten result chunks");
  {
    U64 total_count = 0;
    for (QE_ResultChunk* chunk = result_chunks; chunk; chunk = chunk->next)
    {
      total_count += chunk->count;
    }
    result.indices = push_array(arena, U64, Max(total_count, 1));
    result.count = total_count;
    if (prog->has_score_output)
    {
      result.scores = push_array(arena, F64, Max(total_count, 1));
      result.score_is_distance = prog->score_is_distance;
      result.score_column_name = prog->score_column_name;
      result.score_needle = prog->score_needle;
    }
    
    U64* out_ptr = result.indices;
    F64* out_scores_ptr = result.scores;
    for (QE_ResultChunk* chunk = result_chunks; chunk; chunk = chunk->next)
    {
      MemoryCopy(out_ptr, chunk->indices, chunk->count * sizeof(U64));
      out_ptr += chunk->count;
      if (out_scores_ptr && chunk->scores)
      {
        MemoryCopy(out_scores_ptr, chunk->scores, chunk->count * sizeof(F64));
        out_scores_ptr += chunk->count;
      }
    }
  }
  ProfEnd();
  
  if (out_selection && out_selection->valid)
  {
    result.count = out_selection->count;
  }
  if (out_trace) out_trace->rows_after = result.count;
  
  ProfEnd();
  return result;
}

//~ tec: SELECT-list/HAVING item display name

internal String8
qe_column_list_item_display_name(Arena* arena, IR_Node* item)
{
  IR_Node* alias = ir_node_find_child(item, IR_NodeType_Alias);
  if (alias)
  {
    return alias->value;
  }
  
  if (item->type == IR_NodeType_AggregateCall)
  {
    // tec: only the arguments belong in the name, not a trailing Window/Alias/sort direction
    IR_Node* args[3] = {0};
    U64 arg_count = 0;
    for (IR_Node* c = item->first; c != NULL && arg_count < ArrayCount(args); c = c->next)
    {
      if (c->type == IR_NodeType_Column || 
          c->type == IR_NodeType_Numeric || 
          c->type == IR_NodeType_Literal)
      {
        args[arg_count++] = c;
      }
    }
    
    B32 is_window = ir_node_find_child(item, IR_NodeType_Window) != NULL;
    String8 arg_text = arg_count ? args[0]->value : (is_window ? (String8){0} : str8_lit("*"));
    String8 suffix = is_window ? str8_lit(" OVER") : (String8){0};
    
    // tec: a second, nonalias argument must be part of the display name too
    if (arg_count >= 2)
    {
      return push_str8f(arena, "%.*s(%.*s, %.*s)%.*s", str8_varg(item->value), str8_varg(arg_text), str8_varg(args[1]->value), str8_varg(suffix));
    }
    return push_str8f(arena, "%.*s(%.*s)%.*s", str8_varg(item->value), str8_varg(arg_text), str8_varg(suffix));
  }
  
  return item->value; // tec: plain Column
}

//~ tec: multi-table column qualifier resolution

internal GDB_Table*
qe_resolve_column_table(PLAN_RowSet* rows, String8 column_name, String8* out_bare_name, U64* out_slot)
{
  String8 qualifier = {0};
  String8 bare_name = column_name;
  B32 has_dot = 0;
  U64 dot_pos = 0;
  
  for (U64 i = 0; i < column_name.size; i++)
  {
    if (column_name.str[i] == '.')
    {
      dot_pos = i;
      has_dot = 1;
      break;
    }
  }
  
  if (has_dot)
  {
    qualifier = str8_prefix(column_name, dot_pos);
    bare_name = str8_skip(column_name, dot_pos + 1);
  }
  
  if (out_bare_name) *out_bare_name = bare_name;
  if (out_slot) *out_slot = max_U64;
  
  if (has_dot)
  {
    for (U64 t = 0; t < rows->table_count; t++)
    {
      // tec: prefer a slots alias over the real table name
      String8 alias = rows->aliases ? rows->aliases[t] : (String8){0};
      String8 name_to_match = alias.size ? alias : rows->tables[t]->name;
      if (str8_match(name_to_match, qualifier, StringMatchFlag_CaseInsensitive))
      {
        if (out_slot) *out_slot = t;
        return rows->tables[t];
      }
    }
    log_error("qe_resolve_column_table: no table '%.*s' in scope (column '%.*s')", str8_varg(qualifier), str8_varg(column_name));
    return NULL;
  }
  
  GDB_Table* found = NULL;
  U64 found_slot = max_U64;
  for (U64 t = 0; t < rows->table_count; t++)
  {
    if (gdb_table_find_column(rows->tables[t], bare_name))
    {
      if (found)
      {
        log_error("qe_resolve_column_table: ambiguous column '%.*s' - present in multiple joined tables, qualify it", str8_varg(bare_name));
        return NULL;
      }
      found = rows->tables[t];
      found_slot = t;
    }
  }
  
  if (!found)
  {
    log_error("qe_resolve_column_table: unknown column '%.*s'", str8_varg(bare_name));
  }
  else if (out_slot)
  {
    *out_slot = found_slot;
  }
  return found;
}

//~ tec: dense per-output-row column gather 
internal F64
qe_read_numeric_as_f64(GDB_Column* column, U64 row_index)
{
  void* data = gdb_column_get_data(column, row_index);
  if (!data) return 0.0;
  return gdb_numeric_value_as_f64(column->type, data);
}

internal B32
qe_resolve_date_literal_value(GDB_ColumnType col_type, IR_Node* literal_node, F64* out_value)
{
  if (col_type == GDB_ColumnType_Date)
  {
    S32 days = 0;
    if (!parse_iso_date(literal_node->value, &days))
    {
      log_error("invalid date literal '%.*s'", str8_varg(literal_node->value));
      return 0;
    }
    *out_value = (F64)days;
    return 1;
  }
  if (col_type == GDB_ColumnType_Timestamp)
  {
    S64 seconds = 0;
    if (!parse_iso_timestamp(literal_node->value, &seconds))
    {
      log_error("invalid timestamp literal '%.*s'", str8_varg(literal_node->value));
      return 0;
    }
    *out_value = (F64)seconds;
    return 1;
  }
  return 0;
}

internal B32
qe_resolve_decimal_literal_value(GDB_Column* column, IR_Node* literal_node, F64* out_value)
{
  if (column->type != GDB_ColumnType_Decimal)
  {
    return 0;
  }
  
  S64 raw = 0;
  if (!decimal_from_str8(literal_node->value, column->decimal_scale, &raw))
  {
    log_error("invalid decimal literal '%.*s'", str8_varg(literal_node->value));
    return 0;
  }
  *out_value = (F64)raw;
  return 1;
}

internal B32
qe_resolve_enum_literal_value(GDB_Column* column, IR_Node* literal_node, F64* out_value)
{
  if (column->type != GDB_ColumnType_Enum || !column->enum_type)
  {
    return 0;
  }
  
  U32 code = 0;
  if (!gdb_enum_type_code_from_label(column->enum_type, literal_node->value, &code))
  {
    log_error("'%.*s' is not a valid label for enum type '%.*s'",
              str8_varg(literal_node->value), str8_varg(column->enum_type->name));
    return 0;
  }
  *out_value = (F64)code;
  return 1;
}

internal U64
qe_rowset_table_slot(PLAN_RowSet* rows, GDB_Table* table)
{
  for (U64 t = 0; t < rows->table_count; t++)
  {
    if (rows->tables[t] == table) return t;
  }
  return max_U64;
}

internal THREAD_POOL_TASK_FUNC(qe_gather_numeric_task)
{
  QE_GatherNumericTask* task = (QE_GatherNumericTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  
  for (U64 i = range.min; i < range.max; i++)
  {
    // tec: no row list means the column's own rows in order
    U64 row = task->table_rows ? task->table_rows[i] : i;
    if (row == PLAN_NULL_ROW || !task->base_ptr)
    {
      task->values[i] = 0.0;
      continue;
    }
    
    void* data = (U8*)task->base_ptr + (row - task->min_row) * task->column_size;
    switch (task->column_type)
    {
      case GDB_ColumnType_U32: task->values[i] = (F64)(*(U32*)data); break;
      case GDB_ColumnType_U64: task->values[i] = (F64)(*(U64*)data); break;
      case GDB_ColumnType_F32: task->values[i] = (F64)(*(F32*)data); break;
      case GDB_ColumnType_F64: task->values[i] = *(F64*)data; break;
      case GDB_ColumnType_Bool: task->values[i] = (F64)(*(U8*)data); break;
      case GDB_ColumnType_I32: task->values[i] = (F64)(*(S32*)data); break;
      case GDB_ColumnType_I64: task->values[i] = (F64)(*(S64*)data); break;
      case GDB_ColumnType_Date: task->values[i] = (F64)(*(S32*)data); break;
      case GDB_ColumnType_Timestamp: task->values[i] = (F64)(*(S64*)data); break;
      case GDB_ColumnType_Decimal: task->values[i] = (F64)(*(S64*)data); break;
      case GDB_ColumnType_Enum: task->values[i] = (F64)(*(U32*)data); break;
      default: task->values[i] = 0.0; break;
    }
  }
}

internal THREAD_POOL_TASK_FUNC(qe_row_bounds_task)
{
  QE_RowBoundsTask* task = (QE_RowBoundsTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  U64 min_row = max_U64;
  U64 max_row = 0;
  for (U64 i = range.min; i < range.max; i++)
  {
    U64 row = task->table_rows[i];
    if (row == PLAN_NULL_ROW)
    {
      continue;
    }
    if (row < min_row)
    {
      min_row = row;
    }
    if (row > max_row) 
    {
      max_row = row;
    }
  }
  task->task_min[task_id] = min_row;
  task->task_max[task_id] = max_row;
}

// tec: out_min stays max_U64 when every row is NULL
internal void
qe_row_bounds(U64* table_rows, U64 count, U64* out_min, U64* out_max)
{
  *out_min = max_U64;
  *out_max = 0;
  if (count == 0)
  {
    return;
  }
  
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count / 16384));
  
  QE_RowBoundsTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.table_rows = table_rows;
  task.task_min = push_array(scratch.arena, U64, task_count);
  task.task_max = push_array(scratch.arena, U64, task_count);
  
  if (task_count == 1)
  {
    qe_row_bounds_task(0, 0, 0, &task);
  }
  else
  {
    TP_Arena* pool_arena = app_thread_pool_arena();
    TP_Temp temp = tp_temp_begin(pool_arena);
    tp_for_parallel(pool, pool_arena, task_count, qe_row_bounds_task, &task);
    tp_temp_end(temp);
  }
  
  for (U64 t = 0; t < task_count; t++)
  {
    if (task.task_min[t] < *out_min)
    {
      *out_min = task.task_min[t];
    }
    if (task.task_max[t] > *out_max) 
    {
      *out_max = task.task_max[t];
      s}
  }
  
  scratch_end(scratch);
}

internal F64*
qe_gather_numeric_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column)
{
  ProfBeginFunction();
  
  F64* values = push_array(arena, F64, Max(rows->count, 1));
  
  if (table_slot >= rows->table_count)
  {
    log_error("qe_gather_numeric_column: slot %llu is not part of this row set", table_slot);
    ProfEnd();
    return values;
  }
  
  U64* table_rows = rows->row_indices[table_slot];
  
  
  // tec: bound the row indices this gather actually touches, then pull the whole span in one read
  U64 min_row = max_U64;
  U64 max_row = 0;
  qe_row_bounds(table_rows, rows->count, &min_row, &max_row);
  
  void* base_ptr = 0;
  if (min_row != max_U64)
  {
    U64 range_size = 0;
    base_ptr = gdb_column_get_data_range(arena, column, r1u64(min_row, max_row + 1), &range_size);
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, rows->count));
  
  QE_GatherNumericTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, rows->count, (U32)task_count);
  task.table_rows = table_rows;
  task.base_ptr = base_ptr;
  task.min_row = min_row;
  task.column_type = column->type;
  task.column_size = column->size;
  task.values = values;
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_gather_numeric_task, &task);
  tp_temp_end(temp);
  
  scratch_end(scratch);
  
  ProfEnd();
  return values;
}

internal THREAD_POOL_TASK_FUNC(qe_gather_dict_codes_task)
{
  QE_GatherDictCodesTask* task = (QE_GatherDictCodesTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  
  for (U64 i = range.min; i < range.max; i++)
  {
    U64 row = task->table_rows[i];
    task->values[i] = (row == PLAN_NULL_ROW) ? 0.0 : (F64)task->dict_codes[row];
  }
}

internal F64*
qe_gather_string_dict_codes(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column)
{
  ProfBeginFunction();
  
  F64* values = push_array(arena, F64, Max(rows->count, 1));
  
  if (table_slot >= rows->table_count)
  {
    log_error("qe_gather_string_dict_codes: slot %llu is not part of this row set", table_slot);
    ProfEnd();
    return values;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, rows->count));
  
  QE_GatherDictCodesTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, rows->count, (U32)task_count);
  task.table_rows = rows->row_indices[table_slot];
  task.dict_codes = column->dict_codes;
  task.values = values;
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_gather_dict_codes_task, &task);
  tp_temp_end(temp);
  
  scratch_end(scratch);
  
  ProfEnd();
  return values;
}

internal THREAD_POOL_TASK_FUNC(qe_dict_lookup_task)
{
  QE_DictLookupTask* task = (QE_DictLookupTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  
  for (U64 i = range.min; i < range.max; i++)
  {
    U64 start = task->chunk.offsets[i];
    U64 len = task->chunk.offsets[i + 1] - start;
    String8 s = str8((U8*)task->chunk.data + start, len);
    task->values[i] = (F64)gdb_string_dict_code_or_sentinel(task->dict, s);
  }
}

internal THREAD_POOL_TASK_FUNC(qe_dense_dict_codes_task)
{
  QE_DenseDictCodesTask* task = (QE_DenseDictCodesTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 i = range.min; i < range.max; i++) task->values[i] = (F64)task->codes[i];
}

internal F64*
qe_dict_codes_to_f64_dense(Arena* arena, U32* codes, U64 count)
{
  F64* values = push_array(arena, F64, Max(count, 1));
  if (count == 0) return values;
  
  Temp scratch = scratch_begin(&arena, 1);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_DenseDictCodesTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.codes = codes;
  task.values = values;
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_dense_dict_codes_task, &task);
  tp_temp_end(temp);
  
  scratch_end(scratch);
  return values;
}

// tec: cross-column lookup
internal F64*
qe_dict_codes_from_string_chunk(Arena* arena, GDB_StringDataChunk* chunk, GDB_StringDict* dict)
{
  ProfBeginFunction();
  
  F64* values = push_array(arena, F64, Max(chunk->row_count, 1));
  
  Temp scratch = scratch_begin(&arena, 1);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, chunk->row_count));
  
  QE_DictLookupTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, chunk->row_count, (U32)task_count);
  task.chunk = *chunk;
  task.dict = dict;
  task.values = values;
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_dict_lookup_task, &task);
  tp_temp_end(temp);
  
  scratch_end(scratch);
  
  ProfEnd();
  return values;
}

// tec: from_dict's codes mean nothing against to_dict, so map each distinct value of from_dict to its to_dict code once
internal U32*
qe_dict_recode_lut(Arena* arena, GDB_StringDict* from_dict, GDB_StringDict* to_dict)
{
  U32* lut = push_array(arena, U32, Max(from_dict->value_count, (U32)1));
  for (U32 code = 0; code < from_dict->value_count; code++)
  {
    lut[code] = gdb_string_dict_code_or_sentinel(to_dict, from_dict->values[code]);
  }
  return lut;
}

internal THREAD_POOL_TASK_FUNC(qe_dict_recode_task)
{
  QE_DictRecodeTask* task = (QE_DictRecodeTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 i = range.min; i < range.max; i++)
  {
    task->values[i] = (F64)task->lut[task->codes[i]];
  }
}

internal F64*
qe_dict_codes_recoded_dense(Arena* arena, U32* codes, U64 count, U32* lut)
{
  F64* values = push_array(arena, F64, Max(count, 1));
  if (count == 0) return values;
  
  Temp scratch = scratch_begin(&arena, 1);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_DictRecodeTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.codes = codes;
  task.lut = lut;
  task.values = values;
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_dict_recode_task, &task);
  tp_temp_end(temp);
  
  scratch_end(scratch);
  return values;
}

// tec: left_column's dict codes recoded into right_dict's code space for a device-resident dict-key hash join
// not cached across queries since it depends on which right dict the join targets
internal GPU_Buffer*
qe_hash_join_left_key_recoded_full_f64(Arena* arena, GDB_Column* left_column, GDB_StringDict* right_dict)
{
  if (!left_column->has_dict)
  {
    return 0;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  U32* lut = qe_dict_recode_lut(scratch.arena, left_column->dict, right_dict);
  F64* values = qe_dict_codes_recoded_dense(scratch.arena, left_column->dict_codes, left_column->row_count, lut);
  GPU_Buffer* buffer = gpu_buffer_alloc(Max(left_column->row_count, (U64)1) * sizeof(F64), GPU_BufferFlag_Write, values);
  scratch_end(scratch);
  return buffer;
}

internal THREAD_POOL_TASK_FUNC(qe_narrow_check_task)
{
  QE_NarrowCheckTask* task = (QE_NarrowCheckTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  B32 narrow = 1;
  B32 whole = 1;
  F64 bound = 0.0;
  for (U64 i = range.min; i < range.max; i++)
  {
    F64 v = task->values[i];
    if ((F64)(F32)v != v) 
    { 
      narrow = 0; 
      break; 
    }
    F64 magnitude = v < 0.0 ? -v : v;
    bound = Max(bound, magnitude);
    // tec: past 2^53 the cast is no longer a check, and nothing that large stays exact in F32 sums anyway
    if (whole && (magnitude >= 9007199254740992.0 || (F64)(S64)v != v)) 
    {
      whole = 0; 
    }
  }
  task->task_narrow[task_id] = narrow;
  task->task_int_bound[task_id] = whole ? bound : -1.0;
}

internal B32
qe_values_round_trip_f32(F64* values, U64 count, F64* out_int_bound)
{
  *out_int_bound = 0.0;
  if (count == 0) 
  {
    return 1;
  }
  
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_NarrowCheckTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.values = values;
  task.task_narrow = push_array(scratch.arena, B32, task_count);
  task.task_int_bound = push_array(scratch.arena, F64, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_narrow_check_task, &task);
  tp_temp_end(temp);
  
  B32 all_narrow = 1;
  F64 int_bound = 0.0;
  for (U64 t = 0; t < task_count; t++)
  {
    if (!task.task_narrow[t]) 
    {
      all_narrow = 0; 
      break; 
    }
    if (task.task_int_bound[t] < 0.0) 
    { 
      int_bound = -1.0; 
    }
    else if (int_bound >= 0.0) 
    { 
      int_bound = Max(int_bound, task.task_int_bound[t]); 
    }
  }
  *out_int_bound = int_bound;
  
  scratch_end(scratch);
  return all_narrow;
}

internal THREAD_POOL_TASK_FUNC(qe_fixed_point_task)
{
  QE_FixedPointTask* task = (QE_FixedPointTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  S32 scale = 0;
  F64 largest = 0.0;
  for (U64 i = range.min; i < range.max; i++)
  {
    F64 v = task->values[i];
    F64 magnitude = v < 0.0 ? -v : v;
    if (!(magnitude < QE_AGG_FIXED_SCALED_LIMIT))
    {
      scale = -1;
      break;
    }
    largest = Max(largest, magnitude);
    // tec: a raising scale keeps every earlier value whole, so the scale only ever goes up
    for (;;)
    {
      F64 scaled = v * g_qe_fixed_pow10[scale];
      F64 whole = (F64)(S64)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
      if (whole / g_qe_fixed_pow10[scale] == v)
      {
        break;
      }
      scale += 1;
      if (scale > QE_FIXED_MAX_SCALE)
      {
        break;
      }
    }
    if (scale > QE_FIXED_MAX_SCALE)
    {
      scale = -1;
      break;
    }
  }
  task->task_scale[task_id] = scale;
  task->task_max[task_id] = largest;
}

internal B32
qe_values_fixed_point(F64* values, U64 count, S32* out_scale, F64* out_max)
{
  *out_scale = -1;
  *out_max = 0.0;
  if (count == 0)
  {
    *out_scale = 0;
    return 1;
  }
  
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_FixedPointTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.values = values;
  task.task_scale = push_array(scratch.arena, S32, task_count);
  task.task_max = push_array(scratch.arena, F64, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_fixed_point_task, &task);
  tp_temp_end(temp);
  
  S32 scale = 0;
  F64 largest = 0.0;
  for (U64 t = 0; t < task_count; t++)
  {
    if (task.task_scale[t] < 0)
    {
      scale = -1;
      break;
    }
    scale = Max(scale, task.task_scale[t]);
    largest = Max(largest, task.task_max[t]);
  }
  scratch_end(scratch);
  
  // tec: a larger scale than a range needed makes its values larger, so the whole scaled range has to be checked together
  if (scale >= 0 && largest * g_qe_fixed_pow10[scale] >= QE_AGG_FIXED_SCALED_LIMIT)
  {
    scale = -1;
  }
  *out_scale = scale;
  *out_max = largest;
  return scale >= 0;
}

internal THREAD_POOL_TASK_FUNC(qe_key_range_task)
{
  QE_KeyRangeTask* task = (QE_KeyRangeTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  F64 smallest = 0.0;
  F64 largest = 0.0;
  B32 whole = 1;
  for (U64 i = range.min; i < range.max; i++)
  {
    F64 v = task->values[i];
    if (!(v > -2147483648.0 && v < 2147483648.0) || (F64)(S32)v != v)
    {
      whole = 0;
      break;
    }
    if (i == range.min)
    {
      smallest = v;
      largest = v;
    }
    smallest = Min(smallest, v);
    largest = Max(largest, v);
  }
  task->task_min[task_id] = smallest;
  task->task_max[task_id] = largest;
  task->task_whole[task_id] = whole;
}

internal B32
qe_values_whole_range(F64* values, U64 count, S64* out_min, S64* out_max)
{
  *out_min = 0;
  *out_max = 0;
  if (count == 0)
  {
    return 0;
  }
  
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_KeyRangeTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.values = values;
  task.task_min = push_array(scratch.arena, F64, task_count);
  task.task_max = push_array(scratch.arena, F64, task_count);
  task.task_whole = push_array(scratch.arena, B32, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_key_range_task, &task);
  tp_temp_end(temp);
  
  B32 whole = 1;
  F64 smallest = task.task_min[0];
  F64 largest = task.task_max[0];
  for (U64 t = 0; t < task_count; t++)
  {
    if (!task.task_whole[t])
    {
      whole = 0;
      break;
    }
    smallest = Min(smallest, task.task_min[t]);
    largest = Max(largest, task.task_max[t]);
  }
  scratch_end(scratch);
  
  *out_min = (S64)smallest;
  *out_max = (S64)largest;
  return whole;
}

internal THREAD_POOL_TASK_FUNC(qe_narrow_convert_task)
{
  QE_NarrowConvertTask* task = (QE_NarrowConvertTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 i = range.min; i < range.max; i++) task->dst[i] = (F32)task->src[i];
}

internal F32*
qe_values_to_f32(Arena* arena, F64* values, U64 count)
{
  F32* out = push_array(arena, F32, Max(count, 1));
  if (count == 0) return out;
  
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_NarrowConvertTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.src = values;
  task.dst = out;
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_narrow_convert_task, &task);
  tp_temp_end(temp);
  
  scratch_end(scratch);
  return out;
}

//~ tec: gpu resident aggregate inputs
// a group key or aggregate argument column is uploaded once as a dense F64 (or F32) array and reused by later queries,
// as long as the row set is the whole base table in order and the column has not been written since

internal THREAD_POOL_TASK_FUNC(qe_identity_check_task)
{
  QE_IdentityCheckTask* task = (QE_IdentityCheckTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  B32 identity = 1;
  for (U64 i = range.min; i < range.max; i++)
  {
    if (task->table_rows[i] != i)
    {
      identity = 0;
      break;
    }
  }
  task->task_identity[task_id] = identity;
}

internal B32
qe_rowset_slot_is_identity(PLAN_RowSet* rows, U64 table_slot)
{
  U64* table_rows = rows->row_indices[table_slot];
  if (!table_rows || rows->count == 0)
  {
    return 0;
  }
  if (plan_rows_are_shared_identity(table_rows, rows->count))
  {
    return 1;
  }
  
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, rows->count));
  
  QE_IdentityCheckTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, rows->count, (U32)task_count);
  task.table_rows = table_rows;
  task.task_identity = push_array(scratch.arena, B32, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_identity_check_task, &task);
  tp_temp_end(temp);
  
  B32 all_identity = 1;
  for (U64 t = 0; t < task_count; t++)
  {
    if (!task.task_identity[t])
    {
      all_identity = 0;
      break;
    }
  }
  
  scratch_end(scratch);
  return all_identity;
}

internal B32
qe_aggregate_column_can_be_resident(PLAN_RowSet* rows, U64 table_slot, GDB_Column* column, B32* identity_known, B32* identity_value)
{
  if (!column->parent_table || column->parent_table->is_ephemeral)
  {
    return 0;
  }
  if (table_slot >= QE_AGG_MAX_RESIDENT_SLOTS || table_slot >= rows->table_count)
  {
    return 0;
  }
  if (rows->tables[table_slot] != column->parent_table || rows->count != column->row_count)
  {
    return 0;
  }
  if (!identity_known[table_slot])
  {
    identity_value[table_slot] = qe_rowset_slot_is_identity(rows, table_slot);
    identity_known[table_slot] = 1;
  }
  return identity_value[table_slot];
}

// tec: keyed by the column object, so two tables that share a name cant hand each other a buffer
internal String8
qe_aggregate_resident_key(GDB_Column* column, B32 as_f32)
{
  return push_str8f(gpu_scratch_arena(), "agg_resident_%s:%p", as_f32 ? "f32" : "f64", (void*)column);
}

internal GPU_Buffer*
qe_aggregate_resident_lookup(GDB_Column* column, B32 as_f32, U64 row_count)
{
  U64 generation = as_f32 ? column->agg_f32_generation : column->agg_f64_generation;
  void* known_buffer = as_f32 ? column->agg_f32_buffer : column->agg_f64_buffer;
  if (!known_buffer || generation != column->write_generation)
  {
    return 0;
  }
  
  U64 element_size = as_f32 ? sizeof(F32) : sizeof(F64);
  GPU_Buffer* buffer = gpu_buffer_alloc_pooled(qe_aggregate_resident_key(column, as_f32), row_count * element_size, GPU_BufferFlag_Write, 0);
  if ((void*)buffer != known_buffer)
  {
    // tec: the pool no longer holds the buffer this column was uploaded into
    if (as_f32)
    {
      column->agg_f32_buffer = 0;
    }
    else
    {
      column->agg_f64_buffer = 0;
    }
    return 0;
  }
  return buffer;
}

internal GPU_Buffer*
qe_aggregate_resident_store(GDB_Column* column, B32 as_f32, void* data, U64 row_count)
{
  U64 element_size = as_f32 ? sizeof(F32) : sizeof(F64);
  GPU_Buffer* buffer = gpu_buffer_alloc_pooled(qe_aggregate_resident_key(column, as_f32), row_count * element_size, GPU_BufferFlag_Write, data);
  if (!buffer)
  {
    return 0;
  }
  
  if (as_f32)
  {
    column->agg_f32_buffer = buffer;
    column->agg_f32_generation = column->write_generation;
  }
  else
  {
    column->agg_f64_buffer = buffer;
    column->agg_f64_generation = column->write_generation;
  }
  return buffer;
}

//~ tec: device selection consumers
// a column that is read through a selection is kept whole on the GPU, so it does not depend on which rows a query selected

internal F64*
qe_column_dense_f64(Arena* arena, GDB_Column* column)
{
  ProfBeginFunction();
  
  U64 count = column->row_count;
  F64* values = push_array(arena, F64, Max(count, (U64)1));
  if (count == 0)
  {
    ProfEnd();
    return values;
  }
  
  U64 range_size = 0;
  void* base_ptr = gdb_column_get_data_range(arena, column, r1u64(0, count), &range_size);
  
  Temp scratch = scratch_begin(&arena, 1);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_GatherNumericTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.table_rows = 0;
  task.base_ptr = base_ptr;
  task.min_row = 0;
  task.column_type = column->type;
  task.column_size = column->size;
  task.values = values;
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_gather_numeric_task, &task);
  tp_temp_end(temp);
  
  scratch_end(scratch);
  
  ProfEnd();
  return values;
}

internal GPU_Buffer*
qe_aggregate_full_column_f64(Arena* arena, GDB_Column* column, B32 dict_codes, B32* out_narrow)
{
  if (!column->parent_table || column->parent_table->is_ephemeral || column->type == GDB_ColumnType_Invalid)
  {
    return 0;
  }
  
  U64 rows = column->row_count;
  GPU_Buffer* buffer = qe_aggregate_resident_lookup(column, 0, rows);
  B32 narrow_known = column->agg_narrow_generation == column->write_generation;
  B32 fixed_known = column->agg_fixed_generation == column->write_generation;
  B32 key_known = column->agg_key_generation == column->write_generation;
  if (!buffer || !narrow_known || !fixed_known || !key_known)
  {
    Temp scratch = scratch_begin(&arena, 1);
    F64* values = 0;
    if (dict_codes)
    {
      values = qe_dict_codes_to_f64_dense(scratch.arena, column->dict_codes, rows);
    }
    else
    {
      values = qe_column_dense_f64(scratch.arena, column);
    }
    if (!narrow_known)
    {
      column->agg_narrow = qe_values_round_trip_f32(values, rows, &column->agg_int_bound);
      column->agg_narrow_generation = column->write_generation;
    }
    if (!fixed_known)
    {
      qe_values_fixed_point(values, rows, &column->agg_fixed_scale, &column->agg_fixed_max);
      column->agg_fixed_generation = column->write_generation;
    }
    if (!key_known)
    {
      column->agg_key_whole = qe_values_whole_range(values, rows, &column->agg_key_min, &column->agg_key_max);
      column->agg_key_generation = column->write_generation;
    }
    if (!buffer)
    {
      buffer = qe_aggregate_resident_store(column, 0, values, rows);
    }
    scratch_end(scratch);
  }
  
  *out_narrow = column->agg_narrow;
  return buffer;
}

// tec: only for a column whose values all fit in F32, qe_aggregate_full_column_f64 says which
internal GPU_Buffer*
qe_aggregate_full_column_f32(Arena* arena, GDB_Column* column, B32 dict_codes)
{
  U64 rows = column->row_count;
  GPU_Buffer* buffer = qe_aggregate_resident_lookup(column, 1, rows);
  if (!buffer)
  {
    Temp scratch = scratch_begin(&arena, 1);
    F64* values = 0;
    if (dict_codes)
    {
      values = qe_dict_codes_to_f64_dense(scratch.arena, column->dict_codes, rows);
    }
    else
    {
      values = qe_column_dense_f64(scratch.arena, column);
    }
    F32* narrow_values = qe_values_to_f32(scratch.arena, values, rows);
    buffer = qe_aggregate_resident_store(column, 1, narrow_values, rows);
    scratch_end(scratch);
  }
  return buffer;
}

// tec: dest[i] = source[selection row i], one dispatch of its own since a kernel's arguments cannot change between dispatches of one batch
internal GPU_Buffer*
qe_selection_gather_column(QE_DeviceSelection* selection, GPU_Buffer* source, B32 as_f32, String8 dest_key)
{
  if (!selection || !selection->valid)
  {
    return 0;
  }
  // tec: no row buffer means every row of the table in order, which is what the source already holds
  if (!selection->rows)
  {
    return source;
  }
  U64 element_size = as_f32 ? sizeof(F32) : sizeof(F64);
  GPU_Buffer* dest = gpu_buffer_alloc_pooled(dest_key, Max(selection->count, (U64)1) * element_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Kernel* kernel = gpu_kernel_alloc(str8_lit("selection_gather"));
  if (!dest || !source || !kernel)
  {
    if (kernel)
    {
      gpu_kernel_release(kernel);
    }
    return 0;
  }
  
  gpu_kernel_set_arg_buffer(kernel, 0, selection->rows);
  gpu_kernel_set_arg_buffer(kernel, 1, source);
  gpu_kernel_set_arg_buffer(kernel, 2, dest);
  gpu_kernel_set_arg_buffer(kernel, 3, selection->rows);
  gpu_kernel_set_arg_u64(kernel, 0, selection->count);
  gpu_kernel_set_arg_u64(kernel, 1, selection->stride_words);
  gpu_kernel_set_arg_u64(kernel, 2, selection->row_offset);
  gpu_kernel_set_arg_u64(kernel, 3, as_f32 ? 1 : 0);
  gpu_kernel_set_arg_u64(kernel, 4, 0);
  gpu_kernel_set_arg_u64(kernel, 5, 0);
  gpu_kernel_set_arg_u64(kernel, 6, 0);
  
  GPU_Batch* batch = gpu_batch_begin(0, 0);
  gpu_batch_kernel_execute(batch, kernel, (U32)selection->count, QE_GPU_WORKGROUP_SIZE);
  B32 ok = gpu_batch_end(batch);
  gpu_kernel_release(kernel);
  return ok ? dest : 0;
}

// tec: the table rows of a few selected positions, for reading group key values back
internal B32
qe_selection_rows_for_positions(QE_DeviceSelection* selection, U32* positions, U64 count, U64* out_rows)
{
  if (count == 0)
  {
    return 1;
  }
  if (!selection->rows)
  {
    for (U64 i = 0; i < count; i++)
    {
      out_rows[i] = selection->row_offset + positions[i];
    }
    return 1;
  }
  
  GPU_Buffer* position_buffer = gpu_buffer_alloc_pooled(str8_lit("agg_sel_positions"), count * sizeof(U32), GPU_BufferFlag_Write, 0);
  GPU_Buffer* dest = gpu_buffer_alloc_pooled(str8_lit("agg_sel_position_rows"), count * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Kernel* kernel = gpu_kernel_alloc(str8_lit("selection_gather"));
  if (!position_buffer || !dest || !kernel)
  {
    if (kernel)
    {
      gpu_kernel_release(kernel);
    }
    return 0;
  }
  
  gpu_kernel_set_arg_buffer(kernel, 0, selection->rows);
  gpu_kernel_set_arg_buffer(kernel, 1, selection->rows);
  gpu_kernel_set_arg_buffer(kernel, 2, dest);
  gpu_kernel_set_arg_buffer(kernel, 3, position_buffer);
  gpu_kernel_set_arg_u64(kernel, 0, count);
  gpu_kernel_set_arg_u64(kernel, 1, selection->stride_words);
  gpu_kernel_set_arg_u64(kernel, 2, selection->row_offset);
  gpu_kernel_set_arg_u64(kernel, 3, 2);
  gpu_kernel_set_arg_u64(kernel, 4, 0);
  gpu_kernel_set_arg_u64(kernel, 5, 0);
  gpu_kernel_set_arg_u64(kernel, 6, 0);
  
  Temp scratch = scratch_begin(0, 0);
  U32* rows32 = push_array(scratch.arena, U32, count);
  GPU_Batch* batch = gpu_batch_begin(count * sizeof(U32), count * sizeof(U32));
  gpu_batch_buffer_write(batch, position_buffer, positions, count * sizeof(U32));
  gpu_batch_kernel_execute(batch, kernel, (U32)count, QE_GPU_WORKGROUP_SIZE);
  gpu_batch_buffer_read(batch, dest, rows32, count * sizeof(U32));
  B32 ok = gpu_batch_end(batch);
  gpu_kernel_release(kernel);
  
  if (ok)
  {
    for (U64 i = 0; i < count; i++)
    {
      out_rows[i] = (rows32[i] == max_U32) ? PLAN_NULL_ROW : rows32[i];
    }
  }
  scratch_end(scratch);
  return ok;
}

// tec: dest[i] is the table row that entry i of index_buffer names, looked up through map_buffer when there is one
// the join pairs are the index buffer, and the map is the row list of a filtered side or of an earlier join
internal GPU_Buffer*
qe_rows_resolve(GPU_Buffer* index_buffer, U32 index_stride, U32 index_word, GPU_Buffer* map_buffer, U32 map_stride, U64 offset, U64 count, String8 dest_key)
{
  GPU_Buffer* dest = gpu_buffer_alloc_pooled(dest_key, Max(count, (U64)1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Kernel* kernel = gpu_kernel_alloc(str8_lit("selection_gather"));
  if (!dest || !kernel)
  {
    if (kernel)
    {
      gpu_kernel_release(kernel);
    }
    return 0;
  }
  
  gpu_kernel_set_arg_buffer(kernel, 0, index_buffer);
  gpu_kernel_set_arg_buffer(kernel, 1, map_buffer ? map_buffer : index_buffer);
  gpu_kernel_set_arg_buffer(kernel, 2, dest);
  gpu_kernel_set_arg_buffer(kernel, 3, index_buffer);
  gpu_kernel_set_arg_u64(kernel, 0, count);
  gpu_kernel_set_arg_u64(kernel, 1, index_stride);
  gpu_kernel_set_arg_u64(kernel, 2, map_buffer ? 0 : offset);
  gpu_kernel_set_arg_u64(kernel, 3, map_buffer ? 3 : 4);
  gpu_kernel_set_arg_u64(kernel, 4, map_stride);
  gpu_kernel_set_arg_u64(kernel, 5, index_word);
  gpu_kernel_set_arg_u64(kernel, 6, map_buffer ? offset : 0);
  
  GPU_Batch* batch = gpu_batch_begin(0, 0);
  gpu_batch_kernel_execute(batch, kernel, (U32)count, QE_GPU_WORKGROUP_SIZE);
  B32 ok = gpu_batch_end(batch);
  gpu_kernel_release(kernel);
  return ok ? dest : 0;
}

internal QE_DeviceRows
qe_device_rows_from_selection(QE_DeviceSelection* selection, String8 alias)
{
  QE_DeviceRows device = {0};
  device.valid = selection->valid;
  device.count = selection->count;
  device.table_count = 1;
  device.views[0] = *selection;
  device.aliases[0] = alias;
  return device;
}

internal QE_DeviceSelection*
qe_device_view(QE_DeviceRows* device, U64 table_slot)
{
  if (!device || table_slot >= device->table_count)
  {
    return 0;
  }
  return &device->views[table_slot];
}

// tec: a stand in row set holding just the group representatives' table rows, for qe_aggregate_build_output
// representatives are rewritten to index into it and a max_U64 (no row) stays as it is
internal PLAN_RowSet
qe_device_representative_rows(Arena* arena, QE_DeviceRows* device, PLAN_RowSet* input, U64* representatives, U64 num_groups)
{
  PLAN_RowSet rows = *input;
  rows.count = num_groups;
  rows.row_indices = push_array(arena, U64*, Max(device->table_count, (U32)1));
  
  Temp scratch = scratch_begin(&arena, 1);
  U32* positions = push_array(scratch.arena, U32, Max(num_groups, (U64)1));
  for (U64 g = 0; g < num_groups; g++)
  {
    positions[g] = (representatives[g] == max_U64) ? 0 : (U32)representatives[g];
  }
  
  for (U32 t = 0; t < device->table_count; t++)
  {
    U64* table_rows = push_array(arena, U64, Max(num_groups, (U64)1));
    B32 ok = qe_selection_rows_for_positions(&device->views[t], positions, num_groups, table_rows);
    for (U64 g = 0; g < num_groups; g++)
    {
      if (!ok || representatives[g] == max_U64)
      {
        table_rows[g] = PLAN_NULL_ROW;
      }
    }
    rows.row_indices[t] = table_rows;
  }
  
  for (U64 g = 0; g < num_groups; g++)
  {
    if (representatives[g] != max_U64)
    {
      representatives[g] = g;
    }
  }
  
  scratch_end(scratch);
  return rows;
}

internal PLAN_RowSet
qe_device_rows_to_rowset(Arena* arena, QE_DeviceRows* device)
{
  PLAN_RowSet rows = {0};
  rows.table_count = device->table_count;
  rows.tables = push_array(arena, GDB_Table*, Max(device->table_count, (U32)1));
  rows.aliases = push_array(arena, String8, Max(device->table_count, (U32)1));
  rows.row_indices = push_array(arena, U64*, Max(device->table_count, (U32)1));
  rows.count = device->count;
  
  for (U32 t = 0; t < device->table_count; t++)
  {
    QE_DeviceSelection* view = &device->views[t];
    rows.tables[t] = view->table;
    rows.aliases[t] = device->aliases[t];
    
    U64* indices = push_array(arena, U64, Max(device->count, (U64)1));
    if (device->count > 0 && !view->rows)
    {
      for (U64 i = 0; i < device->count; i++)
      {
        indices[i] = view->row_offset + i;
      }
    }
    else if (device->count > 0)
    {
      Temp scratch = scratch_begin(&arena, 1);
      U64 word_count = device->count * view->stride_words;
      U32* raw = push_array(scratch.arena, U32, word_count);
      gpu_buffer_read(view->rows, raw, word_count * sizeof(U32));
      for (U64 i = 0; i < device->count; i++)
      {
        U32 index = raw[i * view->stride_words];
        indices[i] = (index == max_U32) ? PLAN_NULL_ROW : (view->row_offset + index);
      }
      scratch_end(scratch);
    }
    rows.row_indices[t] = indices;
  }
  return rows;
}

internal B32
qe_aggregate_device(Arena* arena, GDB_Database* database, QE_DeviceRows* device, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir, QE_AggregateHints* hints, QE_AggregateTrace* out_trace, PLAN_Materialized* out_result)
{
  // tec: nothing to gain below the size where the aggregate runs on the CPU, and an empty input has its own answers
  if (device->count == 0 || (hints && hints->cpu_max_rows > 0 && device->count <= hints->cpu_max_rows))
  {
    return 0;
  }
  
  PLAN_RowSet stub = {0};
  stub.table_count = device->table_count;
  stub.tables = push_array(arena, GDB_Table*, device->table_count);
  stub.aliases = push_array(arena, String8, device->table_count);
  stub.row_indices = push_array(arena, U64*, device->table_count);
  stub.count = device->count;
  for (U32 t = 0; t < device->table_count; t++)
  {
    stub.tables[t] = device->views[t].table;
    stub.aliases[t] = device->aliases[t];
  }
  
  B32 needs_host_rows = 0;
  *out_result = qe_aggregate_impl(arena, database, &stub, group_by_ir, column_list_ir, having_ir, hints, out_trace, device, &needs_host_rows);
  return !needs_host_rows;
}

internal THREAD_POOL_TASK_FUNC(qe_string_gather_length_task)
{
  QE_StringGatherTask* task = (QE_StringGatherTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 i = range.min; i < range.max; i++)
  {
    U64 row = task->table_rows[i];
    U64 length = 0;
    if (row != PLAN_NULL_ROW)
    {
      if (task->src_offsets_lead_with_zero)
      {
        length = task->src_offsets[row + 1] - task->src_offsets[row];
      }
      else
      {
        U64 start = (row > 0) ? task->src_offsets[row - 1] : 0;
        length = task->src_offsets[row] - start;
      }
    }
    task->out_offsets[i + 1] = length;
  }
}

internal THREAD_POOL_TASK_FUNC(qe_string_gather_copy_task)
{
  QE_StringGatherTask* task = (QE_StringGatherTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 i = range.min; i < range.max; i++)
  {
    U64 row = task->table_rows[i];
    U64 length = task->out_offsets[i + 1] - task->out_offsets[i];
    if (length > 0)
    {
      U64 start = task->src_offsets_lead_with_zero ? task->src_offsets[row] : ((row > 0) ? task->src_offsets[row - 1] : 0);
      MemoryCopy(task->out_data + task->out_offsets[i], task->src_data + start, length);
    }
  }
}

internal GDB_StringDataChunk
qe_gather_string_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column)
{
  ProfBeginFunction();
  
  GDB_StringDataChunk chunk = {0};
  chunk.row_count = rows->count;
  
  if (table_slot >= rows->table_count)
  {
    log_error("qe_gather_string_column: slot %llu is not part of this row set", table_slot);
    chunk.offsets = push_array(arena, U64, rows->count + 1);
    ProfEnd();
    return chunk;
  }
  
  U64* table_rows = rows->row_indices[table_slot];
  
  // tec: a column is read in place, the chunk path below would build offsets for the whole touched span
  U8* mapped_view = 0;
  Rng1U64 mapped_range = {0};
  U64* src_offsets = column->offsets;
  U8* src_data = column->data;
  OS_Handle mapped_file_map = {0};
  B32 use_resident_copy = column->is_disk_backed && column->agg_str_data && column->agg_str_offsets &&
    column->agg_str_generation == column->write_generation && column->agg_str_data_size > 0;
  if (use_resident_copy)
  {
    // tec: the aggregate already holds the whole column in memory, offsets lead with a zero
    src_offsets = column->agg_str_offsets;
    src_data = (U8*)column->agg_str_data;
  }
  else if (column->is_disk_backed && rows->count > 0)
  {
    OS_Handle file = column->file;
    B32 opened_file = 0;
    if (os_handle_match(os_handle_zero(), file))
    {
      file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column->disk_path);
      opened_file = 1;
    }
    if (os_handle_match(os_handle_zero(), column->file_map) && !os_handle_match(os_handle_zero(), file))
    {
      OS_Handle backing_file = os_file_open(OS_AccessFlag_Read | OS_AccessFlag_ShareRead | OS_AccessFlag_ShareWrite, column->disk_path);
      column->file_map = os_file_map_open(OS_AccessFlag_Read, backing_file);
      column->file_map_backing_file = backing_file;
    }
    if (!os_handle_match(os_handle_zero(), file) && !os_handle_match(os_handle_zero(), column->file_map))
    {
      U64 variable_capacity = 0;
      os_file_read(file, r1u64(0, sizeof(U64)), &variable_capacity);
      U64 file_size = os_properties_from_file(file).size;
      mapped_range = r1u64(sizeof(U64), file_size);
      mapped_file_map = column->file_map;
      mapped_view = (U8*)os_file_map_view_open(mapped_file_map, OS_AccessFlag_Read, mapped_range);
      if (mapped_view)
      {
        src_data = mapped_view;
        src_offsets = (U64*)(mapped_view + variable_capacity);
      }
    }
    if (opened_file)
    {
      os_file_close(file);
    }
  }
  
  if (src_offsets && src_data && rows->count > 0 && (!column->is_disk_backed || mapped_view || use_resident_copy))
  {
    Temp scratch = scratch_begin(&arena, 1);
    U64 task_count = 1;
    
    QE_StringGatherTask task = {0};
    task.ranges = qe_agg_split_work(scratch.arena, rows->count, &task_count);
    task.table_rows = table_rows;
    task.src_offsets = src_offsets;
    task.src_data = src_data;
    task.src_offsets_lead_with_zero = use_resident_copy;
    task.out_offsets = push_array_no_zero(arena, U64, rows->count + 1);
    task.out_offsets[0] = 0;
    qe_agg_run_tasks(task_count, qe_string_gather_length_task, &task);
    
    for (U64 i = 0; i < rows->count; i++)
    {
      task.out_offsets[i + 1] += task.out_offsets[i];
    }
    U64 total_size = task.out_offsets[rows->count];
    task.out_data = push_array_no_zero(arena, U8, Max(total_size, 1));
    qe_agg_run_tasks(task_count, qe_string_gather_copy_task, &task);
    
    chunk.offsets = task.out_offsets;
    chunk.data = task.out_data;
    chunk.size = total_size;
    if (mapped_view)
    {
      os_file_map_view_close(mapped_file_map, mapped_view, mapped_range);
    }
    scratch_end(scratch);
    ProfEnd();
    return chunk;
  }
  
  U64 min_row = max_U64;
  U64 max_row = 0;
  for (U64 i = 0; i < rows->count; i++)
  {
    U64 row = table_rows[i];
    if (row == PLAN_NULL_ROW) continue;
    if (row < min_row) min_row = row;
    if (row > max_row) max_row = row;
  }
  
  chunk.offsets = push_array(arena, U64, rows->count + 1);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  GDB_StringDataChunk src = {0};
  if (min_row != max_U64)
  {
    src = gdb_column_get_string_chunk(scratch.arena, column, r1u64(min_row, max_row + 1));
  }
  
  // tec: rows are pulled in whatever order a prior GPU pass, so this needs a per row reorder
  U64 total_size = 0;
  for (U64 i = 0; i < rows->count; i++)
  {
    U64 row = table_rows[i];
    if (row != PLAN_NULL_ROW && src.data)
    {
      U64 local = row - min_row;
      total_size += src.offsets[local + 1] - src.offsets[local];
    }
  }
  
  U8* data = push_array(arena, U8, Max(total_size, 1));
  U64 cursor = 0;
  chunk.offsets[0] = 0;
  for (U64 i = 0; i < rows->count; i++)
  {
    U64 row = table_rows[i];
    U64 len = 0;
    if (row != PLAN_NULL_ROW && src.data)
    {
      U64 local = row - min_row;
      U64 start = src.offsets[local];
      len = src.offsets[local + 1] - start;
      MemoryCopy(data + cursor, (U8*)src.data + start, len);
    }
    cursor += len;
    chunk.offsets[i + 1] = cursor;
  }
  
  chunk.data = data;
  chunk.size = total_size;
  
  if (src.data)
  {
    gdb_column_close_string_chunk(column);
  }
  
  scratch_end(scratch);
  ProfEnd();
  return chunk;
}

//~ tec: sort

internal S32
qe_str8_compare(String8 a, String8 b)
{
  U64 min_size = Min(a.size, b.size);
  S32 cmp = min_size ? (S32)MemoryCompare(a.str, b.str, min_size) : 0;
  if (cmp != 0) return cmp;
  if (a.size < b.size) return -1;
  if (a.size > b.size) return 1;
  return 0;
}

internal int
qe_sort_rows_compare_ctx(QE_SortRowsCtx* ctx, U64 ia, U64 ib)
{
  for (U32 k = 0; k < ctx->num_keys; k++)
  {
    S32 cmp = 0;
    
    if (ctx->key_is_string[k])
    {
      GDB_StringDataChunk* chunk = &ctx->string_keys[k];
      String8 sa = str8((U8*)chunk->data + chunk->offsets[ia], chunk->offsets[ia + 1] - chunk->offsets[ia]);
      String8 sb = str8((U8*)chunk->data + chunk->offsets[ib], chunk->offsets[ib + 1] - chunk->offsets[ib]);
      cmp = qe_str8_compare(sa, sb);
    }
    else
    {
      F64 va = ctx->numeric_keys[k][ia];
      F64 vb = ctx->numeric_keys[k][ib];
      cmp = (va < vb) ? -1 : (va > vb) ? 1 : 0;
    }
    
    if (cmp != 0) return ctx->key_desc[k] ? -cmp : cmp;
  }
  return 0;
}

internal int
qe_sort_rows_compare(const void* a, const void* b)
{
  return qe_sort_rows_compare_ctx(g_qe_sort_rows_ctx, *(const U64*)a, *(const U64*)b);
}

// tec: ties fall back to the position so the order is total, which makes the heap and the merge agree with a stable sort
internal B32
qe_order_before(QE_OrderLessFn* less, void* context, U64 a, U64 b)
{
  if (less(context, a, b))
  {
    return 1;
  }
  if (less(context, b, a))
  {
    return 0;
  }
  return a < b;
}

// tec: bottom up merge sort
internal void
qe_order_merge_sort(U64* order, U64* buffer, U64 count, QE_OrderLessFn* less, void* context)
{
  U64* source = order;
  U64* target = buffer;
  for (U64 width = 1; width < count; width *= 2)
  {
    for (U64 start = 0; start < count; start += 2 * width)
    {
      U64 middle = Min(start + width, count);
      U64 end = Min(start + 2 * width, count);
      U64 left = start;
      U64 right = middle;
      U64 out = start;
      while (left < middle && right < end)
      {
        if (qe_order_before(less, context, source[right], source[left]))
        {
          target[out] = source[right];
          right += 1;
        }
        else
        {
          target[out] = source[left];
          left += 1;
        }
        out += 1;
      }
      while (left < middle)
      {
        target[out] = source[left];
        left += 1;
        out += 1;
      }
      while (right < end)
      {
        target[out] = source[right];
        right += 1;
        out += 1;
      }
    }
    
    U64* swap = source;
    source = target;
    target = swap;
  }
  
  if (source != order)
  {
    MemoryCopy(order, source, count * sizeof(U64));
  }
}

// tec: max heap, the root is the last row of the ones kept so far
internal void
qe_order_sift_down(U64* heap, U64 count, U64 root, QE_OrderLessFn* less, void* context)
{
  for (;;)
  {
    U64 child = root * 2 + 1;
    if (child >= count)
    {
      return;
    }
    if (child + 1 < count && qe_order_before(less, context, heap[child], heap[child + 1]))
    {
      child += 1;
    }
    if (!qe_order_before(less, context, heap[root], heap[child]))
    {
      return;
    }
    
    U64 swap = heap[root];
    heap[root] = heap[child];
    heap[child] = swap;
    root = child;
  }
}

// tec: leaves the first 'keep' entries of order holding the keep smallest positions in sorted order, buffer needs keep entries
internal U64
qe_order_select_top(U64* order, U64* buffer, U64 count, U64 keep, QE_OrderLessFn* less, void* context)
{
  keep = Min(keep, count);
  for (U64 root = keep / 2; root > 0; root -= 1)
  {
    qe_order_sift_down(order, keep, root - 1, less, context);
  }
  
  for (U64 index = keep; index < count; index += 1)
  {
    if (qe_order_before(less, context, order[index], order[0]))
    {
      order[0] = order[index];
      qe_order_sift_down(order, keep, 0, less, context);
    }
  }
  
  qe_order_merge_sort(order, buffer, keep, less, context);
  return keep;
}

internal B32
qe_sort_rows_less(void* context, U64 a, U64 b)
{
  return qe_sort_rows_compare_ctx((QE_SortRowsCtx*)context, a, b) < 0;
}

// tec: the heap starts as the first keep positions of the range, so no source array is needed
internal U64
qe_order_select_top_range(U64* heap, U64* buffer, U64 start, U64 end, U64 keep, QE_OrderLessFn* less, void* context)
{
  keep = Min(keep, end - start);
  for (U64 i = 0; i < keep; i += 1)
  {
    heap[i] = start + i;
  }
  for (U64 root = keep / 2; root > 0; root -= 1)
  {
    qe_order_sift_down(heap, keep, root - 1, less, context);
  }
  
  // tec: index is later than every position in the heap, so a tie never displaces one and a single less call is enough
  for (U64 index = start + keep; index < end; index += 1)
  {
    if (less(context, index, heap[0]))
    {
      heap[0] = index;
      qe_order_sift_down(heap, keep, 0, less, context);
    }
  }
  
  qe_order_merge_sort(heap, buffer, keep, less, context);
  return keep;
}

// tec: one numeric key, compare against the heap root's value directly instead of the generic comparison
internal U64
qe_order_select_top_range_numeric(U64* heap, U64* buffer, U64 start, U64 end, U64 keep, F64* values, B32 descending, QE_OrderLessFn* less, void* context)
{
  keep = Min(keep, end - start);
  for (U64 i = 0; i < keep; i += 1)
  {
    heap[i] = start + i;
  }
  for (U64 root = keep / 2; root > 0; root -= 1)
  {
    qe_order_sift_down(heap, keep, root - 1, less, context);
  }
  
  if (keep > 0)
  {
    F64 root_value = values[heap[0]];
    for (U64 index = start + keep; index < end; index += 1)
    {
      F64 value = values[index];
      B32 displaces = descending ? (value > root_value) : (value < root_value);
      if (displaces)
      {
        heap[0] = index;
        qe_order_sift_down(heap, keep, 0, less, context);
        root_value = values[heap[0]];
      }
    }
  }
  
  qe_order_merge_sort(heap, buffer, keep, less, context);
  return keep;
}

internal THREAD_POOL_TASK_FUNC(qe_top_k_task)
{
  QE_TopKTask* task = (QE_TopKTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  QE_SortRowsCtx* ctx = task->ctx;
  if (ctx->num_keys == 1 && !ctx->key_is_string[0])
  {
    task->task_kept[task_id] = qe_order_select_top_range_numeric(task->heaps + task_id * task->keep, task->buffers + task_id * task->keep,
                                                                 range.min, range.max, task->keep, ctx->numeric_keys[0], ctx->key_desc[0], qe_sort_rows_less, ctx);
    return;
  }
  task->task_kept[task_id] = qe_order_select_top_range(task->heaps + task_id * task->keep, task->buffers + task_id * task->keep,
                                                       range.min, range.max, task->keep, qe_sort_rows_less, ctx);
}

// tec: every row in the global top keep is in the top keep of its own range and ties break on position, so this matches the single pass
internal U64
qe_sort_rows_top_k_parallel(Arena* arena, QE_SortRowsCtx* ctx, U64 count, U64 keep, U64* out_order, U32* out_tasks)
{
  TP_Context* pool = app_thread_pool();
  U64 max_tasks = Max((U64)1, count / Max((U64)QE_TOP_K_MIN_ROWS_PER_TASK, keep * 4));
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, max_tasks));
  *out_tasks = (U32)task_count;
  
  Temp scratch = scratch_begin(&arena, 1);
  
  QE_TopKTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.ctx = ctx;
  task.keep = keep;
  task.heaps = push_array(scratch.arena, U64, task_count * keep);
  task.buffers = push_array(scratch.arena, U64, task_count * keep);
  task.task_kept = push_array(scratch.arena, U64, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_top_k_task, &task);
  tp_temp_end(temp);
  
  U64 candidate_count = 0;
  U64* candidates = push_array(scratch.arena, U64, task_count * keep);
  for (U64 t = 0; t < task_count; t++)
  {
    MemoryCopy(candidates + candidate_count, task.heaps + t * keep, task.task_kept[t] * sizeof(U64));
    candidate_count += task.task_kept[t];
  }
  
  U64* buffer = push_array(scratch.arena, U64, keep);
  U64 out_count = qe_order_select_top(candidates, buffer, candidate_count, keep, qe_sort_rows_less, ctx);
  MemoryCopy(out_order, candidates, out_count * sizeof(U64));
  
  scratch_end(scratch);
  return out_count;
}

//~ tec: top k over numeric columns, read in place

internal F64
qe_native_numeric_value(GDB_ColumnType type, void* data)
{
  switch (type)
  {
    case GDB_ColumnType_U32: return (F64)(*(U32*)data);
    case GDB_ColumnType_U64: return (F64)(*(U64*)data);
    case GDB_ColumnType_F32: return (F64)(*(F32*)data);
    case GDB_ColumnType_F64: return *(F64*)data;
    case GDB_ColumnType_Bool: return (F64)(*(U8*)data);
    case GDB_ColumnType_I32: return (F64)(*(S32*)data);
    case GDB_ColumnType_I64: return (F64)(*(S64*)data);
    case GDB_ColumnType_Date: return (F64)(*(S32*)data);
    case GDB_ColumnType_Timestamp: return (F64)(*(S64*)data);
    case GDB_ColumnType_Decimal: return (F64)(*(S64*)data);
    case GDB_ColumnType_Enum: return (F64)(*(U32*)data);
    default: return 0.0;
  }
}

internal F64
qe_top_k_key_value(QE_TopKKey* key, U64 position)
{
  U64 row = key->table_rows[position];
  if (row == PLAN_NULL_ROW || !key->base_ptr)
  {
    return 0.0;
  }
  return qe_native_numeric_value(key->column_type, (U8*)key->base_ptr + (row - key->min_row) * key->column_size);
}

// tec: negative when a sorts in front of b, same key order the gathered comparison uses
internal S32
qe_top_k_native_compare(QE_TopKNativeTask* task, U64 a, U64 b)
{
  for (U32 k = 0; k < task->num_keys; k += 1)
  {
    F64 va = qe_top_k_key_value(&task->keys[k], a);
    F64 vb = qe_top_k_key_value(&task->keys[k], b);
    S32 cmp = (va < vb) ? -1 : (va > vb) ? 1 : 0;
    if (cmp != 0)
    {
      return task->keys[k].descending ? -cmp : cmp;
    }
  }
  return 0;
}

// tec: ties fall back to the position so the order is total
internal B32
qe_top_k_native_before(QE_TopKNativeTask* task, U64 a, U64 b)
{
  S32 cmp = qe_top_k_native_compare(task, a, b);
  return cmp < 0 || (cmp == 0 && a < b);
}

// tec: the root is the position every other position comes before
internal void
qe_top_k_native_sift_down(QE_TopKNativeTask* task, U64* heap, U64 count, U64 root)
{
  for (;;)
  {
    U64 child = root * 2 + 1;
    if (child >= count)
    {
      return;
    }
    if (child + 1 < count && qe_top_k_native_before(task, heap[child], heap[child + 1]))
    {
      child += 1;
    }
    if (!qe_top_k_native_before(task, heap[root], heap[child]))
    {
      return;
    }
    U64 swap = heap[root];
    heap[root] = heap[child];
    heap[child] = swap;
    root = child;
  }
}

internal void
qe_top_k_native_heapify(QE_TopKNativeTask* task, U64* heap, U64 count)
{
  for (U64 root = count / 2; root > 0; root -= 1)
  {
    qe_top_k_native_sift_down(task, heap, count, root - 1);
  }
}

// tec: leaves the first position in front of the second and so on
internal void
qe_top_k_native_sort(QE_TopKNativeTask* task, U64* heap, U64 count)
{
  for (U64 end = count; end > 1; end -= 1)
  {
    U64 swap = heap[0];
    heap[0] = heap[end - 1];
    heap[end - 1] = swap;
    qe_top_k_native_sift_down(task, heap, end - 1, 0);
  }
}

internal THREAD_POOL_TASK_FUNC(qe_top_k_native_task)
{
  QE_TopKNativeTask* task = (QE_TopKNativeTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  U64* heap = task->heaps + task_id * task->keep;
  U64 keep = Min(task->keep, range.max - range.min);
  QE_TopKKey* lead = &task->keys[0];
  B32 single_key = task->num_keys == 1;
  
  for (U64 i = 0; i < keep; i += 1)
  {
    heap[i] = range.min + i;
  }
  qe_top_k_native_heapify(task, heap, keep);
  
  if (keep > 0)
  {
    F64 root_value = qe_top_k_key_value(lead, heap[0]);
    for (U64 position = range.min + keep; position < range.max; position += 1)
    {
      // tec: the leading key decides nearly every row, later keys are only read on a tie
      F64 value = qe_top_k_key_value(lead, position);
      S32 lead_cmp = (value < root_value) ? -1 : (value > root_value) ? 1 : 0;
      if (lead->descending)
      {
        lead_cmp = -lead_cmp;
      }
      
      B32 displaces = 0;
      if (lead_cmp < 0)
      {
        displaces = 1;
      }
      else if (lead_cmp == 0 && !single_key)
      {
        displaces = qe_top_k_native_before(task, position, heap[0]);
      }
      
      if (displaces)
      {
        heap[0] = position;
        qe_top_k_native_sift_down(task, heap, keep, 0);
        root_value = qe_top_k_key_value(lead, heap[0]);
      }
    }
  }
  
  qe_top_k_native_sort(task, heap, keep);
  task->task_kept[task_id] = keep;
}

internal B32
qe_top_k_native_supported(PLAN_RowSet* rows, U32 num_keys, B32* key_is_string, F64** key_fuzzy_scores, U64* key_slots)
{
  for (U32 k = 0; k < num_keys; k += 1)
  {
    if (key_is_string[k] || key_fuzzy_scores[k] || key_slots[k] >= rows->table_count || rows->row_indices[key_slots[k]] == 0)
    {
      return 0;
    }
  }
  return num_keys > 0;
}

internal U64
qe_sort_rows_top_k_native(Arena* arena, PLAN_RowSet* rows, U32 num_keys, U64* key_slots, GDB_Column** key_columns, B32* key_desc, U64 keep, U64* out_order, U32* out_tasks)
{
  U64 count = rows->count;
  Temp scratch = scratch_begin(&arena, 1);
  
  QE_TopKNativeTask task = {0};
  task.num_keys = num_keys;
  task.keep = keep;
  for (U32 k = 0; k < num_keys; k += 1)
  {
    QE_TopKKey* key = &task.keys[k];
    key->table_rows = rows->row_indices[key_slots[k]];
    key->column_type = key_columns[k]->type;
    key->column_size = key_columns[k]->size;
    key->descending = key_desc[k];
    
    U64 min_row = max_U64;
    U64 max_row = 0;
    qe_row_bounds(key->table_rows, count, &min_row, &max_row);
    key->min_row = min_row;
    if (min_row != max_U64)
    {
      U64 range_size = 0;
      key->base_ptr = gdb_column_get_data_range(scratch.arena, key_columns[k], r1u64(min_row, max_row + 1), &range_size);
    }
  }
  
  TP_Context* pool = app_thread_pool();
  U64 max_tasks = Max((U64)1, count / Max((U64)QE_TOP_K_MIN_ROWS_PER_TASK, keep * 4));
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, max_tasks));
  *out_tasks = (U32)task_count;
  
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.heaps = push_array(scratch.arena, U64, task_count * keep);
  task.task_kept = push_array(scratch.arena, U64, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_top_k_native_task, &task);
  tp_temp_end(temp);
  
  // tec: the global top keep is among the per range winners, so merging them matches the single pass
  U64* merged = push_array(scratch.arena, U64, keep);
  U64 merged_count = 0;
  for (U64 t = 0; t < task_count; t += 1)
  {
    U64* heap = task.heaps + t * keep;
    for (U64 i = 0; i < task.task_kept[t]; i += 1)
    {
      if (merged_count < keep)
      {
        merged[merged_count] = heap[i];
        merged_count += 1;
        if (merged_count == keep)
        {
          qe_top_k_native_heapify(&task, merged, keep);
        }
      }
      else if (qe_top_k_native_before(&task, heap[i], merged[0]))
      {
        merged[0] = heap[i];
        qe_top_k_native_sift_down(&task, merged, keep, 0);
      }
    }
  }
  qe_top_k_native_sort(&task, merged, merged_count);
  MemoryCopy(out_order, merged, merged_count * sizeof(U64));
  
  scratch_end(scratch);
  return merged_count;
}

internal PLAN_RowSet
qe_sort_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* order_by_ir, QE_SortHints* hints, QE_SortTrace* out_trace)
{
  ProfBeginFunction();
  
  PLAN_RowSet result = *rows;
  if (!order_by_ir || rows->count <= 1)
  {
    ProfEnd();
    return result;
  }
  
  if (rows->table_count > QE_SORT_MAX_TABLES)
  {
    log_error("qe_sort_rows: sorting a %llu-way join is not supported (max %u tables), returning unsorted",
              rows->table_count, (U32)QE_SORT_MAX_TABLES);
    ProfEnd();
    return result;
  }
  
  GDB_Column* key_columns[QE_SORT_MAX_KEYS];
  U64 key_slots[QE_SORT_MAX_KEYS];
  B32 key_desc[QE_SORT_MAX_KEYS];
  B32 key_is_string[QE_SORT_MAX_KEYS];
  F64* key_fuzzy_scores[QE_SORT_MAX_KEYS];
  U32 num_keys = 0;
  B32 any_string_key = 0;
  B32 any_fuzzy_key = 0;
  
  for (IR_Node* col_node = order_by_ir->first; col_node != NULL; col_node = col_node->next)
  {
    if (num_keys >= QE_SORT_MAX_KEYS)
    {
      log_error("qe_sort_rows: more than %u ORDER BY columns is not supported, ignoring the rest", (U32)QE_SORT_MAX_KEYS);
      break;
    }
    
    key_fuzzy_scores[num_keys] = 0;
    
    B32 is_distance = 0;
    if (qe_ir_is_fuzzy_call(col_node, &is_distance))
    {
      if (rows->table_count != 1)
      {
        log_error("qe_sort_rows: ORDER BY %.*s() is only supported over a single base table, ignoring", str8_varg(col_node->value));
        continue;
      }
      
      B32 desc = (col_node->last && col_node->last->type == IR_NodeType_Descending);
      
      IR_Node* order_col_arg = col_node->first;
      IR_Node* order_needle_arg = order_col_arg ? order_col_arg->next : 0;
      
      B32 scores_match = rows->scores && order_col_arg && order_needle_arg &&
        rows->score_is_distance == is_distance &&
        str8_match(rows->score_column_name, order_col_arg->value, 0) &&
        str8_match(rows->score_needle, order_needle_arg->value, 0);
      F64* scores = scores_match ? rows->scores : 0;
      if (!scores)
      {
        scores = push_array(arena, F64, Max(rows->count, 1));
        for (U64 i = 0; i < rows->count; i++)
        {
          scores[i] = qe_row_eval_fuzzy_call(arena, rows, col_node, i, is_distance);
        }
      }
      
      key_fuzzy_scores[num_keys] = scores;
      key_desc[num_keys] = desc;
      any_fuzzy_key = 1;
      num_keys++;
      continue;
    }
    
    String8 bare_name = {0};
    U64 slot = max_U64;
    GDB_Table* table = qe_resolve_column_table(rows, col_node->value, &bare_name, &slot);
    if (!table) continue; // tec: already logged by qe_resolve_column_table
    
    GDB_Column* column = gdb_table_find_column(table, bare_name);
    if (!column) continue;
    
    B32 desc = (col_node->first && col_node->first->type == IR_NodeType_Descending);
    B32 is_string = (column->type == GDB_ColumnType_String8);
    any_string_key |= is_string;
    
    key_slots[num_keys] = slot;
    key_columns[num_keys] = column;
    key_desc[num_keys] = desc;
    key_is_string[num_keys] = is_string;
    num_keys++;
  }
  
  if (num_keys == 0)
  {
    log_error("qe_sort_rows: no usable ORDER BY columns, returning input unsorted");
    ProfEnd();
    return result;
  }
  
  U64 real_count = rows->count;
  
  B32 top_k_active = hints && hints->top_k > 0 && hints->top_k < real_count;
  B32 small_input = hints && hints->gpu_min_rows > 0 && real_count < hints->gpu_min_rows;
  
  // tec: no GPU string comparison kernel exists, so a string or fuzzy score key sorts a plain row idx arr on the CPU instead of the bitonic path below
  if (any_string_key || any_fuzzy_key || top_k_active || small_input)
  {
    Temp scratch = scratch_begin(&arena, 1);
    U64 sort_t_start = os_now_microseconds();
    
    // tec: the parallel top k only ever hands back keep positions, so it never needs an entry per row
    B32 parallel_top_k = top_k_active && real_count >= QE_TOP_K_PARALLEL_MIN_ROWS && app_thread_pool()->worker_count > 1;
    // tec: a top k over numeric columns reads them in place instead of gathering all of each first
    B32 native_top_k = parallel_top_k && qe_top_k_native_supported(rows, num_keys, key_is_string, key_fuzzy_scores, key_slots);
    
    QE_SortRowsCtx ctx = {0};
    ctx.num_keys = num_keys;
    for (U32 k = 0; k < num_keys && !native_top_k; k++)
    {
      ctx.key_desc[k] = key_desc[k];
      if (key_fuzzy_scores[k])
      {
        ctx.key_is_string[k] = 0;
        ctx.numeric_keys[k] = key_fuzzy_scores[k];
      }
      else if (key_is_string[k])
      {
        ctx.key_is_string[k] = 1;
        ctx.string_keys[k] = qe_gather_string_column(scratch.arena, rows, key_slots[k], key_columns[k]);
      }
      else
      {
        ctx.key_is_string[k] = 0;
        ctx.numeric_keys[k] = qe_gather_numeric_column(scratch.arena, rows, key_slots[k], key_columns[k]);
      }
    }
    
    U64 sort_t_gathered = os_now_microseconds();
    U64* order = push_array(scratch.arena, U64, parallel_top_k ? hints->top_k : real_count);
    if (!parallel_top_k)
    {
      for (U64 i = 0; i < real_count; i++) order[i] = i;
    }
    
    U64 out_count = real_count;
    U32 top_k_tasks = 0;
    if (top_k_active)
    {
      if (native_top_k)
      {
        out_count = qe_sort_rows_top_k_native(scratch.arena, rows, num_keys, key_slots, key_columns, key_desc, hints->top_k, order, &top_k_tasks);
      }
      else if (parallel_top_k)
      {
        out_count = qe_sort_rows_top_k_parallel(scratch.arena, &ctx, real_count, hints->top_k, order, &top_k_tasks);
      }
      else
      {
        U64* buffer = push_array(scratch.arena, U64, hints->top_k);
        out_count = qe_order_select_top(order, buffer, real_count, hints->top_k, qe_sort_rows_less, &ctx);
      }
    }
    else
    {
      QE_SortRowsCtx* prev_ctx = g_qe_sort_rows_ctx;
      g_qe_sort_rows_ctx = &ctx;
      quick_sort(order, real_count, sizeof(U64), qe_sort_rows_compare);
      g_qe_sort_rows_ctx = prev_ctx;
    }
    
    U64 sort_t_selected = os_now_microseconds();
    log_debug("qe_sort_rows: rows=%llu kept=%llu native=%d cpu phases (us): gather=%llu select=%llu", real_count, out_count, (int)native_top_k, sort_t_gathered - sort_t_start, sort_t_selected - sort_t_gathered);
    
    result.count = out_count;
    result.row_indices = push_array(arena, U64*, rows->table_count);
    for (U64 t = 0; t < rows->table_count; t++)
    {
      result.row_indices[t] = push_array(arena, U64, Max(out_count, 1));
      for (U64 i = 0; i < out_count; i++)
      {
        result.row_indices[t][i] = rows->row_indices[t][order[i]];
      }
    }
    
    if (rows->scores)
    {
      F64* sorted_scores = push_array(arena, F64, Max(out_count, 1));
      for (U64 i = 0; i < out_count; i++)
      {
        sorted_scores[i] = rows->scores[order[i]];
      }
      result.scores = sorted_scores;
    }
    
    if (out_trace)
    {
      out_trace->row_count = real_count;
      out_trace->kept_rows = out_count;
      out_trace->used_cpu = 1;
      out_trace->top_k_tasks = top_k_tasks;
    }
    
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  U64 padded_count = 2;
  while (padded_count < real_count) padded_count <<= 1;
  
  U32 dir_mask = 0;
  for (U32 k = 0; k < num_keys; k++) if (key_desc[k]) dir_mask |= (1u << k);
  
  Temp scratch = scratch_begin(&arena, 1);
  
  F64* gathered[QE_SORT_MAX_KEYS] = {0};
  for (U32 k = 0; k < num_keys; k++)
  {
    gathered[k] = qe_gather_numeric_column(scratch.arena, rows, key_slots[k], key_columns[k]);
  }
  
  // tec: keys that pack into one 64 bit key sort in far fewer passes than the general network below
  QE_CompositeSort packed = qe_sort_composite_gpu(scratch.arena, gathered, key_desc, num_keys, real_count);
  if (packed.ok)
  {
    result.count = real_count;
    result.row_indices = push_array(arena, U64*, rows->table_count);
    for (U64 t = 0; t < rows->table_count; t++)
    {
      result.row_indices[t] = push_array(arena, U64, real_count);
      for (U64 i = 0; i < real_count; i++)
      {
        result.row_indices[t][i] = rows->row_indices[t][packed.sorted_rows[i]];
      }
    }
    if (rows->scores)
    {
      F64* sorted_scores = push_array(arena, F64, real_count);
      for (U64 i = 0; i < real_count; i++)
      {
        sorted_scores[i] = rows->scores[packed.sorted_rows[i]];
      }
      result.scores = sorted_scores;
    }
    if (out_trace)
    {
      out_trace->row_count = real_count;
      out_trace->kept_rows = real_count;
      out_trace->gpu_time_us = gpu_get_executed_kernel_time_microseconds();
    }
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  B32 use_narrow_keys = 1;
  for (U32 k = 0; k < num_keys && use_narrow_keys; k++)
  {
    for (U64 i = 0; i < real_count; i++)
    {
      F64 v = gathered[k][i];
      if ((F64)(F32)v != v) { use_narrow_keys = 0; break; }
    }
  }
  U64 keys_elem_size = use_narrow_keys ? sizeof(F32) : sizeof(F64);
  void* keys = push_array(scratch.arena, U8, padded_count * QE_SORT_MAX_KEYS * keys_elem_size);
  U32* payload = push_array(scratch.arena, U32, padded_count * 9);
  
  for (U64 i = 0; i < padded_count; i++)
  {
    B32 is_real = i < real_count;
    
    for (U32 k = 0; k < QE_SORT_MAX_KEYS; k++)
    {
      F64 v = (is_real && k < num_keys) ? gathered[k][i] : 0.0;
      if (use_narrow_keys) ((F32*)keys)[i * QE_SORT_MAX_KEYS + k] = (F32)v;
      else                 ((F64*)keys)[i * QE_SORT_MAX_KEYS + k] = v;
    }
    
    for (U32 t = 0; t < QE_SORT_MAX_TABLES; t++)
    {
      U64 row = (is_real && t < rows->table_count) ? rows->row_indices[t][i] : 0;
      payload[i * 9 + t * 2 + 0] = (U32)(row & max_U32);
      payload[i * 9 + t * 2 + 1] = (U32)(row >> 32);
    }
    payload[i * 9 + 8] = is_real ? 1u : 0u;
  }
  
  GPU_Kernel* kernel = gpu_kernel_alloc(use_narrow_keys ? str8_lit("bitonic_sort_f32") : str8_lit("bitonic_sort"));
  if (!kernel)
  {
    log_error("qe_sort_rows: failed to alloc 'bitonic_sort%s' kernel, returning unsorted", use_narrow_keys ? "_f32" : "");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  U64 keys_size = padded_count * QE_SORT_MAX_KEYS * keys_elem_size;
  U64 payload_size = padded_count * 9 * sizeof(U32);
  
  GPU_Buffer* keys_buf = gpu_buffer_alloc(keys_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* payload_buf = gpu_buffer_alloc(payload_size, GPU_BufferFlag_ReadWrite, 0);
  
  gpu_kernel_set_arg_buffer(kernel, 0, keys_buf);
  gpu_kernel_set_arg_buffer(kernel, 1, payload_buf);
  
  U32 num_stages = 0;
  while ((1ull << num_stages) < padded_count) num_stages++;
  
  GPU_Batch* sort_batch = gpu_batch_begin(keys_size + payload_size, payload_size);
  gpu_batch_buffer_write(sort_batch, keys_buf, keys, keys_size);
  gpu_batch_buffer_write(sort_batch, payload_buf, payload, payload_size);
  
  for (U32 stage = 0; stage < num_stages; stage++)
  {
    for (U32 pass_plus1 = stage + 1; pass_plus1 > 0; pass_plus1--)
    {
      U32 pass_ = pass_plus1 - 1;
      
      gpu_kernel_set_arg_u64(kernel, 0, padded_count);
      gpu_kernel_set_arg_u64(kernel, 1, num_keys);
      gpu_kernel_set_arg_u64(kernel, 2, dir_mask);
      gpu_kernel_set_arg_u64(kernel, 3, stage);
      gpu_kernel_set_arg_u64(kernel, 4, pass_);
      
      gpu_batch_kernel_execute(sort_batch, kernel, (U32)(padded_count / 2), QE_GPU_WORKGROUP_SIZE);
    }
  }
  
  U32* sorted_payload = push_array(scratch.arena, U32, padded_count * 9);
  gpu_batch_buffer_read(sort_batch, payload_buf, sorted_payload, payload_size);
  gpu_batch_end(sort_batch);
  U64 sort_gpu_time_us = gpu_get_executed_kernel_time_microseconds();
  log_debug("qe_sort_rows: bitonic sort (real_count=%llu, padded_count=%llu, num_stages=%u, keys=%s) GPU kernel time: %llu microseconds",
            real_count, padded_count, num_stages, use_narrow_keys ? "f32" : "f64", sort_gpu_time_us);
  if (out_trace)
  {
    out_trace->row_count = real_count;
    out_trace->kept_rows = real_count;
    out_trace->gpu_time_us = sort_gpu_time_us;
  }
  
  gpu_buffer_release(keys_buf);
  gpu_buffer_release(payload_buf);
  gpu_kernel_release(kernel);
  
  result.row_indices = push_array(arena, U64*, rows->table_count);
  for (U64 t = 0; t < rows->table_count; t++)
  {
    result.row_indices[t] = push_array(arena, U64, real_count);
  }
  
  for (U64 i = 0; i < real_count; i++)
  {
    for (U64 t = 0; t < rows->table_count; t++)
    {
      U64 lo = sorted_payload[i * 9 + t * 2 + 0];
      U64 hi = sorted_payload[i * 9 + t * 2 + 1];
      result.row_indices[t][i] = lo | (hi << 32);
    }
  }
  
  scratch_end(scratch);
  ProfEnd();
  return result;
}

// tec: reinterpretation to/from the aggregate_hash_reduce_f32 kernel's uint accumulator words
internal U32
qe_f32_to_bits(F32 v)
{
  U32 bits;
  MemoryCopy(&bits, &v, sizeof(bits));
  return bits;
}

internal F32
qe_bits_to_f32(U32 bits)
{
  F32 v;
  MemoryCopy(&v, &bits, sizeof(v));
  return v;
}

//~ tec: window ranking over a composite 64 bit key

internal THREAD_POOL_TASK_FUNC(qe_window_range_task)
{
  QE_WindowRangeTask* task = (QE_WindowRangeTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  S32 scale = 0;
  F64 smallest = 0.0;
  F64 largest = 0.0;
  for (U64 i = range.min; i < range.max; i++)
  {
    F64 v = task->values[i];
    F64 magnitude = v < 0.0 ? -v : v;
    if (!(magnitude < 4.0e15))
    {
      scale = -1;
      break;
    }
    if (i == range.min)
    {
      smallest = v;
      largest = v;
    }
    smallest = Min(smallest, v);
    largest = Max(largest, v);
    for (;;)
    {
      F64 scaled = v * g_qe_fixed_pow10[scale];
      if (magnitude * g_qe_fixed_pow10[scale] >= 4.0e15)
      {
        scale = QE_FIXED_MAX_SCALE + 1;
        break;
      }
      F64 whole = (F64)(S64)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
      if (whole / g_qe_fixed_pow10[scale] == v)
      {
        break;
      }
      scale += 1;
      if (scale > QE_FIXED_MAX_SCALE)
      {
        break;
      }
    }
    if (scale > QE_FIXED_MAX_SCALE)
    {
      scale = -1;
      break;
    }
  }
  task->task_scale[task_id] = scale;
  task->task_min[task_id] = smallest;
  task->task_max[task_id] = largest;
}

// tec: false when the key cannot be packed
internal B32
qe_window_key_range(F64* values, U64 count, QE_WindowKeyRange* out)
{
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_WindowRangeTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.values = values;
  task.task_scale = push_array(scratch.arena, S32, task_count);
  task.task_min = push_array(scratch.arena, F64, task_count);
  task.task_max = push_array(scratch.arena, F64, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_window_range_task, &task);
  tp_temp_end(temp);
  
  S32 scale = 0;
  F64 smallest = task.task_min[0];
  F64 largest = task.task_max[0];
  for (U64 t = 0; t < task_count; t++)
  {
    if (task.task_scale[t] < 0)
    {
      scale = -1;
      break;
    }
    scale = Max(scale, task.task_scale[t]);
    smallest = Min(smallest, task.task_min[t]);
    largest = Max(largest, task.task_max[t]);
  }
  scratch_end(scratch);
  if (scale < 0)
  {
    return 0;
  }
  
  F64 factor = g_qe_fixed_pow10[scale];
  F64 low = smallest * factor;
  F64 high = largest * factor;
  out->scale = scale;
  out->minimum = (S64)(low < 0.0 ? low - 0.5 : low + 0.5);
  out->maximum = (S64)(high < 0.0 ? high - 0.5 : high + 0.5);
  U64 span = (U64)(out->maximum - out->minimum);
  out->bits = 0;
  while (out->bits < 64 && (span >> out->bits) != 0)
  {
    out->bits += 1;
  }
  return 1;
}

internal THREAD_POOL_TASK_FUNC(qe_window_compose_task)
{
  QE_WindowComposeTask* task = (QE_WindowComposeTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 i = range.min; i < range.max; i++)
  {
    if (i >= task->real_count)
    {
      // tec: padding sorts after every real row, its row id is larger than any real one
      task->sort_keys[i * 2 + 0] = max_U32;
      task->sort_keys[i * 2 + 1] = max_U32;
      task->sort_rows[i] = max_U32;
      continue;
    }
    
    U64 composite = 0;
    for (U32 k = 0; k < task->num_keys; k++)
    {
      QE_WindowKeyRange* key_range = &task->key_ranges[k];
      F64 scaled = task->keys[k][i] * g_qe_fixed_pow10[key_range->scale];
      S64 q = (S64)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
      U64 offset = task->descending[k] ? (U64)(key_range->maximum - q) : (U64)(q - key_range->minimum);
      composite |= offset << key_range->shift;
    }
    task->sort_keys[i * 2 + 0] = (U32)(composite & 0xffffffffull);
    task->sort_keys[i * 2 + 1] = (U32)(composite >> 32);
    task->sort_rows[i] = (U32)i;
  }
}

internal QE_CompositeSort
qe_sort_composite_gpu(Arena* arena, F64** keys, B32* key_desc, U64 num_keys, U64 count)
{
  ProfBeginFunction();
  QE_CompositeSort sort = {0};
  if (count == 0 || count >= max_U32 || num_keys == 0 || num_keys > QE_SORT_MAX_KEYS || settings_u64(str8_lit("QE_SORT_COMPOSITE"), 1) == 0)
  {
    ProfEnd();
    return sort;
  }
  
  QE_WindowKeyRange key_ranges[QE_SORT_MAX_KEYS] = {0};
  U32 total_bits = 0;
  for (U32 k = 0; k < num_keys; k++)
  {
    if (!qe_window_key_range(keys[k], count, &key_ranges[k]))
    {
      ProfEnd();
      return sort;
    }
    total_bits += key_ranges[k].bits;
  }
  if (total_bits > 64)
  {
    ProfEnd();
    return sort;
  }
  
  // tec: the first key is the most significant, so the shifts count down from the last
  U32 shift = 0;
  for (U32 k = (U32)num_keys; k > 0; k--)
  {
    key_ranges[k - 1].shift = shift;
    shift += key_ranges[k - 1].bits;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  U64 padded_count = QE_WINDOW_COMPOSITE_TILE;
  while (padded_count < count) padded_count <<= 1;
  
  GPU_Kernel* sort_kernel = gpu_kernel_alloc(str8_lit("bitonic_sort_u64"));
  GPU_Buffer* keys_buf = gpu_buffer_alloc_pooled(str8_lit("wr_u64_keys_buf"), padded_count * 2 * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* rows_buf = gpu_buffer_alloc_pooled(str8_lit("wr_u64_rows_buf"), padded_count * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  if (!sort_kernel || !keys_buf || !rows_buf)
  {
    if (sort_kernel)
    {
      gpu_kernel_release(sort_kernel);
    }
    scratch_end(scratch);
    ProfEnd();
    return sort;
  }
  
  U32* sort_keys = push_array(scratch.arena, U32, padded_count * 2);
  U32* sort_rows = push_array(scratch.arena, U32, padded_count);
  {
    TP_Context* pool = app_thread_pool();
    U64 task_count = Max((U64)1, Min((U64)pool->worker_count, padded_count / 16384));
    QE_WindowComposeTask compose = {0};
    compose.ranges = tp_divide_work(scratch.arena, padded_count, (U32)task_count);
    compose.keys = keys;
    compose.descending = key_desc;
    compose.key_ranges = key_ranges;
    compose.num_keys = (U32)num_keys;
    compose.padded_count = padded_count;
    compose.real_count = count;
    compose.sort_keys = sort_keys;
    compose.sort_rows = sort_rows;
    TP_Arena* pool_arena = app_thread_pool_arena();
    TP_Temp temp = tp_temp_begin(pool_arena);
    tp_for_parallel(pool, pool_arena, task_count, qe_window_compose_task, &compose);
    tp_temp_end(temp);
  }
  
  gpu_kernel_set_arg_buffer(sort_kernel, 0, keys_buf);
  gpu_kernel_set_arg_buffer(sort_kernel, 1, rows_buf);
  gpu_kernel_set_arg_u64(sort_kernel, 0, padded_count);
  
  U32 num_stages = 0;
  while ((1ull << num_stages) < padded_count) 
  {
    num_stages++;
  }
  U64 tile_groups = padded_count / QE_WINDOW_COMPOSITE_TILE;
  
  U32* sorted_keys = push_array(arena, U32, count * 2);
  U32* sorted_rows = push_array(arena, U32, count);
  
  GPU_Batch* batch = gpu_batch_begin(padded_count * 3 * sizeof(U32), count * 3 * sizeof(U32));
  gpu_batch_buffer_write(batch, keys_buf, sort_keys, padded_count * 2 * sizeof(U32));
  gpu_batch_buffer_write(batch, rows_buf, sort_rows, padded_count * sizeof(U32));
  
  gpu_kernel_set_arg_u64(sort_kernel, 1, 0);
  gpu_batch_kernel_execute(batch, sort_kernel, (U32)(tile_groups * 512), 512);
  for (U32 stage = 11; stage <= num_stages; stage++)
  {
    for (U64 stride = 1ull << (stage - 1); stride >= QE_WINDOW_COMPOSITE_TILE; stride >>= 1)
    {
      gpu_kernel_set_arg_u64(sort_kernel, 1, 1);
      gpu_kernel_set_arg_u64(sort_kernel, 2, stage);
      gpu_kernel_set_arg_u64(sort_kernel, 3, stride);
      gpu_batch_kernel_execute(batch, sort_kernel, (U32)(padded_count / 2), 512);
    }
    gpu_kernel_set_arg_u64(sort_kernel, 1, 2);
    gpu_kernel_set_arg_u64(sort_kernel, 2, stage);
    gpu_batch_kernel_execute(batch, sort_kernel, (U32)(tile_groups * 512), 512);
  }
  
  gpu_batch_buffer_read(batch, keys_buf, sorted_keys, count * 2 * sizeof(U32));
  gpu_batch_buffer_read(batch, rows_buf, sorted_rows, count * sizeof(U32));
  B32 ok = gpu_batch_end(batch);
  gpu_kernel_release(sort_kernel);
  scratch_end(scratch);
  if (!ok)
  {
    ProfEnd();
    return sort;
  }
  
  sort.ok = 1;
  sort.sorted_keys = sorted_keys;
  sort.sorted_rows = sorted_rows;
  sort.total_bits = total_bits;
  for (U32 k = 0; k < num_keys; k++)
  {
    sort.key_bits[k] = key_ranges[k].bits;
  }
  log_debug("qe_sort_composite_gpu: count=%llu padded_count=%llu key_bits=%u GPU kernel time: %llu microseconds",
            count, padded_count, total_bits, gpu_get_executed_kernel_time_microseconds());
  ProfEnd();
  return sort;
}

internal QE_WindowRankResult
qe_window_rank_gpu_composite(Arena* arena, F64** keys, B32* key_desc, U64 num_keys, U64 partition_key_count, U64 real_count)
{
  ProfBeginFunction();
  QE_WindowRankResult result = {0};
  result.real_count = real_count;
  
  QE_CompositeSort sort = qe_sort_composite_gpu(arena, keys, key_desc, num_keys, real_count);
  if (!sort.ok)
  {
    ProfEnd();
    return result;
  }
  
  U32 partition_bits = 0;
  for (U32 k = 0; k < partition_key_count && k < num_keys; k++)
  {
    partition_bits += sort.key_bits[k];
  }
  U32 total_bits = sort.total_bits;
  
  // tec: the rank arithmetic is one pass over the sorted keys, a partition is the high bits and a peer group is the whole key
  U32* row_number = push_array(arena, U32, real_count);
  U32* rank_out = push_array(arena, U32, real_count);
  U32* dense_rank = push_array(arena, U32, real_count);
  U64 part_start = 0;
  U64 peer_start = 0;
  U32 dense = 0;
  U64 previous_key = 0;
  U64 previous_part = 0;
  for (U64 i = 0; i < real_count; i++)
  {
    U64 key = ((U64)sort.sorted_keys[i * 2 + 1] << 32) | (U64)sort.sorted_keys[i * 2];
    U64 part = (partition_bits == 0) ? 0 : (key >> (total_bits - partition_bits));
    if (i == 0 || part != previous_part)
    {
      part_start = i;
      peer_start = i;
      dense = 1;
    }
    else if (key != previous_key)
    {
      peer_start = i;
      dense += 1;
    }
    row_number[i] = (U32)(i - part_start + 1);
    rank_out[i] = (U32)(peer_start - part_start + 1);
    dense_rank[i] = dense;
    previous_key = key;
    previous_part = part;
  }
  
  result.ok = 1;
  result.orig_index = sort.sorted_rows;
  result.row_number = row_number;
  result.rank = rank_out;
  result.dense_rank = dense_rank;
  result.gpu_time_us = gpu_get_executed_kernel_time_microseconds();
  ProfEnd();
  return result;
}

internal QE_WindowRankResult
qe_window_rank_gpu(Arena* arena, F64** keys, B32* key_desc, U64 num_keys, U64 partition_key_count, U64 real_count)
{
  ProfBeginFunction();
  QE_WindowRankResult result = {0};
  result.real_count = real_count;
  
  if (real_count == 0)
  {
    result.ok = 1;
    ProfEnd();
    return result;
  }
  
  // tec: keys that pack into one 64 bit key sort in far fewer passes, the general network below is for the rest
  QE_WindowRankResult packed = qe_window_rank_gpu_composite(arena, keys, key_desc, num_keys, partition_key_count, real_count);
  if (packed.ok)
  {
    ProfEnd();
    return packed;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: pad and sort by (partition keys, order keys) like qe_sort_rows, payload word 0 is the row's position in the caller's arrays
  U64 padded_count = 2;
  while (padded_count < real_count) padded_count <<= 1;
  
  U32 dir_mask = 0;
  for (U32 k = 0; k < num_keys; k++) if (key_desc[k]) dir_mask |= (1u << k);
  
  F64* sort_keys = push_array(scratch.arena, F64, padded_count * QE_SORT_MAX_KEYS);
  U32* sort_payload = push_array(scratch.arena, U32, padded_count * 9);
  for (U64 i = 0; i < padded_count; i++)
  {
    B32 is_real = i < real_count;
    for (U32 k = 0; k < QE_SORT_MAX_KEYS; k++)
    {
      sort_keys[i * QE_SORT_MAX_KEYS + k] = (is_real && k < num_keys) ? keys[k][i] : 0.0;
    }
    sort_payload[i * 9 + 0] = is_real ? (U32)i : 0;
    for (U32 w = 1; w < 8; w++) sort_payload[i * 9 + w] = 0;
    sort_payload[i * 9 + 8] = is_real ? 1u : 0u;
  }
  
  // tec: two kernel objects for the two prefix-sum rounds, rebinding one mid-batch would retarget its already recorded dispatches
  GPU_Kernel* sort_kernel = gpu_kernel_alloc(str8_lit("bitonic_sort"));
  GPU_Kernel* flags_kernel = gpu_kernel_alloc(str8_lit("window_rank_flags"));
  GPU_Kernel* prefix_kernel = gpu_kernel_alloc(str8_lit("prefix_sum"));
  GPU_Kernel* prefix_kernel2 = gpu_kernel_alloc(str8_lit("prefix_sum2"));
  GPU_Kernel* scatter_kernel = gpu_kernel_alloc(str8_lit("window_rank_scatter"));
  GPU_Kernel* gather_kernel = gpu_kernel_alloc(str8_lit("window_rank_gather"));
  if (!sort_kernel || !flags_kernel || !prefix_kernel || !prefix_kernel2 || !scatter_kernel || !gather_kernel)
  {
    log_error("qe_window_rank_gpu: failed to alloc one or more kernels");
    if (sort_kernel) gpu_kernel_release(sort_kernel);
    if (flags_kernel) gpu_kernel_release(flags_kernel);
    if (prefix_kernel) gpu_kernel_release(prefix_kernel);
    if (prefix_kernel2) gpu_kernel_release(prefix_kernel2);
    if (scatter_kernel) gpu_kernel_release(scatter_kernel);
    if (gather_kernel) gpu_kernel_release(gather_kernel);
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  U64 keys_size = padded_count * QE_SORT_MAX_KEYS * sizeof(F64);
  U64 payload_size = padded_count * 9 * sizeof(U32);
  U64 n4 = real_count * sizeof(U32);
  U32 prefix_blocks = (U32)((real_count + 255) / 256);
  U64 block_sum_size = ((U64)prefix_blocks + 1) * sizeof(U32);
  
  // tec: pooled and named so repeated window queries reuse these allocations
  GPU_Buffer* keys_buf = gpu_buffer_alloc_pooled(str8_lit("wr_keys_buf"), keys_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* payload_buf = gpu_buffer_alloc_pooled(str8_lit("wr_payload_buf"), payload_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* part_flag_buf = gpu_buffer_alloc_pooled(str8_lit("wr_part_flag_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* peer_flag_buf = gpu_buffer_alloc_pooled(str8_lit("wr_peer_flag_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* part_id_buf = gpu_buffer_alloc_pooled(str8_lit("wr_part_id_buf"), real_count * sizeof(U32) + sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* peer_id_buf = gpu_buffer_alloc_pooled(str8_lit("wr_peer_id_buf"), real_count * sizeof(U32) + sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* part_cursor_buf = gpu_buffer_alloc_pooled(str8_lit("wr_part_cursor_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* peer_cursor_buf = gpu_buffer_alloc_pooled(str8_lit("wr_peer_cursor_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* part_block_sum_buf = gpu_buffer_alloc_pooled(str8_lit("wr_part_block_sum_buf"), block_sum_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* peer_block_sum_buf = gpu_buffer_alloc_pooled(str8_lit("wr_peer_block_sum_buf"), block_sum_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* part_start_pos_buf = gpu_buffer_alloc_pooled(str8_lit("wr_part_start_pos_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* part_base_peer_buf = gpu_buffer_alloc_pooled(str8_lit("wr_part_base_peer_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* peer_start_pos_buf = gpu_buffer_alloc_pooled(str8_lit("wr_peer_start_pos_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* row_number_buf = gpu_buffer_alloc_pooled(str8_lit("wr_row_number_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* rank_buf = gpu_buffer_alloc_pooled(str8_lit("wr_rank_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* dense_rank_buf = gpu_buffer_alloc_pooled(str8_lit("wr_dense_rank_buf"), n4, GPU_BufferFlag_ReadWrite, 0);
  
  B32 all_buffers_ok = keys_buf && payload_buf && part_flag_buf && peer_flag_buf && part_id_buf && peer_id_buf &&
    part_cursor_buf && peer_cursor_buf && part_block_sum_buf && peer_block_sum_buf && part_start_pos_buf &&
    part_base_peer_buf && peer_start_pos_buf && row_number_buf && rank_buf && dense_rank_buf;
  if (!all_buffers_ok)
  {
    log_error("qe_window_rank_gpu: failed to allocate one or more GPU buffers (real_count=%llu)", real_count);
    // tec: these are pooled - releasing one here would destroy it while the pool's slot still points at it
    gpu_kernel_release(sort_kernel);
    gpu_kernel_release(flags_kernel);
    gpu_kernel_release(prefix_kernel);
    gpu_kernel_release(prefix_kernel2);
    gpu_kernel_release(scatter_kernel);
    gpu_kernel_release(gather_kernel);
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  U64 download_size = real_count * 9 * sizeof(U32) + n4 * 3;
  GPU_Batch* batch = gpu_batch_begin(keys_size + payload_size, download_size);
  gpu_batch_buffer_write(batch, keys_buf, sort_keys, keys_size);
  gpu_batch_buffer_write(batch, payload_buf, sort_payload, payload_size);
  
  //- tec: stage 1 - bitonic sort by (partition keys, order keys)
  gpu_kernel_set_arg_buffer(sort_kernel, 0, keys_buf);
  gpu_kernel_set_arg_buffer(sort_kernel, 1, payload_buf);
  U32 num_stages = 0;
  while ((1ull << num_stages) < padded_count) num_stages++;
  for (U32 stage = 0; stage < num_stages; stage++)
  {
    for (U32 pass_plus1 = stage + 1; pass_plus1 > 0; pass_plus1--)
    {
      U32 pass_ = pass_plus1 - 1;
      gpu_kernel_set_arg_u64(sort_kernel, 0, padded_count);
      gpu_kernel_set_arg_u64(sort_kernel, 1, num_keys);
      gpu_kernel_set_arg_u64(sort_kernel, 2, dir_mask);
      gpu_kernel_set_arg_u64(sort_kernel, 3, stage);
      gpu_kernel_set_arg_u64(sort_kernel, 4, pass_);
      gpu_batch_kernel_execute(batch, sort_kernel, (U32)(padded_count / 2), QE_GPU_WORKGROUP_SIZE);
    }
  }
  
  //- tec: stage 2 - partition/peer-group boundary flags over the sorted keys
  gpu_kernel_set_arg_buffer(flags_kernel, 0, keys_buf);
  gpu_kernel_set_arg_buffer(flags_kernel, 1, part_flag_buf);
  gpu_kernel_set_arg_buffer(flags_kernel, 2, peer_flag_buf);
  gpu_kernel_set_arg_u64(flags_kernel, 0, real_count);
  gpu_kernel_set_arg_u64(flags_kernel, 1, num_keys);
  gpu_kernel_set_arg_u64(flags_kernel, 2, partition_key_count);
  gpu_batch_kernel_execute(batch, flags_kernel, (U32)real_count, QE_GPU_WORKGROUP_SIZE);
  
  //- tec: stage 3 - prefix_sum turns each flag array into a dense increasing id
  gpu_kernel_set_arg_buffer(prefix_kernel, 0, part_flag_buf);
  gpu_kernel_set_arg_buffer(prefix_kernel, 1, part_id_buf);
  gpu_kernel_set_arg_buffer(prefix_kernel, 2, part_cursor_buf);
  gpu_kernel_set_arg_buffer(prefix_kernel, 3, part_block_sum_buf);
  gpu_kernel_set_arg_u64(prefix_kernel, 0, real_count);
  gpu_kernel_set_arg_u64(prefix_kernel, 1, 0);
  gpu_batch_kernel_execute(batch, prefix_kernel, prefix_blocks * 256, 256);
  gpu_kernel_set_arg_u64(prefix_kernel, 1, 1);
  gpu_batch_kernel_execute(batch, prefix_kernel, 256, 256);
  gpu_kernel_set_arg_u64(prefix_kernel, 1, 2);
  gpu_batch_kernel_execute(batch, prefix_kernel, prefix_blocks * 256, 256);
  
  gpu_kernel_set_arg_buffer(prefix_kernel2, 0, peer_flag_buf);
  gpu_kernel_set_arg_buffer(prefix_kernel2, 1, peer_id_buf);
  gpu_kernel_set_arg_buffer(prefix_kernel2, 2, peer_cursor_buf);
  gpu_kernel_set_arg_buffer(prefix_kernel2, 3, peer_block_sum_buf);
  gpu_kernel_set_arg_u64(prefix_kernel2, 0, real_count);
  gpu_kernel_set_arg_u64(prefix_kernel2, 1, 0);
  gpu_batch_kernel_execute(batch, prefix_kernel2, prefix_blocks * 256, 256);
  gpu_kernel_set_arg_u64(prefix_kernel2, 1, 1);
  gpu_batch_kernel_execute(batch, prefix_kernel2, 256, 256);
  gpu_kernel_set_arg_u64(prefix_kernel2, 1, 2);
  gpu_batch_kernel_execute(batch, prefix_kernel2, prefix_blocks * 256, 256);
  
  //- tec: stage 4 - each partition/peer group's first sorted position is written by the one row that flagged it, so no atomics
  gpu_kernel_set_arg_buffer(scatter_kernel, 0, part_flag_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 1, peer_flag_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 2, part_id_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 3, peer_id_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 4, part_start_pos_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 5, part_base_peer_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 6, peer_start_pos_buf);
  gpu_kernel_set_arg_u64(scatter_kernel, 0, real_count);
  gpu_batch_kernel_execute(batch, scatter_kernel, (U32)real_count, QE_GPU_WORKGROUP_SIZE);
  
  //- tec: stage 5 - rank values are plain offset lookups
  gpu_kernel_set_arg_buffer(gather_kernel, 0, part_id_buf);
  gpu_kernel_set_arg_buffer(gather_kernel, 1, peer_id_buf);
  gpu_kernel_set_arg_buffer(gather_kernel, 2, part_start_pos_buf);
  gpu_kernel_set_arg_buffer(gather_kernel, 3, part_base_peer_buf);
  gpu_kernel_set_arg_buffer(gather_kernel, 4, peer_start_pos_buf);
  gpu_kernel_set_arg_buffer(gather_kernel, 5, row_number_buf);
  gpu_kernel_set_arg_buffer(gather_kernel, 6, rank_buf);
  gpu_kernel_set_arg_buffer(gather_kernel, 7, dense_rank_buf);
  gpu_kernel_set_arg_u64(gather_kernel, 0, real_count);
  gpu_batch_kernel_execute(batch, gather_kernel, (U32)real_count, QE_GPU_WORKGROUP_SIZE);
  
  U32* sorted_payload = push_array(arena, U32, real_count * 9);
  U32* row_number = push_array(arena, U32, real_count);
  U32* rank_out = push_array(arena, U32, real_count);
  U32* dense_rank = push_array(arena, U32, real_count);
  gpu_batch_buffer_read(batch, payload_buf, sorted_payload, real_count * 9 * sizeof(U32));
  gpu_batch_buffer_read(batch, row_number_buf, row_number, n4);
  gpu_batch_buffer_read(batch, rank_buf, rank_out, n4);
  gpu_batch_buffer_read(batch, dense_rank_buf, dense_rank, n4);
  gpu_batch_end(batch);
  U64 gpu_time_us = gpu_get_executed_kernel_time_microseconds();
  log_debug("qe_window_rank_gpu: real_count=%llu padded_count=%llu num_stages=%u GPU kernel time: %llu microseconds",
            real_count, padded_count, num_stages, gpu_time_us);
  
  // tec: pooled, left resident for the next call
  gpu_kernel_release(sort_kernel);
  gpu_kernel_release(flags_kernel);
  gpu_kernel_release(prefix_kernel);
  gpu_kernel_release(prefix_kernel2);
  gpu_kernel_release(scatter_kernel);
  gpu_kernel_release(gather_kernel);
  
  U32* orig_index = push_array(arena, U32, real_count);
  for (U64 i = 0; i < real_count; i++)
  {
    orig_index[i] = sorted_payload[i * 9 + 0];
  }
  
  result.ok = 1;
  result.orig_index = orig_index;
  result.row_number = row_number;
  result.rank = rank_out;
  result.dense_rank = dense_rank;
  result.gpu_time_us = gpu_time_us;
  
  scratch_end(scratch);
  ProfEnd();
  return result;
}

internal B32
qe_materialized_row_less(PLAN_Materialized* m, IR_Node* order_by_ir, U64 a, U64 b)
{
  for (IR_Node* col_node = order_by_ir->first; col_node != NULL; col_node = col_node->next)
  {
    PLAN_AggColumn* col = NULL;
    for (U64 c = 0; c < m->column_count; c++)
    {
      if (str8_match(m->columns[c].name, col_node->value, 0))
      {
        col = &m->columns[c];
        break;
      }
    }
    
    if (!col)
    {
      log_error("qe_sort_materialized: ORDER BY column '%.*s' not found in result", str8_varg(col_node->value));
      continue;
    }
    
    B32 desc = (col_node->first && col_node->first->type == IR_NodeType_Descending);
    
    if (col->type == GDB_ColumnType_String8)
    {
      S32 cmp = qe_str8_compare(col->string_values[a], col->string_values[b]);
      if (cmp == 0) continue;
      return desc ? (cmp > 0) : (cmp < 0);
    }
    else
    {
      F64 va = col->numeric_values[a];
      F64 vb = col->numeric_values[b];
      if (va == vb) continue;
      return desc ? (va > vb) : (va < vb);
    }
  }
  return 0;
}

internal B32
qe_materialized_order_less(void* context, U64 a, U64 b)
{
  QE_MaterializedOrder* order = (QE_MaterializedOrder*)context;
  return qe_materialized_row_less(order->materialized, order->order_by, a, b);
}

internal PLAN_Materialized
qe_sort_materialized(Arena* arena, PLAN_Materialized* m, IR_Node* order_by_ir, QE_SortHints* hints)
{
  PLAN_Materialized result = *m;
  if (!order_by_ir || m->count <= 1)
  {
    return result;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  U64* order = push_array(scratch.arena, U64, m->count);
  U64* buffer = push_array(scratch.arena, U64, m->count);
  for (U64 i = 0; i < m->count; i += 1)
  {
    order[i] = i;
  }
  
  QE_MaterializedOrder order_context = {0};
  order_context.materialized = m;
  order_context.order_by = order_by_ir;
  
  U64 out_count = m->count;
  B32 top_k_active = hints && hints->top_k > 0 && hints->top_k < m->count;
  if (top_k_active)
  {
    out_count = qe_order_select_top(order, buffer, m->count, hints->top_k, qe_materialized_order_less, &order_context);
  }
  else
  {
    qe_order_merge_sort(order, buffer, m->count, qe_materialized_order_less, &order_context);
  }
  result.count = out_count;
  
  result.columns = push_array(arena, PLAN_AggColumn, m->column_count);
  for (U64 c = 0; c < m->column_count; c++)
  {
    PLAN_AggColumn* src = &m->columns[c];
    PLAN_AggColumn* dst = &result.columns[c];
    dst->name = src->name;
    dst->type = src->type;
    dst->decimal_scale = src->decimal_scale;
    dst->enum_type = src->enum_type;
    
    if (src->type == GDB_ColumnType_String8)
    {
      dst->string_values = push_array(arena, String8, out_count);
      for (U64 i = 0; i < out_count; i++)
      {
        dst->string_values[i] = src->string_values[order[i]];
      }
    }
    else
    {
      dst->numeric_values = push_array(arena, F64, out_count);
      for (U64 i = 0; i < out_count; i++)
      {
        dst->numeric_values[i] = src->numeric_values[order[i]];
      }
    }
    
    if (src->is_null)
    {
      dst->is_null = push_array(arena, U8, out_count);
      for (U64 i = 0; i < out_count; i++)
      {
        dst->is_null[i] = src->is_null[order[i]];
      }
    }
  }
  
  scratch_end(scratch);
  return result;
}

//~ tec: aggregate/HAVING

internal B32
qe_agg_func_code_from_name(String8 name, U32* out_func_code)
{
  if (str8_match(name, str8_lit("count"), StringMatchFlag_CaseInsensitive)) 
  { 
    *out_func_code = QE_AGG_FUNC_COUNT;
    return 1; 
  }
  if (str8_match(name, str8_lit("sum"), StringMatchFlag_CaseInsensitive))
  { 
    *out_func_code = QE_AGG_FUNC_SUM; 
    return 1; 
  }
  if (str8_match(name, str8_lit("avg"), StringMatchFlag_CaseInsensitive)) 
  { 
    *out_func_code = QE_AGG_FUNC_AVG; 
    return 1; 
  }
  if (str8_match(name, str8_lit("min"), StringMatchFlag_CaseInsensitive)) 
  {
    *out_func_code = QE_AGG_FUNC_MIN;
    return 1; 
  }
  if (str8_match(name, str8_lit("max"), StringMatchFlag_CaseInsensitive)) 
  {
    *out_func_code = QE_AGG_FUNC_MAX;
    return 1;
  }
  if (str8_match(name, str8_lit("approx_count_distinct"), StringMatchFlag_CaseInsensitive)) 
  { 
    *out_func_code = QE_AGG_FUNC_APPROX_COUNT_DISTINCT; 
    return 1; 
  }
  if (str8_match(name, str8_lit("approx_percentile"), StringMatchFlag_CaseInsensitive)) 
  { 
    *out_func_code = QE_AGG_FUNC_APPROX_PERCENTILE; 
    return 1; 
  }
  
  log_error("qe_aggregate: unsupported aggregate function '%.*s'", str8_varg(name));
  return 0;
}

// tec: standard HyperLogLog cardinality estimator
internal F64
qe_hll_estimate_cardinality(U32* registers, U64 num_registers)
{
  F64 m = (F64)num_registers;
  F64 alpha = (num_registers == 16) ? 0.673
    : (num_registers == 32) ? 0.697
    : (num_registers == 64) ? 0.709
    : 0.7213 / (1.0 + 1.079 / m);
  
  F64 sum = 0.0;
  U64 zero_registers = 0;
  for (U64 i = 0; i < num_registers; i++)
  {
    sum += 1.0 / (F64)((U64)1 << registers[i]);
    if (registers[i] == 0) zero_registers++;
  }
  
  F64 estimate = alpha * m * m / sum;
  
  // tec: small cardinality correction. raw HLL is biased low when most registers are still empty
  if (estimate <= 2.5 * m && zero_registers > 0)
  {
    estimate = m * log(m / (F64)zero_registers);
  }
  
  return estimate;
}

internal int
qe_tdigest_centroid_compare(const void* a, const void* b)
{
  F32 ma = ((QE_TDigestCentroid*)a)->mean;
  F32 mb = ((QE_TDigestCentroid*)b)->mean;
  return (ma > mb) - (ma < mb);
}

// tec: standard t-digest quantile query 
// sort centroids by mean, then linearly interpolate between consecutive centroids' cumulative weight midpoints to find the target rank
internal F64
qe_tdigest_estimate_percentile(QE_TDigestCentroid* centroids, U64 count, F64 fraction)
{
  if (count == 0) return 0.0;
  
  qsort(centroids, count, sizeof(QE_TDigestCentroid), qe_tdigest_centroid_compare);
  
  F64 total_weight = 0.0;
  for (U64 i = 0; i < count; i++) total_weight += centroids[i].weight;
  {
    if (total_weight <= 0.0)
    {
      return 0.0;
    }
  }
  
  F64 target = fraction * total_weight;
  
  F64 cumulative = 0.0;
  F64 prev_pos = -1.0;
  F64 prev_mean = (F64)centroids[0].mean;
  for (U64 i = 0; i < count; i++)
  {
    F64 pos = cumulative + (F64)centroids[i].weight * 0.5;
    if (target <= pos)
    {
      // tec: target falls before the first centroid's own cumulative-weight midpoint
      // no lower centroid to interpolate from, so just report this one
      if (prev_pos < 0.0) 
      {
        return (F64)centroids[i].mean;
      }
      F64 t = (target - prev_pos) / (pos - prev_pos);
      return prev_mean + t * ((F64)centroids[i].mean - prev_mean);
    }
    cumulative += centroids[i].weight;
    prev_pos = pos;
    prev_mean = (F64)centroids[i].mean;
  }
  
  // tec: target falls after the last centroid's midpoint
  return (F64)centroids[count - 1].mean; 
}

internal B32
qe_aggregate_collect_exprs(Arena* arena, PLAN_RowSet* input, IR_Node* node, QE_AggExprInfo* exprs, U32* num_exprs)
{
  for (IR_Node* n = node; n != NULL; n = n->next)
  {
    if (n->type == IR_NodeType_AggregateCall)
    {
      String8 name = qe_column_list_item_display_name(arena, n);
      
      B32 dup = 0;
      for (U32 e = 0; e < *num_exprs; e++)
      {
        if (str8_match(exprs[e].display_name, name, 0)) { dup = 1; break; }
      }
      
      if (!dup)
      {
        if (*num_exprs >= QE_AGG_MAX_EXPRS)
        {
          log_error("qe_aggregate: more than %u aggregate expressions is not supported, ignoring '%.*s'",
                    (U32)QE_AGG_MAX_EXPRS, str8_varg(name));
        }
        else
        {
          QE_AggExprInfo* info = &exprs[*num_exprs];
          info->display_name = name;
          if (!qe_agg_func_code_from_name(n->value, &info->func_code))
          {
            return 0;
          }
          
          IR_Node* arg = n->first;
          B32 is_star = (!arg) || str8_match(arg->value, str8_lit("*"), 0);
          
          if (is_star || info->func_code == QE_AGG_FUNC_COUNT)
          {
            info->arg_table = NULL;
            info->arg_slot = max_U64;
            info->arg_column = NULL;
          }
          else
          {
            String8 bare_name = {0};
            U64 slot = max_U64;
            GDB_Table* table = qe_resolve_column_table(input, arg->value, &bare_name, &slot);
            info->arg_table = table;
            info->arg_slot = slot;
            info->arg_column = table ? gdb_table_find_column(table, bare_name) : NULL;
          }
          
          if (info->func_code == QE_AGG_FUNC_APPROX_PERCENTILE)
          {
            IR_Node* frac_node = arg ? arg->next : NULL;
            if (!frac_node || frac_node->type != IR_NodeType_Numeric)
            {
              log_error("qe_aggregate: APPROX_PERCENTILE requires a second numeric argument (the fraction), "
                        "e.g. APPROX_PERCENTILE(col, 0.95)");
              return 0;
            }
            F64 fraction = f64_from_str8(frac_node->value);
            if (fraction < 0.0 || fraction > 1.0)
            {
              log_error("qe_aggregate: APPROX_PERCENTILE fraction %.4f is out of range [0,1]", fraction);
              return 0;
            }
            info->f64_param = fraction;
          }
          
          (*num_exprs)++;
        }
      }
    }
    
    if (!qe_aggregate_collect_exprs(arena, input, n->first, exprs, num_exprs))
    {
      return 0;
    }
  }
  return 1;
}

internal void
qe_agg_output_type_for_expr(QE_AggExprInfo* expr, GDB_ColumnType* out_type, U32* out_decimal_scale, GDB_EnumType** out_enum_type)
{
  *out_type = qe_agg_func_policy[expr->func_code].output_type;
  *out_decimal_scale = 0;
  *out_enum_type = NULL;
  
  if (!expr->arg_column) return;
  
  if (expr->func_code == QE_AGG_FUNC_MIN || expr->func_code == QE_AGG_FUNC_MAX)
  {
    *out_type = expr->arg_column->type;
    *out_decimal_scale = expr->arg_column->decimal_scale;
    *out_enum_type = expr->arg_column->enum_type;
  }
  else if (expr->func_code == QE_AGG_FUNC_SUM)
  {
    switch (expr->arg_column->type)
    {
      case GDB_ColumnType_U32: case GDB_ColumnType_U64: case GDB_ColumnType_Bool:
      *out_type = GDB_ColumnType_U64;
      break;
      case GDB_ColumnType_I32: case GDB_ColumnType_I64:
      *out_type = GDB_ColumnType_I64;
      break;
      default: break;
    }
  }
}

internal THREAD_POOL_TASK_FUNC(qe_agg_base_rows_task)
{
  QE_AggBaseRowsTask* task = (QE_AggBaseRowsTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 g = range.min; g < range.max; g++)
  {
    U64 dense_idx = task->representatives[g];
    task->base_rows[g] = (dense_idx < task->input_count) ? task->table_rows[dense_idx] : PLAN_NULL_ROW;
  }
}

internal THREAD_POOL_TASK_FUNC(qe_agg_string_out_task)
{
  QE_AggStringOutTask* task = (QE_AggStringOutTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 g = range.min; g < range.max; g++)
  {
    U64 start = task->chunk.offsets[g];
    U64 end = task->chunk.offsets[g + 1];
    task->values[g] = str8((U8*)task->chunk.data + start, end - start);
  }
}

internal THREAD_POOL_TASK_FUNC(qe_agg_result_copy_task)
{
  QE_AggResultCopyTask* task = (QE_AggResultCopyTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 g = range.min; g < range.max; g++)
  {
    task->values[g] = task->results[g * task->stride + task->expr];
  }
}

// tec: splits count entries over the pool and runs the task, small counts stay on the calling thread
internal Rng1U64*
qe_agg_split_work(Arena* arena, U64 count, U64* out_task_count)
{
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count / 16384));
  *out_task_count = task_count;
  return tp_divide_work(arena, count, (U32)task_count);
}

internal void
qe_agg_run_tasks(U64 task_count, TP_TaskFunc* task_func, void* task_data)
{
  if (task_count == 1)
  {
    task_func(0, 0, 0, task_data);
    return;
  }
  TP_Context* pool = app_thread_pool();
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, task_func, task_data);
  tp_temp_end(temp);
}

internal void
qe_agg_copy_results(F64* values, F64* results, U64 stride, U64 expr, U64 num_groups)
{
  if (num_groups == 0)
  {
    return;
  }
  Temp scratch = scratch_begin(0, 0);
  U64 task_count = 1;
  QE_AggResultCopyTask task = {0};
  task.ranges = qe_agg_split_work(scratch.arena, num_groups, &task_count);
  task.results = results;
  task.stride = stride;
  task.expr = expr;
  task.values = values;
  qe_agg_run_tasks(task_count, qe_agg_result_copy_task, &task);
  scratch_end(scratch);
}

// tec: assembles the final materialized result in column_list order
internal PLAN_Materialized
qe_aggregate_build_output(Arena* arena, PLAN_RowSet* input, IR_Node* column_list_ir, QE_AggExprInfo* exprs, U32 num_exprs,
                          U64 num_groups, U64* representative_readback, F64* results_readback)
{
  PLAN_Materialized result = {0};
  
  U64 max_output_cols = num_exprs;
  for (IR_Node* item = column_list_ir ? column_list_ir->first : NULL; item; item = item->next) max_output_cols++;
  
  PLAN_AggColumn* out_columns = push_array(arena, PLAN_AggColumn, Max(max_output_cols, 1));
  U64 out_count = 0;
  
  for (IR_Node* item = column_list_ir ? column_list_ir->first : NULL; item; item = item->next)
  {
    String8 name = qe_column_list_item_display_name(arena, item);
    PLAN_AggColumn* dst = &out_columns[out_count];
    
    if (item->type == IR_NodeType_AggregateCall)
    {
      U32 e = 0;
      for (; e < num_exprs; e++) if (str8_match(exprs[e].display_name, name, 0)) break;
      if (e >= num_exprs)
      {
        log_error("qe_aggregate: internal error - expression '%.*s' missing from computed results", str8_varg(name));
        continue;
      }
      
      dst->name = name;
      qe_agg_output_type_for_expr(&exprs[e], &dst->type, &dst->decimal_scale, &dst->enum_type);
      dst->numeric_values = push_array(arena, F64, Max(num_groups, 1));
      qe_agg_copy_results(dst->numeric_values, results_readback, num_exprs, e, num_groups);
      out_count++;
    }
    else if (item->type == IR_NodeType_Column)
    {
      String8 bare_name = {0};
      U64 table_slot = max_U64;
      GDB_Table* table = qe_resolve_column_table(input, item->value, &bare_name, &table_slot);
      GDB_Column* column = table ? gdb_table_find_column(table, bare_name) : NULL;
      if (!column) continue;
      
      dst->name = name;
      dst->type = column->type;
      dst->decimal_scale = column->decimal_scale;
      dst->enum_type = column->enum_type;
      
      // tec: representative rows are scattered across the whole input,
      // so translate them to table rows in parallel, bound the touched range and pull it in one bulk read
      Temp column_scratch = scratch_begin(&arena, 1);
      U64* base_rows = push_array_no_zero(column_scratch.arena, U64, Max(num_groups, 1));
      U64 task_count = 1;
      {
        QE_AggBaseRowsTask rows_task = {0};
        rows_task.ranges = qe_agg_split_work(column_scratch.arena, num_groups, &task_count);
        rows_task.representatives = representative_readback;
        rows_task.table_rows = input->row_indices[table_slot];
        rows_task.input_count = input->count;
        rows_task.base_rows = base_rows;
        if (num_groups > 0)
        {
          qe_agg_run_tasks(task_count, qe_agg_base_rows_task, &rows_task);
        }
      }
      if (column->type == GDB_ColumnType_String8)
      {
        dst->string_values = push_array_no_zero(arena, String8, Max(num_groups, 1));
        
        // tec: the gather reads only the representative rows, the offsets of the whole input are never built
        PLAN_RowSet group_rows = {0};
        group_rows.table_count = table_slot + 1;
        group_rows.row_indices = push_array(column_scratch.arena, U64*, table_slot + 1);
        group_rows.row_indices[table_slot] = base_rows;
        group_rows.count = num_groups;
        
        QE_AggStringOutTask string_task = {0};
        string_task.chunk = qe_gather_string_column(arena, &group_rows, table_slot, column);
        string_task.ranges = qe_agg_split_work(column_scratch.arena, num_groups, &task_count);
        string_task.values = dst->string_values;
        if (num_groups > 0)
        {
          qe_agg_run_tasks(task_count, qe_agg_string_out_task, &string_task);
        }
      }
      else
      {
        U64 min_row = max_U64, max_row = 0;
        qe_row_bounds(base_rows, num_groups, &min_row, &max_row);
        
        dst->numeric_values = push_array_no_zero(arena, F64, Max(num_groups, 1));
        
        void* base_ptr = 0;
        U64 range_size = 0;
        if (min_row != max_U64) base_ptr = gdb_column_get_data_range(arena, column, r1u64(min_row, max_row + 1), &range_size);
        
        QE_GatherNumericTask gather = {0};
        gather.ranges = qe_agg_split_work(column_scratch.arena, num_groups, &task_count);
        gather.table_rows = base_rows;
        gather.base_ptr = base_ptr;
        gather.min_row = min_row;
        gather.column_type = column->type;
        gather.column_size = column->size;
        gather.values = dst->numeric_values;
        if (num_groups > 0)
        {
          qe_agg_run_tasks(task_count, qe_gather_numeric_task, &gather);
        }
      }
      scratch_end(column_scratch);
      out_count++;
    }
  }
  
  for (U32 e = 0; e < num_exprs; e++)
  {
    B32 already = 0;
    for (U64 c = 0; c < out_count; c++) if (str8_match(out_columns[c].name, exprs[e].display_name, 0)) { already = 1; break; }
    if (already) continue;
    
    PLAN_AggColumn* dst = &out_columns[out_count];
    dst->name = exprs[e].display_name;
    qe_agg_output_type_for_expr(&exprs[e], &dst->type, &dst->decimal_scale, &dst->enum_type);
    dst->numeric_values = push_array(arena, F64, Max(num_groups, 1));
    qe_agg_copy_results(dst->numeric_values, results_readback, num_exprs, e, num_groups);
    out_count++;
  }
  
  result.columns = out_columns;
  result.column_count = out_count;
  result.count = num_groups;
  return result;
}

internal B32
qe_aggregate_cpu_supported(QE_AggExprInfo* exprs, U32 num_exprs)
{
  for (U32 index = 0; index < num_exprs; index += 1)
  {
    U32 code = exprs[index].func_code;
    B32 exact = code == QE_AGG_FUNC_COUNT || code == QE_AGG_FUNC_SUM || code == QE_AGG_FUNC_AVG || code == QE_AGG_FUNC_MIN || code == QE_AGG_FUNC_MAX;
    if (!exact)
    {
      return 0;
    }
  }
  return 1;
}

// tec: FNV-1a over string keys, a multiply and shift mix over the bits of numeric ones
internal U64
qe_aggregate_hash_key(QE_AggregateKeys* keys, U64 row)
{
  U64 hash = 14695981039346656037ull;
  for (U32 column = 0; column < keys->column_count; column += 1)
  {
    if (keys->string_mask & (1u << column))
    {
      GDB_StringDataChunk* chunk = &keys->strings[column];
      U8* bytes = (U8*)chunk->data + chunk->offsets[row];
      U64 size = chunk->offsets[row + 1] - chunk->offsets[row];
      for (U64 index = 0; index < size; index += 1)
      {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
      }
      hash ^= 0xffull;
      hash *= 1099511628211ull;
    }
    else
    {
      F64 value = keys->numeric[column][row];
      if (value == 0.0)
      {
        value = 0.0;
      }
      U64 bits = 0;
      MemoryCopy(&bits, &value, sizeof(bits));
      hash ^= bits;
      hash *= 0x9E3779B97F4A7C15ull;
      hash ^= hash >> 32;
    }
  }
  return hash;
}

internal B32
qe_aggregate_keys_equal(QE_AggregateKeys* keys, U64 a, U64 b)
{
  for (U32 column = 0; column < keys->column_count; column += 1)
  {
    if (keys->string_mask & (1u << column))
    {
      GDB_StringDataChunk* chunk = &keys->strings[column];
      U64 size_a = chunk->offsets[a + 1] - chunk->offsets[a];
      U64 size_b = chunk->offsets[b + 1] - chunk->offsets[b];
      if (size_a != size_b)
      {
        return 0;
      }
      U8* bytes_a = (U8*)chunk->data + chunk->offsets[a];
      U8* bytes_b = (U8*)chunk->data + chunk->offsets[b];
      if (size_a > 0 && MemoryCompare(bytes_a, bytes_b, size_a) != 0)
      {
        return 0;
      }
    }
    else
    {
      if (keys->numeric[column][a] != keys->numeric[column][b])
      {
        return 0;
      }
    }
  }
  return 1;
}

internal F64
qe_aggregate_finish(QE_AggExprInfo* expr, QE_AggregateAccumulator* accumulator)
{
  F64 result = accumulator->max;
  if (expr->func_code == QE_AGG_FUNC_COUNT)
  {
    result = (F64)accumulator->count;
  }
  else if (expr->func_code == QE_AGG_FUNC_SUM)
  {
    result = accumulator->sum;
  }
  else if (expr->func_code == QE_AGG_FUNC_AVG)
  {
    result = 0.0;
    if (accumulator->count > 0)
    {
      result = accumulator->sum / (F64)accumulator->count;
    }
  }
  else if (expr->func_code == QE_AGG_FUNC_MIN)
  {
    result = accumulator->min;
  }
  return result;
}

// tec: same result layout as the GPU path, group order is first appearance in the input
internal U64
qe_aggregate_cpu_reduce(Arena* arena, U64 row_count, QE_AggregateKeys* keys, QE_AggExprInfo* exprs, U32 num_exprs, F64** expr_args, U64** out_representatives, F64** out_results)
{
  U64 expr_stride = Max((U64)num_exprs, (U64)1);
  U64 max_groups = (keys->column_count == 0) ? 1 : row_count;
  
  U64 table_size = 16;
  while (table_size < max_groups * 2)
  {
    table_size <<= 1;
  }
  U32* table = push_array(arena, U32, table_size);
  for (U64 slot = 0; slot < table_size; slot += 1)
  {
    table[slot] = max_U32;
  }
  
  U64* representatives = push_array(arena, U64, Max(max_groups, (U64)1));
  QE_AggregateAccumulator* accumulators = push_array(arena, QE_AggregateAccumulator, Max(max_groups, (U64)1) * expr_stride);
  U64 group_count = 0;
  
  for (U64 row = 0; row < row_count; row += 1)
  {
    U64 group = 0;
    B32 is_new_group = 0;
    
    if (keys->column_count == 0)
    {
      is_new_group = (row == 0);
    }
    else
    {
      U64 slot = qe_aggregate_hash_key(keys, row) & (table_size - 1);
      B32 found = 0;
      while (!found)
      {
        U32 occupant = table[slot];
        if (occupant == max_U32)
        {
          group = group_count;
          table[slot] = (U32)group;
          is_new_group = 1;
          found = 1;
        }
        else if (qe_aggregate_keys_equal(keys, representatives[occupant], row))
        {
          group = occupant;
          found = 1;
        }
        else
        {
          slot = (slot + 1) & (table_size - 1);
        }
      }
    }
    
    if (is_new_group)
    {
      representatives[group] = row;
      for (U64 expr_index = 0; expr_index < expr_stride; expr_index += 1)
      {
        QE_AggregateAccumulator* fresh = &accumulators[group * expr_stride + expr_index];
        fresh->sum = 0.0;
        fresh->count = 0;
        fresh->min = 1.0e300;
        fresh->max = -1.0e300;
      }
      group_count += 1;
    }
    
    for (U32 expr_index = 0; expr_index < num_exprs; expr_index += 1)
    {
      QE_AggregateAccumulator* accumulator = &accumulators[group * expr_stride + expr_index];
      F64 value = 0.0;
      if (expr_args[expr_index])
      {
        value = expr_args[expr_index][row];
      }
      accumulator->sum += value;
      accumulator->count += 1;
      if (value < accumulator->min)
      {
        accumulator->min = value;
      }
      if (value > accumulator->max)
      {
        accumulator->max = value;
      }
    }
  }
  
  F64* results = push_array(arena, F64, Max(group_count * num_exprs, (U64)1));
  for (U64 group = 0; group < group_count; group += 1)
  {
    for (U32 expr_index = 0; expr_index < num_exprs; expr_index += 1)
    {
      results[group * num_exprs + expr_index] = qe_aggregate_finish(&exprs[expr_index], &accumulators[group * expr_stride + expr_index]);
    }
  }
  
  *out_representatives = representatives;
  *out_results = results;
  return group_count;
}

internal PLAN_Materialized
qe_aggregate(Arena* arena, GDB_Database* database, PLAN_RowSet* input, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir, QE_AggregateHints* hints, QE_AggregateTrace* out_trace)
{
  return qe_aggregate_impl(arena, database, input, group_by_ir, column_list_ir, having_ir, hints, out_trace, 0, 0);
}

// tec: the hash reduce accumulates in F32, it is allowed with few rows per group, or with more when every SUM and AVG
// argument is a whole number whose largest possible group sum stays inside what F32 holds exactly
internal B32
qe_aggregate_hash_allowed(QE_AggExprInfo* exprs, U32 num_exprs, U32* arg_owner, F64* arg_int_bound, U64 row_count, U64 num_groups, U64 max_group_rows, U64 cap, U64 exact_cap, B32 fixed_ok, U64 fixed_cap)
{
  U64 average_rows = row_count / Max(num_groups, (U64)1);
  if (fixed_ok)
  {
    return average_rows <= fixed_cap;
  }
  if (average_rows <= cap)
  {
    return 1;
  }
  if (average_rows > exact_cap)
  {
    return 0;
  }
  for (U32 e = 0; e < num_exprs; e += 1)
  {
    if (exprs[e].func_code != QE_AGG_FUNC_SUM && exprs[e].func_code != QE_AGG_FUNC_AVG)
    {
      continue;
    }
    F64 bound = exprs[e].arg_column ? arg_int_bound[arg_owner[e]] : 0.0;
    if (bound < 0.0 || bound * (F64)max_group_rows >= QE_AGG_F32_EXACT_SUM_LIMIT)
    {
      return 0;
    }
  }
  return 1;
}

// tec: turns the per slot row counts of the group by hash table into CSR offsets and the list of occupied slots
internal void
qe_aggregate_slot_table(Arena* arena, U32* count_readback, U64 num_slots, B32 group_by, U32** out_slot_offsets, U32** out_group_ids, U64* out_num_groups)
{
  U32* slot_offsets = push_array(arena, U32, num_slots + 1);
  U32* group_ids_scratch = group_by ? push_array(arena, U32, num_slots) : 0;
  U32 running = 0;
  U64 num_groups_found = 0;
  for (U64 s = 0; s < num_slots; s++)
  {
    slot_offsets[s] = running;
    U32 c = count_readback[s];
    running += c;
    if (c != 0 && group_ids_scratch) group_ids_scratch[num_groups_found++] = (U32)s;
  }
  slot_offsets[num_slots] = running;
  
  *out_slot_offsets = slot_offsets;
  if (!group_by)
  {
    U32* single = push_array(arena, U32, 1);
    single[0] = 0;
    *out_group_ids = single;
    *out_num_groups = 1;
  }
  else
  {
    *out_group_ids = group_ids_scratch;
    *out_num_groups = num_groups_found;
  }
}

// tec: with a device selection, input only stands in for the table and the row count, the rows themselves stay on the GPU.
// out_needs_host_rows is set, before any result exists, when the query has to run on host row ids instead
internal PLAN_Materialized
qe_aggregate_impl(Arena* arena, GDB_Database* database, PLAN_RowSet* input, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir, QE_AggregateHints* hints, QE_AggregateTrace* out_trace, QE_DeviceRows* device, B32* out_needs_host_rows)
{
  ProfBeginFunction();
  
  PLAN_Materialized result = {0};
  U64 row_count = input->count;
  U64 qe_agg_t_start = os_now_microseconds();
  if (device)
  {
    log_debug("qe_aggregate: reading %llu rows through a device selection", row_count);
  }
  
  if (gpu_device_lost())
  {
    log_error("qe_aggregate: Vulkan device is lost - refusing to attempt any GPU work");
    ProfEnd();
    return result;
  }
  
  //- tec: resolve GROUP BY key columns (0 means one global group)
  U64 group_slots[QE_AGG_MAX_GROUP_COLS];
  GDB_Column* group_columns[QE_AGG_MAX_GROUP_COLS];
  U32 num_group_cols = 0;
  
  for (IR_Node* col_node = group_by_ir ? group_by_ir->first : NULL; col_node != NULL; col_node = col_node->next)
  {
    if (num_group_cols >= QE_AGG_MAX_GROUP_COLS)
    {
      log_error("qe_aggregate: more than %u GROUP BY columns is not supported, ignoring the rest", (U32)QE_AGG_MAX_GROUP_COLS);
      break;
    }
    
    String8 bare_name = {0};
    U64 slot = max_U64;
    GDB_Table* table = qe_resolve_column_table(input, col_node->value, &bare_name, &slot);
    if (!table) continue;
    GDB_Column* column = gdb_table_find_column(table, bare_name);
    if (!column) continue;
    
    group_slots[num_group_cols] = slot;
    group_columns[num_group_cols] = column;
    num_group_cols++;
  }
  
  //- tec: union of aggregate expressions referenced by the select list and HAVING
  QE_AggExprInfo exprs[QE_AGG_MAX_EXPRS];
  U32 num_exprs = 0;
  if (!qe_aggregate_collect_exprs(arena, input, column_list_ir ? column_list_ir->first : NULL, exprs, &num_exprs) ||
      !qe_aggregate_collect_exprs(arena, input, having_ir ? having_ir->first : NULL, exprs, &num_exprs))
  {
    ProfEnd();
    return result;
  }
  
  // tec: GROUP BY/aggregate kernels dont know about NULL yet, so a NULL cell is grouped/summed using its placeholder value instead of being excluded
  {
    B32 warned = 0;
    for (U32 c = 0; c < num_group_cols && !warned; c++)
    {
      if (group_columns[c]->null_flags)
      {
        log_error("qe_aggregate: GROUP BY column '%.*s' has NULLs - they are not excluded/grouped per standard SQL semantics yet",
                  str8_varg(group_columns[c]->name));
        warned = 1;
      }
    }
    for (U32 e = 0; e < num_exprs && !warned; e++)
    {
      if (exprs[e].arg_column && exprs[e].arg_column->null_flags)
      {
        log_error("qe_aggregate: aggregate argument column '%.*s' has NULLs - they are not excluded from SUM/AVG/MIN/MAX/COUNT yet",
                  str8_varg(exprs[e].arg_column->name));
        warned = 1;
      }
    }
  }
  
  
  //- tec: no point spinning up a GPU dispatch for zero rows.
  if (row_count == 0)
  {
    U64 num_groups = (num_group_cols == 0) ? 1 : 0;
    U64* representative_readback = push_array(arena, U64, Max(num_groups, 1));
    F64* results_readback = push_array(arena, F64, Max(num_groups * Max(num_exprs, 1), 1));
    for (U64 g = 0; g < num_groups; g++) representative_readback[g] = max_U64;
    for (U32 e = 0; e < num_exprs; e++) results_readback[e] = 0.0; // COUNT/SUM/AVG all correctly 0 for an empty group
    
    result = qe_aggregate_build_output(arena, input, column_list_ir, exprs, num_exprs, num_groups, representative_readback, results_readback);
    ProfEnd();
    return result;
  }
  
  // tec: COUNT(*) over a selection is the size of the selection, no row is ever read
  if (device && num_group_cols == 0 && num_exprs > 0)
  {
    B32 count_only = 1;
    for (U32 e = 0; e < num_exprs; e++)
    {
      if (exprs[e].func_code != QE_AGG_FUNC_COUNT || exprs[e].arg_column)
      {
        count_only = 0;
      }
    }
    if (count_only)
    {
      F64* count_results = push_array(arena, F64, num_exprs);
      for (U32 e = 0; e < num_exprs; e++)
      {
        count_results[e] = (F64)row_count;
      }
      U64 count_representative = max_U64;
      result = qe_aggregate_build_output(arena, input, column_list_ir, exprs, num_exprs, 1, &count_representative, count_results);
      if (out_trace)
      {
        out_trace->input_row_count = row_count;
        out_trace->group_count = 1;
      }
      ProfEnd();
      return result;
    }
  }
  
  //- tec: an aggregate small enough for the CPU never touches the GPU, so it gathers everything on the host
  B32 use_cpu = !device && hints && hints->cpu_max_rows > 0 && row_count <= hints->cpu_max_rows && qe_aggregate_cpu_supported(exprs, num_exprs);
  
  B32 identity_known[QE_AGG_MAX_RESIDENT_SLOTS] = {0};
  B32 identity_value[QE_AGG_MAX_RESIDENT_SLOTS] = {0};
  
  //- tec: gather GROUP BY key columns (dense, index-aligned 0..row_count-1 to the input row set)
  // a key column that is already resident on the GPU is not gathered at all
  F64* group_numeric[QE_AGG_MAX_GROUP_COLS] = {0};
  GPU_Buffer* group_resident[QE_AGG_MAX_GROUP_COLS] = {0};
  B32 key_whole[QE_AGG_MAX_GROUP_COLS] = {0};
  S64 key_min[QE_AGG_MAX_GROUP_COLS] = {0};
  S64 key_max[QE_AGG_MAX_GROUP_COLS] = {0};
  GDB_StringDataChunk group_string[QE_AGG_MAX_GROUP_COLS];
  MemoryZeroArray(group_string);
  U32 group_string_mask = 0;
  
  for (U32 c = 0; c < num_group_cols; c++)
  {
    GDB_Column* key_column = group_columns[c];
    B32 is_dict_key = 0;
    if (key_column->type == GDB_ColumnType_String8)
    {
      gdb_column_ensure_string_dict(key_column);
      if (!key_column->has_dict)
      {
        if (device)
        {
          *out_needs_host_rows = 1;
          ProfEnd();
          return result;
        }
        B32 str_resident_ok = qe_aggregate_column_can_be_resident(input, group_slots[c], key_column, identity_known, identity_value);
        if (str_resident_ok && key_column->agg_str_generation == key_column->write_generation)
        {
          log_debug("qe_aggregate: reusing host-resident GROUP BY key for string column '%.*s' (generation %llu, %llu bytes) - no read, no regather",
                    str8_varg(key_column->name), key_column->agg_str_generation, key_column->agg_str_data_size);
          group_string[c].data = key_column->agg_str_data;
          group_string[c].offsets = key_column->agg_str_offsets;
          group_string[c].size = key_column->agg_str_data_size;
          group_string[c].row_count = key_column->row_count;
        }
        else if (str_resident_ok)
        {
          // tec: whole column, identity row order
          if (key_column->agg_str_arena)
          {
            arena_release(key_column->agg_str_arena);
          }
          key_column->agg_str_arena = arena_alloc();
          GDB_StringDataChunk fresh = qe_gather_string_column(key_column->agg_str_arena, input, group_slots[c], key_column);
          key_column->agg_str_data = fresh.data;
          key_column->agg_str_offsets = fresh.offsets;
          key_column->agg_str_data_size = fresh.size;
          key_column->agg_str_generation = key_column->write_generation;
          group_string[c] = fresh;
        }
        else
        {
          group_string[c] = qe_gather_string_column(arena, input, group_slots[c], key_column);
        }
        group_string_mask |= (1u << c);
        continue;
      }
      is_dict_key = 1;
    }
    
    if (device)
    {
      B32 key_narrow = 0;
      GPU_Buffer* full_key = qe_aggregate_full_column_f64(arena, key_column, is_dict_key, &key_narrow);
      if (full_key)
      {
        group_resident[c] = qe_selection_gather_column(qe_device_view(device, group_slots[c]), full_key, 0, push_str8f(gpu_scratch_arena(), "agg_sel_key:%u", c));
      }
      if (!group_resident[c])
      {
        *out_needs_host_rows = 1;
        ProfEnd();
        return result;
      }
      if (key_column->agg_key_generation == key_column->write_generation)
      {
        key_whole[c] = key_column->agg_key_whole;
        key_min[c] = key_column->agg_key_min;
        key_max[c] = key_column->agg_key_max;
      }
      continue;
    }
    
    B32 resident_ok = !use_cpu && qe_aggregate_column_can_be_resident(input, group_slots[c], key_column, identity_known, identity_value);
    if (resident_ok)
    {
      group_resident[c] = qe_aggregate_resident_lookup(key_column, 0, row_count);
      if (group_resident[c] && key_column->agg_key_generation == key_column->write_generation)
      {
        key_whole[c] = key_column->agg_key_whole;
        key_min[c] = key_column->agg_key_min;
        key_max[c] = key_column->agg_key_max;
      }
    }
    if (!group_resident[c])
    {
      if (is_dict_key)
      {
        group_numeric[c] = qe_gather_string_dict_codes(arena, input, group_slots[c], key_column);
      }
      else
      {
        group_numeric[c] = qe_gather_numeric_column(arena, input, group_slots[c], key_column);
      }
      key_whole[c] = qe_values_whole_range(group_numeric[c], row_count, &key_min[c], &key_max[c]);
      if (resident_ok)
      {
        key_column->agg_key_whole = key_whole[c];
        key_column->agg_key_min = key_min[c];
        key_column->agg_key_max = key_max[c];
        key_column->agg_key_generation = key_column->write_generation;
        group_resident[c] = qe_aggregate_resident_store(key_column, 0, group_numeric[c], row_count);
      }
    }
  }
  
  //- tec: gather aggregate argument columns
  // exprs reading the same source column share one gather, one narrowing check and one device buffer, owned by the first of them
  U32 arg_owner[QE_AGG_MAX_EXPRS] = {0};
  for (U32 e = 0; e < num_exprs; e++)
  {
    arg_owner[e] = e;
    if (!exprs[e].arg_column)
    {
      continue;
    }
    for (U32 e2 = 0; e2 < e; e2++)
    {
      if (exprs[e2].arg_column == exprs[e].arg_column && exprs[e2].arg_slot == exprs[e].arg_slot)
      {
        arg_owner[e] = e2;
        break;
      }
    }
  }
  
  F64* expr_args[QE_AGG_MAX_EXPRS] = {0};
  GPU_Buffer* arg_full_f64[QE_AGG_MAX_EXPRS] = {0};
  B32 arg_resident_ok[QE_AGG_MAX_EXPRS] = {0};
  B32 arg_narrow_flag[QE_AGG_MAX_EXPRS] = {0};
  B32 arg_narrow_known[QE_AGG_MAX_EXPRS] = {0};
  F64 arg_int_bound[QE_AGG_MAX_EXPRS] = {0};
  S32 arg_fixed_scale[QE_AGG_MAX_EXPRS] = {0};
  F64 arg_fixed_max[QE_AGG_MAX_EXPRS] = {0};
  B32 arg_fixed_known[QE_AGG_MAX_EXPRS] = {0};
  for (U32 e = 0; e < num_exprs; e++)
  {
    arg_int_bound[e] = -1.0;
    arg_fixed_scale[e] = -1;
  }
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!exprs[e].arg_column || arg_owner[e] != e)
    {
      continue;
    }
    GDB_Column* arg_column = exprs[e].arg_column;
    if (device)
    {
      B32 arg_column_narrow = 0;
      GPU_Buffer* full_arg = 0;
      if (arg_column->type != GDB_ColumnType_String8)
      {
        full_arg = qe_aggregate_full_column_f64(arena, arg_column, 0, &arg_column_narrow);
      }
      if (!full_arg)
      {
        *out_needs_host_rows = 1;
        ProfEnd();
        return result;
      }
      arg_full_f64[e] = full_arg;
      arg_narrow_flag[e] = arg_column_narrow;
      arg_narrow_known[e] = 1;
      arg_int_bound[e] = arg_column->agg_int_bound;
      arg_fixed_scale[e] = arg_column->agg_fixed_scale;
      arg_fixed_max[e] = arg_column->agg_fixed_max;
      arg_fixed_known[e] = 1;
      continue;
    }
    arg_resident_ok[e] = !use_cpu && qe_aggregate_column_can_be_resident(input, exprs[e].arg_slot, arg_column, identity_known, identity_value);
    if (arg_resident_ok[e] && arg_column->agg_narrow_generation == arg_column->write_generation &&
        arg_column->agg_fixed_generation == arg_column->write_generation)
    {
      arg_narrow_flag[e] = arg_column->agg_narrow;
      arg_narrow_known[e] = 1;
      arg_int_bound[e] = arg_column->agg_int_bound;
      arg_fixed_scale[e] = arg_column->agg_fixed_scale;
      arg_fixed_max[e] = arg_column->agg_fixed_max;
      arg_fixed_known[e] = 1;
    }
    else
    {
      expr_args[e] = qe_gather_numeric_column(arena, input, exprs[e].arg_slot, arg_column);
    }
  }
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (exprs[e].arg_column && arg_owner[e] != e)
    {
      expr_args[e] = expr_args[arg_owner[e]];
    }
  }
  
  if (use_cpu)
  {
    Temp cpu_scratch = scratch_begin(&arena, 1);
    
    QE_AggregateKeys keys = {0};
    keys.column_count = num_group_cols;
    keys.string_mask = group_string_mask;
    for (U32 c = 0; c < num_group_cols; c += 1)
    {
      keys.numeric[c] = group_numeric[c];
      keys.strings[c] = group_string[c];
    }
    
    U64* cpu_representatives = 0;
    F64* cpu_results = 0;
    U64 cpu_groups = qe_aggregate_cpu_reduce(cpu_scratch.arena, row_count, &keys, exprs, num_exprs, expr_args, &cpu_representatives, &cpu_results);
    result = qe_aggregate_build_output(arena, input, column_list_ir, exprs, num_exprs, cpu_groups, cpu_representatives, cpu_results);
    
    U64 qe_agg_t_cpu_done = os_now_microseconds();
    log_debug("qe_aggregate: row_count=%llu num_groups=%llu on the CPU, total=%llu us", row_count, cpu_groups, qe_agg_t_cpu_done - qe_agg_t_start);
    if (out_trace)
    {
      out_trace->input_row_count = row_count;
      out_trace->group_count = cpu_groups;
      out_trace->used_cpu = 1;
      out_trace->gather_time_us = qe_agg_t_cpu_done - qe_agg_t_start;
    }
    
    scratch_end(cpu_scratch);
    ProfEnd();
    return result;
  }
  
  // tec: narrow numeric aggregate args, the answer is remembered on a resident column until it is written
  B32 arg_narrow = 1;
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!exprs[e].arg_column || arg_owner[e] != e)
    {
      continue;
    }
    if (!arg_narrow_known[e] && (arg_narrow || arg_resident_ok[e]))
    {
      arg_narrow_flag[e] = qe_values_round_trip_f32(expr_args[e], row_count, &arg_int_bound[e]);
      arg_narrow_known[e] = 1;
      if (arg_resident_ok[e])
      {
        exprs[e].arg_column->agg_int_bound = arg_int_bound[e];
        exprs[e].arg_column->agg_narrow = arg_narrow_flag[e];
        exprs[e].arg_column->agg_narrow_generation = exprs[e].arg_column->write_generation;
      }
    }
    if (arg_narrow_known[e] && !arg_narrow_flag[e])
    {
      arg_narrow = 0;
    }
  }
  
  // tec: decimals with a few places can still aggregate exactly
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!exprs[e].arg_column || arg_owner[e] != e || arg_fixed_known[e] || !expr_args[e])
    {
      continue;
    }
    qe_values_fixed_point(expr_args[e], row_count, &arg_fixed_scale[e], &arg_fixed_max[e]);
    arg_fixed_known[e] = 1;
    if (arg_resident_ok[e])
    {
      exprs[e].arg_column->agg_fixed_scale = arg_fixed_scale[e];
      exprs[e].arg_column->agg_fixed_max = arg_fixed_max[e];
      exprs[e].arg_column->agg_fixed_generation = exprs[e].arg_column->write_generation;
    }
  }
  
  F32* expr_args_f32[QE_AGG_MAX_EXPRS] = {0};
  GPU_Buffer* arg_resident_buf[QE_AGG_MAX_EXPRS] = {0};
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!exprs[e].arg_column || arg_owner[e] != e)
    {
      continue;
    }
    GDB_Column* arg_column = exprs[e].arg_column;
    if (device)
    {
      GPU_Buffer* selection_source = arg_narrow ? qe_aggregate_full_column_f32(arena, arg_column, 0) : arg_full_f64[e];
      if (selection_source)
      {
        arg_resident_buf[e] = qe_selection_gather_column(qe_device_view(device, exprs[e].arg_slot), selection_source, arg_narrow, push_str8f(gpu_scratch_arena(), "agg_sel_arg:%u", e));
      }
      if (!arg_resident_buf[e])
      {
        *out_needs_host_rows = 1;
        ProfEnd();
        return result;
      }
    }
    else if (arg_resident_ok[e])
    {
      arg_resident_buf[e] = qe_aggregate_resident_lookup(arg_column, arg_narrow, row_count);
      if (!arg_resident_buf[e])
      {
        if (!expr_args[e])
        {
          expr_args[e] = qe_gather_numeric_column(arena, input, exprs[e].arg_slot, arg_column);
        }
        if (arg_narrow)
        {
          expr_args_f32[e] = qe_values_to_f32(arena, expr_args[e], row_count);
          arg_resident_buf[e] = qe_aggregate_resident_store(arg_column, 1, expr_args_f32[e], row_count);
        }
        else
        {
          arg_resident_buf[e] = qe_aggregate_resident_store(arg_column, 0, expr_args[e], row_count);
        }
      }
    }
    else if (arg_narrow)
    {
      expr_args_f32[e] = qe_values_to_f32(arena, expr_args[e], row_count);
    }
  }
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (exprs[e].arg_column && arg_owner[e] != e)
    {
      expr_args_f32[e] = expr_args_f32[arg_owner[e]];
      arg_resident_buf[e] = arg_resident_buf[arg_owner[e]];
    }
  }
  U64 qe_agg_t_gathered = os_now_microseconds();
  
  // tec: max_num_buckets is the worst-case
  U64 max_num_buckets = 1;
  while (max_num_buckets < row_count) max_num_buckets <<= 1;
  
  U64 num_buckets;
  if (num_group_cols == 0)
  {
    num_buckets = 1;
    max_num_buckets = 1;
  }
  else
  {
    // tec: most GROUP BYs aggregate many rows per group. 
    // guess ~64 rows/group so the common case gets a table sized for its actual cardinality instead of one sized for row_count.
    // if the guess is wrong the overflow retry loop below grows num_buckets back up towards max_num_buckets,
    // at the cost of rerunning the assign dispatch over all rows each retry
    num_buckets = 16;
    if (hints && hints->group_count > 0)
    {
      // tec: one bucket per estimated group keeps the 8 slots of a bucket from overflowing, the estimate gets a quarter of headroom
      U64 wanted_buckets = hints->group_count + hints->group_count / 4;
      while (num_buckets < wanted_buckets && num_buckets < max_num_buckets)
      {
        num_buckets <<= 1;
      }
    }
    else
    {
      while (num_buckets < row_count / 64 && num_buckets < max_num_buckets) num_buckets <<= 1;
    }
  }
  U64 K = (num_group_cols == 0) ? 1 : 8;
  
  Temp scratch = scratch_begin(&arena, 1);
  
  GPU_Kernel* assign_kernel = gpu_kernel_alloc(str8_lit("aggregate_assign"));
  if (!assign_kernel)
  {
    log_error("qe_aggregate: failed to alloc 'aggregate_assign' kernel");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  GPU_Buffer* group_col_bufs[QE_AGG_MAX_GROUP_COLS * 2] = {0};
  U64 group_col_sizes[QE_AGG_MAX_GROUP_COLS * 2] = {0};
  for (U32 c = 0; c < num_group_cols; c++)
  {
    if (group_string_mask & (1u << c))
    {
      U64 data_size = Max(group_string[c].size, 4);
      U64 off_size = (row_count + 1) * sizeof(U64);
      group_col_bufs[c * 2 + 0] = gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_group_col_data:%u", c), data_size, GPU_BufferFlag_Write, 0);
      group_col_bufs[c * 2 + 1] = gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_group_col_off:%u", c), off_size, GPU_BufferFlag_Write, 0);
      group_col_sizes[c * 2 + 0] = data_size;
      group_col_sizes[c * 2 + 1] = off_size;
    }
    else if (group_resident[c])
    {
      group_col_bufs[c * 2 + 0] = group_resident[c];
    }
    else
    {
      U64 data_size = row_count * sizeof(F64);
      group_col_bufs[c * 2 + 0] = gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_group_col_data:%u", c), data_size, GPU_BufferFlag_Write, 0);
      group_col_sizes[c * 2 + 0] = data_size;
    }
  }
  
  if (!group_col_bufs[0] && num_group_cols > 0)
  {
    log_error("qe_aggregate: failed to allocate group-by key column GPU buffers (row_count=%llu)", row_count);
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  U64 assign_upload_bytes = 0;
  for (U32 c = 0; c < num_group_cols * 2; c++) assign_upload_bytes += group_col_sizes[c];
  
  U64 num_slots = 0;
  B32 compact_table = 0;
  B32 all_counts = 1;
  B32 plain_funcs = 1;
  for (U32 e = 0; e < num_exprs; e++)
  {
    all_counts = all_counts && exprs[e].func_code == QE_AGG_FUNC_COUNT;
    plain_funcs = plain_funcs && exprs[e].func_code <= QE_AGG_FUNC_MAX;
  }
  // tec: F32 accumulators lose precision as a group grows and the kernel contends on a hot group, so this is a cap on the average rows per group
  U64 hash_max_rows_per_group = settings_u64(str8_lit("QE_AGG_HASH_REDUCE_MAX_ROWS_PER_GROUP"), QE_AGG_HASH_REDUCE_MAX_AVG_ROWS_PER_GROUP);
  U64 hash_exact_max_rows_per_group = settings_u64(str8_lit("QE_AGG_HASH_REDUCE_EXACT_MAX_ROWS_PER_GROUP"), QE_AGG_HASH_REDUCE_EXACT_MAX_AVG_ROWS_PER_GROUP);
  U64 hash_fixed_max_rows_per_group = settings_u64(str8_lit("QE_AGG_HASH_REDUCE_FIXED_MAX_ROWS_PER_GROUP"), QE_AGG_HASH_REDUCE_FIXED_MAX_AVG_ROWS_PER_GROUP);
  U64 max_group_rows = 0;
  B32 hash_allowed = 0;
  // tec: F64 arguments that are all decimals with few places aggregate exactly as scaled integers, the scale of each rides in four bits
  B32 fixed_args_ok = plain_funcs;
  U32 fixed_scales = 0;
  for (U32 e = 0; e < num_exprs && fixed_args_ok; e++)
  {
    if (!exprs[e].arg_column || exprs[e].func_code == QE_AGG_FUNC_COUNT)
    {
      continue;
    }
    U32 owner = arg_owner[e];
    if (arg_fixed_scale[owner] < 0 || arg_fixed_max[owner] * g_qe_fixed_pow10[arg_fixed_scale[owner]] >= QE_AGG_FIXED_SCALED_LIMIT)
    {
      fixed_args_ok = 0;
    }
    else
    {
      fixed_scales |= (U32)arg_fixed_scale[owner] << (e * 4u);
    }
  }
  // tec: the hash kernel reads F64 arguments, so it is only for the queries that are not all narrow
  B32 fixed_ok = fixed_args_ok && !arg_narrow && settings_u64(str8_lit("QE_AGG_HASH_REDUCE_FIXED"), 1) != 0;
  
  //- tec: one whole number key with a small range needs no hash table
  B32 dense_ok = fixed_args_ok && num_group_cols == 1 && !(group_string_mask & 1u) && key_whole[0] &&
    !use_cpu && row_count < max_U32 && settings_u64(str8_lit("QE_AGG_DENSE"), 1) != 0;
  U64 dense_groups = 0;
  if (dense_ok)
  {
    dense_groups = (U64)(key_max[0] - key_min[0]) + 1;
    U64 acc_words = dense_groups * (2 + 2 * (U64)num_exprs);
    U64 mm_words = dense_groups * 2 * (U64)num_exprs;
    dense_ok = dense_groups >= 2 && acc_words <= QE_AGG_DENSE_SHARED_WORDS && mm_words <= QE_AGG_DENSE_SHARED_WORDS;
  }
  if (dense_ok)
  {
    U64 entries = dense_groups * num_exprs;
    GPU_Buffer* key_buf = group_resident[0];
    U64 dense_upload_bytes = 0;
    if (!key_buf)
    {
      key_buf = gpu_buffer_alloc_pooled(str8_lit("agg_dense_key_buf"), row_count * sizeof(F64), GPU_BufferFlag_Write, 0);
      dense_upload_bytes += row_count * sizeof(F64);
    }
    
    U64 dense_arg_size = arg_narrow ? sizeof(F32) : sizeof(F64);
    GPU_Buffer* dense_arg_bufs[QE_AGG_MAX_EXPRS] = {0};
    U32 dense_func_codes = 0;
    for (U32 e = 0; e < num_exprs; e++)
    {
      dense_func_codes |= ((U32)exprs[e].func_code & 0xfu) << (e * 4u);
      if (!exprs[e].arg_column)
      {
        continue;
      }
      if (arg_owner[e] != e)
      {
        dense_arg_bufs[e] = dense_arg_bufs[arg_owner[e]];
      }
      else if (arg_resident_buf[e])
      {
        dense_arg_bufs[e] = arg_resident_buf[e];
      }
      else
      {
        dense_arg_bufs[e] = gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_dense_arg_buf:%u", e), row_count * dense_arg_size, GPU_BufferFlag_Write, 0);
        dense_upload_bytes += row_count * dense_arg_size;
      }
    }
    
    Temp dense_scratch = scratch_begin(&arena, 1);
    U64 info_size = dense_groups * 2 * sizeof(U32);
    U64 sum_size = entries * 2 * sizeof(U32);
    U64 mm_size = entries * sizeof(U32);
    GPU_Buffer* info_buf = gpu_buffer_alloc_pooled(str8_lit("agg_dense_info_buf"), info_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* dsum_buf = gpu_buffer_alloc_pooled(str8_lit("agg_dense_sum_buf"), sum_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* dmin_buf = gpu_buffer_alloc_pooled(str8_lit("agg_dense_min_buf"), mm_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* dmax_buf = gpu_buffer_alloc_pooled(str8_lit("agg_dense_max_buf"), mm_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Kernel* dense_kernel = gpu_kernel_alloc(arg_narrow ? str8_lit("aggregate_dense_f32") : str8_lit("aggregate_dense_f64"));
    
    B32 dense_ran = 0;
    if (key_buf && info_buf && dsum_buf && dmin_buf && dmax_buf && dense_kernel)
    {
      gpu_kernel_set_arg_buffer(dense_kernel, 0, key_buf);
      gpu_kernel_set_arg_buffer(dense_kernel, 1, info_buf);
      gpu_kernel_set_arg_buffer(dense_kernel, 2, dsum_buf);
      gpu_kernel_set_arg_buffer(dense_kernel, 3, dmin_buf);
      gpu_kernel_set_arg_buffer(dense_kernel, 4, dmax_buf);
      for (U32 e = 0; e < QE_AGG_MAX_EXPRS; e++)
      {
        gpu_kernel_set_arg_buffer(dense_kernel, 5 + e, dense_arg_bufs[e] ? dense_arg_bufs[e] : info_buf);
      }
      gpu_kernel_set_arg_u64(dense_kernel, 0, row_count);
      gpu_kernel_set_arg_u64(dense_kernel, 1, num_exprs);
      gpu_kernel_set_arg_u64(dense_kernel, 2, dense_func_codes);
      gpu_kernel_set_arg_u64(dense_kernel, 3, fixed_scales);
      gpu_kernel_set_arg_u64(dense_kernel, 4, dense_groups);
      gpu_kernel_set_arg_u64(dense_kernel, 5, (U64)key_min[0]);
      
      U32* info_readback = push_array(dense_scratch.arena, U32, dense_groups * 2);
      U32* sum_readback = push_array(dense_scratch.arena, U32, entries * 2);
      U32* min_readback = push_array(dense_scratch.arena, U32, entries);
      U32* max_readback = push_array(dense_scratch.arena, U32, entries);
      
      GPU_Batch* dense_batch = gpu_batch_begin(dense_upload_bytes, info_size + sum_size + mm_size * 2);
      if (!group_resident[0])
      {
        gpu_batch_buffer_write(dense_batch, key_buf, group_numeric[0], row_count * sizeof(F64));
      }
      for (U32 e = 0; e < num_exprs; e++)
      {
        if (!dense_arg_bufs[e] || arg_resident_buf[e] || arg_owner[e] != e)
        {
          continue;
        }
        gpu_batch_buffer_write(dense_batch, dense_arg_bufs[e], arg_narrow ? (void*)expr_args_f32[e] : (void*)expr_args[e], row_count * dense_arg_size);
      }
      gpu_batch_buffer_zero(dense_batch, info_buf, info_size);
      gpu_batch_buffer_zero(dense_batch, dsum_buf, sum_size);
      gpu_batch_buffer_fill(dense_batch, dmin_buf, mm_size, 0x7fffffffu);
      gpu_batch_buffer_fill(dense_batch, dmax_buf, mm_size, 0x80000000u);
      U64 workgroups = (row_count + QE_AGG_DENSE_ROWS_PER_WORKGROUP - 1) / QE_AGG_DENSE_ROWS_PER_WORKGROUP;
      gpu_batch_kernel_execute(dense_batch, dense_kernel, (U32)(workgroups * QE_GPU_WORKGROUP_SIZE), QE_GPU_WORKGROUP_SIZE);
      gpu_batch_buffer_read(dense_batch, info_buf, info_readback, info_size);
      gpu_batch_buffer_read(dense_batch, dsum_buf, sum_readback, sum_size);
      gpu_batch_buffer_read(dense_batch, dmin_buf, min_readback, mm_size);
      gpu_batch_buffer_read(dense_batch, dmax_buf, max_readback, mm_size);
      dense_ran = gpu_batch_end(dense_batch);
      
      if (dense_ran)
      {
        U64 present = 0;
        for (U64 g = 0; g < dense_groups; g++)
        {
          present += info_readback[g * 2 + 1] != 0;
        }
        
        F64* dense_results = push_array(dense_scratch.arena, F64, Max(present * num_exprs, (U64)1));
        U64* dense_representatives = push_array(dense_scratch.arena, U64, Max(present, (U64)1));
        U64 out_group = 0;
        for (U64 g = 0; g < dense_groups; g++)
        {
          U32 count = info_readback[g * 2 + 1];
          if (count == 0)
          {
            continue;
          }
          U32 smallest_row = ~info_readback[g * 2];
          dense_representatives[out_group] = smallest_row;
          for (U32 e = 0; e < num_exprs; e++)
          {
            U64 index = g * num_exprs + e;
            F64 divisor = g_qe_fixed_pow10[(fixed_scales >> (e * 4u)) & 0xfu];
            S64 sum_scaled = (S64)(((U64)sum_readback[index * 2 + 1] << 32) | (U64)sum_readback[index * 2]);
            F64 value = 0.0;
            switch (exprs[e].func_code)
            {
              case QE_AGG_FUNC_COUNT:
              { 
                value = (F64)count; 
              } break;
              case QE_AGG_FUNC_SUM:  
              { 
                value = (F64)sum_scaled / divisor; 
              } break;
              case QE_AGG_FUNC_AVG:  
              { 
                value = (F64)sum_scaled / divisor / (F64)count;
              } break;
              case QE_AGG_FUNC_MIN:  
              { 
                value = (F64)(S32)min_readback[index] / divisor;
              } break;
              case QE_AGG_FUNC_MAX:   
              { 
                value = (F64)(S32)max_readback[index] / divisor; 
              } break;
              default: break;
            }
            dense_results[out_group * num_exprs + e] = value;
          }
          out_group += 1;
        }
        
        PLAN_RowSet dense_output_rows = *input;
        if (device)
        {
          dense_output_rows = qe_device_representative_rows(arena, device, input, dense_representatives, present);
        }
        result = qe_aggregate_build_output(arena, &dense_output_rows, column_list_ir, exprs, num_exprs, present, dense_representatives, dense_results);
        
        U64 qe_agg_t_dense = os_now_microseconds();
        log_debug("qe_aggregate: row_count=%llu num_groups=%llu dense phases (us): gather=%llu reduce=%llu total=%llu",
                  row_count, present, qe_agg_t_gathered - qe_agg_t_start, qe_agg_t_dense - qe_agg_t_gathered, qe_agg_t_dense - qe_agg_t_start);
        if (out_trace)
        {
          out_trace->input_row_count = row_count;
          out_trace->group_count = present;
          out_trace->gather_time_us = qe_agg_t_gathered - qe_agg_t_start;
          out_trace->reduce_time_us = qe_agg_t_dense - qe_agg_t_gathered;
        }
      }
    }
    if (dense_kernel)
    {
      gpu_kernel_release(dense_kernel);
    }
    scratch_end(dense_scratch);
    if (dense_ran)
    {
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    log_error("qe_aggregate: dense aggregation could not run, falling back to the hash aggregation");
  }
  GPU_Buffer* owner_buf = 0;
  GPU_Buffer* count_buf = 0;
  GPU_Buffer* row_slot_buf = 0;
  GPU_Buffer* overflow_buf = 0;
  U32* count_readback = 0;
  U32 overflow_readback = 0;
  U32 assign_passes = 0;
  
  for (;;)
  {
    num_slots = num_buckets * K;
    
    // tec: a table this large is compacted on the GPU below instead of coming back whole, when a path that can use the compact form is possible
    compact_table = num_group_cols > 0 && (all_counts || (plain_funcs && (arg_narrow || fixed_ok))) &&
      num_slots >= QE_AGG_GPU_COMPACT_MIN_SLOTS && settings_u64(str8_lit("QE_AGG_GPU_COMPACT"), 1) != 0;
    
    if (num_slots * sizeof(U32) > gpu_device_max_storage_buffer_range())
    {
      log_error("qe_aggregate: group-by hash table (%llu bytes) exceeds this GPU's maxStorageBufferRange "
                "(%llu bytes) - row_count=%llu is too large for a single hash table on this device",
                num_slots * sizeof(U32), gpu_device_max_storage_buffer_range(), row_count);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    if (num_slots * sizeof(U32) > settings_u64(str8_lit("GPU_MAX_BUFFER_SIZE"), GPU_MAX_BUFFER_SIZE))
    {
      log_debug("qe_aggregate: group-by hash table (%llu bytes, row_count=%llu) exceeds GPU_MAX_BUFFER_SIZE - "
                "allocating it as a single large buffer rather than chunking", num_slots * sizeof(U32), row_count);
    }
    
    owner_buf = gpu_buffer_alloc_pooled(str8_lit("agg_owner_buf"), num_slots * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    count_buf = gpu_buffer_alloc_pooled(str8_lit("agg_count_buf"), num_slots * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    row_slot_buf = gpu_buffer_alloc_pooled(str8_lit("agg_row_slot_buf"), row_count * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    overflow_buf = gpu_buffer_alloc_pooled(str8_lit("agg_overflow_buf"), sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    
    if (!owner_buf || !count_buf || !row_slot_buf || !overflow_buf)
    {
      log_error("qe_aggregate: failed to allocate group-by hash table GPU buffers (row_count=%llu, num_slots=%llu)",
                row_count, num_slots);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    
    gpu_kernel_set_arg_buffer(assign_kernel, 0, owner_buf);
    gpu_kernel_set_arg_buffer(assign_kernel, 1, count_buf);
    gpu_kernel_set_arg_buffer(assign_kernel, 2, row_slot_buf);
    gpu_kernel_set_arg_buffer(assign_kernel, 3, overflow_buf);
    
    for (U32 c = 0; c < QE_AGG_MAX_GROUP_COLS; c++)
    {
      GPU_Buffer* data_buf = (c < num_group_cols) ? group_col_bufs[c * 2 + 0] : overflow_buf;
      GPU_Buffer* off_buf = (c < num_group_cols && group_col_bufs[c * 2 + 1]) ? group_col_bufs[c * 2 + 1] : overflow_buf;
      gpu_kernel_set_arg_buffer(assign_kernel, 4 + c * 2, data_buf);
      gpu_kernel_set_arg_buffer(assign_kernel, 4 + c * 2 + 1, off_buf);
    }
    
    gpu_kernel_set_arg_u64(assign_kernel, 0, row_count);
    gpu_kernel_set_arg_u64(assign_kernel, 1, num_buckets);
    gpu_kernel_set_arg_u64(assign_kernel, 2, K);
    gpu_kernel_set_arg_u64(assign_kernel, 3, num_group_cols);
    gpu_kernel_set_arg_u64(assign_kernel, 4, group_string_mask);
    
    // tec: assign_download_bytes only covers count_buf now
    // owner_buf never needs to come back to the CPU, its occupancy is fully recoverable from count_buf
    // (a slot's count is nonzero iff some row claimed it), and owner_buf is only ever read by the assign kernel itself
    U64 assign_download_bytes = (compact_table ? 0 : (U64)num_slots * sizeof(U32)) + sizeof(U32);
    
    count_readback = compact_table ? 0 : push_array(scratch.arena, U32, num_slots);
    overflow_readback = 0;
    
    if (num_group_cols == 0)
    {
      // tec: no GROUP BY
      count_readback[0] = (U32)Min(row_count, (U64)max_U32);
    }
    else
    {
      assign_passes += 1;
      GPU_Batch* assign_batch = gpu_batch_begin(assign_upload_bytes, assign_download_bytes);
      gpu_batch_buffer_fill(assign_batch, owner_buf, num_slots * sizeof(U32), max_U32);
      gpu_batch_buffer_zero(assign_batch, count_buf, num_slots * sizeof(U32));
      gpu_batch_buffer_zero(assign_batch, overflow_buf, sizeof(U32));
      for (U32 c = 0; c < num_group_cols; c++)
      {
        if (group_string_mask & (1u << c))
        {
          gpu_batch_buffer_write(assign_batch, group_col_bufs[c * 2 + 0], group_string[c].data, group_col_sizes[c * 2 + 0]);
          gpu_batch_buffer_write(assign_batch, group_col_bufs[c * 2 + 1], group_string[c].offsets, group_col_sizes[c * 2 + 1]);
        }
        else if (!group_resident[c])
        {
          gpu_batch_buffer_write(assign_batch, group_col_bufs[c * 2 + 0], group_numeric[c], group_col_sizes[c * 2 + 0]);
        }
      }
      gpu_batch_kernel_execute(assign_batch, assign_kernel, (U32)row_count, QE_GPU_WORKGROUP_SIZE);
      if (!compact_table)
      {
        gpu_batch_buffer_read(assign_batch, count_buf, count_readback, num_slots * sizeof(U32));
      }
      gpu_batch_buffer_read(assign_batch, overflow_buf, &overflow_readback, sizeof(U32));
      B32 assign_ok = gpu_batch_end(assign_batch);
      if (!assign_ok)
      {
        log_error("qe_aggregate: GPU dispatch failed (device lost?) while assigning group-by hash slots");
        scratch_end(scratch);
        ProfEnd();
        return result;
      }
    }
    if (overflow_readback == 0 || num_group_cols == 0)
    {
      break;
    }
    
    // tec: prefer growing num_buckets
    // step size scales with how badly this size missed,
    // so a very wrong initial guess converges in one or two retries instead of creeping up by 2x each time
    if (num_buckets < max_num_buckets)
    {
      F64 overflow_frac = (F64)overflow_readback / (F64)row_count;
      U64 growth = (overflow_frac >= 0.20) ? 8 : (overflow_frac >= 0.05) ? 4 : 2;
      U64 next_num_buckets = Min(num_buckets * growth, max_num_buckets);
      log_debug("qe_aggregate: %u row(s) overflowed the group-by hash table with num_buckets=%llu - retrying with num_buckets=%llu",
                overflow_readback, num_buckets, next_num_buckets);
      num_buckets = next_num_buckets;
    }
    else
    {
      U64 next_K = K * 2;
      log_debug("qe_aggregate: %u row(s) overflowed the group-by hash table with K=%llu - retrying with K=%llu",
                overflow_readback, K, next_K);
      K = next_K;
    }
  }
  
  gpu_kernel_release(assign_kernel);
  U64 qe_agg_t_assigned = os_now_microseconds();
  
  U64 num_groups = 0;
  U32* slot_offsets = 0;
  U32* group_ids = 0;
  U32* compact_owner = 0;
  U32* compact_count = 0;
  GPU_Buffer* compact_offsets_buf = 0;
  B32 compacted = 0;
  
  // tec: the group count comes off the GPU first, and the table only comes back whole when the path chosen for it needs the slots
  if (compact_table)
  {
    U64 compact_blocks = (num_slots + 255) / 256;
    U64 compact_max_groups = Max(Min(num_slots, row_count), (U64)1);
    compact_offsets_buf = gpu_buffer_alloc_pooled(str8_lit("agg_compact_offsets_buf"), (num_slots + 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* block_sum_buf = gpu_buffer_alloc_pooled(str8_lit("agg_compact_block_sum_buf"), compact_blocks * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* out_owner_buf = gpu_buffer_alloc_pooled(str8_lit("agg_compact_owner_buf"), compact_max_groups * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* out_count_buf = gpu_buffer_alloc_pooled(str8_lit("agg_compact_count_buf"), compact_max_groups * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* total_buf = gpu_buffer_alloc_pooled(str8_lit("agg_compact_total_buf"), sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    GPU_Kernel* compact_kernel = gpu_kernel_alloc(str8_lit("aggregate_compact"));
    
    B32 have_groups = 0;
    if (compact_offsets_buf && block_sum_buf && out_owner_buf && out_count_buf && total_buf && compact_kernel)
    {
      gpu_kernel_set_arg_buffer(compact_kernel, 0, count_buf);
      gpu_kernel_set_arg_buffer(compact_kernel, 1, owner_buf);
      gpu_kernel_set_arg_buffer(compact_kernel, 2, compact_offsets_buf);
      gpu_kernel_set_arg_buffer(compact_kernel, 3, block_sum_buf);
      gpu_kernel_set_arg_buffer(compact_kernel, 4, out_owner_buf);
      gpu_kernel_set_arg_buffer(compact_kernel, 5, out_count_buf);
      gpu_kernel_set_arg_buffer(compact_kernel, 6, total_buf);
      gpu_kernel_set_arg_u64(compact_kernel, 0, num_slots);
      
      U32 total_readback = 0;
      GPU_Batch* compact_batch = gpu_batch_begin(0, sizeof(U32));
      gpu_kernel_set_arg_u64(compact_kernel, 1, 0);
      gpu_batch_kernel_execute(compact_batch, compact_kernel, (U32)(compact_blocks * 256), 256);
      gpu_kernel_set_arg_u64(compact_kernel, 1, 1);
      gpu_batch_kernel_execute(compact_batch, compact_kernel, 256, 256);
      gpu_kernel_set_arg_u64(compact_kernel, 1, 2);
      gpu_batch_kernel_execute(compact_batch, compact_kernel, (U32)(compact_blocks * 256), 256);
      gpu_batch_buffer_read(compact_batch, total_buf, &total_readback, sizeof(U32));
      have_groups = gpu_batch_end(compact_batch);
      num_groups = total_readback;
    }
    if (compact_kernel)
    {
      gpu_kernel_release(compact_kernel);
    }
    
    B32 wants_count_only = all_counts && settings_u64(str8_lit("QE_AGG_COUNT_ONLY"), 1) != 0;
    B32 wants_hash = plain_funcs && num_groups > QE_AGG_TILE_REDUCE_MAX_GROUPS && (arg_narrow || fixed_ok) &&
      row_count / Max(num_groups, (U64)1) <= Max(Max(hash_max_rows_per_group, hash_exact_max_rows_per_group), hash_fixed_max_rows_per_group) &&
      settings_u64(str8_lit("QE_AGG_HASH_REDUCE"), 1) != 0;
    if (have_groups && num_groups > 0 && num_groups <= compact_max_groups && (wants_count_only || wants_hash))
    {
      compact_owner = push_array(scratch.arena, U32, num_groups);
      compact_count = push_array(scratch.arena, U32, num_groups);
      GPU_Batch* dense_batch = gpu_batch_begin(0, num_groups * sizeof(U32) * 2);
      gpu_batch_buffer_read(dense_batch, out_owner_buf, compact_owner, num_groups * sizeof(U32));
      gpu_batch_buffer_read(dense_batch, out_count_buf, compact_count, num_groups * sizeof(U32));
      compacted = gpu_batch_end(dense_batch);
      if (compacted)
      {
        for (U64 g = 0; g < num_groups; g += 1)
        {
          max_group_rows = Max(max_group_rows, (U64)compact_count[g]);
        }
        hash_allowed = wants_hash && qe_aggregate_hash_allowed(exprs, num_exprs, arg_owner, arg_int_bound, row_count, num_groups, max_group_rows, hash_max_rows_per_group, hash_exact_max_rows_per_group, fixed_ok, hash_fixed_max_rows_per_group);
        // tec: nothing else uses the compact form here, so a hash reduce that turned out not allowed reads the whole table back instead
        if (!wants_count_only && !hash_allowed)
        {
          compacted = 0;
        }
      }
    }
    
    if (!compacted)
    {
      count_readback = push_array(scratch.arena, U32, num_slots);
      GPU_Batch* count_batch = gpu_batch_begin(0, num_slots * sizeof(U32));
      gpu_batch_buffer_read(count_batch, count_buf, count_readback, num_slots * sizeof(U32));
      gpu_batch_end(count_batch);
    }
  }
  
  if (!compacted)
  {
    qe_aggregate_slot_table(scratch.arena, count_readback, num_slots, num_group_cols > 0, &slot_offsets, &group_ids, &num_groups);
    if (plain_funcs && (arg_narrow || fixed_ok) && num_group_cols > 0 && num_groups > QE_AGG_TILE_REDUCE_MAX_GROUPS)
    {
      max_group_rows = 0;
      for (U64 g = 0; g < num_groups; g += 1)
      {
        max_group_rows = Max(max_group_rows, (U64)count_readback[group_ids[g]]);
      }
      hash_allowed = qe_aggregate_hash_allowed(exprs, num_exprs, arg_owner, arg_int_bound, row_count, num_groups, max_group_rows, hash_max_rows_per_group, hash_exact_max_rows_per_group, fixed_ok, hash_fixed_max_rows_per_group);
    }
  }
  
  // tec: only COUNTs, the assign pass already counted every group's rows, so only the owner rows come back
  B32 count_only = num_group_cols > 0 && num_groups > 0 && settings_u64(str8_lit("QE_AGG_COUNT_ONLY"), 1) != 0;
  for (U32 e = 0; e < num_exprs && count_only; e++)
  {
    if (exprs[e].func_code != QE_AGG_FUNC_COUNT)
    {
      count_only = 0;
    }
  }
  if (count_only)
  {
    U32* owner_by_group = compact_owner;
    U32* count_by_group = compact_count;
    B32 owner_ok = compacted;
    if (!compacted)
    {
      U32* owner_readback = push_array(scratch.arena, U32, num_slots);
      GPU_Batch* owner_batch = gpu_batch_begin(0, num_slots * sizeof(U32));
      gpu_batch_buffer_read(owner_batch, owner_buf, owner_readback, num_slots * sizeof(U32));
      owner_ok = gpu_batch_end(owner_batch);
      owner_by_group = push_array(scratch.arena, U32, num_groups);
      count_by_group = push_array(scratch.arena, U32, num_groups);
      for (U64 g = 0; g < num_groups; g++)
      {
        owner_by_group[g] = owner_readback[group_ids[g]];
        count_by_group[g] = count_readback[group_ids[g]];
      }
    }
    if (owner_ok)
    {
      F64* count_results = push_array(scratch.arena, F64, Max(num_groups * num_exprs, (U64)1));
      U64* count_representatives = push_array(scratch.arena, U64, num_groups);
      for (U64 g = 0; g < num_groups; g++)
      {
        count_representatives[g] = (owner_by_group[g] == max_U32) ? max_U64 : owner_by_group[g];
        for (U32 e = 0; e < num_exprs; e++)
        {
          count_results[g * num_exprs + e] = (F64)count_by_group[g];
        }
      }
      
      PLAN_RowSet count_output_rows = *input;
      if (device)
      {
        count_output_rows = qe_device_representative_rows(arena, device, input, count_representatives, num_groups);
      }
      result = qe_aggregate_build_output(arena, &count_output_rows, column_list_ir, exprs, num_exprs, num_groups, count_representatives, count_results);
      
      U64 qe_agg_t_counted = os_now_microseconds();
      log_debug("qe_aggregate: row_count=%llu num_groups=%llu count only phases (us): gather=%llu assign=%llu output=%llu total=%llu",
                row_count, num_groups,
                qe_agg_t_gathered - qe_agg_t_start,
                qe_agg_t_assigned - qe_agg_t_gathered,
                qe_agg_t_counted - qe_agg_t_assigned,
                qe_agg_t_counted - qe_agg_t_start);
      if (out_trace)
      {
        out_trace->input_row_count = row_count;
        out_trace->group_count = num_groups;
        out_trace->gather_time_us = qe_agg_t_gathered - qe_agg_t_start;
        out_trace->assign_time_us = qe_agg_t_assigned - qe_agg_t_gathered;
        out_trace->reduce_time_us = qe_agg_t_counted - qe_agg_t_assigned;
        out_trace->assign_passes = assign_passes;
      }
      
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    log_error("qe_aggregate: owner row download failed, falling back to the member list reduce");
  }
  
  // tec: few groups and only the plain aggregates, reduce by streaming the rows in order instead of through the sorted member lists.
  // F32 arguments stay on the member list reduce, where it is as fast, the win is F64 arguments whose scattered reads cost far more
  B32 tile_reduce = !compacted && num_group_cols > 0 && num_groups > 0 && num_groups <= QE_AGG_TILE_REDUCE_MAX_GROUPS && !arg_narrow &&
    num_slots <= QE_AGG_TILE_REDUCE_MAX_SLOTS && settings_u64(str8_lit("QE_AGG_TILE_REDUCE"), 1) != 0;
  for (U32 e = 0; e < num_exprs && tile_reduce; e++)
  {
    if (exprs[e].func_code > QE_AGG_FUNC_MAX)
    {
      tile_reduce = 0;
    }
  }
  if (tile_reduce)
  {
    GPU_Buffer* tile_arg_bufs[QE_AGG_MAX_EXPRS] = {0};
    U64 tile_arg_elem_size = arg_narrow ? sizeof(F32) : sizeof(F64);
    U64 tile_upload_bytes = num_slots * sizeof(U32);
    U32 tile_arg_mask = 0;
    U32 tile_func_codes = 0;
    for (U32 e = 0; e < num_exprs; e++)
    {
      tile_func_codes |= ((U32)exprs[e].func_code & 0xfu) << (e * 4u);
      if (!exprs[e].arg_column)
      {
        continue;
      }
      tile_arg_mask |= 1u << e;
      if (arg_owner[e] != e)
      {
        tile_arg_bufs[e] = tile_arg_bufs[arg_owner[e]];
      }
      else if (arg_resident_buf[e])
      {
        tile_arg_bufs[e] = arg_resident_buf[e];
      }
      else
      {
        tile_arg_bufs[e] = gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_arg_buf:%u", e), row_count * tile_arg_elem_size, GPU_BufferFlag_Write, 0);
        tile_upload_bytes += row_count * tile_arg_elem_size;
      }
    }
    
    // tec: about 320 workgroups keeps the GPU full, and the partials stay small when there are many groups
    U64 rows_per_wg = (row_count + 319) / 320;
    rows_per_wg = ((rows_per_wg + QE_AGG_TILE_REDUCE_SUB_TILE - 1) / QE_AGG_TILE_REDUCE_SUB_TILE) * QE_AGG_TILE_REDUCE_SUB_TILE;
    rows_per_wg = Max(rows_per_wg, (U64)QE_AGG_TILE_REDUCE_SUB_TILE);
    while (rows_per_wg < row_count && ((row_count + rows_per_wg - 1) / rows_per_wg) * num_groups * num_exprs * 4 * sizeof(F64) > MB(64))
    {
      rows_per_wg *= 2;
    }
    U64 wg_count = (row_count + rows_per_wg - 1) / rows_per_wg;
    U64 tile_partial_count = wg_count * num_groups * Max((U64)num_exprs, (U64)1) * 4;
    
    GPU_Buffer* slot_group_buf = gpu_buffer_alloc_pooled(str8_lit("agg_slot_group_buf"), num_slots * sizeof(U32), GPU_BufferFlag_Write, 0);
    GPU_Buffer* tile_partials_buf = gpu_buffer_alloc_pooled(str8_lit("agg_tile_partials_buf"), tile_partial_count * sizeof(F64), GPU_BufferFlag_ReadWrite, 0);
    GPU_Kernel* tile_kernel = gpu_kernel_alloc(str8_lit("aggregate_tile_reduce"));
    
    if (slot_group_buf && tile_partials_buf && tile_kernel)
    {
      U32* slot_group = push_array(scratch.arena, U32, num_slots);
      for (U64 s = 0; s < num_slots; s++)
      {
        slot_group[s] = max_U32;
      }
      for (U64 g = 0; g < num_groups; g++)
      {
        slot_group[group_ids[g]] = (U32)g;
      }
      
      gpu_kernel_set_arg_buffer(tile_kernel, 0, row_slot_buf);
      gpu_kernel_set_arg_buffer(tile_kernel, 1, slot_group_buf);
      gpu_kernel_set_arg_buffer(tile_kernel, 2, tile_partials_buf);
      for (U32 e = 0; e < QE_AGG_MAX_EXPRS; e++)
      {
        gpu_kernel_set_arg_buffer(tile_kernel, 3 + e, tile_arg_bufs[e] ? tile_arg_bufs[e] : row_slot_buf);
      }
      gpu_kernel_set_arg_u64(tile_kernel, 0, row_count);
      gpu_kernel_set_arg_u64(tile_kernel, 1, num_groups);
      gpu_kernel_set_arg_u64(tile_kernel, 2, num_exprs);
      gpu_kernel_set_arg_u64(tile_kernel, 3, rows_per_wg);
      gpu_kernel_set_arg_u64(tile_kernel, 4, arg_narrow ? 0 : 1);
      gpu_kernel_set_arg_u64(tile_kernel, 5, tile_arg_mask);
      gpu_kernel_set_arg_u64(tile_kernel, 6, tile_func_codes);
      
      F64* tile_partials = push_array(scratch.arena, F64, tile_partial_count);
      U32* owner_readback = push_array(scratch.arena, U32, num_slots);
      
      GPU_Batch* tile_batch = gpu_batch_begin(tile_upload_bytes, tile_partial_count * sizeof(F64) + num_slots * sizeof(U32));
      gpu_batch_buffer_write(tile_batch, slot_group_buf, slot_group, num_slots * sizeof(U32));
      for (U32 e = 0; e < num_exprs; e++)
      {
        if (!tile_arg_bufs[e] || arg_resident_buf[e] || arg_owner[e] != e)
        {
          continue;
        }
        void* arg_data = arg_narrow ? (void*)expr_args_f32[e] : (void*)expr_args[e];
        gpu_batch_buffer_write(tile_batch, tile_arg_bufs[e], arg_data, row_count * tile_arg_elem_size);
      }
      gpu_batch_kernel_execute(tile_batch, tile_kernel, (U32)(wg_count * QE_GPU_WORKGROUP_SIZE), QE_GPU_WORKGROUP_SIZE);
      gpu_batch_buffer_read(tile_batch, tile_partials_buf, tile_partials, tile_partial_count * sizeof(F64));
      gpu_batch_buffer_read(tile_batch, owner_buf, owner_readback, num_slots * sizeof(U32));
      B32 tile_ok = gpu_batch_end(tile_batch);
      gpu_kernel_release(tile_kernel);
      
      if (tile_ok)
      {
        F64* tile_results = push_array(scratch.arena, F64, Max(num_groups * num_exprs, (U64)1));
        U64* tile_representatives = push_array(scratch.arena, U64, num_groups);
        for (U64 g = 0; g < num_groups; g++)
        {
          U32 owner_row = owner_readback[group_ids[g]];
          tile_representatives[g] = (owner_row == max_U32) ? max_U64 : owner_row;
          for (U32 e = 0; e < num_exprs; e++)
          {
            F64 acc = 0.0, mn = 1.0e300, mx = -1.0e300;
            U64 count = 0;
            for (U64 w = 0; w < wg_count; w++)
            {
              F64* p = &tile_partials[((w * num_groups + g) * num_exprs + e) * 4];
              acc += p[0];
              count += (U64)p[1];
              if (p[2] < mn)
              {
                mn = p[2];
              }
              if (p[3] > mx)
              {
                mx = p[3];
              }
            }
            
            F64 value = 0.0;
            switch (exprs[e].func_code)
            {
              case QE_AGG_FUNC_COUNT: { value = (F64)count; } break;
              case QE_AGG_FUNC_SUM:   { value = acc; } break;
              case QE_AGG_FUNC_AVG:   { value = (count > 0) ? (acc / (F64)count) : 0.0; } break;
              case QE_AGG_FUNC_MIN:   { value = mn; } break;
              case QE_AGG_FUNC_MAX:   { value = mx; } break;
              default: break;
            }
            tile_results[g * num_exprs + e] = value;
          }
        }
        
        PLAN_RowSet tile_output_rows = *input;
        if (device)
        {
          tile_output_rows = qe_device_representative_rows(arena, device, input, tile_representatives, num_groups);
        }
        result = qe_aggregate_build_output(arena, &tile_output_rows, column_list_ir, exprs, num_exprs, num_groups, tile_representatives, tile_results);
        
        U64 qe_agg_t_tiled = os_now_microseconds();
        log_debug("qe_aggregate: row_count=%llu num_groups=%llu tile reduce phases (us): gather=%llu assign=%llu reduce=%llu total=%llu",
                  row_count, num_groups,
                  qe_agg_t_gathered - qe_agg_t_start,
                  qe_agg_t_assigned - qe_agg_t_gathered,
                  qe_agg_t_tiled - qe_agg_t_assigned,
                  qe_agg_t_tiled - qe_agg_t_start);
        if (out_trace)
        {
          out_trace->input_row_count = row_count;
          out_trace->group_count = num_groups;
          out_trace->gather_time_us = qe_agg_t_gathered - qe_agg_t_start;
          out_trace->assign_time_us = qe_agg_t_assigned - qe_agg_t_gathered;
          out_trace->reduce_time_us = qe_agg_t_tiled - qe_agg_t_assigned;
          out_trace->assign_passes = assign_passes;
        }
        
        scratch_end(scratch);
        ProfEnd();
        return result;
      }
      log_error("qe_aggregate: tile reduce dispatch failed, falling back to the sorted member list reduce");
    }
    else if (tile_kernel)
    {
      gpu_kernel_release(tile_kernel);
    }
  }
  
  // tec: high cardinality GROUP BY, one atomic hash agg dispatch over every row
  B32 hash_reduce = num_group_cols > 0 && num_groups > QE_AGG_TILE_REDUCE_MAX_GROUPS && (arg_narrow || fixed_ok) && hash_allowed &&
    settings_u64(str8_lit("QE_AGG_HASH_REDUCE"), 1) != 0;
  for (U32 e = 0; e < num_exprs && hash_reduce; e++)
  {
    if (exprs[e].func_code > QE_AGG_FUNC_MAX)
    {
      hash_reduce = 0;
    }
  }
  if (hash_reduce)
  {
    GPU_Buffer* hash_arg_bufs[QE_AGG_MAX_EXPRS] = {0};
    U64 hash_arg_size = fixed_ok ? sizeof(F64) : sizeof(F32);
    U64 hash_upload_bytes = compacted ? 0 : num_slots * sizeof(U32);
    U32 hash_func_codes = 0;
    for (U32 e = 0; e < num_exprs; e++)
    {
      hash_func_codes |= ((U32)exprs[e].func_code & 0xfu) << (e * 4u);
      if (!exprs[e].arg_column)
      {
        continue;
      }
      if (arg_owner[e] != e)
      {
        hash_arg_bufs[e] = hash_arg_bufs[arg_owner[e]];
      }
      else if (arg_resident_buf[e])
      {
        hash_arg_bufs[e] = arg_resident_buf[e];
      }
      else
      {
        hash_arg_bufs[e] = gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_hash_arg_buf:%u", e), row_count * hash_arg_size, GPU_BufferFlag_Write, 0);
        hash_upload_bytes += row_count * hash_arg_size;
      }
    }
    
    // tec: the compaction offsets already map every occupied slot to its dense group id
    GPU_Buffer* slot_group_buf = compacted ? compact_offsets_buf : gpu_buffer_alloc_pooled(str8_lit("agg_slot_group_buf"), num_slots * sizeof(U32), GPU_BufferFlag_Write, 0);
    U64 sums_size = Max(num_groups * num_exprs, (U64)1) * sizeof(U32);
    // tec: the fixed point sums are 64 bit, a low and a high word per entry
    U64 sum_words_size = fixed_ok ? sums_size * 2 : sums_size;
    GPU_Buffer* sum_buf = gpu_buffer_alloc_pooled(str8_lit("agg_hash_sum_buf"), sum_words_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* hcount_buf = gpu_buffer_alloc_pooled(str8_lit("agg_hash_count_buf"), sums_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* hmin_buf = gpu_buffer_alloc_pooled(str8_lit("agg_hash_min_buf"), sums_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Buffer* hmax_buf = gpu_buffer_alloc_pooled(str8_lit("agg_hash_max_buf"), sums_size, GPU_BufferFlag_ReadWrite, 0);
    GPU_Kernel* hash_kernel = gpu_kernel_alloc(fixed_ok ? str8_lit("aggregate_hash_reduce_fixed") : str8_lit("aggregate_hash_reduce_f32"));
    
    if (slot_group_buf && sum_buf && hcount_buf && hmin_buf && hmax_buf && hash_kernel)
    {
      U32* slot_group = 0;
      if (!compacted)
      {
        slot_group = push_array(scratch.arena, U32, num_slots);
        for (U64 s = 0; s < num_slots; s++)
        {
          slot_group[s] = max_U32;
        }
        for (U64 g = 0; g < num_groups; g++)
        {
          slot_group[group_ids[g]] = (U32)g;
        }
      }
      
      gpu_kernel_set_arg_buffer(hash_kernel, 0, row_slot_buf);
      gpu_kernel_set_arg_buffer(hash_kernel, 1, slot_group_buf);
      gpu_kernel_set_arg_buffer(hash_kernel, 2, sum_buf);
      gpu_kernel_set_arg_buffer(hash_kernel, 3, hcount_buf);
      gpu_kernel_set_arg_buffer(hash_kernel, 4, hmin_buf);
      gpu_kernel_set_arg_buffer(hash_kernel, 5, hmax_buf);
      for (U32 e = 0; e < QE_AGG_MAX_EXPRS; e++)
      {
        gpu_kernel_set_arg_buffer(hash_kernel, 6 + e, hash_arg_bufs[e] ? hash_arg_bufs[e] : row_slot_buf);
      }
      gpu_kernel_set_arg_u64(hash_kernel, 0, row_count);
      gpu_kernel_set_arg_u64(hash_kernel, 1, num_exprs);
      gpu_kernel_set_arg_u64(hash_kernel, 2, hash_func_codes);
      gpu_kernel_set_arg_u64(hash_kernel, 3, fixed_scales);
      
      U32* sum_readback = push_array(scratch.arena, U32, Max(num_groups * num_exprs, (U64)1) * 2);
      U32* count_readback2 = push_array(scratch.arena, U32, Max(num_groups * num_exprs, (U64)1));
      U32* min_readback = push_array(scratch.arena, U32, Max(num_groups * num_exprs, (U64)1));
      U32* max_readback = push_array(scratch.arena, U32, Max(num_groups * num_exprs, (U64)1));
      U32* owner_readback = compacted ? compact_owner : push_array(scratch.arena, U32, num_slots);
      
      GPU_Batch* hash_batch = gpu_batch_begin(hash_upload_bytes, sum_words_size + sums_size * 3 + (compacted ? 0 : num_slots * sizeof(U32)));
      if (!compacted)
      {
        gpu_batch_buffer_write(hash_batch, slot_group_buf, slot_group, num_slots * sizeof(U32));
      }
      for (U32 e = 0; e < num_exprs; e++)
      {
        if (!hash_arg_bufs[e] || arg_resident_buf[e] || arg_owner[e] != e)
        {
          continue;
        }
        gpu_batch_buffer_write(hash_batch, hash_arg_bufs[e], fixed_ok ? (void*)expr_args[e] : (void*)expr_args_f32[e], row_count * hash_arg_size);
      }
      gpu_batch_buffer_zero(hash_batch, sum_buf, sum_words_size);
      gpu_batch_buffer_zero(hash_batch, hcount_buf, sums_size);
      gpu_batch_buffer_fill(hash_batch, hmin_buf, sums_size, fixed_ok ? 0x7fffffffu : qe_f32_to_bits(1.0e30f));
      gpu_batch_buffer_fill(hash_batch, hmax_buf, sums_size, fixed_ok ? 0x80000000u : qe_f32_to_bits(-1.0e30f));
      gpu_batch_kernel_execute(hash_batch, hash_kernel, (U32)row_count, QE_GPU_WORKGROUP_SIZE);
      gpu_batch_buffer_read(hash_batch, sum_buf, sum_readback, sum_words_size);
      gpu_batch_buffer_read(hash_batch, hcount_buf, count_readback2, sums_size);
      gpu_batch_buffer_read(hash_batch, hmin_buf, min_readback, sums_size);
      gpu_batch_buffer_read(hash_batch, hmax_buf, max_readback, sums_size);
      if (!compacted)
      {
        gpu_batch_buffer_read(hash_batch, owner_buf, owner_readback, num_slots * sizeof(U32));
      }
      B32 hash_ok = gpu_batch_end(hash_batch);
      gpu_kernel_release(hash_kernel);
      
      if (hash_ok)
      {
        F64* hash_results = push_array(scratch.arena, F64, Max(num_groups * num_exprs, (U64)1));
        U64* hash_representatives = push_array(scratch.arena, U64, num_groups);
        for (U64 g = 0; g < num_groups; g++)
        {
          U32 owner_row = compacted ? owner_readback[g] : owner_readback[group_ids[g]];
          hash_representatives[g] = (owner_row == max_U32) ? max_U64 : owner_row;
          for (U32 e = 0; e < num_exprs; e++)
          {
            U64 idx = g * num_exprs + e;
            F64 acc = 0.0;
            F64 count = (F64)count_readback2[idx];
            F64 mn = 0.0;
            F64 mx = 0.0;
            if (fixed_ok)
            {
              // tec: back out of the scaled integers, the sum is the 64 bit value held in two words
              F64 divisor = g_qe_fixed_pow10[(fixed_scales >> (e * 4u)) & 0xfu];
              S64 sum_scaled = (S64)(((U64)sum_readback[idx * 2 + 1] << 32) | (U64)sum_readback[idx * 2]);
              acc = (F64)sum_scaled / divisor;
              mn = (F64)(S32)min_readback[idx] / divisor;
              mx = (F64)(S32)max_readback[idx] / divisor;
            }
            else
            {
              acc = (F64)qe_bits_to_f32(sum_readback[idx]);
              mn = (F64)qe_bits_to_f32(min_readback[idx]);
              mx = (F64)qe_bits_to_f32(max_readback[idx]);
            }
            
            F64 value = 0.0;
            switch (exprs[e].func_code)
            {
              case QE_AGG_FUNC_COUNT: { value = count; } break;
              case QE_AGG_FUNC_SUM:   { value = acc; } break;
              case QE_AGG_FUNC_AVG:   { value = (count > 0) ? (acc / count) : 0.0; } break;
              case QE_AGG_FUNC_MIN:   { value = mn; } break;
              case QE_AGG_FUNC_MAX:   { value = mx; } break;
              default: break;
            }
            hash_results[idx] = value;
          }
        }
        
        PLAN_RowSet hash_output_rows = *input;
        if (device)
        {
          hash_output_rows = qe_device_representative_rows(arena, device, input, hash_representatives, num_groups);
        }
        result = qe_aggregate_build_output(arena, &hash_output_rows, column_list_ir, exprs, num_exprs, num_groups, hash_representatives, hash_results);
        
        U64 qe_agg_t_hashed = os_now_microseconds();
        log_debug("qe_aggregate: row_count=%llu num_groups=%llu hash reduce phases (us): gather=%llu assign=%llu reduce=%llu total=%llu",
                  row_count, num_groups,
                  qe_agg_t_gathered - qe_agg_t_start,
                  qe_agg_t_assigned - qe_agg_t_gathered,
                  qe_agg_t_hashed - qe_agg_t_assigned,
                  qe_agg_t_hashed - qe_agg_t_start);
        if (out_trace)
        {
          out_trace->input_row_count = row_count;
          out_trace->group_count = num_groups;
          out_trace->gather_time_us = qe_agg_t_gathered - qe_agg_t_start;
          out_trace->assign_time_us = qe_agg_t_assigned - qe_agg_t_gathered;
          out_trace->reduce_time_us = qe_agg_t_hashed - qe_agg_t_assigned;
          out_trace->assign_passes = assign_passes;
        }
        
        scratch_end(scratch);
        ProfEnd();
        return result;
      }
      log_error("qe_aggregate: hash reduce dispatch failed, falling back to the sorted member list reduce");
    }
    else if (hash_kernel)
    {
      gpu_kernel_release(hash_kernel);
    }
  }
  
  // tec: the member list reduce needs the whole slot table, which a compacted table never brought back
  if (compacted)
  {
    count_readback = push_array(scratch.arena, U32, num_slots);
    GPU_Batch* fallback_batch = gpu_batch_begin(0, num_slots * sizeof(U32));
    gpu_batch_buffer_read(fallback_batch, count_buf, count_readback, num_slots * sizeof(U32));
    gpu_batch_end(fallback_batch);
    qe_aggregate_slot_table(scratch.arena, count_readback, num_slots, 1, &slot_offsets, &group_ids, &num_groups);
    compacted = 0;
  }
  
  //- tec: pass 2/3 scatter rows into per slot CSR member lists, pass 3/3 one thread per group reduces its own member rows
  // tec: a table with few slots would queue every row's cursor atomic on a few cache lines, so it counts per tile in shared memory instead
  B32 tiled_scatter = num_group_cols > 0 && num_slots <= QE_AGG_TILED_SCATTER_MAX_SLOTS;
  GPU_Kernel* scatter_kernel = gpu_kernel_alloc(tiled_scatter ? str8_lit("csr_scatter_tiled") : str8_lit("csr_scatter"));
  GPU_Kernel* reduce_kernel = gpu_kernel_alloc(arg_narrow ? str8_lit("aggregate_reduce_f32") : str8_lit("aggregate_reduce"));
  if (!scatter_kernel || !reduce_kernel)
  {
    log_error("qe_aggregate: failed to alloc '%s' kernel", scatter_kernel ? "aggregate_reduce" : "csr_scatter");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  // tec: split each group's CSR range into ceil(member_count / agg_rows_per_chunk) chunks,
  // each getting its own reduce kernel workgroup
  U64 agg_rows_per_chunk = settings_u64(str8_lit("QE_AGG_ROWS_PER_CHUNK"), 4096);
  U64* chunks_per_group = push_array(scratch.arena, U64, Max(num_groups, 1));
  U64* chunk_base_of_group = push_array(scratch.arena, U64, Max(num_groups, 1));
  U64 total_chunks = 0;
  for (U64 g = 0; g < num_groups; g++)
  {
    U32 slot = group_ids[g];
    U64 member_count = slot_offsets[slot + 1] - slot_offsets[slot];
    U64 c = (member_count + agg_rows_per_chunk - 1) / agg_rows_per_chunk;
    if (c < 1) c = 1;
    chunks_per_group[g] = c;
    chunk_base_of_group[g] = total_chunks;
    total_chunks += c;
  }
  
  U32* chunk_range = push_array(scratch.arena, U32, Max(total_chunks, 1) * 2);
  U32* chunk_group = push_array(scratch.arena, U32, Max(total_chunks, 1));
  for (U64 g = 0; g < num_groups; g++)
  {
    U32 slot = group_ids[g];
    U32 group_start = slot_offsets[slot];
    U32 group_end = slot_offsets[slot + 1];
    for (U64 k = 0; k < chunks_per_group[g]; k++)
    {
      U64 chunk_idx = chunk_base_of_group[g] + k;
      U32 c_start = group_start + (U32)(k * agg_rows_per_chunk);
      U32 c_end = Min(c_start + (U32)agg_rows_per_chunk, group_end);
      chunk_range[chunk_idx * 2 + 0] = c_start;
      chunk_range[chunk_idx * 2 + 1] = c_end;
      chunk_group[chunk_idx] = (U32)g;
    }
  }
  
  // tec: no GROUP BY. chunk_range already is a partition of raw row indices [0,row_count),
  // so the reduce kernel can read rows directly instead of indirecting through group_member_rows,
  // and members_buf (a full row_count-sized scatter target) is never touched
  B32 identity_mode = (num_group_cols == 0);
  
  U64 cursor_size = num_slots * sizeof(U32);
  U64 members_size = identity_mode ? sizeof(U32) : Max(row_count, 1) * sizeof(U32);
  U64 chunk_range_size = Max(total_chunks, 1) * 2 * sizeof(U32);
  U64 chunk_group_size = Max(total_chunks, 1) * sizeof(U32);
  U64 repr_size = Max(num_groups, 1) * sizeof(U32);
  U64 partials_size = Max(total_chunks, 1) * Max(num_exprs, 1) * 4 * sizeof(F64);
  U64 results_size = Max(num_groups * Max(num_exprs, 1), 1) * sizeof(F64);
  
  GPU_Buffer* cursor_buf = gpu_buffer_alloc_pooled(str8_lit("agg_cursor_buf"), cursor_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* members_buf = gpu_buffer_alloc_pooled(str8_lit("agg_members_buf"), members_size, GPU_BufferFlag_ReadWrite, 0);
  
  gpu_kernel_set_arg_buffer(scatter_kernel, 0, row_slot_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 1, cursor_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 2, members_buf);
  gpu_kernel_set_arg_u64(scatter_kernel, 0, row_count);
  gpu_kernel_set_arg_u64(scatter_kernel, 1, num_slots);
  
  GPU_Buffer* chunk_range_buf = gpu_buffer_alloc_pooled(str8_lit("agg_chunk_range_buf"), chunk_range_size, GPU_BufferFlag_Write, 0);
  GPU_Buffer* chunk_group_buf = gpu_buffer_alloc_pooled(str8_lit("agg_chunk_group_buf"), chunk_group_size, GPU_BufferFlag_Write, 0);
  GPU_Buffer* repr_buf = gpu_buffer_alloc_pooled(str8_lit("agg_repr_buf"), repr_size, GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* partials_buf = gpu_buffer_alloc_pooled(str8_lit("agg_partials_buf"), partials_size, GPU_BufferFlag_ReadWrite, 0);
  
  // tec: APPROX_COUNT_DISTINCT keeps one HyperLogLog sketch (an array of registers) per (group, expr) instead of a partials_buf-style scalar
  U64 hll_precision = settings_u64(str8_lit("QE_HLL_PRECISION"), 10);
  U64 hll_words = 1ull << hll_precision;
  U32 hll_slot_of_expr[QE_AGG_MAX_EXPRS] = {0};
  U32 num_hll_exprs = 0;
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (exprs[e].func_code == QE_AGG_FUNC_APPROX_COUNT_DISTINCT)
    {
      hll_slot_of_expr[e] = num_hll_exprs++;
    }
  }
  
  U64 sketch_size = Max(num_groups, 1) * Max(num_hll_exprs, 1) * hll_words * sizeof(U32);
  GPU_Buffer* sketch_buf = repr_buf; // tec: unused filler binding when there's no APPROX_COUNT_DISTINCT expr
  U32* sketch_readback = 0;
  
  if (num_hll_exprs > 0)
  {
    U64 sketch_max_bytes = settings_u64(str8_lit("QE_HLL_SKETCH_MAX_TOTAL_BYTES"), MB(64));
    if (sketch_size > gpu_device_max_storage_buffer_range())
    {
      log_error("qe_aggregate: APPROX_COUNT_DISTINCT sketch buffer (%llu bytes) exceeds this GPU's maxStorageBufferRange "
                "(%llu bytes) - num_groups=%llu at QE_HLL_PRECISION=%llu is too large for this device",
                sketch_size, gpu_device_max_storage_buffer_range(), num_groups, hll_precision);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    if (sketch_size > sketch_max_bytes)
    {
      log_error("qe_aggregate: APPROX_COUNT_DISTINCT sketch buffer (%llu bytes, num_groups=%llu, QE_HLL_PRECISION=%llu) "
                "exceeds QE_HLL_SKETCH_MAX_TOTAL_BYTES (%llu bytes) - reduce QE_HLL_PRECISION or GROUP BY cardinality",
                sketch_size, num_groups, hll_precision, sketch_max_bytes);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    
    sketch_buf = gpu_buffer_alloc_pooled(str8_lit("agg_sketch_buf"), sketch_size, GPU_BufferFlag_ReadWrite, 0);
    if (!sketch_buf)
    {
      log_error("qe_aggregate: failed to allocate APPROX_COUNT_DISTINCT sketch GPU buffer (%llu bytes)", sketch_size);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    sketch_readback = push_array(scratch.arena, U32, Max(num_groups, 1) * num_hll_exprs * hll_words);
  }
  
  // tec: APPROX_PERCENTILE keeps one t-digest centroid per GPU thread per chunk
  // (one real sampled value from that thread's rows, weighted by how many)
  U32 pct_slot_of_expr[QE_AGG_MAX_EXPRS] = {0};
  U32 num_pct_exprs = 0;
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (exprs[e].func_code == QE_AGG_FUNC_APPROX_PERCENTILE)
    {
      pct_slot_of_expr[e] = num_pct_exprs++;
    }
  }
  
  U64 digest_size = Max(total_chunks, 1) * Max(num_pct_exprs, 1) * QE_GPU_WORKGROUP_SIZE * sizeof(F32);
  // tec: unused filler binding when there's no APPROX_PERCENTILE expr
  GPU_Buffer* digest_mean_buf = repr_buf;  
  GPU_Buffer* digest_weight_buf = repr_buf;
  F32* digest_mean_readback = 0;
  F32* digest_weight_readback = 0;
  
  if (num_pct_exprs > 0)
  {
    U64 digest_max_bytes = settings_u64(str8_lit("QE_TDIGEST_SKETCH_MAX_TOTAL_BYTES"), MB(64));
    if (digest_size > gpu_device_max_storage_buffer_range())
    {
      log_error("qe_aggregate: APPROX_PERCENTILE digest buffer (%llu bytes) exceeds this GPU's maxStorageBufferRange "
                "(%llu bytes) - total_chunks=%llu is too large for this device",
                digest_size, gpu_device_max_storage_buffer_range(), total_chunks);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    if (digest_size * 2 > digest_max_bytes)
    {
      log_error("qe_aggregate: APPROX_PERCENTILE digest buffers (%llu bytes, total_chunks=%llu) exceed "
                "QE_TDIGEST_SKETCH_MAX_TOTAL_BYTES (%llu bytes) - increase QE_AGG_ROWS_PER_CHUNK to shrink total_chunks",
                digest_size * 2, total_chunks, digest_max_bytes);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    
    digest_mean_buf = gpu_buffer_alloc_pooled(str8_lit("agg_digest_mean_buf"), digest_size, GPU_BufferFlag_ReadWrite, 0);
    digest_weight_buf = gpu_buffer_alloc_pooled(str8_lit("agg_digest_weight_buf"), digest_size, GPU_BufferFlag_ReadWrite, 0);
    if (!digest_mean_buf || !digest_weight_buf)
    {
      log_error("qe_aggregate: failed to allocate APPROX_PERCENTILE digest GPU buffers (%llu bytes each)", digest_size);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    U64 digest_elems = Max(total_chunks, 1) * num_pct_exprs * QE_GPU_WORKGROUP_SIZE;
    digest_mean_readback = push_array(scratch.arena, F32, digest_elems);
    digest_weight_readback = push_array(scratch.arena, F32, digest_elems);
  }
  
  gpu_kernel_set_arg_buffer(reduce_kernel, 0, identity_mode ? chunk_range_buf : members_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 1, chunk_range_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 2, chunk_group_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 3, repr_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 4, partials_buf);
  
  // tec: exprs sharing the same source column also share the same GPU buffer and upload
  GPU_Buffer* arg_bufs[QE_AGG_MAX_EXPRS] = {0};
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!exprs[e].arg_column) continue;
    
    if (arg_owner[e] != e)
    {
      arg_bufs[e] = arg_bufs[arg_owner[e]];
      continue;
    }
    
    if (arg_resident_buf[e])
    {
      arg_bufs[e] = arg_resident_buf[e];
    }
    else
    {
      arg_bufs[e] = gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_arg_buf:%u", e), row_count * (arg_narrow ? sizeof(F32) : sizeof(F64)), GPU_BufferFlag_Write, 0);
    }
  }
  
  for (U32 e = 0; e < QE_AGG_MAX_EXPRS; e++)
  {
    gpu_kernel_set_arg_buffer(reduce_kernel, 5 + e, arg_bufs[e] ? arg_bufs[e] : repr_buf);
  }
  gpu_kernel_set_arg_buffer(reduce_kernel, 13, sketch_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 14, digest_mean_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 15, digest_weight_buf);
  
  U32 func_codes_packed = 0;
  U32 hll_slot_packed = 0;
  U32 pct_slot_packed = 0;
  for (U32 e = 0; e < num_exprs; e++)
  {
    func_codes_packed |= (exprs[e].func_code & 0xfu) << (e * 4u);
    if (exprs[e].func_code == QE_AGG_FUNC_APPROX_COUNT_DISTINCT)
    {
      hll_slot_packed |= (hll_slot_of_expr[e] & 0xfu) << (e * 4u);
    }
    if (exprs[e].func_code == QE_AGG_FUNC_APPROX_PERCENTILE)
    {
      pct_slot_packed |= (pct_slot_of_expr[e] & 0xfu) << (e * 4u);
    }
  }
  
  gpu_kernel_set_arg_u64(reduce_kernel, 0, total_chunks);
  gpu_kernel_set_arg_u64(reduce_kernel, 1, num_exprs);
  gpu_kernel_set_arg_u64(reduce_kernel, 2, func_codes_packed);
  gpu_kernel_set_arg_u64(reduce_kernel, 3, identity_mode ? 1 : 0);
  gpu_kernel_set_arg_u64(reduce_kernel, 4, hll_precision);
  gpu_kernel_set_arg_u64(reduce_kernel, 5, hll_slot_packed);
  gpu_kernel_set_arg_u64(reduce_kernel, 6, num_hll_exprs);
  // tec: pack both remaining values into the two 32-bit halves of the last U64 slot instead
  gpu_kernel_set_arg_u64(reduce_kernel, 7, ((U64)pct_slot_packed << 32) | (U64)num_pct_exprs);
  
  U64 arg_elem_size = arg_narrow ? sizeof(F32) : sizeof(F64);
  U64 reduce_upload_bytes = cursor_size + chunk_range_size + chunk_group_size;
  for (U32 e = 0; e < num_exprs; e++) 
  {
    if (arg_bufs[e] && !arg_resident_buf[e]) 
    {
      reduce_upload_bytes += row_count * arg_elem_size;
    }
  }
  U64 reduce_download_bytes = repr_size + partials_size + (num_hll_exprs > 0 ? sketch_size : 0) + (num_pct_exprs > 0 ? digest_size * 2 : 0);
  
  U32* repr32 = push_array(scratch.arena, U32, Max(num_groups, 1));
  F64* partials_readback = push_array(scratch.arena, F64, Max(total_chunks, 1) * Max(num_exprs, 1) * 4);
  F64* results_readback = push_array(scratch.arena, F64, Max(num_groups * Max(num_exprs, 1), 1));
  
  // tec: the scatter output only feeds the reduce, so both dispatches and the result download share one submit when the optimizer fuses them
  B32 fuse_scatter_reduce = hints && hints->fuse_round_trips;
  U64 scatter_download_bytes = fuse_scatter_reduce ? reduce_download_bytes : 0;
  GPU_Batch* scatter_batch = gpu_batch_begin(reduce_upload_bytes, scatter_download_bytes);
  gpu_batch_buffer_write(scatter_batch, chunk_range_buf, chunk_range, chunk_range_size);
  gpu_batch_buffer_write(scatter_batch, chunk_group_buf, chunk_group, chunk_group_size);
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!arg_bufs[e] || arg_resident_buf[e])
    {
      continue;
    }
    B32 already_uploaded = 0;
    for (U32 e2 = 0; e2 < e; e2++) 
    {
      if (arg_bufs[e2] == arg_bufs[e]) 
      { 
        already_uploaded = 1; 
        break; 
      }
    }
    if (!already_uploaded)
    {
      void* arg_data = arg_narrow ? (void*)expr_args_f32[e] : (void*)expr_args[e];
      gpu_batch_buffer_write(scatter_batch, arg_bufs[e], arg_data, row_count * arg_elem_size);
    }
  }
  if (!identity_mode)
  {
    // tec: no scatter needed in identity_mode - chunk_range already partitions raw row indices
    gpu_batch_buffer_write(scatter_batch, cursor_buf, slot_offsets, cursor_size);
    U32 scatter_threads = (U32)row_count;
    if (tiled_scatter)
    {
      scatter_threads = (U32)(((row_count + QE_AGG_TILED_SCATTER_TILE_ROWS - 1) / QE_AGG_TILED_SCATTER_TILE_ROWS) * QE_GPU_WORKGROUP_SIZE);
    }
    gpu_batch_kernel_execute(scatter_batch, scatter_kernel, scatter_threads, QE_GPU_WORKGROUP_SIZE);
  }
  
  GPU_Batch* reduce_batch = scatter_batch;
  if (!fuse_scatter_reduce)
  {
    gpu_batch_end(scatter_batch);
    reduce_batch = gpu_batch_begin(0, reduce_download_bytes);
  }
  if (num_hll_exprs > 0)
  {
    gpu_batch_buffer_zero(reduce_batch, sketch_buf, sketch_size);
  }
  // tec: one workgroup per chunk (a group with many rows spans many chunks/workgroups)
  gpu_batch_kernel_execute(reduce_batch, reduce_kernel, (U32)Max(total_chunks, 1) * QE_GPU_WORKGROUP_SIZE, QE_GPU_WORKGROUP_SIZE);
  gpu_batch_buffer_read(reduce_batch, repr_buf, repr32, repr_size);
  gpu_batch_buffer_read(reduce_batch, partials_buf, partials_readback, partials_size);
  if (num_hll_exprs > 0)
  {
    gpu_batch_buffer_read(reduce_batch, sketch_buf, sketch_readback, sketch_size);
  }
  if (num_pct_exprs > 0)
  {
    gpu_batch_buffer_read(reduce_batch, digest_mean_buf, digest_mean_readback, digest_size);
    gpu_batch_buffer_read(reduce_batch, digest_weight_buf, digest_weight_readback, digest_size);
  }
  gpu_batch_end(reduce_batch);
  log_debug("qe_aggregate: reduce batch (row_count=%llu, is_string=%d, total_chunks=%llu) GPU time: %llu microseconds",
            row_count, (group_string_mask != 0), total_chunks, gpu_get_executed_kernel_time_microseconds());
  
  gpu_kernel_release(scatter_kernel);
  U64 qe_agg_t_reduced = os_now_microseconds();
  
  // tec: combine each group's chunk partials into its final SUM/AVG/MIN/MAX/COUNT
  // cheap even when total_chunks is large, since any one group's own chunk count stays small
  for (U64 g = 0; g < num_groups; g++)
  {
    for (U32 e = 0; e < num_exprs; e++)
    {
      F64 acc = 0.0, mn = 1.0e300, mx = -1.0e300;
      U64 count = 0;
      for (U64 k = 0; k < chunks_per_group[g]; k++)
      {
        U64 chunk_idx = chunk_base_of_group[g] + k;
        F64* p = &partials_readback[(chunk_idx * num_exprs + e) * 4];
        acc += p[0];
        count += (U64)p[1];
        if (p[2] < mn) 
        {
          mn = p[2];
        }
        if (p[3] > mx) 
        {
          mx = p[3];
        }
      }
      
      F64 result;
      if (exprs[e].func_code == QE_AGG_FUNC_COUNT) 
      {
        result = (F64)count;
      }
      else if (exprs[e].func_code == QE_AGG_FUNC_SUM) 
      {
        result = acc;
      }
      else if (exprs[e].func_code == QE_AGG_FUNC_AVG) 
      {
        result = (count > 0) ? (acc / (F64)count) : 0.0;
      }
      else if (exprs[e].func_code == QE_AGG_FUNC_MIN) 
      {
        result = mn;
      }
      else if (exprs[e].func_code == QE_AGG_FUNC_MAX)
      {
        result = mx;
      }
      else if (exprs[e].func_code == QE_AGG_FUNC_APPROX_COUNT_DISTINCT)
      {
        U32* registers = &sketch_readback[(g * num_hll_exprs + hll_slot_of_expr[e]) * hll_words];
        // tec: round rather than truncate
        result = round_f64(qe_hll_estimate_cardinality(registers, hll_words));
      }
      else if (exprs[e].func_code == QE_AGG_FUNC_APPROX_PERCENTILE)
      {
        U32 pct_slot = pct_slot_of_expr[e];
        U64 max_centroids = chunks_per_group[g] * QE_GPU_WORKGROUP_SIZE;
        QE_TDigestCentroid* centroids = push_array(scratch.arena, QE_TDigestCentroid, Max(max_centroids, 1));
        U64 valid_count = 0;
        for (U64 k = 0; k < chunks_per_group[g]; k++)
        {
          U64 chunk_idx = chunk_base_of_group[g] + k;
          U64 base = (chunk_idx * num_pct_exprs + pct_slot) * QE_GPU_WORKGROUP_SIZE;
          for (U32 t = 0; t < QE_GPU_WORKGROUP_SIZE; t++)
          {
            F32 w = digest_weight_readback[base + t];
            if (w > 0.0f)
            {
              centroids[valid_count].mean = digest_mean_readback[base + t];
              centroids[valid_count].weight = w;
              valid_count++;
            }
          }
        }
        result = qe_tdigest_estimate_percentile(centroids, valid_count, exprs[e].f64_param);
      }
      else result = mx;
      
      results_readback[g * num_exprs + e] = result;
    }
  }
  
  U64* representative_readback = push_array(scratch.arena, U64, Max(num_groups, 1));
  for (U64 g = 0; g < num_groups; g++) representative_readback[g] = (repr32[g] == max_U32) ? max_U64 : repr32[g];
  
  gpu_kernel_release(reduce_kernel);
  U64 qe_agg_t_combined = os_now_microseconds();
  
  PLAN_RowSet output_rows = *input;
  if (device)
  {
    output_rows = qe_device_representative_rows(arena, device, input, representative_readback, num_groups);
  }
  result = qe_aggregate_build_output(arena, &output_rows, column_list_ir, exprs, num_exprs, num_groups, representative_readback, results_readback);
  U64 qe_agg_t_output = os_now_microseconds();
  
  log_debug("qe_aggregate: row_count=%llu num_groups=%llu phases (us): gather=%llu assign=%llu reduce=%llu combine=%llu output=%llu total=%llu",
            row_count, num_groups,
            qe_agg_t_gathered - qe_agg_t_start,
            qe_agg_t_assigned - qe_agg_t_gathered,
            qe_agg_t_reduced - qe_agg_t_assigned,
            qe_agg_t_combined - qe_agg_t_reduced,
            qe_agg_t_output - qe_agg_t_combined,
            qe_agg_t_output - qe_agg_t_start);
  if (out_trace)
  {
    out_trace->input_row_count = row_count;
    out_trace->group_count = num_groups;
    out_trace->gather_time_us = qe_agg_t_gathered - qe_agg_t_start;
    out_trace->assign_time_us = qe_agg_t_assigned - qe_agg_t_gathered;
    out_trace->reduce_time_us = qe_agg_t_reduced - qe_agg_t_assigned;
    out_trace->combine_time_us = qe_agg_t_combined - qe_agg_t_reduced;
    out_trace->assign_passes = assign_passes;
  }
  
  scratch_end(scratch);
  ProfEnd();
  return result;
}

//~ tec: HAVING
internal B32
qe_str8_contains(String8 haystack, String8 needle)
{
  if (needle.size == 0) return 1;
  if (needle.size > haystack.size) return 0;
  
  for (U64 i = 0; i + needle.size <= haystack.size; i++)
  {
    if (MemoryMatch(haystack.str + i, needle.str, needle.size)) return 1;
  }
  return 0;
}

// tec: reference for fuzzy search
internal F64
qe_str8_trigram_similarity(String8 haystack, String8 needle)
{
  U64 n = settings_u64(str8_lit("QE_TRIGRAM_N"), 3);
  if (n == 0) n = 3;
  
  if (haystack.size < n || needle.size < n)
  {
    return (haystack.size == needle.size && MemoryMatch(haystack.str, needle.str, haystack.size)) ? 1.0 : 0.0;
  }
  
  U64 haystack_grams = haystack.size - n + 1;
  U64 needle_grams = needle.size - n + 1;
  
  Temp scratch = scratch_begin(0, 0);
  B32* needle_used = push_array(scratch.arena, B32, needle_grams);
  
  U64 shared = 0;
  for (U64 i = 0; i < haystack_grams; i++)
  {
    for (U64 j = 0; j < needle_grams; j++)
    {
      if (!needle_used[j] && MemoryMatch(haystack.str + i, needle.str + j, n))
      {
        needle_used[j] = 1;
        shared++;
        break;
      }
    }
  }
  
  U64 union_size = haystack_grams + needle_grams - shared;
  F64 result = (union_size == 0) ? 0.0 : (F64)shared / (F64)union_size;
  
  scratch_end(scratch);
  return result;
}

internal U64
qe_str8_edit_distance(String8 haystack, String8 needle)
{
  Temp scratch = scratch_begin(0, 0);
  U64 row_len = needle.size + 1;
  U64* dp = push_array(scratch.arena, U64, row_len);
  for (U64 j = 0; j < row_len; j++) dp[j] = j;
  
  for (U64 i = 1; i <= haystack.size; i++)
  {
    U64 prev_diag = dp[0];
    dp[0] = i;
    for (U64 j = 1; j <= needle.size; j++)
    {
      U64 tmp = dp[j];
      U64 cost = (haystack.str[i - 1] == needle.str[j - 1]) ? 0 : 1;
      U64 del = dp[j] + 1;
      U64 ins = dp[j - 1] + 1;
      U64 sub = prev_diag + cost;
      dp[j] = Min(Min(del, ins), sub);
      prev_diag = tmp;
    }
  }
  
  U64 result = dp[needle.size];
  scratch_end(scratch);
  return result;
}

internal B32
qe_ir_is_fuzzy_call(IR_Node* node, B32* out_is_distance)
{
  if (!node || node->type != IR_NodeType_AggregateCall) return 0;
  
  if (str8_match(node->value, str8_lit("similarity"), StringMatchFlag_CaseInsensitive))
  {
    if (out_is_distance) *out_is_distance = 0;
    return 1;
  }
  if (str8_match(node->value, str8_lit("edit_distance"), StringMatchFlag_CaseInsensitive) ||
      str8_match(node->value, str8_lit("levenshtein"), StringMatchFlag_CaseInsensitive))
  {
    if (out_is_distance) *out_is_distance = 1;
    return 1;
  }
  return 0;
}

internal F64
qe_having_load_value(PLAN_Materialized* m, IR_Node* node, U64 row, B32* out_is_string, String8* out_string)
{
  if (node->type == IR_NodeType_Column || node->type == IR_NodeType_AggregateCall)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 name = qe_column_list_item_display_name(scratch.arena, node);
    
    PLAN_AggColumn* col = NULL;
    for (U64 c = 0; c < m->column_count; c++)
    {
      if (str8_match(m->columns[c].name, name, 0)) { col = &m->columns[c]; break; }
    }
    
    F64 result = 0.0;
    if (!col)
    {
      log_error("HAVING: column '%.*s' not found in aggregate result", str8_varg(name));
    }
    else if (col->type == GDB_ColumnType_String8)
    {
      *out_is_string = 1;
      *out_string = col->string_values[row];
    }
    else
    {
      *out_is_string = 0;
      result = col->numeric_values[row];
    }
    
    scratch_end(scratch);
    return result;
  }
  else if (node->type == IR_NodeType_Literal)
  {
    *out_is_string = 1;
    *out_string = node->value;
    return 0.0;
  }
  
  *out_is_string = 0;
  return f64_from_str8(node->value);
}

internal B32
qe_having_eval(PLAN_Materialized* m, IR_Node* condition, U64 row)
{
  if (!condition) return 1;
  
  if (condition->type != IR_NodeType_Operator)
  {
    B32 is_str = 0;
    String8 s = {0};
    F64 v = qe_having_load_value(m, condition, row, &is_str, &s);
    return is_str ? (s.size > 0) : (v != 0.0);
  }
  
  String8 op = condition->value;
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;
  
  if (str8_match(op, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    return qe_having_eval(m, left, row) && qe_having_eval(m, right, row);
  }
  if (str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    return qe_having_eval(m, left, row) || qe_having_eval(m, right, row);
  }
  
  if (!left || !right)
  {
    log_error("qe_having_eval: malformed comparison, missing operand(s)");
    return 1;
  }
  
  // tec: HAVING only sees the small post-aggregate result, so walking the list is fine
  if (str8_match(op, str8_lit("in"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive))
  {
    if (right->type != IR_NodeType_InList)
    {
      log_error("qe_having_eval: 'in' needs a value list on its right side");
      return 0;
    }
    
    B32 lstr_in = 0;
    String8 ls_in = {0};
    F64 lv_in = qe_having_load_value(m, left, row, &lstr_in, &ls_in);
    
    B32 found = 0;
    for (IR_Node* item = right->first; item != NULL && !found; item = item->next)
    {
      B32 item_is_string = (item->type == IR_NodeType_Literal);
      if (item_is_string != lstr_in) 
      {
        continue;
      }
      found = item_is_string ? (qe_str8_compare(ls_in, item->value) == 0) : (lv_in == f64_from_str8(item->value));
    }
    return str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive) ? !found : found;
  }
  
  B32 lstr = 0, rstr = 0;
  String8 ls = {0}, rs = {0};
  F64 lv = qe_having_load_value(m, left, row, &lstr, &ls);
  
  F64 rv = 0.0;
  B32 right_resolved = 0;
  if (left->type == IR_NodeType_Column && (right->type == IR_NodeType_Literal || right->type == IR_NodeType_Numeric))
  {
    Temp scratch = scratch_begin(0, 0);
    String8 name = qe_column_list_item_display_name(scratch.arena, left);
    for (U64 c = 0; c < m->column_count; c++)
    {
      if (!str8_match(m->columns[c].name, name, 0)) 
      {
        continue;
      }
      
      if (right->type == IR_NodeType_Literal &&
          (m->columns[c].type == GDB_ColumnType_Date || m->columns[c].type == GDB_ColumnType_Timestamp))
      {
        right_resolved = 1;
        if (!qe_resolve_date_literal_value(m->columns[c].type, right, &rv)) 
        { 
          scratch_end(scratch); 
          return 0; 
        }
      }
      else if (right->type == IR_NodeType_Literal && m->columns[c].type == GDB_ColumnType_Enum)
      {
        right_resolved = 1;
        U32 code = 0;
        if (!m->columns[c].enum_type || !gdb_enum_type_code_from_label(m->columns[c].enum_type, right->value, &code)) 
        { 
          scratch_end(scratch);
          return 0; 
        }
        rv = (F64)code;
      }
      else if (right->type == IR_NodeType_Numeric && m->columns[c].type == GDB_ColumnType_Decimal)
      {
        right_resolved = 1;
        S64 raw = 0;
        if (!decimal_from_str8(right->value, m->columns[c].decimal_scale, &raw)) 
        { 
          scratch_end(scratch); 
          return 0; 
        }
        rv = (F64)raw;
      }
      break;
    }
    scratch_end(scratch);
  }
  if (!right_resolved)
  {
    rv = qe_having_load_value(m, right, row, &rstr, &rs);
  }
  
  if (lstr || rstr)
  {
    if (str8_match(op, str8_lit("contains"), StringMatchFlag_CaseInsensitive))
    {
      return qe_str8_contains(ls, rs);
    }
    
    S32 cmp = qe_str8_compare(ls, rs);
    if (str8_match(op, str8_lit("!="), 0)) return cmp != 0;
    if (str8_match(op, str8_lit("<"), 0)) return cmp < 0;
    if (str8_match(op, str8_lit(">"), 0)) return cmp > 0;
    if (str8_match(op, str8_lit("<="), 0)) return cmp <= 0;
    if (str8_match(op, str8_lit(">="), 0)) return cmp >= 0;
    return cmp == 0; // tec: default '=' / '=='
  }
  
  if (str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0)) return lv == rv;
  if (str8_match(op, str8_lit("!="), 0)) return lv != rv;
  if (str8_match(op, str8_lit("<="), 0)) return lv <= rv;
  if (str8_match(op, str8_lit(">="), 0)) return lv >= rv;
  if (str8_match(op, str8_lit("<"), 0)) return lv < rv;
  if (str8_match(op, str8_lit(">"), 0)) return lv > rv;
  
  log_error("qe_having_eval: unsupported operator '%.*s'", str8_varg(op));
  return 1;
}

// tec: null when any part of the condition is not a numeric comparison against a literal, the caller then evaluates it row by row
internal QE_HavingNode*
qe_having_compile(Arena* arena, PLAN_Materialized* m, IR_Node* condition)
{
  if (!condition || condition->type != IR_NodeType_Operator)
  {
    return 0;
  }
  
  String8 op = condition->value;
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : 0;
  if (!left || !right)
  {
    return 0;
  }
  
  B32 is_and = str8_match(op, str8_lit("and"), StringMatchFlag_CaseInsensitive);
  B32 is_or = str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive);
  if (is_and || is_or)
  {
    QE_HavingNode* left_node = qe_having_compile(arena, m, left);
    QE_HavingNode* right_node = left_node ? qe_having_compile(arena, m, right) : 0;
    if (!left_node || !right_node)
    {
      return 0;
    }
    QE_HavingNode* node = push_array(arena, QE_HavingNode, 1);
    node->kind = is_and ? QE_HavingNodeKind_And : QE_HavingNodeKind_Or;
    node->left = left_node;
    node->right = right_node;
    return node;
  }
  
  if ((left->type != IR_NodeType_Column && left->type != IR_NodeType_AggregateCall) || right->type != IR_NodeType_Numeric)
  {
    return 0;
  }
  
  QE_HavingCompare compare = QE_HavingCompare_Equal;
  if (str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0)) compare = QE_HavingCompare_Equal;
  else if (str8_match(op, str8_lit("!="), 0)) compare = QE_HavingCompare_NotEqual;
  else if (str8_match(op, str8_lit("<"), 0)) compare = QE_HavingCompare_Less;
  else if (str8_match(op, str8_lit("<="), 0)) compare = QE_HavingCompare_LessEqual;
  else if (str8_match(op, str8_lit(">"), 0)) compare = QE_HavingCompare_Greater;
  else if (str8_match(op, str8_lit(">="), 0)) compare = QE_HavingCompare_GreaterEqual;
  else return 0;
  
  Temp scratch = scratch_begin(&arena, 1);
  String8 name = qe_column_list_item_display_name(scratch.arena, left);
  PLAN_AggColumn* column = 0;
  for (U64 c = 0; c < m->column_count; c += 1)
  {
    if (str8_match(m->columns[c].name, name, 0))
    {
      column = &m->columns[c];
      break;
    }
  }
  scratch_end(scratch);
  if (!column || column->type == GDB_ColumnType_String8)
  {
    return 0;
  }
  
  F64 literal = 0.0;
  if (column->type == GDB_ColumnType_Decimal)
  {
    S64 raw = 0;
    if (!decimal_from_str8(right->value, column->decimal_scale, &raw))
    {
      return 0;
    }
    literal = (F64)raw;
  }
  else
  {
    literal = f64_from_str8(right->value);
  }
  
  QE_HavingNode* node = push_array(arena, QE_HavingNode, 1);
  node->kind = QE_HavingNodeKind_Compare;
  node->compare = compare;
  node->values = column->numeric_values;
  node->literal = literal;
  return node;
}

internal B32
qe_having_node_eval(QE_HavingNode* node, U64 row)
{
  switch (node->kind)
  {
    case QE_HavingNodeKind_And: return qe_having_node_eval(node->left, row) && qe_having_node_eval(node->right, row);
    case QE_HavingNodeKind_Or: return qe_having_node_eval(node->left, row) || qe_having_node_eval(node->right, row);
    default: break;
  }
  
  F64 value = node->values[row];
  switch (node->compare)
  {
    case QE_HavingCompare_Equal: return value == node->literal;
    case QE_HavingCompare_NotEqual: return value != node->literal;
    case QE_HavingCompare_Less: return value < node->literal;
    case QE_HavingCompare_LessEqual: return value <= node->literal;
    case QE_HavingCompare_Greater: return value > node->literal;
    case QE_HavingCompare_GreaterEqual: return value >= node->literal;
  }
  return 1;
}

internal PLAN_Materialized
qe_apply_having(Arena* arena, PLAN_Materialized* m, IR_Node* having_ir)
{
  IR_Node* condition = having_ir ? having_ir->first : NULL; // tec: mirrors qe_compile_condition's where_clause->first convention
  if (!condition) return *m;
  
  Temp scratch = scratch_begin(&arena, 1);
  U64* keep = push_array(scratch.arena, U64, Max(m->count, 1));
  U64 keep_count = 0;
  
  // tec: the general evaluation looks every operand up by name for every group, which adds up over a million groups
  QE_HavingNode* compiled = qe_having_compile(scratch.arena, m, condition);
  for (U64 i = 0; i < m->count; i++)
  {
    B32 kept = compiled ? qe_having_node_eval(compiled, i) : qe_having_eval(m, condition, i);
    if (kept) keep[keep_count++] = i;
  }
  
  PLAN_Materialized result = {0};
  result.count = keep_count;
  result.column_count = m->column_count;
  result.columns = push_array(arena, PLAN_AggColumn, Max(m->column_count, 1));
  
  for (U64 c = 0; c < m->column_count; c++)
  {
    PLAN_AggColumn* src = &m->columns[c];
    PLAN_AggColumn* dst = &result.columns[c];
    dst->name = src->name;
    dst->type = src->type;
    dst->decimal_scale = src->decimal_scale;
    dst->enum_type = src->enum_type;
    
    if (src->type == GDB_ColumnType_String8)
    {
      dst->string_values = push_array(arena, String8, Max(keep_count, 1));
      for (U64 i = 0; i < keep_count; i++) dst->string_values[i] = src->string_values[keep[i]];
    }
    else
    {
      dst->numeric_values = push_array(arena, F64, Max(keep_count, 1));
      for (U64 i = 0; i < keep_count; i++) dst->numeric_values[i] = src->numeric_values[keep[i]];
    }
    
    if (src->is_null)
    {
      dst->is_null = push_array(arena, U8, Max(keep_count, 1));
      for (U64 i = 0; i < keep_count; i++) dst->is_null[i] = src->is_null[keep[i]];
    }
  }
  
  scratch_end(scratch);
  return result;
}

//~ tec: hash join

internal String8
qe_bare_column_name(String8 name)
{
  for (U64 i = 0; i < name.size; i++)
  {
    if (name.str[i] == '.') return str8_skip(name, i + 1);
  }
  return name;
}

//~ tec: index scan

internal S32
qe_index_row_cmp_target(Arena* arena, GDB_Column* column, B32 is_string, U64 row, F64 target_numeric, String8 target_string)
{
  if (is_string)
  {
    String8 sv = gdb_column_get_string(arena, column, row);
    return qe_str8_compare(sv, target_string);
  }
  F64 v = qe_read_numeric_as_f64(column, row);
  return (v < target_numeric) ? -1 : (v > target_numeric) ? 1 : 0;
}

// tec: first index i in order[lo..count) such that order[i]'s key >= target (standard lower_bound)
internal U64
qe_index_lower_bound(Arena* arena, GDB_Column* column, B32 is_string, U64* order, U64 count, F64 target_numeric, String8 target_string)
{
  U64 lo = 0, hi = count;
  while (lo < hi)
  {
    U64 mid = lo + (hi - lo) / 2;
    if (qe_index_row_cmp_target(arena, column, is_string, order[mid], target_numeric, target_string) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// tec: first index i such that order[i]'s key > target (standard upper_bound)
internal U64
qe_index_upper_bound(Arena* arena, GDB_Column* column, B32 is_string, U64* order, U64 count, F64 target_numeric, String8 target_string)
{
  U64 lo = 0, hi = count;
  while (lo < hi)
  {
    U64 mid = lo + (hi - lo) / 2;
    if (qe_index_row_cmp_target(arena, column, is_string, order[mid], target_numeric, target_string) <= 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

internal B32
qe_resolve_leaf_comparison(GDB_Table* table, IR_Node* condition,
                           GDB_Column** out_column,
                           B32* out_is_eq, B32* out_is_lt, B32* out_is_le, B32* out_is_gt, B32* out_is_ge,
                           B32* out_is_string, F64* out_target_numeric, String8* out_target_string)
{
  if (!condition || condition->type != IR_NodeType_Operator)
  {
    return 0;
  }
  
  String8 op = condition->value;
  B32 is_eq = str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0);
  B32 is_lt = str8_match(op, str8_lit("<"), 0);
  B32 is_le = str8_match(op, str8_lit("<="), 0);
  B32 is_gt = str8_match(op, str8_lit(">"), 0);
  B32 is_ge = str8_match(op, str8_lit(">="), 0);
  if (!(is_eq || is_lt || is_le || is_gt || is_ge)) return 0;
  
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;
  if (!left || !right || left->type != IR_NodeType_Column)
  {
    return 0;
  }
  if (right->type != IR_NodeType_Numeric && right->type != IR_NodeType_Literal)
  {
    return 0;
  }
  
  GDB_Column* column = gdb_table_find_column(table, qe_bare_column_name(left->value));
  if (!column) return 0;
  
  B32 is_string_key = (column->type == GDB_ColumnType_String8);
  B32 is_date_key = (column->type == GDB_ColumnType_Date || column->type == GDB_ColumnType_Timestamp);
  B32 is_enum_key = (column->type == GDB_ColumnType_Enum);
  B32 literal_given = (right->type == IR_NodeType_Literal);
  
  if (is_date_key || is_enum_key)
  {
    if (!literal_given)
    {
      // tec: date/timestamp/enum comparisons always use a quoted literal
      return 0;
    }
  }
  else if (is_string_key != literal_given)
  {
    // tec: type mismatch, dont guess
    return 0;
  }
  
  F64 target_numeric = 0.0;
  if (is_date_key)
  {
    // unparsable literal, cant use
    if (!qe_resolve_date_literal_value(column->type, right, &target_numeric))
    {
      return 0;
    }
  }
  else if (is_enum_key)
  {
    // unparsable literal, cant use
    if (!qe_resolve_enum_literal_value(column, right, &target_numeric))
    {
      return 0;
    }
  }
  else if (column->type == GDB_ColumnType_Decimal)
  {
    // unparsable literal, cant use
    if (!qe_resolve_decimal_literal_value(column, right, &target_numeric))
    {
      return 0;
    }
  }
  else if (!is_string_key)
  {
    target_numeric = f64_from_str8(right->value);
  }
  
  *out_column = column;
  *out_is_eq = is_eq;
  *out_is_lt = is_lt;
  *out_is_le = is_le;
  *out_is_gt = is_gt;
  *out_is_ge = is_ge;
  *out_is_string = is_string_key;
  *out_target_numeric = target_numeric;
  *out_target_string = is_string_key ? right->value : (String8){0};
  return 1;
}

// tec: finds the slice of the index order that satisfies one comparison, without touching any rows
internal B32
qe_index_leaf_range(Arena* arena, GDB_Table* table, IR_Node* condition, GDB_Index** out_index, U64* out_range_lo, U64* out_range_hi)
{
  GDB_Column* column = 0;
  B32 is_eq = 0, is_lt = 0, is_le = 0, is_gt = 0, is_ge = 0, is_string_key = 0;
  F64 target_numeric = 0.0;
  String8 target_string = {0};
  if (!qe_resolve_leaf_comparison(table, condition, &column, &is_eq, &is_lt, &is_le, &is_gt, &is_ge,
                                  &is_string_key, &target_numeric, &target_string))
  {
    return 0;
  }
  
  // tec: the sorted (key,row) array has no room for NULLs to sort correctly against a real value,
  // so fall back rather than risk treating a NULL as its placeholder value
  if (column->null_flags) return 0;
  
  GDB_Index* index = gdb_table_find_index_on_column(table, column);
  if (!index) return 0;
  
  U64 row_count = table->row_count;
  *out_index = index;
  if (row_count == 0)
  {
    *out_range_lo = 0;
    *out_range_hi = 0;
    return 1;
  }
  
  // tec: recreate index if needed
  if (index->order_count != row_count)
  {
    gdb_index_build_order(index);
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  U64 range_lo = 0, range_hi = 0;
  if (is_eq)
  {
    range_lo = qe_index_lower_bound(scratch.arena, column, is_string_key, index->order, row_count, target_numeric, target_string);
    range_hi = qe_index_upper_bound(scratch.arena, column, is_string_key, index->order, row_count, target_numeric, target_string);
  }
  else if (is_lt)
  {
    range_lo = 0;
    range_hi = qe_index_lower_bound(scratch.arena, column, is_string_key, index->order, row_count, target_numeric, target_string);
  }
  else if (is_le)
  {
    range_lo = 0;
    range_hi = qe_index_upper_bound(scratch.arena, column, is_string_key, index->order, row_count, target_numeric, target_string);
  }
  else if (is_gt)
  {
    range_lo = qe_index_upper_bound(scratch.arena, column, is_string_key, index->order, row_count, target_numeric, target_string);
    range_hi = row_count;
  }
  else // tec: is_ge
  {
    range_lo = qe_index_lower_bound(scratch.arena, column, is_string_key, index->order, row_count, target_numeric, target_string);
    range_hi = row_count;
  }
  
  scratch_end(scratch);
  *out_range_lo = range_lo;
  *out_range_hi = range_hi;
  return 1;
}

// tec: how many rows an index would return for this comparison, exact and without reading any of them
internal B32
qe_index_leaf_match_count(Arena* arena, GDB_Table* table, IR_Node* condition, U64* out_match_count)
{
  GDB_Index* index = 0;
  U64 range_lo = 0;
  U64 range_hi = 0;
  if (!qe_index_leaf_range(arena, table, condition, &index, &range_lo, &range_hi))
  {
    return 0;
  }
  *out_match_count = range_hi - range_lo;
  return 1;
}

internal B32
qe_index_range_for_leaf(Arena* arena, GDB_Table* table, IR_Node* condition, QE_ScanResult* out_result)
{
  GDB_Index* index = 0;
  U64 range_lo = 0;
  U64 range_hi = 0;
  if (!qe_index_leaf_range(arena, table, condition, &index, &range_lo, &range_hi))
  {
    return 0;
  }
  
  U64 match_count = range_hi - range_lo;
  out_result->indices = push_array(arena, U64, Max(match_count, 1));
  out_result->count = match_count;
  for (U64 i = 0; i < match_count; i++)
  {
    out_result->indices[i] = index->order[range_lo + i];
  }
  return 1;
}

internal U32
qe_collect_and_leaves(IR_Node* condition, IR_Node** out_leaves, U32 count, U32 max_leaves)
{
  if (!condition || count >= max_leaves) return count;
  
  if (condition->type == IR_NodeType_Operator &&
      str8_match(condition->value, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    count = qe_collect_and_leaves(condition->first, out_leaves, count, max_leaves);
    count = qe_collect_and_leaves(condition->first ? condition->first->next : NULL, out_leaves, count, max_leaves);
    return count;
  }
  
  out_leaves[count++] = condition;
  return count;
}

// tec: narrows by one indexable leaf, then applies the whole condition to the rows that are left
internal B32
qe_index_narrow_and_filter(Arena* arena, GDB_Table* table, IR_Node* root, IR_Node* leaf, QE_ScanResult* out_result)
{
  QE_ScanResult narrowed = {0};
  if (!qe_index_range_for_leaf(arena, table, leaf, &narrowed))
  {
    return 0;
  }
  
  String8 empty_alias = {0};
  U64* row_indices_for_table = narrowed.indices;
  PLAN_RowSet single_table_rows = {0};
  single_table_rows.tables = &table;
  single_table_rows.aliases = &empty_alias;
  single_table_rows.table_count = 1;
  single_table_rows.row_indices = &row_indices_for_table;
  single_table_rows.count = narrowed.count;
  
  PLAN_RowSet filtered = qe_filter_joined_rows(arena, &single_table_rows, root);
  out_result->indices = filtered.row_indices[0];
  out_result->count = filtered.count;
  return 1;
}

// tec: the optimizer picked this leaf, so it is used whether or not it is the first indexable one
internal B32
qe_index_scan_with_leaf(Arena* arena, GDB_Table* table, IR_Node* where_clause, IR_Node* leaf, QE_ScanResult* out_result)
{
  if (!where_clause || !where_clause->first || !leaf)
  {
    return 0;
  }
  IR_Node* root = where_clause->first;
  if (leaf == root)
  {
    return qe_index_range_for_leaf(arena, table, root, out_result);
  }
  return qe_index_narrow_and_filter(arena, table, root, leaf, out_result);
}

// tec: walks the index in key order in growing chunks and stops once enough rows pass the condition, condition is the root expression or NULL
internal void
qe_index_ordered_scan(Arena* arena, GDB_Table* table, String8 alias, GDB_Index* index, IR_Node* condition, B32 descending, U64 wanted, QE_ScanResult* out_result)
{
  U64 row_count = table->row_count;
  if (index->order_count != row_count)
  {
    gdb_index_build_order(index);
  }
  
  out_result->indices = push_array(arena, U64, Max(wanted, (U64)1));
  out_result->count = 0;
  
  U64 position = 0;
  U64 chunk_size = Max(wanted * 2, (U64)1024);
  while (out_result->count < wanted && position < row_count)
  {
    Temp scratch = scratch_begin(&arena, 1);
    
    U64 take = Min(chunk_size, row_count - position);
    U64* chunk_rows = push_array(scratch.arena, U64, take);
    for (U64 offset = 0; offset < take; offset += 1)
    {
      U64 order_position = position + offset;
      if (descending)
      {
        order_position = row_count - 1 - order_position;
      }
      chunk_rows[offset] = index->order[order_position];
    }
    
    U64* kept_rows = chunk_rows;
    U64 kept_count = take;
    if (condition)
    {
      PLAN_RowSet chunk = {0};
      chunk.tables = &table;
      chunk.aliases = &alias;
      chunk.table_count = 1;
      chunk.row_indices = &chunk_rows;
      chunk.count = take;
      PLAN_RowSet filtered = qe_filter_joined_rows(scratch.arena, &chunk, condition);
      kept_rows = filtered.row_indices[0];
      kept_count = filtered.count;
    }
    
    U64 room = wanted - out_result->count;
    U64 copy_count = Min(kept_count, room);
    MemoryCopy(out_result->indices + out_result->count, kept_rows, copy_count * sizeof(U64));
    out_result->count += copy_count;
    
    scratch_end(scratch);
    position += take;
    chunk_size *= 2;
  }
}

internal B32
qe_try_index_scan(Arena* arena, GDB_Table* table, IR_Node* where_clause, QE_ScanResult* out_result)
{
  if (!where_clause || !where_clause->first) return 0;
  IR_Node* root = where_clause->first;
  
  if (qe_index_range_for_leaf(arena, table, root, out_result))
  {
    return 1;
  }
  
  if (root->type != IR_NodeType_Operator || !str8_match(root->value, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    return 0;
  }
  
  U64 max_and_leaves = settings_u64(str8_lit("QE_INDEX_SCAN_MAX_AND_LEAVES"), 16);
  IR_Node** leaves = push_array(arena, IR_Node*, max_and_leaves);
  U32 leaf_count = qe_collect_and_leaves(root, leaves, 0, (U32)max_and_leaves);
  
  for (U32 i = 0; i < leaf_count; i++)
  {
    if (qe_index_narrow_and_filter(arena, table, root, leaves[i], out_result))
    {
      return 1;
    }
  }
  
  return 0;
}

// tec: does column's [min,max] over a chunk provably rule out every row satisfying `leaf`?
internal B32
qe_zonemap_chunk_is_prunable(QE_ZonemapLeaf* leaf, GDB_ZoneMapChunk* chunk)
{
  if (!chunk->has_values) return 0; // tec: an all NULL (so far) chunk can never be proven empty
  
  if (leaf->is_eq) return leaf->target < chunk->min || leaf->target > chunk->max;
  if (leaf->is_lt) return chunk->min >= leaf->target;
  if (leaf->is_le) return chunk->min > leaf->target;
  if (leaf->is_gt) return chunk->max <= leaf->target;
  if (leaf->is_ge) return chunk->max < leaf->target;
  return 0;
}

internal Rng1U64*
qe_scan_build_dispatch_ranges(Arena* arena, GDB_Table* table, IR_Node* where_clause,
                              U64 rows_per_chunk, U64* out_range_count, U64* out_pruned_rows,
                              String8* out_pruned_column_name)
{
  *out_pruned_rows = 0;
  *out_pruned_column_name = (String8){0};
  
  U64 uniform_range_count = (table->row_count == 0) ? 0 : (table->row_count + rows_per_chunk - 1) / rows_per_chunk;
  
  if (g_gdb_state->zonemap_chunk_rows > rows_per_chunk)
  {
    Rng1U64* ranges = push_array(arena, Rng1U64, Max(uniform_range_count, 1));
    for (U64 i = 0; i < uniform_range_count; i++)
    {
      U64 start = i * rows_per_chunk;
      ranges[i] = r1u64(start, Min(start + rows_per_chunk, table->row_count));
    }
    *out_range_count = uniform_range_count;
    return ranges;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: collect top level AND leaves (or just the root)
  IR_Node* root = (where_clause && where_clause->first) ? where_clause->first : NULL;
  U64 max_leaves = settings_u64(str8_lit("QE_INDEX_SCAN_MAX_AND_LEAVES"), 16);
  IR_Node** condition_leaves = push_array(scratch.arena, IR_Node*, Max(max_leaves, 1));
  U32 leaf_count = 0;
  if (root)
  {
    if (root->type == IR_NodeType_Operator && str8_match(root->value, str8_lit("and"), StringMatchFlag_CaseInsensitive))
    {
      leaf_count = qe_collect_and_leaves(root, condition_leaves, 0, (U32)max_leaves);
    }
    else
    {
      condition_leaves[0] = root;
      leaf_count = 1;
    }
  }
  
  //- tec: resolve each leaf into a zone map usable comparison
  QE_ZonemapLeaf* zonemap_leaves = push_array(scratch.arena, QE_ZonemapLeaf, Max(leaf_count, 1));
  U32 zonemap_leaf_count = 0;
  U64 zone_map_chunk_count = 0;
  
  for (U32 i = 0; i < leaf_count; i++)
  {
    GDB_Column* column = 0;
    B32 is_eq = 0, is_lt = 0, is_le = 0, is_gt = 0, is_ge = 0, is_string = 0;
    F64 target_numeric = 0.0;
    String8 target_string = {0};
    if (!qe_resolve_leaf_comparison(table, condition_leaves[i], &column, &is_eq, &is_lt, &is_le, &is_gt, &is_ge,
                                    &is_string, &target_numeric, &target_string))
    {
      continue;
    }
    if (is_string || !gdb_column_type_is_zone_map_eligible(column->type)) 
    {
      continue;
    }
    // tec: != can't be safely pruned via a min/max range
    if (!(is_eq || is_lt || is_le || is_gt || is_ge)) 
    {
      continue;
    }
    
    gdb_column_ensure_zone_map(column);
    if (!column->has_zone_map) 
    {
      continue;
    }
    
    QE_ZonemapLeaf* out = &zonemap_leaves[zonemap_leaf_count++];
    out->column = column;
    out->is_eq = is_eq;
    out->is_lt = is_lt;
    out->is_le = is_le; 
    out->is_gt = is_gt;
    out->is_ge = is_ge;
    out->target = target_numeric;
    
    // tec: identical across all columns of this table
    zone_map_chunk_count = column->zone_map_chunk_count; 
  }
  
  if (zonemap_leaf_count == 0 || zone_map_chunk_count == 0)
  {
    // tec: nothing to prune
    Rng1U64* ranges = push_array(arena, Rng1U64, Max(uniform_range_count, 1));
    for (U64 i = 0; i < uniform_range_count; i++)
    {
      U64 start = i * rows_per_chunk;
      ranges[i] = r1u64(start, Min(start + rows_per_chunk, table->row_count));
    }
    *out_range_count = uniform_range_count;
    scratch_end(scratch);
    return ranges;
  }
  
  //- tec: mark each zone map chunk prunable if any kept leaf proves it empty
  // tec: if multiple columns are independently eligible, this reports whichever leaf's column happened to prune the first chunk its checked against
  B32* chunk_prunable = push_array(scratch.arena, B32, zone_map_chunk_count);
  String8 first_pruned_column_name = {0};
  for (U64 z = 0; z < zone_map_chunk_count; z++)
  {
    for (U32 i = 0; i < zonemap_leaf_count; i++)
    {
      GDB_ZoneMapChunk* chunk = &zonemap_leaves[i].column->zone_map[z];
      if (qe_zonemap_chunk_is_prunable(&zonemap_leaves[i], chunk))
      {
        chunk_prunable[z] = 1;
        if (first_pruned_column_name.size == 0) first_pruned_column_name = zonemap_leaves[i].column->name;
        break;
      }
    }
  }
  
  //- tec: combine consecutive unprunable chunks into rows_per_chunk-capped dispatch ranges
  U64 zonemap_chunk_rows = g_gdb_state->zonemap_chunk_rows;
  U64 range_cap = rows_per_chunk;
  
  Rng1U64* ranges = push_array(arena, Rng1U64, zone_map_chunk_count + 1);
  U64 range_count = 0;
  U64 pruned_rows = 0;
  
  U64 z = 0;
  while (z < zone_map_chunk_count)
  {
    U64 chunk_row_start = z * zonemap_chunk_rows;
    if (chunk_prunable[z])
    {
      U64 chunk_row_end = Min(chunk_row_start + zonemap_chunk_rows, table->row_count);
      pruned_rows += chunk_row_end - chunk_row_start;
      z++;
      continue;
    }
    
    U64 run_start = chunk_row_start;
    U64 run_end = Min(run_start + zonemap_chunk_rows, table->row_count);
    z++;
    while (z < zone_map_chunk_count && !chunk_prunable[z])
    {
      // tec: check the PROSPECTIVE size before committing to grow, so a run can never overshoot range_cap
      U64 prospective_end = Min(run_end + zonemap_chunk_rows, table->row_count);
      if (prospective_end - run_start > range_cap) 
      {
        break;
      }
      run_end = prospective_end;
      z++;
    }
    
    ranges[range_count++] = r1u64(run_start, run_end);
  }
  
  *out_range_count = range_count;
  *out_pruned_rows = pruned_rows;
  *out_pruned_column_name = first_pruned_column_name;
  scratch_end(scratch);
  return ranges;
}

internal B32
qe_column_belongs_to_table(GDB_Table* table, String8 alias, String8 column_name)
{
  String8 bare = column_name;
  
  for (U64 i = 0; i < column_name.size; i++)
  {
    if (column_name.str[i] == '.')
    {
      String8 qualifier = str8_prefix(column_name, i);
      String8 name_to_match = alias.size ? alias : table->name;
      if (!str8_match(qualifier, name_to_match, StringMatchFlag_CaseInsensitive)) return 0;
      bare = str8_skip(column_name, i + 1);
      break;
    }
  }
  
  return gdb_table_find_column(table, bare) != NULL;
}

internal B32
qe_column_belongs_to_rowset(PLAN_RowSet* rows, String8 column_name)
{
  for (U64 t = 0; t < rows->table_count; t++)
  {
    String8 alias = rows->aliases ? rows->aliases[t] : (String8){0};
    if (qe_column_belongs_to_table(rows->tables[t], alias, column_name)) return 1;
  }
  return 0;
}

internal IR_Node*
qe_validate_equi_condition(PLAN_RowSet* left_rows, GDB_Table* right_table, String8 right_alias, IR_Node* condition)
{
  if (!condition || condition->type != IR_NodeType_Operator) 
  {
    return NULL;
  }
  if (!(str8_match(condition->value, str8_lit("="), 0) || 
        str8_match(condition->value, str8_lit("=="), 0))) 
  {
    return NULL;
  }
  
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;
  if (!left || !right || 
      left->type != IR_NodeType_Column || 
      right->type != IR_NodeType_Column) 
  {
    return NULL;
  }
  
  B32 left_is_right = qe_column_belongs_to_table(right_table, right_alias, left->value);
  B32 right_is_right = qe_column_belongs_to_table(right_table, right_alias, right->value);
  B32 left_is_left = qe_column_belongs_to_rowset(left_rows, left->value);
  B32 right_is_left = qe_column_belongs_to_rowset(left_rows, right->value);
  
  B32 valid = (left_is_left && right_is_right) || (left_is_right && right_is_left);
  return valid ? condition : NULL;
}

internal IR_Node*
qe_find_equi_condition(PLAN_RowSet* left_rows, GDB_Table* right_table, String8 right_alias, IR_Node* condition)
{
  if (!condition) 
  {
    return NULL;
  }
  
  if (condition->type == IR_NodeType_Operator && str8_match(condition->value, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    IR_Node* left = condition->first;
    IR_Node* right = left ? left->next : NULL;
    
    IR_Node* found = qe_find_equi_condition(left_rows, right_table, right_alias, left);
    if (found)
    {
      return found;
    }
    return qe_find_equi_condition(left_rows, right_table, right_alias, right);
  }
  
  return qe_validate_equi_condition(left_rows, right_table, right_alias, condition);
}

internal F64
qe_row_load_value(Arena* arena, PLAN_RowSet* rows, IR_Node* node, U64 output_row, B32* out_is_string, String8* out_string, B32* out_is_null)
{
  *out_is_null = 0;
  *out_is_string = 0;
  
  if (node->type == IR_NodeType_Column)
  {
    String8 bare = {0};
    U64 slot = max_U64;
    GDB_Table* table = qe_resolve_column_table(rows, node->value, &bare, &slot);
    if (!table) 
    {
      return 0.0;
    }
    
    U64 row = rows->row_indices[slot][output_row];
    if (row == PLAN_NULL_ROW) 
    {
      *out_is_null = 1; 
      return 0.0; 
    }
    
    GDB_Column* column = gdb_table_find_column(table, bare);
    if (!column) 
    {
      return 0.0;
    }
    
    if (gdb_column_is_null(column, row)) 
    { 
      *out_is_null = 1; 
      return 0.0; 
    }
    
    if (column->type == GDB_ColumnType_String8)
    {
      *out_is_string = 1;
      *out_string = gdb_column_get_string(arena, column, row);
      return 0.0;
    }
    return qe_read_numeric_as_f64(column, row);
  }
  else if (node->type == IR_NodeType_Literal)
  {
    *out_is_string = 1;
    *out_string = node->value;
    return 0.0;
  }
  
  return f64_from_str8(node->value);
}

internal F64
qe_row_eval_fuzzy_call(Arena* arena, PLAN_RowSet* rows, IR_Node* call, U64 output_row, B32 is_distance)
{
  IR_Node* col_arg = call->first;
  IR_Node* needle_arg = col_arg ? col_arg->next : 0;
  if (!col_arg || !needle_arg)
  {
    log_error("qe_row_eval_fuzzy_call: %.*s() requires (column, string literal)", str8_varg(call->value));
    return 0.0;
  }
  
  B32 is_str = 0, is_null = 0;
  String8 haystack = {0};
  qe_row_load_value(arena, rows, col_arg, output_row, &is_str, &haystack, &is_null);
  if (is_null || !is_str) return is_distance ? (F64)needle_arg->value.size : 0.0;
  
  return is_distance ? (F64)qe_str8_edit_distance(haystack, needle_arg->value)
    : qe_str8_trigram_similarity(haystack, needle_arg->value);
}

//~ tec: 'col [NOT] IN (list)' on the CPU

internal S32
qe_f64_compare_for_sort(const void* a, const void* b)
{
  F64 x = *(const F64*)a;
  F64 y = *(const F64*)b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

internal S32
qe_str8_compare_for_sort(const void* a, const void* b)
{
  return qe_str8_compare(*(const String8*)a, *(const String8*)b);
}

internal B32
qe_in_item_resolve(GDB_Column* column, IR_Node* item, B32* out_is_string, F64* out_num, String8* out_str)
{
  *out_is_string = 0;
  *out_num = 0.0;
  *out_str = (String8){0};
  
  GDB_ColumnType type = column ? column->type : GDB_ColumnType_Invalid;
  
  if (type == GDB_ColumnType_String8 || (!column && item->type == IR_NodeType_Literal))
  {
    *out_is_string = 1;
    *out_str = item->value;
    return 1;
  }
  if (type == GDB_ColumnType_Date || type == GDB_ColumnType_Timestamp)
  {
    return item->type == IR_NodeType_Literal && qe_resolve_date_literal_value(type, item, out_num);
  }
  if (type == GDB_ColumnType_Enum)
  {
    return item->type == IR_NodeType_Literal && qe_resolve_enum_literal_value(column, item, out_num);
  }
  if (type == GDB_ColumnType_Decimal)
  {
    return qe_resolve_decimal_literal_value(column, item, out_num);
  }
  
  *out_num = f64_from_str8(item->value);
  return 1;
}

internal GDB_Column*
qe_in_left_column(PLAN_RowSet* rows, IR_Node* left)
{
  if (!left || left->type != IR_NodeType_Column) 
  {
    return NULL;
  }
  String8 bare = {0};
  U64 slot = max_U64;
  GDB_Table* table = qe_resolve_column_table(rows, left->value, &bare, &slot);
  return table ? gdb_table_find_column(table, bare) : NULL;
}

internal void
qe_in_sets_register(Arena* arena, PLAN_RowSet* rows, IR_Node* condition)
{
  if (!condition || condition->type != IR_NodeType_Operator) return;
  
  String8 op = condition->value;
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;
  
  if (str8_match(op, str8_lit("and"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    qe_in_sets_register(arena, rows, left);
    qe_in_sets_register(arena, rows, right);
    return;
  }
  
  if (!(str8_match(op, str8_lit("in"), StringMatchFlag_CaseInsensitive) ||
        str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive)) ||
      !right || right->type != IR_NodeType_InList)
  {
    return;
  }
  
  U64 item_count = 0;
  for (IR_Node* item = right->first; item != NULL; item = item->next) item_count++;
  
  GDB_Column* column = qe_in_left_column(rows, left);
  QE_InSet* set = push_array(arena, QE_InSet, 1);
  set->list_node = right;
  set->nums = push_array(arena, F64, Max(item_count, 1));
  set->strs = push_array(arena, String8, Max(item_count, 1));
  
  for (IR_Node* item = right->first; item != NULL; item = item->next)
  {
    B32 is_string = 0;
    F64 num = 0.0;
    String8 str = {0};
    if (!qe_in_item_resolve(column, item, &is_string, &num, &str)) 
    {
      continue;
    }
    if (is_string) 
    {
      set->strs[set->str_count++] = str;
    }
    else
    {
      set->nums[set->num_count++] = num;
    }
  }
  
  if (set->num_count) 
  {
    quick_sort(set->nums, set->num_count, sizeof(F64), qe_f64_compare_for_sort);
  }
  if (set->str_count) 
  {
    quick_sort(set->strs, set->str_count, sizeof(String8), qe_str8_compare_for_sort);
  }
  
  set->next = g_qe_in_sets;
  g_qe_in_sets = set;
}

internal void
qe_in_sets_unregister_all(void)
{
  g_qe_in_sets = 0;
}

internal B32
qe_in_list_contains(PLAN_RowSet* rows, IR_Node* left, IR_Node* list_node, B32 lstr, F64 lv, String8 ls)
{
  for (QE_InSet* set = g_qe_in_sets; set != NULL; set = set->next)
  {
    if (set->list_node != list_node) 
    {
      continue;
    }
    
    U64 lo = 0, hi = lstr ? set->str_count : set->num_count;
    while (lo < hi)
    {
      U64 mid = lo + (hi - lo) / 2;
      S32 cmp = lstr ? qe_str8_compare(set->strs[mid], ls)
        : ((set->nums[mid] < lv) ? -1 : (set->nums[mid] > lv) ? 1 : 0);
      if (cmp == 0) return 1;
      if (cmp < 0) lo = mid + 1;
      else hi = mid;
    }
    return 0;
  }
  
  // tec: not registered, walk the list
  GDB_Column* column = rows ? qe_in_left_column(rows, left) : NULL;
  for (IR_Node* item = list_node->first; item != NULL; item = item->next)
  {
    B32 is_string = 0;
    F64 num = 0.0;
    String8 str = {0};
    if (!qe_in_item_resolve(column, item, &is_string, &num, &str)) continue;
    if (is_string != lstr) continue;
    if (is_string ? (qe_str8_compare(ls, str) == 0) : (lv == num)) return 1;
  }
  return 0;
}

internal B32
qe_row_condition_eval(Arena* arena, PLAN_RowSet* rows, IR_Node* condition, U64 output_row)
{
  if (!condition) return 1;
  
  if (condition->type != IR_NodeType_Operator)
  {
    B32 is_str = 0, is_null = 0;
    String8 s = {0};
    F64 v = qe_row_load_value(arena, rows, condition, output_row, &is_str, &s, &is_null);
    if (is_null) 
    {
      return 0;
    }
    return is_str ? (s.size > 0) : (v != 0.0);
  }
  
  String8 op = condition->value;
  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;
  
  if (str8_match(op, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    return qe_row_condition_eval(arena, rows, left, output_row) && qe_row_condition_eval(arena, rows, right, output_row);
  }
  if (str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    return qe_row_condition_eval(arena, rows, left, output_row) || qe_row_condition_eval(arena, rows, right, output_row);
  }
  
  if (str8_match(op, str8_lit("is null"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("is not null"), StringMatchFlag_CaseInsensitive))
  {
    if (!left)
    {
      log_error("qe_row_condition_eval: malformed 'is null', missing operand");
      return 1;
    }
    
    B32 lstr = 0, lnull = 0;
    String8 ls = {0};
    qe_row_load_value(arena, rows, left, output_row, &lstr, &ls, &lnull);
    
    B32 is_not = str8_match(op, str8_lit("is not null"), StringMatchFlag_CaseInsensitive);
    return is_not ? !lnull : lnull;
  }
  
  if (str8_match(op, str8_lit("in"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive))
  {
    if (!left || !right || right->type != IR_NodeType_InList)
    {
      log_error("qe_row_condition_eval: 'in' needs a value list on its right side");
      return 0;
    }
    
    B32 lstr = 0, lnull = 0;
    String8 ls = {0};
    F64 lv = qe_row_load_value(arena, rows, left, output_row, &lstr, &ls, &lnull);
    if (lnull) return 0; // tec: NULL [NOT] IN (...) is never true
    
    B32 found = qe_in_list_contains(rows, left, right, lstr, lv, ls);
    return str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive) ? !found : found;
  }
  
  // tec: fuzzy search
  if (left && qe_ir_is_fuzzy_call(left, 0))
  {
    if (!right)
    {
      log_error("qe_row_condition_eval: %.*s() used without a comparison", str8_varg(left->value));
      return 1;
    }
    
    B32 is_distance = 0;
    qe_ir_is_fuzzy_call(left, &is_distance);
    F64 score = qe_row_eval_fuzzy_call(arena, rows, left, output_row, is_distance);
    
    B32 rstr = 0, rnull = 0;
    String8 rs = {0};
    F64 rv = qe_row_load_value(arena, rows, right, output_row, &rstr, &rs, &rnull);
    if (rnull) return 0;
    
    if (str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0)) return score == rv;
    if (str8_match(op, str8_lit("!="), 0)) return score != rv;
    if (str8_match(op, str8_lit("<="), 0)) return score <= rv;
    if (str8_match(op, str8_lit(">="), 0)) return score >= rv;
    if (str8_match(op, str8_lit("<"), 0)) return score < rv;
    if (str8_match(op, str8_lit(">"), 0)) return score > rv;
    
    log_error("qe_row_condition_eval: unsupported operator '%.*s' for fuzzy predicate", str8_varg(op));
    return 1;
  }
  
  if (!left || !right)
  {
    log_error("qe_row_condition_eval: malformed comparison, missing operand(s)");
    return 1;
  }
  
  B32 lstr = 0, rstr = 0, lnull = 0, rnull = 0;
  String8 ls = {0}, rs = {0};
  F64 lv = qe_row_load_value(arena, rows, left, output_row, &lstr, &ls, &lnull);
  
  F64 rv = 0.0;
  B32 right_resolved = 0;
  if (left->type == IR_NodeType_Column && (right->type == IR_NodeType_Literal || right->type == IR_NodeType_Numeric))
  {
    String8 bare = {0};
    U64 slot = max_U64;
    GDB_Table* col_table = qe_resolve_column_table(rows, left->value, &bare, &slot);
    GDB_Column* column = col_table ? gdb_table_find_column(col_table, bare) : NULL;
    if (column && right->type == IR_NodeType_Literal &&
        (column->type == GDB_ColumnType_Date || column->type == GDB_ColumnType_Timestamp))
    {
      right_resolved = 1;
      if (!qe_resolve_date_literal_value(column->type, right, &rv)) 
      {
        // unparsable literal never matches
        return 0;
      }
    }
    else if (column && right->type == IR_NodeType_Literal && column->type == GDB_ColumnType_Enum)
    {
      right_resolved = 1;
      if (!qe_resolve_enum_literal_value(column, right, &rv))
      {
        // unparsable literal never matches
        return 0;
      }
    }
    else if (column && right->type == IR_NodeType_Numeric && column->type == GDB_ColumnType_Decimal)
    {
      right_resolved = 1;
      if (!qe_resolve_decimal_literal_value(column, right, &rv))
      {
        // unparsable literal never matches
        return 0;
      }
    }
  }
  if (!right_resolved)
  {
    rv = qe_row_load_value(arena, rows, right, output_row, &rstr, &rs, &rnull);
  }
  
  if (lnull || rnull) return 0; // tec: simplified 3-valued logic - a NULL comparison is never true
  
  if (lstr || rstr)
  {
    if (str8_match(op, str8_lit("contains"), StringMatchFlag_CaseInsensitive)) return qe_str8_contains(ls, rs);
    
    S32 cmp = qe_str8_compare(ls, rs);
    if (str8_match(op, str8_lit("!="), 0)) return cmp != 0;
    if (str8_match(op, str8_lit("<"), 0)) return cmp < 0;
    if (str8_match(op, str8_lit(">"), 0)) return cmp > 0;
    if (str8_match(op, str8_lit("<="), 0)) return cmp <= 0;
    if (str8_match(op, str8_lit(">="), 0)) return cmp >= 0;
    return cmp == 0; // tec: default '=' / '=='
  }
  
  if (str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0)) return lv == rv;
  if (str8_match(op, str8_lit("!="), 0)) return lv != rv;
  if (str8_match(op, str8_lit("<="), 0)) return lv <= rv;
  if (str8_match(op, str8_lit(">="), 0)) return lv >= rv;
  if (str8_match(op, str8_lit("<"), 0)) return lv < rv;
  if (str8_match(op, str8_lit(">"), 0)) return lv > rv;
  
  log_error("qe_row_condition_eval: unsupported operator '%.*s'", str8_varg(op));
  return 1;
}

internal THREAD_POOL_TASK_FUNC(qe_cpu_scan_filter_task)
{
  QE_CpuScanTask* task = (QE_CpuScanTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  U64* local_matched = task->task_matched_indices[task_id];
  U64 local_count = 0;
  
  for (U64 i = range.min; i < range.max; i++)
  {
    if (qe_row_condition_eval(arena, task->rows, task->condition_root, i))
    {
      local_matched[local_count++] = i;
    }
  }
  
  task->task_matched_counts[task_id] = local_count;
}

internal QE_ScanResult
qe_cpu_scan_filter(Arena* arena, GDB_Table* table, IR_Node* where_clause, QE_ScanTrace* out_trace)
{
  QE_ScanResult result = {0};
  U64 row_count = table->row_count;
  if (out_trace) out_trace->rows_before = row_count;
  
  Temp scratch = scratch_begin(&arena, 1);
  
  GDB_Table* tables_arr[1] = { table };
  U64* all_rows = push_array(scratch.arena, U64, Max(row_count, 1));
  for (U64 i = 0; i < row_count; i++) all_rows[i] = i;
  U64* row_indices_arr[1] = { all_rows };
  
  PLAN_RowSet rows = {0};
  rows.tables = tables_arr;
  rows.table_count = 1;
  rows.row_indices = row_indices_arr;
  rows.count = row_count;
  
  IR_Node* condition_root = where_clause ? where_clause->first : NULL;
  qe_in_sets_register(scratch.arena, &rows, condition_root);
  
  U64 cpu_scan_start = os_now_microseconds();
  
  TP_Context* pool = app_thread_pool();
  U64 task_count = (row_count > 1) ? Min((U64)pool->worker_count, row_count) : 1;
  
  QE_CpuScanTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, row_count, (U32)task_count);
  task.rows = &rows;
  task.condition_root = condition_root;
  task.task_matched_counts = push_array(scratch.arena, U64, task_count);
  task.task_matched_indices = push_array(scratch.arena, U64*, task_count);
  for (U64 t = 0; t < task_count; t++)
  {
    U64 width = task.ranges[t].max - task.ranges[t].min;
    task.task_matched_indices[t] = push_array(scratch.arena, U64, Max(width, 1));
  }
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_cpu_scan_filter_task, &task);
  tp_temp_end(temp);
  qe_in_sets_unregister_all();
  
  U64 matched_count = 0;
  for (U64 t = 0; t < task_count; t++) matched_count += task.task_matched_counts[t];
  
  U64* matched = push_array(arena, U64, Max(matched_count, 1));
  U64 out_i = 0;
  for (U64 t = 0; t < task_count; t++)
  {
    MemoryCopy(matched + out_i, task.task_matched_indices[t], task.task_matched_counts[t] * sizeof(U64));
    out_i += task.task_matched_counts[t];
  }
  
  U64 cpu_scan_time_us = os_now_microseconds() - cpu_scan_start;
  log_debug("qe_cpu_scan_filter: CPU scan (row_count=%llu) total time: %llu microseconds", row_count, cpu_scan_time_us);
  
  result.indices = matched;
  result.count = matched_count;
  
  if (out_trace)
  {
    out_trace->rows_after = matched_count;
    out_trace->gpu_kernel_time_us = 0; // tec: no GPU involved 
    out_trace->submit_wait_time_us = cpu_scan_time_us;
  }
  
  scratch_end(scratch);
  return result;
}

internal PLAN_RowSet
qe_filter_joined_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* condition)
{
  PLAN_RowSet result = *rows;
  if (!condition) return result;
  
  Temp scratch = scratch_begin(&arena, 1);
  
  // tec: same per row evaluation as the CPU scan, split over the pool
  qe_in_sets_register(scratch.arena, rows, condition);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, rows->count / 4096));
  QE_CpuScanTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, rows->count, (U32)task_count);
  task.rows = rows;
  task.condition_root = condition;
  task.task_matched_counts = push_array(scratch.arena, U64, task_count);
  task.task_matched_indices = push_array(scratch.arena, U64*, task_count);
  for (U64 t = 0; t < task_count; t++)
  {
    U64 width = task.ranges[t].max - task.ranges[t].min;
    task.task_matched_indices[t] = push_array_no_zero(scratch.arena, U64, Max(width, 1));
  }
  if (task_count == 1)
  {
    qe_cpu_scan_filter_task(arena, 0, 0, &task);
  }
  else
  {
    TP_Arena* pool_arena = app_thread_pool_arena();
    TP_Temp temp = tp_temp_begin(pool_arena);
    tp_for_parallel(pool, pool_arena, task_count, qe_cpu_scan_filter_task, &task);
    tp_temp_end(temp);
  }
  qe_in_sets_unregister_all();
  
  U64 keep_count = 0;
  for (U64 t = 0; t < task_count; t++) keep_count += task.task_matched_counts[t];
  U64* keep = push_array_no_zero(scratch.arena, U64, Max(keep_count, 1));
  U64 keep_cursor = 0;
  for (U64 t = 0; t < task_count; t++)
  {
    MemoryCopy(keep + keep_cursor, task.task_matched_indices[t], task.task_matched_counts[t] * sizeof(U64));
    keep_cursor += task.task_matched_counts[t];
  }
  
  result.row_indices = push_array(arena, U64*, rows->table_count);
  for (U64 t = 0; t < rows->table_count; t++)
  {
    result.row_indices[t] = push_array(arena, U64, Max(keep_count, 1));
    for (U64 i = 0; i < keep_count; i++) result.row_indices[t][i] = rows->row_indices[t][keep[i]];
  }
  result.count = keep_count;
  
  scratch_end(scratch);
  return result;
}

internal THREAD_POOL_TASK_FUNC(qe_join_pack_range_task)
{
  QE_JoinPackTask* task = (QE_JoinPackTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  F64 min_first = task->first[range.min];
  F64 max_first = min_first;
  F64 min_second = task->second[range.min];
  F64 max_second = min_second;
  for (U64 i = range.min + 1; i < range.max; i++)
  {
    min_first = Min(min_first, task->first[i]);
    max_first = Max(max_first, task->first[i]);
    min_second = Min(min_second, task->second[i]);
    max_second = Max(max_second, task->second[i]);
  }
  task->task_min_first[task_id] = min_first;
  task->task_max_first[task_id] = max_first;
  task->task_min_second[task_id] = min_second;
  task->task_max_second[task_id] = max_second;
}

internal THREAD_POOL_TASK_FUNC(qe_join_pack_compose_task)
{
  QE_JoinPackTask* task = (QE_JoinPackTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  for (U64 i = range.min; i < range.max; i++)
  {
    task->first[i] = (task->first[i] - task->min_first) * task->span_second + (task->second[i] - task->min_second);
  }
}

internal B32
qe_join_key_is_whole_number(GDB_Column* column)
{
  GDB_ColumnType type = column->type;
  return type == GDB_ColumnType_U32 || type == GDB_ColumnType_U64 || type == GDB_ColumnType_I32 || type == GDB_ColumnType_I64;
}

// tec: false when the pair does not fit in 53 bits, the arrays are untouched then
// on success first holds (first - min_first) * span_second + (second - min_second) for both sides, equal pairs stay equal and different pairs stay different
internal B32
qe_join_pack_keys(Arena* arena, F64* build_first, F64* build_second, U64 build_count, F64* probe_first, F64* probe_second, U64 probe_count)
{
  if (build_count == 0 || probe_count == 0)
  {
    return 0;
  }
  
  TP_Context* pool = app_thread_pool();
  QE_JoinPackTask tasks[2] = {0};
  U64 counts[2] = { build_count, probe_count };
  F64* firsts[2] = { build_first, probe_first };
  F64* seconds[2] = { build_second, probe_second };
  U64 task_counts[2] = {0};
  
  F64 min_first = 0.0, max_first = 0.0, min_second = 0.0, max_second = 0.0;
  for (U32 side = 0; side < 2; side++)
  {
    U64 task_count = Max((U64)1, Min((U64)pool->worker_count, counts[side] / 16384));
    task_counts[side] = task_count;
    QE_JoinPackTask* task = &tasks[side];
    task->ranges = tp_divide_work(arena, counts[side], (U32)task_count);
    task->first = firsts[side];
    task->second = seconds[side];
    task->task_min_first = push_array(arena, F64, task_count);
    task->task_max_first = push_array(arena, F64, task_count);
    task->task_min_second = push_array(arena, F64, task_count);
    task->task_max_second = push_array(arena, F64, task_count);
    qe_agg_run_tasks(task_count, qe_join_pack_range_task, task);
    for (U64 t = 0; t < task_count; t++)
    {
      B32 first_entry = (side == 0 && t == 0);
      min_first = first_entry ? task->task_min_first[t] : Min(min_first, task->task_min_first[t]);
      max_first = first_entry ? task->task_max_first[t] : Max(max_first, task->task_max_first[t]);
      min_second = first_entry ? task->task_min_second[t] : Min(min_second, task->task_min_second[t]);
      max_second = first_entry ? task->task_max_second[t] : Max(max_second, task->task_max_second[t]);
    }
  }
  
  F64 span_first = max_first - min_first + 1.0;
  F64 span_second = max_second - min_second + 1.0;
  if (span_first * span_second >= 9.0e15)
  {
    return 0;
  }
  
  for (U32 side = 0; side < 2; side++)
  {
    QE_JoinPackTask* task = &tasks[side];
    task->min_first = min_first;
    task->min_second = min_second;
    task->span_second = span_second;
    qe_agg_run_tasks(task_counts[side], qe_join_pack_compose_task, task);
  }
  return 1;
}

// tec: the default guess assumes a mostly one to one join, a hint that says the join fans out replaces it so the probe does not run twice
internal U64
qe_join_output_capacity(U64 probe_rows, U64 build_rows, QE_JoinHints* hints, U64 hard_max_capacity)
{
  U64 capacity = Max(probe_rows, build_rows) + probe_rows;
  if (hints && hints->output_rows > 0)
  {
    U64 hinted = hints->output_rows + hints->output_rows / 4 + 16;
    hinted = Min(hinted, (U64)QE_JOIN_HINT_MAX_PAIRS);
    capacity = Max(capacity, hinted);
  }
  capacity = Min(capacity, hard_max_capacity);
  capacity = Max(capacity, (U64)1);
  return capacity;
}

internal B32
qe_join_type_is_filter(String8 join_type)
{
  B32 is_semi = str8_match(join_type, str8_lit("semi"), StringMatchFlag_CaseInsensitive);
  B32 is_anti = str8_match(join_type, str8_lit("anti"), StringMatchFlag_CaseInsensitive);
  return is_semi || is_anti;
}

// tec: FNV-1a over the bytes of a string cell, a multiply and shift mix over the bits of a numeric one
internal U64
qe_key_hash(GDB_Column* column, U64 row)
{
  U64 hash = 14695981039346656037ull;
  if (column->type == GDB_ColumnType_String8)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 text = gdb_column_get_string(scratch.arena, column, row);
    for (U64 index = 0; index < text.size; index += 1)
    {
      hash ^= text.str[index];
      hash *= 1099511628211ull;
    }
    scratch_end(scratch);
    return hash;
  }
  
  F64 value = gdb_index_numeric_value(column, row);
  if (value == 0.0)
  {
    value = 0.0;
  }
  U64 bits = 0;
  MemoryCopy(&bits, &value, sizeof(bits));
  hash ^= bits;
  hash *= 0x9E3779B97F4A7C15ull;
  hash ^= hash >> 32;
  return hash;
}

internal B32
qe_key_equal(GDB_Column* column, U64 row_a, U64 row_b)
{
  if (column->type == GDB_ColumnType_String8)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 text_a = gdb_column_get_string(scratch.arena, column, row_a);
    String8 text_b = gdb_column_get_string(scratch.arena, column, row_b);
    B32 equal = str8_match(text_a, text_b, 0);
    scratch_end(scratch);
    return equal;
  }
  return gdb_index_numeric_value(column, row_a) == gdb_index_numeric_value(column, row_b);
}

// tec: a match only has to be found once, so duplicate keys on the build side are dropped and a semi join cannot fan out. rows NULL means every row
internal U64*
qe_distinct_key_rows(Arena* arena, GDB_Column* key_column, U64* rows, U64 row_count, U64* out_count)
{
  Temp scratch = scratch_begin(&arena, 1);
  
  U64 table_size = 16;
  while (table_size < row_count * 2)
  {
    table_size <<= 1;
  }
  U64* slots = push_array(scratch.arena, U64, table_size);
  for (U64 slot = 0; slot < table_size; slot += 1)
  {
    slots[slot] = max_U64;
  }
  
  U64* kept = push_array(arena, U64, Max(row_count, (U64)1));
  U64 kept_count = 0;
  for (U64 position = 0; position < row_count; position += 1)
  {
    U64 row = rows ? rows[position] : position;
    U64 slot = qe_key_hash(key_column, row) & (table_size - 1);
    B32 duplicate = 0;
    while (slots[slot] != max_U64)
    {
      if (qe_key_equal(key_column, kept[slots[slot]], row))
      {
        duplicate = 1;
        break;
      }
      slot = (slot + 1) & (table_size - 1);
    }
    if (!duplicate)
    {
      slots[slot] = kept_count;
      kept[kept_count] = row;
      kept_count += 1;
    }
  }
  
  scratch_end(scratch);
  *out_count = kept_count;
  return kept;
}

// tec: pairs are (probe row, build row) with the build row all ones for an unmatched probe row, the result is the left rows in their original order
internal PLAN_RowSet
qe_join_filter_left_rows(Arena* arena, PLAN_RowSet* left, U32* pairs, U64 pair_count, B32 keep_matched)
{
  Temp scratch = scratch_begin(&arena, 1);
  U8* keep = push_array(scratch.arena, U8, Max(left->count, (U64)1));
  for (U64 pair = 0; pair < pair_count; pair += 1)
  {
    U64 probe_index = pairs[pair * 4 + 0] | ((U64)pairs[pair * 4 + 1] << 32);
    B32 unmatched = pairs[pair * 4 + 2] == max_U32 && pairs[pair * 4 + 3] == max_U32;
    if (keep_matched && !unmatched)
    {
      keep[probe_index] = 1;
    }
    if (!keep_matched && unmatched)
    {
      keep[probe_index] = 1;
    }
  }
  
  U64 kept_count = 0;
  for (U64 row = 0; row < left->count; row += 1)
  {
    kept_count += keep[row];
  }
  
  PLAN_RowSet result = *left;
  result.count = kept_count;
  result.scores = NULL;
  result.row_indices = push_array(arena, U64*, left->table_count);
  for (U64 table_index = 0; table_index < left->table_count; table_index += 1)
  {
    result.row_indices[table_index] = push_array(arena, U64, Max(kept_count, (U64)1));
    U64 out = 0;
    for (U64 row = 0; row < left->count; row += 1)
    {
      if (keep[row])
      {
        result.row_indices[table_index][out] = left->row_indices[table_index][row];
        out += 1;
      }
    }
  }
  
  scratch_end(scratch);
  return result;
}

internal PLAN_RowSet
qe_hash_join(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias, U64* right_rows, U64 right_count, String8 join_type, IR_Node* condition, QE_JoinHints* hints, QE_JoinTrace* out_trace)
{
  return qe_hash_join_impl(arena, left, right_table, right_alias, right_rows, right_count, join_type, condition, hints, out_trace, 0, 0);
}

// tec: device_left is given when the left rows only exist on the GPU, and left is then just a stand in for their tables and count.
// with out_device the joined rows stay on the GPU too, as one dense row list per table, and the returned row set holds no row ids.
// a join that cannot run that way sets out_device->declined before doing any work
internal PLAN_RowSet
qe_hash_join_impl(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias, U64* right_rows, U64 right_count, String8 join_type, IR_Node* condition, QE_JoinHints* hints, QE_JoinTrace* out_trace, QE_DeviceRows* device_left, QE_DeviceRows* out_device)
{
  ProfBeginFunction();
  PLAN_RowSet result = {0};
  if (out_device)
  {
    MemoryZeroStruct(out_device);
  }
  
  if (gpu_device_lost())
  {
    log_error("qe_hash_join: Vulkan device is lost - refusing to attempt any GPU work");
    ProfEnd();
    return result;
  }
  
  if (!condition || condition->type != IR_NodeType_Operator || !condition->first || !condition->first->next)
  {
    log_error("qe_hash_join: malformed or missing equi-join condition");
    ProfEnd();
    return result;
  }
  
  IR_Node* cond_left = condition->first;
  IR_Node* cond_right = cond_left->next;
  
  B32 left_side_is_right_table = qe_column_belongs_to_table(right_table, right_alias, cond_left->value);
  IR_Node* right_key_node = left_side_is_right_table ? cond_left : cond_right;
  IR_Node* left_key_node = left_side_is_right_table ? cond_right : cond_left;
  
  GDB_Column* right_key_column = gdb_table_find_column(right_table, qe_bare_column_name(right_key_node->value));
  String8 left_bare = {0};
  U64 left_key_slot = max_U64;
  GDB_Table* left_key_table = qe_resolve_column_table(left, left_key_node->value, &left_bare, &left_key_slot);
  GDB_Column* left_key_column = left_key_table ? gdb_table_find_column(left_key_table, left_bare) : NULL;
  
  if (!right_key_column || !left_key_column)
  {
    log_error("qe_hash_join: could not resolve join key column(s)");
    ProfEnd();
    return result;
  }
  
  B32 is_string_key = (right_key_column->type == GDB_ColumnType_String8);
  if (is_string_key != (left_key_column->type == GDB_ColumnType_String8))
  {
    log_error("qe_hash_join: join key type mismatch (one side numeric, other string)");
    ProfEnd();
    return result;
  }
  
  // tec: dictionary codes are only comparable within the column that produced them
  B32 dict_key = 0;
  GDB_StringDict* join_dict = 0;
  if (is_string_key)
  {
    gdb_column_ensure_string_dict(right_key_column);
    if (right_key_column->has_dict)
    {
      dict_key = 1;
      join_dict = right_key_column->dict;
      is_string_key = 0;
    }
  }
  
  B32 is_filter_join = qe_join_type_is_filter(join_type);
  B32 is_anti_join = str8_match(join_type, str8_lit("anti"), StringMatchFlag_CaseInsensitive);
  
  // tec: left rows that only exist on the GPU need a key with no host row list: numeric, or dictcoded and a join that keeps the rows of both sides
  B32 device_join = device_left && out_device && !is_string_key && !is_filter_join && left->table_count + 1 <= QE_DEVICE_ROWS_MAX_TABLES;
  if (device_join && dict_key)
  {
    gdb_column_ensure_string_dict(left_key_column);
    if (!left_key_column->has_dict)
    {
      // tec: no dict on the left column means recoding would need a per row string lookup, which needs the host row list this path doesn't have
      device_join = 0;
    }
  }
  if (device_left && !device_join)
  {
    if (out_device)
    {
      out_device->declined = 1;
    }
    ProfEnd();
    return result;
  }
  
  if (is_filter_join && !right_key_column->is_unique)
  {
    U64 source_count = right_rows ? right_count : right_table->row_count;
    right_rows = qe_distinct_key_rows(arena, right_key_column, right_rows, source_count, &right_count);
  }
  
  // tec: a NULL right_rows means every row of the right table
  U64 build_row_count = right_rows ? right_count : right_table->row_count;
  U64 probe_row_count = left->count;
  
  PLAN_RowSet right_subset = {0};
  if (right_rows)
  {
    right_subset.tables = &right_table;
    right_subset.aliases = &right_alias;
    right_subset.table_count = 1;
    right_subset.row_indices = &right_rows;
    right_subset.count = right_count;
  }
  B32 is_left_join = str8_match(join_type, str8_lit("left"), StringMatchFlag_CaseInsensitive);
  
  U64 num_buckets = 1;
  while (num_buckets < build_row_count) num_buckets <<= 1;
  
  if (num_buckets * sizeof(U32) > gpu_device_max_storage_buffer_range())
  {
    log_error("qe_hash_join: build-side hash table (%llu bytes) exceeds this GPU's maxStorageBufferRange "
              "(%llu bytes) - build_row_count=%llu is too large for a single hash table on this device",
              num_buckets * sizeof(U32), gpu_device_max_storage_buffer_range(), build_row_count);
    ProfEnd();
    return result;
  }
  if (num_buckets * sizeof(U32) > settings_u64(str8_lit("GPU_MAX_BUFFER_SIZE"), GPU_MAX_BUFFER_SIZE))
  {
    log_debug("qe_hash_join: build-side hash table (%llu bytes, build_row_count=%llu) exceeds GPU_MAX_BUFFER_SIZE - "
              "allocating it as a single large buffer rather than chunking", num_buckets * sizeof(U32), build_row_count);
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: build-side key data
  void* build_data = NULL;
  U64* build_offsets = NULL;
  U64 build_data_size = 4;
  
  if (dict_key)
  {
    if (right_rows)
    {
      F64* selected_codes = push_array(scratch.arena, F64, Max(build_row_count, 1));
      for (U64 i = 0; i < build_row_count; i += 1)
      {
        selected_codes[i] = (F64)right_key_column->dict_codes[right_rows[i]];
      }
      build_data = selected_codes;
    }
    else
    {
      build_data = qe_dict_codes_to_f64_dense(scratch.arena, right_key_column->dict_codes, build_row_count);
    }
    build_data_size = Max(build_row_count, 1) * sizeof(F64);
  }
  else if (is_string_key)
  {
    GDB_StringDataChunk chunk = {0};
    if (right_rows)
    {
      chunk = qe_gather_string_column(scratch.arena, &right_subset, 0, right_key_column);
    }
    else
    {
      chunk = gdb_column_get_string_chunk(scratch.arena, right_key_column, r1u64(0, build_row_count));
    }
    build_data = chunk.data;
    build_offsets = chunk.offsets;
    build_data_size = Max(chunk.size, 4);
  }
  else if (right_rows)
  {
    build_data = qe_gather_numeric_column(scratch.arena, &right_subset, 0, right_key_column);
    build_data_size = Max(build_row_count, 1) * sizeof(F64);
  }
  else
  {
    // tec: bulk range read
    F64* values = push_array(scratch.arena, F64, Max(build_row_count, 1));
    U64 range_size = 0;
    void* base_ptr = (build_row_count > 0) ? gdb_column_get_data_range(scratch.arena, right_key_column, r1u64(0, build_row_count), &range_size) : 0;
    for (U64 i = 0; i < build_row_count; i++)
    {
      void* data = base_ptr ? (U8*)base_ptr + i * right_key_column->size : 0;
      switch (data ? right_key_column->type : GDB_ColumnType_U32)
      {
        case GDB_ColumnType_U32: values[i] = (F64)(*(U32*)data); break;
        case GDB_ColumnType_U64: values[i] = (F64)(*(U64*)data); break;
        case GDB_ColumnType_F32: values[i] = (F64)(*(F32*)data); break;
        case GDB_ColumnType_F64: values[i] = *(F64*)data; break;
        case GDB_ColumnType_Bool: values[i] = (F64)(*(U8*)data); break;
        case GDB_ColumnType_I32: values[i] = (F64)(*(S32*)data); break;
        case GDB_ColumnType_I64: values[i] = (F64)(*(S64*)data); break;
        case GDB_ColumnType_Date: values[i] = (F64)(*(S32*)data); break;
        case GDB_ColumnType_Timestamp: values[i] = (F64)(*(S64*)data); break;
        case GDB_ColumnType_Decimal: values[i] = (F64)(*(S64*)data); break;
        case GDB_ColumnType_Enum: values[i] = (F64)(*(U32*)data); break;
        default: values[i] = 0.0; break;
      }
    }
    build_data = values;
    build_data_size = Max(build_row_count, 1) * sizeof(F64);
  }
  
  //- tec: probe-side key data
  void* probe_data = NULL;
  U64* probe_offsets = NULL;
  U64 probe_data_size = 4;
  
  GPU_Buffer* probe_device_buffer = 0;
  if (device_join)
  {
    // tec: the key column stays on the GPU as a dense array and is read through the left rows, so nothing is gathered or uploaded here
    GPU_Buffer* full_key;
    if (dict_key)
    {
      // tec: left's own dict codes, recoded into right's dict code space so the probe compares against the same numbers the build side used
      full_key = qe_hash_join_left_key_recoded_full_f64(scratch.arena, left_key_column, join_dict);
    }
    else
    {
      B32 key_narrow = 0;
      full_key = qe_aggregate_full_column_f64(scratch.arena, left_key_column, 0, &key_narrow);
    }
    probe_device_buffer = full_key ? qe_selection_gather_column(qe_device_view(device_left, left_key_slot), full_key, 0, str8_lit("hj_probe_dense")) : 0;
    if (!probe_device_buffer)
    {
      out_device->declined = 1;
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    if (dict_key)
    {
      log_debug("qe_hash_join: dict-coded join key '%.*s' recoded into '%.*s''s dict space, join output stays device-resident",
                str8_varg(left_key_column->name), str8_varg(right_key_column->name));
    }
    probe_data_size = Max(probe_row_count, 1) * sizeof(F64);
  }
  else if (dict_key)
  {
    GDB_StringDataChunk chunk = qe_gather_string_column(scratch.arena, left, left_key_slot, left_key_column);
    probe_data = qe_dict_codes_from_string_chunk(scratch.arena, &chunk, join_dict);
    probe_data_size = Max(probe_row_count, 1) * sizeof(F64);
  }
  else if (is_string_key)
  {
    GDB_StringDataChunk chunk = qe_gather_string_column(scratch.arena, left, left_key_slot, left_key_column);
    probe_data = chunk.data;
    probe_offsets = chunk.offsets;
    probe_data_size = Max(chunk.size, 4);
  }
  else
  {
    probe_data = qe_gather_numeric_column(scratch.arena, left, left_key_slot, left_key_column);
    probe_data_size = Max(probe_row_count, 1) * sizeof(F64);
  }
  
  //- tec: a second whole number equality folds into the key, so the join emits only the rows matching both
  if (hints && hints->second_key && !device_join && !is_string_key && !dict_key && !is_filter_join && !is_left_join &&
      qe_join_key_is_whole_number(right_key_column) && qe_join_key_is_whole_number(left_key_column))
  {
    IR_Node* second_a = hints->second_key->first;
    IR_Node* second_b = second_a ? second_a->next : NULL;
    if (second_a && second_b)
    {
      B32 a_is_right = qe_column_belongs_to_table(right_table, right_alias, second_a->value);
      IR_Node* second_right = a_is_right ? second_a : second_b;
      IR_Node* second_left = a_is_right ? second_b : second_a;
      
      GDB_Column* right_second_column = gdb_table_find_column(right_table, qe_bare_column_name(second_right->value));
      String8 second_left_bare = {0};
      U64 second_left_slot = max_U64;
      GDB_Table* second_left_table = qe_resolve_column_table(left, second_left->value, &second_left_bare, &second_left_slot);
      GDB_Column* left_second_column = second_left_table ? gdb_table_find_column(second_left_table, second_left_bare) : NULL;
      
      if (right_second_column && left_second_column && qe_join_key_is_whole_number(right_second_column) &&
          qe_join_key_is_whole_number(left_second_column))
      {
        PLAN_RowSet build_rows = right_subset;
        if (!right_rows)
        {
          U64* identity = push_array_no_zero(scratch.arena, U64, Max(build_row_count, 1));
          for (U64 i = 0; i < build_row_count; i++)
          {
            identity[i] = i;
          }
          build_rows.tables = &right_table;
          build_rows.aliases = &right_alias;
          build_rows.table_count = 1;
          build_rows.row_indices = &identity;
          build_rows.count = build_row_count;
        }
        F64* build_second = qe_gather_numeric_column(scratch.arena, &build_rows, 0, right_second_column);
        F64* probe_second = qe_gather_numeric_column(scratch.arena, left, second_left_slot, left_second_column);
        if (qe_join_pack_keys(scratch.arena, (F64*)build_data, build_second, build_row_count, (F64*)probe_data, probe_second, probe_row_count))
        {
          hints->second_key_applied = 1;
        }
      }
    }
  }
  
  //- tec: pass 1/2
  // hash the build side into a bucket histogram. upload the build key data/offsets, dispatch, and read the bucket histogram back
  GPU_Kernel* build_kernel = gpu_kernel_alloc(str8_lit("hash_join_build_count"));
  if (!build_kernel)
  {
    log_error("qe_hash_join: failed to alloc 'hash_join_build_count' kernel");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  U64 build_off_size = is_string_key ? (build_row_count + 1) * sizeof(U64) : 4;
  GPU_Buffer* build_data_buf = gpu_buffer_alloc_pooled(str8_lit("hj_build_data_buf"), build_data_size, GPU_BufferFlag_Write, 0);
  GPU_Buffer* build_off_buf = gpu_buffer_alloc_pooled(str8_lit("hj_build_off_buf"), build_off_size, GPU_BufferFlag_Write, 0);
  GPU_Buffer* bucket_count_buf = gpu_buffer_alloc_pooled(str8_lit("hj_bucket_count_buf"), num_buckets * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* row_bucket_buf = gpu_buffer_alloc_pooled(str8_lit("hj_row_bucket_buf"), Max(build_row_count, 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  
  if (!build_data_buf || !build_off_buf || !bucket_count_buf || !row_bucket_buf)
  {
    log_error("qe_hash_join: failed to allocate build-side GPU buffers (build_row_count=%llu, num_buckets=%llu)",
              build_row_count, num_buckets);
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  gpu_kernel_set_arg_buffer(build_kernel, 0, build_data_buf);
  gpu_kernel_set_arg_buffer(build_kernel, 1, build_off_buf);
  gpu_kernel_set_arg_buffer(build_kernel, 2, bucket_count_buf);
  gpu_kernel_set_arg_buffer(build_kernel, 3, row_bucket_buf);
  gpu_kernel_set_arg_u64(build_kernel, 0, build_row_count);
  gpu_kernel_set_arg_u64(build_kernel, 1, num_buckets);
  gpu_kernel_set_arg_u64(build_kernel, 2, is_string_key ? 1 : 0);
  
  // tec: with the prefix sum on the GPU the build pass, the scatter and the probe share one submit, the bucket counts never come back to the CPU
  B32 gpu_prefix = hints && hints->fuse_round_trips && num_buckets <= QE_JOIN_GPU_PREFIX_MAX_BUCKETS;
  GPU_Kernel* prefix_kernel = 0;
  if (gpu_prefix)
  {
    prefix_kernel = gpu_kernel_alloc(str8_lit("prefix_sum"));
    gpu_prefix = prefix_kernel != 0;
  }
  
  U32* bucket_count_readback = push_array(scratch.arena, U32, num_buckets);
  
  U32* bucket_offsets = 0;
  if (!gpu_prefix)
  {
    U64 build_start_us = out_trace ? os_now_microseconds() : 0;
    GPU_Batch* build_batch = gpu_batch_begin(build_data_size + build_off_size, num_buckets * sizeof(U32));
    gpu_batch_buffer_write(build_batch, build_data_buf, build_data, build_data_size);
    if (is_string_key) gpu_batch_buffer_write(build_batch, build_off_buf, build_offsets, build_off_size);
    gpu_batch_buffer_zero(build_batch, bucket_count_buf, num_buckets * sizeof(U32));
    if (build_row_count > 0)
    {
      gpu_batch_kernel_execute(build_batch, build_kernel, (U32)build_row_count, QE_GPU_WORKGROUP_SIZE);
    }
    gpu_batch_buffer_read(build_batch, bucket_count_buf, bucket_count_readback, num_buckets * sizeof(U32));
    gpu_batch_end(build_batch);
    if (out_trace) out_trace->build_time_us = os_now_microseconds() - build_start_us;
    
    gpu_kernel_release(build_kernel);
    
    bucket_offsets = push_array(scratch.arena, U32, num_buckets + 1);
    U32 running = 0;
    for (U64 b = 0; b < num_buckets; b++)
    {
      bucket_offsets[b] = running;
      running += bucket_count_readback[b];
    }
    bucket_offsets[num_buckets] = running;
  }
  
  //- tec: pass 2/2
  // scatter build rows into per-bucket CSR lists 
  GPU_Kernel* scatter_kernel = gpu_kernel_alloc(str8_lit("csr_scatter"));
  if (!scatter_kernel)
  {
    log_error("qe_hash_join: failed to alloc 'csr_scatter' kernel");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  GPU_Buffer* cursor_buf = gpu_buffer_alloc_pooled(str8_lit("hj_cursor_buf"), num_buckets * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* bucket_rows_buf = gpu_buffer_alloc_pooled(str8_lit("hj_bucket_rows_buf"), Max(build_row_count, 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  
  if (!cursor_buf || !bucket_rows_buf)
  {
    log_error("qe_hash_join: failed to allocate scatter-pass GPU buffers (num_buckets=%llu, build_row_count=%llu)",
              num_buckets, build_row_count);
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  gpu_kernel_set_arg_buffer(scatter_kernel, 0, row_bucket_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 1, cursor_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 2, bucket_rows_buf);
  gpu_kernel_set_arg_u64(scatter_kernel, 0, build_row_count);
  
  U64 out_hard_max_capacity = gpu_device_max_storage_buffer_range() / (4 * sizeof(U32));
  U64 out_capacity = qe_join_output_capacity(probe_row_count, build_row_count, hints, out_hard_max_capacity);
  if (out_capacity * 4 * sizeof(U32) > settings_u64(str8_lit("GPU_MAX_BUFFER_SIZE"), GPU_MAX_BUFFER_SIZE))
  {
    log_debug("qe_hash_join: probe output buffer (%llu bytes, %llu-pair capacity) exceeds GPU_MAX_BUFFER_SIZE - "
              "allocating it as a single large buffer rather than chunking", out_capacity * 4 * sizeof(U32), out_capacity);
  }
  
  GPU_Kernel* probe_kernel = gpu_kernel_alloc(str8_lit("hash_join_probe"));
  if (!probe_kernel)
  {
    log_error("qe_hash_join: failed to alloc 'hash_join_probe' kernel");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  U64 probe_off_size = is_string_key ? (probe_row_count + 1) * sizeof(U64) : 4;
  U64 bucket_offsets_size = (num_buckets + 1) * sizeof(U32);
  
  GPU_Buffer* probe_data_buf = probe_device_buffer ? probe_device_buffer : gpu_buffer_alloc_pooled(str8_lit("hj_probe_data_buf"), probe_data_size, GPU_BufferFlag_Write, 0);
  U64 probe_upload_size = probe_device_buffer ? 0 : probe_data_size;
  GPU_Buffer* probe_off_buf = gpu_buffer_alloc_pooled(str8_lit("hj_probe_off_buf"), probe_off_size, GPU_BufferFlag_Write, 0);
  GPU_BufferFlags offsets_flags = gpu_prefix ? GPU_BufferFlag_ReadWrite : GPU_BufferFlag_Write;
  GPU_Buffer* bucket_offsets_buf = gpu_buffer_alloc_pooled(str8_lit("hj_bucket_offsets_buf"), bucket_offsets_size, offsets_flags, 0);
  
  GPU_Buffer* out_count_buf = gpu_buffer_alloc_pooled(str8_lit("hj_out_count_buf"), sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* out_pairs_buf = gpu_buffer_alloc_pooled(str8_lit("hj_out_pairs_buf"), out_capacity * 4 * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  
  if (!probe_data_buf || !probe_off_buf || !bucket_offsets_buf || !out_count_buf || !out_pairs_buf)
  {
    log_error("qe_hash_join: failed to allocate probe-pass GPU buffers (probe_row_count=%llu, out_capacity=%llu)",
              probe_row_count, out_capacity);
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  gpu_kernel_set_arg_buffer(probe_kernel, 0, build_data_buf);
  gpu_kernel_set_arg_buffer(probe_kernel, 1, build_off_buf);
  gpu_kernel_set_arg_buffer(probe_kernel, 2, bucket_offsets_buf);
  gpu_kernel_set_arg_buffer(probe_kernel, 3, bucket_rows_buf);
  gpu_kernel_set_arg_buffer(probe_kernel, 4, probe_data_buf);
  gpu_kernel_set_arg_buffer(probe_kernel, 5, probe_off_buf);
  gpu_kernel_set_arg_buffer(probe_kernel, 6, out_pairs_buf);
  gpu_kernel_set_arg_buffer(probe_kernel, 7, out_count_buf);
  
  gpu_kernel_set_arg_u64(probe_kernel, 0, probe_row_count);
  gpu_kernel_set_arg_u64(probe_kernel, 1, num_buckets);
  gpu_kernel_set_arg_u64(probe_kernel, 2, (is_left_join || is_anti_join) ? 1 : 0);
  gpu_kernel_set_arg_u64(probe_kernel, 3, is_string_key ? 1 : 0);
  gpu_kernel_set_arg_u64(probe_kernel, 4, out_capacity);
  
  U32 prefix_blocks = (U32)((num_buckets + 255) / 256);
  if (gpu_prefix)
  {
    GPU_Buffer* block_sum_buf = gpu_buffer_alloc_pooled(str8_lit("hj_block_sum_buf"), ((U64)prefix_blocks + 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    if (!block_sum_buf)
    {
      log_error("qe_hash_join: failed to allocate the prefix sum block buffer (num_buckets=%llu)", num_buckets);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    gpu_kernel_set_arg_buffer(prefix_kernel, 0, bucket_count_buf);
    gpu_kernel_set_arg_buffer(prefix_kernel, 1, bucket_offsets_buf);
    gpu_kernel_set_arg_buffer(prefix_kernel, 2, cursor_buf);
    gpu_kernel_set_arg_buffer(prefix_kernel, 3, block_sum_buf);
    gpu_kernel_set_arg_u64(prefix_kernel, 0, num_buckets);
  }
  
  U64 probe_upload_bytes = num_buckets * sizeof(U32) + probe_upload_size + bucket_offsets_size;
  if (is_string_key) probe_upload_bytes += probe_off_size;
  
  U64 match_count = 0;
  U64 probe_dispatch_start_us = out_trace ? os_now_microseconds() : 0;
  // tec: with a trustworthy output estimate the pairs come back in the same submit as the count, a bigger result falls back to a second download
  U64 speculative_pairs = 0;
  U32* speculative_data = 0;
  if (hints && hints->fuse_round_trips && hints->output_rows > 0 && !device_join)
  {
    speculative_pairs = hints->output_rows + hints->output_rows / 4 + 16;
    speculative_pairs = Min(speculative_pairs, out_capacity);
    speculative_pairs = Min(speculative_pairs, (U64)QE_JOIN_SPECULATIVE_MAX_PAIRS);
    speculative_data = push_array(scratch.arena, U32, speculative_pairs * 4);
  }
  
  U32 probe_passes = 0;
  for (;;)
  {
    probe_passes += 1;
    U32 match_count32 = 0;
    U64 batch_upload_bytes = probe_upload_bytes;
    if (gpu_prefix)
    {
      batch_upload_bytes = 0;
      if (probe_passes == 1)
      {
        batch_upload_bytes = build_data_size + probe_upload_size;
        if (is_string_key)
        {
          batch_upload_bytes += build_off_size + probe_off_size;
        }
      }
    }
    GPU_Batch* probe_batch = gpu_batch_begin(batch_upload_bytes, sizeof(U32) + speculative_pairs * 4 * sizeof(U32));
    if (gpu_prefix)
    {
      // tec: a second pass only exists because the output buffer was too small, the inputs and the counts are still on the device
      if (probe_passes == 1)
      {
        gpu_batch_buffer_write(probe_batch, build_data_buf, build_data, build_data_size);
        if (is_string_key)
        {
          gpu_batch_buffer_write(probe_batch, build_off_buf, build_offsets, build_off_size);
        }
        if (!probe_device_buffer)
        {
          gpu_batch_buffer_write(probe_batch, probe_data_buf, probe_data, probe_data_size);
        }
        if (is_string_key)
        {
          gpu_batch_buffer_write(probe_batch, probe_off_buf, probe_offsets, probe_off_size);
        }
        gpu_batch_buffer_zero(probe_batch, bucket_count_buf, num_buckets * sizeof(U32));
        if (build_row_count > 0)
        {
          gpu_batch_kernel_execute(probe_batch, build_kernel, (U32)build_row_count, QE_GPU_WORKGROUP_SIZE);
        }
      }
      gpu_kernel_set_arg_u64(prefix_kernel, 1, 0);
      gpu_batch_kernel_execute(probe_batch, prefix_kernel, prefix_blocks * 256, 256);
      gpu_kernel_set_arg_u64(prefix_kernel, 1, 1);
      gpu_batch_kernel_execute(probe_batch, prefix_kernel, 256, 256);
      gpu_kernel_set_arg_u64(prefix_kernel, 1, 2);
      gpu_batch_kernel_execute(probe_batch, prefix_kernel, prefix_blocks * 256, 256);
      if (build_row_count > 0)
      {
        gpu_batch_kernel_execute(probe_batch, scatter_kernel, (U32)build_row_count, QE_GPU_WORKGROUP_SIZE);
      }
      gpu_batch_buffer_zero(probe_batch, out_count_buf, sizeof(U32));
      if (probe_row_count > 0)
      {
        gpu_batch_kernel_execute(probe_batch, probe_kernel, (U32)probe_row_count, QE_GPU_WORKGROUP_SIZE);
      }
    }
    else
    {
      gpu_batch_buffer_write(probe_batch, cursor_buf, bucket_offsets, num_buckets * sizeof(U32));
      if (build_row_count > 0)
      {
        gpu_batch_kernel_execute(probe_batch, scatter_kernel, (U32)build_row_count, QE_GPU_WORKGROUP_SIZE);
      }
      if (!probe_device_buffer)
      {
        gpu_batch_buffer_write(probe_batch, probe_data_buf, probe_data, probe_data_size);
      }
      if (is_string_key) gpu_batch_buffer_write(probe_batch, probe_off_buf, probe_offsets, probe_off_size);
      gpu_batch_buffer_write(probe_batch, bucket_offsets_buf, bucket_offsets, bucket_offsets_size);
      gpu_batch_buffer_zero(probe_batch, out_count_buf, sizeof(U32));
      if (probe_row_count > 0)
      {
        gpu_batch_kernel_execute(probe_batch, probe_kernel, (U32)probe_row_count, QE_GPU_WORKGROUP_SIZE);
      }
    }
    gpu_batch_buffer_read(probe_batch, out_count_buf, &match_count32, sizeof(U32));
    if (speculative_pairs > 0)
    {
      gpu_batch_buffer_read(probe_batch, out_pairs_buf, speculative_data, speculative_pairs * 4 * sizeof(U32));
    }
    if (!gpu_batch_end(probe_batch))
    {
      log_error("qe_hash_join: GPU dispatch failed (device lost?) during probe pass");
      gpu_kernel_release(scatter_kernel);
      gpu_kernel_release(probe_kernel);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    
    match_count = match_count32;
    
    if (match_count <= out_capacity)
    {
      break;
    }
    
    U64 exact_capacity = Min(match_count, out_hard_max_capacity);
    if (exact_capacity == out_capacity)
    {
      log_error("qe_hash_join: join produced %llu matches, exceeding this GPU's maximum output buffer "
                "capacity (%llu pairs) - result is truncated", match_count, out_capacity);
      match_count = out_capacity;
      break;
    }
    
    log_debug("qe_hash_join: probe output heuristic undercounted (%llu matches > %llu-pair capacity) - "
              "retrying with exact capacity", match_count, out_capacity);
    out_capacity = exact_capacity;
    out_pairs_buf = gpu_buffer_alloc_pooled(str8_lit("hj_out_pairs_buf"), out_capacity * 4 * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
    if (!out_pairs_buf)
    {
      log_error("qe_hash_join: failed to reallocate probe output buffer for exact capacity %llu", out_capacity);
      gpu_kernel_release(scatter_kernel);
      gpu_kernel_release(probe_kernel);
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    gpu_kernel_set_arg_buffer(probe_kernel, 6, out_pairs_buf);
    gpu_kernel_set_arg_u64(probe_kernel, 4, out_capacity);
  }
  
  if (out_trace) out_trace->probe_dispatch_time_us = os_now_microseconds() - probe_dispatch_start_us;
  gpu_kernel_release(scatter_kernel);
  if (gpu_prefix)
  {
    gpu_kernel_release(build_kernel);
    gpu_kernel_release(prefix_kernel);
  }
  
  U64 download_start_us = out_trace ? os_now_microseconds() : 0;
  U32* pairs_readback = speculative_data;
  if (!device_join && (!speculative_data || match_count > speculative_pairs))
  {
    pairs_readback = push_array(scratch.arena, U32, Max(match_count, 1) * 4);
    if (match_count > 0)
    {
      gpu_buffer_read(out_pairs_buf, pairs_readback, match_count * 4 * sizeof(U32));
    }
  }
  if (out_trace) out_trace->probe_download_time_us = os_now_microseconds() - download_start_us;
  
  gpu_kernel_release(probe_kernel);
  
  if (out_trace)
  {
    out_trace->build_row_count = build_row_count;
    out_trace->probe_row_count = probe_row_count;
    out_trace->output_row_count = match_count;
    out_trace->probe_passes = probe_passes;
    out_trace->output_capacity = out_capacity;
    log_debug("qe_hash_join: build_row_count=%llu probe_row_count=%llu phases (us): build=%llu probe_dispatch=%llu probe_download=%llu",
              build_row_count, probe_row_count, out_trace->build_time_us, out_trace->probe_dispatch_time_us, out_trace->probe_download_time_us);
  }
  
  if (device_join)
  {
    // tec: the pairs stay on the GPU. every table's rows are read out of them as a dense list, through the row list the table's side already had
    GPU_Buffer* right_map = 0;
    B32 resolved = 1;
    if (right_rows)
    {
      U32* right_rows32 = push_array(scratch.arena, U32, Max(right_count, (U64)1));
      for (U64 i = 0; i < right_count; i++)
      {
        right_rows32[i] = (U32)right_rows[i];
      }
      right_map = gpu_buffer_alloc_pooled(str8_lit("hj_right_rows"), Max(right_count, (U64)1) * sizeof(U32), GPU_BufferFlag_Write, right_rows32);
      resolved = right_map != 0;
    }
    
    out_device->count = match_count;
    out_device->table_count = left->table_count + 1;
    for (U32 t = 0; t < left->table_count && resolved; t++)
    {
      QE_DeviceSelection* in_view = &device_left->views[t];
      QE_DeviceSelection* out_view = &out_device->views[t];
      out_view->table = left->tables[t];
      out_view->count = match_count;
      out_view->stride_words = 1;
      out_view->row_offset = 0;
      // tec: named by the depth of the join, so the rows of a join below (the map here) are never the buffer being written
      out_view->rows = qe_rows_resolve(out_pairs_buf, 4, 0, in_view->rows, in_view->stride_words, in_view->row_offset, match_count, push_str8f(gpu_scratch_arena(), "hj_out_rows:%u:%u", out_device->table_count, t));
      out_view->valid = out_view->rows != 0;
      out_device->aliases[t] = left->aliases ? left->aliases[t] : (String8){0};
      resolved = out_view->valid;
    }
    if (resolved)
    {
      QE_DeviceSelection* right_view = &out_device->views[left->table_count];
      right_view->table = right_table;
      right_view->count = match_count;
      right_view->stride_words = 1;
      right_view->row_offset = 0;
      right_view->rows = qe_rows_resolve(out_pairs_buf, 4, 2, right_map, 1, 0, match_count, push_str8f(gpu_scratch_arena(), "hj_out_rows:%u:%u", out_device->table_count, left->table_count));
      right_view->valid = right_view->rows != 0;
      out_device->aliases[left->table_count] = right_alias;
      resolved = right_view->valid;
    }
    
    if (!resolved)
    {
      MemoryZeroStruct(out_device);
      out_device->declined = 1;
      scratch_end(scratch);
      ProfEnd();
      return result;
    }
    out_device->valid = 1;
    
    result.table_count = out_device->table_count;
    result.tables = push_array(arena, GDB_Table*, result.table_count);
    result.aliases = push_array(arena, String8, result.table_count);
    for (U32 t = 0; t < out_device->table_count; t++)
    {
      result.tables[t] = out_device->views[t].table;
      result.aliases[t] = out_device->aliases[t];
    }
    result.count = match_count;
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  if (is_filter_join)
  {
    PLAN_RowSet filtered = qe_join_filter_left_rows(arena, left, pairs_readback, match_count, !is_anti_join);
    if (out_trace)
    {
      out_trace->output_row_count = filtered.count;
    }
    scratch_end(scratch);
    ProfEnd();
    return filtered;
  }
  
  // tec: expand (probe_array_index, build_row) pairs into the final multi table row set
  // the left sides existing table columns come along unchanged, the right table is appended
  result.table_count = left->table_count + 1;
  result.tables = push_array(arena, GDB_Table*, result.table_count);
  MemoryCopy(result.tables, left->tables, left->table_count * sizeof(GDB_Table*));
  result.tables[left->table_count] = right_table;
  
  result.aliases = push_array(arena, String8, result.table_count);
  if (left->aliases)
  {
    MemoryCopy(result.aliases, left->aliases, left->table_count * sizeof(String8));
  }
  result.aliases[left->table_count] = right_alias;
  
  result.count = match_count;
  result.row_indices = push_array(arena, U64*, result.table_count);
  for (U64 t = 0; t < result.table_count; t++)
  {
    result.row_indices[t] = push_array(arena, U64, Max(match_count, 1));
  }
  
  for (U64 i = 0; i < match_count; i++)
  {
    U64 probe_idx = pairs_readback[i * 4 + 0] | ((U64)pairs_readback[i * 4 + 1] << 32);
    
    U32 build_lo = pairs_readback[i * 4 + 2];
    U32 build_hi = pairs_readback[i * 4 + 3];
    U64 build_row = (build_lo == max_U32 && build_hi == max_U32) ? PLAN_NULL_ROW : (build_lo | ((U64)build_hi << 32));
    if (right_rows && build_row != PLAN_NULL_ROW)
    {
      build_row = right_rows[build_row];
    }
    
    for (U64 t = 0; t < left->table_count; t++)
    {
      result.row_indices[t][i] = left->row_indices[t][probe_idx];
    }
    result.row_indices[left->table_count][i] = build_row;
  }
  
  scratch_end(scratch);
  ProfEnd();
  return result;
}
