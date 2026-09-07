#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

typedef enum GPU_BackendKind
{
  GPU_BackendKind_Vulkan,
  /*
  GPU_BackendKind_HIP,
  GPU_BackendKind_CUDA,
  GPU_BackendKind_DX12,
  GPU_BackendKind_Metal,
  */
  GPU_BackendKind_COUNT,
} GPU_BackendKind;

typedef struct GPU_Backend GPU_Backend;
struct GPU_Backend
{
  U32 struct_size;
  
  void   (*init)(void);
  void   (*release)(void);
  void   (*wait)(void);
  U64    (*get_executed_kernel_time_microseconds)(void);
  U64    (*device_total_memory)(void);
  U64    (*device_free_memory)(void);
  U64    (*device_max_storage_buffer_range)(void);
  B32    (*device_lost)(void);
  
  GPU_Buffer* (*buffer_alloc)(U64 size, GPU_BufferFlags flags, void* data);
  GPU_Buffer* (*buffer_alloc_pooled)(String8 name, U64 size, GPU_BufferFlags flags, void* data);
  GPU_Buffer* (*buffer_import_host_readonly)(void* host_ptr, U64 size);
  GPU_Buffer* (*buffer_import_host_readonly_pooled)(String8 name, void* host_ptr, U64 size);
  void        (*buffer_release)(GPU_Buffer* buffer);
  void        (*buffer_write)(GPU_Buffer* buffer, void* data, U64 size);
  void        (*buffer_read)(GPU_Buffer* buffer, void* data, U64 size);
  
  GPU_Kernel* (*kernel_alloc)(String8 name);
  void        (*kernel_release)(GPU_Kernel* kernel);
  void        (*kernel_execute)(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
  void        (*kernel_set_arg_buffer)(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer);
  void        (*kernel_set_arg_u64)(GPU_Kernel* kernel, U32 index, U64 value);
  
  GPU_Batch* (*batch_begin)(U64 upload_bytes_needed, U64 download_bytes_needed);
  void       (*batch_buffer_write)(GPU_Batch* batch, GPU_Buffer* buffer, void* data, U64 size);
  void       (*batch_buffer_zero)(GPU_Batch* batch, GPU_Buffer* buffer, U64 size);
  void       (*batch_buffer_fill)(GPU_Batch* batch, GPU_Buffer* buffer, U64 size, U32 value);
  void       (*batch_kernel_execute)(GPU_Batch* batch, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
  void       (*batch_buffer_read)(GPU_Batch* batch, GPU_Buffer* buffer, void* out_data, U64 size);
  B32        (*batch_end)(GPU_Batch* batch);
  
  Arena*  (*scratch_arena)(void);
  String8 (*name)(void);
};

typedef struct GPU_BackendRegistration GPU_BackendRegistration;
struct GPU_BackendRegistration
{
  GPU_BackendKind kind;
  String8 name;
  GPU_Backend* backend;
};

//~ tec: global state
#define GPU_BACKEND_NAME_BUF_SIZE 64

global GPU_BackendRegistration g_gpu_backend_registrations[GPU_BackendKind_COUNT];
global U32 g_gpu_backend_registration_count = 0;

global GPU_BackendRegistration* g_active_gpu_backend_reg = 0;

global char g_gpu_requested_backend_name_buf[GPU_BACKEND_NAME_BUF_SIZE];
global String8 g_gpu_requested_backend_name = {0};

//~ tec: funcs
internal void         gpu_backend_register(GPU_BackendKind kind, String8 name, GPU_Backend* backend);
internal void         gpu_backend_register_all(void);

internal void         gpu_request_backend(String8 name);
internal String8      gpu_active_backend_name(void);
internal String8List  gpu_available_backend_names(Arena* arena);

internal B32           gpu_backend_switch(String8 name);

#endif //GPU_BACKEND_H
