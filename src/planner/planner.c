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

internal String8
plan_alias_from_table_ir(IR_Node* table_ir)
{
  IR_Node* alias_ir = ir_node_find_child(table_ir, IR_NodeType_Alias);
  return alias_ir ? alias_ir->value : (String8){0};
}

internal PLAN_Node*
plan_build_from_select(Arena* arena, GDB_Database* database, IR_Node* select_ir_node)
{
  if (!select_ir_node) return NULL;
  
  //- tec: FROM - primary table, any comma-joined tables, and any JOIN clauses, folded into
  // a left-deep tree in the order they appear (e.g. "FROM a, b JOIN c" -> Join(Join(a,b), c))
  PLAN_Node* from_plan = NULL;
  
  for (IR_Node* child = select_ir_node->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_Table)
    {
      PLAN_Node* scan = plan_node_make(arena, PLAN_NodeType_Scan);
      scan->value = child->value;
      scan->table = gdb_database_find_table(database, child->value);
      scan->alias = plan_alias_from_table_ir(child);

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
      // tec: sql_parse_join_clause always builds this as two children: the joined table first, the ON condition expression last
      IR_Node* joined_table_ir = child->first;
      IR_Node* join_condition_ir = child->last;
      
      PLAN_Node* scan = plan_node_make(arena, PLAN_NodeType_Scan);
      scan->value = joined_table_ir->value;
      scan->table = gdb_database_find_table(database, joined_table_ir->value);
      scan->alias = plan_alias_from_table_ir(joined_table_ir);

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
  // has no GROUP BY but still aggregates into a single group)
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

internal PLAN_ExecResult plan_execute_join(Arena* arena, GDB_Database* database, PLAN_Node* join_plan, IR_Node* select_ir_node, IR_Node* residual_where_root);

internal PLAN_ExecResult
plan_wrap_scan_result(Arena* arena, GDB_Table* table, String8 alias, QE_ScanResult scan_result)
{
  PLAN_ExecResult result = {0};
  result.rows.tables = push_array(arena, GDB_Table*, 1);
  result.rows.tables[0] = table;
  result.rows.aliases = push_array(arena, String8, 1);
  result.rows.aliases[0] = alias;
  result.rows.table_count = 1;
  result.rows.row_indices = push_array(arena, U64*, 1);
  result.rows.row_indices[0] = scan_result.indices;
  result.rows.count = scan_result.count;
  result.supported = 1;
  return result;
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
      
      // tec: a bare Scan is always unfiltered (no where_clause) - it's reached both directly
      // (no WHERE at all) and as a Join's raw input, where it must return ALL of its rows
      // regardless of what the query's WHERE says about *other* tables. WHERE is applied
      // explicitly by the Filter case below (Filter-over-Scan) or as a post-join residual filter
      QE_ScanResult scan_result = qe_scan_filter(arena, database, plan->table, NULL);
      result = plan_wrap_scan_result(arena, plan->table, plan->alias, scan_result);
    } break;

    case PLAN_NodeType_Filter:
    {
      if (plan->input && plan->input->type == PLAN_NodeType_Scan)
      {
        // tec: try an index lookup first
        QE_ScanResult scan_result = {0};
        if (!qe_try_index_scan(arena, plan->input->table, plan->condition, &scan_result))
        {
          scan_result = qe_scan_filter(arena, database, plan->input->table, plan->condition);
        }
        result = plan_wrap_scan_result(arena, plan->input->table, plan->input->alias, scan_result);
      }
      else if (plan->input && plan->input->type == PLAN_NodeType_Join)
      {
        // tec: plan->condition is the Where IR node itself - ->first is its actual condition
        // tree root (same convention as Having, see qe_apply_having)
        IR_Node* where_root = plan->condition ? plan->condition->first : NULL;
        result = plan_execute_join(arena, database, plan->input, select_ir_node, where_root);
      }
      else
      {
        log_error("plan_execute: filtering over this input has no kernel yet (task #6) - query cannot execute");
      }
    } break;
    
    case PLAN_NodeType_Join:
    {
      result = plan_execute_join(arena, database, plan, select_ir_node, NULL);
    } break;
    
    case PLAN_NodeType_Aggregate:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node);
      if (result.supported)
      {
        if (result.is_materialized)
        {
          log_error("plan_execute: nested aggregation is not supported");
          result.supported = 0;
        }
        else
        {
          IR_Node* having_ir = ir_node_find_child(select_ir_node, IR_NodeType_Having);
          PLAN_Materialized materialized = qe_aggregate(arena, database, &result.rows, plan->group_by, plan->column_list, having_ir);
          
          result.rows = (PLAN_RowSet){0};
          result.is_materialized = 1;
          result.materialized = materialized;
        }
      }
    } break;
    
    case PLAN_NodeType_Having:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node);
      if (result.supported)
      {
        if (!result.is_materialized)
        {
          log_error("plan_execute: HAVING without GROUP BY/aggregates in scope is not supported");
          result.supported = 0;
        }
        else
        {
          result.materialized = qe_apply_having(arena, &result.materialized, plan->condition);
        }
      }
    } break;
    
    case PLAN_NodeType_Project:
    {
      // tec: no projection kernel yet
      result = plan_execute(arena, database, plan->input, select_ir_node);
    } break;
    
    case PLAN_NodeType_Sort:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node);
      if (result.supported)
      {
        if (result.is_materialized)
        {
          result.materialized = qe_sort_materialized(arena, &result.materialized, plan->order_by);
        }
        else
        {
          result.rows = qe_sort_rows(arena, &result.rows, plan->order_by);
        }
      }
    } break;
    
    case PLAN_NodeType_Limit:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node);
      if (result.supported)
      {
        U64 offset = plan->offset_node ? u64_from_str8(plan->offset_node->value, 10) : 0;
        U64 limit = plan->limit_node ? u64_from_str8(plan->limit_node->value, 10) : max_U64;
        
        if (result.is_materialized)
        {
          PLAN_Materialized* m = &result.materialized;
          if (offset >= m->count)
          {
            m->count = 0;
          }
          else
          {
            U64 new_count = Min(m->count - offset, limit);
            for (U64 c = 0; c < m->column_count; c++)
            {
              PLAN_AggColumn* col = &m->columns[c];
              if (col->numeric_values) col->numeric_values = col->numeric_values + offset;
              if (col->string_values) col->string_values = col->string_values + offset;
            }
            m->count = new_count;
          }
        }
        else
        {
          PLAN_RowSet* rs = &result.rows;
          if (offset >= rs->count)
          {
            rs->count = 0;
          }
          else
          {
            U64 new_count = Min(rs->count - offset, limit);
            for (U64 t = 0; t < rs->table_count; t++)
            {
              rs->row_indices[t] = rs->row_indices[t] + offset;
            }
            rs->count = new_count;
          }
        }
      }
    } break;
  }
  
  return result;
}

internal PLAN_ExecResult
plan_execute_join(Arena* arena, GDB_Database* database, PLAN_Node* join_plan, IR_Node* select_ir_node, IR_Node* residual_where_root)
{
  PLAN_ExecResult result = {0};
  
  PLAN_ExecResult left_result = plan_execute(arena, database, join_plan->input, select_ir_node);
  if (!left_result.supported || left_result.is_materialized)
  {
    log_error("plan_execute: join's left-hand input has no usable row set");
    return result;
  }
  
  GDB_Table* right_table = join_plan->input2 ? join_plan->input2->table : NULL;
  if (!right_table)
  {
    log_error("plan_execute: join's right-hand side must resolve to a real table");
    return result;
  }
  
  String8 right_alias = join_plan->input2 ? join_plan->input2->alias : (String8){0};
  String8 join_type = join_plan->value;
  IR_Node* equi_condition = NULL;
  B32 is_cross = str8_match(join_plan->value, str8_lit("cross"), 0);

  if (!is_cross)
  {
    equi_condition = qe_find_equi_condition(&left_result.rows, right_table, right_alias, join_plan->condition);
  }
  else
  {
    join_type = str8_lit("inner"); // tec: a comma-join's real predicate lives in WHERE, not here
    equi_condition = residual_where_root ? qe_find_equi_condition(&left_result.rows, right_table, right_alias, residual_where_root) : NULL;
  }

  if (!equi_condition)
  {
    log_error("plan_execute: only equi-joins are supported (no usable 'a = b' clause found between the joined tables)");
    return result;
  }

  PLAN_RowSet joined_rows = qe_hash_join(arena, &left_result.rows, right_table, right_alias, join_type, equi_condition);
  
  result.rows = residual_where_root ? qe_filter_joined_rows(arena, &joined_rows, residual_where_root) : joined_rows;
  result.supported = 1;
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
