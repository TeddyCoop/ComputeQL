#ifndef CLIENT_H
#define CLIENT_H

internal void client_run_one_shot(String8 host, U16 port, String8 sql_query);
internal void client_run_interactive(String8 host, U16 port);

//~ tec: query helpers
internal B32 client_send_query_and_print(OS_Handle conn, String8 sql_query);
internal B32 client_line_is_quit_command(String8 line);

#endif //CLIENT_H
