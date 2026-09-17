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
  
  // tec: because null checking is weird, theres no right side operand.
  // so this needs to be checked now
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
  
  if (!left || !right)
  {
    log_error("qe_compile_condition: malformed comparison, missing operand(s)");
    qe_bytecode_emit(prog, QE_Opcode_PushTrue);
    return;
  }
  
  // tec: string comparisons - column op 'literal', where the column is a String8 column
  if (left->type == IR_NodeType_Column && right->type == IR_NodeType_Literal)
  {
    QE_ColumnBinding* binding = qe_bind_column(prog, table, left->value);
    if (binding && binding->type == GDB_ColumnType_String8)
    {
      B32 is_contains = str8_match(op, str8_lit("contains"), StringMatchFlag_CaseInsensitive);
      B32 is_eq = str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0);
      
      if (!is_contains && !is_eq)
      {
        log_error("qe_compile_condition: unsupported string operator '%.*s', defaulting to '='", str8_varg(op));
        is_eq = 1;
      }
      
      // tec: dictionary encoded equality
      if (is_eq)
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
            qe_bytecode_emit(prog, QE_Opcode_PushFalse);
            return;
          }
          if (out_trace) out_trace->dict_hit = 1;
          
          QE_ColumnBinding* dict_binding = qe_bind_column_dict_codes(prog, table, left->value);
          if (dict_binding)
          {
            U32 operand = ((U32)dict_binding->type << 8) | (dict_binding->first_slot & 0xff);
            qe_bytecode_emit(prog, QE_Opcode_LoadNumCol);
            qe_bytecode_emit(prog, operand);
            qe_bytecode_emit(prog, QE_Opcode_PushConst);
            qe_bytecode_emit(prog, qe_add_numeric_const(prog, (F64)code));
            qe_bytecode_emit(prog, QE_Opcode_CmpEq);
            return;
          }
        }
      }
      
      QE_StringConstRef ref = qe_add_string_const(prog, right->value);
      
      qe_bytecode_emit(prog, is_contains ? QE_Opcode_StrContains : QE_Opcode_StrEq);
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
typedef struct QE_PrefetchBindingResult QE_PrefetchBindingResult;
struct QE_PrefetchBindingResult
{
  B32 valid;
  B32 is_string;
  B32 cached;
  void* data_ptr;
  U64 size;
  GDB_StringDataChunk str_chunk;
};

typedef struct QE_PrefetchSlot QE_PrefetchSlot;
struct QE_PrefetchSlot
{
  Arena* arena;
  Rng1U64 chunk_range;
  U64 chunk_rows;
  QE_PrefetchBindingResult bindings[QE_MAX_COLUMN_BINDINGS];
};

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

typedef struct QE_PrefetchCtx QE_PrefetchCtx;
struct QE_PrefetchCtx
{
  QE_BytecodeProgram* prog;
  QE_PrefetchSlot slots[2];
  
  OS_Handle worker;
  // tec: main -> worker, "a request is pending" (mailbox depth 1)
  OS_Handle request_sem; 
  // tec: worker -> main, "the requested slot is filled"
  OS_Handle ready_sem;   
  
  U32 pending_slot;
  Rng1U64 pending_range;
  U64 pending_rows;
  B32 stop;
};

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
  ProfBeginFunction();
  
  QE_ScanResult result = {0};
  
  QE_BytecodeProgram* prog = push_array(arena, QE_BytecodeProgram, 1);
  qe_bytecode_program_build(arena, prog, database, table, NULL, where_clause, out_trace);
  
  U32 stack_depth = qe_bytecode_program_max_stack_depth(prog);
  log_info("scan_filter bytecode peak operand-stack depth: %u (of MAX_STACK=%u)", stack_depth, (U32)QE_SCAN_MAX_STACK);
  
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
  
  typedef struct QE_ResultChunk QE_ResultChunk;
  struct QE_ResultChunk
  {
    U64* indices;
    U64 count;
    QE_ResultChunk* next;
  };
  
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
    log_info("qe_scan_filter: zone-map pruning skipped %llu of %llu rows, %llu dispatch range(s) remain",
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
    
    log_info("filtering rows %llu-%llu", chunk_range.min, chunk_range.max);
    
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
        log_info("qe_scan_filter: reusing GPU-resident buffer for column '%.*s' (generation %llu, %llu bytes) - no read, no re-upload",
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
        if (in->valid)
        {
          String8 data_key = push_str8f(gpu_scratch_arena(), "%.*s.data", str8_varg(col_pool_key));
          GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(data_key, in->str_chunk.size, GPU_BufferFlag_Write | GPU_BufferFlag_HostVisible, in->str_chunk.data);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 0, data_buf);
          
          // tec: +1 row for the trailing offset used to compute the last strings size
          U64 offsets_size = (in->str_chunk.row_count + 1) * sizeof(U64);
          String8 offsets_key = push_str8f(gpu_scratch_arena(), "%.*s.offsets", str8_varg(col_pool_key));
          GPU_Buffer* offsets_buf = gpu_buffer_alloc_pooled(offsets_key, offsets_size, GPU_BufferFlag_Write | GPU_BufferFlag_CopyHostPointer, in->str_chunk.offsets);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 1, offsets_buf);
          
          column_slot_used[binding->first_slot] = 1;
          column_slot_used[binding->first_slot + 1] = 1;
        }
        else
        {
          log_error("qe_scan_filter: failed to load string data/offsets for column '%.*s'", str8_varg(binding->name));
        }
        
        // tec: safe to close now, the data's already been copyed into the GPU buffer above
        gdb_column_close_string_chunk(binding->column);
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
    
    // tec: at most 2 attempts
    U32 result_count32[2] = {0, 0};
    U64 result_count = 0;
    for (U32 attempt = 0; attempt < 2; attempt++)
    {
      U64 submit_wait_start = os_now_microseconds();
      GPU_Batch* dispatch_batch = gpu_batch_begin(0, sizeof(result_count32));
      gpu_batch_buffer_zero(dispatch_batch, result_counter_buffer, 2 * sizeof(U32));
      gpu_batch_kernel_execute(dispatch_batch, kernel, (U32)chunk_rows, QE_GPU_WORKGROUP_SIZE);
      gpu_batch_buffer_read(dispatch_batch, result_counter_buffer, result_count32, sizeof(result_count32));
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
    
    if (result_count != 0)
    {
      U32* raw_indices = push_array(arena, U32, result_count * 2);
      gpu_buffer_read(output_buffer, raw_indices, result_count * 2 * sizeof(U32));
      
      U64* chunk_data = push_array(arena, U64, result_count);
      for (U64 i = 0; i < result_count; i++)
      {
        chunk_data[i] = chunk_row_start + raw_indices[i * 2 + 0];
      }
      
      QE_ResultChunk* rc = push_array(arena, QE_ResultChunk, 1);
      rc->indices = chunk_data;
      rc->count = result_count;
      rc->next = 0;
      *tail = rc;
      tail = &rc->next;
    }
  }
  
  if (prefetch)
  {
    qe_prefetch_stop(prefetch);
  }
  
  gpu_kernel_release(kernel);
  
  log_info("gpu kernel total execution time: %llu microseconds", gpu_kernel_execution_time);
  log_info("load from disk total time: %llu microseconds", load_data_from_disk_time);
  log_info("buffer alloc total time: %llu microseconds", buffer_alloc_time);
  log_info("submit+wait (gpu_kernel_execute) total time: %llu microseconds", submit_wait_time);
  if (chunk_count > 1)
  {
    log_info("prefetch stall time (time the GPU sat idle waiting on disk I/O the pipeline failed to hide): %llu microseconds", prefetch_stall_time);
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
    
    U64* out_ptr = result.indices;
    for (QE_ResultChunk* chunk = result_chunks; chunk; chunk = chunk->next)
    {
      MemoryCopy(out_ptr, chunk->indices, chunk->count * sizeof(U64));
      out_ptr += chunk->count;
    }
  }
  ProfEnd();
  
  if (out_trace) out_trace->rows_after = result.count;
  
  ProfEnd();
  return result;
}

//~ tec: SELECT-list/HAVING item display name

internal String8
qe_column_list_item_display_name(Arena* arena, IR_Node* item)
{
  IR_Node* alias = ir_node_find_child(item, IR_NodeType_Alias);
  if (alias) return alias->value;
  
  if (item->type == IR_NodeType_AggregateCall)
  {
    IR_Node* arg = item->first;
    String8 arg_text = arg ? arg->value : str8_lit("*");
    // tec: a second, nonalias argument must be part of the display name too
    IR_Node* arg2 = (arg && arg->next && arg->next->type != IR_NodeType_Alias) ? arg->next : NULL;
    if (arg2)
    {
      return push_str8f(arena, "%.*s(%.*s, %.*s)", str8_varg(item->value), str8_varg(arg_text), str8_varg(arg2->value));
    }
    return push_str8f(arena, "%.*s(%.*s)", str8_varg(item->value), str8_varg(arg_text));
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

typedef struct QE_GatherNumericTask QE_GatherNumericTask;
struct QE_GatherNumericTask
{
  Rng1U64* ranges;
  U64* table_rows;
  void* base_ptr;
  U64 min_row;
  GDB_ColumnType column_type;
  U64 column_size;
  F64* values;
};

internal THREAD_POOL_TASK_FUNC(qe_gather_numeric_task)
{
  QE_GatherNumericTask* task = (QE_GatherNumericTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  
  for (U64 i = range.min; i < range.max; i++)
  {
    U64 row = task->table_rows[i];
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
  for (U64 i = 0; i < rows->count; i++)
  {
    U64 row = table_rows[i];
    if (row == PLAN_NULL_ROW) continue;
    if (row < min_row) min_row = row;
    if (row > max_row) max_row = row;
  }
  
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

typedef struct QE_GatherDictCodesTask QE_GatherDictCodesTask;
struct QE_GatherDictCodesTask
{
  Rng1U64* ranges;
  U64* table_rows;
  U32* dict_codes;
  F64* values;
};

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

typedef struct QE_DictLookupTask QE_DictLookupTask;
struct QE_DictLookupTask
{
  Rng1U64* ranges;
  GDB_StringDataChunk chunk;
  GDB_StringDict* dict;
  F64* values;
};

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

typedef struct QE_DenseDictCodesTask QE_DenseDictCodesTask;
struct QE_DenseDictCodesTask
{
  Rng1U64* ranges;
  U32* codes;
  F64* values;
};

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

typedef struct QE_NarrowCheckTask QE_NarrowCheckTask;
struct QE_NarrowCheckTask
{
  Rng1U64* ranges;
  F64* values;
  B32* task_narrow;
};

internal THREAD_POOL_TASK_FUNC(qe_narrow_check_task)
{
  QE_NarrowCheckTask* task = (QE_NarrowCheckTask*)raw_task;
  Rng1U64 range = task->ranges[task_id];
  B32 narrow = 1;
  for (U64 i = range.min; i < range.max; i++)
  {
    F64 v = task->values[i];
    if ((F64)(F32)v != v) { narrow = 0; break; }
  }
  task->task_narrow[task_id] = narrow;
}

internal B32
qe_values_round_trip_f32(F64* values, U64 count)
{
  if (count == 0) return 1;
  
  Temp scratch = scratch_begin(0, 0);
  TP_Context* pool = app_thread_pool();
  U64 task_count = Max((U64)1, Min((U64)pool->worker_count, count));
  
  QE_NarrowCheckTask task = {0};
  task.ranges = tp_divide_work(scratch.arena, count, (U32)task_count);
  task.values = values;
  task.task_narrow = push_array(scratch.arena, B32, task_count);
  
  TP_Arena* pool_arena = app_thread_pool_arena();
  TP_Temp temp = tp_temp_begin(pool_arena);
  tp_for_parallel(pool, pool_arena, task_count, qe_narrow_check_task, &task);
  tp_temp_end(temp);
  
  B32 all_narrow = 1;
  for (U64 t = 0; t < task_count; t++) if (!task.task_narrow[t]) { all_narrow = 0; break; }
  
  scratch_end(scratch);
  return all_narrow;
}

typedef struct QE_NarrowConvertTask QE_NarrowConvertTask;
struct QE_NarrowConvertTask
{
  Rng1U64* ranges;
  F64* src;
  F32* dst;
};

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

typedef struct QE_SortRowsCtx QE_SortRowsCtx;
struct QE_SortRowsCtx
{
  U32 num_keys;
  B32 key_is_string[QE_SORT_MAX_KEYS];
  B32 key_desc[QE_SORT_MAX_KEYS];
  F64* numeric_keys[QE_SORT_MAX_KEYS];        // tec: dense, indexed by output-row position (0..count-1)
  GDB_StringDataChunk string_keys[QE_SORT_MAX_KEYS];
};

global QE_SortRowsCtx* g_qe_sort_rows_ctx = 0;

internal int
qe_sort_rows_compare(const void* a, const void* b)
{
  U64 ia = *(const U64*)a;
  U64 ib = *(const U64*)b;
  QE_SortRowsCtx* ctx = g_qe_sort_rows_ctx;
  
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

internal PLAN_RowSet
qe_sort_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* order_by_ir, QE_SortTrace* out_trace)
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
  U32 num_keys = 0;
  B32 any_string_key = 0;
  
  for (IR_Node* col_node = order_by_ir->first; col_node != NULL; col_node = col_node->next)
  {
    if (num_keys >= QE_SORT_MAX_KEYS)
    {
      log_error("qe_sort_rows: more than %u ORDER BY columns is not supported, ignoring the rest", (U32)QE_SORT_MAX_KEYS);
      break;
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
  
  // tec: no GPU string-comparison kernel exists, so a string key (alone or mixed with numeric
  // keys) sorts a plain row-index array on the CPU instead of going through the bitonic path below
  if (any_string_key)
  {
    Temp scratch = scratch_begin(&arena, 1);
    
    QE_SortRowsCtx ctx = {0};
    ctx.num_keys = num_keys;
    for (U32 k = 0; k < num_keys; k++)
    {
      ctx.key_is_string[k] = key_is_string[k];
      ctx.key_desc[k] = key_desc[k];
      if (key_is_string[k])
      {
        ctx.string_keys[k] = qe_gather_string_column(scratch.arena, rows, key_slots[k], key_columns[k]);
      }
      else
      {
        ctx.numeric_keys[k] = qe_gather_numeric_column(scratch.arena, rows, key_slots[k], key_columns[k]);
      }
    }
    
    U64* order = push_array(scratch.arena, U64, real_count);
    for (U64 i = 0; i < real_count; i++) order[i] = i;
    
    QE_SortRowsCtx* prev_ctx = g_qe_sort_rows_ctx;
    g_qe_sort_rows_ctx = &ctx;
    quick_sort(order, real_count, sizeof(U64), qe_sort_rows_compare);
    g_qe_sort_rows_ctx = prev_ctx;
    
    result.row_indices = push_array(arena, U64*, rows->table_count);
    for (U64 t = 0; t < rows->table_count; t++)
    {
      result.row_indices[t] = push_array(arena, U64, real_count);
      for (U64 i = 0; i < real_count; i++)
      {
        result.row_indices[t][i] = rows->row_indices[t][order[i]];
      }
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
  log_info("qe_sort_rows: bitonic sort (real_count=%llu, padded_count=%llu, num_stages=%u, keys=%s) GPU kernel time: %llu microseconds",
           real_count, padded_count, num_stages, use_narrow_keys ? "f32" : "f64", sort_gpu_time_us);
  if (out_trace)
  {
    out_trace->row_count = real_count;
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

internal PLAN_Materialized
qe_sort_materialized(Arena* arena, PLAN_Materialized* m, IR_Node* order_by_ir)
{
  // tec: post-aggregate row counts are always small (bounded by distinct group count), unlike qe_sort_rows which sorts the (potentially huge) base table rows
  PLAN_Materialized result = *m;
  if (!order_by_ir || m->count <= 1) return result;
  
  Temp scratch = scratch_begin(&arena, 1);
  
  U64* order = push_array(scratch.arena, U64, m->count);
  for (U64 i = 0; i < m->count; i++) order[i] = i;
  
  for (U64 i = 1; i < m->count; i++)
  {
    U64 key = order[i];
    U64 j = i;
    while (j > 0 && qe_materialized_row_less(m, order_by_ir, key, order[j - 1]))
    {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = key;
  }
  
  result.columns = push_array(arena, PLAN_AggColumn, m->column_count);
  for (U64 c = 0; c < m->column_count; c++)
  {
    PLAN_AggColumn* src = &m->columns[c];
    PLAN_AggColumn* dst = &result.columns[c];
    dst->name = src->name;
    dst->type = src->type;
    
    if (src->type == GDB_ColumnType_String8)
    {
      dst->string_values = push_array(arena, String8, m->count);
      for (U64 i = 0; i < m->count; i++) dst->string_values[i] = src->string_values[order[i]];
    }
    else
    {
      dst->numeric_values = push_array(arena, F64, m->count);
      for (U64 i = 0; i < m->count; i++) dst->numeric_values[i] = src->numeric_values[order[i]];
    }
  }
  
  scratch_end(scratch);
  return result;
}

//~ tec: aggregate/HAVING

typedef struct QE_AggExprInfo QE_AggExprInfo;
struct QE_AggExprInfo
{
  String8 display_name;
  U32 func_code;
  GDB_Table* arg_table;
  U64 arg_slot;
  GDB_Column* arg_column;
  // tec: APPROX_PERCENTILE's fraction argument
  F64 f64_param;   
};

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

// tec: a t-digest centroid - a (mean, weight) pair.
typedef struct QE_TDigestCentroid QE_TDigestCentroid;
struct QE_TDigestCentroid
{
  F32 mean;
  F32 weight;
};

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
      for (U64 g = 0; g < num_groups; g++) dst->numeric_values[g] = results_readback[g * num_exprs + e];
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
      // so instead of a per-group gdb_column_get_string/qe_read_numeric_as_f64 call bound the touched range and pull it in one bulk read,
      // then index into it
      U64 min_row = max_U64, max_row = 0;
      for (U64 g = 0; g < num_groups; g++)
      {
        U64 dense_idx = representative_readback[g];
        U64 base_row = (dense_idx < input->count) ? input->row_indices[table_slot][dense_idx] : PLAN_NULL_ROW;
        if (base_row == PLAN_NULL_ROW) continue;
        if (base_row < min_row) min_row = base_row;
        if (base_row > max_row) max_row = base_row;
      }
      
      if (column->type == GDB_ColumnType_String8)
      {
        dst->string_values = push_array(arena, String8, Max(num_groups, 1));
        
        GDB_StringDataChunk chunk = {0};
        if (min_row != max_U64) chunk = gdb_column_get_string_chunk(arena, column, r1u64(min_row, max_row + 1));
        
        for (U64 g = 0; g < num_groups; g++)
        {
          U64 dense_idx = representative_readback[g];
          U64 base_row = (dense_idx < input->count) ? input->row_indices[table_slot][dense_idx] : PLAN_NULL_ROW;
          if (base_row == PLAN_NULL_ROW || !chunk.offsets)
          {
            dst->string_values[g] = str8_lit("");
            continue;
          }
          U64 local = base_row - min_row;
          U64 start = chunk.offsets[local];
          U64 end = chunk.offsets[local + 1];
          dst->string_values[g] = str8((U8*)chunk.data + start, end - start);
        }
      }
      else
      {
        dst->numeric_values = push_array(arena, F64, Max(num_groups, 1));
        
        void* base_ptr = 0;
        U64 range_size = 0;
        if (min_row != max_U64) base_ptr = gdb_column_get_data_range(arena, column, r1u64(min_row, max_row + 1), &range_size);
        
        for (U64 g = 0; g < num_groups; g++)
        {
          U64 dense_idx = representative_readback[g];
          U64 base_row = (dense_idx < input->count) ? input->row_indices[table_slot][dense_idx] : PLAN_NULL_ROW;
          if (base_row == PLAN_NULL_ROW || !base_ptr)
          {
            dst->numeric_values[g] = 0.0;
            continue;
          }
          void* data = (U8*)base_ptr + (base_row - min_row) * column->size;
          switch (column->type)
          {
            case GDB_ColumnType_U32: dst->numeric_values[g] = (F64)(*(U32*)data); break;
            case GDB_ColumnType_U64: dst->numeric_values[g] = (F64)(*(U64*)data); break;
            case GDB_ColumnType_F32: dst->numeric_values[g] = (F64)(*(F32*)data); break;
            case GDB_ColumnType_F64: dst->numeric_values[g] = *(F64*)data; break;
            case GDB_ColumnType_Bool: dst->numeric_values[g] = (F64)(*(U8*)data); break;
            case GDB_ColumnType_I32: dst->numeric_values[g] = (F64)(*(S32*)data); break;
            case GDB_ColumnType_I64: dst->numeric_values[g] = (F64)(*(S64*)data); break;
            case GDB_ColumnType_Date: dst->numeric_values[g] = (F64)(*(S32*)data); break;
            case GDB_ColumnType_Timestamp: dst->numeric_values[g] = (F64)(*(S64*)data); break;
            case GDB_ColumnType_Decimal: dst->numeric_values[g] = (F64)(*(S64*)data); break;
            case GDB_ColumnType_Enum: dst->numeric_values[g] = (F64)(*(U32*)data); break;
            default: dst->numeric_values[g] = 0.0; break;
          }
        }
      }
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
    for (U64 g = 0; g < num_groups; g++) dst->numeric_values[g] = results_readback[g * num_exprs + e];
    out_count++;
  }
  
  result.columns = out_columns;
  result.column_count = out_count;
  result.count = num_groups;
  return result;
}

internal PLAN_Materialized
qe_aggregate(Arena* arena, GDB_Database* database, PLAN_RowSet* input, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir, QE_AggregateTrace* out_trace)
{
  ProfBeginFunction();
  
  PLAN_Materialized result = {0};
  U64 row_count = input->count;
  U64 qe_agg_t_start = os_now_microseconds();
  
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
  
  //- tec: gather GROUP BY key columns (dense, index-aligned 0..row_count-1 to the input row set)
  F64* group_numeric[QE_AGG_MAX_GROUP_COLS] = {0};
  GDB_StringDataChunk group_string[QE_AGG_MAX_GROUP_COLS];
  MemoryZeroArray(group_string);
  U32 group_string_mask = 0;
  
  for (U32 c = 0; c < num_group_cols; c++)
  {
    if (group_columns[c]->type == GDB_ColumnType_String8)
    {
      gdb_column_ensure_string_dict(group_columns[c]);
      if (group_columns[c]->has_dict)
      {
        group_numeric[c] = qe_gather_string_dict_codes(arena, input, group_slots[c], group_columns[c]);
      }
      else
      {
        group_string[c] = qe_gather_string_column(arena, input, group_slots[c], group_columns[c]);
        group_string_mask |= (1u << c);
      }
    }
    else
    {
      group_numeric[c] = qe_gather_numeric_column(arena, input, group_slots[c], group_columns[c]);
    }
  }
  
  //- tec: gather aggregate argument columns
  F64* expr_args[QE_AGG_MAX_EXPRS] = {0};
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!exprs[e].arg_column) continue;
    
    U32 same_as = e;
    for (U32 e2 = 0; e2 < e; e2++)
    {
      if (exprs[e2].arg_column == exprs[e].arg_column && exprs[e2].arg_slot == exprs[e].arg_slot) 
      { 
        same_as = e2; 
        break; 
      }
    }
    
    expr_args[e] = (same_as != e) ? expr_args[same_as] : qe_gather_numeric_column(arena, input, exprs[e].arg_slot, exprs[e].arg_column);
  }
  
  // tec: narrow numeric aggregate args
  B32 arg_narrow = 1;
  for (U32 e = 0; e < num_exprs && arg_narrow; e++)
  {
    if (!expr_args[e])
    {
      continue;
    }
    B32 already_checked = 0;
    for (U32 e2 = 0; e2 < e; e2++) 
    {
      if (expr_args[e2] == expr_args[e]) 
      { 
        already_checked = 1; 
        break; 
      }
    }
    if (already_checked) 
    {
      continue;
    }
    if (!qe_values_round_trip_f32(expr_args[e], row_count)) 
    {
      arg_narrow = 0;
    }
  }
  F32* expr_args_f32[QE_AGG_MAX_EXPRS] = {0};
  if (arg_narrow)
  {
    for (U32 e = 0; e < num_exprs; e++)
    {
      if (!expr_args[e]) 
      {
        continue;
      }
      U32 same_as = e;
      for (U32 e2 = 0; e2 < e; e2++) 
      {
        if (expr_args[e2] == expr_args[e]) 
        { 
          same_as = e2; 
          break; 
        }
      }
      if (same_as != e) 
      { 
        expr_args_f32[e] = expr_args_f32[same_as]; 
        continue;
      }
      expr_args_f32[e] = qe_values_to_f32(arena, expr_args[e], row_count);
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
    while (num_buckets < row_count / 64 && num_buckets < max_num_buckets) num_buckets <<= 1;
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
  GPU_Buffer* owner_buf = 0;
  GPU_Buffer* count_buf = 0;
  GPU_Buffer* row_slot_buf = 0;
  GPU_Buffer* overflow_buf = 0;
  U32* count_readback = 0;
  U32 overflow_readback = 0;
  
  for (;;)
  {
    num_slots = num_buckets * K;
    
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
      log_info("qe_aggregate: group-by hash table (%llu bytes, row_count=%llu) exceeds GPU_MAX_BUFFER_SIZE - "
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
    U64 assign_download_bytes = (U64)num_slots * sizeof(U32) + sizeof(U32);
    
    count_readback = push_array(scratch.arena, U32, num_slots);
    overflow_readback = 0;
    
    if (num_group_cols == 0)
    {
      // tec: no GROUP BY
      count_readback[0] = (U32)Min(row_count, (U64)max_U32);
    }
    else
    {
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
        else
        {
          gpu_batch_buffer_write(assign_batch, group_col_bufs[c * 2 + 0], group_numeric[c], group_col_sizes[c * 2 + 0]);
        }
      }
      gpu_batch_kernel_execute(assign_batch, assign_kernel, (U32)row_count, QE_GPU_WORKGROUP_SIZE);
      gpu_batch_buffer_read(assign_batch, count_buf, count_readback, num_slots * sizeof(U32));
      gpu_batch_buffer_read(assign_batch, overflow_buf, &overflow_readback, sizeof(U32));
      if (!gpu_batch_end(assign_batch))
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
      log_info("qe_aggregate: %u row(s) overflowed the group-by hash table with num_buckets=%llu - retrying with num_buckets=%llu",
               overflow_readback, num_buckets, next_num_buckets);
      num_buckets = next_num_buckets;
    }
    else
    {
      U64 next_K = K * 2;
      log_info("qe_aggregate: %u row(s) overflowed the group-by hash table with K=%llu - retrying with K=%llu",
               overflow_readback, K, next_K);
      K = next_K;
    }
  }
  
  gpu_kernel_release(assign_kernel);
  U64 qe_agg_t_assigned = os_now_microseconds();
  
  // tec: prefix-sum slot_row_count -> slot_offsets, and (for GROUP BY) compact occupied slots
  U32* slot_offsets = push_array(scratch.arena, U32, num_slots + 1);
  U32* group_ids_scratch = (num_group_cols == 0) ? 0 : push_array(scratch.arena, U32, num_slots);
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
  
  U64 num_groups;
  U32* group_ids;
  if (num_group_cols == 0)
  {
    num_groups = 1;
    group_ids = push_array(scratch.arena, U32, 1);
    group_ids[0] = 0;
  }
  else
  {
    num_groups = num_groups_found;
    group_ids = group_ids_scratch;
  }
  
  //- tec: pass 2/3 
  // scatter rows into per slot CSR member lists
  // and pass 3/3 - one thread per group, serial reduction over its own member rows
  GPU_Kernel* scatter_kernel = gpu_kernel_alloc(str8_lit("csr_scatter"));
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
    if (!expr_args[e]) continue;
    
    U32 same_as = e;
    for (U32 e2 = 0; e2 < e; e2++)
    {
      if (expr_args[e2] == expr_args[e]) { same_as = e2; break; }
    }
    
    arg_bufs[e] = (same_as != e) ? arg_bufs[same_as]
      : gpu_buffer_alloc_pooled(push_str8f(gpu_scratch_arena(), "agg_arg_buf:%u", e), row_count * (arg_narrow ? sizeof(F32) : sizeof(F64)), GPU_BufferFlag_Write, 0);
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
    if (arg_bufs[e]) 
    {
      reduce_upload_bytes += row_count * arg_elem_size;
    }
  }
  U64 reduce_download_bytes = repr_size + partials_size + (num_hll_exprs > 0 ? sketch_size : 0) + (num_pct_exprs > 0 ? digest_size * 2 : 0);
  
  U32* repr32 = push_array(scratch.arena, U32, Max(num_groups, 1));
  F64* partials_readback = push_array(scratch.arena, F64, Max(total_chunks, 1) * Max(num_exprs, 1) * 4);
  F64* results_readback = push_array(scratch.arena, F64, Max(num_groups * Max(num_exprs, 1), 1));
  
  GPU_Batch* scatter_batch = gpu_batch_begin(reduce_upload_bytes, 0);
  gpu_batch_buffer_write(scatter_batch, chunk_range_buf, chunk_range, chunk_range_size);
  gpu_batch_buffer_write(scatter_batch, chunk_group_buf, chunk_group, chunk_group_size);
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (!arg_bufs[e])
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
    gpu_batch_kernel_execute(scatter_batch, scatter_kernel, (U32)row_count, QE_GPU_WORKGROUP_SIZE);
  }
  gpu_batch_end(scatter_batch);
  
  GPU_Batch* reduce_batch = gpu_batch_begin(0, reduce_download_bytes);
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
  log_info("qe_aggregate: reduce batch (row_count=%llu, is_string=%d, total_chunks=%llu) GPU time: %llu microseconds",
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
  
  result = qe_aggregate_build_output(arena, input, column_list_ir, exprs, num_exprs, num_groups, representative_readback, results_readback);
  
  log_info("qe_aggregate: row_count=%llu num_groups=%llu phases (us): gather=%llu assign=%llu reduce=%llu combine=%llu total=%llu",
           row_count, num_groups,
           qe_agg_t_gathered - qe_agg_t_start,
           qe_agg_t_assigned - qe_agg_t_gathered,
           qe_agg_t_reduced - qe_agg_t_assigned,
           qe_agg_t_combined - qe_agg_t_reduced,
           qe_agg_t_combined - qe_agg_t_start);
  if (out_trace)
  {
    out_trace->input_row_count = row_count;
    out_trace->group_count = num_groups;
    out_trace->gather_time_us = qe_agg_t_gathered - qe_agg_t_start;
    out_trace->assign_time_us = qe_agg_t_assigned - qe_agg_t_gathered;
    out_trace->reduce_time_us = qe_agg_t_reduced - qe_agg_t_assigned;
    out_trace->combine_time_us = qe_agg_t_combined - qe_agg_t_reduced;
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
    
    B32 eq = qe_str8_compare(ls, rs) == 0;
    if (str8_match(op, str8_lit("!="), 0))
    {
      return !eq;
    }
    return eq; // tec: default '=' / '=='
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

internal PLAN_Materialized
qe_apply_having(Arena* arena, PLAN_Materialized* m, IR_Node* having_ir)
{
  IR_Node* condition = having_ir ? having_ir->first : NULL; // tec: mirrors qe_compile_condition's where_clause->first convention
  if (!condition) return *m;
  
  Temp scratch = scratch_begin(&arena, 1);
  U64* keep = push_array(scratch.arena, U64, Max(m->count, 1));
  U64 keep_count = 0;
  
  for (U64 i = 0; i < m->count; i++)
  {
    if (qe_having_eval(m, condition, i)) keep[keep_count++] = i;
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

internal B32
qe_index_range_for_leaf(Arena* arena, GDB_Table* table, IR_Node* condition, QE_ScanResult* out_result)
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
  if (row_count == 0)
  {
    out_result->indices = push_array(arena, U64, 1);
    out_result->count = 0;
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
  
  U64 match_count = range_hi - range_lo;
  out_result->indices = push_array(arena, U64, Max(match_count, 1));
  out_result->count = match_count;
  for (U64 i = 0; i < match_count; i++)
  {
    out_result->indices[i] = index->order[range_lo + i];
  }
  
  scratch_end(scratch);
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
    QE_ScanResult narrowed = {0};
    if (!qe_index_range_for_leaf(arena, table, leaves[i], &narrowed))
    {
      continue;
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
  
  return 0;
}

typedef struct QE_ZonemapLeaf QE_ZonemapLeaf;
struct QE_ZonemapLeaf
{
  GDB_Column* column;
  B32 is_eq, is_lt, is_le, is_gt, is_ge;
  F64 target;
};

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
    
    B32 eq = qe_str8_compare(ls, rs) == 0;
    if (str8_match(op, str8_lit("!="), 0)) return !eq;
    return eq; // tec: default '=' / '=='
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

typedef struct QE_CpuScanTask QE_CpuScanTask;
struct QE_CpuScanTask
{
  Rng1U64* ranges;
  PLAN_RowSet* rows;
  IR_Node* condition_root;
  U64* task_matched_counts;
  U64** task_matched_indices;
};

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
  log_info("qe_cpu_scan_filter: CPU scan (row_count=%llu) total time: %llu microseconds", row_count, cpu_scan_time_us);
  
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
  U64* keep = push_array(scratch.arena, U64, Max(rows->count, 1));
  U64 keep_count = 0;
  
  for (U64 i = 0; i < rows->count; i++)
  {
    if (qe_row_condition_eval(arena, rows, condition, i)) keep[keep_count++] = i;
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

internal PLAN_RowSet
qe_hash_join(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias, String8 join_type, IR_Node* condition, QE_JoinTrace* out_trace)
{
  ProfBeginFunction();
  PLAN_RowSet result = {0};
  
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
  
  U64 build_row_count = right_table->row_count;
  U64 probe_row_count = left->count;
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
    log_info("qe_hash_join: build-side hash table (%llu bytes, build_row_count=%llu) exceeds GPU_MAX_BUFFER_SIZE - "
             "allocating it as a single large buffer rather than chunking", num_buckets * sizeof(U32), build_row_count);
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: build-side key data
  void* build_data = NULL;
  U64* build_offsets = NULL;
  U64 build_data_size = 4;
  
  if (dict_key)
  {
    build_data = qe_dict_codes_to_f64_dense(scratch.arena, right_key_column->dict_codes, build_row_count);
    build_data_size = Max(build_row_count, 1) * sizeof(F64);
  }
  else if (is_string_key)
  {
    GDB_StringDataChunk chunk = gdb_column_get_string_chunk(scratch.arena, right_key_column, r1u64(0, build_row_count));
    build_data = chunk.data;
    build_offsets = chunk.offsets;
    build_data_size = Max(chunk.size, 4);
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
  
  if (dict_key)
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
  
  U32* bucket_count_readback = push_array(scratch.arena, U32, num_buckets);
  
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
  
  U32* bucket_offsets = push_array(scratch.arena, U32, num_buckets + 1);
  U32 running = 0;
  for (U64 b = 0; b < num_buckets; b++)
  {
    bucket_offsets[b] = running;
    running += bucket_count_readback[b];
  }
  bucket_offsets[num_buckets] = running;
  
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
  U64 out_capacity = Max(probe_row_count, build_row_count) + probe_row_count;
  if (out_capacity > out_hard_max_capacity) out_capacity = out_hard_max_capacity;
  if (out_capacity < 1) out_capacity = 1;
  if (out_capacity * 4 * sizeof(U32) > settings_u64(str8_lit("GPU_MAX_BUFFER_SIZE"), GPU_MAX_BUFFER_SIZE))
  {
    log_info("qe_hash_join: probe output buffer (%llu bytes, %llu-pair capacity) exceeds GPU_MAX_BUFFER_SIZE - "
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
  
  GPU_Buffer* probe_data_buf = gpu_buffer_alloc_pooled(str8_lit("hj_probe_data_buf"), probe_data_size, GPU_BufferFlag_Write, 0);
  GPU_Buffer* probe_off_buf = gpu_buffer_alloc_pooled(str8_lit("hj_probe_off_buf"), probe_off_size, GPU_BufferFlag_Write, 0);
  GPU_Buffer* bucket_offsets_buf = gpu_buffer_alloc_pooled(str8_lit("hj_bucket_offsets_buf"), bucket_offsets_size, GPU_BufferFlag_Write, 0);
  
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
  gpu_kernel_set_arg_u64(probe_kernel, 2, is_left_join ? 1 : 0);
  gpu_kernel_set_arg_u64(probe_kernel, 3, is_string_key ? 1 : 0);
  gpu_kernel_set_arg_u64(probe_kernel, 4, out_capacity);
  
  U64 probe_upload_bytes = num_buckets * sizeof(U32) + probe_data_size + bucket_offsets_size;
  if (is_string_key) probe_upload_bytes += probe_off_size;
  
  U64 match_count = 0;
  U64 probe_dispatch_start_us = out_trace ? os_now_microseconds() : 0;
  for (;;)
  {
    U32 match_count32 = 0;
    GPU_Batch* probe_batch = gpu_batch_begin(probe_upload_bytes, sizeof(U32));
    gpu_batch_buffer_write(probe_batch, cursor_buf, bucket_offsets, num_buckets * sizeof(U32));
    if (build_row_count > 0)
    {
      gpu_batch_kernel_execute(probe_batch, scatter_kernel, (U32)build_row_count, QE_GPU_WORKGROUP_SIZE);
    }
    gpu_batch_buffer_write(probe_batch, probe_data_buf, probe_data, probe_data_size);
    if (is_string_key) gpu_batch_buffer_write(probe_batch, probe_off_buf, probe_offsets, probe_off_size);
    gpu_batch_buffer_write(probe_batch, bucket_offsets_buf, bucket_offsets, bucket_offsets_size);
    gpu_batch_buffer_zero(probe_batch, out_count_buf, sizeof(U32));
    if (probe_row_count > 0)
    {
      gpu_batch_kernel_execute(probe_batch, probe_kernel, (U32)probe_row_count, QE_GPU_WORKGROUP_SIZE);
    }
    gpu_batch_buffer_read(probe_batch, out_count_buf, &match_count32, sizeof(U32));
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
    
    log_info("qe_hash_join: probe output heuristic undercounted (%llu matches > %llu-pair capacity) - "
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
  
  U64 download_start_us = out_trace ? os_now_microseconds() : 0;
  U32* pairs_readback = push_array(scratch.arena, U32, Max(match_count, 1) * 4);
  if (match_count > 0)
  {
    gpu_buffer_read(out_pairs_buf, pairs_readback, match_count * 4 * sizeof(U32));
  }
  if (out_trace) out_trace->probe_download_time_us = os_now_microseconds() - download_start_us;
  
  gpu_kernel_release(probe_kernel);
  
  if (out_trace)
  {
    out_trace->build_row_count = build_row_count;
    out_trace->probe_row_count = probe_row_count;
    out_trace->output_row_count = match_count;
    log_info("qe_hash_join: build_row_count=%llu probe_row_count=%llu phases (us): build=%llu probe_dispatch=%llu probe_download=%llu",
             build_row_count, probe_row_count, out_trace->build_time_us, out_trace->probe_dispatch_time_us, out_trace->probe_download_time_us);
  }
  
  // tec: expand (probe_array_index, build_row) pairs into the final multi-table row set
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
