internal PLAN_Node*
plan_node_make(Arena* arena, PLAN_NodeType type)
{
  PLAN_Node* node = push_array(arena, PLAN_Node, 1);
  node->type = type;
  return node;
}

internal B32
plan_ir_contains_aggregate(IR_Node* node)
{
  for (IR_Node* n = node; n != NULL; n = n->next)
  {
    if (n->type == IR_NodeType_AggregateCall) return 1;
    if (plan_ir_contains_aggregate(n->first)) return 1;
  }
  return 0;
}

internal PLAN_Node*
plan_build_from_select(Arena* arena, GDB_Database* database, IR_Node* select_ir_node)
{
  if (!select_ir_node) return NULL;
  
  //- tec: FROM - primary table, any comma-joined tables, and any JOIN clauses, folded into
  // a left-deep tree in the order they appear (e.g. "FROM a, b JOIN c" -> Join(Join(a,b), c)).
  PLAN_Node* from_plan = NULL;
  
  for (IR_Node* child = select_ir_node->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_Table)
    {
      PLAN_Node* scan = plan_node_make(arena, PLAN_NodeType_Scan);
      scan->value = child->value;
      scan->table = gdb_database_find_table(database, child->value);
      
      if (!from_plan)
      {
        from_plan = scan;
      }
      else
      {
        PLAN_Node* join = plan_node_make(arena, PLAN_NodeType_Join);
        join->value = str8_lit("cross");
        join->input = from_plan;
        join->input2 = scan;
        from_plan = join;
      }
    }
    else if (child->type == IR_NodeType_Join)
    {
      // tec: sql_parse_join_clause always builds this as exactly two children - the joined table first, the ON condition expression last
      IR_Node* joined_table_ir = child->first;
      IR_Node* join_condition_ir = child->last;
      
      PLAN_Node* scan = plan_node_make(arena, PLAN_NodeType_Scan);
      scan->value = joined_table_ir->value;
      scan->table = gdb_database_find_table(database, joined_table_ir->value);
      
      PLAN_Node* join = plan_node_make(arena, PLAN_NodeType_Join);
      join->value = child->value; // "inner" / "left"
      join->input = from_plan;
      join->input2 = scan;
      join->condition = join_condition_ir;
      from_plan = join;
    }
  }
  
  PLAN_Node* plan = from_plan;
  
  //- tec: WHERE
  IR_Node* where_ir = ir_node_find_child(select_ir_node, IR_NodeType_Where);
  if (where_ir)
  {
    PLAN_Node* filter = plan_node_make(arena, PLAN_NodeType_Filter);
    filter->input = plan;
    filter->condition = where_ir;
    plan = filter;
  }
  
  //- tec: GROUP BY / aggregates - needed whenever there's an explicit GROUP BY, or an
  // aggregate call anywhere in the select list or HAVING (ie "SELECT COUNT(*) FROM t"
  // has no GROUP BY but still aggregates into a single group).
  IR_Node* column_list_ir = ir_node_find_child(select_ir_node, IR_NodeType_ColumnList);
  IR_Node* group_by_ir = ir_node_find_child(select_ir_node, IR_NodeType_GroupBy);
  IR_Node* having_ir = ir_node_find_child(select_ir_node, IR_NodeType_Having);
  
  B32 needs_aggregate = (group_by_ir != NULL) ||
    plan_ir_contains_aggregate(column_list_ir ? column_list_ir->first : NULL) ||
    plan_ir_contains_aggregate(having_ir ? having_ir->first : NULL);
  
  if (needs_aggregate)
  {
    PLAN_Node* aggregate = plan_node_make(arena, PLAN_NodeType_Aggregate);
    aggregate->input = plan;
    aggregate->group_by = group_by_ir;
    aggregate->column_list = column_list_ir;
    plan = aggregate;
  }
  
  if (having_ir)
  {
    PLAN_Node* having = plan_node_make(arena, PLAN_NodeType_Having);
    having->input = plan;
    having->condition = having_ir;
    plan = having;
  }
  
  //- tec: SELECT list projection
  {
    PLAN_Node* project = plan_node_make(arena, PLAN_NodeType_Project);
    project->input = plan;
    project->column_list = column_list_ir;
    plan = project;
  }
  
  //- tec: ORDER BY
  IR_Node* order_by_ir = ir_node_find_child(select_ir_node, IR_NodeType_OrderBy);
  if (order_by_ir)
  {
    PLAN_Node* sort = plan_node_make(arena, PLAN_NodeType_Sort);
    sort->input = plan;
    sort->order_by = order_by_ir;
    plan = sort;
  }
  
  //- tec: LIMIT / OFFSET
  IR_Node* limit_ir = ir_node_find_child(select_ir_node, IR_NodeType_Limit);
  IR_Node* offset_ir = ir_node_find_child(select_ir_node, IR_NodeType_Offset);
  if (limit_ir || offset_ir)
  {
    PLAN_Node* limit = plan_node_make(arena, PLAN_NodeType_Limit);
    limit->input = plan;
    limit->limit_node = limit_ir;
    limit->offset_node = offset_ir;
    plan = limit;
  }
  
  return plan;
}

internal PLAN_ExecResult
plan_execute(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node)
{
  PLAN_ExecResult result = {0};
  if (!plan) return result;
  
  switch (plan->type)
  {
    case PLAN_NodeType_Scan:
    {
      if (!plan->table)
      {
        log_error("plan_execute: table '%.*s' not found", str8_varg(plan->value));
        break;
      }
      
      // tec: todays single scan_filter kernel does the scan and (if present) the WHERE
      // filter in one GPU dispatch, re-deriving the Where IR node from select_ir_node itself
      // that's why this is reached both for a bare Scan and for Filter-over-Scan below
      QE_ScanResult scan_result = qe_scan_filter(arena, database, plan->table, select_ir_node);
      result.indices = scan_result.indices;
      result.count = scan_result.count;
      result.supported = 1;
    } break;
    
    case PLAN_NodeType_Filter:
    {
      if (plan->input && plan->input->type == PLAN_NodeType_Scan)
      {
        result = plan_execute(arena, database, plan->input, select_ir_node);
      }
      else
      {
        log_error("plan_execute: filtering over a join has no kernel yet (task #6) - query cannot execute");
      }
    } break;
    
    case PLAN_NodeType_Join:
    case PLAN_NodeType_Aggregate:
    case PLAN_NodeType_Having:
    {
      String8 type_name = plan_node_type_to_string(plan->type);
      log_error("plan_execute: '%.*s' has no GPU kernel yet (task #6) - query cannot execute",
                str8_varg(type_name));
    } break;
    
    case PLAN_NodeType_Project:
    {
      // tec: no projection kernel yet - application.c's Select case already does its own
      // column selection on the CPU from the matched row indices, so just pass rows through.
      result = plan_execute(arena, database, plan->input, select_ir_node);
    } break;
    
    case PLAN_NodeType_Sort:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node);
      if (result.supported)
      {
        log_error("plan_execute: 'order by' has no sort kernel yet (task #6) - returning unsorted rows");
      }
    } break;
    
    case PLAN_NodeType_Limit:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node);
      if (result.supported)
      {
        U64 offset = plan->offset_node ? u64_from_str8(plan->offset_node->value, 10) : 0;
        U64 limit = plan->limit_node ? u64_from_str8(plan->limit_node->value, 10) : max_U64;
        
        if (offset >= result.count)
        {
          result.indices = NULL;
          result.count = 0;
        }
        else
        {
          result.indices = result.indices + offset;
          result.count = Min(result.count - offset, limit);
        }
      }
    } break;
  }
  
  return result;
}

internal String8
plan_node_type_to_string(PLAN_NodeType type)
{
  String8 result = str8_lit("PLAN_NodeType_<unknown>");
  switch (type)
  {
    case PLAN_NodeType_Scan: result = str8_lit("PLAN_NodeType_Scan"); break;
    case PLAN_NodeType_Filter: result = str8_lit("PLAN_NodeType_Filter"); break;
    case PLAN_NodeType_Join: result = str8_lit("PLAN_NodeType_Join"); break;
    case PLAN_NodeType_Aggregate: result = str8_lit("PLAN_NodeType_Aggregate"); break;
    case PLAN_NodeType_Having: result = str8_lit("PLAN_NodeType_Having"); break;
    case PLAN_NodeType_Project: result = str8_lit("PLAN_NodeType_Project"); break;
    case PLAN_NodeType_Sort: result = str8_lit("PLAN_NodeType_Sort"); break;
    case PLAN_NodeType_Limit: result = str8_lit("PLAN_NodeType_Limit"); break;
  }
  return result;
}

internal void
plan_print(PLAN_Node* plan, U64 depth)
{
  if (!plan) return;
  
  for (U64 i = 0; i < depth; i++) printf("  ");
  String8 type_name = plan_node_type_to_string(plan->type);
  
  if (plan->type == PLAN_NodeType_Scan)
  {
    printf("- [%.*s] table='%.*s' (%s)\n", str8_varg(type_name), str8_varg(plan->value),
           plan->table ? "resolved" : "NOT FOUND");
  }
  else if (plan->type == PLAN_NodeType_Join)
  {
    printf("- [%.*s] type='%.*s'\n", str8_varg(type_name), str8_varg(plan->value));
  }
  else
  {
    printf("- [%.*s]\n", str8_varg(type_name));
  }
  
  if (plan->input) plan_print(plan->input, depth + 1);
  if (plan->input2) plan_print(plan->input2, depth + 1);
}
