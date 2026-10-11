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
    log_debug("[vulkan validation] %s", data->pMessage);
  }
  return VK_FALSE;
}

#ifndef CQL_VULKAN_VALIDATION
#define CQL_VULKAN_VALIDATION 0
#endif

internal B32
gpu_vulkan_validation_requested(void)
{
  return settings_bool(str8_lit("CQL_VULKAN_VALIDATION"), CQL_VULKAN_VALIDATION);
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

internal B32
gpu_vulkan_memory_type_is_device_local(U32 memory_type_index)
{
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(g_vulkan_state->physical_device, &mem_props);
  return (mem_props.memoryTypes[memory_type_index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
}

internal void
gpu_vulkan_init(void)
{
  ProfBeginFunction();
  
  Arena* arena = arena_alloc();
  g_vulkan_state = push_array(arena, GPU_VulkanState, 1);
  g_vulkan_state->arena = arena;
  
  U64 kernel_cache_cap = settings_u64(str8_lit("GPU_VULKAN_MAX_CACHED_KERNELS"), 32);
  if (kernel_cache_cap < 16)
  {
    log_error("settings: GPU_VULKAN_MAX_CACHED_KERNELS (%llu) is below the safe minimum (16) - using the minimum instead", kernel_cache_cap);
    kernel_cache_cap = 16;
  }
  g_vulkan_state->kernel_cache_cap = (U32)kernel_cache_cap;
  g_vulkan_state->kernel_cache = push_array(arena, GPU_VulkanKernel*, kernel_cache_cap);
  
  U64 pooled_buffer_cap = settings_u64(str8_lit("GPU_VULKAN_MAX_POOLED_BUFFERS"), 128);
  g_vulkan_state->pooled_buffer_cap = (U32)pooled_buffer_cap;
  g_vulkan_state->pooled_buffers = push_array(arena, GPU_PooledBuffer, pooled_buffer_cap);
  
  U64 hash_slots_cap = settings_u64(str8_lit("GPU_VULKAN_POOLED_BUFFER_HASH_SLOTS"), 256);
  if (!IsPow2(hash_slots_cap))
  {
    log_error("settings: GPU_VULKAN_POOLED_BUFFER_HASH_SLOTS (%llu) must be a power of two, using default (256)", hash_slots_cap);
    hash_slots_cap = 256;
  }
  g_vulkan_state->pooled_buffer_hash_slots_cap = (U32)hash_slots_cap;
  g_vulkan_state->pooled_buffer_hash_slots = push_array(arena, U32, hash_slots_cap);
  
  U64 mem_block_cap = settings_u64(str8_lit("GPU_VULKAN_MAX_MEM_BLOCKS"), 64);
  g_vulkan_state->mem_block_cap = (U32)mem_block_cap;
  g_vulkan_state->mem_blocks = push_array(arena, GPU_VulkanMemBlock, mem_block_cap);
  g_vulkan_state->mem_block_size = settings_u64(str8_lit("GPU_VULKAN_MEM_BLOCK_SIZE"), MB(1));
  
  U64 pending_read_cap = settings_u64(str8_lit("GPU_BATCH_MAX_PENDING_READS"), 8);
  if (pending_read_cap < 8)
  {
    log_error("settings: GPU_BATCH_MAX_PENDING_READS (%llu) is below the safe minimum (8) - using the minimum instead", pending_read_cap);
    pending_read_cap = 8;
  }
  g_vulkan_state->active_batch.pending_read_cap = (U32)pending_read_cap;
  g_vulkan_state->active_batch.pending_reads = push_array(arena, GPU_PendingRead, pending_read_cap);
  
  VkResult res;
  
  //- tec: instance
  VkApplicationInfo app_info =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "GPU SQL Engine",
    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
    .pEngineName = "cql",
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
    
    if (has_debug_utils_ext) 
    {
      enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
    if (has_validation_features_ext)
    {
      enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
    }
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
      log_info("CQL_VULKAN_VALIDATION requested but VK_LAYER_KHRONOS_validation was not found - install the Vulkan SDK's validation layer to enable it");
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
      U64 budget_percent = Min(settings_u64(str8_lit("GPU_VULKAN_REBAR_BUDGET_PERCENT"), 50), (U64)100);
      g_vulkan_state->rebar_budget = g_vulkan_state->rebar_heap_size / 100 * budget_percent;
      log_debug("Resizable BAR detected (%llu MB host-visible VRAM, %llu MB of it for buffers) - GPU buffer uploads will skip staging where possible",
                g_vulkan_state->rebar_heap_size / (1024 * 1024), g_vulkan_state->rebar_budget / (1024 * 1024));
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
      log_debug("VK_EXT_external_memory_host supported (alignment=%llu) - disk-backed numeric column reads will import mapped file views directly, skipping the upload copy", g_vulkan_state->min_imported_host_pointer_alignment);
    }
  }
  
  //- tec: copy queue
  U32 copy_queue_family_index = compute_queue_family_index;
  U32 copy_queue_index = 0;
  B32 copy_queue_separate = 0;
  if (settings_u64(str8_lit("GPU_VULKAN_COPY_QUEUE"), 1) != 0)
  {
    U32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(selected, &family_count, 0);
    VkQueueFamilyProperties* families = push_array(arena, VkQueueFamilyProperties, family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(selected, &family_count, families);
    for (U32 family = 0; family < family_count && !copy_queue_separate; family++)
    {
      VkQueueFlags flags = families[family].queueFlags;
      if ((flags & VK_QUEUE_TRANSFER_BIT) && !(flags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) && families[family].queueCount > 0)
      {
        copy_queue_family_index = family;
        copy_queue_separate = 1;
      }
    }
    if (!copy_queue_separate && families[compute_queue_family_index].queueCount >= 2)
    {
      copy_queue_index = 1;
      copy_queue_separate = 1;
    }
  }
  
  //- tec: logical device
  F32 priorities[2] = { 1.0f, 1.0f };
  VkDeviceQueueCreateInfo queue_infos[2] = {0};
  U32 queue_info_count = 1;
  queue_infos[0] = (VkDeviceQueueCreateInfo)
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = compute_queue_family_index,
    .queueCount = (copy_queue_separate && copy_queue_family_index == compute_queue_family_index) ? 2 : 1,
    .pQueuePriorities = priorities,
  };
  if (copy_queue_separate && copy_queue_family_index != compute_queue_family_index)
  {
    queue_infos[1] = (VkDeviceQueueCreateInfo)
    {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = copy_queue_family_index,
      .queueCount = 1,
      .pQueuePriorities = priorities,
    };
    queue_info_count = 2;
  }
  
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
    .queueCreateInfoCount = queue_info_count,
    .pQueueCreateInfos = queue_infos,
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
  g_vulkan_state->copy_queue_family_index = copy_queue_family_index;
  g_vulkan_state->copy_queue_separate = copy_queue_separate;
  if (copy_queue_separate)
  {
    vkGetDeviceQueue(g_vulkan_state->device, copy_queue_family_index, copy_queue_index, &g_vulkan_state->copy_queue);
    log_debug("gpu_vulkan: stream copies run on queue %u of family %u", copy_queue_index, copy_queue_family_index);
  }
  else
  {
    g_vulkan_state->copy_queue = g_vulkan_state->compute_queue;
  }
  
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
  
  if (copy_queue_separate && copy_queue_family_index != compute_queue_family_index)
  {
    pool_info.queueFamilyIndex = copy_queue_family_index;
    res = vkCreateCommandPool(g_vulkan_state->device, &pool_info, 0, &g_vulkan_state->copy_command_pool);
    if (res != VK_SUCCESS)
    {
      log_error("failed to create the Vulkan copy command pool");
      return;
    }
  }
  else
  {
    g_vulkan_state->copy_command_pool = g_vulkan_state->command_pool;
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
    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128 * GPU_VULKAN_MAX_BOUND_BUFFERS },
  };
  
  VkDescriptorPoolCreateInfo desc_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    .poolSizeCount = 1,
    .pPoolSizes = pool_sizes,
    .maxSets = 128,
  };
  
  res = vkCreateDescriptorPool(g_vulkan_state->device, &desc_pool_info, 0, &g_vulkan_state->descriptor_pool);
  if (res != VK_SUCCESS)
  {
    log_error("failed to create Vulkan descriptor pool");
    return;
  }
  
  //- tec: shared descriptor set layout
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
gpu_vulkan_release(void)
{
  gpu_vulkan_import_pool_stop();
  
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
  
  for (U32 i = 0; i < g_vulkan_state->mem_block_count; i++)
  {
    GPU_VulkanMemBlock* block = &g_vulkan_state->mem_blocks[i];
    if (block->mapped_ptr)
    {
      vkUnmapMemory(g_vulkan_state->device, block->memory);
    }
    vkFreeMemory(g_vulkan_state->device, block->memory, 0);
  }
  
  for (U32 i = 0; i < g_vulkan_state->kernel_cache_count; i++)
  {
    GPU_VulkanKernel* kernel = g_vulkan_state->kernel_cache[i];
    vkDestroyPipeline(g_vulkan_state->device, kernel->pipeline, 0);
    vkDestroyShaderModule(g_vulkan_state->device, kernel->shader, 0);
  }
  
  vkDestroyFence(g_vulkan_state->device, g_vulkan_state->submit_fence, 0);
  vkDestroyQueryPool(g_vulkan_state->device, g_vulkan_state->timestamp_query_pool, 0);
  vkDestroyPipelineLayout(g_vulkan_state->device, g_vulkan_state->shared_pipeline_layout, 0);
  vkDestroyDescriptorSetLayout(g_vulkan_state->device, g_vulkan_state->shared_descriptor_set_layout, 0);
  vkDestroyDescriptorPool(g_vulkan_state->device, g_vulkan_state->descriptor_pool, 0);
  if (g_vulkan_state->copy_command_pool != g_vulkan_state->command_pool)
  {
    vkDestroyCommandPool(g_vulkan_state->device, g_vulkan_state->copy_command_pool, 0);
  }
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
gpu_vulkan_wait(void)
{
  vkQueueWaitIdle(g_vulkan_state->compute_queue);
}

internal U64
gpu_vulkan_device_total_memory(void)
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
gpu_vulkan_device_max_storage_buffer_range(void)
{
  VkPhysicalDeviceProperties device_props;
  vkGetPhysicalDeviceProperties(g_vulkan_state->physical_device, &device_props);
  return device_props.limits.maxStorageBufferRange;
}

//~ tec: streaming

internal void
gpu_vulkan_release_import_objects(VkBuffer buffer, VkDeviceMemory memory)
{
  vkDestroyBuffer(g_vulkan_state->device, buffer, 0);
  vkFreeMemory(g_vulkan_state->device, memory, 0);
}

internal void
gpu_vulkan_import_worker_main(void* raw)
{
  TCTX tctx_;
  tctx_init_and_equip(&tctx_);
  GPU_VulkanImportPool* pool = (GPU_VulkanImportPool*)raw;
  for (;;)
  {
    os_semaphore_take(pool->work_semaphore, max_U64);
    if (pool->stop)
    {
      break;
    }
    
    GPU_VulkanImportJob* job = 0;
    OS_MutexScope(pool->mutex)
    {
      job = &pool->jobs[pool->queue[pool->queue_head]];
      pool->queue_head = (pool->queue_head + 1) % GPU_VULKAN_STREAM_JOB_CAP;
      pool->queue_count -= 1;
    }
    
    if (job->is_release)
    {
      gpu_vulkan_release_import_objects(job->release_buffer, job->release_memory);
      os_semaphore_drop(pool->release_semaphore);
    }
    else
    {
      job->ok = gpu_vulkan_import_host_into(job->buffer, job->host_ptr, job->size);
      os_semaphore_drop(job->done);
    }
  }
}

internal void
gpu_vulkan_import_pool_start(void)
{
  GPU_VulkanImportPool* pool = &g_import_pool;
  if (pool->started)
  {
    return;
  }
  pool->started = 1;
  
  U64 wanted = settings_u64(str8_lit("GPU_VULKAN_IMPORT_THREADS"), 3);
  pool->thread_count = (U32)Min(wanted, (U64)GPU_VULKAN_STREAM_MAX_WORKERS);
  pool->mutex = os_mutex_alloc();
  pool->work_semaphore = os_semaphore_alloc(0, GPU_VULKAN_STREAM_JOB_CAP, str8_zero());
  pool->release_semaphore = os_semaphore_alloc(0, GPU_VULKAN_STREAM_JOB_CAP, str8_zero());
  for (U32 index = 0; index < GPU_VULKAN_STREAM_JOB_CAP; index++)
  {
    pool->jobs[index].done = os_semaphore_alloc(0, 1, str8_zero());
  }
  for (U32 index = 0; index < pool->thread_count; index++)
  {
    pool->threads[index] = os_thread_launch(gpu_vulkan_import_worker_main, pool, 0);
  }
}

internal void
gpu_vulkan_import_pool_stop(void)
{
  GPU_VulkanImportPool* pool = &g_import_pool;
  if (!pool->started)
  {
    return;
  }
  gpu_vulkan_stream_finish_releases();
  pool->stop = 1;
  for (U32 index = 0; index < pool->thread_count; index++)
  {
    os_semaphore_drop(pool->work_semaphore);
  }
  for (U32 index = 0; index < pool->thread_count; index++)
  {
    os_thread_join(pool->threads[index], max_U64);
  }
  MemoryZeroStruct(pool);
}

// tec: reserves a job, 0 if the pool is not running or is full. the caller then does the work itself
internal GPU_VulkanImportJob*
gpu_vulkan_import_job_reserve(B32 is_release)
{
  GPU_VulkanImportPool* pool = &g_import_pool;
  if (!pool->started || pool->thread_count == 0 || pool->queue_count >= GPU_VULKAN_STREAM_JOB_CAP / 2)
  {
    return 0;
  }
  GPU_VulkanImportJob* job = &pool->jobs[pool->next_job];
  pool->next_job = (pool->next_job + 1) % GPU_VULKAN_STREAM_JOB_CAP;
  job->is_release = is_release;
  job->ok = 0;
  return job;
}

internal void
gpu_vulkan_import_job_submit(GPU_VulkanImportJob* job)
{
  GPU_VulkanImportPool* pool = &g_import_pool;
  OS_MutexScope(pool->mutex)
  {
    pool->queue[(pool->queue_head + pool->queue_count) % GPU_VULKAN_STREAM_JOB_CAP] = (U32)(job - pool->jobs);
    pool->queue_count += 1;
  }
  os_semaphore_drop(pool->work_semaphore);
}

// tec: waits for every import started for the slot, true if they all worked
internal B32
gpu_vulkan_stream_join_imports(GPU_VulkanStreamSlot* slot)
{
  B32 ok = 1;
  for (U32 index = 0; index < slot->pending_copy_count; index++)
  {
    GPU_VulkanStreamPendingCopy* pending = &slot->pending_copies[index];
    if (pending->job)
    {
      os_semaphore_take(pending->job->done, max_U64);
      ok = ok && pending->job->ok;
      pending->job = 0;
    }
  }
  return ok;
}

// tec: gives the imports of a slot to the workers to unpin, or does it here when they cannot. only imports that worked have objects to release
internal void
gpu_vulkan_stream_release_imports(GPU_VulkanStreamSlot* slot)
{
  for (U32 index = 0; index < slot->import_count; index++)
  {
    if (slot->imports[index].buffer == 0)
    {
      continue;
    }
    GPU_VulkanImportJob* job = gpu_vulkan_import_job_reserve(1);
    if (job)
    {
      job->release_buffer = slot->imports[index].buffer;
      job->release_memory = slot->imports[index].memory;
      g_import_pool.releases_outstanding += 1;
      gpu_vulkan_import_job_submit(job);
    }
    else
    {
      gpu_vulkan_release_import_objects(slot->imports[index].buffer, slot->imports[index].memory);
    }
  }
  slot->import_count = 0;
}

internal void
gpu_vulkan_stream_finish_releases(void)
{
  while (g_import_pool.releases_outstanding > 0)
  {
    os_semaphore_take(g_import_pool.release_semaphore, max_U64);
    g_import_pool.releases_outstanding -= 1;
  }
}

internal GPU_Stream*
gpu_vulkan_stream_alloc(U32 slot_count)
{
  if (slot_count == 0 || slot_count > GPU_VULKAN_STREAM_MAX_SLOTS || g_vulkan_state->device_lost)
  {
    return 0;
  }
  gpu_vulkan_import_pool_start();
  gpu_vulkan_stream_finish_releases();
  
  GPU_VulkanStream* stream = push_array(g_vulkan_state->arena, GPU_VulkanStream, 1);
  stream->slot_count = slot_count;
  stream->stats.separate_copy_queue = g_vulkan_state->copy_queue_separate;
  
  VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
  VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
  
  for (U32 index = 0; index < slot_count; index++)
  {
    GPU_VulkanStreamSlot* slot = &stream->slots[index];
    
    VkCommandBufferAllocateInfo copy_alloc =
    {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = g_vulkan_state->copy_command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
    };
    VkCommandBufferAllocateInfo compute_alloc = copy_alloc;
    compute_alloc.commandPool = g_vulkan_state->command_pool;
    
    VkDescriptorSetAllocateInfo set_alloc =
    {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = g_vulkan_state->descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &g_vulkan_state->shared_descriptor_set_layout,
    };
    
    B32 ok = vkAllocateCommandBuffers(g_vulkan_state->device, &copy_alloc, &slot->copy_cmd) == VK_SUCCESS &&
      vkAllocateCommandBuffers(g_vulkan_state->device, &compute_alloc, &slot->compute_cmd) == VK_SUCCESS &&
      vkCreateSemaphore(g_vulkan_state->device, &semaphore_info, 0, &slot->copy_done) == VK_SUCCESS &&
      vkCreateFence(g_vulkan_state->device, &fence_info, 0, &slot->done) == VK_SUCCESS &&
      vkAllocateDescriptorSets(g_vulkan_state->device, &set_alloc, &slot->set) == VK_SUCCESS;
    if (!ok)
    {
      log_error("gpu_vulkan_stream_alloc: failed to create the objects of slot %u", index);
      stream->slot_count = index + 1;
      gpu_vulkan_stream_release((GPU_Stream*)stream);
      return 0;
    }
  }
  return (GPU_Stream*)stream;
}

internal void
gpu_vulkan_stream_release(GPU_Stream* stream_handle)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  if (!stream)
  {
    return;
  }
  
  for (U32 index = 0; index < stream->slot_count; index++)
  {
    GPU_VulkanStreamSlot* slot = &stream->slots[index];
    if (slot->pending && slot->done)
    {
      vkWaitForFences(g_vulkan_state->device, 1, &slot->done, VK_TRUE, GPU_VULKAN_STREAM_TIMEOUT_NS);
    }
    gpu_vulkan_stream_join_imports(slot);
    slot->pending_copy_count = 0;
  }
  
  for (U32 index = 0; index < stream->slot_count; index++)
  {
    GPU_VulkanStreamSlot* slot = &stream->slots[index];
    if (slot->copy_cmd)
    {
      vkFreeCommandBuffers(g_vulkan_state->device, g_vulkan_state->copy_command_pool, 1, &slot->copy_cmd);
    }
    if (slot->compute_cmd)
    {
      vkFreeCommandBuffers(g_vulkan_state->device, g_vulkan_state->command_pool, 1, &slot->compute_cmd);
    }
    if (slot->copy_done)
    {
      vkDestroySemaphore(g_vulkan_state->device, slot->copy_done, 0);
    }
    if (slot->done)
    {
      vkDestroyFence(g_vulkan_state->device, slot->done, 0);
    }
    if (slot->set)
    {
      vkFreeDescriptorSets(g_vulkan_state->device, g_vulkan_state->descriptor_pool, 1, &slot->set);
    }
    if (slot->staging_capacity != 0)
    {
      vkUnmapMemory(g_vulkan_state->device, slot->staging_memory);
      vkDestroyBuffer(g_vulkan_state->device, slot->staging, 0);
      vkFreeMemory(g_vulkan_state->device, slot->staging_memory, 0);
    }
    if (slot->readback_capacity != 0)
    {
      vkUnmapMemory(g_vulkan_state->device, slot->readback_memory);
      vkDestroyBuffer(g_vulkan_state->device, slot->readback, 0);
      vkFreeMemory(g_vulkan_state->device, slot->readback_memory, 0);
    }
    gpu_vulkan_stream_release_imports(slot);
  }
  // tec: unpinning the last imports is not waited for, it is waited for before the next stream, the next query and the pool stopping
}

internal B32
gpu_vulkan_stream_wait(GPU_Stream* stream_handle, U32 slot_index)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  GPU_VulkanStreamSlot* slot = &stream->slots[slot_index];
  if (!slot->pending)
  {
    return 1;
  }
  
  U64 wait_start = os_now_microseconds();
  VkResult result = vkWaitForFences(g_vulkan_state->device, 1, &slot->done, VK_TRUE, GPU_VULKAN_STREAM_TIMEOUT_NS);
  if (result != VK_SUCCESS)
  {
    log_error("gpu_vulkan_stream_wait: vkWaitForFences failed with VkResult %d after %llu microseconds - GPU hung or device lost. Aborting.",
              (S32)result, os_now_microseconds() - wait_start);
    os_abort(1);
  }
  slot->pending = 0;
  g_vulkan_state->submit_count += 1;
  
  for (U32 index = 0; index < slot->read_count; index++)
  {
    GPU_PendingRead* read = &slot->reads[index];
    MemoryCopy(read->out_data, (U8*)slot->readback_mapped + read->staging_offset, read->size);
  }
  slot->read_count = 0;
  return 1;
}

internal B32
gpu_vulkan_stream_slot_begin(GPU_Stream* stream_handle, U32 slot_index, U64 upload_bytes, U64 readback_bytes)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  if (!stream || slot_index >= stream->slot_count || g_vulkan_state->device_lost)
  {
    return 0;
  }
  GPU_VulkanStreamSlot* slot = &stream->slots[slot_index];
  
  gpu_vulkan_stream_wait(stream_handle, slot_index);
  
  // tec: imports of a chunk that was begun but never submitted must finish before their storage is reused
  gpu_vulkan_stream_join_imports(slot);
  slot->pending_copy_count = 0;
  slot->copy_recording = 0;
  slot->compute_recording = 0;
  slot->wrote_since_barrier = 0;
  slot->dispatched_since_barrier = 0;
  slot->staging_cursor = 0;
  slot->readback_cursor = 0;
  slot->read_count = 0;
  gpu_vulkan_stream_release_imports(slot);
  
  // tec: the slot is idle so its buffers can grow. 
  // a device that can import host memory copies from the column in place and needs no staging
  if (upload_bytes > 0 && !g_vulkan_state->has_external_memory_host_ext)
  {
    gpu_vulkan_ensure_staging_capacity(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       &slot->staging, &slot->staging_memory, &slot->staging_mapped, &slot->staging_capacity, upload_bytes);
    if (slot->staging_capacity < upload_bytes)
    {
      return 0;
    }
  }
  if (readback_bytes > 0)
  {
    gpu_vulkan_ensure_staging_capacity(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                       &slot->readback, &slot->readback_memory, &slot->readback_mapped, &slot->readback_capacity, readback_bytes);
    if (slot->readback_capacity < readback_bytes)
    {
      return 0;
    }
  }
  return 1;
}

internal VkCommandBuffer
gpu_vulkan_stream_copy_cmd(GPU_VulkanStreamSlot* slot)
{
  if (!slot->copy_recording)
  {
    vkResetCommandBuffer(slot->copy_cmd, 0);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(slot->copy_cmd, &begin_info);
    slot->copy_recording = 1;
  }
  return slot->copy_cmd;
}

internal VkCommandBuffer
gpu_vulkan_stream_compute_cmd(GPU_VulkanStreamSlot* slot)
{
  if (!slot->compute_recording)
  {
    vkResetCommandBuffer(slot->compute_cmd, 0);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(slot->compute_cmd, &begin_info);
    slot->compute_recording = 1;
  }
  return slot->compute_cmd;
}

internal void
gpu_vulkan_stream_record_copy(GPU_VulkanStream* stream, GPU_VulkanStreamSlot* slot, VkBuffer source, U64 source_offset, GPU_VulkanBuffer* destination, U64 size)
{
  VkBufferCopy region = { .srcOffset = source_offset, .dstOffset = destination->bind_offset, .size = size };
  vkCmdCopyBuffer(gpu_vulkan_stream_copy_cmd(slot), source, destination->buffer, 1, &region);
  stream->stats.bytes_copied += size;
}

// tec: copies host memory into destination. with host import a worker imports it and the copy is recorded at submit.
// otherwise it goes through the slot's staging buffer now
internal B32
gpu_vulkan_stream_copy(GPU_Stream* stream_handle, U32 slot_index, GPU_Buffer* destination_handle, void* host_ptr, U64 size)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  GPU_VulkanBuffer* destination = (GPU_VulkanBuffer*)destination_handle;
  GPU_VulkanStreamSlot* slot = &stream->slots[slot_index];
  if (size == 0)
  {
    return 1;
  }
  if (!destination || size > destination->size || g_vulkan_state->device_lost)
  {
    return 0;
  }
  
  if (g_vulkan_state->has_external_memory_host_ext && slot->import_count < GPU_VULKAN_STREAM_MAX_IMPORTS)
  {
    GPU_VulkanBuffer* import = &slot->imports[slot->import_count];
    MemoryZeroStruct(import);
    GPU_VulkanImportJob* job = gpu_vulkan_import_job_reserve(0);
    U64 import_start = os_now_microseconds();
    if (job)
    {
      job->host_ptr = host_ptr;
      job->size = size;
      job->buffer = import;
      gpu_vulkan_import_job_submit(job);
    }
    else if (!gpu_vulkan_import_host_into(import, host_ptr, size))
    {
      return 0;
    }
    
    GPU_VulkanStreamPendingCopy* pending = &slot->pending_copies[slot->pending_copy_count++];
    pending->destination = destination;
    pending->source = import;
    pending->job = job;
    pending->size = size;
    slot->import_count += 1;
    stream->stats.imports_created += 1;
    stream->stats.import_microseconds += os_now_microseconds() - import_start;
    return 1;
  }
  
  if (slot->staging_cursor + size > slot->staging_capacity)
  {
    log_error("gpu_vulkan_stream_copy: %llu bytes do not fit the slot's staging space (%llu of %llu used) - give slot_begin the bytes it will copy",
              size, slot->staging_cursor, slot->staging_capacity);
    return 0;
  }
  MemoryCopy((U8*)slot->staging_mapped + slot->staging_cursor, host_ptr, size);
  gpu_vulkan_stream_record_copy(stream, slot, slot->staging, slot->staging_cursor, destination, size);
  slot->staging_cursor += size;
  stream->stats.staged_bytes += size;
  return 1;
}

internal void
gpu_vulkan_stream_barrier(GPU_VulkanStreamSlot* slot, VkCommandBuffer cmd, VkAccessFlags dst_access, VkPipelineStageFlags dst_stage)
{
  if (!slot->wrote_since_barrier && !slot->dispatched_since_barrier)
  {
    return;
  }
  VkMemoryBarrier barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = (slot->wrote_since_barrier ? VK_ACCESS_TRANSFER_WRITE_BIT : 0) | (slot->dispatched_since_barrier ? VK_ACCESS_SHADER_WRITE_BIT : 0),
    .dstAccessMask = dst_access,
  };
  VkPipelineStageFlags src_stage = (slot->wrote_since_barrier ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0) |
  (slot->dispatched_since_barrier ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0);
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &barrier, 0, 0, 0, 0);
  slot->wrote_since_barrier = 0;
  slot->dispatched_since_barrier = 0;
}

internal void
gpu_vulkan_stream_zero(GPU_Stream* stream_handle, U32 slot_index, GPU_Buffer* buffer_handle, U64 size)
{
  gpu_vulkan_stream_fill(stream_handle, slot_index, buffer_handle, size, 0);
}

internal void
gpu_vulkan_stream_fill(GPU_Stream* stream_handle, U32 slot_index, GPU_Buffer* buffer_handle, U64 size, U32 value)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  GPU_VulkanStreamSlot* slot = &stream->slots[slot_index];
  if (size == 0)
  {
    return;
  }
  VkCommandBuffer cmd = gpu_vulkan_stream_compute_cmd(slot);
  vkCmdFillBuffer(cmd, buffer->buffer, buffer->bind_offset, size, value);
  slot->wrote_since_barrier = 1;
}

internal void
gpu_vulkan_stream_dispatch(GPU_Stream* stream_handle, U32 slot_index, GPU_Kernel* kernel_handle, U32 global_work_size, U32 local_work_size)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  GPU_VulkanKernel* kernel = (GPU_VulkanKernel*)kernel_handle;
  GPU_VulkanStreamSlot* slot = &stream->slots[slot_index];
  VkCommandBuffer cmd = gpu_vulkan_stream_compute_cmd(slot);
  
  // tec: the kernel's bound buffers are copied into the slot's own descriptor set, so the next chunk can rebind the kernel while this one is in flight
  VkDescriptorBufferInfo infos[GPU_VULKAN_MAX_BOUND_BUFFERS];
  VkWriteDescriptorSet writes[GPU_VULKAN_MAX_BOUND_BUFFERS];
  U32 write_count = 0;
  for (U32 binding = 0; binding < kernel->bound_buffer_count; binding++)
  {
    GPU_VulkanBuffer* bound = kernel->bound_buffers[binding];
    if (!bound)
    {
      continue;
    }
    infos[write_count] = (VkDescriptorBufferInfo){ .buffer = bound->buffer, .offset = bound->bind_offset, .range = bound->size };
    writes[write_count] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = slot->set,
      .dstBinding = binding,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &infos[write_count],
    };
    write_count += 1;
  }
  vkUpdateDescriptorSets(g_vulkan_state->device, write_count, writes, 0, 0);
  
  gpu_vulkan_stream_barrier(slot, cmd, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_vulkan_state->shared_pipeline_layout, 0, 1, &slot->set, 0, 0);
  vkCmdPushConstants(cmd, g_vulkan_state->shared_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(kernel->push_constants), kernel->push_constants);
  vkCmdDispatch(cmd, (global_work_size + local_work_size - 1) / local_work_size, 1, 1);
  slot->dispatched_since_barrier = 1;
}

internal void
gpu_vulkan_stream_read(GPU_Stream* stream_handle, U32 slot_index, GPU_Buffer* buffer_handle, void* out_data, U64 size)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  GPU_VulkanStreamSlot* slot = &stream->slots[slot_index];
  if (size == 0)
  {
    return;
  }
  if (slot->read_count >= GPU_VULKAN_STREAM_MAX_READS || slot->readback_cursor + size > slot->readback_capacity)
  {
    log_error("gpu_vulkan_stream_read: read of %llu bytes does not fit the slot (%u reads, %llu of %llu bytes used), dropping it",
              size, slot->read_count, slot->readback_cursor, slot->readback_capacity);
    MemoryZero(out_data, size);
    return;
  }
  
  VkCommandBuffer cmd = gpu_vulkan_stream_compute_cmd(slot);
  gpu_vulkan_stream_barrier(slot, cmd, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferCopy region = { .srcOffset = buffer->bind_offset, .dstOffset = slot->readback_cursor, .size = size };
  vkCmdCopyBuffer(cmd, buffer->buffer, slot->readback, 1, &region);
  
  GPU_PendingRead* read = &slot->reads[slot->read_count++];
  read->staging_offset = slot->readback_cursor;
  read->out_data = out_data;
  read->size = size;
  slot->readback_cursor = AlignPow2(slot->readback_cursor + size, 16);
}

internal B32
gpu_vulkan_stream_submit(GPU_Stream* stream_handle, U32 slot_index)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  GPU_VulkanStreamSlot* slot = &stream->slots[slot_index];
  if (g_vulkan_state->device_lost || !slot->compute_recording)
  {
    return 0;
  }
  
  // tec: the imports started when the copies were asked for are due now
  U64 join_start = os_now_microseconds();
  B32 imports_ok = gpu_vulkan_stream_join_imports(slot);
  stream->stats.import_microseconds += os_now_microseconds() - join_start;
  if (imports_ok)
  {
    for (U32 index = 0; index < slot->pending_copy_count; index++)
    {
      GPU_VulkanStreamPendingCopy* pending = &slot->pending_copies[index];
      gpu_vulkan_stream_record_copy(stream, slot, pending->source->buffer, pending->source->bind_offset, pending->destination, pending->size);
    }
  }
  else
  {
    log_error("gpu_vulkan_stream_submit: importing host memory failed");
  }
  slot->pending_copy_count = 0;
  if (!imports_ok)
  {
    vkEndCommandBuffer(slot->compute_cmd);
    if (slot->copy_recording)
    {
      vkEndCommandBuffer(slot->copy_cmd);
    }
    slot->copy_recording = 0;
    slot->compute_recording = 0;
    return 0;
  }
  
  B32 has_copy = slot->copy_recording;
  if (has_copy)
  {
    vkEndCommandBuffer(slot->copy_cmd);
    VkSubmitInfo copy_submit =
    {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &slot->copy_cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &slot->copy_done,
    };
    VkResult copy_result = vkQueueSubmit(g_vulkan_state->copy_queue, 1, &copy_submit, 0);
    if (copy_result != VK_SUCCESS)
    {
      gpu_vulkan_note_result(copy_result);
      log_error("gpu_vulkan_stream_submit: copy submit failed with VkResult %d", (S32)copy_result);
      vkEndCommandBuffer(slot->compute_cmd);
      slot->copy_recording = 0;
      slot->compute_recording = 0;
      return 0;
    }
  }
  
  if (slot->read_count > 0)
  {
    VkMemoryBarrier host_barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(slot->compute_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, 0, 0, 0);
  }
  vkEndCommandBuffer(slot->compute_cmd);
  
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  VkSubmitInfo compute_submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount = has_copy ? 1 : 0,
    .pWaitSemaphores = has_copy ? &slot->copy_done : 0,
    .pWaitDstStageMask = has_copy ? &wait_stage : 0,
    .commandBufferCount = 1,
    .pCommandBuffers = &slot->compute_cmd,
  };
  vkResetFences(g_vulkan_state->device, 1, &slot->done);
  VkResult result = vkQueueSubmit(g_vulkan_state->compute_queue, 1, &compute_submit, slot->done);
  slot->copy_recording = 0;
  slot->compute_recording = 0;
  if (result != VK_SUCCESS)
  {
    gpu_vulkan_note_result(result);
    log_error("gpu_vulkan_stream_submit: compute submit failed with VkResult %d", (S32)result);
    return 0;
  }
  slot->pending = 1;
  return 1;
}

internal GPU_StreamStats
gpu_vulkan_stream_get_stats(GPU_Stream* stream_handle)
{
  GPU_VulkanStream* stream = (GPU_VulkanStream*)stream_handle;
  return stream->stats;
}

//~ tec: memory
internal U64
gpu_vulkan_device_free_memory(void)
{
  if (!g_vulkan_state->has_memory_budget_ext)
  {
    return gpu_vulkan_device_total_memory();
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
gpu_vulkan_get_executed_kernel_time_microseconds(void)
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
gpu_vulkan_device_lost(void)
{
  return g_vulkan_state->device_lost;
}

internal void
gpu_vulkan_vram_charge(U64 size)
{
  g_vulkan_state->vram_bytes_in_use += size;
  g_vulkan_state->vram_bytes_peak = Max(g_vulkan_state->vram_bytes_peak, g_vulkan_state->vram_bytes_in_use);
}

internal void
gpu_vulkan_vram_uncharge(U64 size)
{
  g_vulkan_state->vram_bytes_in_use -= Min(size, g_vulkan_state->vram_bytes_in_use);
}

internal void
gpu_vulkan_pool_begin_epoch(void)
{
  gpu_vulkan_stream_finish_releases();
  g_vulkan_state->pool_epoch += 1;
}

internal GPU_MemoryStats
gpu_vulkan_memory_stats(void)
{
  GPU_MemoryStats stats = {0};
  stats.bytes_in_use = g_vulkan_state->vram_bytes_in_use;
  stats.bytes_peak = g_vulkan_state->vram_bytes_peak;
  stats.alloc_failures = g_vulkan_state->alloc_failure_count;
  stats.alloc_retry_successes = g_vulkan_state->alloc_retry_success_count;
  stats.pool_evictions = g_vulkan_state->pool_eviction_count;
  stats.pool_evicted_bytes = g_vulkan_state->pool_evicted_bytes;
  return stats;
}

// tec: frees pooled buffers no running query has looked up, least recently used first, until bytes_wanted came back. returns the bytes freed
internal U64
gpu_vulkan_pool_evict(U64 bytes_wanted)
{
  if (g_vulkan_state->active_batch.recording && g_vulkan_state->active_batch.has_commands)
  {
    return 0;
  }
  
  U64 freed = 0;
  while (freed < bytes_wanted)
  {
    GPU_PooledBuffer* victim = 0;
    for (U32 i = 0; i < g_vulkan_state->pooled_buffer_count; i++)
    {
      GPU_PooledBuffer* slot = &g_vulkan_state->pooled_buffers[i];
      if (!slot->buffer || slot->buffer->vram_charge == 0 || slot->last_epoch >= g_vulkan_state->pool_epoch)
      {
        continue;
      }
      if (!victim || slot->last_epoch < victim->last_epoch)
      {
        victim = slot;
      }
    }
    if (!victim)
    {
      break;
    }
    
    U64 charge = victim->buffer->vram_charge;
    log_debug("gpu_vulkan_pool_evict: '%.*s' (%llu bytes, last used in epoch %llu of %llu)", str8_varg(victim->name), charge, victim->last_epoch, g_vulkan_state->pool_epoch);
    gpu_vulkan_buffer_release((GPU_Buffer*)victim->buffer);
    victim->buffer = 0;
    victim->capacity = 0;
    victim->imported_host_ptr = 0;
    freed += charge;
    g_vulkan_state->pool_eviction_count += 1;
    g_vulkan_state->pool_evicted_bytes += charge;
  }
  return freed;
}

internal VkResult
gpu_vulkan_allocate_memory(VkMemoryAllocateInfo* alloc_info, B32 device_local, VkDeviceMemory* out_memory)
{
  U64 size = alloc_info->allocationSize;
  
  if (device_local && g_vulkan_state->has_memory_budget_ext && size >= MB(1))
  {
    U64 free_bytes = gpu_vulkan_device_free_memory();
    if (size > free_bytes)
    {
      U64 freed = gpu_vulkan_pool_evict(size - free_bytes);
      if (freed < size - free_bytes && g_gpu_evict_hook)
      {
        g_gpu_evict_hook(size - free_bytes - freed);
      }
    }
  }
  
  VkResult result = vkAllocateMemory(g_vulkan_state->device, alloc_info, 0, out_memory);
  if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)
  {
    U64 freed = gpu_vulkan_pool_evict(size);
    if (freed < size && g_gpu_evict_hook)
    {
      freed += g_gpu_evict_hook(size - freed);
    }
    if (freed > 0)
    {
      result = vkAllocateMemory(g_vulkan_state->device, alloc_info, 0, out_memory);
      if (result == VK_SUCCESS)
      {
        g_vulkan_state->alloc_retry_success_count += 1;
        log_info("gpu_vulkan: allocation of %llu bytes succeeded after evicting %llu bytes of pooled buffers", size, freed);
      }
    }
  }
  
  if (result != VK_SUCCESS)
  {
    gpu_vulkan_note_result(result);
    g_vulkan_state->alloc_failure_count += 1;
    log_error("gpu_vulkan: vkAllocateMemory of %llu bytes (%s) failed with VkResult %d. this process holds %llu bytes of device memory, driver reports %llu free. "
              "pooled buffers used by the running query cannot be evicted",
              size, device_local ? "device local" : "host visible", (S32)result, g_vulkan_state->vram_bytes_in_use, gpu_vulkan_device_free_memory());
  }
  return result;
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
gpu_vulkan_end_and_submit_cmd_tagged(VkCommandBuffer cmd, const char* tag)
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
    log_error("vkQueueSubmit failed with VkResult %d - not waiting on the fence", (S32)submit_result);
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
  
  g_vulkan_state->submit_count += 1;
  log_debug("submit+wait wall time [%s]: %llu microseconds", tag, os_now_microseconds() - t0);
  return 1;
}

internal B32
gpu_vulkan_alloc_raw_buffer(U64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props, VkBuffer* out_buffer, VkDeviceMemory* out_memory)
{
  if (g_vulkan_state->device_lost)
  {
    return 0;
  }
  
  U64 raw_alloc_t0 = os_now_microseconds();
  
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
  
  VkResult alloc_result = gpu_vulkan_allocate_memory(&alloc_info, (mem_props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0, out_memory);
  if (alloc_result != VK_SUCCESS)
  {
    vkDestroyBuffer(g_vulkan_state->device, *out_buffer, 0);
    return 0;
  }
  
  vkBindBufferMemory(g_vulkan_state->device, *out_buffer, *out_memory, 0);
  log_debug("gpu_vulkan_alloc_raw_buffer (create+alloc+bind) size=%llu: %llu microseconds", size, os_now_microseconds() - raw_alloc_t0);
  return 1;
}

internal B32
gpu_vulkan_alloc_dedicated_memory(GPU_VulkanBuffer* result, VkMemoryRequirements* mem_req, U32 mem_type, B32 wants_mapped)
{
  VkMemoryAllocateInfo alloc_info =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = mem_req->size,
    .memoryTypeIndex = mem_type,
  };
  
  U64 dedicated_t0 = os_now_microseconds();
  B32 device_local = gpu_vulkan_memory_type_is_device_local(mem_type);
  VkResult alloc_result = gpu_vulkan_allocate_memory(&alloc_info, device_local, &result->memory);
  if (alloc_result != VK_SUCCESS)
  {
    return 0;
  }
  log_debug("gpu_vulkan_alloc_dedicated_memory size=%llu: vkAllocateMemory=%llu microseconds", mem_req->size, os_now_microseconds() - dedicated_t0);
  
  vkBindBufferMemory(g_vulkan_state->device, result->buffer, result->memory, 0);
  result->owns_memory = 1;
  if (device_local)
  {
    result->vram_charge = mem_req->size;
    gpu_vulkan_vram_charge(mem_req->size);
  }
  
  if (wants_mapped)
  {
    vkMapMemory(g_vulkan_state->device, result->memory, 0, mem_req->size, 0, &result->mapped_ptr);
  }
  return 1;
}

internal GPU_VulkanMemBlock*
gpu_vulkan_find_or_create_mem_block(U32 memory_type_index, B32 wants_mapped, U64 needed_size, U64 alignment)
{
  for (U32 i = 0; i < g_vulkan_state->mem_block_count; i++)
  {
    GPU_VulkanMemBlock* block = &g_vulkan_state->mem_blocks[i];
    if (block->memory_type_index != memory_type_index)
    {
      continue;
    }
    if ((block->mapped_ptr != 0) != wants_mapped)
    {
      continue;
    }
    
    U64 aligned_cursor = AlignPow2(block->cursor, alignment);
    if (aligned_cursor + needed_size <= block->size)
    {
      return block;
    }
  }
  
  if (g_vulkan_state->mem_block_count >= g_vulkan_state->mem_block_cap)
  {
    log_error("gpu_vulkan: suballocation block table full (%u) - falling back to a dedicated allocation", g_vulkan_state->mem_block_cap);
    return 0;
  }
  
  VkMemoryAllocateInfo alloc_info =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = g_vulkan_state->mem_block_size,
    .memoryTypeIndex = memory_type_index,
  };
  
  U64 block_alloc_t0 = os_now_microseconds();
  VkDeviceMemory memory;
  B32 device_local = gpu_vulkan_memory_type_is_device_local(memory_type_index);
  VkResult alloc_result = gpu_vulkan_allocate_memory(&alloc_info, device_local, &memory);
  if (alloc_result != VK_SUCCESS)
  {
    log_error("gpu_vulkan: failed to allocate %llu byte suballocation block - falling back to a dedicated allocation", g_vulkan_state->mem_block_size);
    return 0;
  }
  if (device_local)
  {
    // tec: blocks are never freed before exit, so the charge is never given back
    gpu_vulkan_vram_charge(g_vulkan_state->mem_block_size);
  }
  log_debug("gpu_vulkan_find_or_create_mem_block: new %llu byte block (type=%u mapped=%d): vkAllocateMemory=%llu microseconds",
            g_vulkan_state->mem_block_size, memory_type_index, wants_mapped, os_now_microseconds() - block_alloc_t0);
  
  GPU_VulkanMemBlock* block = &g_vulkan_state->mem_blocks[g_vulkan_state->mem_block_count++];
  block->memory = memory;
  block->size = g_vulkan_state->mem_block_size;
  block->cursor = 0;
  block->memory_type_index = memory_type_index;
  block->mapped_ptr = 0;
  
  if (wants_mapped)
  {
    vkMapMemory(g_vulkan_state->device, memory, 0, g_vulkan_state->mem_block_size, 0, &block->mapped_ptr);
  }
  
  return block;
}

internal B32
gpu_vulkan_buffer_alloc_backing(GPU_VulkanBuffer* result, U64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_props, B32 shared)
{
  if (g_vulkan_state->device_lost)
  {
    return 0;
  }
  
  U32 sharing_families[2] = { g_vulkan_state->compute_queue_family_index, g_vulkan_state->copy_queue_family_index };
  B32 concurrent = shared && sharing_families[0] != sharing_families[1];
  VkBufferCreateInfo buf_info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = usage,
    .sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = concurrent ? 2 : 0,
    .pQueueFamilyIndices = concurrent ? sharing_families : 0,
  };
  
  if (vkCreateBuffer(g_vulkan_state->device, &buf_info, 0, &result->buffer) != VK_SUCCESS)
  {
    log_error("failed to create Vulkan buffer");
    return 0;
  }
  
  VkMemoryRequirements mem_req;
  vkGetBufferMemoryRequirements(g_vulkan_state->device, result->buffer, &mem_req);
  
  U32 mem_type = gpu_vulkan_find_memory_type(mem_req.memoryTypeBits, mem_props);
  if (mem_type == UINT32_MAX)
  {
    log_error("No matching memory type for Vulkan buffer.");
    vkDestroyBuffer(g_vulkan_state->device, result->buffer, 0);
    return 0;
  }
  
  B32 wants_mapped = (mem_props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
  
  if (mem_req.size < g_vulkan_state->mem_block_size)
  {
    GPU_VulkanMemBlock* block = gpu_vulkan_find_or_create_mem_block(mem_type, wants_mapped, mem_req.size, mem_req.alignment);
    if (block)
    {
      U64 offset = AlignPow2(block->cursor, mem_req.alignment);
      vkBindBufferMemory(g_vulkan_state->device, result->buffer, block->memory, offset);
      block->cursor = offset + mem_req.size;
      
      result->memory = block->memory;
      result->owns_memory = 0;
      if (block->mapped_ptr)
      {
        result->mapped_ptr = (U8*)block->mapped_ptr + offset;
      }
      return 1;
    }
  }
  
  // tec: fallback, if block table was full or the block allocation failed
  if (!gpu_vulkan_alloc_dedicated_memory(result, &mem_req, mem_type, wants_mapped))
  {
    vkDestroyBuffer(g_vulkan_state->device, result->buffer, 0);
    return 0;
  }
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
gpu_vulkan_staged_upload(GPU_VulkanBuffer* dst, void* data, U64 size)
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
  gpu_vulkan_end_and_submit_cmd_tagged(cmd, "staged_upload");
}

// tec: downloads `size` bytes from `src` (a device-local, non-mapped buffer) into `data` via the persistent download staging buffer
internal void
gpu_vulkan_staged_download(GPU_VulkanBuffer* src, void* data, U64 size)
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
  
  if (gpu_vulkan_end_and_submit_cmd_tagged(cmd, "staged_download"))
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
gpu_vulkan_buffer_alloc(U64 size, GPU_BufferFlags flags, void* data)
{
  GPU_VulkanBuffer* result = push_array(g_vulkan_state->arena, GPU_VulkanBuffer, 1);
  result->size = size;
  result->mapped_ptr = 0;
  
  VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  
  B32 host_visible = (flags & GPU_BufferFlag_HostVisible) != 0;
  B32 device_local = (flags & GPU_BufferFlag_DeviceLocal) != 0;
  B32 cpu_reads = (flags & (GPU_BufferFlag_Read | GPU_BufferFlag_ReadWrite)) != 0;
  
  B32 shared = (flags & GPU_BufferFlag_Shared) != 0;
  
  if (!host_visible && !cpu_reads && !shared && g_vulkan_state->rebar_supported && size <= g_vulkan_state->rebar_heap_size &&
      g_vulkan_state->rebar_bytes_in_use + size <= g_vulkan_state->rebar_budget)
  {
    VkMemoryPropertyFlags rebar_props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (gpu_vulkan_buffer_alloc_backing(result, size, usage, rebar_props, 0))
    {
      // tec: a buffer suballocated out of a shared block is not freed on its own, 
      // so only a buffer with memory of its own is counted
      if (result->owns_memory)
      {
        result->rebar_charge = size;
        g_vulkan_state->rebar_bytes_in_use += size;
      }
      if (data)
      {
        MemoryCopy(result->mapped_ptr, data, size);
      }
      return (GPU_Buffer*)result;
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
  
  if (!gpu_vulkan_buffer_alloc_backing(result, size, usage, mem_props, shared))
  {
    return 0;
  }
  
  if (host_visible)
  {
    if (data)
    {
      MemoryCopy(result->mapped_ptr, data, size);
    }
  }
  else if (data)
  {
    gpu_vulkan_staged_upload(result, data, size);
  }
  
  return (GPU_Buffer*)result;
}

internal GPU_PooledBuffer*
gpu_vulkan_pooled_buffer_find(String8 name)
{
  U32 mask = g_vulkan_state->pooled_buffer_hash_slots_cap - 1;
  U32 slot_idx = (U32)u64_hash_from_str8(name) & mask;
  for (U32 probes = 0; probes < g_vulkan_state->pooled_buffer_hash_slots_cap; probes += 1)
  {
    U32 index_plus_one = g_vulkan_state->pooled_buffer_hash_slots[slot_idx];
    if (index_plus_one == 0) break;
    
    GPU_PooledBuffer* candidate = &g_vulkan_state->pooled_buffers[index_plus_one - 1];
    if (str8_match(candidate->name, name, 0))
    {
      return candidate;
    }
    slot_idx = (slot_idx + 1) & mask;
  }
  return 0;
}

internal void
gpu_vulkan_pooled_buffer_hash_insert(String8 name, U32 index)
{
  U32 mask = g_vulkan_state->pooled_buffer_hash_slots_cap - 1;
  U32 slot_idx = (U32)u64_hash_from_str8(name) & mask;
  while (g_vulkan_state->pooled_buffer_hash_slots[slot_idx] != 0)
  {
    slot_idx = (slot_idx + 1) & mask;
  }
  g_vulkan_state->pooled_buffer_hash_slots[slot_idx] = index + 1;
}

internal GPU_Buffer*
gpu_vulkan_buffer_alloc_pooled(String8 name, U64 size, GPU_BufferFlags flags, void* data)
{
  GPU_PooledBuffer* slot = gpu_vulkan_pooled_buffer_find(name);
  if (slot)
  {
    slot->last_epoch = g_vulkan_state->pool_epoch;
    if (size > slot->capacity || !slot->buffer)
    {
      // tec: the old buffer goes first so its memory is available to the new one
      gpu_vulkan_buffer_release((GPU_Buffer*)slot->buffer);
      slot->buffer = 0;
      slot->capacity = 0;
      slot->imported_host_ptr = 0;
      slot->buffer = (GPU_VulkanBuffer*)gpu_vulkan_buffer_alloc(size, flags, data);
      // tec: only record the new capacity if the allocation actually succeeded
      slot->capacity = slot->buffer ? size : 0;
    }
    else
    {
      slot->buffer->size = size;
      if (data) gpu_vulkan_buffer_write((GPU_Buffer*)slot->buffer, data, size);
    }
    return (GPU_Buffer*)slot->buffer;
  }
  
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)gpu_vulkan_buffer_alloc(size, flags, data);
  if (g_vulkan_state->pooled_buffer_count < g_vulkan_state->pooled_buffer_cap)
  {
    U32 index = g_vulkan_state->pooled_buffer_count++;
    GPU_PooledBuffer* new_slot = &g_vulkan_state->pooled_buffers[index];
    new_slot->name = push_str8_copy(g_vulkan_state->arena, name);
    new_slot->buffer = buffer;
    new_slot->capacity = buffer ? size : 0;
    new_slot->last_epoch = g_vulkan_state->pool_epoch;
    gpu_vulkan_pooled_buffer_hash_insert(name, index);
  }
  else
  {
    log_error("gpu_vulkan_buffer_alloc_pooled: pool full, '%.*s' will not be cached", str8_varg(name));
  }
  return (GPU_Buffer*)buffer;
}

// tec: imports an existing host pointer (such as an OS mapped file view) directly as a VkBuffer's backing memory via VK_EXT_external_memory_host
// fills 'result'  instead of allocating it, worker threads can run this
internal B32
gpu_vulkan_import_host_into(GPU_VulkanBuffer* result, void* host_ptr, U64 size)
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
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
  
  MemoryZeroStruct(result);
  result->buffer = buffer;
  result->memory = memory;
  result->size = size;
  result->bind_offset = front_pad;
  result->owns_memory = 1;
  return 1;
}

internal GPU_Buffer*
gpu_vulkan_buffer_import_host_readonly(void* host_ptr, U64 size)
{
  GPU_VulkanBuffer* result = push_array(g_vulkan_state->arena, GPU_VulkanBuffer, 1);
  if (!gpu_vulkan_import_host_into(result, host_ptr, size))
  {
    return 0;
  }
  return (GPU_Buffer*)result;
}

internal GPU_Buffer*
gpu_vulkan_buffer_import_host_readonly_pooled(String8 name, void* host_ptr, U64 size)
{
  GPU_PooledBuffer* slot = gpu_vulkan_pooled_buffer_find(name);
  if (slot)
  {
    slot->last_epoch = g_vulkan_state->pool_epoch;
    if (slot->buffer && slot->imported_host_ptr == host_ptr && slot->capacity == size)
    {
      return (GPU_Buffer*)slot->buffer;
    }
    
    GPU_VulkanBuffer* fresh = (GPU_VulkanBuffer*)gpu_vulkan_buffer_import_host_readonly(host_ptr, size);
    // tec: whatever the slot held described other memory, so it is never handed back for this pointer
    gpu_vulkan_buffer_release((GPU_Buffer*)slot->buffer);
    slot->buffer = fresh;
    slot->capacity = fresh ? size : 0;
    slot->imported_host_ptr = fresh ? host_ptr : 0;
    return (GPU_Buffer*)fresh;
  }
  
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)gpu_vulkan_buffer_import_host_readonly(host_ptr, size);
  if (buffer)
  {
    if (g_vulkan_state->pooled_buffer_count < g_vulkan_state->pooled_buffer_cap)
    {
      U32 index = g_vulkan_state->pooled_buffer_count++;
      GPU_PooledBuffer* new_slot = &g_vulkan_state->pooled_buffers[index];
      new_slot->name = push_str8_copy(g_vulkan_state->arena, name);
      new_slot->buffer = buffer;
      new_slot->capacity = size;
      new_slot->imported_host_ptr = host_ptr;
      new_slot->last_epoch = g_vulkan_state->pool_epoch;
      gpu_vulkan_pooled_buffer_hash_insert(name, index);
    }
    else
    {
      log_error("gpu_vulkan_buffer_import_host_readonly_pooled: pool full, '%.*s' will not be cached", str8_varg(name));
    }
  }
  return (GPU_Buffer*)buffer;
}

internal void
gpu_vulkan_buffer_release(GPU_Buffer* buffer_handle)
{
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  if (!buffer)
  {
    return;
  }
  
  vkDestroyBuffer(g_vulkan_state->device, buffer->buffer, 0);
  
  if (buffer->owns_memory)
  {
    if (buffer->mapped_ptr)
    {
      vkUnmapMemory(g_vulkan_state->device, buffer->memory);
    }
    vkFreeMemory(g_vulkan_state->device, buffer->memory, 0);
  }
  
  g_vulkan_state->rebar_bytes_in_use -= Min(buffer->rebar_charge, g_vulkan_state->rebar_bytes_in_use);
  buffer->rebar_charge = 0;
  gpu_vulkan_vram_uncharge(buffer->vram_charge);
  buffer->vram_charge = 0;
}

internal void
gpu_vulkan_buffer_write(GPU_Buffer* buffer_handle, void* data, U64 size)
{
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  if (size > buffer->size)
  {
    log_error("gpu_vulkan_buffer_write: write size exceeds buffer size.");
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
gpu_vulkan_buffer_read(GPU_Buffer* buffer_handle, void* data, U64 size)
{
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  if (size > buffer->size)
  {
    log_error("gpu_vulkan_buffer_read: read size exceeds buffer size.");
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

// tec: precompiled shaders live next to the executable at shaders/<kernel_name>.spv
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
gpu_vulkan_kernel_alloc(String8 name)
{
  ProfBeginFunction();
  
  // tec: cached by name
  for (U32 i = 0; i < g_vulkan_state->kernel_cache_count; i++)
  {
    if (str8_match(g_vulkan_state->kernel_cache[i]->name, name, 0))
    {
      ProfEnd();
      return (GPU_Kernel*)g_vulkan_state->kernel_cache[i];
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
  
  GPU_VulkanKernel* kernel = push_array(g_vulkan_state->arena, GPU_VulkanKernel, 1);
  kernel->name = push_str8_copy(g_vulkan_state->arena, name);
  kernel->shader = shader;
  kernel->pipeline = pipeline;
  kernel->descriptor_set = descriptor_set;
  
  if (g_vulkan_state->kernel_cache_count < g_vulkan_state->kernel_cache_cap)
  {
    g_vulkan_state->kernel_cache[g_vulkan_state->kernel_cache_count++] = kernel;
  }
  else
  {
    log_error("gpu_vulkan_kernel_alloc: kernel cache full (%u), '%.*s' will be recompiled every call",
              g_vulkan_state->kernel_cache_cap, str8_varg(name));
  }
  
  ProfEnd();
  return (GPU_Kernel*)kernel;
}

internal void
gpu_vulkan_kernel_release(GPU_Kernel *kernel)
{
  // do nothing, kernels are cached
}

internal void
gpu_vulkan_kernel_set_arg_buffer(GPU_Kernel* kernel_handle, U32 index, GPU_Buffer* buffer_handle)
{
  GPU_VulkanKernel* kernel = (GPU_VulkanKernel*)kernel_handle;
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  
  if (index >= GPU_VULKAN_MAX_BOUND_BUFFERS)
  {
    log_error("gpu_vulkan_kernel_set_arg_buffer: index %u exceeds GPU_VULKAN_MAX_BOUND_BUFFERS", index);
    return;
  }
  
  if (!buffer)
  {
    log_error("gpu_vulkan_kernel_set_arg_buffer: buffer argument for index %u is NULL - a prior GPU buffer allocation failed", index);
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
gpu_vulkan_kernel_set_arg_u64(GPU_Kernel* kernel_handle, U32 index, U64 value)
{
  GPU_VulkanKernel* kernel = (GPU_VulkanKernel*)kernel_handle;
  if (index >= GPU_VULKAN_PUSH_CONSTANT_COUNT)
  {
    log_error("gpu_vulkan_kernel_set_arg_u64: index %u exceeds GPU_VULKAN_PUSH_CONSTANT_COUNT", index);
    return;
  }
  
  kernel->push_constants[index] = value;
}

internal void
gpu_vulkan_kernel_execute(GPU_Kernel* kernel_handle, U32 global_work_size, U32 local_work_size)
{
  GPU_VulkanKernel* kernel = (GPU_VulkanKernel*)kernel_handle;
  U32 group_count = (global_work_size + local_work_size - 1) / local_work_size;
  
  VkCommandBuffer cmd = gpu_vulkan_begin_one_time_cmd();
  
  vkCmdResetQueryPool(cmd, g_vulkan_state->timestamp_query_pool, 0, 2);
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vulkan_state->timestamp_query_pool, 0);
  
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_vulkan_state->shared_pipeline_layout, 0, 1, &kernel->descriptor_set, 0, 0);
  vkCmdPushConstants(cmd, g_vulkan_state->shared_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(kernel->push_constants), kernel->push_constants);
  
  vkCmdDispatch(cmd, group_count, 1, 1);
  
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vulkan_state->timestamp_query_pool, 1);
  
  if (gpu_vulkan_end_and_submit_cmd_tagged(cmd, "kernel_execute_direct"))
  {
    U64 timestamps[2];
    vkGetQueryPoolResults(g_vulkan_state->device, g_vulkan_state->timestamp_query_pool, 0, 2, sizeof(timestamps), timestamps, sizeof(U64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    
    F64 elapsed_ns = (F64)(timestamps[1] - timestamps[0]) * (F64)g_vulkan_state->timestamp_period_ns;
    g_vulkan_state->last_kernel_time_microseconds = (U64)(elapsed_ns / 1000.0);
  }
}

//~ tec: batching

internal GPU_Batch*
gpu_vulkan_batch_begin(U64 upload_bytes_needed, U64 download_bytes_needed)
{
  GPU_VulkanBatch* batch = &g_vulkan_state->active_batch;
  GPU_PendingRead* pending_reads = batch->pending_reads;
  U32 pending_read_cap = batch->pending_read_cap;
  MemoryZeroStruct(batch);
  batch->pending_reads = pending_reads;
  batch->pending_read_cap = pending_read_cap;
  
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
  batch->recording = 1;
  return (GPU_Batch*)batch;
}

internal void
gpu_vulkan_batch_buffer_write(GPU_Batch* batch_handle, GPU_Buffer* buffer_handle, void* data, U64 size)
{
  GPU_VulkanBatch* batch = (GPU_VulkanBatch*)batch_handle;
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  if (size == 0 || !data) return;
  
  if (buffer->mapped_ptr)
  {
    MemoryCopy(buffer->mapped_ptr, data, size);
    return;
  }
  
  if (batch->upload_cursor + size > g_vulkan_state->upload_staging_capacity)
  {
    log_error("gpu_vulkan_batch_buffer_write: exceeded reserved upload staging capacity, dropping write");
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
gpu_vulkan_batch_buffer_fill(GPU_Batch* batch_handle, GPU_Buffer* buffer_handle, U64 size, U32 value)
{
  GPU_VulkanBatch* batch = (GPU_VulkanBatch*)batch_handle;
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  if (size == 0) return;
  
  vkCmdFillBuffer(batch->cmd, buffer->buffer, buffer->bind_offset, size, value);
  
  batch->wrote_since_barrier = 1;
  batch->has_commands = 1;
}

internal void
gpu_vulkan_batch_buffer_zero(GPU_Batch* batch, GPU_Buffer* buffer, U64 size)
{
  gpu_vulkan_batch_buffer_fill(batch, buffer, size, 0);
}

internal void
gpu_vulkan_batch_kernel_execute(GPU_Batch* batch_handle, GPU_Kernel* kernel_handle, U32 global_work_size, U32 local_work_size)
{
  GPU_VulkanBatch* batch = (GPU_VulkanBatch*)batch_handle;
  GPU_VulkanKernel* kernel = (GPU_VulkanKernel*)kernel_handle;
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
gpu_vulkan_batch_buffer_read(GPU_Batch* batch_handle, GPU_Buffer* buffer_handle, void* out_data, U64 size)
{
  GPU_VulkanBatch* batch = (GPU_VulkanBatch*)batch_handle;
  GPU_VulkanBuffer* buffer = (GPU_VulkanBuffer*)buffer_handle;
  if (size == 0) return;
  if (batch->pending_read_count >= batch->pending_read_cap)
  {
    log_error("gpu_vulkan_batch_buffer_read: too many pending reads in one batch (max %u), dropping read", batch->pending_read_cap);
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
    log_error("gpu_vulkan_batch_buffer_read: exceeded reserved download staging capacity, dropping read");
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
gpu_vulkan_batch_end(GPU_Batch* batch_handle)
{
  GPU_VulkanBatch* batch = (GPU_VulkanBatch*)batch_handle;
  B32 ok = 1;
  
  if (batch->has_commands)
  {
    ok = gpu_vulkan_end_and_submit_cmd_tagged(batch->cmd, batch->had_dispatch ? "batch_dispatch" : "batch_upload_only");
    
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
  
  batch->recording = 0;
  return ok;
}

internal Arena*
gpu_vulkan_scratch_arena(void)
{
  return g_vulkan_state->arena;
}

internal String8
gpu_vulkan_name(void)
{
  return str8_lit("vulkan");
}

internal GPU_Backend*
gpu_vulkan_get_backend(void)
{
  local_persist GPU_Backend backend = {0};
  backend.struct_size = sizeof(GPU_Backend);
  
  backend.init = gpu_vulkan_init;
  backend.release = gpu_vulkan_release;
  backend.wait = gpu_vulkan_wait;
  backend.get_executed_kernel_time_microseconds = gpu_vulkan_get_executed_kernel_time_microseconds;
  backend.device_total_memory = gpu_vulkan_device_total_memory;
  backend.device_free_memory = gpu_vulkan_device_free_memory;
  backend.device_max_storage_buffer_range = gpu_vulkan_device_max_storage_buffer_range;
  backend.device_lost = gpu_vulkan_device_lost;
  backend.pool_begin_epoch = gpu_vulkan_pool_begin_epoch;
  backend.memory_stats = gpu_vulkan_memory_stats;
  
  backend.buffer_alloc = gpu_vulkan_buffer_alloc;
  backend.buffer_alloc_pooled = gpu_vulkan_buffer_alloc_pooled;
  backend.buffer_import_host_readonly = gpu_vulkan_buffer_import_host_readonly;
  backend.buffer_import_host_readonly_pooled = gpu_vulkan_buffer_import_host_readonly_pooled;
  backend.buffer_release = gpu_vulkan_buffer_release;
  backend.buffer_write = gpu_vulkan_buffer_write;
  backend.buffer_read = gpu_vulkan_buffer_read;
  
  backend.kernel_alloc = gpu_vulkan_kernel_alloc;
  backend.kernel_release = gpu_vulkan_kernel_release;
  backend.kernel_execute = gpu_vulkan_kernel_execute;
  backend.kernel_set_arg_buffer = gpu_vulkan_kernel_set_arg_buffer;
  backend.kernel_set_arg_u64 = gpu_vulkan_kernel_set_arg_u64;
  
  backend.batch_begin = gpu_vulkan_batch_begin;
  backend.batch_buffer_write = gpu_vulkan_batch_buffer_write;
  backend.batch_buffer_zero = gpu_vulkan_batch_buffer_zero;
  backend.batch_buffer_fill = gpu_vulkan_batch_buffer_fill;
  backend.batch_kernel_execute = gpu_vulkan_batch_kernel_execute;
  backend.batch_buffer_read = gpu_vulkan_batch_buffer_read;
  backend.batch_end = gpu_vulkan_batch_end;
  
  backend.stream_alloc = gpu_vulkan_stream_alloc;
  backend.stream_release = gpu_vulkan_stream_release;
  backend.stream_slot_begin = gpu_vulkan_stream_slot_begin;
  backend.stream_copy = gpu_vulkan_stream_copy;
  backend.stream_zero = gpu_vulkan_stream_zero;
  backend.stream_fill = gpu_vulkan_stream_fill;
  backend.stream_dispatch = gpu_vulkan_stream_dispatch;
  backend.stream_read = gpu_vulkan_stream_read;
  backend.stream_submit = gpu_vulkan_stream_submit;
  backend.stream_wait = gpu_vulkan_stream_wait;
  backend.stream_get_stats = gpu_vulkan_stream_get_stats;
  
  backend.scratch_arena = gpu_vulkan_scratch_arena;
  backend.name = gpu_vulkan_name;
  
  return &backend;
}