#ifndef PLAN_NODE_H
#define PLAN_NODE_H

//~ tec: logical/physical query plan node
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
  String8 alias;     // tec: Scan only - the "AS x" alias from FROM/JOIN, or {0} if unaliased
  
  // tec: these all point back into the IR tree rather than owning a copy
  IR_Node* condition;   // tec: Filter = the Where IR node, Having = the Having IR node, Join = the ON condition expression
  IR_Node* group_by;    // tec: Aggregate only - GroupBy IR node, NULL means the whole input is a single group
  IR_Node* column_list; // tec: Aggregate/Project - the select list (ColumnList IR node: columns/aliases/aggregate calls)
  IR_Node* order_by;    // tec: Sort only - OrderBy IR node
  IR_Node* limit_node;  // tec: Limit only - Limit IR node, NULL if no LIMIT was given
  IR_Node* offset_node; // tec: Limit only - Offset IR node, NULL if no OFFSET was given
};

#endif //PLAN_NODE_H
