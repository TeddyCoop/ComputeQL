// compute_ql: ROW_NUMBER / RANK / DENSE_RANK on the generated sort and flags kernels against the precompiled ones, called directly on key arrays.
// keys avoid the composite sort. the returned order is checked: every row once, keys in order, and the three rank columns recomputed from that order
// --rows=N sets the number of rows
#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)
#define BENCH_TIMED_RUNS 5
#define BENCH_WARMUP_RUNS 2

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
#include "third_party/sqlite/sqlite3.h"
#include "third_party/duckdb/duckdb.h"

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

#include "tests/bench_common.h"

internal U64
win_verify(F64** keys, B32* desc, U32 key_count, U32 partition_count, U64 n, QE_WindowRankResult* r)
{
  if (!r->ok)
  {
    return 1;
  }
  U8* seen = (U8*)calloc(n, 1);
  U64 bad = 0;
  U32 part_start = 0;
  U32 peer_start = 0;
  U32 dense = 0;
  for (U64 i = 0; i < n && !bad; i++)
  {
    U32 row = r->orig_index[i];
    if (row >= n || seen[row])
    {
      bad = i + 1;
      break;
    }
    seen[row] = 1;
    B32 new_part = 1;
    B32 new_peer = 1;
    if (i > 0)
    {
      U32 prev = r->orig_index[i - 1];
      new_part = 0;
      new_peer = 0;
      B32 decided = 0;
      for (U32 k = 0; k < key_count; k++)
      {
        F64 a = keys[k][prev];
        F64 b = keys[k][row];
        if (a == b)
        {
          continue;
        }
        if (!decided && (desc[k] ? a < b : a > b))
        {
          bad = i + 1;
        }
        decided = 1;
        if (k < partition_count)
        {
          new_part = 1;
        }
        new_peer = 1;
        break;
      }
    }
    if (new_part)
    {
      part_start = (U32)i;
      peer_start = (U32)i;
      dense = 1;
    }
    else if (new_peer)
    {
      peer_start = (U32)i;
      dense += 1;
    }
    if (r->row_number[i] != (U32)(i - part_start + 1) || r->rank[i] != peer_start - part_start + 1 || r->dense_rank[i] != dense)
    {
      bad = i + 1;
    }
  }
  free(seen);
  return bad;
}

typedef struct WinCase WinCase;
struct WinCase
{
  char* label;
  U32 key_count;
  U32 partition_count;
  U32 desc_mask;
  U32 narrow;
};

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  log_alloc();

  U64 n = 200000;
  if (cmd_line_has_argument(cmdline, str8_lit("rows")))
  {
    n = u64_from_str8(cmd_line_string(cmdline, str8_lit("rows")), 10);
  }
  settings_load_from_string(str8_lit("QE_SORT_COMPOSITE:0"));
  cql_init();
  gpu_init();
  qk_init();

  Arena* arena = arena_alloc(.reserve_size = GB(2), .commit_size = MB(64));
  Bench_Rng rng = { 0x9E3779B97F4A7C15ULL };
  // tec: key 0 has 7 values, key 1 is wide with fractions, key 2 has 50 values, key 3 is wide
  F64* wide[4];
  F64* narrow[4];
  for (U32 k = 0; k < 4; k++)
  {
    wide[k] = push_array(arena, F64, n);
    narrow[k] = push_array(arena, F64, n);
    for (U64 i = 0; i < n; i++)
    {
      U64 r = bench_rng_next(&rng);
      F64 small_value = (F64)(r % (k == 0 ? 7u : 50u));
      narrow[k][i] = small_value;
      wide[k][i] = (k == 0 || k == 2) ? small_value : (F64)(r % 4000000000000ULL) / 7.0;
    }
  }

  WinCase cases[] =
  {
    { "order wide", 1, 0, 0, 0 },
    { "order wide desc", 1, 0, 1, 0 },
    { "partition 7, order wide", 2, 1, 0, 0 },
    { "partition 7, order wide desc", 2, 1, 2, 0 },
    { "partition 7, order 50, wide", 3, 1, 4, 0 },
    { "partitions 7 and wide, order 50", 3, 2, 0, 0 },
    { "four keys", 4, 2, 10, 0 },
    { "narrow partition and order", 2, 1, 0, 1 },
    { "narrow four keys with ties", 4, 2, 5, 1 },
  };
  U64 failures = 0;
  printf("window rank: %llu rows\n", n);
  for (U32 c = 0; c < ArrayCount(cases); c++)
  {
    WinCase* wc = &cases[c];
    F64* keys[4];
    B32 desc[4];
    for (U32 k = 0; k < wc->key_count; k++)
    {
      keys[k] = wc->narrow ? narrow[k] : wide[k];
      desc[k] = (wc->desc_mask >> k) & 1;
    }
    U64 bad[2] = {0};
    F64 ms[2] = {0};
    for (U32 mode = 0; mode < 2; mode++)
    {
      settings_load_from_string(mode ? str8_lit("QE_GENERATE_SORT:1") : str8_lit("QE_GENERATE_SORT:0"));
      F64 best = 1e30;
      for (U32 run = 0; run < 4; run++)
      {
        Temp scratch = scratch_begin(0, 0);
        U64 t0 = os_now_microseconds();
        QE_WindowRankResult r = qe_window_rank_gpu(scratch.arena, keys, desc, wc->key_count, wc->partition_count, n);
        F64 elapsed = (F64)(os_now_microseconds() - t0) / 1000.0;
        if (run == 0)
        {
          bad[mode] = win_verify(keys, desc, wc->key_count, wc->partition_count, n, &r);
        }
        else if (elapsed < best)
        {
          best = elapsed;
        }
        scratch_end(scratch);
      }
      ms[mode] = best;
    }
    if (bad[0] || bad[1])
    {
      failures += 1;
    }
    printf("%-34s precompiled %9.3f ms  generated %9.3f ms  %5.2fx%s%s\n", wc->label, ms[0], ms[1], ms[0] / Max(ms[1], 0.0001),
           bad[0] ? "  !! PRECOMPILED WRONG" : "", bad[1] ? "  !! GENERATED WRONG" : "");
    fflush(stdout);
  }
  printf("%llu failures\n", failures);
  arena_release(arena);
  log_release();
  ProfEnd();
  ProfEndCapture();
  if (failures > 0)
  {
    exit(1);
  }
}
