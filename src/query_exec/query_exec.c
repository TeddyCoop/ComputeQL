
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
  prog->consts[index * 2 + 0] = (U32)(bits & 0xffffffffull);
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
    // tec: bare column/literal used as a boolean predicate on its own - not really valid SQL,
    // but load it as a numeric value and let 'nonzero' mean true.
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

  // tec: string comparisons - column op 'literal', where the column is a String8 column.
  // this is the common/parity case for SQL text like `name = 'Alice'` / `name contains 'sub'`.
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

  // tec: generic numeric comparison - works for column-op-column, column-op-literal and
  // literal-op-column since the stack preserves left/right push order.
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

internal QE_ScanResult
qe_scan_filter(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* root_node)
{
  ProfBeginFunction();

  QE_ScanResult result = {0};

  IR_Node* where_clause = ir_node_find_child(root_node, IR_NodeType_Where);

  QE_BytecodeProgram* prog = push_array(arena, QE_BytecodeProgram, 1);
  qe_bytecode_program_build(prog, database, table, root_node, where_clause);

  GPU_Kernel* kernel = gpu_kernel_alloc(str8_lit("scan_filter"));
  if (!kernel)
  {
    log_error("qe_scan_filter: failed to alloc 'scan_filter' kernel");
    ProfEnd();
    return result;
  }

  //- tec: upload the (query-invariant) bytecode + constant pool buffers once, outside the chunk loop
  GPU_Buffer* bytecode_buffer = gpu_buffer_alloc(Max(prog->word_count, 1) * sizeof(U32), GPU_BufferFlag_Write, 0);
  gpu_buffer_write(bytecode_buffer, prog->words, prog->word_count * sizeof(U32));

  GPU_Buffer* num_consts_buffer = gpu_buffer_alloc(Max(prog->const_count * 2, 1) * sizeof(U32), GPU_BufferFlag_Write, 0);
  if (prog->const_count > 0)
  {
    gpu_buffer_write(num_consts_buffer, prog->consts, prog->const_count * 2 * sizeof(U32));
  }

  GPU_Buffer* str_consts_buffer = gpu_buffer_alloc(Max(prog->str_const_pool_size, 4), GPU_BufferFlag_Write, 0);
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

  for (U64 chunk_index = 0; chunk_index < chunk_count; chunk_index++)
  {
    Temp chunk_arena = temp_begin(arena);

    U64 chunk_row_start = chunk_index * rows_per_chunk;
    U64 chunk_rows = needs_chunking ? Min(rows_per_chunk, table->row_count - chunk_row_start) : table->row_count;
    Rng1U64 chunk_range = r1u64(chunk_row_start, chunk_row_start + chunk_rows);

    log_info("filtering rows %llu-%llu", chunk_range.min, chunk_range.max);

    ProfBegin("allocating column GPU buffers");
    GPU_Buffer* column_gpu_buffers[QE_MAX_COLUMN_BINDINGS] = {0};

    for (U32 i = 0; i < prog->binding_count; i++)
    {
      QE_ColumnBinding* binding = &prog->bindings[i];
      U32 descriptor_binding = QE_BINDING_COLUMN_BASE + binding->first_slot;

      if (binding->type == GDB_ColumnType_String8)
      {
        U64 start_read_time = os_now_microseconds();
        GDB_StringDataChunk str_chunk = gdb_column_get_string_chunk(chunk_arena.arena, binding->column, chunk_range);
        load_data_from_disk_time += os_now_microseconds() - start_read_time;

        if (str_chunk.data && str_chunk.offsets)
        {
          GPU_Buffer* data_buf = gpu_buffer_alloc(str_chunk.size, GPU_BufferFlag_Write | GPU_BufferFlag_HostVisible, 0);
          gpu_buffer_write(data_buf, str_chunk.data, str_chunk.size);
          column_gpu_buffers[binding->first_slot + 0] = data_buf;
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 0, data_buf);

          // tec: +1 row for the trailing offset used to compute the last string's size
          U64 offsets_size = (str_chunk.row_count + 1) * sizeof(U64);
          GPU_Buffer* offsets_buf = gpu_buffer_alloc(offsets_size, GPU_BufferFlag_Write | GPU_BufferFlag_CopyHostPointer, str_chunk.offsets);
          column_gpu_buffers[binding->first_slot + 1] = offsets_buf;
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding + 1, offsets_buf);
        }
        else
        {
          log_error("qe_scan_filter: failed to load string data/offsets for column '%.*s'", str8_varg(binding->name));
        }

        gdb_column_close_string_chunk(binding->column);
      }
      else
      {
        U64 size = 0;
        U64 start_read_time = os_now_microseconds();
        void* data_ptr = gdb_column_get_data_range(chunk_arena.arena, binding->column, chunk_range, &size);
        load_data_from_disk_time += os_now_microseconds() - start_read_time;

        if (data_ptr)
        {
          GPU_Buffer* data_buf = gpu_buffer_alloc(size, GPU_BufferFlag_Write, 0);
          gpu_buffer_write(data_buf, data_ptr, size);
          column_gpu_buffers[binding->first_slot] = data_buf;
          gpu_kernel_set_arg_buffer(kernel, descriptor_binding, data_buf);
        }
      }
    }

    GPU_Buffer* output_buffer = gpu_buffer_alloc(Max(chunk_rows, 1) * 2 * sizeof(U32), GPU_BufferFlag_Read, 0);
    U32 zero_count[2] = {0, 0};
    GPU_Buffer* result_counter_buffer = gpu_buffer_alloc(sizeof(zero_count), GPU_BufferFlag_ReadWrite | GPU_BufferFlag_HostCached, zero_count);
    ProfEnd();

    gpu_kernel_set_arg_buffer(kernel, QE_BINDING_OUT_INDICES, output_buffer);
    gpu_kernel_set_arg_buffer(kernel, QE_BINDING_OUT_COUNT, result_counter_buffer);
    gpu_kernel_set_arg_u64(kernel, QE_PUSH_CONSTANT_ROW_COUNT, chunk_rows);

    // tec: TODO fix local size - one workgroup per row for now, revisit for performance later
    gpu_kernel_execute(kernel, (U32)chunk_rows, 1);
    gpu_kernel_execution_time += gpu_get_executed_kernel_time_microseconds();

    gpu_wait();

    U32 result_count32[2] = {0, 0};
    gpu_buffer_read(result_counter_buffer, result_count32, sizeof(result_count32));
    U64 result_count = result_count32[0];

    for (U32 i = 0; i < prog->binding_count; i++)
    {
      QE_ColumnBinding* binding = &prog->bindings[i];
      if (column_gpu_buffers[binding->first_slot]) gpu_buffer_release(column_gpu_buffers[binding->first_slot]);
      if (binding->slot_count == 2 && column_gpu_buffers[binding->first_slot + 1]) gpu_buffer_release(column_gpu_buffers[binding->first_slot + 1]);
    }
    temp_end(chunk_arena);

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

    gpu_buffer_release(output_buffer);
    gpu_buffer_release(result_counter_buffer);
  }

  gpu_buffer_release(bytecode_buffer);
  gpu_buffer_release(num_consts_buffer);
  gpu_buffer_release(str_consts_buffer);
  gpu_kernel_release(kernel);

  log_info("gpu kernel total execution time: %llu microseconds", gpu_kernel_execution_time);
  log_info("load from disk total time: %llu microseconds", load_data_from_disk_time);

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
