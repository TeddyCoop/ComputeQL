#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "third_party/vulkan/vulkan/vulkan.h"

#include "base/base_inc.c"
#include "os/os_inc.c"

//~ tec: host to VRAM transfer micro benchmark
//
// owns its Vulkan instance and device, so it can ask for queues the engine does not create yet. measures
//   - how fast host memory can be produced: first touch, memcpy by 1..N threads, into plain and into mapped staging memory
//   - the copy engine rate from a staging buffer, on the compute queue, a transfer only queue and an async compute queue
//   - path B: importing host memory as a VkBuffer (VK_EXT_external_memory_host), what pinning costs, and the copy rate from it
//   - path C: a kernel reading the imported buffer directly over PCIe, next to the same kernel reading VRAM
//   - overlap: a kernel loop on the compute queue while a copy runs on another queue
//   - the pipelines the stream would use: a staging ring fed by N threads (path A), and imported windows (path B)
//
// run from build\ after compiling shaders\transfer_read.spv. flags: --mb=<host region size, default 2048>

#define TX_MAX_FAMILIES 16
#define TX_QUEUE_KINDS 3
#define TX_REPEATS 5
#define TX_TIMEOUT_NS (60ull * 1000000000ull)

typedef enum TX_QueueKind
{
  TX_QueueKind_Compute,
  TX_QueueKind_Transfer,
  TX_QueueKind_AsyncCompute,
} TX_QueueKind;

global char* g_queue_kind_names[TX_QUEUE_KINDS] = { "compute queue", "transfer only queue", "async compute queue" };

typedef struct TX_Queue TX_Queue;
struct TX_Queue
{
  B32 available;
  U32 family;
  VkQueue queue;
  VkCommandPool pool;
  VkFence fence;
};

typedef struct TX_Buffer TX_Buffer;
struct TX_Buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  U64 size;
  void* mapped;
};

typedef struct TX_Context TX_Context;
struct TX_Context
{
  VkInstance instance;
  VkPhysicalDevice physical;
  VkDevice device;
  VkPhysicalDeviceMemoryProperties memory_properties;
  VkPhysicalDeviceProperties properties;
  VkQueueFamilyProperties families[TX_MAX_FAMILIES];
  U32 family_count;

  TX_Queue queues[TX_QUEUE_KINDS];
  U32 sharing_families[TX_QUEUE_KINDS];
  U32 sharing_family_count;

  B32 has_host_import;
  U64 import_alignment;
  PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_properties;

  VkDescriptorSetLayout set_layout;
  VkPipelineLayout pipeline_layout;
  VkPipeline read_pipeline;
  VkDescriptorPool descriptor_pool;
  B32 has_read_kernel;
};

global TX_Context g_tx = {0};
global FILE* g_report = 0;
global Arena* g_tx_arena = 0;

internal void
tx_say(char* format, ...)
{
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  if (g_report)
  {
    va_start(args, format);
    vfprintf(g_report, format, args);
    va_end(args);
  }
}

internal F64
tx_gbps(U64 bytes, U64 microseconds)
{
  if (microseconds == 0)
  {
    return 0.0;
  }
  return (F64)bytes / 1000000000.0 / ((F64)microseconds / 1000000.0);
}

internal U64
tx_u64_argument(CmdLine* cmdline, char* name, U64 fallback)
{
  String8 key = str8_cstring(name);
  if (cmd_line_has_argument(cmdline, key))
  {
    return u64_from_str8(cmd_line_string(cmdline, key), 10);
  }
  return fallback;
}

//~ tec: device setup

internal B32
tx_has_extension(VkExtensionProperties* extensions, U32 count, char* name)
{
  for (U32 index = 0; index < count; index += 1)
  {
    if (strcmp(extensions[index].extensionName, name) == 0)
    {
      return 1;
    }
  }
  return 0;
}

internal B32
tx_setup(void)
{
  VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "gpu transfer bench", .apiVersion = VK_API_VERSION_1_3 };
  VkInstanceCreateInfo instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
  if (vkCreateInstance(&instance_info, 0, &g_tx.instance) != VK_SUCCESS)
  {
    tx_say("failed to create a Vulkan instance\n");
    return 0;
  }

  U32 device_count = 0;
  vkEnumeratePhysicalDevices(g_tx.instance, &device_count, 0);
  VkPhysicalDevice devices[8];
  device_count = Min(device_count, (U32)ArrayCount(devices));
  vkEnumeratePhysicalDevices(g_tx.instance, &device_count, devices);

  // tec: the first device with a compute queue, the same pick the engine makes
  for (U32 index = 0; index < device_count && !g_tx.physical; index += 1)
  {
    U32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &count, 0);
    VkQueueFamilyProperties properties[TX_MAX_FAMILIES];
    count = Min(count, (U32)TX_MAX_FAMILIES);
    vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &count, properties);
    for (U32 family = 0; family < count; family += 1)
    {
      if (properties[family].queueFlags & VK_QUEUE_COMPUTE_BIT)
      {
        g_tx.physical = devices[index];
        g_tx.family_count = count;
        MemoryCopy(g_tx.families, properties, sizeof(VkQueueFamilyProperties) * count);
        break;
      }
    }
  }
  if (!g_tx.physical)
  {
    tx_say("no Vulkan device with a compute queue\n");
    return 0;
  }

  vkGetPhysicalDeviceProperties(g_tx.physical, &g_tx.properties);
  vkGetPhysicalDeviceMemoryProperties(g_tx.physical, &g_tx.memory_properties);

  U32 extension_count = 0;
  vkEnumerateDeviceExtensionProperties(g_tx.physical, 0, &extension_count, 0);
  VkExtensionProperties* extensions = push_array(g_tx_arena, VkExtensionProperties, extension_count);
  vkEnumerateDeviceExtensionProperties(g_tx.physical, 0, &extension_count, extensions);
  g_tx.has_host_import = tx_has_extension(extensions, extension_count, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);

  if (g_tx.has_host_import)
  {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_properties };
    vkGetPhysicalDeviceProperties2(g_tx.physical, &properties2);
    g_tx.import_alignment = host_properties.minImportedHostPointerAlignment;
  }

  // tec: queue selection. the compute queue is the first family with compute, the transfer queue a family that can only transfer (the DMA engine), the async
  // compute queue a compute family without graphics, or a second queue of the compute family when that is all there is
  U32 compute_family = max_U32;
  U32 transfer_family = max_U32;
  U32 async_family = max_U32;
  for (U32 family = 0; family < g_tx.family_count; family += 1)
  {
    VkQueueFlags flags = g_tx.families[family].queueFlags;
    B32 compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
    B32 graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
    B32 transfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0;
    if (compute && compute_family == max_U32)
    {
      compute_family = family;
    }
    if (transfer && !compute && !graphics && transfer_family == max_U32)
    {
      transfer_family = family;
    }
    if (compute && !graphics && async_family == max_U32)
    {
      async_family = family;
    }
  }

  U32 wanted_family[TX_QUEUE_KINDS] = { compute_family, transfer_family, async_family };
  F32 priority = 1.0f;
  VkDeviceQueueCreateInfo queue_infos[TX_QUEUE_KINDS];
  U32 queue_info_count = 0;
  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    U32 family = wanted_family[kind];
    if (family == max_U32)
    {
      continue;
    }
    B32 already = 0;
    for (U32 other = 0; other < queue_info_count; other += 1)
    {
      already = already || queue_infos[other].queueFamilyIndex == family;
    }
    if (already)
    {
      continue;
    }
    queue_infos[queue_info_count] = (VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    g_tx.sharing_families[g_tx.sharing_family_count] = family;
    g_tx.sharing_family_count += 1;
    queue_info_count += 1;
  }

  char* enabled_extensions[2];
  U32 enabled_extension_count = 0;
  if (g_tx.has_host_import)
  {
    enabled_extensions[enabled_extension_count] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    enabled_extension_count += 1;
  }

  VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = queue_info_count,
    .pQueueCreateInfos = queue_infos,
    .enabledExtensionCount = enabled_extension_count,
    .ppEnabledExtensionNames = (const char* const*)enabled_extensions,
  };
  if (vkCreateDevice(g_tx.physical, &device_info, 0, &g_tx.device) != VK_SUCCESS)
  {
    tx_say("failed to create the Vulkan device\n");
    return 0;
  }

  if (g_tx.has_host_import)
  {
    g_tx.get_host_pointer_properties = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_tx.device, "vkGetMemoryHostPointerPropertiesEXT");
    g_tx.has_host_import = g_tx.get_host_pointer_properties != 0 && g_tx.import_alignment != 0 && g_tx.import_alignment <= 4096;
  }

  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    U32 family = wanted_family[kind];
    TX_Queue* queue = &g_tx.queues[kind];
    if (family == max_U32)
    {
      continue;
    }
    queue->available = 1;
    queue->family = family;
    vkGetDeviceQueue(g_tx.device, family, 0, &queue->queue);
    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    vkCreateCommandPool(g_tx.device, &pool_info, 0, &queue->pool);
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(g_tx.device, &fence_info, 0, &queue->fence);
  }

  tx_say("device: %s\n", g_tx.properties.deviceName);
  tx_say("queue families:\n");
  for (U32 family = 0; family < g_tx.family_count; family += 1)
  {
    VkQueueFlags flags = g_tx.families[family].queueFlags;
    tx_say("  family %u x%u:%s%s%s%s\n", family, g_tx.families[family].queueCount, (flags & VK_QUEUE_GRAPHICS_BIT) ? " graphics" : "",
           (flags & VK_QUEUE_COMPUTE_BIT) ? " compute" : "", (flags & VK_QUEUE_TRANSFER_BIT) ? " transfer" : "", (flags & VK_QUEUE_SPARSE_BINDING_BIT) ? " sparse" : "");
  }
  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    if (g_tx.queues[kind].available)
    {
      tx_say("%s: family %u\n", g_queue_kind_names[kind], g_tx.queues[kind].family);
    }
    else
    {
      tx_say("%s: not available on this device\n", g_queue_kind_names[kind]);
    }
  }
  tx_say("host pointer import: %s", g_tx.has_host_import ? "supported" : "not supported");
  if (g_tx.has_host_import)
  {
    tx_say(" (alignment %llu)", g_tx.import_alignment);
  }
  tx_say("\n");
  for (U32 heap = 0; heap < g_tx.memory_properties.memoryHeapCount; heap += 1)
  {
    tx_say("heap %u: %llu MB%s\n", heap, g_tx.memory_properties.memoryHeaps[heap].size / MB(1), (g_tx.memory_properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device local" : "");
  }
  return 1;
}

//~ tec: buffers

internal U32
tx_find_memory_type(U32 type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoided)
{
  for (U32 index = 0; index < g_tx.memory_properties.memoryTypeCount; index += 1)
  {
    VkMemoryPropertyFlags flags = g_tx.memory_properties.memoryTypes[index].propertyFlags;
    if ((type_bits & (1u << index)) && (flags & required) == required && (flags & avoided) == 0)
    {
      return index;
    }
  }
  return max_U32;
}

// tec: concurrent sharing between every family the bench submits on, so a buffer can be used by any queue without ownership transfers
internal VkBufferCreateInfo
tx_buffer_info(U64 size, VkBufferUsageFlags usage, void* next)
{
  VkBufferCreateInfo info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .pNext = next,
    .size = size,
    .usage = usage,
    .sharingMode = g_tx.sharing_family_count > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = g_tx.sharing_family_count > 1 ? g_tx.sharing_family_count : 0,
    .pQueueFamilyIndices = g_tx.sharing_families,
  };
  return info;
}

internal TX_Buffer
tx_buffer_alloc(U64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoided, B32 map)
{
  TX_Buffer result = {0};
  result.size = size;
  VkBufferCreateInfo info = tx_buffer_info(size, usage, 0);
  if (vkCreateBuffer(g_tx.device, &info, 0, &result.buffer) != VK_SUCCESS)
  {
    MemoryZeroStruct(&result);
    return result;
  }
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(g_tx.device, result.buffer, &requirements);
  U32 type = tx_find_memory_type(requirements.memoryTypeBits, required, avoided);
  if (type == max_U32)
  {
    vkDestroyBuffer(g_tx.device, result.buffer, 0);
    MemoryZeroStruct(&result);
    return result;
  }
  VkMemoryAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type };
  if (vkAllocateMemory(g_tx.device, &alloc_info, 0, &result.memory) != VK_SUCCESS)
  {
    vkDestroyBuffer(g_tx.device, result.buffer, 0);
    MemoryZeroStruct(&result);
    return result;
  }
  vkBindBufferMemory(g_tx.device, result.buffer, result.memory, 0);
  if (map)
  {
    vkMapMemory(g_tx.device, result.memory, 0, size, 0, &result.mapped);
  }
  return result;
}

internal void
tx_buffer_release(TX_Buffer* buffer)
{
  if (!buffer->buffer)
  {
    return;
  }
  if (buffer->mapped)
  {
    vkUnmapMemory(g_tx.device, buffer->memory);
  }
  vkDestroyBuffer(g_tx.device, buffer->buffer, 0);
  vkFreeMemory(g_tx.device, buffer->memory, 0);
  MemoryZeroStruct(buffer);
}

internal TX_Buffer
tx_vram_alloc(U64 size)
{
  return tx_buffer_alloc(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
}

// tec: plain host visible memory, the kind a staging buffer uses
internal TX_Buffer
tx_staging_alloc(U64 size)
{
  return tx_buffer_alloc(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
}

// tec: wraps `size` bytes of host memory at `ptr` (page aligned) as a buffer. import_microseconds receives the time of the whole import
internal TX_Buffer
tx_import_host(void* ptr, U64 size, U64* import_microseconds)
{
  TX_Buffer result = {0};
  U64 start = os_now_microseconds();
  VkMemoryHostPointerPropertiesEXT host_properties = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
  if (g_tx.get_host_pointer_properties(g_tx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &host_properties) != VK_SUCCESS)
  {
    return result;
  }
  VkExternalMemoryBufferCreateInfo external_info = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
  VkBufferCreateInfo info = tx_buffer_info(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &external_info);
  if (vkCreateBuffer(g_tx.device, &info, 0, &result.buffer) != VK_SUCCESS)
  {
    MemoryZeroStruct(&result);
    return result;
  }
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(g_tx.device, result.buffer, &requirements);
  U32 type = tx_find_memory_type(requirements.memoryTypeBits & host_properties.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
  if (type == max_U32)
  {
    vkDestroyBuffer(g_tx.device, result.buffer, 0);
    MemoryZeroStruct(&result);
    return result;
  }
  VkImportMemoryHostPointerInfoEXT import_info = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = ptr };
  VkMemoryAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import_info, .allocationSize = size, .memoryTypeIndex = type };
  if (vkAllocateMemory(g_tx.device, &alloc_info, 0, &result.memory) != VK_SUCCESS)
  {
    vkDestroyBuffer(g_tx.device, result.buffer, 0);
    MemoryZeroStruct(&result);
    return result;
  }
  vkBindBufferMemory(g_tx.device, result.buffer, result.memory, 0);
  result.size = size;
  *import_microseconds = os_now_microseconds() - start;
  return result;
}

//~ tec: submission

internal VkCommandBuffer
tx_begin(TX_Queue* queue)
{
  VkCommandBufferAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = queue->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
  VkCommandBuffer cmd = 0;
  vkAllocateCommandBuffers(g_tx.device, &alloc_info, &cmd);
  VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
  vkBeginCommandBuffer(cmd, &begin_info);
  return cmd;
}

internal void
tx_submit_async(TX_Queue* queue, VkCommandBuffer cmd)
{
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
  vkResetFences(g_tx.device, 1, &queue->fence);
  vkQueueSubmit(queue->queue, 1, &submit, queue->fence);
}

internal void
tx_wait(TX_Queue* queue, VkCommandBuffer cmd)
{
  vkWaitForFences(g_tx.device, 1, &queue->fence, VK_TRUE, TX_TIMEOUT_NS);
  vkFreeCommandBuffers(g_tx.device, queue->pool, 1, &cmd);
}

internal U64
tx_submit_and_wait(TX_Queue* queue, VkCommandBuffer cmd)
{
  U64 start = os_now_microseconds();
  tx_submit_async(queue, cmd);
  tx_wait(queue, cmd);
  return os_now_microseconds() - start;
}

internal void
tx_record_copy(VkCommandBuffer cmd, VkBuffer source, U64 source_offset, VkBuffer destination, U64 destination_offset, U64 size)
{
  VkBufferCopy region = { .srcOffset = source_offset, .dstOffset = destination_offset, .size = size };
  vkCmdCopyBuffer(cmd, source, destination, 1, &region);
}

// tec: the best of TX_REPEATS copies of `size` bytes after one warm up
internal U64
tx_time_copy(TX_Queue* queue, VkBuffer source, U64 source_offset, VkBuffer destination, U64 size)
{
  U64 best = max_U64;
  for (U32 run = 0; run < TX_REPEATS + 1; run += 1)
  {
    VkCommandBuffer cmd = tx_begin(queue);
    tx_record_copy(cmd, source, source_offset, destination, 0, size);
    U64 elapsed = tx_submit_and_wait(queue, cmd);
    if (run > 0)
    {
      best = Min(best, elapsed);
    }
  }
  return best;
}

//~ tec: read kernel

internal B32
tx_load_read_kernel(void)
{
  String8 path = push_str8f(g_tx_arena, "%.*s/shaders/transfer_read.spv", str8_varg(os_get_process_info()->binary_path));
  if (!os_file_path_exists(path))
  {
    tx_say("shaders/transfer_read.spv not found, the kernel tests are skipped\n");
    return 0;
  }
  OS_Handle file = os_file_open(OS_AccessFlag_Read, path);
  U64 file_size = os_properties_from_file(file).size;
  String8 code = os_string_from_file_range(g_tx_arena, file, r1u64(0, file_size));
  os_file_close(file);

  VkShaderModuleCreateInfo module_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code.size, .pCode = (U32*)code.str };
  VkShaderModule module = 0;
  if (vkCreateShaderModule(g_tx.device, &module_info, 0, &module) != VK_SUCCESS)
  {
    return 0;
  }

  VkDescriptorSetLayoutBinding bindings[2];
  for (U32 index = 0; index < 2; index += 1)
  {
    bindings[index] = (VkDescriptorSetLayoutBinding){ .binding = index, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
  }
  VkDescriptorSetLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings };
  vkCreateDescriptorSetLayout(g_tx.device, &layout_info, 0, &g_tx.set_layout);
  VkPushConstantRange push_range = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 2 * sizeof(U32) };
  VkPipelineLayoutCreateInfo pipeline_layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_tx.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range };
  vkCreatePipelineLayout(g_tx.device, &pipeline_layout_info, 0, &g_tx.pipeline_layout);
  VkComputePipelineCreateInfo pipeline_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
    .layout = g_tx.pipeline_layout,
  };
  if (vkCreateComputePipelines(g_tx.device, 0, 1, &pipeline_info, 0, &g_tx.read_pipeline) != VK_SUCCESS)
  {
    return 0;
  }
  VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 64 };
  VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 32, .poolSizeCount = 1, .pPoolSizes = &pool_size };
  vkCreateDescriptorPool(g_tx.device, &pool_info, 0, &g_tx.descriptor_pool);
  g_tx.has_read_kernel = 1;
  return 1;
}

#define TX_KERNEL_UNROLL_BYTES 128

// tec: records `passes` dispatches that each read all of `data`, with a barrier between them so they run one after another
internal void
tx_record_read_kernel(VkCommandBuffer cmd, TX_Buffer* data, TX_Buffer* sink, U32 passes)
{
  VkDescriptorSetAllocateInfo set_alloc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_tx.descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &g_tx.set_layout };
  VkDescriptorSet set = 0;
  vkAllocateDescriptorSets(g_tx.device, &set_alloc, &set);
  VkDescriptorBufferInfo infos[2] = { { .buffer = data->buffer, .range = data->size }, { .buffer = sink->buffer, .range = sink->size } };
  VkWriteDescriptorSet writes[2];
  for (U32 index = 0; index < 2; index += 1)
  {
    writes[index] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = index, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &infos[index] };
  }
  vkUpdateDescriptorSets(g_tx.device, 2, writes, 0, 0);

  U32 threads = (U32)(data->size / TX_KERNEL_UNROLL_BYTES);
  U32 constants[2] = { threads, 0x7fffffffu };
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_tx.read_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_tx.pipeline_layout, 0, 1, &set, 0, 0);
  vkCmdPushConstants(cmd, g_tx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants);
  VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
  for (U32 pass = 0; pass < passes; pass += 1)
  {
    vkCmdDispatch(cmd, (threads + 255) / 256, 1, 1);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, 0, 0, 0);
  }
}

// tec: best time of `passes` full reads of `data`, per pass
internal U64
tx_time_read_kernel(TX_Queue* queue, TX_Buffer* data, TX_Buffer* sink, U32 passes)
{
  U64 best = max_U64;
  for (U32 run = 0; run < TX_REPEATS + 1; run += 1)
  {
    VkCommandBuffer cmd = tx_begin(queue);
    tx_record_read_kernel(cmd, data, sink, passes);
    U64 elapsed = tx_submit_and_wait(queue, cmd);
    vkResetDescriptorPool(g_tx.device, g_tx.descriptor_pool, 0);
    if (run > 0)
    {
      best = Min(best, elapsed);
    }
  }
  return best / passes;
}

//~ tec: host side

typedef struct TX_CopyJob TX_CopyJob;
struct TX_CopyJob
{
  void* destination;
  void* source;
  U64 size;
};

internal void
tx_copy_job_run(void* ptr)
{
  TX_CopyJob* job = (TX_CopyJob*)ptr;
  MemoryCopy(job->destination, job->source, job->size);
}

// tec: memcpy split over `thread_count` threads, the calling thread takes the first part
internal void
tx_parallel_copy(void* destination, void* source, U64 size, U32 thread_count)
{
  TX_CopyJob jobs[32];
  OS_Handle threads[32];
  thread_count = Clamp(1, thread_count, 32);
  U64 part = (size / thread_count) & ~(U64)63;
  for (U32 index = 0; index < thread_count; index += 1)
  {
    U64 offset = part * index;
    U64 length = index + 1 == thread_count ? size - offset : part;
    jobs[index] = (TX_CopyJob){ (U8*)destination + offset, (U8*)source + offset, length };
  }
  for (U32 index = 1; index < thread_count; index += 1)
  {
    threads[index] = os_thread_launch(tx_copy_job_run, &jobs[index], 0);
  }
  tx_copy_job_run(&jobs[0]);
  for (U32 index = 1; index < thread_count; index += 1)
  {
    os_thread_join(threads[index], max_U64);
  }
}

internal void*
tx_host_alloc(U64 size, U64* touch_microseconds)
{
  void* ptr = os_reserve(size);
  os_commit(ptr, size);
  U64 start = os_now_microseconds();
  for (U64 offset = 0; offset < size; offset += 4096)
  {
    ((U8*)ptr)[offset] = (U8)(offset >> 12);
  }
  if (touch_microseconds)
  {
    *touch_microseconds = os_now_microseconds() - start;
  }
  return ptr;
}

// tec: --only=<name> runs one section, no flag runs all of them
internal B32
tx_selected(String8 only, char* name)
{
  return only.size == 0 || str8_match(only, str8_cstring(name), 0);
}

internal void
tx_print_pcie_link(void)
{
  FILE* pipe = _popen("powershell -NoProfile -Command \"Get-PnpDevice -Class Display -Status OK | ForEach-Object { $n=$_.FriendlyName; $_ | Get-PnpDeviceProperty -KeyName "
                      "'DEVPKEY_PciDevice_CurrentLinkSpeed','DEVPKEY_PciDevice_CurrentLinkWidth' | ForEach-Object { $n + ' ' + $_.KeyName + '=' + $_.Data } }\"", "r");
  if (!pipe)
  {
    tx_say("PCIe link: could not be read\n");
    return;
  }
  char line[256];
  U32 speed_code = 0;
  U32 width = 0;
  while (fgets(line, sizeof(line), pipe))
  {
    char* equals = strrchr(line, '=');
    if (!equals)
    {
      continue;
    }
    if (strstr(line, "CurrentLinkSpeed"))
    {
      speed_code = (U32)atoi(equals + 1);
    }
    else if (strstr(line, "CurrentLinkWidth"))
    {
      width = (U32)atoi(equals + 1);
    }
  }
  _pclose(pipe);
  // tec: the speed property is an enum, 1 = 2.5 GT/s up to 5 = 32 GT/s. the encoding is 8b/10b up to gen 2 and 128b/130b from gen 3
  F64 transfers[] = { 0.0, 2.5, 5.0, 8.0, 16.0, 32.0, 64.0 };
  if (speed_code == 0 || speed_code >= ArrayCount(transfers) || width == 0)
  {
    tx_say("PCIe link: could not be read\n");
    return;
  }
  F64 efficiency = speed_code <= 2 ? 0.8 : 128.0 / 130.0;
  F64 theoretical = transfers[speed_code] * efficiency * (F64)width / 8.0;
  tx_say("PCIe link: gen %u x%u, %.1f GT/s per lane, %.1f GB/s theoretical per direction before protocol overhead\n", speed_code, width, transfers[speed_code], theoretical);
}

//~ tec: the measurements

global F64 g_best_copy_gbps = 0.0;
global F64 g_best_pipeline_gbps[3] = {0};

internal void
tx_test_host_memory(U64 host_bytes)
{
  tx_say("\n== host memory ==\n");
  U64 touch_us = 0;
  void* source = tx_host_alloc(host_bytes, &touch_us);
  tx_say("first touch of %llu MB: %.2f GB/s\n", host_bytes / MB(1), tx_gbps(host_bytes, touch_us));

  U64 test_bytes = Min(host_bytes / 2, MB(512));
  void* host_destination = tx_host_alloc(test_bytes, 0);
  TX_Buffer staging = tx_staging_alloc(test_bytes);
  U32 thread_counts[] = { 1, 2, 4, 6, 8 };
  tx_say("memcpy of %llu MB, best of %u:\n%8s %14s %18s\n", test_bytes / MB(1), TX_REPEATS, "threads", "to plain RAM", "to mapped staging");
  for (U32 index = 0; index < ArrayCount(thread_counts); index += 1)
  {
    U64 best_plain = max_U64;
    U64 best_staging = max_U64;
    for (U32 run = 0; run < TX_REPEATS + 1; run += 1)
    {
      U64 start = os_now_microseconds();
      tx_parallel_copy(host_destination, source, test_bytes, thread_counts[index]);
      U64 plain = os_now_microseconds() - start;
      U64 staging_us = 0;
      if (staging.mapped)
      {
        start = os_now_microseconds();
        tx_parallel_copy(staging.mapped, source, test_bytes, thread_counts[index]);
        staging_us = os_now_microseconds() - start;
      }
      if (run > 0)
      {
        best_plain = Min(best_plain, plain);
        best_staging = Min(best_staging, staging_us != 0 ? staging_us : max_U64);
      }
    }
    tx_say("%8u %11.2f GB/s %14.2f GB/s\n", thread_counts[index], tx_gbps(test_bytes, best_plain), best_staging == max_U64 ? 0.0 : tx_gbps(test_bytes, best_staging));
  }
  tx_buffer_release(&staging);
  os_release(host_destination, test_bytes);
  os_release(source, host_bytes);
}

// tec: what allocating device memory costs, one block against many, and a second round after freeing
internal void
tx_test_vram_alloc(void)
{
  tx_say("\n== allocating VRAM ==\n");
  U64 total = MB(2048);
  U64 sizes[] = { MB(2048), MB(512), MB(128), MB(32) };
  for (U32 index = 0; index < ArrayCount(sizes); index += 1)
  {
    U64 count = total / sizes[index];
    TX_Buffer* buffers = push_array(g_tx_arena, TX_Buffer, count);
    for (U32 round = 0; round < 2; round += 1)
    {
      U64 start = os_now_microseconds();
      for (U64 buffer = 0; buffer < count; buffer += 1)
      {
        buffers[buffer] = tx_vram_alloc(sizes[index]);
      }
      U64 allocated = os_now_microseconds() - start;
      start = os_now_microseconds();
      for (U64 buffer = 0; buffer < count; buffer += 1)
      {
        tx_buffer_release(&buffers[buffer]);
      }
      U64 freed = os_now_microseconds() - start;
      tx_say("%llu x %llu MB, round %u: allocate %.1f ms (%.1f GB/s), free %.1f ms\n", count, sizes[index] / MB(1), round, (F64)allocated / 1000.0, tx_gbps(total, allocated), (F64)freed / 1000.0);
    }
  }
}

internal void
tx_test_staging_copy(void)
{
  tx_say("\n== copy engine from a staging buffer to VRAM ==\n");
  U64 sizes[] = { MB(16), MB(64), MB(256) };
  tx_say("%-22s", "queue");
  for (U32 index = 0; index < ArrayCount(sizes); index += 1)
  {
    tx_say(" %10llu MB", sizes[index] / MB(1));
  }
  tx_say("\n");
  TX_Buffer staging = tx_staging_alloc(MB(256));
  TX_Buffer vram = tx_vram_alloc(MB(256));
  if (!staging.buffer || !vram.buffer)
  {
    tx_say("could not allocate the buffers\n");
    tx_buffer_release(&staging);
    tx_buffer_release(&vram);
    return;
  }
  MemorySet(staging.mapped, 0x5a, staging.size);
  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    if (!g_tx.queues[kind].available)
    {
      continue;
    }
    tx_say("%-22s", g_queue_kind_names[kind]);
    for (U32 index = 0; index < ArrayCount(sizes); index += 1)
    {
      U64 elapsed = tx_time_copy(&g_tx.queues[kind], staging.buffer, 0, vram.buffer, sizes[index]);
      F64 rate = tx_gbps(sizes[index], elapsed);
      g_best_copy_gbps = Max(g_best_copy_gbps, rate);
      tx_say(" %9.2f GB/s", rate);
    }
    tx_say("\n");
  }
  tx_buffer_release(&staging);
  tx_buffer_release(&vram);
}

internal void
tx_test_import(void* host, U64 host_bytes)
{
  tx_say("\n== path B: imported host memory ==\n");
  if (!g_tx.has_host_import)
  {
    tx_say("not supported on this device\n");
    return;
  }
  U64 window_sizes[] = { MB(64), MB(256), MB(1024) };
  TX_Buffer vram = tx_vram_alloc(MB(1024));
  if (!vram.buffer)
  {
    tx_say("could not allocate the VRAM buffer\n");
    return;
  }
  tx_say("%10s %12s %12s %14s", "window", "import", "pin rate", "second import");
  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    if (g_tx.queues[kind].available)
    {
      tx_say(" %22s", g_queue_kind_names[kind]);
    }
  }
  tx_say("\n");
  for (U32 index = 0; index < ArrayCount(window_sizes); index += 1)
  {
    U64 window = window_sizes[index];
    if (window > host_bytes)
    {
      continue;
    }
    U64 first_us = 0;
    TX_Buffer imported = tx_import_host(host, window, &first_us);
    if (!imported.buffer)
    {
      tx_say("%7llu MB import failed\n", window / MB(1));
      continue;
    }
    tx_say("%7llu MB %9.2f ms %8.2f GB/s", window / MB(1), (F64)first_us / 1000.0, tx_gbps(window, first_us));

    U64 second_us = 0;
    TX_Buffer second = tx_import_host(host, window, &second_us);
    tx_buffer_release(&second);
    tx_say(" %11.2f ms", (F64)second_us / 1000.0);

    for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
    {
      if (!g_tx.queues[kind].available)
      {
        continue;
      }
      U64 elapsed = tx_time_copy(&g_tx.queues[kind], imported.buffer, 0, vram.buffer, window);
      F64 rate = tx_gbps(window, elapsed);
      g_best_copy_gbps = Max(g_best_copy_gbps, rate);
      tx_say(" %17.2f GB/s", rate);
    }
    tx_say("\n");
    tx_buffer_release(&imported);
  }
  tx_buffer_release(&vram);
}

internal void
tx_test_kernel_reads(void* host, U64 host_bytes)
{
  tx_say("\n== path C: a kernel reading the data where it is ==\n");
  if (!g_tx.has_read_kernel)
  {
    tx_say("skipped, no kernel\n");
    return;
  }
  U64 bytes = MB(1024);
  TX_Buffer vram = tx_vram_alloc(bytes);
  TX_Buffer sink = tx_vram_alloc(KB(4));
  if (!vram.buffer || !sink.buffer)
  {
    tx_say("could not allocate the buffers\n");
    tx_buffer_release(&vram);
    tx_buffer_release(&sink);
    return;
  }
  TX_Queue* queue = &g_tx.queues[TX_QueueKind_Compute];
  U64 vram_us = tx_time_read_kernel(queue, &vram, &sink, 4);
  tx_say("kernel reading VRAM:                   %8.2f GB/s (%llu MB in %.2f ms)\n", tx_gbps(bytes, vram_us), bytes / MB(1), (F64)vram_us / 1000.0);

  if (g_tx.has_host_import && host_bytes >= bytes)
  {
    U64 import_us = 0;
    TX_Buffer imported = tx_import_host(host, bytes, &import_us);
    if (imported.buffer)
    {
      U64 host_us = tx_time_read_kernel(queue, &imported, &sink, 1);
      tx_say("kernel reading imported host memory:   %8.2f GB/s (%llu MB in %.2f ms)\n", tx_gbps(bytes, host_us), bytes / MB(1), (F64)host_us / 1000.0);
      g_best_pipeline_gbps[2] = tx_gbps(bytes, host_us);
      tx_buffer_release(&imported);
    }
  }
  tx_buffer_release(&vram);
  tx_buffer_release(&sink);
}

// tec: a kernel loop on the compute queue and a copy on `copy_queue` started together. the copy is imported memory to VRAM when imports work, else staging
internal void
tx_test_overlap(void* host, U64 host_bytes)
{
  tx_say("\n== overlap of a kernel loop with a copy ==\n");
  if (!g_tx.has_read_kernel)
  {
    tx_say("skipped, no kernel\n");
    return;
  }
  U64 data_bytes = MB(512);
  U64 copy_bytes = MB(512);
  TX_Buffer data = tx_vram_alloc(data_bytes);
  TX_Buffer sink = tx_vram_alloc(KB(4));
  TX_Buffer copy_destination = tx_vram_alloc(copy_bytes);
  U64 import_us = 0;
  TX_Buffer imported = {0};
  TX_Buffer staging = {0};
  VkBuffer copy_source = 0;
  char* copy_source_name = "imported host memory";
  if (g_tx.has_host_import && host_bytes >= copy_bytes)
  {
    imported = tx_import_host(host, copy_bytes, &import_us);
    copy_source = imported.buffer;
  }
  if (!copy_source)
  {
    staging = tx_staging_alloc(copy_bytes);
    MemorySet(staging.mapped, 0x33, staging.size);
    copy_source = staging.buffer;
    copy_source_name = "staging buffer";
  }
  if (!data.buffer || !sink.buffer || !copy_destination.buffer || !copy_source)
  {
    tx_say("could not allocate the buffers\n");
    goto done;
  }

  TX_Queue* compute = &g_tx.queues[TX_QueueKind_Compute];
  tx_say("kernel reads %llu MB of VRAM per pass; copy moves %llu MB from %s to VRAM\n", data_bytes / MB(1), copy_bytes / MB(1), copy_source_name);
  tx_say("%-22s %10s %10s %10s %10s %12s\n", "copy queue", "kernel ms", "copy ms", "both ms", "serial ms", "overlap eff");
  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    TX_Queue* copy_queue = &g_tx.queues[kind];
    if (!copy_queue->available)
    {
      continue;
    }
    U64 copy_us = tx_time_copy(copy_queue, copy_source, 0, copy_destination.buffer, copy_bytes);
    U64 one_pass_us = tx_time_read_kernel(compute, &data, &sink, 1);
    // tec: enough passes that the kernel loop takes about as long as the copy
    U32 passes = (U32)Clamp(1, (copy_us + one_pass_us - 1) / Max(one_pass_us, 1), 4000);
    U64 kernel_us = tx_time_read_kernel(compute, &data, &sink, passes) * passes;

    U64 best_both = max_U64;
    for (U32 run = 0; run < TX_REPEATS + 1; run += 1)
    {
      VkCommandBuffer kernel_cmd = tx_begin(compute);
      tx_record_read_kernel(kernel_cmd, &data, &sink, passes);
      VkCommandBuffer copy_cmd = 0;
      B32 same_queue = copy_queue == compute;
      if (same_queue)
      {
        tx_record_copy(kernel_cmd, copy_source, 0, copy_destination.buffer, 0, copy_bytes);
      }
      else
      {
        copy_cmd = tx_begin(copy_queue);
        tx_record_copy(copy_cmd, copy_source, 0, copy_destination.buffer, 0, copy_bytes);
      }
      U64 start = os_now_microseconds();
      if (copy_cmd)
      {
        tx_submit_async(copy_queue, copy_cmd);
      }
      tx_submit_async(compute, kernel_cmd);
      tx_wait(compute, kernel_cmd);
      if (copy_cmd)
      {
        tx_wait(copy_queue, copy_cmd);
      }
      U64 elapsed = os_now_microseconds() - start;
      vkResetDescriptorPool(g_tx.device, g_tx.descriptor_pool, 0);
      if (run > 0)
      {
        best_both = Min(best_both, elapsed);
      }
    }
    F64 efficiency = (F64)Max(kernel_us, copy_us) / (F64)best_both;
    tx_say("%-22s %10.2f %10.2f %10.2f %10.2f %12.2f\n", g_queue_kind_names[kind], (F64)kernel_us / 1000.0, (F64)copy_us / 1000.0, (F64)best_both / 1000.0, (F64)(kernel_us + copy_us) / 1000.0,
           efficiency);
  }

  done:
  tx_buffer_release(&data);
  tx_buffer_release(&sink);
  tx_buffer_release(&copy_destination);
  tx_buffer_release(&imported);
  tx_buffer_release(&staging);
}

// tec: path A end to end. a controlling thread memcpys the next slice into a free staging slot (with `feeders` threads) while the copy engine moves the
// slice before it. slots are reused as their copies finish
internal F64
tx_pipeline_staging(TX_Queue* copy_queue, void* host, U64 total_bytes, U64 slice_bytes, U32 slot_count, U32 feeders)
{
  TX_Buffer slots[8];
  VkFence fences[8];
  VkCommandBuffer commands[8];
  B32 pending[8] = {0};
  TX_Buffer vram = tx_vram_alloc(slice_bytes * slot_count);
  slot_count = Min(slot_count, (U32)ArrayCount(slots));
  B32 ok = vram.buffer != 0;
  for (U32 index = 0; index < slot_count; index += 1)
  {
    slots[index] = tx_staging_alloc(slice_bytes);
    ok = ok && slots[index].buffer != 0;
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(g_tx.device, &fence_info, 0, &fences[index]);
  }
  F64 rate = 0.0;
  if (ok)
  {
    U64 slice_count = total_bytes / slice_bytes;
    U64 start = os_now_microseconds();
    for (U64 slice = 0; slice < slice_count; slice += 1)
    {
      U32 slot = (U32)(slice % slot_count);
      if (pending[slot])
      {
        vkWaitForFences(g_tx.device, 1, &fences[slot], VK_TRUE, TX_TIMEOUT_NS);
        vkFreeCommandBuffers(g_tx.device, copy_queue->pool, 1, &commands[slot]);
        pending[slot] = 0;
      }
      tx_parallel_copy(slots[slot].mapped, (U8*)host + slice * slice_bytes, slice_bytes, feeders);
      commands[slot] = tx_begin(copy_queue);
      tx_record_copy(commands[slot], slots[slot].buffer, 0, vram.buffer, slot * slice_bytes, slice_bytes);
      vkEndCommandBuffer(commands[slot]);
      VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands[slot] };
      vkResetFences(g_tx.device, 1, &fences[slot]);
      vkQueueSubmit(copy_queue->queue, 1, &submit, fences[slot]);
      pending[slot] = 1;
    }
    for (U32 slot = 0; slot < slot_count; slot += 1)
    {
      if (pending[slot])
      {
        vkWaitForFences(g_tx.device, 1, &fences[slot], VK_TRUE, TX_TIMEOUT_NS);
        vkFreeCommandBuffers(g_tx.device, copy_queue->pool, 1, &commands[slot]);
      }
    }
    rate = tx_gbps(slice_count * slice_bytes, os_now_microseconds() - start);
  }
  for (U32 index = 0; index < slot_count; index += 1)
  {
    vkDestroyFence(g_tx.device, fences[index], 0);
    tx_buffer_release(&slots[index]);
  }
  tx_buffer_release(&vram);
  return rate;
}

// tec: path B end to end. each window of the host region is imported once, then copied through two VRAM slots on the copy queue with no host work
internal F64
tx_pipeline_import(TX_Queue* copy_queue, void* host, U64 total_bytes, U64 window_bytes, B32 include_import)
{
  U64 window_count = total_bytes / window_bytes;
  TX_Buffer vram = tx_vram_alloc(window_bytes * 2);
  if (!vram.buffer || window_count == 0)
  {
    tx_buffer_release(&vram);
    return 0.0;
  }
  TX_Buffer windows[64];
  window_count = Min(window_count, (U64)ArrayCount(windows));
  U64 import_total_us = 0;
  for (U64 index = 0; index < window_count; index += 1)
  {
    U64 import_us = 0;
    windows[index] = tx_import_host((U8*)host + index * window_bytes, window_bytes, &import_us);
    import_total_us += import_us;
    if (!windows[index].buffer)
    {
      window_count = index;
      break;
    }
  }

  F64 rate = 0.0;
  if (window_count > 0)
  {
    U64 best = max_U64;
    for (U32 run = 0; run < 3; run += 1)
    {
      VkCommandBuffer cmd = tx_begin(copy_queue);
      for (U64 index = 0; index < window_count; index += 1)
      {
        tx_record_copy(cmd, windows[index].buffer, 0, vram.buffer, (index % 2) * window_bytes, window_bytes);
      }
      // tec: Min evaluates its arguments twice, so the submit must not be inside it
      U64 elapsed = tx_submit_and_wait(copy_queue, cmd);
      best = Min(best, elapsed);
    }
    U64 moved = window_count * window_bytes;
    rate = tx_gbps(moved, include_import ? best + import_total_us : best);
  }
  for (U64 index = 0; index < window_count; index += 1)
  {
    tx_buffer_release(&windows[index]);
  }
  tx_buffer_release(&vram);
  return rate;
}

internal void
tx_test_pipelines(void* host, U64 host_bytes)
{
  tx_say("\n== streaming %llu MB from host memory to VRAM ==\n", host_bytes / MB(1));
  tx_say("path A, staging ring (memcpy by N threads into a ring of slots, copy engine drains it):\n");
  tx_say("%-22s %8s %6s %8s %14s\n", "queue", "slice", "slots", "threads", "rate");
  U32 feeder_counts[] = { 1, 2, 4, 6 };
  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    if (!g_tx.queues[kind].available)
    {
      continue;
    }
    for (U32 index = 0; index < ArrayCount(feeder_counts); index += 1)
    {
      F64 rate = tx_pipeline_staging(&g_tx.queues[kind], host, host_bytes, MB(64), 3, feeder_counts[index]);
      g_best_pipeline_gbps[0] = Max(g_best_pipeline_gbps[0], rate);
      tx_say("%-22s %5llu MB %6u %8u %9.2f GB/s\n", g_queue_kind_names[kind], (U64)64, 3, feeder_counts[index], rate);
    }
  }

  tx_say("path B, imported windows (no memcpy, no staging):\n");
  if (!g_tx.has_host_import)
  {
    tx_say("not supported on this device\n");
    return;
  }
  tx_say("%-22s %8s %16s %16s\n", "queue", "window", "copy only", "including import");
  U64 window_sizes[] = { MB(64), MB(256) };
  for (U32 kind = 0; kind < TX_QUEUE_KINDS; kind += 1)
  {
    if (!g_tx.queues[kind].available)
    {
      continue;
    }
    for (U32 index = 0; index < ArrayCount(window_sizes); index += 1)
    {
      F64 copy_rate = tx_pipeline_import(&g_tx.queues[kind], host, host_bytes, window_sizes[index], 0);
      F64 total_rate = tx_pipeline_import(&g_tx.queues[kind], host, host_bytes, window_sizes[index], 1);
      g_best_pipeline_gbps[1] = Max(g_best_pipeline_gbps[1], copy_rate);
      tx_say("%-22s %5llu MB %11.2f GB/s %11.2f GB/s\n", g_queue_kind_names[kind], window_sizes[index] / MB(1), copy_rate, total_rate);
    }
  }
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  setvbuf(stdout, NULL, _IONBF, 0);
  log_alloc();
  g_tx_arena = arena_alloc();

  if (!os_file_path_exists(str8_lit("bench_reports/")))
  {
    os_make_directory(str8_lit("bench_reports/"));
  }
  g_report = fopen("bench_reports/gpu_transfer_report.txt", "w");

  U64 host_mb = tx_u64_argument(cmdline, "mb", 2048);
  U64 host_bytes = host_mb * MB(1);

  tx_say("compute_ql host to VRAM transfer benchmark\n\n");
  if (!tx_setup())
  {
    log_release();
    os_abort(1);
  }
  tx_print_pcie_link();
  tx_load_read_kernel();

  String8 only = cmd_line_string(cmdline, str8_lit("only"));
  if (tx_selected(only, "host"))
  {
    tx_test_host_memory(host_bytes);
  }
  if (tx_selected(only, "alloc"))
  {
    tx_test_vram_alloc();
  }
  if (tx_selected(only, "staging"))
  {
    tx_test_staging_copy();
  }

  U64 touch_us = 0;
  void* host = tx_host_alloc(host_bytes, &touch_us);
  if (tx_selected(only, "import"))
  {
    tx_test_import(host, host_bytes);
  }
  if (tx_selected(only, "kernel"))
  {
    tx_test_kernel_reads(host, host_bytes);
  }
  if (tx_selected(only, "overlap"))
  {
    tx_test_overlap(host, host_bytes);
  }
  if (tx_selected(only, "pipelines"))
  {
    tx_test_pipelines(host, host_bytes);
  }

  tx_say("\n== summary ==\n");
  tx_say("highest raw copy engine rate seen:           %.2f GB/s (the ceiling for any copy based path)\n", g_best_copy_gbps);
  tx_say("path A, staging ring, best:                  %.2f GB/s\n", g_best_pipeline_gbps[0]);
  tx_say("path B, imported windows, best (copy only):  %.2f GB/s\n", g_best_pipeline_gbps[1]);
  tx_say("path C, kernel reads imported memory:        %.2f GB/s\n", g_best_pipeline_gbps[2]);

  if (g_report)
  {
    fclose(g_report);
  }
  os_release(host, host_bytes);
  log_release();
  ProfEndCapture();
}
