/* date = May 14th 2025 1:39 pm */

#ifndef GPU_VULKAN_H
#define GPU_VULKAN_H

#include "third_party/vulkan/vulkan/vulkan.h"

#define GPU_VULKAN_MAX_BOUND_BUFFERS 16
#define GPU_VULKAN_PUSH_CONSTANT_COUNT 8

struct GPU_Buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  U64 size;
  void* mapped_ptr;
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

  VkQueryPool timestamp_query_pool;
  F32 timestamp_period_ns;

  VkFence submit_fence;
  U64 last_kernel_time_microseconds;

  B32 has_memory_budget_ext;
};

global GPU_State* g_vulkan_state = 0;

#endif //GPU_VULKAN_H
