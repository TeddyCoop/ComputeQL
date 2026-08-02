/* date = May 14th 2025 1:39 pm */

#ifndef GPU_VULKAN_H
#define GPU_VULKAN_H

#include "third_party/vulkan/vulkan/vulkan.h"

#define GPU_VULKAN_MAX_BOUND_BUFFERS 16
#define GPU_VULKAN_PUSH_CONSTANT_COUNT 8
#define GPU_VULKAN_MAX_CACHED_KERNELS 16

struct GPU_Buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  U64 size;
  void* mapped_ptr;
  U64 bind_offset;
};

struct GPU_Kernel
{
  String8 name;
  
  VkShaderModule shader;
  VkPipeline pipeline;
  
  VkDescriptorSet descriptor_set;
  
  GPU_Buffer* bound_buffers[GPU_VULKAN_MAX_BOUND_BUFFERS];
  U32 bound_buffer_count;
  
  U64 push_constants[GPU_VULKAN_PUSH_CONSTANT_COUNT];
};

struct GPU_State
{
  Arena* arena;
  
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  
  VkQueue compute_queue;
  U32 compute_queue_family_index;
  
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  
  VkDescriptorPool descriptor_pool;
  VkDescriptorSetLayout shared_descriptor_set_layout;
  VkPipelineLayout shared_pipeline_layout;

  // tec: gpu_kernel_alloc caches by name
  GPU_Kernel* kernel_cache[GPU_VULKAN_MAX_CACHED_KERNELS];
  U32 kernel_cache_count;
  
  VkQueryPool timestamp_query_pool;
  F32 timestamp_period_ns;
  
  VkFence submit_fence;
  U64 last_kernel_time_microseconds;
  
  B32 has_memory_budget_ext;
  
  // tec: lets gpu_buffer_import_host_readonly import an OS mapped file view directly as a VkBuffer's backing memory, with no CPU copy at all
  B32 has_external_memory_host_ext;
  U64 min_imported_host_pointer_alignment;
  PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT_fn; // tec: extension function, not in the static loader import lib - must be resolved via vkGetDeviceProcAddr
  
  // tec: Resizable BAR
  // a memory type that's both DEVICE_LOCAL and HOST_VISIBLE lets gpu_buffer_alloc write straight into vram with a memcpy, skipping the staging-buffer + vkCmdCopyBuffer round trip entirely
  // rebar_heap_size bounds how large a buffer can use that path before falling back to the staged path.
  B32 rebar_supported;
  U64 rebar_heap_size;
  
  // tec: persistent, lazily-grown, permanently-mapped staging buffers that are reused
  VkBuffer upload_staging_buffer;
  VkDeviceMemory upload_staging_memory;
  void* upload_staging_mapped;
  U64 upload_staging_capacity;
  
  VkBuffer download_staging_buffer;
  VkDeviceMemory download_staging_memory;
  void* download_staging_mapped;
  U64 download_staging_capacity;
};

global GPU_State* g_vulkan_state = 0;

#endif //GPU_VULKAN_H
