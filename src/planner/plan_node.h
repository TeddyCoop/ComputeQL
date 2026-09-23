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
  PLAN_NodeType_Window,
  
  // tec: Sort and Limit in one node, only the first offset + limit rows are ordered
  PLAN_NodeType_TopN, 
  
  // tec: keeps the left rows that have a match on the right, the right side never reaches the output
  PLAN_NodeType_SemiJoin, 
  
  // tec: keeps the left rows that have no match on the right
  PLAN_NodeType_AntiJoin, 
} PLAN_NodeType;

// tec: how a Scan, or a Filter over a Scan, produces its rows, Auto leaves it to the executor's fixed cascade
typedef enum PLAN_ScanStrategy
{
  PLAN_ScanStrategy_Auto,
  PLAN_ScanStrategy_Identity,
  PLAN_ScanStrategy_Index,
  PLAN_ScanStrategy_Cpu,
  PLAN_ScanStrategy_Gpu,
} PLAN_ScanStrategy;

// tec: how a Sort or TopN gets its order, Auto leaves it to the executor
typedef enum PLAN_SortStrategy
{
  PLAN_SortStrategy_Auto,
  
  // tec: the input already comes out in this order, an index scan on the sort column
  PLAN_SortStrategy_Presorted, 
  
  // tec: keep the first K rows in a heap while reading the input
  PLAN_SortStrategy_Heap,      
  
  // tec: read the index in order and stop after K rows, the input is never executed
  PLAN_SortStrategy_IndexWalk, 
} PLAN_SortStrategy;

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
  IR_Node* column_list; // tec: Aggregate/Project/Window - the select list (ColumnList IR node: columns/aliases/aggregate calls)
  IR_Node* order_by;    // tec: Sort/TopN only - OrderBy IR node
  IR_Node* limit_node;  // tec: Limit/TopN only - Limit IR node, NULL if no LIMIT was given
  IR_Node* offset_node; // tec: Limit/TopN only - Offset IR node, NULL if no OFFSET was given
  
  B32 has_estimate;     // tec: est_rows is only meaningful once the optimizer annotated the plan
  F64 est_rows;         // tec: estimated rows this node produces
  
  // tec: chosen by the optimizer's cost model, on the Filter for a filtered scan and on the Scan for a bare one
  PLAN_ScanStrategy scan_strategy;
  IR_Node* index_leaf;  // tec: the comparison an Index strategy narrows by
  F64 est_cost_us;      // tec: estimated microseconds for the chosen strategy
  String8 scan_reason;
  
  F64 est_hash_rows;    // tec: Join only - rows the hash join emits before residual ON conditions, which is what its output buffer holds
  F64 est_round_trips;  // tec: GPU submits this node is expected to cost, the cost model prefers plans that need fewer
  
  PLAN_SortStrategy sort_strategy;
  GDB_Index* order_index;   // tec: Presorted and IndexWalk - the index the order comes from
  B32 order_descending;
  String8 sort_reason;
};

#endif //PLAN_NODE_H
