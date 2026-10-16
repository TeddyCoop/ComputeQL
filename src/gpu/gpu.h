#ifndef GPU_H
#define GPU_H

#if !defined(GPU_MAX_BUFFER_SIZE)
#define GPU_MAX_BUFFER_SIZE MB(64)
#endif
//#define GPU_MAX_BUFFER_SIZE MB(512)
//#define GPU_MAX_BUFFER_SIZE GB(1)

typedef enum GPU_BufferFlags
{
  GPU_BufferFlag_Read  = (1 << 0),
  GPU_BufferFlag_Write = (1 << 1),
  GPU_BufferFlag_ReadWrite = (1 << 2),
  GPU_BufferFlag_HostVisible = (1 << 3),
  GPU_BufferFlag_DeviceLocal = (1 << 4),
  GPU_BufferFlag_HostCached = (1 << 5),
  GPU_BufferFlag_ZeroCopy = (1 << 6),
  GPU_BufferFlag_CopyHostPointer = (1 << 7),
  GPU_BufferFlag_Dynamic = (1 << 8),
  GPU_BufferFlag_Shared = (1 << 9),
  GPU_BufferFlag_COUNT,
} GPU_BufferFlags;

typedef struct GPU_MemoryStats GPU_MemoryStats;
struct GPU_MemoryStats
{
  U64 bytes_in_use;
  U64 bytes_peak;
  U64 alloc_failures;
  U64 alloc_retry_successes;
  U64 pool_evictions;
  U64 pool_evicted_bytes;
  U64 alloc_calls;
  U64 unrecovered_alloc_failures;
};

#define GPU_KERNEL_LOG_CAP 32
#define GPU_KERNEL_LOG_NAME_SIZE 48
typedef struct GPU_KernelLog GPU_KernelLog;
struct GPU_KernelLog
{
  U32 count;
  char names[GPU_KERNEL_LOG_CAP][GPU_KERNEL_LOG_NAME_SIZE];
  U64 uses[GPU_KERNEL_LOG_CAP];
};
global GPU_KernelLog g_gpu_kernel_log = {0};

internal void gpu_kernel_log_reset(void);
internal void gpu_kernel_log_note(String8 name);

global U64 g_gpu_fault_alloc_at = 0;

typedef struct GPU_Buffer GPU_Buffer;
typedef struct GPU_Kernel GPU_Kernel;
typedef struct GPU_Batch GPU_Batch;
typedef struct GPU_Stream GPU_Stream;

#define GPU_MISSING_ARG_KERNEL_CAP 16
global U64 g_gpu_missing_buffer_uses = 0;
global GPU_Kernel* g_gpu_missing_arg_kernels[GPU_MISSING_ARG_KERNEL_CAP] = {0};
global U32 g_gpu_missing_arg_kernel_count = 0;

internal void gpu_note_missing_buffer(char* operation);
internal B32 gpu_kernel_has_missing_arg(GPU_Kernel* kernel);
internal void gpu_kernel_set_missing_arg(GPU_Kernel* kernel, B32 missing);
internal U64 gpu_failure_count(void);

typedef struct GPU_StreamStats GPU_StreamStats;
struct GPU_StreamStats
{
  U64 bytes_copied;
  U64 imports_created;
  U64 import_microseconds;
  U64 staged_bytes;
  B32 separate_copy_queue;
};

internal void gpu_init(void);
internal void gpu_release(void);
internal void gpu_wait(void);
internal U64 gpu_get_executed_kernel_time_microseconds(void);
internal Arena* gpu_scratch_arena(void);

// tec: returns total device memory in bytes
internal U64 gpu_device_total_memory(void);
internal U64 gpu_device_free_memory(void);
internal U64 gpu_device_max_storage_buffer_range(void);

internal B32 gpu_device_lost(void);

internal void gpu_pool_begin_epoch(void);
internal GPU_MemoryStats gpu_memory_stats(void);

// tec: a cache registers here to give back device memory, called with the bytes still needed after pooled buffers have been evicted, returns the bytes freed
typedef U64 GPU_EvictHook(U64 bytes_wanted);
global GPU_EvictHook* g_gpu_evict_hook = 0;

internal GPU_Buffer* gpu_buffer_alloc(U64 size, GPU_BufferFlags flags, void* data);
internal GPU_Buffer* gpu_buffer_alloc_pooled(String8 name, U64 size, GPU_BufferFlags flags, void* data);
// tec: imports an existing host pointer directly. returns 0 if fails for any reason or not supported
internal GPU_Buffer* gpu_buffer_import_host_readonly(void* host_ptr, U64 size);
// tec: reuses the last import registered under `name` as long as host_ptr/size are unchanged
internal GPU_Buffer* gpu_buffer_import_host_readonly_pooled(String8 name, void* host_ptr, U64 size);
internal void gpu_buffer_release(GPU_Buffer* buffer);
internal void gpu_buffer_write(GPU_Buffer* buffer, void* data, U64 size);
internal void gpu_buffer_read(GPU_Buffer* buffer, void* data, U64 size);

internal GPU_Kernel* gpu_kernel_alloc(String8 name);
internal void gpu_kernel_release(GPU_Kernel *kernel);
internal void gpu_kernel_execute(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
internal void gpu_kernel_set_arg_buffer(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer);
internal void gpu_kernel_set_arg_u64(GPU_Kernel* kernel, U32 index, U64 value);

//~ tec: batching
internal GPU_Batch* gpu_batch_begin(U64 upload_bytes_needed, U64 download_bytes_needed);
internal void gpu_batch_buffer_write(GPU_Batch* batch, GPU_Buffer* buffer, void* data, U64 size);
internal void gpu_batch_buffer_zero(GPU_Batch* batch, GPU_Buffer* buffer, U64 size);
internal void gpu_batch_buffer_fill(GPU_Batch* batch, GPU_Buffer* buffer, U64 size, U32 value);
internal void gpu_batch_kernel_execute(GPU_Batch* batch, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
internal void gpu_batch_buffer_read(GPU_Batch* batch, GPU_Buffer* buffer, void* out_data, U64 size);
internal B32 gpu_batch_end(GPU_Batch* batch);

//~ tec: streams
internal GPU_Stream* gpu_stream_alloc(U32 slot_count);
internal void gpu_stream_release(GPU_Stream* stream);
internal B32 gpu_stream_slot_begin(GPU_Stream* stream, U32 slot, U64 upload_bytes, U64 readback_bytes);
internal B32 gpu_stream_copy(GPU_Stream* stream, U32 slot, GPU_Buffer* destination, void* host_ptr, U64 size);
internal void gpu_stream_zero(GPU_Stream* stream, U32 slot, GPU_Buffer* buffer, U64 size);
internal void gpu_stream_fill(GPU_Stream* stream, U32 slot, GPU_Buffer* buffer, U64 size, U32 value);
internal void gpu_stream_dispatch(GPU_Stream* stream, U32 slot, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
internal void gpu_stream_read(GPU_Stream* stream, U32 slot, GPU_Buffer* buffer, void* out_data, U64 size);
internal B32 gpu_stream_submit(GPU_Stream* stream, U32 slot);
internal B32 gpu_stream_wait(GPU_Stream* stream, U32 slot);
internal GPU_StreamStats gpu_stream_get_stats(GPU_Stream* stream);


#endif //GPU_H
