/* date = January 23rd 2025 10:18 pm */

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
  GPU_BufferFlag_COUNT,
} GPU_BufferFlags;

typedef struct GPU_State GPU_State;
typedef struct GPU_Buffer GPU_Buffer;
typedef struct GPU_Kernel GPU_Kernel;

internal void gpu_init(void);
internal void gpu_release(void);
internal void gpu_wait(void);
internal U64 gpu_get_executed_kernel_time_microseconds(void);

// tec: returns total device memory in bytes
internal U64 gpu_device_total_memory(void);
internal U64 gpu_device_free_memory(void);

internal GPU_Buffer* gpu_buffer_alloc(U64 size, GPU_BufferFlags flags, void* data);
internal void gpu_buffer_release(GPU_Buffer* buffer);
internal void gpu_buffer_write(GPU_Buffer* buffer, void* data, U64 size);
internal void gpu_buffer_read(GPU_Buffer* buffer, void* data, U64 size);

internal GPU_Kernel* gpu_kernel_alloc(String8 name);
internal void gpu_kernel_release(GPU_Kernel *kernel);
internal void gpu_kernel_execute(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
internal void gpu_kernel_set_arg_buffer(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer);
internal void gpu_kernel_set_arg_u64(GPU_Kernel* kernel, U32 index, U64 value);

#endif //GPU_H
