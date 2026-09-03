#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1

#include "base/base_inc.h"
#include "os/os_inc.h"

#include "base/base_inc.c"
#include "os/os_inc.c"

#define NET_SMOKE_TEST_PORT 47321

internal void
net_smoke_echo_thread(void *ptr)
{
  OS_Handle listen_socket = *(OS_Handle*)ptr;
  OS_Handle conn = os_net_accept(listen_socket, 0, 0);
  if(os_handle_match(conn, os_handle_zero()))
  {
    log_error("net_loopback_smoke: accept failed");
    return;
  }

  U8 buf[64] = {0};
  U64 msg_size = 14; // "hello, socket!"
  if(os_net_recv_exact(conn, buf, msg_size))
  {
    os_net_send_exact(conn, buf, msg_size);
  }
  os_net_close(conn);
}

internal void
entry_point(CmdLine *cmdline)
{
  ProfBeginCapture();
  ProfBeginFunction();

  log_alloc();

  B32 pass = 0;

  if(!os_net_init())
  {
    log_error("net_loopback_smoke: os_net_init failed");
  }
  else
  {
    OS_Handle listen_socket = os_net_listen(NET_SMOKE_TEST_PORT, 1);
    if(os_handle_match(listen_socket, os_handle_zero()))
    {
      log_error("net_loopback_smoke: listen failed");
    }
    else
    {
      OS_Handle echo_thread = os_thread_launch(net_smoke_echo_thread, &listen_socket, 0);

      OS_Handle client = os_net_connect(str8_lit("127.0.0.1"), NET_SMOKE_TEST_PORT);
      if(os_handle_match(client, os_handle_zero()))
      {
        log_error("net_loopback_smoke: connect failed");
      }
      else
      {
        String8 msg = str8_lit("hello, socket!");
        U8 recv_buf[64] = {0};

        B32 round_trip_ok = os_net_send_exact(client, msg.str, msg.size) &&
                             os_net_recv_exact(client, recv_buf, msg.size);

        os_net_close(client);

        pass = round_trip_ok && MemoryMatch(msg.str, recv_buf, msg.size);
      }

      os_thread_join(echo_thread, os_now_microseconds() + Million(5));
      os_net_close(listen_socket);
    }

    os_net_release();
  }

  if(pass) 
  { 
    log_info("net_loopback_smoke: PASS"); 
  }
  else
  {
	log_error("net_loopback_smoke: FAIL"); 
  }

  ProfEnd();
  ProfEndCapture();
  log_release();
}
