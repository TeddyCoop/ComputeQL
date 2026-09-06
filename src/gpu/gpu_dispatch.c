#define GPU_BACKEND_NAME_BUF_SIZE 64

global GPU_BackendRegistration g_gpu_backend_registrations[GPU_BackendKind_COUNT];
global U32 g_gpu_backend_registration_count = 0;

global GPU_BackendRegistration* g_active_gpu_backend_reg = 0;

global char g_gpu_requested_backend_name_buf[GPU_BACKEND_NAME_BUF_SIZE];
global String8 g_gpu_requested_backend_name = {0};

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
  g_active_gpu_backend_reg->backend->buffer_write(buffer, data, size);
}

internal void
gpu_buffer_read(GPU_Buffer* buffer, void* data, U64 size)
{
  g_active_gpu_backend_reg->backend->buffer_read(buffer, data, size);
}

internal GPU_Kernel*
gpu_kernel_alloc(String8 name)
{
  return g_active_gpu_backend_reg->backend->kernel_alloc(name);
}

internal void
gpu_kernel_release(GPU_Kernel* kernel)
{
  g_active_gpu_backend_reg->backend->kernel_release(kernel);
}

internal void
gpu_kernel_execute(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size)
{
  g_active_gpu_backend_reg->backend->kernel_execute(kernel, global_work_size, local_work_size);
}

internal void
gpu_kernel_set_arg_buffer(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer)
{
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
  g_active_gpu_backend_reg->backend->batch_buffer_write(batch, buffer, data, size);
}

internal void
gpu_batch_buffer_zero(GPU_Batch* batch, GPU_Buffer* buffer, U64 size)
{
  g_active_gpu_backend_reg->backend->batch_buffer_zero(batch, buffer, size);
}

internal void
gpu_batch_buffer_fill(GPU_Batch* batch, GPU_Buffer* buffer, U64 size, U32 value)
{
  g_active_gpu_backend_reg->backend->batch_buffer_fill(batch, buffer, size, value);
}

internal void
gpu_batch_kernel_execute(GPU_Batch* batch, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size)
{
  g_active_gpu_backend_reg->backend->batch_kernel_execute(batch, kernel, global_work_size, local_work_size);
}

internal void
gpu_batch_buffer_read(GPU_Batch* batch, GPU_Buffer* buffer, void* out_data, U64 size)
{
  g_active_gpu_backend_reg->backend->batch_buffer_read(batch, buffer, out_data, size);
}

internal B32
gpu_batch_end(GPU_Batch* batch)
{
  return g_active_gpu_backend_reg->backend->batch_end(batch);
}

internal Arena*
gpu_scratch_arena(void)
{
  return g_active_gpu_backend_reg->backend->scratch_arena();
}
