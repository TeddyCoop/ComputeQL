internal void
gpu_backend_register_all(void)
{
#if GPU_BACKEND_VULKAN
  gpu_backend_register(GPU_BackendKind_Vulkan, str8_lit("vulkan"), gpu_vulkan_get_backend());
#endif
}

internal void
gpu_backend_register(GPU_BackendKind kind, String8 name, GPU_Backend* backend)
{
  if (g_gpu_backend_registration_count >= ArrayCount(g_gpu_backend_registrations))
  {
    log_error("gpu_backend_register: too many backends registered, dropping '%.*s'", str8_varg(name));
    return;
  }
  if (backend->struct_size != sizeof(GPU_Backend))
  {
    log_error("gpu_backend_register: '%.*s' backend struct_size mismatch (got %u, expected %u) - refusing to register",
              str8_varg(name), backend->struct_size, (U32)sizeof(GPU_Backend));
    return;
  }
  
  GPU_BackendRegistration* reg = &g_gpu_backend_registrations[g_gpu_backend_registration_count++];
  reg->kind = kind;
  reg->name = name;
  reg->backend = backend;
}

internal void
gpu_request_backend(String8 name)
{
  U64 len = Min(name.size, (U64)GPU_BACKEND_NAME_BUF_SIZE - 1);
  MemoryCopy(g_gpu_requested_backend_name_buf, name.str, len);
  g_gpu_requested_backend_name_buf[len] = 0;
  g_gpu_requested_backend_name = str8((U8*)g_gpu_requested_backend_name_buf, len);
}

internal String8
gpu_active_backend_name(void)
{
  return g_active_gpu_backend_reg ? g_active_gpu_backend_reg->name : str8_zero();
}

internal String8List
gpu_available_backend_names(Arena* arena)
{
  String8List list = {0};
  for (U32 i = 0; i < g_gpu_backend_registration_count; i++)
  {
    str8_list_push(arena, &list, g_gpu_backend_registrations[i].name);
  }
  return list;
}

internal GPU_BackendRegistration*
gpu_find_backend_registration(String8 name)
{
  GPU_BackendRegistration* found = 0;
  for (U32 i = 0; i < g_gpu_backend_registration_count && !found; i++)
  {
    if (str8_match(g_gpu_backend_registrations[i].name, name, StringMatchFlag_CaseInsensitive))
    {
      found = &g_gpu_backend_registrations[i];
    }
  }
  return found;
}

internal GPU_BackendRegistration*
gpu_choose_backend_registration(void)
{
  GPU_BackendRegistration* chosen = 0;
  
  if (g_gpu_requested_backend_name.size != 0)
  {
    chosen = gpu_find_backend_registration(g_gpu_requested_backend_name);
    if (!chosen)
    {
      log_error("gpu: requested backend '%.*s' is not compiled in", str8_varg(g_gpu_requested_backend_name));
    }
    return chosen;
  }
  
  if (g_gpu_backend_registration_count == 1)
  {
    chosen = &g_gpu_backend_registrations[0];
  }
  else if (g_gpu_backend_registration_count > 1)
  {
    Temp scratch = scratch_begin(0, 0);
    String8List available = gpu_available_backend_names(scratch.arena);
    String8 joined = str8_list_join(scratch.arena, &available, &(StringJoin){.sep = str8_lit(", ")});
    log_error("gpu: multiple GPU backends are compiled in (%.*s) - pass --gpu=<name> to pick one", str8_varg(joined));
    scratch_end(scratch);
  }
  
  return chosen;
}

internal void
gpu_init(void)
{
  gpu_backend_register_all();
  
  if (g_gpu_backend_registration_count == 0)
  {
    log_error("gpu_init: no GPU backend compiled in");
    os_abort(1);
  }
  
  g_active_gpu_backend_reg = gpu_choose_backend_registration();
  if (!g_active_gpu_backend_reg)
  {
    os_abort(1);
  }
  
  {
    Temp scratch = scratch_begin(0, 0);
    String8List available = gpu_available_backend_names(scratch.arena);
    String8 joined = str8_list_join(scratch.arena, &available, &(StringJoin){.sep = str8_lit(", ")});
    log_info("gpu: using backend '%.*s' (available: %.*s)",
             str8_varg(g_active_gpu_backend_reg->name), str8_varg(joined));
    scratch_end(scratch);
  }
  
  g_active_gpu_backend_reg->backend->init();
}

internal B32
gpu_backend_switch(String8 name)
{
  GPU_BackendRegistration* next = gpu_find_backend_registration(name);
  if (!next)
  {
    log_error("gpu_backend_switch: backend '%.*s' is not compiled in", str8_varg(name));
    return 0;
  }
  
  if (g_active_gpu_backend_reg)
  {
    g_active_gpu_backend_reg->backend->wait();
    g_active_gpu_backend_reg->backend->release();
  }
  
  g_active_gpu_backend_reg = next;
  g_active_gpu_backend_reg->backend->init();
  
  log_info("gpu: switched to backend '%.*s'", str8_varg(g_active_gpu_backend_reg->name));
  return 1;
}

internal void
gpu_release(void)
{
  g_active_gpu_backend_reg->backend->release();
}

internal void
gpu_wait(void)
{
  g_active_gpu_backend_reg->backend->wait();
}

internal U64
gpu_get_executed_kernel_time_microseconds(void)
{
  return g_active_gpu_backend_reg->backend->get_executed_kernel_time_microseconds();
}

internal U64
gpu_device_total_memory(void)
{
  return g_active_gpu_backend_reg->backend->device_total_memory();
}

internal U64
gpu_device_free_memory(void)
{
  return g_active_gpu_backend_reg->backend->device_free_memory();
}

internal U64
gpu_device_max_storage_buffer_range(void)
{
  return g_active_gpu_backend_reg->backend->device_max_storage_buffer_range();
}

internal B32
gpu_device_lost(void)
{
  return g_active_gpu_backend_reg->backend->device_lost();
}

internal void
gpu_pool_begin_epoch(void)
{
  g_active_gpu_backend_reg->backend->pool_begin_epoch();
}

internal GPU_MemoryStats
gpu_memory_stats(void)
{
  return g_active_gpu_backend_reg->backend->memory_stats();
}

internal GPU_Buffer*
gpu_buffer_alloc(U64 size, GPU_BufferFlags flags, void* data)
{
  return g_active_gpu_backend_reg->backend->buffer_alloc(size, flags, data);
}

internal GPU_Buffer*
gpu_buffer_alloc_pooled(String8 name, U64 size, GPU_BufferFlags flags, void* data)
{
  return g_active_gpu_backend_reg->backend->buffer_alloc_pooled(name, size, flags, data);
}

internal GPU_Buffer*
gpu_buffer_import_host_readonly(void* host_ptr, U64 size)
{
  return g_active_gpu_backend_reg->backend->buffer_import_host_readonly(host_ptr, size);
}

internal GPU_Buffer*
gpu_buffer_import_host_readonly_pooled(String8 name, void* host_ptr, U64 size)
{
  return g_active_gpu_backend_reg->backend->buffer_import_host_readonly_pooled(name, host_ptr, size);
}

internal void
gpu_buffer_release(GPU_Buffer* buffer)
{
  g_active_gpu_backend_reg->backend->buffer_release(buffer);
}

internal void
gpu_buffer_write(GPU_Buffer* buffer, void* data, U64 size)
{
  if (!buffer)
  {
    gpu_note_missing_buffer("buffer_write");
    return;
  }
  g_active_gpu_backend_reg->backend->buffer_write(buffer, data, size);
}

internal void
gpu_buffer_read(GPU_Buffer* buffer, void* data, U64 size)
{
  if (!buffer)
  {
    gpu_note_missing_buffer("buffer_read");
    MemoryZero(data, size);
    return;
  }
  g_active_gpu_backend_reg->backend->buffer_read(buffer, data, size);
}

internal void
gpu_note_missing_buffer(char* operation)
{
  g_gpu_missing_buffer_uses += 1;
  log_error("gpu: %s skipped, a buffer or batch it needs was never allocated", operation);
}

internal B32
gpu_kernel_has_missing_arg(GPU_Kernel* kernel)
{
  for (U32 i = 0; i < g_gpu_missing_arg_kernel_count; i++)
  {
    if (g_gpu_missing_arg_kernels[i] == kernel)
    {
      return 1;
    }
  }
  return 0;
}

internal void
gpu_kernel_set_missing_arg(GPU_Kernel* kernel, B32 missing)
{
  for (U32 i = 0; i < g_gpu_missing_arg_kernel_count; i++)
  {
    if (g_gpu_missing_arg_kernels[i] == kernel)
    {
      if (!missing)
      {
        g_gpu_missing_arg_kernel_count -= 1;
        g_gpu_missing_arg_kernels[i] = g_gpu_missing_arg_kernels[g_gpu_missing_arg_kernel_count];
      }
      return;
    }
  }
  if (missing && kernel && g_gpu_missing_arg_kernel_count < GPU_MISSING_ARG_KERNEL_CAP)
  {
    g_gpu_missing_arg_kernels[g_gpu_missing_arg_kernel_count] = kernel;
    g_gpu_missing_arg_kernel_count += 1;
  }
}

internal U64
gpu_failure_count(void)
{
  return gpu_memory_stats().unrecovered_alloc_failures + g_gpu_missing_buffer_uses;
}

internal void
gpu_kernel_log_reset(void)
{
  MemoryZeroStruct(&g_gpu_kernel_log);
}

internal void
gpu_kernel_log_note(String8 name)
{
  U64 size = Min(name.size, (U64)(GPU_KERNEL_LOG_NAME_SIZE - 1));
  for (U32 i = 0; i < g_gpu_kernel_log.count; i++)
  {
    String8 logged = str8_cstring(g_gpu_kernel_log.names[i]);
    if (str8_match(logged, str8_prefix(name, size), 0))
    {
      g_gpu_kernel_log.uses[i] += 1;
      return;
    }
  }
  if (g_gpu_kernel_log.count >= GPU_KERNEL_LOG_CAP)
  {
    return;
  }
  U32 slot = g_gpu_kernel_log.count;
  MemoryCopy(g_gpu_kernel_log.names[slot], name.str, size);
  g_gpu_kernel_log.names[slot][size] = 0;
  g_gpu_kernel_log.uses[slot] = 1;
  g_gpu_kernel_log.count += 1;
}

internal GPU_Kernel*
gpu_kernel_alloc(String8 name)
{
  gpu_kernel_log_note(name);
  GPU_Kernel* kernel = g_active_gpu_backend_reg->backend->kernel_alloc(name);
  gpu_kernel_set_missing_arg(kernel, 0);
  return kernel;
}

internal void
gpu_kernel_release(GPU_Kernel* kernel)
{
  g_active_gpu_backend_reg->backend->kernel_release(kernel);
}

internal void
gpu_kernel_execute(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size)
{
  if (!kernel || gpu_kernel_has_missing_arg(kernel))
  {
    gpu_note_missing_buffer("kernel_execute");
    return;
  }
  g_active_gpu_backend_reg->backend->kernel_execute(kernel, global_work_size, local_work_size);
}

internal void
gpu_kernel_set_arg_buffer(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer)
{
  if (!buffer)
  {
    // tec: the descriptor would keep the buffer an earlier query bound there, so the kernel must not run
    gpu_kernel_set_missing_arg(kernel, 1);
    gpu_note_missing_buffer("kernel_set_arg_buffer");
    return;
  }
  g_active_gpu_backend_reg->backend->kernel_set_arg_buffer(kernel, index, buffer);
}

internal void
gpu_kernel_set_arg_u64(GPU_Kernel* kernel, U32 index, U64 value)
{
  g_active_gpu_backend_reg->backend->kernel_set_arg_u64(kernel, index, value);
}

internal GPU_Batch*
gpu_batch_begin(U64 upload_bytes_needed, U64 download_bytes_needed)
{
  return g_active_gpu_backend_reg->backend->batch_begin(upload_bytes_needed, download_bytes_needed);
}

internal void
gpu_batch_buffer_write(GPU_Batch* batch, GPU_Buffer* buffer, void* data, U64 size)
{
  if (!batch || !buffer)
  {
    gpu_note_missing_buffer("batch_buffer_write");
    return;
  }
  g_active_gpu_backend_reg->backend->batch_buffer_write(batch, buffer, data, size);
}

internal void
gpu_batch_buffer_zero(GPU_Batch* batch, GPU_Buffer* buffer, U64 size)
{
  if (!batch || !buffer)
  {
    gpu_note_missing_buffer("batch_buffer_zero");
    return;
  }
  g_active_gpu_backend_reg->backend->batch_buffer_zero(batch, buffer, size);
}

internal void
gpu_batch_buffer_fill(GPU_Batch* batch, GPU_Buffer* buffer, U64 size, U32 value)
{
  if (!batch || !buffer)
  {
    gpu_note_missing_buffer("batch_buffer_fill");
    return;
  }
  g_active_gpu_backend_reg->backend->batch_buffer_fill(batch, buffer, size, value);
}

internal void
gpu_batch_kernel_execute(GPU_Batch* batch, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size)
{
  if (!batch || !kernel || gpu_kernel_has_missing_arg(kernel))
  {
    gpu_note_missing_buffer("batch_kernel_execute");
    return;
  }
  g_active_gpu_backend_reg->backend->batch_kernel_execute(batch, kernel, global_work_size, local_work_size);
}

internal void
gpu_batch_buffer_read(GPU_Batch* batch, GPU_Buffer* buffer, void* out_data, U64 size)
{
  if (!batch || !buffer)
  {
    gpu_note_missing_buffer("batch_buffer_read");
    MemoryZero(out_data, size);
    return;
  }
  g_active_gpu_backend_reg->backend->batch_buffer_read(batch, buffer, out_data, size);
}

internal B32
gpu_batch_end(GPU_Batch* batch)
{
  if (!batch)
  {
    gpu_note_missing_buffer("batch_end");
    return 0;
  }
  return g_active_gpu_backend_reg->backend->batch_end(batch);
}

internal GPU_Stream*
gpu_stream_alloc(U32 slot_count)
{
  return g_active_gpu_backend_reg->backend->stream_alloc(slot_count);
}

internal void
gpu_stream_release(GPU_Stream* stream)
{
  g_active_gpu_backend_reg->backend->stream_release(stream);
}

internal B32
gpu_stream_slot_begin(GPU_Stream* stream, U32 slot, U64 upload_bytes, U64 readback_bytes)
{
  return g_active_gpu_backend_reg->backend->stream_slot_begin(stream, slot, upload_bytes, readback_bytes);
}

internal B32
gpu_stream_copy(GPU_Stream* stream, U32 slot, GPU_Buffer* destination, void* host_ptr, U64 size)
{
  if (!stream || !destination)
  {
    gpu_note_missing_buffer("stream_copy");
    return 0;
  }
  return g_active_gpu_backend_reg->backend->stream_copy(stream, slot, destination, host_ptr, size);
}

internal void
gpu_stream_zero(GPU_Stream* stream, U32 slot, GPU_Buffer* buffer, U64 size)
{
  if (!stream || !buffer)
  {
    gpu_note_missing_buffer("stream_zero");
    return;
  }
  g_active_gpu_backend_reg->backend->stream_zero(stream, slot, buffer, size);
}

internal void
gpu_stream_fill(GPU_Stream* stream, U32 slot, GPU_Buffer* buffer, U64 size, U32 value)
{
  if (!stream || !buffer)
  {
    gpu_note_missing_buffer("stream_fill");
    return;
  }
  g_active_gpu_backend_reg->backend->stream_fill(stream, slot, buffer, size, value);
}

internal void
gpu_stream_dispatch(GPU_Stream* stream, U32 slot, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size)
{
  if (!stream || !kernel || gpu_kernel_has_missing_arg(kernel))
  {
    gpu_note_missing_buffer("stream_dispatch");
    return;
  }
  g_active_gpu_backend_reg->backend->stream_dispatch(stream, slot, kernel, global_work_size, local_work_size);
}

internal void
gpu_stream_read(GPU_Stream* stream, U32 slot, GPU_Buffer* buffer, void* out_data, U64 size)
{
  if (!stream || !buffer)
  {
    gpu_note_missing_buffer("stream_read");
    MemoryZero(out_data, size);
    return;
  }
  g_active_gpu_backend_reg->backend->stream_read(stream, slot, buffer, out_data, size);
}

internal B32
gpu_stream_submit(GPU_Stream* stream, U32 slot)
{
  return g_active_gpu_backend_reg->backend->stream_submit(stream, slot);
}

internal B32
gpu_stream_wait(GPU_Stream* stream, U32 slot)
{
  return g_active_gpu_backend_reg->backend->stream_wait(stream, slot);
}

internal GPU_StreamStats
gpu_stream_get_stats(GPU_Stream* stream)
{
  return g_active_gpu_backend_reg->backend->stream_get_stats(stream);
}

internal Arena*
gpu_scratch_arena(void)
{
  return g_active_gpu_backend_reg->backend->scratch_arena();
}
