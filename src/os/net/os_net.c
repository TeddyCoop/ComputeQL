internal B32
os_net_send_exact(OS_Handle socket, void *data, U64 size)
{
  U8 *cursor = (U8*)data;
  U64 remaining = size;
  while(remaining > 0)
  {
    U64 sent = os_net_send(socket, cursor, remaining);
    if(sent == 0) { return 0; }
    cursor += sent;
    remaining -= sent;
  }
  return 1;
}

internal B32
os_net_recv_exact(OS_Handle socket, void *out_data, U64 size)
{
  U8 *cursor = (U8*)out_data;
  U64 remaining = size;
  while(remaining > 0)
  {
    U64 received = os_net_recv(socket, cursor, remaining);
    if(received == 0) { return 0; }
    cursor += received;
    remaining -= received;
  }
  return 1;
}
