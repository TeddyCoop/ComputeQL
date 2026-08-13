
internal void
qe_bytecode_emit(QE_BytecodeProgram* prog, U32 word)
{
  if (prog->word_count >= QE_BYTECODE_MAX_WORDS)
  {
    log_error("qe_bytecode_emit: bytecode program exceeded max word count (%u)", (U32)QE_BYTECODE_MAX_WORDS);
    return;
  }
  prog->words[prog->word_count++] = word;
}

internal U32
qe_add_numeric_const(QE_BytecodeProgram* prog, F64 value)
{
  if (prog->const_count >= QE_MAX_NUMERIC_CONSTS)
  {
    log_error("qe_add_numeric_const: exceeded max numeric const count (%u)", (U32)QE_MAX_NUMERIC_CONSTS);
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

typedef struct QE_StringConstRef QE_StringConstRef;
struct QE_StringConstRef
{
  U32 word_offset;
  U32 byte_len;
};

internal QE_StringConstRef
qe_add_string_const(QE_BytecodeProgram* prog, String8 str)
{
  QE_StringConstRef ref = {0};
  
  // tec: keep every string constant on a 4-byte boundary so word_offset*4 == byte offset
  U64 aligned_offset = AlignPow2(prog->str_const_pool_size, 4);
  
  if (aligned_offset + str.size > QE_STRING_CONST_POOL_SIZE)
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
qe_compile_condition(QE_BytecodeProgram* prog, GDB_Table* table, IR_Node* condition)
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
    qe_compile_condition(prog, table, left);
    qe_compile_condition(prog, table, right);
    qe_bytecode_emit(prog, QE_Opcode_And);
    return;
  }
  else if (str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    qe_compile_condition(prog, table, left);
    qe_compile_condition(prog, table, right);
    qe_bytecode_emit(prog, QE_Opcode_Or);
    return;
  }
  
  if (!left || !right)
  {
    log_error("qe_compile_condition: malformed comparison, missing operand(s)");
    qe_bytecode_emit(prog, QE_Opcode_PushTrue);
    return;
  }
  
  // tec: string comparisons
  // column op 'literal', where the column is a String8 column.
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
      
      QE_StringConstRef ref = qe_add_string_const(prog, right->value);
      
      qe_bytecode_emit(prog, is_contains ? QE_Opcode_StrContains : QE_Opcode_StrEq);
      qe_bytecode_emit(prog, binding->first_slot);
      qe_bytecode_emit(prog, ref.word_offset);
      qe_bytecode_emit(prog, ref.byte_len);
      return;
    }
  }
  
  // tec: generic numeric comparison
  qe_compile_load_value(prog, table, left);
  qe_compile_load_value(prog, table, right);
  qe_bytecode_emit(prog, qe_opcode_from_comparison_operator(op));
}

internal void
qe_bytecode_program_build(QE_BytecodeProgram* prog, GDB_Database* database, GDB_Table* table, IR_Node* root_node, IR_Node* where_clause)
{
  MemoryZeroStruct(prog);
  
  if (where_clause && where_clause->first)
  {
    qe_compile_condition(prog, table, where_clause->first);
  }
  else
  {
    qe_bytecode_emit(prog, QE_Opcode_PushTrue);
  }
  
  qe_bytecode_emit(prog, QE_Opcode_Halt);
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
    
    if (binding->type == GDB_ColumnType_String8)
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

internal QE_ScanResult
qe_scan_filter(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* where_clause)
{
  ProfBeginFunction();

  QE_ScanResult result = {0};

  QE_BytecodeProgram* prog = push_array(arena, QE_BytecodeProgram, 1);
  qe_bytecode_program_build(prog, database, table, NULL, where_clause);

  GPU_Kernel* kernel = gpu_kernel_alloc(str8_lit("scan_filter"));
  if (!kernel)
  {
    log_error("qe_scan_filter: failed to alloc 'scan_filter' kernel");
    ProfEnd();
    return result;
  }
  
  //- tec: upload the (query-invariant) bytecode + constant pool buffers once, outside the chunk loop
  GPU_Buffer* bytecode_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_bytecode"), Max(prog->word_count, 1) * sizeof(U32), GPU_BufferFlag_Write, 0);
  gpu_buffer_write(bytecode_buffer, prog->words, prog->word_count * sizeof(U32));

  GPU_Buffer* num_consts_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_num_consts"), Max(prog->const_count * 2, 1) * sizeof(U32), GPU_BufferFlag_Write, 0);
  if (prog->const_count > 0)
  {
    gpu_buffer_write(num_consts_buffer, prog->consts, prog->const_count * 2 * sizeof(U32));
  }

  GPU_Buffer* str_consts_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_str_consts"), Max(prog->str_const_pool_size, 4), GPU_BufferFlag_Write, 0);
  if (prog->str_const_pool_size > 0)
  {
    gpu_buffer_write(str_consts_buffer, prog->str_const_pool, prog->str_const_pool_size);
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
  
  B32 needs_chunking = largest_column_size > GPU_MAX_BUFFER_SIZE;
  
  U64 rows_per_chunk = table->row_count;
  U64 chunk_count = 1;
  
  if (needs_chunking)
  {
    U64 row_size = 0;
    for (U32 i = 0; i < prog->binding_count; i++)
    {
      row_size += table->row_count ? (gdb_column_get_total_size(prog->bindings[i].column) / table->row_count) : 0;
    }
    if (row_size == 0) row_size = 1;
    
    rows_per_chunk = GPU_MAX_BUFFER_SIZE / row_size;
    if (rows_per_chunk == 0) rows_per_chunk = 1;
    
    chunk_count = (table->row_count + rows_per_chunk - 1) / rows_per_chunk;
  }
  
  // tec: only worth a background thread + two extra arenas when there's a next chunk to hide IO for the common single-chunk case
  QE_PrefetchCtx* prefetch = (chunk_count > 1) ? qe_prefetch_start(arena, prog) : 0;
  
  for (U64 chunk_index = 0; chunk_index < chunk_count; chunk_index++)
  {
    Temp fallback_arena = {0};
    B32 using_prefetch = (prefetch != 0);
    
    U64 chunk_row_start = chunk_index * rows_per_chunk;
    U64 chunk_rows = needs_chunking ? Min(rows_per_chunk, table->row_count - chunk_row_start) : table->row_count;
    Rng1U64 chunk_range = r1u64(chunk_row_start, chunk_row_start + chunk_rows);
    
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

    for (U32 i = 0; i < prog->binding_count; i++)
    {
      QE_ColumnBinding* binding = &prog->bindings[i];
      QE_PrefetchBindingResult* in = &slot->bindings[i];
      U32 descriptor_binding = QE_BINDING_COLUMN_BASE + binding->first_slot;
      
      String8 col_pool_key = push_str8f(g_vulkan_state->arena, "scan_col:%.*s.%.*s", str8_varg(table->name), str8_varg(binding->name));

      if (binding->type == GDB_ColumnType_String8)
      {
        if (in->valid)
        {
          String8 data_key = push_str8f(g_vulkan_state->arena, "%.*s.data", str8_varg(col_pool_key));
          GPU_Buffer* data_buf = gpu_buffer_alloc_pooled(data_key, in->str_chunk.size, GPU_BufferFlag_Write | GPU_BufferFlag_HostVisible, in->str_chunk.data);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 0, data_buf);

          // tec: +1 row for the trailing offset used to compute the last strings size
          U64 offsets_size = (in->str_chunk.row_count + 1) * sizeof(U64);
          String8 offsets_key = push_str8f(g_vulkan_state->arena, "%.*s.offsets", str8_varg(col_pool_key));
          GPU_Buffer* offsets_buf = gpu_buffer_alloc_pooled(offsets_key, offsets_size, GPU_BufferFlag_Write | GPU_BufferFlag_CopyHostPointer, in->str_chunk.offsets);
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 1, offsets_buf);
        }
        else
        {
          log_error("qe_scan_filter: failed to load string data/offsets for column '%.*s'", str8_varg(binding->name));
        }

        // tec: safe to close now, the data's already been copyed into the GPU buffer above
        gdb_column_close_string_chunk(binding->column);
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
          String8 fallback_key = push_str8f(g_vulkan_state->arena, "%.*s.alloc_fallback", str8_varg(col_pool_key));
          data_buf = gpu_buffer_alloc_pooled(fallback_key, in->size, GPU_BufferFlag_Write, in->data_ptr);
        }
        gpu_kernel_set_arg_buffer(kernel, descriptor_binding, data_buf);
      }
    }
    
    GPU_Buffer* output_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_output"), Max(chunk_rows, 1) * 2 * sizeof(U32), GPU_BufferFlag_Read, 0);
    U32 zero_count[2] = {0, 0};
    GPU_Buffer* result_counter_buffer = gpu_buffer_alloc_pooled(str8_lit("scan_filter_result_counter"), sizeof(zero_count), GPU_BufferFlag_ReadWrite | GPU_BufferFlag_HostCached, zero_count);
    buffer_alloc_time += os_now_microseconds() - buffer_alloc_start;
    ProfEnd();

    gpu_kernel_set_arg_buffer(kernel, QE_BINDING_OUT_INDICES, output_buffer);
    gpu_kernel_set_arg_buffer(kernel, QE_BINDING_OUT_COUNT, result_counter_buffer);
    gpu_kernel_set_arg_u64(kernel, QE_PUSH_CONSTANT_ROW_COUNT, chunk_rows);
    
    // tec: this chunk's columns are now fully read+uploaded+closed
    // so its safe to kick off the next chunk's read in the background
    if (using_prefetch && chunk_index + 1 < chunk_count)
    {
      U64 next_row_start = (chunk_index + 1) * rows_per_chunk;
      U64 next_rows = Min(rows_per_chunk, table->row_count - next_row_start);
      Rng1U64 next_range = r1u64(next_row_start, next_row_start + next_rows);
      qe_prefetch_request(prefetch, (U32)((chunk_index + 1) % 2), next_range, next_rows);
    }
    
    U64 submit_wait_start = os_now_microseconds();
    gpu_kernel_execute(kernel, (U32)chunk_rows, QE_GPU_WORKGROUP_SIZE);
    gpu_kernel_execution_time += gpu_get_executed_kernel_time_microseconds();
    submit_wait_time += os_now_microseconds() - submit_wait_start;

    U32 result_count32[2] = {0, 0};
    gpu_buffer_read(result_counter_buffer, result_count32, sizeof(result_count32));
    U64 result_count = result_count32[0];
    
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
  
  switch (column->type)
  {
    case GDB_ColumnType_U32: return (F64)(*(U32*)data);
    case GDB_ColumnType_U64: return (F64)(*(U64*)data);
    case GDB_ColumnType_F32: return (F64)(*(F32*)data);
    case GDB_ColumnType_F64: return *(F64*)data;
    default: return 0.0;
  }
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

  for (U64 i = 0; i < rows->count; i++)
  {
    U64 row = table_rows[i];
    if (row == PLAN_NULL_ROW || !base_ptr)
    {
      values[i] = 0.0;
      continue;
    }

    void* data = (U8*)base_ptr + (row - min_row) * column->size;
    switch (column->type)
    {
      case GDB_ColumnType_U32: values[i] = (F64)(*(U32*)data); break;
      case GDB_ColumnType_U64: values[i] = (F64)(*(U64*)data); break;
      case GDB_ColumnType_F32: values[i] = (F64)(*(F32*)data); break;
      case GDB_ColumnType_F64: values[i] = *(F64*)data; break;
      default: values[i] = 0.0; break;
    }
  }

  ProfEnd();
  return values;
}

internal GDB_StringDataChunk
qe_gather_string_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column)
{
  ProfBeginFunction();

  GDB_StringDataChunk chunk = {0};
  chunk.row_count = rows->count;
  chunk.offsets = push_array(arena, U64, rows->count + 1);

  if (table_slot >= rows->table_count)
  {
    log_error("qe_gather_string_column: slot %llu is not part of this row set", table_slot);
    ProfEnd();
    return chunk;
  }

  Temp scratch = scratch_begin(&arena, 1);
  String8* strs = push_array(scratch.arena, String8, Max(rows->count, 1));
  U64* table_rows = rows->row_indices[table_slot];
  U64 total_size = 0;

  U64 min_row = max_U64;
  U64 max_row = 0;
  for (U64 i = 0; i < rows->count; i++)
  {
    U64 row = table_rows[i];
    if (row == PLAN_NULL_ROW) continue;
    if (row < min_row) min_row = row;
    if (row > max_row) max_row = row;
  }

  GDB_StringDataChunk src = {0};
  if (min_row != max_U64)
  {
    src = gdb_column_get_string_chunk(scratch.arena, column, r1u64(min_row, max_row + 1));
  }

  for (U64 i = 0; i < rows->count; i++)
  {
    U64 row = table_rows[i];
    String8 s = {0};
    if (row != PLAN_NULL_ROW && src.data)
    {
      U64 local = row - min_row;
      U64 start = src.offsets[local];
      U64 end = src.offsets[local + 1];
      s.str = (U8*)src.data + start;
      s.size = end - start;
    }
    strs[i] = s;
    total_size += s.size;
  }

  U8* data = push_array(arena, U8, Max(total_size, 1));
  U64 cursor = 0;
  chunk.offsets[0] = 0;
  for (U64 i = 0; i < rows->count; i++)
  {
    MemoryCopy(data + cursor, strs[i].str, strs[i].size);
    cursor += strs[i].size;
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
qe_sort_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* order_by_ir)
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
  
  F64* keys = push_array(scratch.arena, F64, padded_count * QE_SORT_MAX_KEYS);
  U32* payload = push_array(scratch.arena, U32, padded_count * 9);
  
  for (U64 i = 0; i < padded_count; i++)
  {
    B32 is_real = i < real_count;
    
    for (U32 k = 0; k < QE_SORT_MAX_KEYS; k++)
    {
      keys[i * QE_SORT_MAX_KEYS + k] = (is_real && k < num_keys) ? gathered[k][i] : 0.0;
    }
    
    for (U32 t = 0; t < QE_SORT_MAX_TABLES; t++)
    {
      U64 row = (is_real && t < rows->table_count) ? rows->row_indices[t][i] : 0;
      payload[i * 9 + t * 2 + 0] = (U32)(row & max_U32);
      payload[i * 9 + t * 2 + 1] = (U32)(row >> 32);
    }
    payload[i * 9 + 8] = is_real ? 1u : 0u;
  }
  
  GPU_Kernel* kernel = gpu_kernel_alloc(str8_lit("bitonic_sort"));
  if (!kernel)
  {
    log_error("qe_sort_rows: failed to alloc 'bitonic_sort' kernel, returning unsorted");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  GPU_Buffer* keys_buf = gpu_buffer_alloc(padded_count * QE_SORT_MAX_KEYS * sizeof(F64), GPU_BufferFlag_ReadWrite, keys);
  GPU_Buffer* payload_buf = gpu_buffer_alloc(padded_count * 9 * sizeof(U32), GPU_BufferFlag_ReadWrite, payload);
  
  gpu_kernel_set_arg_buffer(kernel, 0, keys_buf);
  gpu_kernel_set_arg_buffer(kernel, 1, payload_buf);
  
  U32 num_stages = 0;
  while ((1ull << num_stages) < padded_count) num_stages++;
  
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
      
      gpu_kernel_execute(kernel, (U32)(padded_count / 2), QE_GPU_WORKGROUP_SIZE);
    }
  }
  
  U32* sorted_payload = push_array(scratch.arena, U32, padded_count * 9);
  gpu_buffer_read(payload_buf, sorted_payload, padded_count * 9 * sizeof(U32));
  
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
  U32 func_code;          // 0 COUNT, 1 SUM, 2 AVG, 3 MIN, 4 MAX
  GDB_Table* arg_table;   // NULL for COUNT(*)/COUNT(col) - counting never needs the argument's value
  U64 arg_slot;           // tec: input row-set slot arg_table was resolved to (see qe_resolve_column_table)
  GDB_Column* arg_column;
};

internal U32
qe_agg_func_code_from_name(String8 name)
{
  if (str8_match(name, str8_lit("count"), StringMatchFlag_CaseInsensitive)) return 0;
  if (str8_match(name, str8_lit("sum"),   StringMatchFlag_CaseInsensitive)) return 1;
  if (str8_match(name, str8_lit("avg"),   StringMatchFlag_CaseInsensitive)) return 2;
  if (str8_match(name, str8_lit("min"),   StringMatchFlag_CaseInsensitive)) return 3;
  if (str8_match(name, str8_lit("max"),   StringMatchFlag_CaseInsensitive)) return 4;
  
  log_error("qe_aggregate: unsupported aggregate function '%.*s', defaulting to COUNT", str8_varg(name));
  return 0;
}

internal void
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
          info->func_code = qe_agg_func_code_from_name(n->value);
          
          IR_Node* arg = n->first;
          B32 is_star = (!arg) || str8_match(arg->value, str8_lit("*"), 0);
          
          if (is_star || info->func_code == 0)
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
          
          (*num_exprs)++;
        }
      }
    }
    
    qe_aggregate_collect_exprs(arena, input, n->first, exprs, num_exprs);
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
      dst->type = (exprs[e].func_code == 0) ? GDB_ColumnType_U64 : GDB_ColumnType_F64;
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
      
      if (column->type == GDB_ColumnType_String8)
      {
        dst->string_values = push_array(arena, String8, Max(num_groups, 1));
        for (U64 g = 0; g < num_groups; g++)
        {
          U64 dense_idx = representative_readback[g];
          U64 base_row = (dense_idx < input->count) ? input->row_indices[table_slot][dense_idx] : PLAN_NULL_ROW;
          dst->string_values[g] = (base_row == PLAN_NULL_ROW) ? str8_lit("") : gdb_column_get_string(arena, column, base_row);
        }
      }
      else
      {
        dst->numeric_values = push_array(arena, F64, Max(num_groups, 1));
        for (U64 g = 0; g < num_groups; g++)
        {
          U64 dense_idx = representative_readback[g];
          U64 base_row = (dense_idx < input->count) ? input->row_indices[table_slot][dense_idx] : PLAN_NULL_ROW;
          dst->numeric_values[g] = (base_row == PLAN_NULL_ROW) ? 0.0 : qe_read_numeric_as_f64(column, base_row);
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
    dst->type = (exprs[e].func_code == 0) ? GDB_ColumnType_U64 : GDB_ColumnType_F64;
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
qe_aggregate(Arena* arena, GDB_Database* database, PLAN_RowSet* input, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir)
{
  ProfBeginFunction();
  
  PLAN_Materialized result = {0};
  U64 row_count = input->count;
  
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
  qe_aggregate_collect_exprs(arena, input, column_list_ir ? column_list_ir->first : NULL, exprs, &num_exprs);
  qe_aggregate_collect_exprs(arena, input, having_ir ? having_ir->first : NULL, exprs, &num_exprs);
  
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
      group_string[c] = qe_gather_string_column(arena, input, group_slots[c], group_columns[c]);
      group_string_mask |= (1u << c);
    }
    else
    {
      group_numeric[c] = qe_gather_numeric_column(arena, input, group_slots[c], group_columns[c]);
    }
  }

  //- tec: gather aggregate argument columns (dense F64, one per expr that needs a real column)
  F64* expr_args[QE_AGG_MAX_EXPRS] = {0};
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (exprs[e].arg_column)
    {
      expr_args[e] = qe_gather_numeric_column(arena, input, exprs[e].arg_slot, exprs[e].arg_column);
    }
  }
  
  //- tec: size the open-addressing hash table
  U64 num_buckets, K;
  if (num_group_cols == 0)
  {
    num_buckets = 1;
    K = 1;
  }
  else
  {
    num_buckets = 16;
    while (num_buckets < row_count) num_buckets <<= 1;
    K = 8;
  }
  U64 num_slots = num_buckets * K;
  
  if (num_slots * sizeof(U32) > GPU_MAX_BUFFER_SIZE)
  {
    log_error("qe_aggregate: group-by hash table too large for a single GPU buffer (row_count=%llu) - "
              "chunked cross-bucket aggregation isn't supported yet", row_count);
    ProfEnd();
    return result;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  U32* owner_init = push_array(scratch.arena, U32, num_slots);
  for (U64 i = 0; i < num_slots; i++) owner_init[i] = max_U32;
  U32* count_init = push_array(scratch.arena, U32, num_slots); // tec: push_array zero-inits
  
  GPU_Kernel* assign_kernel = gpu_kernel_alloc(str8_lit("aggregate_assign"));
  if (!assign_kernel)
  {
    log_error("qe_aggregate: failed to alloc 'aggregate_assign' kernel");
    scratch_end(scratch);
    ProfEnd();
    return result;
  }
  
  GPU_Buffer* owner_buf = gpu_buffer_alloc(num_slots * sizeof(U32), GPU_BufferFlag_ReadWrite, owner_init);
  GPU_Buffer* count_buf = gpu_buffer_alloc(num_slots * sizeof(U32), GPU_BufferFlag_ReadWrite, count_init);
  GPU_Buffer* row_slot_buf = gpu_buffer_alloc(row_count * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  U32 zero1[1] = {0};
  GPU_Buffer* overflow_buf = gpu_buffer_alloc(sizeof(U32), GPU_BufferFlag_ReadWrite, zero1);
  
  gpu_kernel_set_arg_buffer(assign_kernel, 0, owner_buf);
  gpu_kernel_set_arg_buffer(assign_kernel, 1, count_buf);
  gpu_kernel_set_arg_buffer(assign_kernel, 2, row_slot_buf);
  gpu_kernel_set_arg_buffer(assign_kernel, 3, overflow_buf);
  
  GPU_Buffer* group_col_bufs[QE_AGG_MAX_GROUP_COLS * 2] = {0};
  for (U32 c = 0; c < num_group_cols; c++)
  {
    if (group_string_mask & (1u << c))
    {
      GPU_Buffer* data_buf = gpu_buffer_alloc(Max(group_string[c].size, 4), GPU_BufferFlag_Write, group_string[c].data);
      GPU_Buffer* off_buf = gpu_buffer_alloc((row_count + 1) * sizeof(U64), GPU_BufferFlag_Write, group_string[c].offsets);
      group_col_bufs[c * 2 + 0] = data_buf;
      group_col_bufs[c * 2 + 1] = off_buf;
    }
    else
    {
      GPU_Buffer* data_buf = gpu_buffer_alloc(row_count * sizeof(F64), GPU_BufferFlag_Write, group_numeric[c]);
      group_col_bufs[c * 2 + 0] = data_buf;
    }
    gpu_kernel_set_arg_buffer(assign_kernel, 4 + c * 2, group_col_bufs[c * 2 + 0]);
    if (group_col_bufs[c * 2 + 1]) gpu_kernel_set_arg_buffer(assign_kernel, 4 + c * 2 + 1, group_col_bufs[c * 2 + 1]);
  }
  
  gpu_kernel_set_arg_u64(assign_kernel, 0, row_count);
  gpu_kernel_set_arg_u64(assign_kernel, 1, num_buckets);
  gpu_kernel_set_arg_u64(assign_kernel, 2, K);
  gpu_kernel_set_arg_u64(assign_kernel, 3, num_group_cols);
  gpu_kernel_set_arg_u64(assign_kernel, 4, group_string_mask);
  
  gpu_kernel_execute(assign_kernel, (U32)row_count, QE_GPU_WORKGROUP_SIZE);
  
  U32* owner_readback = push_array(scratch.arena, U32, num_slots);
  U32* count_readback = push_array(scratch.arena, U32, num_slots);
  gpu_buffer_read(owner_buf, owner_readback, num_slots * sizeof(U32));
  gpu_buffer_read(count_buf, count_readback, num_slots * sizeof(U32));
  
  U32 overflow_readback = 0;
  gpu_buffer_read(overflow_buf, &overflow_readback, sizeof(U32));
  if (overflow_readback > 0)
  {
    log_error("qe_aggregate: %u row(s) dropped due to hash-table probe exhaustion - GROUP BY result is incomplete", overflow_readback);
  }
  
  gpu_buffer_release(owner_buf);
  gpu_buffer_release(count_buf);
  gpu_buffer_release(overflow_buf);
  for (U32 c = 0; c < num_group_cols; c++)
  {
    if (group_col_bufs[c * 2 + 0]) gpu_buffer_release(group_col_bufs[c * 2 + 0]);
    if (group_col_bufs[c * 2 + 1]) gpu_buffer_release(group_col_bufs[c * 2 + 1]);
  }
  gpu_kernel_release(assign_kernel);
  
  // tec: prefix-sum slot_row_count -> slot_offsets, and (for GROUP BY) compact occupied slots
  // into group_ with no GROUP BY, slot 0 is always the one group regardless of occupancy
  U32* slot_offsets = push_array(scratch.arena, U32, num_slots + 1);
  U32 running = 0;
  for (U64 s = 0; s < num_slots; s++)
  {
    slot_offsets[s] = running;
    running += count_readback[s];
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
    num_groups = 0;
    for (U64 s = 0; s < num_slots; s++) if (owner_readback[s] != max_U32) num_groups++;
    
    group_ids = push_array(scratch.arena, U32, Max(num_groups, 1));
    U64 gi = 0;
    for (U64 s = 0; s < num_slots; s++) if (owner_readback[s] != max_U32) group_ids[gi++] = (U32)s;
  }
  
  //- tec: pass 2/3 - scatter rows into per slot CSR member lists (csr_scatter.comp, shared with qe_hash_join)
  GPU_Kernel* scatter_kernel = gpu_kernel_alloc(str8_lit("csr_scatter"));
  GPU_Buffer* cursor_buf = gpu_buffer_alloc(num_slots * sizeof(U32), GPU_BufferFlag_ReadWrite, slot_offsets);
  GPU_Buffer* members_buf = gpu_buffer_alloc(Max(row_count, 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  
  gpu_kernel_set_arg_buffer(scatter_kernel, 0, row_slot_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 1, cursor_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 2, members_buf);
  gpu_kernel_set_arg_u64(scatter_kernel, 0, row_count);
  
  gpu_kernel_execute(scatter_kernel, (U32)row_count, QE_GPU_WORKGROUP_SIZE);
  
  gpu_buffer_release(row_slot_buf);
  gpu_buffer_release(cursor_buf);
  gpu_kernel_release(scatter_kernel);
  
  //- tec: pass 3/3 - one thread per group, serial reduction over its own member rows
  GPU_Kernel* reduce_kernel = gpu_kernel_alloc(str8_lit("aggregate_reduce"));
  
  GPU_Buffer* offsets_buf = gpu_buffer_alloc((num_slots + 1) * sizeof(U32), GPU_BufferFlag_Write, slot_offsets);
  GPU_Buffer* group_ids_buf = gpu_buffer_alloc(Max(num_groups, 1) * sizeof(U32), GPU_BufferFlag_Write, group_ids);
  GPU_Buffer* repr_buf = gpu_buffer_alloc(Max(num_groups, 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* results_buf = gpu_buffer_alloc(Max(num_groups * Max(num_exprs, 1), 1) * sizeof(F64), GPU_BufferFlag_ReadWrite, 0);
  
  gpu_kernel_set_arg_buffer(reduce_kernel, 0, members_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 1, offsets_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 2, group_ids_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 3, repr_buf);
  gpu_kernel_set_arg_buffer(reduce_kernel, 4, results_buf);
  
  GPU_Buffer* arg_bufs[QE_AGG_MAX_EXPRS] = {0};
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (expr_args[e])
    {
      arg_bufs[e] = gpu_buffer_alloc(row_count * sizeof(F64), GPU_BufferFlag_Write, expr_args[e]);
      gpu_kernel_set_arg_buffer(reduce_kernel, 5 + e, arg_bufs[e]);
    }
  }
  
  U32 func_codes_packed = 0;
  for (U32 e = 0; e < num_exprs; e++) func_codes_packed |= (exprs[e].func_code & 0xfu) << (e * 4u);
  
  gpu_kernel_set_arg_u64(reduce_kernel, 0, num_groups);
  gpu_kernel_set_arg_u64(reduce_kernel, 1, num_exprs);
  gpu_kernel_set_arg_u64(reduce_kernel, 2, func_codes_packed);
  
  gpu_kernel_execute(reduce_kernel, (U32)Max(num_groups, 1), QE_GPU_WORKGROUP_SIZE);
  
  U64* representative_readback = push_array(scratch.arena, U64, Max(num_groups, 1));
  {
    U32* repr32 = push_array(scratch.arena, U32, Max(num_groups, 1));
    gpu_buffer_read(repr_buf, repr32, Max(num_groups, 1) * sizeof(U32));
    for (U64 g = 0; g < num_groups; g++) representative_readback[g] = (repr32[g] == max_U32) ? max_U64 : repr32[g];
  }
  
  F64* results_readback = push_array(scratch.arena, F64, Max(num_groups * Max(num_exprs, 1), 1));
  gpu_buffer_read(results_buf, results_readback, Max(num_groups * Max(num_exprs, 1), 1) * sizeof(F64));
  
  gpu_buffer_release(members_buf);
  gpu_buffer_release(offsets_buf);
  gpu_buffer_release(group_ids_buf);
  gpu_buffer_release(repr_buf);
  gpu_buffer_release(results_buf);
  for (U32 e = 0; e < num_exprs; e++) if (arg_bufs[e]) gpu_buffer_release(arg_bufs[e]);
  gpu_kernel_release(reduce_kernel);
  
  result = qe_aggregate_build_output(arena, input, column_list_ir, exprs, num_exprs, num_groups, representative_readback, results_readback);
  
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
  F64 rv = qe_having_load_value(m, right, row, &rstr, &rs);
  
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
typedef struct QE_IndexScanCtx QE_IndexScanCtx;
struct QE_IndexScanCtx
{
  B32 is_string;
  F64* numeric_keys; // tec: dense, indexed by row (0..row_count-1)
  GDB_StringDataChunk string_keys;
};

global QE_IndexScanCtx* g_qe_index_scan_ctx = 0;

internal int
qe_index_scan_compare(const void* a, const void* b)
{
  U64 ra = *(const U64*)a;
  U64 rb = *(const U64*)b;
  QE_IndexScanCtx* ctx = g_qe_index_scan_ctx;

  if (ctx->is_string)
  {
    GDB_StringDataChunk* chunk = &ctx->string_keys;
    String8 sa = str8((U8*)chunk->data + chunk->offsets[ra], chunk->offsets[ra + 1] - chunk->offsets[ra]);
    String8 sb = str8((U8*)chunk->data + chunk->offsets[rb], chunk->offsets[rb + 1] - chunk->offsets[rb]);
    return qe_str8_compare(sa, sb);
  }

  F64 va = ctx->numeric_keys[ra];
  F64 vb = ctx->numeric_keys[rb];
  return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

internal S32
qe_index_row_cmp_target(QE_IndexScanCtx* ctx, U64 row, F64 target_numeric, String8 target_string)
{
  if (ctx->is_string)
  {
    U64 s = ctx->string_keys.offsets[row], e = ctx->string_keys.offsets[row + 1];
    String8 sv = str8((U8*)ctx->string_keys.data + s, e - s);
    return qe_str8_compare(sv, target_string);
  }
  F64 v = ctx->numeric_keys[row];
  return (v < target_numeric) ? -1 : (v > target_numeric) ? 1 : 0;
}

// tec: first index i in order[lo..count) such that order[i]'s key >= target (standard lower_bound)
internal U64
qe_index_lower_bound(QE_IndexScanCtx* ctx, U64* order, U64 count, F64 target_numeric, String8 target_string)
{
  U64 lo = 0, hi = count;
  while (lo < hi)
  {
    U64 mid = lo + (hi - lo) / 2;
    if (qe_index_row_cmp_target(ctx, order[mid], target_numeric, target_string) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// tec: first index i such that order[i]'s key > target (standard upper_bound)
internal U64
qe_index_upper_bound(QE_IndexScanCtx* ctx, U64* order, U64 count, F64 target_numeric, String8 target_string)
{
  U64 lo = 0, hi = count;
  while (lo < hi)
  {
    U64 mid = lo + (hi - lo) / 2;
    if (qe_index_row_cmp_target(ctx, order[mid], target_numeric, target_string) <= 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

internal B32
qe_try_index_scan(Arena* arena, GDB_Table* table, IR_Node* where_clause, QE_ScanResult* out_result)
{
  if (!where_clause || !where_clause->first) return 0;

  IR_Node* condition = where_clause->first;
  if (condition->type != IR_NodeType_Operator) return 0;

  String8 op = condition->value;
  B32 is_eq = str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0);
  B32 is_lt = str8_match(op, str8_lit("<"), 0);
  B32 is_le = str8_match(op, str8_lit("<="), 0);
  B32 is_gt = str8_match(op, str8_lit(">"), 0);
  B32 is_ge = str8_match(op, str8_lit(">="), 0);
  if (!(is_eq || is_lt || is_le || is_gt || is_ge)) return 0;

  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;
  if (!left || !right || left->type != IR_NodeType_Column) return 0;
  if (right->type != IR_NodeType_Numeric && right->type != IR_NodeType_Literal) return 0;

  GDB_Column* column = gdb_table_find_column(table, qe_bare_column_name(left->value));
  if (!column) return 0;

  GDB_Index* index = gdb_table_find_index_on_column(table, column);
  if (!index) return 0;

  B32 is_string_key = (column->type == GDB_ColumnType_String8);
  if (is_string_key != (right->type == IR_NodeType_Literal)) return 0; // tec: type mismatch, dont guess

  U64 row_count = table->row_count;
  if (row_count == 0)
  {
    out_result->indices = push_array(arena, U64, 1);
    out_result->count = 0;
    return 1;
  }

  Temp scratch = scratch_begin(&arena, 1);

  QE_IndexScanCtx ctx = {0};
  ctx.is_string = is_string_key;

  if (is_string_key)
  {
    ctx.string_keys = gdb_column_get_string_chunk(scratch.arena, column, r1u64(0, row_count));
  }
  else
  {
    U64 range_size = 0;
    void* base_ptr = gdb_column_get_data_range(scratch.arena, column, r1u64(0, row_count), &range_size);
    F64* numeric_keys = push_array(scratch.arena, F64, row_count);
    for (U64 i = 0; i < row_count; i++)
    {
      void* data = (U8*)base_ptr + i * column->size;
      switch (column->type)
      {
        case GDB_ColumnType_U32: numeric_keys[i] = (F64)(*(U32*)data); break;
        case GDB_ColumnType_U64: numeric_keys[i] = (F64)(*(U64*)data); break;
        case GDB_ColumnType_F32: numeric_keys[i] = (F64)(*(F32*)data); break;
        case GDB_ColumnType_F64: numeric_keys[i] = *(F64*)data; break;
        default: numeric_keys[i] = 0.0; break;
      }
    }
    ctx.numeric_keys = numeric_keys;
  }

  U64* order = push_array(scratch.arena, U64, row_count);
  for (U64 i = 0; i < row_count; i++) order[i] = i;

  QE_IndexScanCtx* prev_ctx = g_qe_index_scan_ctx;
  g_qe_index_scan_ctx = &ctx;
  quick_sort(order, row_count, sizeof(U64), qe_index_scan_compare);
  g_qe_index_scan_ctx = prev_ctx;

  F64 target_numeric = is_string_key ? 0.0 : f64_from_str8(right->value);
  String8 target_string = is_string_key ? right->value : (String8){0};

  U64 range_lo = 0, range_hi = 0;
  if (is_eq)
  {
    range_lo = qe_index_lower_bound(&ctx, order, row_count, target_numeric, target_string);
    range_hi = qe_index_upper_bound(&ctx, order, row_count, target_numeric, target_string);
  }
  else if (is_lt)
  {
    range_lo = 0;
    range_hi = qe_index_lower_bound(&ctx, order, row_count, target_numeric, target_string);
  }
  else if (is_le)
  {
    range_lo = 0;
    range_hi = qe_index_upper_bound(&ctx, order, row_count, target_numeric, target_string);
  }
  else if (is_gt)
  {
    range_lo = qe_index_upper_bound(&ctx, order, row_count, target_numeric, target_string);
    range_hi = row_count;
  }
  else // tec: is_ge
  {
    range_lo = qe_index_lower_bound(&ctx, order, row_count, target_numeric, target_string);
    range_hi = row_count;
  }

  U64 match_count = range_hi - range_lo;
  out_result->indices = push_array(arena, U64, Max(match_count, 1));
  out_result->count = match_count;
  for (U64 i = 0; i < match_count; i++)
  {
    out_result->indices[i] = order[range_lo + i];
  }

  scratch_end(scratch);
  return 1;
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
  if (!condition || condition->type != IR_NodeType_Operator) return NULL;
  if (!(str8_match(condition->value, str8_lit("="), 0) || str8_match(condition->value, str8_lit("=="), 0))) return NULL;

  IR_Node* left = condition->first;
  IR_Node* right = left ? left->next : NULL;
  if (!left || !right || left->type != IR_NodeType_Column || right->type != IR_NodeType_Column) return NULL;

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
  if (!condition) return NULL;

  if (condition->type == IR_NodeType_Operator && str8_match(condition->value, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    IR_Node* left = condition->first;
    IR_Node* right = left ? left->next : NULL;

    IR_Node* found = qe_find_equi_condition(left_rows, right_table, right_alias, left);
    if (found) return found;
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
    if (!table) return 0.0;

    U64 row = rows->row_indices[slot][output_row];
    if (row == PLAN_NULL_ROW) { *out_is_null = 1; return 0.0; }
    
    GDB_Column* column = gdb_table_find_column(table, bare);
    if (!column) return 0.0;
    
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
    if (is_null) return 0;
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
  
  if (!left || !right)
  {
    log_error("qe_row_condition_eval: malformed comparison, missing operand(s)");
    return 1;
  }
  
  B32 lstr = 0, rstr = 0, lnull = 0, rnull = 0;
  String8 ls = {0}, rs = {0};
  F64 lv = qe_row_load_value(arena, rows, left, output_row, &lstr, &ls, &lnull);
  F64 rv = qe_row_load_value(arena, rows, right, output_row, &rstr, &rs, &rnull);
  
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
qe_hash_join(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias, String8 join_type, IR_Node* condition)
{
  ProfBeginFunction();
  PLAN_RowSet result = {0};

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
  
  U64 build_row_count = right_table->row_count;
  U64 probe_row_count = left->count;
  B32 is_left_join = str8_match(join_type, str8_lit("left"), StringMatchFlag_CaseInsensitive);
  
  U64 num_buckets = 1;
  while (num_buckets < build_row_count) num_buckets <<= 1;
  
  if (num_buckets * sizeof(U32) > GPU_MAX_BUFFER_SIZE)
  {
    log_error("qe_hash_join: build-side table too large for a single GPU hash table (row_count=%llu) - "
              "chunked join isn't supported yet", build_row_count);
    ProfEnd();
    return result;
  }
  
  Temp scratch = scratch_begin(&arena, 1);
  
  //- tec: build-side key data
  void* build_data = NULL;
  U64* build_offsets = NULL;
  U64 build_data_size = 4;
  
  if (is_string_key)
  {
    GDB_StringDataChunk chunk = gdb_column_get_string_chunk(scratch.arena, right_key_column, r1u64(0, build_row_count));
    build_data = chunk.data;
    build_offsets = chunk.offsets;
    build_data_size = Max(chunk.size, 4);
  }
  else
  {
    F64* values = push_array(scratch.arena, F64, Max(build_row_count, 1));
    for (U64 i = 0; i < build_row_count; i++) values[i] = qe_read_numeric_as_f64(right_key_column, i);
    build_data = values;
    build_data_size = Max(build_row_count, 1) * sizeof(F64);
  }
  
  //- tec: probe-side key data
  void* probe_data = NULL;
  U64* probe_offsets = NULL;
  U64 probe_data_size = 4;
  
  if (is_string_key)
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
  
  //- tec: pass 1/2 - hash the build side into a bucket histogram
  GPU_Kernel* build_kernel = gpu_kernel_alloc(str8_lit("hash_join_build_count"));
  GPU_Buffer* build_data_buf = gpu_buffer_alloc(build_data_size, GPU_BufferFlag_Write, build_data);
  GPU_Buffer* build_off_buf = gpu_buffer_alloc(is_string_key ? (build_row_count + 1) * sizeof(U64) : 4, GPU_BufferFlag_Write, is_string_key ? build_offsets : 0);
  GPU_Buffer* bucket_count_buf = gpu_buffer_alloc(num_buckets * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  GPU_Buffer* row_bucket_buf = gpu_buffer_alloc(Max(build_row_count, 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  
  gpu_kernel_set_arg_buffer(build_kernel, 0, build_data_buf);
  gpu_kernel_set_arg_buffer(build_kernel, 1, build_off_buf);
  gpu_kernel_set_arg_buffer(build_kernel, 2, bucket_count_buf);
  gpu_kernel_set_arg_buffer(build_kernel, 3, row_bucket_buf);
  gpu_kernel_set_arg_u64(build_kernel, 0, build_row_count);
  gpu_kernel_set_arg_u64(build_kernel, 1, num_buckets);
  gpu_kernel_set_arg_u64(build_kernel, 2, is_string_key ? 1 : 0);
  
  if (build_row_count > 0)
  {
    gpu_kernel_execute(build_kernel, (U32)build_row_count, QE_GPU_WORKGROUP_SIZE);
  }
  
  U32* bucket_count_readback = push_array(scratch.arena, U32, num_buckets);
  gpu_buffer_read(bucket_count_buf, bucket_count_readback, num_buckets * sizeof(U32));
  
  gpu_kernel_release(build_kernel);
  gpu_buffer_release(bucket_count_buf);
  
  U32* bucket_offsets = push_array(scratch.arena, U32, num_buckets + 1);
  U32 running = 0;
  for (U64 b = 0; b < num_buckets; b++)
  {
    bucket_offsets[b] = running;
    running += bucket_count_readback[b];
  }
  bucket_offsets[num_buckets] = running;
  
  //- tec: pass 2/2 (build side). scatter build rows into per-bucket CSR lists
  GPU_Kernel* scatter_kernel = gpu_kernel_alloc(str8_lit("csr_scatter"));
  GPU_Buffer* cursor_buf = gpu_buffer_alloc(num_buckets * sizeof(U32), GPU_BufferFlag_ReadWrite, bucket_offsets);
  GPU_Buffer* bucket_rows_buf = gpu_buffer_alloc(Max(build_row_count, 1) * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  
  gpu_kernel_set_arg_buffer(scatter_kernel, 0, row_bucket_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 1, cursor_buf);
  gpu_kernel_set_arg_buffer(scatter_kernel, 2, bucket_rows_buf);
  gpu_kernel_set_arg_u64(scatter_kernel, 0, build_row_count);
  
  if (build_row_count > 0)
  {
    gpu_kernel_execute(scatter_kernel, (U32)build_row_count, QE_GPU_WORKGROUP_SIZE);
  }
  
  gpu_buffer_release(row_bucket_buf);
  gpu_buffer_release(cursor_buf);
  gpu_kernel_release(scatter_kernel);
  
  // tec: probe from the left side. output capacity is a bounded heuristic
  // if the real match count exceeds it, thats detected below and reported rather than silently truncated
  U64 out_capacity = Max(probe_row_count, build_row_count) * 64 + probe_row_count;
  U64 max_capacity = GPU_MAX_BUFFER_SIZE / (4 * sizeof(U32));
  if (out_capacity > max_capacity) out_capacity = max_capacity;
  if (out_capacity < 1) out_capacity = 1;
  
  GPU_Kernel* probe_kernel = gpu_kernel_alloc(str8_lit("hash_join_probe"));
  
  GPU_Buffer* probe_data_buf = gpu_buffer_alloc(probe_data_size, GPU_BufferFlag_Write, probe_data);
  GPU_Buffer* probe_off_buf = gpu_buffer_alloc(is_string_key ? (probe_row_count + 1) * sizeof(U64) : 4, GPU_BufferFlag_Write, is_string_key ? probe_offsets : 0);
  GPU_Buffer* bucket_offsets_buf = gpu_buffer_alloc((num_buckets + 1) * sizeof(U32), GPU_BufferFlag_Write, bucket_offsets);
  
  U32 zero_count[1] = {0};
  GPU_Buffer* out_count_buf = gpu_buffer_alloc(sizeof(U32), GPU_BufferFlag_ReadWrite, zero_count);
  GPU_Buffer* out_pairs_buf = gpu_buffer_alloc(out_capacity * 4 * sizeof(U32), GPU_BufferFlag_ReadWrite, 0);
  
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
  
  if (probe_row_count > 0)
  {
    gpu_kernel_execute(probe_kernel, (U32)probe_row_count, QE_GPU_WORKGROUP_SIZE);
  }
  
  U32 match_count32 = 0;
  gpu_buffer_read(out_count_buf, &match_count32, sizeof(U32));
  U64 match_count = match_count32;
  
  if (match_count > out_capacity)
  {
    log_error("qe_hash_join: join produced %llu matches, exceeding the %llu-pair output capacity - result is truncated",
              match_count, out_capacity);
    match_count = out_capacity;
  }
  
  U32* pairs_readback = push_array(scratch.arena, U32, Max(match_count, 1) * 4);
  if (match_count > 0)
  {
    gpu_buffer_read(out_pairs_buf, pairs_readback, match_count * 4 * sizeof(U32));
  }
  
  gpu_buffer_release(build_data_buf);
  gpu_buffer_release(build_off_buf);
  gpu_buffer_release(bucket_offsets_buf);
  gpu_buffer_release(bucket_rows_buf);
  gpu_buffer_release(probe_data_buf);
  gpu_buffer_release(probe_off_buf);
  gpu_buffer_release(out_count_buf);
  gpu_buffer_release(out_pairs_buf);
  gpu_kernel_release(probe_kernel);
  
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
