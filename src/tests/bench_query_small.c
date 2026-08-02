// compute_ql vs sqlite: small dataset (<100 rows), integer/string search.

#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1
#define GPU_MAX_BUFFER_SIZE GB(2)

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "gdb/gdb_inc.h"
#include "ir_gen/ir_gen_inc.h"
#include "gpu/gpu_inc.h"
#include "query_exec/query_exec.h"
#include "planner/planner.h"
#include "third_party/sqlite/sqlite3.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"

#include "tests/bench_common.h"

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();

  U64 t0 = os_now_microseconds();
  gdb_init();
  gpu_init();
  U64 t1 = os_now_microseconds();
  printf("engine startup (gdb_init + gpu_init, one-time): %.4f ms\n", (F64)(t1 - t0) / 1000.0);

  Arena* arena = arena_alloc(.reserve_size = GB(1), .commit_size = MB(64));
  bench_run_query_suite(arena, 50, "small");
  arena_release(arena);

  log_release();

  ProfEnd();
  ProfEndCapture();
}
