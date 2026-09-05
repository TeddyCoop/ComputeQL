internal VkBool32 VKAPI_PTR
gpu_vulkan_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                           const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data)
{
  (void)types; (void)user_data;
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
  {
    log_error("[vulkan validation] %s", data->pMessage);
  }
  else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
  {
    log_warn("[vulkan validation] %s", data->pMessage);
  }
  else
  {
    log_info("[vulkan validation] %s", data->pMessage);
  }
  return VK_FALSE;
}

internal B32
gpu_vulkan_validation_requested(void)
{
  return 1;
}

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

  const char* wanted_layer = "VK_LAYER_KHRONOS_validation";
  B32 has_validation_layer = 0;
  B32 has_debug_utils_ext = 0;
  const char* enabled_instance_extensions[2];
  U32 enabled_instance_extension_count = 0;
  VkDebugUtilsMessengerCreateInfoEXT messenger_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    .pfnUserCallback = gpu_vulkan_debug_callback,
  };
  VkValidationFeatureEnableEXT enabled_validation_features[] =
  {
    VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
  };
  VkValidationFeaturesEXT validation_features =
  {
    .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
    .enabledValidationFeatureCount = ArrayCount(enabled_validation_features),
    .pEnabledValidationFeatures = enabled_validation_features,
  };

  if (gpu_vulkan_validation_requested())
  {
    U32 layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, 0);
    VkLayerProperties* layers = push_array(arena, VkLayerProperties, layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers);
    for (U32 i = 0; i < layer_count; i++)
    {
      if (MemoryMatch(layers[i].layerName, wanted_layer, cstring8_length((U8*)wanted_layer) + 1))
      {
        has_validation_layer = 1;
        break;
      }
    }

    B32 has_validation_features_ext = 0;
    U32 ext_count = 0;
    vkEnumerateInstanceExtensionProperties(0, &ext_count, 0);
    VkExtensionProperties* exts = push_array(arena, VkExtensionProperties, ext_count);
    vkEnumerateInstanceExtensionProperties(0, &ext_count, exts);
    for (U32 i = 0; i < ext_count; i++)
    {
      if (MemoryMatch(exts[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME, cstring8_length((U8*)VK_EXT_DEBUG_UTILS_EXTENSION_NAME) + 1))
      {
        has_debug_utils_ext = 1;
      }
      else if (MemoryMatch(exts[i].extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, cstring8_length((U8*)VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) + 1))
      {
        has_validation_features_ext = 1;
      }
    }

    if (has_debug_utils_ext) enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (has_validation_features_ext) enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
    validation_features.pNext = has_debug_utils_ext ? &messenger_info : 0;

    if (has_validation_layer)
    {
      inst_info.enabledLayerCount = 1;
      inst_info.ppEnabledLayerNames = &wanted_layer;
    }
    inst_info.enabledExtensionCount = enabled_instance_extension_count;
    inst_info.ppEnabledExtensionNames = enabled_instance_extensions;
    if (has_validation_layer && has_validation_features_ext)
    {
      inst_info.pNext = &validation_features;
    }
    else if (has_validation_layer && has_debug_utils_ext)
    {
      inst_info.pNext = &messenger_info;
    }

    if (!has_validation_layer)
    {
      log_info("GDB_VULKAN_VALIDATION requested but VK_LAYER_KHRONOS_validation was not found - install the Vulkan SDK's validation layer to enable it");
    }
  }

  res = vkCreateInstance(&inst_info, 0, &g_vulkan_state->instance);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan instance");
    return;
  }

  if (has_validation_layer && has_debug_utils_ext)
  {
    PFN_vkCreateDebugUtilsMessengerEXT create_messenger_fn =
      (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vulkan_state->instance, "vkCreateDebugUtilsMessengerEXT");
    g_vulkan_state->vkDestroyDebugUtilsMessengerEXT_fn =
      (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vulkan_state->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (create_messenger_fn)
    {
      create_messenger_fn(g_vulkan_state->instance, &messenger_info, 0, &g_vulkan_state->debug_messenger);
    }
    log_info("Vulkan validation layer active (GPU-Assisted Validation + synchronization validation enabled)");
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
  
  //- tec: detect Resizable BAR / Smart Access Memory
  {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(selected, &mem_props);
    
    VkMemoryPropertyFlags rebar_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (U32 i = 0; i < mem_props.memoryTypeCount; i++)
    {
      if ((mem_props.memoryTypes[i].propertyFlags & rebar_flags) == rebar_flags)
      {
        U64 heap_size = mem_props.memoryHeaps[mem_props.memoryTypes[i].heapIndex].size;
        if (heap_size > g_vulkan_state->rebar_heap_size)
        {
          g_vulkan_state->rebar_supported = 1;
          g_vulkan_state->rebar_heap_size = heap_size;
        }
      }
    }
    
    if (g_vulkan_state->rebar_supported)
    {
      log_info("Resizable BAR detected (%llu MB host-visible VRAM) - GPU buffer uploads will skip staging where possible", g_vulkan_state->rebar_heap_size / (1024 * 1024));
    }
  }
  
  //- tec: device properties (timestamps for GPU kernel timing)
  VkPhysicalDeviceProperties device_props;
  vkGetPhysicalDeviceProperties(selected, &device_props);
  g_vulkan_state->timestamp_period_ns = device_props.limits.timestampPeriod;
  
  //- tec: optional VK_EXT_memory_budget for free-memory queries
  U32 ext_count = 0;
  vkEnumerateDeviceExtensionProperties(selected, 0, &ext_count, 0);
  VkExtensionProperties *ext_props = push_array(arena, VkExtensionProperties, ext_count);
  vkEnumerateDeviceExtensionProperties(selected, 0, &ext_count, ext_props);
  
  char *enabled_extensions[2];
  U32 enabled_extension_count = 0;
  U64 budget_ext_name_size = cstring8_length((U8*)VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) + 1;
  U64 ext_mem_host_name_size = cstring8_length((U8*)VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) + 1;
  for (U32 i = 0; i < ext_count; i++)
  {
    if (MemoryMatch(ext_props[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, budget_ext_name_size))
    {
      enabled_extensions[enabled_extension_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
      g_vulkan_state->has_memory_budget_ext = 1;
    }
    else if (MemoryMatch(ext_props[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, ext_mem_host_name_size))
    {
      enabled_extensions[enabled_extension_count++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
      g_vulkan_state->has_external_memory_host_ext = 1;
    }
  }
  
  //- tec: VK_EXT_external_memory_host
  if (g_vulkan_state->has_external_memory_host_ext)
  {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT ext_mem_host_props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ext_mem_host_props };
    vkGetPhysicalDeviceProperties2(selected, &props2);
    g_vulkan_state->min_imported_host_pointer_alignment = ext_mem_host_props.minImportedHostPointerAlignment;
    
    if (g_vulkan_state->min_imported_host_pointer_alignment == 0 || g_vulkan_state->min_imported_host_pointer_alignment > 4096)
    {
      g_vulkan_state->has_external_memory_host_ext = 0;
    }
    else
    {
      log_info("VK_EXT_external_memory_host supported (alignment=%llu) - disk-backed numeric column reads will import mapped file views directly, skipping the upload copy", g_vulkan_state->min_imported_host_pointer_alignment);
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

  if (has_validation_layer)
  {
    VkPhysicalDeviceVulkan12Features features12_query = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 features2_query = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12_query };
    vkGetPhysicalDeviceFeatures2(selected, &features2_query);
    if (features12_query.bufferDeviceAddress)
    {
      features12.bufferDeviceAddress = VK_TRUE;
    }
  }

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
  
  if (g_vulkan_state->has_external_memory_host_ext)
  {
    g_vulkan_state->vkGetMemoryHostPointerPropertiesEXT_fn = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vulkan_state->device, "vkGetMemoryHostPointerPropertiesEXT");
    if (!g_vulkan_state->vkGetMemoryHostPointerPropertiesEXT_fn)
    {
      g_vulkan_state->has_external_memory_host_ext = 0;
    }
  }
  
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
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
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
  if (g_vulkan_state->upload_staging_capacity != 0)
  {
    vkUnmapMemory(g_vulkan_state->device, g_vulkan_state->upload_staging_memory);
    vkDestroyBuffer(g_vulkan_state->device, g_vulkan_state->upload_staging_buffer, 0);
    vkFreeMemory(g_vulkan_state->device, g_vulkan_state->upload_staging_memory, 0);
  }
  if (g_vulkan_state->download_staging_capacity != 0)
  {
    vkUnmapMemory(g_vulkan_state->device, g_vulkan_state->download_staging_memory);
    vkDestroyBuffer(g_vulkan_state->device, g_vulkan_state->download_staging_buffer, 0);
    vkFreeMemory(g_vulkan_state->device, g_vulkan_state->download_staging_memory, 0);
  }
  
  for (U32 i = 0; i < g_vulkan_state->kernel_cache_count; i++)
  {
    GPU_Kernel* kernel = g_vulkan_state->kernel_cache[i];
    vkDestroyPipeline(g_vulkan_state->device, kernel->pipeline, 0);
    vkDestroyShaderModule(g_vulkan_state->device, kernel->shader, 0);
  }

  vkDestroyFence(g_vulkan_state->device, g_vulkan_state->submit_fence, 0);
  vkDestroyQueryPool(g_vulkan_state->device, g_vulkan_state->timestamp_query_pool, 0);
  vkDestroyPipelineLayout(g_vulkan_state->device, g_vulkan_state->shared_pipeline_layout, 0);
  vkDestroyDescriptorSetLayout(g_vulkan_state->device, g_vulkan_state->shared_descriptor_set_layout, 0);
  vkDestroyDescriptorPool(g_vulkan_state->device, g_vulkan_state->descriptor_pool, 0);
  vkDestroyCommandPool(g_vulkan_state->device, g_vulkan_state->command_pool, 0);
  vkDestroyDevice(g_vulkan_state->device, 0);
  if (g_vulkan_state->debug_messenger && g_vulkan_state->vkDestroyDebugUtilsMessengerEXT_fn)
  {
    g_vulkan_state->vkDestroyDebugUtilsMessengerEXT_fn(g_vulkan_state->instance, g_vulkan_state->debug_messenger, 0);
  }
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
gpu_device_max_storage_buffer_range(void)
{
  VkPhysicalDeviceProperties device_props;
  vkGetPhysicalDeviceProperties(g_vulkan_state->physical_device, &device_props);
  return device_props.limits.maxStorageBufferRange;
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

internal void
gpu_vulkan_note_result(VkResult result)
{
  if (result != VK_ERROR_DEVICE_LOST) return;
  if (!g_vulkan_state->device_lost)
  {
    log_error("Vulkan device lost (VK_ERROR_DEVICE_LOST) - per the Vulkan spec the entire VkDevice "
              "is now permanently unusable, and there is no device recreation path. All "
              "further GPU allocations and dispatches will now fail immediately rather than retry.");
  }
  g_vulkan_state->device_lost = 1;
}

internal B32
gpu_device_lost(void)
{
  return g_vulkan_state->device_lost;
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

internal B32
gpu_vulkan_end_and_submit_cmd(VkCommandBuffer cmd)
{
  if (g_vulkan_state->device_lost)
  {
    return 0;
  }

  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit_info =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &cmd,
  };

  U64 t0 = os_now_microseconds();
  vkResetFences(g_vulkan_state->device, 1, &g_vulkan_state->submit_fence);

  VkResult submit_result = vkQueueSubmit(g_vulkan_state->compute_queue, 1, &submit_info, g_vulkan_state->submit_fence);
  if (submit_result != VK_SUCCESS)
  {
    // tec: the fence was never signaled if the submit itself failed
    gpu_vulkan_note_result(submit_result);
    log_error("vkQueueSubmit failed with VkResult %d - not waiting on the fence", (int)submit_result);
    return 0;
  }

  VkResult wait_result = vkWaitForFences(g_vulkan_state->device, 1, &g_vulkan_state->submit_fence, VK_TRUE, Billion(30));
  if (wait_result != VK_SUCCESS)
  {
    log_error("vkWaitForFences failed/timed out with VkResult %d after %llu microseconds - GPU is hung or the "
              "device was lost. The command buffer/fence may still be pending; continuing would risk resetting/"
              "resubmitting them while still in flight, corrupting further GPU work. Aborting.",
              (int)wait_result, os_now_microseconds() - t0);
    os_abort(1);
  }

  log_info("submit+wait wall time: %llu microseconds", os_now_microseconds() - t0);
  return 1;
}

internal B32
gpu_vulkan_alloc_raw_buffer(U64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props, VkBuffer* out_buffer, VkDeviceMemory* out_memory)
{
  if (g_vulkan_state->device_lost)
  {
    return 0;
  }

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
  
  VkResult alloc_result = vkAllocateMemory(g_vulkan_state->device, &alloc_info, 0, out_memory);
  if (alloc_result != VK_SUCCESS)
  {
    gpu_vulkan_note_result(alloc_result);
    log_error("Failed to allocate Vulkan buffer memory.");
    vkDestroyBuffer(g_vulkan_state->device, *out_buffer, 0);
    return 0;
  }
  
  vkBindBufferMemory(g_vulkan_state->device, *out_buffer, *out_memory, 0);
  return 1;
}

// tec: grows a persistent staging buffer
// (one of the g_vulkan_state->{upload,download}_staging_*groups) to at least `needed_size`, remapping if it had to reallocate
internal void
gpu_vulkan_ensure_staging_capacity(VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props,
                                   VkBuffer* buffer, VkDeviceMemory* memory, void** mapped, U64* capacity,
                                   U64 needed_size)
{
  if (needed_size <= *capacity)
  {
    return;
  }
  
  if (*capacity != 0)
  {
    vkUnmapMemory(g_vulkan_state->device, *memory);
    vkDestroyBuffer(g_vulkan_state->device, *buffer, 0);
    vkFreeMemory(g_vulkan_state->device, *memory, 0);
    *capacity = 0;
  }
  
  if (!gpu_vulkan_alloc_raw_buffer(needed_size, usage, mem_props, buffer, memory))
  {
    *mapped = 0;
    return;
  }
  
  vkMapMemory(g_vulkan_state->device, *memory, 0, needed_size, 0, mapped);
  *capacity = needed_size;
}

// tec: uploads `data` into `dst` (a device-local, non-mapped buffer) via the persistent upload staging buffer
internal void
gpu_vulkan_staged_upload(GPU_Buffer* dst, void* data, U64 size)
{
  gpu_vulkan_ensure_staging_capacity(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     &g_vulkan_state->upload_staging_buffer, &g_vulkan_state->upload_staging_memory,
                                     &g_vulkan_state->upload_staging_mapped, &g_vulkan_state->upload_staging_capacity,
                                     size);
  if (size > g_vulkan_state->upload_staging_capacity)
  {
    return;
  }
  
  MemoryCopy(g_vulkan_state->upload_staging_mapped, data, size);
  
  VkCommandBuffer cmd = gpu_vulkan_begin_one_time_cmd();
  VkBufferCopy copy_region = { .size = size };
  vkCmdCopyBuffer(cmd, g_vulkan_state->upload_staging_buffer, dst->buffer, 1, &copy_region);
  gpu_vulkan_end_and_submit_cmd(cmd);
}

// tec: downloads `size` bytes from `src` (a device-local, non-mapped buffer) into `data` via the persistent download staging buffer
internal void
gpu_vulkan_staged_download(GPU_Buffer* src, void* data, U64 size)
{
  gpu_vulkan_ensure_staging_capacity(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                     &g_vulkan_state->download_staging_buffer, &g_vulkan_state->download_staging_memory,
                                     &g_vulkan_state->download_staging_mapped, &g_vulkan_state->download_staging_capacity,
                                     size);
  if (size > g_vulkan_state->download_staging_capacity)
  {
    return;
  }
  
  VkCommandBuffer cmd = gpu_vulkan_begin_one_time_cmd();
  VkBufferCopy copy_region = { .size = size };
  vkCmdCopyBuffer(cmd, src->buffer, g_vulkan_state->download_staging_buffer, 1, &copy_region);

  if (gpu_vulkan_end_and_submit_cmd(cmd))
  {
    MemoryCopy(data, g_vulkan_state->download_staging_mapped, size);
  }
  else
  {
    // tec: the copy never completed
    MemoryZero(data, size);
  }
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
  B32 cpu_reads = (flags & (GPU_BufferFlag_Read | GPU_BufferFlag_ReadWrite)) != 0;

  if (!host_visible && !cpu_reads && g_vulkan_state->rebar_supported && size <= g_vulkan_state->rebar_heap_size)
  {
    VkMemoryPropertyFlags rebar_props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (gpu_vulkan_alloc_raw_buffer(size, usage, rebar_props, &result->buffer, &result->memory))
    {
      vkMapMemory(g_vulkan_state->device, result->memory, 0, size, 0, &result->mapped_ptr);
      if (data)
      {
        MemoryCopy(result->mapped_ptr, data, size);
      }
      return result;
    }
  }
  
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

internal GPU_Buffer*
gpu_buffer_alloc_pooled(String8 name, U64 size, GPU_BufferFlags flags, void* data)
{
  for (U32 i = 0; i < g_vulkan_state->pooled_buffer_count; i++)
  {
    GPU_PooledBuffer* slot = &g_vulkan_state->pooled_buffers[i];
    if (!str8_match(slot->name, name, 0)) continue;

    if (size > slot->capacity)
    {
      gpu_buffer_release(slot->buffer);
      slot->buffer = gpu_buffer_alloc(size, flags, data);
      // tec: only record the new capacity if the allocation actually succeeded
      slot->capacity = slot->buffer ? size : 0;
    }
    else
    {
      if (!slot->buffer)
      {
        return 0;
      }
      slot->buffer->size = size;
      if (data) gpu_buffer_write(slot->buffer, data, size);
    }
    return slot->buffer;
  }

  GPU_Buffer* buffer = gpu_buffer_alloc(size, flags, data);
  if (g_vulkan_state->pooled_buffer_count < GPU_VULKAN_MAX_POOLED_BUFFERS)
  {
    GPU_PooledBuffer* slot = &g_vulkan_state->pooled_buffers[g_vulkan_state->pooled_buffer_count++];
    slot->name = name;
    slot->buffer = buffer;
    slot->capacity = size;
  }
  else
  {
    log_error("gpu_buffer_alloc_pooled: pool full, '%.*s' will not be cached", str8_varg(name));
  }
  return buffer;
}

// tec: imports an existing host pointer (such as an OS mapped file view) directly as a VkBuffer's backing memory via VK_EXT_external_memory_host
internal GPU_Buffer*
gpu_buffer_import_host_readonly(void* host_ptr, U64 size)
{
  if (!g_vulkan_state->has_external_memory_host_ext || !host_ptr || size == 0)
  {
    return 0;
  }
  
  U64 align = g_vulkan_state->min_imported_host_pointer_alignment;
  U64 addr = (U64)host_ptr;
  U64 aligned_addr = addr & ~(align - 1);
  void* aligned_ptr = (void*)aligned_addr;
  U64 front_pad = addr - aligned_addr;
  U64 import_size = AlignPow2(front_pad + size, align);
  
  VkMemoryHostPointerPropertiesEXT host_props = 
  { 
    .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT 
  };
  
  if (g_vulkan_state->vkGetMemoryHostPointerPropertiesEXT_fn(g_vulkan_state->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, aligned_ptr, &host_props) != VK_SUCCESS)
  {
    return 0;
  }
  
  VkExternalMemoryBufferCreateInfo ext_buf_info = 
  { 
    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, 
    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
  };
  VkBufferCreateInfo buf_info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .pNext = &ext_buf_info,
    .size = import_size,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  
  VkBuffer buffer;
  if (vkCreateBuffer(g_vulkan_state->device, &buf_info, 0, &buffer) != VK_SUCCESS)
  {
    return 0;
  }
  
  VkMemoryRequirements mem_req;
  vkGetBufferMemoryRequirements(g_vulkan_state->device, buffer, &mem_req);
  
  U32 mem_type = gpu_vulkan_find_memory_type(mem_req.memoryTypeBits & host_props.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (mem_type == UINT32_MAX)
  {
    vkDestroyBuffer(g_vulkan_state->device, buffer, 0);
    return 0;
  }
  
  VkImportMemoryHostPointerInfoEXT import_info = 
  { 
    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, 
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, 
    .pHostPointer = aligned_ptr 
  };
  VkMemoryAllocateInfo alloc_info = 
  { 
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 
    .pNext = &import_info,
    .allocationSize = import_size,
    .memoryTypeIndex = mem_type
  };
  
  VkDeviceMemory memory;
  if (vkAllocateMemory(g_vulkan_state->device, &alloc_info, 0, &memory) != VK_SUCCESS)
  {
    vkDestroyBuffer(g_vulkan_state->device, buffer, 0);
    return 0;
  }
  
  if (vkBindBufferMemory(g_vulkan_state->device, buffer, memory, 0) != VK_SUCCESS)
  {
    vkDestroyBuffer(g_vulkan_state->device, buffer, 0);
    vkFreeMemory(g_vulkan_state->device, memory, 0);
    return 0;
  }
  
  GPU_Buffer* result = push_array(g_vulkan_state->arena, GPU_Buffer, 1);
  result->buffer = buffer;
  result->memory = memory;
  result->size = size;
  result->bind_offset = front_pad;
  return result;
}

internal GPU_Buffer*
gpu_buffer_import_host_readonly_pooled(String8 name, void* host_ptr, U64 size)
{
  for (U32 i = 0; i < g_vulkan_state->pooled_buffer_count; i++)
  {
    GPU_PooledBuffer* slot = &g_vulkan_state->pooled_buffers[i];
    if (!str8_match(slot->name, name, 0)) continue;

    if (slot->imported_host_ptr == host_ptr && slot->capacity == size)
    {
      return slot->buffer;
    }

    GPU_Buffer* fresh = gpu_buffer_import_host_readonly(host_ptr, size);
    if (fresh)
    {
      gpu_buffer_release(slot->buffer);
      slot->buffer = fresh;
      slot->capacity = size;
      slot->imported_host_ptr = host_ptr;
    }
    return slot->buffer;
  }

  GPU_Buffer* buffer = gpu_buffer_import_host_readonly(host_ptr, size);
  if (buffer)
  {
    if (g_vulkan_state->pooled_buffer_count < GPU_VULKAN_MAX_POOLED_BUFFERS)
    {
      GPU_PooledBuffer* slot = &g_vulkan_state->pooled_buffers[g_vulkan_state->pooled_buffer_count++];
      slot->name = name;
      slot->buffer = buffer;
      slot->capacity = size;
      slot->imported_host_ptr = host_ptr;
    }
    else
    {
      log_error("gpu_buffer_import_host_readonly_pooled: pool full, '%.*s' will not be cached", str8_varg(name));
    }
  }
  return buffer;
}

internal void
gpu_buffer_release(GPU_Buffer* buffer)
{
  if (!buffer)
  {
    return;
  }

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

  // tec: cached by name
  for (U32 i = 0; i < g_vulkan_state->kernel_cache_count; i++)
  {
    if (str8_match(g_vulkan_state->kernel_cache[i]->name, name, 0))
    {
      ProfEnd();
      return g_vulkan_state->kernel_cache[i];
    }
  }

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

  if (g_vulkan_state->kernel_cache_count < GPU_VULKAN_MAX_CACHED_KERNELS)
  {
    g_vulkan_state->kernel_cache[g_vulkan_state->kernel_cache_count++] = kernel;
  }
  else
  {
    log_error("gpu_kernel_alloc: kernel cache full (%u), '%.*s' will be recompiled every call",
              (U32)GPU_VULKAN_MAX_CACHED_KERNELS, str8_varg(name));
  }

  ProfEnd();
  return kernel;
}

internal void
gpu_kernel_release(GPU_Kernel *kernel)
{
  // do nothing, kernels are cached
}

internal void
gpu_kernel_set_arg_buffer(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer)
{
  if (index >= GPU_VULKAN_MAX_BOUND_BUFFERS)
  {
    log_error("gpu_kernel_set_arg_buffer: index %u exceeds GPU_VULKAN_MAX_BOUND_BUFFERS", index);
    return;
  }

  if (!buffer)
  {
    log_error("gpu_kernel_set_arg_buffer: buffer argument for index %u is NULL - a prior GPU buffer allocation failed", index);
    return;
  }

  VkDescriptorBufferInfo buffer_info =
  {
    .buffer = buffer->buffer,
    .offset = buffer->bind_offset,
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

  if (gpu_vulkan_end_and_submit_cmd(cmd))
  {
    U64 timestamps[2];
    vkGetQueryPoolResults(g_vulkan_state->device, g_vulkan_state->timestamp_query_pool, 0, 2, sizeof(timestamps), timestamps, sizeof(U64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    F64 elapsed_ns = (F64)(timestamps[1] - timestamps[0]) * (F64)g_vulkan_state->timestamp_period_ns;
    g_vulkan_state->last_kernel_time_microseconds = (U64)(elapsed_ns / 1000.0);
  }
}

//~ tec: batching

internal GPU_Batch*
gpu_batch_begin(U64 upload_bytes_needed, U64 download_bytes_needed)
{
  GPU_Batch* batch = &g_vulkan_state->active_batch;
  MemoryZeroStruct(batch);

  // tec: grow now. growing later could destory a recorded buffer
  if (upload_bytes_needed > 0)
  {
    gpu_vulkan_ensure_staging_capacity(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       &g_vulkan_state->upload_staging_buffer, &g_vulkan_state->upload_staging_memory,
                                       &g_vulkan_state->upload_staging_mapped, &g_vulkan_state->upload_staging_capacity,
                                       upload_bytes_needed);
  }
  if (download_bytes_needed > 0)
  {
    gpu_vulkan_ensure_staging_capacity(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                       &g_vulkan_state->download_staging_buffer, &g_vulkan_state->download_staging_memory,
                                       &g_vulkan_state->download_staging_mapped, &g_vulkan_state->download_staging_capacity,
                                       download_bytes_needed);
  }

  batch->cmd = gpu_vulkan_begin_one_time_cmd();
  return batch;
}

internal void
gpu_batch_buffer_write(GPU_Batch* batch, GPU_Buffer* buffer, void* data, U64 size)
{
  if (size == 0 || !data) return;

  if (buffer->mapped_ptr)
  {
    MemoryCopy(buffer->mapped_ptr, data, size);
    return;
  }

  if (batch->upload_cursor + size > g_vulkan_state->upload_staging_capacity)
  {
    log_error("gpu_batch_buffer_write: exceeded reserved upload staging capacity, dropping write");
    return;
  }

  MemoryCopy((U8*)g_vulkan_state->upload_staging_mapped + batch->upload_cursor, data, size);

  VkBufferCopy copy_region = { .srcOffset = batch->upload_cursor, .dstOffset = buffer->bind_offset, .size = size };
  vkCmdCopyBuffer(batch->cmd, g_vulkan_state->upload_staging_buffer, buffer->buffer, 1, &copy_region);

  batch->upload_cursor += size;
  batch->wrote_since_barrier = 1;
  batch->has_commands = 1;
}

internal void
gpu_batch_buffer_fill(GPU_Batch* batch, GPU_Buffer* buffer, U64 size, U32 value)
{
  if (size == 0) return;

  vkCmdFillBuffer(batch->cmd, buffer->buffer, buffer->bind_offset, size, value);

  batch->wrote_since_barrier = 1;
  batch->has_commands = 1;
}

internal void
gpu_batch_buffer_zero(GPU_Batch* batch, GPU_Buffer* buffer, U64 size)
{
  gpu_batch_buffer_fill(batch, buffer, size, 0);
}

internal void
gpu_batch_kernel_execute(GPU_Batch* batch, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size)
{
  if (batch->wrote_since_barrier || batch->dispatched_since_barrier)
  {
    VkMemoryBarrier barrier =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = (batch->wrote_since_barrier ? VK_ACCESS_TRANSFER_WRITE_BIT : 0) |
                       (batch->dispatched_since_barrier ? VK_ACCESS_SHADER_WRITE_BIT : 0),
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    VkPipelineStageFlags src_stage = (batch->wrote_since_barrier ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0) |
                                      (batch->dispatched_since_barrier ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0);
    vkCmdPipelineBarrier(batch->cmd, src_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, 0, 0, 0);
    batch->wrote_since_barrier = 0;
    batch->dispatched_since_barrier = 0;
  }

  U32 group_count = (global_work_size + local_work_size - 1) / local_work_size;

  if (!batch->had_dispatch)
  {
    vkCmdResetQueryPool(batch->cmd, g_vulkan_state->timestamp_query_pool, 0, 2);
    vkCmdWriteTimestamp(batch->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vulkan_state->timestamp_query_pool, 0);
  }
  else
  {
    vkCmdResetQueryPool(batch->cmd, g_vulkan_state->timestamp_query_pool, 1, 1);
  }

  vkCmdBindPipeline(batch->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->pipeline);
  vkCmdBindDescriptorSets(batch->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_vulkan_state->shared_pipeline_layout, 0, 1, &kernel->descriptor_set, 0, 0);
  vkCmdPushConstants(batch->cmd, g_vulkan_state->shared_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(kernel->push_constants), kernel->push_constants);

  vkCmdDispatch(batch->cmd, group_count, 1, 1);

  vkCmdWriteTimestamp(batch->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vulkan_state->timestamp_query_pool, 1);

  batch->dispatched_since_barrier = 1;
  batch->had_dispatch = 1;
  batch->has_commands = 1;
}

internal void
gpu_batch_buffer_read(GPU_Batch* batch, GPU_Buffer* buffer, void* out_data, U64 size)
{
  if (size == 0) return;
  if (batch->pending_read_count >= GPU_BATCH_MAX_PENDING_READS)
  {
    log_error("gpu_batch_buffer_read: too many pending reads in one batch (max %u), dropping read", (U32)GPU_BATCH_MAX_PENDING_READS);
    return;
  }

  GPU_PendingRead* pr = &batch->pending_reads[batch->pending_read_count];

  if (buffer->mapped_ptr)
  {
    pr->from_staging = 0;
    pr->mapped_src = buffer->mapped_ptr;
    pr->out_data = out_data;
    pr->size = size;
    batch->pending_read_count++;
    return;
  }

  if (batch->dispatched_since_barrier)
  {
    VkMemoryBarrier barrier =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, 0, 0, 0);
    batch->dispatched_since_barrier = 0;
  }

  if (batch->download_cursor + size > g_vulkan_state->download_staging_capacity)
  {
    log_error("gpu_batch_buffer_read: exceeded reserved download staging capacity, dropping read");
    return;
  }

  VkBufferCopy copy_region = { .srcOffset = buffer->bind_offset, .dstOffset = batch->download_cursor, .size = size };
  vkCmdCopyBuffer(batch->cmd, buffer->buffer, g_vulkan_state->download_staging_buffer, 1, &copy_region);

  pr->from_staging = 1;
  pr->staging_offset = batch->download_cursor;
  pr->out_data = out_data;
  pr->size = size;
  batch->pending_read_count++;

  batch->download_cursor += size;
  batch->has_commands = 1;
}

internal B32
gpu_batch_end(GPU_Batch* batch)
{
  B32 ok = 1;

  if (batch->has_commands)
  {
    ok = gpu_vulkan_end_and_submit_cmd(batch->cmd);

    if (ok && batch->had_dispatch)
    {
      U64 timestamps[2];
      vkGetQueryPoolResults(g_vulkan_state->device, g_vulkan_state->timestamp_query_pool, 0, 2, sizeof(timestamps), timestamps, sizeof(U64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      F64 elapsed_ns = (F64)(timestamps[1] - timestamps[0]) * (F64)g_vulkan_state->timestamp_period_ns;
      g_vulkan_state->last_kernel_time_microseconds = (U64)(elapsed_ns / 1000.0);
    }
  }
  else
  {
    vkEndCommandBuffer(batch->cmd);
  }

  for (U32 i = 0; i < batch->pending_read_count; i++)
  {
    GPU_PendingRead* pr = &batch->pending_reads[i];
    if (ok)
    {
      void* src = pr->from_staging ? (void*)((U8*)g_vulkan_state->download_staging_mapped + pr->staging_offset) : pr->mapped_src;
      MemoryCopy(pr->out_data, src, pr->size);
    }
    else
    {
      // tec: the copy/dispatch never completed
      MemoryZero(pr->out_data, pr->size);
    }
  }

  return ok;
}