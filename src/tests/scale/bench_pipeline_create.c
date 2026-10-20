// how long the driver takes to build each of compute_ql's compute pipelines, and the registers, LDS and scratch each one uses.
// a cold build gets a SPIR-V copy with a unique OpString so no driver shader cache can hold it, a warm build reads a VkPipelineCache filled by a cold one.
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define BENCH_TIMED_RUNS 1
#define BENCH_WARMUP_RUNS 1

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "thread_pool/thread_pool.h"
#include "settings/settings.h"
#include "cql/cql_inc.h"
#include "ir_gen/ir_gen_inc.h"
#include "gpu/gpu_inc.h"
#include "planner/plan_node.h"
#include "query_exec/query_exec.h"
#include "optimizer/optimizer_inc.h"
#include "planner/planner.h"
#include "application.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "thread_pool/thread_pool.c"
#include "settings/settings.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "cql/cql_inc.c"
#include "query_exec/query_exec.c"
#include "optimizer/optimizer_inc.c"
#include "planner/planner.c"
#include "application.c"

#define PC_REPEATS 5

global char* g_pc_kernels[] =
{
  "scan_simple", "scan_filter", "scan_filter_typed", "selection_gather",
  "aggregate_assign", "aggregate_compact", "aggregate_reduce", "aggregate_reduce_f32", "aggregate_tile_reduce",
  "aggregate_dense_f32", "aggregate_dense_f64", "aggregate_dense_native", "aggregate_global_f32", "aggregate_global_native",
  "aggregate_hash_reduce_f32", "aggregate_hash_reduce_fixed",
  "hash_join_build_count", "hash_join_probe", "join_pack_key", "join_probe_native", "csr_scatter", "csr_scatter_tiled", "prefix_sum", "prefix_sum2",
  "bitonic_sort", "bitonic_sort_f32", "bitonic_sort_u64", "window_rank_flags", "window_rank_gather", "window_rank_scatter",
};

typedef struct PC_Build PC_Build;
struct PC_Build
{
  F64 module_ms;
  F64 pipeline_ms;
  VkPipeline pipeline;
  VkShaderModule module;
};

internal String8 pc_unique_spirv(Arena* arena, String8 spirv, U64 tag);
internal PC_Build pc_build(String8 spirv, VkPipelineCache cache);
internal void pc_release(PC_Build* build);
internal F64 pc_median(F64* values, U64 count);

// tec: a copy of spirv with an OpString "cql-pc-<tag>" placed where the debug instructions start, under a new result id
internal String8
pc_unique_spirv(Arena* arena, String8 spirv, U64 tag)
{
  U32* words = (U32*)spirv.str;
  U64 word_count = spirv.size / 4;

  char text[32];
  int text_len = snprintf(text, sizeof(text), "cql-pc-%llu", tag);
  U64 string_words = (U64)(text_len + 1 + 3) / 4;
  U64 insert_words = 2 + string_words;

  // tec: OpCapability, OpExtension, OpExtInstImport, OpMemoryModel, OpEntryPoint and OpExecutionMode(Id) come before any debug instruction
  U64 at = 5;
  while (at < word_count)
  {
    U32 opcode = words[at] & 0xffffu;
    U32 length = words[at] >> 16;
    if (!(opcode == 17 || opcode == 10 || opcode == 11 || opcode == 14 || opcode == 15 || opcode == 16 || opcode == 331) || length == 0)
    {
      break;
    }
    at += length;
  }

  U32* out = push_array(arena, U32, word_count + insert_words);
  MemoryCopy(out, words, at * 4);
  U32 id = words[3];
  out[3] = id + 1;
  out[at] = ((U32)insert_words << 16) | 7u;
  out[at + 1] = id;
  MemoryCopy(&out[at + 2], text, (U64)text_len);
  MemoryCopy(&out[at + insert_words], &words[at], (word_count - at) * 4);
  return str8((U8*)out, (word_count + insert_words) * 4);
}

internal PC_Build
pc_build(String8 spirv, VkPipelineCache cache)
{
  PC_Build build = {0};
  VkShaderModuleCreateInfo module_info =
  {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = spirv.size,
    .pCode = (U32*)spirv.str,
  };
  U64 t0 = os_now_microseconds();
  if (vkCreateShaderModule(g_vulkan_state->device, &module_info, 0, &build.module) != VK_SUCCESS)
  {
    log_error("bench_pipeline_create: vkCreateShaderModule failed");
    return build;
  }
  U64 t1 = os_now_microseconds();

  VkComputePipelineCreateInfo pipeline_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage =
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = build.module,
      .pName = "main",
    },
    .layout = g_vulkan_state->shared_pipeline_layout,
  };
  if (vkCreateComputePipelines(g_vulkan_state->device, cache, 1, &pipeline_info, 0, &build.pipeline) != VK_SUCCESS)
  {
    log_error("bench_pipeline_create: vkCreateComputePipelines failed");
    build.pipeline = VK_NULL_HANDLE;
  }
  U64 t2 = os_now_microseconds();
  build.module_ms = (F64)(t1 - t0) / 1000.0;
  build.pipeline_ms = (F64)(t2 - t1) / 1000.0;
  return build;
}

internal void
pc_release(PC_Build* build)
{
  if (build->pipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(g_vulkan_state->device, build->pipeline, 0);
  }
  if (build->module != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(g_vulkan_state->device, build->module, 0);
  }
  MemoryZeroStruct(build);
}

internal F64
pc_median(F64* values, U64 count)
{
  for (U64 i = 1; i < count; i++)
  {
    F64 key = values[i];
    U64 j = i;
    while (j > 0 && values[j - 1] > key)
    {
      values[j] = values[j - 1];
      j -= 1;
    }
    values[j] = key;
  }
  return values[count / 2];
}

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();
  settings_load_from_string(str8_lit("GPU_VULKAN_SHADER_INFO:1"));
  cql_init();
  gpu_init();

  printf("shader info extension: %s\n", g_vulkan_state->has_shader_info_ext ? "yes" : "no");
  printf("| kernel | SPIR-V bytes | first build in process (ms) | cold build, median of %d (ms) | same SPIR-V again, no cache (ms) | warm from VkPipelineCache (ms) | "
         "VGPRs used / available | SGPRs used | LDS bytes | scratch bytes |\n", PC_REPEATS);
  printf("|---|---|---|---|---|---|---|---|---|---|\n");

  U64 tag = os_now_microseconds();
  for (U64 k = 0; k < ArrayCount(g_pc_kernels); k++)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 name = str8_cstring(g_pc_kernels[k]);
    String8 spirv = gpu_vulkan_load_spirv_from_disk(scratch.arena, name);
    if (spirv.size == 0)
    {
      scratch_end(scratch);
      continue;
    }

    // tec: the plain binary first, as the engine builds it, which the driver's own cache may already hold from earlier runs
    PC_Build first = pc_build(spirv, VK_NULL_HANDLE);
    F64 first_ms = first.module_ms + first.pipeline_ms;

    VkShaderStatisticsInfoAMD stats = {0};
    B32 have_stats = 0;
    if (g_vulkan_state->has_shader_info_ext && first.pipeline != VK_NULL_HANDLE)
    {
      size_t stats_size = sizeof(stats);
      have_stats = g_vulkan_state->vkGetShaderInfoAMD_fn(g_vulkan_state->device, first.pipeline, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_INFO_TYPE_STATISTICS_AMD,
                                                         &stats_size, &stats) == VK_SUCCESS;
    }
    pc_release(&first);

    F64 cold[PC_REPEATS];
    F64 again[PC_REPEATS];
    F64 warm[PC_REPEATS];
    for (U64 r = 0; r < PC_REPEATS; r++)
    {
      String8 unique = pc_unique_spirv(scratch.arena, spirv, tag++);
      PC_Build c = pc_build(unique, VK_NULL_HANDLE);
      cold[r] = c.module_ms + c.pipeline_ms;
      pc_release(&c);

      PC_Build a = pc_build(spirv, VK_NULL_HANDLE);
      again[r] = a.module_ms + a.pipeline_ms;
      pc_release(&a);

      // tec: a fresh cache filled by one cold build, then the timed build reads it
      VkPipelineCacheCreateInfo cache_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
      VkPipelineCache cache = VK_NULL_HANDLE;
      vkCreatePipelineCache(g_vulkan_state->device, &cache_info, 0, &cache);
      String8 cached = pc_unique_spirv(scratch.arena, spirv, tag++);
      PC_Build fill = pc_build(cached, cache);
      pc_release(&fill);
      PC_Build w = pc_build(cached, cache);
      warm[r] = w.module_ms + w.pipeline_ms;
      pc_release(&w);
      vkDestroyPipelineCache(g_vulkan_state->device, cache, 0);
    }

    printf("| %s | %llu | %.2f | %.2f | %.2f | %.2f |", g_pc_kernels[k], spirv.size, first_ms, pc_median(cold, PC_REPEATS), pc_median(again, PC_REPEATS),
           pc_median(warm, PC_REPEATS));
    if (have_stats)
    {
      printf(" %u / %u | %u | %u | %u |\n", stats.resourceUsage.numUsedVgprs, stats.numAvailableVgprs, stats.resourceUsage.numUsedSgprs,
             (U32)stats.resourceUsage.ldsUsageSizeInBytes, (U32)stats.resourceUsage.scratchMemUsageInBytes);
    }
    else
    {
      printf(" n/a | n/a | n/a | n/a |\n");
    }
    scratch_end(scratch);
  }

  log_release();
  ProfEnd();
  ProfEndCapture();
}
