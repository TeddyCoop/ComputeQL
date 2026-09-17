#ifndef PLANNER_H
#define PLANNER_H

//~ tec: PLAN_NodeType/PLAN_Node now live in planner/plan_node.h (included before this file, and
// before query_exec.h - see main.c) so query_exec.h's QE_NodeTrace can use them directly instead
// of a forward declaration/U32 stand-in.

internal PLAN_Node* plan_node_make(Arena* arena, PLAN_NodeType type);
internal PLAN_Node* plan_build_from_select(Arena* arena, GDB_Database* database, IR_Node* select_ir_node);
internal B32 plan_ir_contains_aggregate(IR_Node* node);

internal PLAN_ExecResult plan_execute(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node, QE_TraceCtx* trace);

internal String8 plan_node_type_to_string(PLAN_NodeType type);
internal void plan_print(Arena* arena, String8List* out, PLAN_Node* plan, U64 depth);
internal void plan_print_analyzed(Arena* arena, String8List* out, PLAN_Node* plan, QE_TraceCtx* trace, U64 depth);

#endif //PLANNER_H
