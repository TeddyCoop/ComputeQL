#ifndef SERVER_H
#define SERVER_H

internal void server_run(U16 port);

//~ tec: connection
global Arena *g_server_arena = 0;

typedef struct SRV_ConnCtx SRV_ConnCtx;
struct SRV_ConnCtx
{
  OS_Handle socket;
};

internal void server_connection_thread_proc(void *ptr);

#endif //SERVER_H
