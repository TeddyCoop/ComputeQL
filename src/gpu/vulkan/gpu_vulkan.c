internal U32
gpu_vulkan_find_memory_type(U32 type_bits, VkMemoryPropertyFlags props)
{
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(g_vulkan_state->physical_device, &mem_props);
  
  for (U32 i = 0; i < mem_props.memoryTypeCount; i++)
  {
    if ((type_bits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
    {
      return i;
    }
  }
  
  log_error("failed to find suitable Vulkan memory type.");
  return max_U32;
}

internal void
gpu_init(void)
{
  ProfBeginFunction();
  
  Arena* arena = arena_alloc();
  g_vulkan_state = push_array(arena, GPU_State, 1);
  g_vulkan_state->arena = arena;
  
  VkResult res;
  
  //- tec: instance
  VkApplicationInfo app_info =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "GPU SQL Engine",
    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
    .pEngineName = "gdb",
    .engineVersion = VK_MAKE_VERSION(1, 0, 0),
    .apiVersion = VK_API_VERSION_1_3,
  };
  
  VkInstanceCreateInfo inst_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app_info,
  };
  
  res = vkCreateInstance(&inst_info, 0, &g_vulkan_state->instance);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan instance");
    return;
  }
  
  //- tec: physical device
  U32 dev_count = 0;
  vkEnumeratePhysicalDevices(g_vulkan_state->instance, &dev_count, 0);
  VkPhysicalDevice *devices = push_array(arena, VkPhysicalDevice, dev_count);
  vkEnumeratePhysicalDevices(g_vulkan_state->instance, &dev_count, devices);
  
  VkPhysicalDevice selected = 0;
  U32 compute_queue_family_index = ~0u;
  for (U32 i = 0; i < dev_count; i++)
  {
    U32 queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, 0);
    VkQueueFamilyProperties *props = push_array(arena, VkQueueFamilyProperties, queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, props);
    
    for (U32 j = 0; j < queue_count; j++)
    {
      if (props[j].queueFlags & VK_QUEUE_COMPUTE_BIT) 
      {
        selected = devices[i];
        compute_queue_family_index = j;
        break;
      }
    }
    if (selected) break;
  }
  
  if (!selected)
  {
    log_error("no suitable Vulkan GPU found");
    return;
  }
  
  g_vulkan_state->physical_device = selected;
  g_vulkan_state->compute_queue_family_index = compute_queue_family_index;
  
  //- tec: device properties (timestamps for GPU kernel timing)
  VkPhysicalDeviceProperties device_props;
  vkGetPhysicalDeviceProperties(selected, &device_props);
  g_vulkan_state->timestamp_period_ns = device_props.limits.timestampPeriod;
  
  //- tec: optional VK_EXT_memory_budget for free-memory queries
  U32 ext_count = 0;
  vkEnumerateDeviceExtensionProperties(selected, 0, &ext_count, 0);
  VkExtensionProperties *ext_props = push_array(arena, VkExtensionProperties, ext_count);
  vkEnumerateDeviceExtensionProperties(selected, 0, &ext_count, ext_props);
  
  char *enabled_extensions[1];
  U32 enabled_extension_count = 0;
  U64 budget_ext_name_size = cstring8_length((U8*)VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) + 1;
  for (U32 i = 0; i < ext_count; i++)
  {
    if (MemoryMatch(ext_props[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, budget_ext_name_size))
    {
      enabled_extensions[enabled_extension_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
      g_vulkan_state->has_memory_budget_ext = 1;
      break;
    }
  }
  
  //- tec: logical device
  F32 priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = compute_queue_family_index,
    .queueCount = 1,
    .pQueuePriorities = &priority,
  };
  
  VkPhysicalDeviceVulkan12Features features12 =
  {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    .descriptorBindingPartiallyBound = VK_TRUE,
  };
  
  // tec: shaderFloat64 lets scan_filter.comp use GLSL 'double' for precision-safe numeric
  // comparisons (float alone can't exactly represent U64/large-integer column values).
  VkPhysicalDeviceFeatures supported_features;
  vkGetPhysicalDeviceFeatures(selected, &supported_features);
  
  VkPhysicalDeviceFeatures enabled_features = {0};
  if (supported_features.shaderFloat64)
  {
    enabled_features.shaderFloat64 = VK_TRUE;
  }
  else
  {
    log_error("selected Vulkan GPU does not support shaderFloat64; numeric WHERE comparisons on U64/F64 columns may lose precision");
  }
  
  VkDeviceCreateInfo dev_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext = &features12,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queue_info,
    .enabledExtensionCount = enabled_extension_count,
    .ppEnabledExtensionNames = (const char* const*)enabled_extensions,
    .pEnabledFeatures = &enabled_features,
  };
  
  res = vkCreateDevice(selected, &dev_info, 0, &g_vulkan_state->device);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan logical device");
    return;
  }
  
  vkGetDeviceQueue(g_vulkan_state->device, compute_queue_family_index, 0, &g_vulkan_state->compute_queue);
  
  //- tec: command pool
  VkCommandPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = compute_queue_family_index,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
  };
  
  res = vkCreateCommandPool(g_vulkan_state->device, &pool_info, 0, &g_vulkan_state->command_pool);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan command pool");
    return;
  }
  
  //- tec: command buffer
  VkCommandBufferAllocateInfo alloc_info = 
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = g_vulkan_state->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  
  res = vkAllocateCommandBuffers(g_vulkan_state->device, &alloc_info, &g_vulkan_state->command_buffer);
  if (res != VK_SUCCESS) 
  {
    log_error("failed to allocate Vulkan command buffer");
    return;
  }
  
  // tec: descriptor pool (sized for many kernels, each with up to GPU_VULKAN_MAX_BOUND_BUFFERS bindings)
  VkDescriptorPoolSize pool_sizes[] =
  {
    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 * GPU_VULKAN_MAX_BOUND_BUFFERS },
  };
  
  VkDescriptorPoolCreateInfo desc_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .poolSizeCount = 1,
    .pPoolSizes = pool_sizes,
    .maxSets = 64,
  };
  
  res = vkCreateDescriptorPool(g_vulkan_state->device, &desc_pool_info, 0, &g_vulkan_state->descriptor_pool);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan descriptor pool");
    return;
  }
  
  //- tec: shared descriptor set layout - every kernel gets GPU_VULKAN_MAX_BOUND_BUFFERS storage
  //  buffer bindings, individually optional (partially bound) so a kernel using fewer buffers
  //  doesn't need every slot written.
  VkDescriptorSetLayoutBinding bindings[GPU_VULKAN_MAX_BOUND_BUFFERS];
  VkDescriptorBindingFlags binding_flags[GPU_VULKAN_MAX_BOUND_BUFFERS];
  for (U32 i = 0; i < GPU_VULKAN_MAX_BOUND_BUFFERS; i++)
  {
    bindings[i] = (VkDescriptorSetLayoutBinding){
      .binding = i,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    binding_flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
  }
  
  VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
    .bindingCount = GPU_VULKAN_MAX_BOUND_BUFFERS,
    .pBindingFlags = binding_flags,
  };
  
  VkDescriptorSetLayoutCreateInfo layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = &binding_flags_info,
    .bindingCount = GPU_VULKAN_MAX_BOUND_BUFFERS,
    .pBindings = bindings,
  };
  
  res = vkCreateDescriptorSetLayout(g_vulkan_state->device, &layout_info, 0, &g_vulkan_state->shared_descriptor_set_layout);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan descriptor set layout");
    return;
  }
  
  //- tec: shared pipeline layout 0 fixed push constant block used by every kernel for scalar args
  VkPushConstantRange push_constant_range =
  {
    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    .offset = 0,
    .size = sizeof(U64) * GPU_VULKAN_PUSH_CONSTANT_COUNT,
  };
  
  VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &g_vulkan_state->shared_descriptor_set_layout,
    .pushConstantRangeCount = 1,
    .pPushConstantRanges = &push_constant_range,
  };
  
  res = vkCreatePipelineLayout(g_vulkan_state->device, &pipeline_layout_info, 0, &g_vulkan_state->shared_pipeline_layout);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan pipeline layout");
    return;
  }
  
  //- tec: timestamp queries, used to time each kernel dispatch on the GPU
  VkQueryPoolCreateInfo query_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
    .queryType = VK_QUERY_TYPE_TIMESTAMP,
    .queryCount = 2,
  };
  
  res = vkCreateQueryPool(g_vulkan_state->device, &query_pool_info, 0, &g_vulkan_state->timestamp_query_pool);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan timestamp query pool");
    return;
  }
  
  //- tec: fence used to wait for a dispatch/transfer submitted on the single compute queue
  VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
  res = vkCreateFence(g_vulkan_state->device, &fence_info, 0, &g_vulkan_state->submit_fence);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan fence");
    return;
  }
  
  ProfEnd();
}

internal void
gpu_release(void)
{
  vkDestroyFence(g_vulkan_state->device, g_vulkan_state->submit_fence, 0);
  vkDestroyQueryPool(g_vulkan_state->device, g_vulkan_state->timestamp_query_pool, 0);
  vkDestroyPipelineLayout(g_vulkan_state->device, g_vulkan_state->shared_pipeline_layout, 0);
  vkDestroyDescriptorSetLayout(g_vulkan_state->device, g_vulkan_state->shared_descriptor_set_layout, 0);
  vkDestroyDescriptorPool(g_vulkan_state->device, g_vulkan_state->descriptor_pool, 0);
  vkDestroyCommandPool(g_vulkan_state->device, g_vulkan_state->command_pool, 0);
  vkDestroyDevice(g_vulkan_state->device, 0);
  vkDestroyInstance(g_vulkan_state->instance, 0);
  arena_release(g_vulkan_state->arena);
}

internal void
gpu_wait(void)
{
  vkQueueWaitIdle(g_vulkan_state->compute_queue);
}

internal U64
gpu_device_total_memory(void)
{
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(g_vulkan_state->physical_device, &mem_props);
  
  U64 total = 0;
  for (U32 i = 0; i < mem_props.memoryHeapCount; i++)
  {
    if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
    {
      total += mem_props.memoryHeaps[i].size;
    }
  }
  return total;
}

internal U64
gpu_device_free_memory(void)
{
  if (!g_vulkan_state->has_memory_budget_ext)
  {
    return gpu_device_total_memory();
  }
  
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
  VkPhysicalDeviceMemoryProperties2 mem_props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &budget_props };
  vkGetPhysicalDeviceMemoryProperties2(g_vulkan_state->physical_device, &mem_props2);
  
  U64 free_bytes = 0;
  for (U32 i = 0; i < mem_props2.memoryProperties.memoryHeapCount; i++)
  {
    if (mem_props2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
    {
      U64 budget = budget_props.heapBudget[i];
      U64 usage = budget_props.heapUsage[i];
      free_bytes += (budget > usage) ? (budget - usage) : 0;
    }
  }
  return free_bytes;
}

internal U64
gpu_get_executed_kernel_time_microseconds(void)
{
  return g_vulkan_state->last_kernel_time_microseconds;
}

internal VkCommandBuffer
gpu_vulkan_begin_one_time_cmd(void)
{
  vkResetCommandBuffer(g_vulkan_state->command_buffer, 0);
  VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(g_vulkan_state->command_buffer, &begin_info);
  return g_vulkan_state->command_buffer;
}

internal void
gpu_vulkan_end_and_submit_cmd(VkCommandBuffer cmd)
{
  vkEndCommandBuffer(cmd);
  
  VkSubmitInfo submit_info =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &cmd,
  };
  
  vkResetFences(g_vulkan_state->device, 1, &g_vulkan_state->submit_fence);
  vkQueueSubmit(g_vulkan_state->compute_queue, 1, &submit_info, g_vulkan_state->submit_fence);
  vkWaitForFences(g_vulkan_state->device, 1, &g_vulkan_state->submit_fence, VK_TRUE, UINT64_MAX);
}

internal B32
gpu_vulkan_alloc_raw_buffer(U64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props, VkBuffer* out_buffer, VkDeviceMemory* out_memory)
{
  VkBufferCreateInfo buf_info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  
  if (vkCreateBuffer(g_vulkan_state->device, &buf_info, 0, out_buffer) != VK_SUCCESS)
  {
    log_error("failed to create Vulkan buffer");
    return 0;
  }
  
  VkMemoryRequirements mem_req;
  vkGetBufferMemoryRequirements(g_vulkan_state->device, *out_buffer, &mem_req);
  
  U32 mem_type = gpu_vulkan_find_memory_type(mem_req.memoryTypeBits, mem_props);
  if (mem_type == UINT32_MAX)
  {
    log_error("No matching memory type for Vulkan buffer.");
    vkDestroyBuffer(g_vulkan_state->device, *out_buffer, 0);
    return 0;
  }
  
  VkMemoryAllocateInfo alloc_info =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = mem_req.size,
    .memoryTypeIndex = mem_type,
  };
  
  if (vkAllocateMemory(g_vulkan_state->device, &alloc_info, 0, out_memory) != VK_SUCCESS)
  {
    log_error("Failed to allocate Vulkan buffer memory.");
    vkDestroyBuffer(g_vulkan_state->device, *out_buffer, 0);
    return 0;
  }
  
  vkBindBufferMemory(g_vulkan_state->device, *out_buffer, *out_memory, 0);
  return 1;
}

// tec: uploads `data` into `dst` (a device-local, non-mapped buffer) via a transient staging buffer
internal void
gpu_vulkan_staged_upload(GPU_Buffer* dst, void* data, U64 size)
{
  VkBuffer staging_buffer;
  VkDeviceMemory staging_memory;
  VkMemoryPropertyFlags staging_mem_props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  if (!gpu_vulkan_alloc_raw_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_mem_props, &staging_buffer, &staging_memory))
  {
    return;
  }
  
  void* mapped = 0;
  vkMapMemory(g_vulkan_state->device, staging_memory, 0, size, 0, &mapped);
  MemoryCopy(mapped, data, size);
  vkUnmapMemory(g_vulkan_state->device, staging_memory);
  
  VkCommandBuffer cmd = gpu_vulkan_begin_one_time_cmd();
  VkBufferCopy copy_region = { .size = size };
  vkCmdCopyBuffer(cmd, staging_buffer, dst->buffer, 1, &copy_region);
  gpu_vulkan_end_and_submit_cmd(cmd);
  
  vkDestroyBuffer(g_vulkan_state->device, staging_buffer, 0);
  vkFreeMemory(g_vulkan_state->device, staging_memory, 0);
}

// tec: downloads `size` bytes from `src` (a device-local, non-mapped buffer) into `data` via a transient staging buffer
internal void
gpu_vulkan_staged_download(GPU_Buffer* src, void* data, U64 size)
{
  VkBuffer staging_buffer;
  VkDeviceMemory staging_memory;
  VkMemoryPropertyFlags staging_mem_props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  if (!gpu_vulkan_alloc_raw_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging_mem_props, &staging_buffer, &staging_memory))
  {
    return;
  }
  
  VkCommandBuffer cmd = gpu_vulkan_begin_one_time_cmd();
  VkBufferCopy copy_region = { .size = size };
  vkCmdCopyBuffer(cmd, src->buffer, staging_buffer, 1, &copy_region);
  gpu_vulkan_end_and_submit_cmd(cmd);
  
  void* mapped = 0;
  vkMapMemory(g_vulkan_state->device, staging_memory, 0, size, 0, &mapped);
  MemoryCopy(data, mapped, size);
  vkUnmapMemory(g_vulkan_state->device, staging_memory);
  
  vkDestroyBuffer(g_vulkan_state->device, staging_buffer, 0);
  vkFreeMemory(g_vulkan_state->device, staging_memory, 0);
}

internal GPU_Buffer*
gpu_buffer_alloc(U64 size, GPU_BufferFlags flags, void* data)
{
  GPU_Buffer* result = push_array(g_vulkan_state->arena, GPU_Buffer, 1);
  result->size = size;
  result->mapped_ptr = 0;
  
  VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  
  B32 host_visible = (flags & GPU_BufferFlag_HostVisible) != 0;
  B32 device_local = (flags & GPU_BufferFlag_DeviceLocal) != 0;
  
  VkMemoryPropertyFlags mem_props = 0;
  if (host_visible)
  {
    mem_props |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }
  if (device_local || !host_visible)
  {
    // tec: default to device-local when neither flag is given, since that's the fast path for compute
    mem_props |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }
  
  if (!gpu_vulkan_alloc_raw_buffer(size, usage, mem_props, &result->buffer, &result->memory))
  {
    return 0;
  }
  
  if (host_visible)
  {
    vkMapMemory(g_vulkan_state->device, result->memory, 0, size, 0, &result->mapped_ptr);
    if (data)
    {
      MemoryCopy(result->mapped_ptr, data, size);
    }
  }
  else if (data)
  {
    gpu_vulkan_staged_upload(result, data, size);
  }
  
  return result;
}

internal void
gpu_buffer_release(GPU_Buffer* buffer)
{
  if (buffer->mapped_ptr)
  {
    vkUnmapMemory(g_vulkan_state->device, buffer->memory);
  }
  
  vkDestroyBuffer(g_vulkan_state->device, buffer->buffer, 0);
  vkFreeMemory(g_vulkan_state->device, buffer->memory, 0);
}

internal void
gpu_buffer_write(GPU_Buffer* buffer, void* data, U64 size)
{
  if (size > buffer->size)
  {
    log_error("gpu_buffer_write: write size exceeds buffer size.");
    return;
  }
  
  if (buffer->mapped_ptr)
  {
    MemoryCopy(buffer->mapped_ptr, data, size);
  }
  else
  {
    gpu_vulkan_staged_upload(buffer, data, size);
  }
}

internal void
gpu_buffer_read(GPU_Buffer* buffer, void* data, U64 size)
{
  if (size > buffer->size)
  {
    log_error("gpu_buffer_read: read size exceeds buffer size.");
    return;
  }
  
  if (buffer->mapped_ptr)
  {
    MemoryCopy(data, buffer->mapped_ptr, size);
  }
  else
  {
    gpu_vulkan_staged_download(buffer, data, size);
  }
}

// tec: precompiled shaders live next to the executable at shaders/<kernel_name>.spv —
//  authored offline as GLSL and compiled by glslc, never compiled at runtime.
internal String8
gpu_vulkan_load_spirv_from_disk(Arena* arena, String8 kernel_name)
{
  ProfBeginFunction();
  Temp scratch = scratch_begin(&arena, 1);
  
  String8 exe_dir = os_get_process_info()->binary_path;
  String8 path = push_str8f(scratch.arena, "%.*s/shaders/%.*s.spv", str8_varg(exe_dir), str8_varg(kernel_name));
  
  String8 result = {0};
  if (os_file_path_exists(path))
  {
    OS_Handle file = os_file_open(OS_AccessFlag_Read, path);
    U64 file_size = os_properties_from_file(file).size;
    if (file_size > 0)
    {
      result = os_string_from_file_range(arena, file, r1u64(0, file_size));
    }
    os_file_close(file);
  }
  
  if (result.size == 0)
  {
    log_error("failed to load precompiled SPIR-V shader '%.*s' (expected at '%.*s')", str8_varg(kernel_name), str8_varg(path));
  }
  
  scratch_end(scratch);
  ProfEnd();
  return result;
}

internal GPU_Kernel*
gpu_kernel_alloc(String8 name)
{
  ProfBeginFunction();
  
  String8 spirv_bin = gpu_vulkan_load_spirv_from_disk(g_vulkan_state->arena, name);
  if (spirv_bin.size == 0)
  {
    ProfEnd();
    return 0;
  }
  
  VkShaderModuleCreateInfo shader_info =
  {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = spirv_bin.size,
    .pCode = (U32*)spirv_bin.str,
  };
  
  VkShaderModule shader;
  if (vkCreateShaderModule(g_vulkan_state->device, &shader_info, 0, &shader) != VK_SUCCESS)
  {
    log_error("failed to create Vulkan shader module for kernel '%.*s'", str8_varg(name));
    ProfEnd();
    return 0;
  }
  
  VkComputePipelineCreateInfo pipeline_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage =
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shader,
      .pName = "main",
    },
    .layout = g_vulkan_state->shared_pipeline_layout,
  };
  
  VkPipeline pipeline;
  if (vkCreateComputePipelines(g_vulkan_state->device, 0, 1, &pipeline_info, 0, &pipeline) != VK_SUCCESS)
  {
    log_error("failed to create Vulkan compute pipeline for kernel '%.*s'", str8_varg(name));
    vkDestroyShaderModule(g_vulkan_state->device, shader, 0);
    ProfEnd();
    return 0;
  }
  
  VkDescriptorSetAllocateInfo desc_alloc_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = g_vulkan_state->descriptor_pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &g_vulkan_state->shared_descriptor_set_layout,
  };
  
  VkDescriptorSet descriptor_set;
  if (vkAllocateDescriptorSets(g_vulkan_state->device, &desc_alloc_info, &descriptor_set) != VK_SUCCESS)
  {
    log_error("failed to allocate Vulkan descriptor set for kernel '%.*s'", str8_varg(name));
    vkDestroyPipeline(g_vulkan_state->device, pipeline, 0);
    vkDestroyShaderModule(g_vulkan_state->device, shader, 0);
    ProfEnd();
    return 0;
  }
  
  GPU_Kernel* kernel = push_array(g_vulkan_state->arena, GPU_Kernel, 1);
  kernel->name = push_str8_copy(g_vulkan_state->arena, name);
  kernel->shader = shader;
  kernel->pipeline = pipeline;
  kernel->descriptor_set = descriptor_set;
  
  ProfEnd();
  return kernel;
}

internal void
gpu_kernel_release(GPU_Kernel *kernel)
{
  vkFreeDescriptorSets(g_vulkan_state->device, g_vulkan_state->descriptor_pool, 1, &kernel->descriptor_set);
  vkDestroyPipeline(g_vulkan_state->device, kernel->pipeline, 0);
  vkDestroyShaderModule(g_vulkan_state->device, kernel->shader, 0);
}

internal void
gpu_kernel_set_arg_buffer(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer)
{
  if (index >= GPU_VULKAN_MAX_BOUND_BUFFERS)
  {
    log_error("gpu_kernel_set_arg_buffer: index %u exceeds GPU_VULKAN_MAX_BOUND_BUFFERS", index);
    return;
  }
  
  VkDescriptorBufferInfo buffer_info =
  {
    .buffer = buffer->buffer,
    .offset = 0,
    .range = buffer->size,
  };
  
  VkWriteDescriptorSet write =
  {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet = kernel->descriptor_set,
    .dstBinding = index,
    .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .pBufferInfo = &buffer_info,
  };
  
  vkUpdateDescriptorSets(g_vulkan_state->device, 1, &write, 0, 0);
  
  kernel->bound_buffers[index] = buffer;
  if (index >= kernel->bound_buffer_count)
  {
    kernel->bound_buffer_count = index + 1;
  }
}

internal void
gpu_kernel_set_arg_u64(GPU_Kernel* kernel, U32 index, U64 value)
{
  if (index >= GPU_VULKAN_PUSH_CONSTANT_COUNT)
  {
    log_error("gpu_kernel_set_arg_u64: index %u exceeds GPU_VULKAN_PUSH_CONSTANT_COUNT", index);
    return;
  }
  
  kernel->push_constants[index] = value;
}

internal void
gpu_kernel_execute(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size)
{
  U32 group_count = (global_work_size + local_work_size - 1) / local_work_size;
  
  VkCommandBuffer cmd = gpu_vulkan_begin_one_time_cmd();
  
  vkCmdResetQueryPool(cmd, g_vulkan_state->timestamp_query_pool, 0, 2);
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vulkan_state->timestamp_query_pool, 0);
  
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_vulkan_state->shared_pipeline_layout, 0, 1, &kernel->descriptor_set, 0, 0);
  vkCmdPushConstants(cmd, g_vulkan_state->shared_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(kernel->push_constants), kernel->push_constants);
  
  vkCmdDispatch(cmd, group_count, 1, 1);
  
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vulkan_state->timestamp_query_pool, 1);
  
  gpu_vulkan_end_and_submit_cmd(cmd);
  
  U64 timestamps[2];
  vkGetQueryPoolResults(g_vulkan_state->device, g_vulkan_state->timestamp_query_pool, 0, 2, sizeof(timestamps), timestamps, sizeof(U64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  
  F64 elapsed_ns = (F64)(timestamps[1] - timestamps[0]) * (F64)g_vulkan_state->timestamp_period_ns;
  g_vulkan_state->last_kernel_time_microseconds = (U64)(elapsed_ns / 1000.0);
}