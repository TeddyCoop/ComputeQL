#ifndef CLIENT_H
#define CLIENT_H

internal void client_run_one_shot(String8 host, U16 port, String8 sql_query);
internal void client_run_interactive(String8 host, U16 port);

#endif //CLIENT_H
