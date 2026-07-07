/* date = February 9th 2025 8:43 pm */

#ifndef APPLICATION_H
#define APPLICATION_H

typedef struct APP_KernelResult APP_KernelResult;
struct APP_KernelResult
{
  U64* indices;
  U64 count;
  U64 cap;
};

internal void app_execute_query(String8 sql_query);
internal APP_KernelResult app_perform_kernel(Arena* arena, GDB_Database* database, IR_Node* root_node);

#endif //APPLICATION_H
