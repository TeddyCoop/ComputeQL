#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "settings/settings.h"
#include "gpu/gpu_inc.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "settings/settings.c"
#include "gpu/gpu_inc.c"

//~ tec: device memory accounting, pooled buffer eviction and recovery from a failed pooled allocation

global U64 g_failed_checks = 0;

internal void
check(B32 condition, char* description)
{
  printf("  [%s] %s\n", condition ? "pass" : "FAIL", description);
  if (!condition)
  {
    g_failed_checks += 1;
  }
}

internal void*
host_alloc(U64 size)
{
  void* ptr = os_reserve(size);
  os_commit(ptr, size);
  return ptr;
}

internal B32
buffer_holds_pattern(GPU_Buffer* buffer, U32 seed, U64 words)
{
  U32* readback = (U32*)host_alloc(words * sizeof(U32));
  gpu_buffer_read(buffer, readback, words * sizeof(U32));
  B32 intact = 1;
  for (U64 index = 0; index < words && intact; index += 1)
  {
    intact = readback[index] == (U32)index + seed;
  }
  os_release(readback, words * sizeof(U32));
  return intact;
}

internal void
fill_pattern(U32* words, U64 count, U32 seed)
{
  for (U64 index = 0; index < count; index += 1)
  {
    words[index] = (U32)index + seed;
  }
}

internal void
test_accounting(void)
{
  printf("accounting\n");
  GPU_MemoryStats before = gpu_memory_stats();
  GPU_Buffer* buffer = gpu_buffer_alloc(MB(64), GPU_BufferFlag_ReadWrite, 0);
  GPU_MemoryStats during = gpu_memory_stats();
  check(buffer != 0, "64 MB buffer allocates");
  check(during.bytes_in_use >= before.bytes_in_use + MB(64), "bytes in use grows by at least the buffer size");
  check(during.bytes_peak >= during.bytes_in_use, "peak is at least the current use");
  gpu_buffer_release(buffer);
  GPU_MemoryStats after = gpu_memory_stats();
  check(after.bytes_in_use == before.bytes_in_use, "releasing the buffer returns the accounting to where it was");
}

internal void
test_failed_pool_entry_recovers(void)
{
  printf("failed pooled allocation\n");
  U64 impossible = gpu_device_total_memory() * 4;
  GPU_MemoryStats before = gpu_memory_stats();
  GPU_Buffer* huge = gpu_buffer_alloc_pooled(str8_lit("test_recover"), impossible, GPU_BufferFlag_Write, 0);
  check(huge == 0, "a buffer several times the size of the device is refused");
  
  GPU_Buffer* tiny = gpu_buffer_alloc_pooled(str8_lit("test_recover"), MB(1), GPU_BufferFlag_Write, 0);
  check(tiny != 0, "the same pool name works for a smaller buffer afterwards");
  
  GPU_Buffer* again = gpu_buffer_alloc_pooled(str8_lit("test_recover"), MB(1), GPU_BufferFlag_Write, 0);
  check(again != 0 && again == tiny, "and a repeat lookup returns the same buffer");
  GPU_MemoryStats after = gpu_memory_stats();
  check(after.bytes_in_use >= before.bytes_in_use, "accounting did not go backwards");
}

// tec: fills device memory with pooled buffers from one epoch, starts a new epoch, then asks for more than is left
internal void
test_eviction(void)
{
  printf("eviction\n");
  if (!g_vulkan_state->has_memory_budget_ext)
  {
    printf("  VK_EXT_memory_budget not present, the free memory figure is the heap size, skipping\n");
    return;
  }
  
  U64 chunk_bytes = MB(256);
  U64 chunk_words = chunk_bytes / sizeof(U32);
  U32* pattern = (U32*)host_alloc(chunk_bytes);
  
  gpu_pool_begin_epoch();
  // tec: the entry from the previous test is older than this epoch, looking it up again keeps it from counting as an eviction below
  gpu_buffer_alloc_pooled(str8_lit("test_recover"), MB(1), GPU_BufferFlag_Write, 0);
  U64 filled = 0;
  U64 max_chunks = 60;
  for (U64 index = 0; index < max_chunks; index += 1)
  {
    if (gpu_device_free_memory() < chunk_bytes * 2)
    {
      break;
    }
    String8 name = push_str8f(gpu_scratch_arena(), "test_fill_%llu", index);
    fill_pattern(pattern, chunk_words, (U32)index * 7);
    GPU_Buffer* buffer = gpu_buffer_alloc_pooled(name, chunk_bytes, GPU_BufferFlag_Write, pattern);
    if (!buffer)
    {
      break;
    }
    filled += 1;
  }
  GPU_MemoryStats full = gpu_memory_stats();
  printf("  filled with %llu pooled buffers of %llu MB, %llu MB in use, driver reports %llu MB free\n", filled, chunk_bytes / MB(1), full.bytes_in_use / MB(1), gpu_device_free_memory() / MB(1));
  check(filled > 0, "pool filled");
  
  // tec: same epoch, everything is pinned, so a request that does not fit fails or spills without freeing the pool
  U64 free_now = gpu_device_free_memory();
  GPU_Buffer* pinned_big = gpu_buffer_alloc_pooled(str8_lit("test_pinned_big"), Min(free_now + chunk_bytes, GB(2)), GPU_BufferFlag_Write, 0);
  GPU_MemoryStats pinned = gpu_memory_stats();
  printf("  same epoch request for %llu MB: %s\n", Min(free_now + chunk_bytes, GB(2)) / MB(1), pinned_big ? "granted by the driver" : "refused");
  check(pinned.pool_evictions == full.pool_evictions, "buffers used in the current epoch are not evicted");
  if (filled > 0)
  {
    GPU_Buffer* first = gpu_buffer_alloc_pooled(str8_lit("test_fill_0"), chunk_bytes, GPU_BufferFlag_Write, 0);
    check(first != 0 && buffer_holds_pattern(first, 0, chunk_words), "a buffer pinned in the current epoch still holds its data");
  }
  
  gpu_pool_begin_epoch();
  U64 request = Min(gpu_device_free_memory() + chunk_bytes * 2, GB(2));
  GPU_Buffer* big = gpu_buffer_alloc_pooled(str8_lit("test_evict_big"), request, GPU_BufferFlag_Write, 0);
  GPU_MemoryStats evicted = gpu_memory_stats();
  printf("  next epoch request for %llu MB: %s, %llu evictions (%llu MB), %llu retried allocations succeeded\n", request / MB(1), big ? "granted" : "refused",
         evicted.pool_evictions - pinned.pool_evictions, (evicted.pool_evicted_bytes - pinned.pool_evicted_bytes) / MB(1), evicted.alloc_retry_successes);
  check(big != 0, "an allocation bigger than the free memory succeeds in a new epoch");
  check(evicted.pool_evictions > pinned.pool_evictions, "least recently used pooled buffers were evicted to make room");
  
  // tec: a buffer evicted from the pool is allocated again from scratch on its next lookup and has to be usable
  GPU_Buffer* reloaded = gpu_buffer_alloc_pooled(str8_lit("test_fill_0"), chunk_bytes, GPU_BufferFlag_Write, pattern);
  check(reloaded != 0, "an evicted entry can be allocated again");
  
  os_release(pattern, chunk_bytes);
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  setvbuf(stdout, NULL, _IONBF, 0);
  log_alloc();
  gpu_init();
  
  printf("device memory %llu MB, driver reports %llu MB free, memory budget extension %s\n", gpu_device_total_memory() / MB(1), gpu_device_free_memory() / MB(1),
         g_vulkan_state->has_memory_budget_ext ? "present" : "absent");
  
  test_accounting();
  test_failed_pool_entry_recovers();
  test_eviction();
  
  GPU_MemoryStats stats = gpu_memory_stats();
  printf("\nfinal: in use %llu MB, peak %llu MB, failures %llu, retries that succeeded %llu, evictions %llu (%llu MB)\n", stats.bytes_in_use / MB(1), stats.bytes_peak / MB(1),
         stats.alloc_failures, stats.alloc_retry_successes, stats.pool_evictions, stats.pool_evicted_bytes / MB(1));
  printf("%s\n", g_failed_checks == 0 ? "all checks passed" : "CHECKS FAILED");
  
  log_release();
  ProfEndCapture();
  if (g_failed_checks != 0)
  {
    os_abort(1);
  }
}
