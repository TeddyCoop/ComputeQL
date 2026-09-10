#ifndef GPU_VULKAN_H
#define GPU_VULKAN_H

#include "third_party/vulkan/vulkan/vulkan.h"

#define GPU_VULKAN_MAX_BOUND_BUFFERS 16
#define GPU_VULKAN_PUSH_CONSTANT_COUNT 8
#define GPU_VULKAN_MAX_CACHED_KERNELS 16
#define GPU_VULKAN_MAX_POOLED_BUFFERS 128
#define GPU_VULKAN_POOLED_BUFFER_HASH_SLOTS 256
#define GPU_BATCH_MAX_PENDING_READS 8
#define GPU_VULKAN_MEM_BLOCK_SIZE MB(1)
#define GPU_VULKAN_MAX_MEM_BLOCKS 64

typedef struct GPU_VulkanBuffer GPU_VulkanBuffer;
struct GPU_VulkanBuffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  U64 size;
  void* mapped_ptr;
  U64 bind_offset;
  B32 owns_memory;
};

typedef struct GPU_VulkanMemBlock GPU_VulkanMemBlock;
struct GPU_VulkanMemBlock
{
  VkDeviceMemory memory;
  U64 size;
  U64 cursor;
  void* mapped_ptr;
  U32 memory_type_index;
};

typedef struct GPU_VulkanKernel GPU_VulkanKernel;
struct GPU_VulkanKernel
{
  String8 name;

  VkShaderModule shader;
  VkPipeline pipeline;

  VkDescriptorSet descriptor_set;

  GPU_VulkanBuffer* bound_buffers[GPU_VULKAN_MAX_BOUND_BUFFERS];
  U32 bound_buffer_count;

  U64 push_constants[GPU_VULKAN_PUSH_CONSTANT_COUNT];
};

typedef struct GPU_PooledBuffer GPU_PooledBuffer;
struct GPU_PooledBuffer
{
  String8 name;
  GPU_VulkanBuffer* buffer;
  U64 capacity;
  void* imported_host_ptr;
};

typedef struct GPU_PendingRead GPU_PendingRead;
struct GPU_PendingRead
{
  B32 from_staging;   
  U64 staging_offset;
  void* mapped_src;
  void* out_data;
  U64 size;
};

typedef struct GPU_VulkanBatch GPU_VulkanBatch;
struct GPU_VulkanBatch
{
  VkCommandBuffer cmd;
  U64 upload_cursor;
  U64 download_cursor;
  
  B32 wrote_since_barrier;
  B32 dispatched_since_barrier;
  B32 had_dispatch;
  B32 has_commands;
  
  GPU_PendingRead pending_reads[GPU_BATCH_MAX_PENDING_READS];
  U32 pending_read_count;
};

typedef struct GPU_VulkanState GPU_VulkanState;
struct GPU_VulkanState
{
  Arena* arena;
  
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  
  VkDebugUtilsMessengerEXT debug_messenger;
  PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT_fn;
  
  VkQueue compute_queue;
  U32 compute_queue_family_index;
  
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  
  VkDescriptorPool descriptor_pool;
  VkDescriptorSetLayout shared_descriptor_set_layout;
  VkPipelineLayout shared_pipeline_layout;
  
  GPU_VulkanKernel* kernel_cache[GPU_VULKAN_MAX_CACHED_KERNELS];
  U32 kernel_cache_count;
  
  GPU_PooledBuffer pooled_buffers[GPU_VULKAN_MAX_POOLED_BUFFERS];
  U32 pooled_buffer_count;
  U32 pooled_buffer_hash_slots[GPU_VULKAN_POOLED_BUFFER_HASH_SLOTS];
  
  GPU_VulkanMemBlock mem_blocks[GPU_VULKAN_MAX_MEM_BLOCKS];
  U32 mem_block_count;
  
  VkQueryPool timestamp_query_pool;
  F32 timestamp_period_ns;
  
  VkFence submit_fence;
  U64 last_kernel_time_microseconds;
  
  B32 device_lost;
  
  B32 has_memory_budget_ext;
  
  // tec: lets gpu_buffer_import_host_readonly import an OS mapped file view directly as a VkBuffer's backing memory, with no CPU copy at all
  B32 has_external_memory_host_ext;
  U64 min_imported_host_pointer_alignment;
  PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT_fn;
  
  // tec: Resizable BAR
  // a memory type that's both DEVICE_LOCAL and HOST_VISIBLE lets gpu_buffer_alloc write straight into vram with a memcpy,
  // skipping the staging-buffer + vkCmdCopyBuffer round trip entirely
  // rebar_heap_size bounds how large a buffer can use that path before falling back to the staged path.
  B32 rebar_supported;
  U64 rebar_heap_size;
  
  VkBuffer upload_staging_buffer;
  VkDeviceMemory upload_staging_memory;
  void* upload_staging_mapped;
  U64 upload_staging_capacity;
  
  VkBuffer download_staging_buffer;
  VkDeviceMemory download_staging_memory;
  void* download_staging_mapped;
  U64 download_staging_capacity;
  
  GPU_VulkanBatch active_batch;
};

global GPU_VulkanState* g_vulkan_state = 0;


internal void gpu_vulkan_init(void);
internal void gpu_vulkan_release(void);
internal B32 gpu_vulkan_validation_requested(void);
internal Arena* gpu_vulkan_scratch_arena(void);
internal String8 gpu_vulkan_name(void);
internal GPU_Backend* gpu_vulkan_get_backend(void);

internal void gpu_vulkan_wait(void);
internal B32 gpu_vulkan_device_lost(void);

internal U64 gpu_vulkan_device_total_memory(void);
internal U32 gpu_vulkan_find_memory_type(U32 type_bits, VkMemoryPropertyFlags props);
internal U64 gpu_vulkan_device_max_storage_buffer_range(void);
internal U64 gpu_vulkan_device_free_memory(void);
internal U64 gpu_vulkan_get_executed_kernel_time_microseconds(void);
internal void gpu_vulkan_note_result(VkResult result);

internal VkCommandBuffer gpu_vulkan_begin_one_time_cmd(void);
internal B32 gpu_vulkan_end_and_submit_cmd_tagged(VkCommandBuffer cmd, const char* tag);

internal B32 gpu_vulkan_alloc_raw_buffer(U64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props, VkBuffer* out_buffer, VkDeviceMemory* out_memory);
internal B32 gpu_vulkan_alloc_dedicated_memory(GPU_VulkanBuffer* result, VkMemoryRequirements* mem_req, U32 mem_type, B32 wants_mapped);
internal GPU_VulkanMemBlock* gpu_vulkan_find_or_create_mem_block(U32 memory_type_index, B32 wants_mapped, U64 needed_size, U64 alignment);
internal B32 gpu_vulkan_buffer_alloc_backing(GPU_VulkanBuffer* result, U64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props);
internal void gpu_vulkan_ensure_staging_capacity(VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props,
                                                 VkBuffer* buffer, VkDeviceMemory* memory, void** mapped, U64* capacity,
                                                 U64 needed_size);
internal void gpu_vulkan_staged_upload(GPU_VulkanBuffer* dst, void* data, U64 size);
internal GPU_Buffer* gpu_vulkan_buffer_alloc(U64 size, GPU_BufferFlags flags, void* data);
internal void gpu_vulkan_buffer_release(GPU_Buffer* buffer);
internal void gpu_vulkan_buffer_read(GPU_Buffer* buffer, void* data, U64 size);
internal void gpu_vulkan_buffer_write(GPU_Buffer* buffer, void* data, U64 size);

internal GPU_PooledBuffer* gpu_vulkan_pooled_buffer_find(String8 name);
internal void gpu_vulkan_pooled_buffer_hash_insert(String8 name, U32 index);
internal GPU_Buffer* gpu_vulkan_buffer_alloc_pooled(String8 name, U64 size, GPU_BufferFlags flags, void* data);
internal GPU_Buffer* gpu_vulkan_buffer_import_host_readonly(void* host_ptr, U64 size);
internal GPU_Buffer* gpu_vulkan_buffer_import_host_readonly_pooled(String8 name, void* host_ptr, U64 size);

internal String8 gpu_vulkan_load_spirv_from_disk(Arena* arena, String8 kernel_name);
internal GPU_Kernel* gpu_vulkan_kernel_alloc(String8 name);
internal void gpu_vulkan_kernel_release(GPU_Kernel *kernel);
internal void gpu_vulkan_kernel_set_arg_buffer(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer);
internal void gpu_vulkan_kernel_set_arg_u64(GPU_Kernel* kernel, U32 index, U64 value);
internal void gpu_vulkan_kernel_execute(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);

internal GPU_Batch* gpu_vulkan_batch_begin(U64 upload_bytes_needed, U64 download_bytes_needed);
internal void gpu_vulkan_batch_buffer_write(GPU_Batch* batch, GPU_Buffer* buffer, void* data, U64 size);
internal void gpu_vulkan_batch_buffer_fill(GPU_Batch* batch, GPU_Buffer* buffer, U64 size, U32 value);
internal void gpu_vulkan_batch_buffer_zero(GPU_Batch* batch, GPU_Buffer* buffer, U64 size);
internal void gpu_vulkan_batch_kernel_execute(GPU_Batch* batch, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
internal void gpu_vulkan_batch_buffer_read(GPU_Batch* batch, GPU_Buffer* buffer, void* out_data, U64 size);
internal B32 gpu_vulkan_batch_end(GPU_Batch* batch);

#endif //GPU_VULKAN_H
