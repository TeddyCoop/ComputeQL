////////////////////////////////
//~ tec: @os_hooks Networking (Implemented Per-OS)

internal B32
os_net_init(void)
{
  WSADATA wsa_data = {0};
  int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  return (result == 0);
}

internal void
os_net_release(void)
{
  WSACleanup();
}

internal OS_Handle
os_net_listen(U16 port, U32 backlog)
{
  OS_Handle result = os_handle_zero();

  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if(sock == INVALID_SOCKET) { return result; }

  BOOL reuse_addr = TRUE;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse_addr, sizeof(reuse_addr));

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if(bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
     listen(sock, (int)backlog) == SOCKET_ERROR)
  {
    closesocket(sock);
    return result;
  }

  result.u64[0] = (U64)sock;
  return result;
}

internal OS_Handle
os_net_accept(OS_Handle listen_socket, Arena *arena, String8 *out_peer_ip)
{
  OS_Handle result = os_handle_zero();

  SOCKET listen_sock = (SOCKET)listen_socket.u64[0];
  struct sockaddr_in peer_addr = {0};
  int peer_addr_len = sizeof(peer_addr);

  SOCKET conn_sock = accept(listen_sock, (struct sockaddr*)&peer_addr, &peer_addr_len);
  if(conn_sock == INVALID_SOCKET) { return result; }

  if(out_peer_ip != 0)
  {
    char ip_buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
    *out_peer_ip = push_str8_copy(arena, str8_cstring(ip_buf));
  }

  result.u64[0] = (U64)conn_sock;
  return result;
}

internal OS_Handle
os_net_connect(String8 host, U16 port)
{
  OS_Handle result = os_handle_zero();

  char host_cstr[256];
  U64 copy_size = Min(host.size, sizeof(host_cstr) - 1);
  MemoryCopy(host_cstr, host.str, copy_size);
  host_cstr[copy_size] = 0;

  char port_cstr[8];
  snprintf(port_cstr, sizeof(port_cstr), "%u", (U32)port);

  struct addrinfo hints = {0};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo *addr_result = 0;
  if(getaddrinfo(host_cstr, port_cstr, &hints, &addr_result) != 0) { return result; }

  SOCKET sock = INVALID_SOCKET;
  for(struct addrinfo *node = addr_result; node != 0; node = node->ai_next)
  {
    sock = socket(node->ai_family, node->ai_socktype, node->ai_protocol);
    if(sock == INVALID_SOCKET) { continue; }
    if(connect(sock, node->ai_addr, (int)node->ai_addrlen) == 0) { break; }
    closesocket(sock);
    sock = INVALID_SOCKET;
  }
  freeaddrinfo(addr_result);

  if(sock != INVALID_SOCKET) { result.u64[0] = (U64)sock; }
  return result;
}

internal void
os_net_close(OS_Handle socket)
{
  if(os_handle_match(socket, os_handle_zero())) { return; }
  closesocket((SOCKET)socket.u64[0]);
}

#define OS_NET_W32_MAX_CHUNK 0x7fffffff

internal U64
os_net_send(OS_Handle socket, void *data, U64 size)
{
  int sent = send((SOCKET)socket.u64[0], (const char*)data, (int)Min(size, (U64)OS_NET_W32_MAX_CHUNK), 0);
  if(sent == SOCKET_ERROR) { return 0; }
  return (U64)sent;
}

internal U64
os_net_recv(OS_Handle socket, void *out_data, U64 max_size)
{
  int received = recv((SOCKET)socket.u64[0], (char*)out_data, (int)Min(max_size, (U64)OS_NET_W32_MAX_CHUNK), 0);
  if(received == SOCKET_ERROR || received == 0) { return 0; }
  return (U64)received;
}
