#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1

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
#include "server/server.h"
#include "client/client.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "gpu/gpu_inc.c"
#include "ir_gen/ir_gen_inc.c"
#include "gdb/gdb_inc.c"
#include "query_exec/query_exec.c"
#include "planner/planner.c"
#include "application.c"
#include "thread_pool/thread_pool.c"
#include "server/server.c"
#include "client/client.c"

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();
  
  log_alloc();
  g_query_exec_mutex = os_mutex_alloc();

  String8 query_str = cmd_line_string(cmdline, str8_lit("query"));
  B32 valid_query = query_str.size != 0;
  B32 should_serve = cmd_line_has_flag(cmdline, str8_lit("serve"));
  String8 connect_str = cmd_line_string(cmdline, str8_lit("connect"));

  if (connect_str.size != 0)
  {
    // tec: pure network client so no local database/GPU state needed
    if (!os_net_init())
    {
      log_error("failed to initialize networking (os_net_init)");
    }
    else
    {
      Temp scratch = scratch_begin(0, 0);
      String8List connect_parts = str8_split_by_string_chars(scratch.arena, connect_str, str8_lit(":"), 0);

      if (connect_parts.first && connect_parts.first->next)
      {
        String8 host = connect_parts.first->string;
        U16 port = (U16)u64_from_str8(connect_parts.first->next->string, 10);

        if (valid_query) 
		{ 
	      client_run_one_shot(host, port, query_str); 
		}
        else
		{ 
	      client_run_interactive(host, port);
		}
      }
      else
      {
        log_error("--connect expects host:port");
      }

      scratch_end(scratch);
    }
  }
  else
  {
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

    if (should_serve)
    {
      String8 port_str = cmd_line_string(cmdline, str8_lit("serve"));
      U16 port = port_str.size ? (U16)u64_from_str8(port_str, 10) : 5432;

      if (!os_net_init())
      {
        log_error("failed to initialize networking (os_net_init)");
      }
      else
      {
		// tec: blocking accept loop, does not return for now
        server_run(port); 
      }
    }
    else if (valid_query)
    {
      app_execute_query(query_str);
    }
    else
    {
      log_info("invalid query %.*s", str8_varg(query_str));
    }
  }

  ProfEnd();
  ProfEndCapture();
  log_release();
}