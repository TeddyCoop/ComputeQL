#ifndef OS_NET_H
#define OS_NET_H

//~ tec: @os_hooks Networking (Implemented Per-OS)
// IPv4/TCP for now. so blocking sockets
internal B32       os_net_init(void);
internal void      os_net_release(void);

internal OS_Handle os_net_listen(U16 port, U32 backlog);
// tec: out_peer_ip is optional
internal OS_Handle os_net_accept(OS_Handle listen_socket, Arena *arena, String8 *out_peer_ip);
internal OS_Handle os_net_connect(String8 host, U16 port);
internal void      os_net_close(OS_Handle socket);

// tec: single blocking send/recv
internal U64 os_net_send(OS_Handle socket, void *data, U64 size);
internal U64 os_net_recv(OS_Handle socket, void *out_data, U64 max_size);

//~ tec: helpers
internal B32 os_net_send_exact(OS_Handle socket, void *data, U64 size);
internal B32 os_net_recv_exact(OS_Handle socket, void *out_data, U64 size);

#endif //OS_NET_H
