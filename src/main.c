#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1

#define PRINT_SELECT_OUTPUT 0
//#define GPU_MAX_BUFFER_SIZE MB(512)
#define GPU_MAX_BUFFER_SIZE GB(2)

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "gdb/gdb_inc.h"
#include "ir_gen/ir_gen_inc.h"
#include "gpu/gpu_inc.h"
#include "query_exec/query_exec.h"
#include "planner/planner.h"
#include "application.h"
#include "thread_pool/thread_pool.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"
#include "application.c"
#include "thread_pool/thread_pool.c"

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  
  // tec: create needed folders
  {
    if (!os_file_path_exists(str8_lit("gdb_data/")))
    {
      os_make_directory(str8_lit("gdb_data/"));
    }
  }

  gdb_init();
  gpu_init();

  log_info("total gpu memory: %llu (MB)", gpu_device_total_memory() >> 20);

  String8 query_str = cmd_line_string(cmdline, str8_lit("query"));
  B32 valid_query = 0;
  {
    valid_query = query_str.size == 0 ? 0 : 1;
  }
  
  if (valid_query)
  {
    app_execute_query(query_str);
  }
  else
  {
    log_info("invalid query %.*s", str8_varg(query_str));
  }
  
  ProfEnd();
  ProfEndCapture();
  log_release();
}