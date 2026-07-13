#ifndef PLANNER_H
#define PLANNER_H

//~ tec: logical/physical query plan.
//
// There's no cost-based optimization yet (no join ordering, no predicate pushdown), so
// there's just one plan tree today rather than a separate logical-plan/physical-plan pair -
// plan_build_from_select() lowers an IR_NodeType_Select node straight into this tree in
// canonical SQL evaluation order (FROM/JOIN -> WHERE -> GROUP BY -> HAVING -> SELECT list ->
// ORDER BY -> LIMIT/OFFSET), and plan_execute() walks it directly.
//
// Scan/Filter/Join/Sort/Limit-over-rows all produce a PLAN_RowSet (row indices into one or
// more base tables). Aggregate/Having produce a PLAN_Materialized instead - their output rows
// aren't indices into anything, they're freshly computed values. PLAN_ExecResult carries
// whichever one is valid, tagged by is_materialized.

typedef enum PLAN_NodeType
{
  PLAN_NodeType_Scan,
  PLAN_NodeType_Filter,
  PLAN_NodeType_Join,
  PLAN_NodeType_Aggregate,
  PLAN_NodeType_Having,
  PLAN_NodeType_Project,
  PLAN_NodeType_Sort,
  PLAN_NodeType_Limit,
} PLAN_NodeType;

typedef struct PLAN_Node PLAN_Node;
struct PLAN_Node
{
  PLAN_NodeType type;
  
  PLAN_Node* input;  // tec: primary input plan (every type but Scan has one)
  PLAN_Node* input2; // tec: Join only - right-hand side input plan
  
  String8 value;     // tec: Scan = table name, Join = join type ("inner"/"left"/"cross")
  GDB_Table* table;  // tec: Scan only - resolved at plan-build time, NULL if the table lookup failed
  
  // tec: these all point back into the IR tree rather than owning a copy - the IR outlives
  // the plan (same arena, and the plan is only ever built and consumed within one query).
  IR_Node* condition;   // tec: Filter = the Where IR node, Having = the Having IR node, Join = the ON condition expression
  IR_Node* group_by;    // tec: Aggregate only - GroupBy IR node, NULL means the whole input is a single group
  IR_Node* column_list; // tec: Aggregate/Project - the select list (ColumnList IR node: columns/aliases/aggregate calls)
  IR_Node* order_by;    // tec: Sort only - OrderBy IR node
  IR_Node* limit_node;  // tec: Limit only - Limit IR node, NULL if no LIMIT was given
  IR_Node* offset_node; // tec: Limit only - Offset IR node, NULL if no OFFSET was given
};

internal PLAN_Node* plan_node_make(Arena* arena, PLAN_NodeType type);
internal PLAN_Node* plan_build_from_select(Arena* arena, GDB_Database* database, IR_Node* select_ir_node);
internal B32 plan_ir_contains_aggregate(IR_Node* node);

internal PLAN_ExecResult plan_execute(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node);

internal String8 plan_node_type_to_string(PLAN_NodeType type);
internal void plan_print(PLAN_Node* plan, U64 depth);

#endif //PLANNER_H
