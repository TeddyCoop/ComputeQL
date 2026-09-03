/* date = February 9th 2025 8:43 pm */

#ifndef APPLICATION_H
#define APPLICATION_H

typedef struct APP_QueryResult APP_QueryResult;
struct APP_QueryResult
{
  B32 had_parse_error;
  String8 output_text;
};

internal APP_QueryResult app_execute_query_capture(Arena* arena, String8 sql_query, GDB_Database** io_database);
internal void app_execute_query(String8 sql_query);
internal PLAN_ExecResult app_perform_kernel(Arena* arena, GDB_Database* database, IR_Node* root_node);

#endif //APPLICATION_H
