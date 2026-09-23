internal PLAN_Node*
plan_node_make(Arena* arena, PLAN_NodeType type)
{
  PLAN_Node* node = push_array(arena, PLAN_Node, 1);
  node->type = type;
  return node;
}

internal B32
plan_ir_is_window_call(IR_Node* node)
{
  return node->type == IR_NodeType_AggregateCall && ir_node_find_child(node, IR_NodeType_Window) != NULL;
}

internal B32
plan_ir_contains_aggregate(IR_Node* node)
{
  for (IR_Node* n = node; n != NULL; n = n->next)
  {
    if ((n->type == IR_NodeType_AggregateCall && !qe_ir_is_fuzzy_call(n, 0) && !plan_ir_is_window_call(n)) ||
        plan_ir_contains_aggregate(n->first)) 
    {
      return 1;
    }
  }
  return 0;
}

internal String8
plan_alias_from_table_ir(IR_Node* table_ir)
{
  IR_Node* alias_ir = ir_node_find_child(table_ir, IR_NodeType_Alias);
  return alias_ir ? alias_ir->value : (String8){0};
}

// tec: children of the IR node are the outer key, the inner key, the inner table and an optional filter on it
internal PLAN_Node*
plan_add_semi_join(Arena* arena, GDB_Database* database, PLAN_Node* input, IR_Node* semi_ir)
{
  IR_Node* outer_key = semi_ir->first;
  IR_Node* inner_key = outer_key->next;
  IR_Node* table_ir = inner_key->next;
  IR_Node* filter_ir = table_ir->next;

  PLAN_Node* scan = plan_node_make(arena, PLAN_NodeType_Scan);
  scan->value = table_ir->value;
  scan->table = gdb_database_find_table_or_catalog(database, table_ir->value);
  scan->alias = plan_alias_from_table_ir(table_ir);

  PLAN_Node* right = scan;
  if (filter_ir)
  {
    PLAN_Node* filter = plan_node_make(arena, PLAN_NodeType_Filter);
    filter->input = scan;
    filter->condition = filter_ir;
    right = filter;
  }

  B32 is_anti = str8_match(semi_ir->value, str8_lit("anti"), 0);
  PLAN_Node* join = plan_node_make(arena, is_anti ? PLAN_NodeType_AntiJoin : PLAN_NodeType_SemiJoin);
  join->value = semi_ir->value;
  join->input = input;
  join->input2 = right;

  IR_Node* equality = ir_node_make(arena, IR_NodeType_Operator, str8_lit("="));
  ir_node_add_child(equality, optimizer_ir_clone(arena, outer_key));
  ir_node_add_child(equality, optimizer_ir_clone(arena, inner_key));
  join->condition = equality;
  return join;
}

internal B32
plan_node_is_join(PLAN_Node* plan)
{
  B32 is_join = plan->type == PLAN_NodeType_Join;
  B32 is_semi = plan->type == PLAN_NodeType_SemiJoin;
  B32 is_anti = plan->type == PLAN_NodeType_AntiJoin;
  return is_join || is_semi || is_anti;
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
      scan->table = gdb_database_find_table_or_catalog(database, child->value);
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
      scan->table = gdb_database_find_table_or_catalog(database, joined_table_ir->value);
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
  
  //- tec: WHERE, with predicates pushed toward the scans when the optimizer can place them
  IR_Node* where_ir = ir_node_find_child(select_ir_node, IR_NodeType_Where);
  
  // tec: a scan over one table binds bare column names, so 'alias.col' in its WHERE has to become 'col'
  if (where_ir && from_plan && from_plan->type == PLAN_NodeType_Scan)
  {
    optimizer_ir_strip_column_qualifiers(where_ir);
  }
  
  PLAN_Node* rewritten_plan = NULL;
  if (from_plan && optimizer_enabled())
  {
    rewritten_plan = optimizer_rewrite_from_where(arena, from_plan, where_ir, select_ir_node);
  }
  
  if (rewritten_plan)
  {
    plan = rewritten_plan;
  }
  else if (where_ir)
  {
    PLAN_Node* filter = plan_node_make(arena, PLAN_NodeType_Filter);
    filter->input = plan;
    filter->condition = where_ir;
    plan = filter;
  }
  
  //- tec: semi and anti joins from IN subqueries and EXISTS, they only filter the rows the FROM and WHERE produced
  for (IR_Node* child = select_ir_node->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_SemiJoin && plan)
    {
      plan = plan_add_semi_join(arena, database, plan, child);
    }
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
  
  //- tec: with an aggregate in the same select, Window ends up above Aggregate and refuses at execute time
  B32 has_window = 0;
  for (IR_Node* item = column_list_ir ? column_list_ir->first : NULL; 
       item != NULL; 
       item = item->next)
  {
    if (plan_ir_is_window_call(item)) has_window = 1;
  }
  if (has_window)
  {
    PLAN_Node* window = plan_node_make(arena, PLAN_NodeType_Window);
    window->input = plan;
    window->column_list = column_list_ir;
    window->order_by = ir_node_find_child(select_ir_node, IR_NodeType_OrderBy);
    plan = window;
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

  if (optimizer_scan_costing_enabled())
  {
    optimizer_choose_scan_strategies(arena, plan);
  }

  if (optimizer_sizing_enabled() && optimizer_plan_uses_estimates(plan))
  {
    optimizer_annotate_plan(arena, plan);
  }
  plan = optimizer_plan_ordering(arena, plan);

  return plan;
}

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
  result.rows.scores = scan_result.scores;
  result.rows.score_is_distance = scan_result.score_is_distance;
  result.rows.score_column_name = scan_result.score_column_name;
  result.rows.score_needle = scan_result.score_needle;
  result.supported = 1;
  return result;
}

internal U64
plan_result_row_count(PLAN_ExecResult* result)
{
  if (result->is_materialized)
  {
    return result->materialized.count;
  }
  return result->rows.count;
}

// tec: Filter over a scan records its own scan trace, every other node without an operator trace only reports rows here
internal B32
plan_node_records_rows_only(PLAN_Node* plan)
{
  switch (plan->type)
  {
    case PLAN_NodeType_Having:
    case PLAN_NodeType_Window:
    case PLAN_NodeType_Limit:
    case PLAN_NodeType_Project:
    {
      return 1;
    }
    
    case PLAN_NodeType_Filter:
    {
      B32 over_scan = plan->input && plan->input->type == PLAN_NodeType_Scan;
      return !over_scan;
    }
    
    default:
    {
      return 0;
    }
  }
}

internal PLAN_ExecResult
plan_execute(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node, QE_TraceCtx* trace)
{
  PLAN_ExecResult result = plan_execute_node(arena, database, plan, select_ir_node, trace);
  
  B32 wants_rows = trace && plan && result.supported && plan_node_records_rows_only(plan);
  if (wants_rows)
  {
    QE_NodeTrace* node_trace = qe_trace_record_begin(trace, plan, plan->type);
    node_trace->rows.rows_out = plan_result_row_count(&result);
  }
  return result;
}

internal PLAN_ExecResult
plan_execute_node(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node, QE_TraceCtx* trace)
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
      
      QE_NodeTrace* node_trace = qe_trace_record_begin(trace, plan, PLAN_NodeType_Scan);
      QE_ScanTrace* strace = node_trace ? &node_trace->scan : NULL;

      if (plan->scan_strategy == PLAN_ScanStrategy_Identity)
      {
        result = plan_execute_identity_scan(arena, plan, strace);
        break;
      }

      if (strace) strace->strategy = QE_TraceStrategy_GpuScan;
      
      // tec: a bare Scan is always unfiltered (no where_clause)
      QE_ScanResult scan_result = qe_scan_filter(arena, database, plan->table, NULL, strace);
      result = plan_wrap_scan_result(arena, plan->table, plan->alias, scan_result);
    } break;
    
    case PLAN_NodeType_Filter:
    {
      if (plan->input && plan->input->type == PLAN_NodeType_Scan && !plan->input->table)
      {
        log_error("plan_execute: table '%.*s' not found", str8_varg(plan->input->value));
      }
      else if (plan->input && plan->input->type == PLAN_NodeType_Scan)
      {
        QE_NodeTrace* node_trace = qe_trace_record_begin(trace, plan, PLAN_NodeType_Filter);
        QE_ScanTrace* strace = node_trace ? &node_trace->scan : NULL;
        
        // tec: try an index lookup first
        QE_ScanResult scan_result = {0};
        B32 impossible = optimizer_enabled() && plan->condition && optimizer_ir_is_constant_false(plan->condition->first);
        if (impossible)
        {
          scan_result.indices = push_array(arena, U64, 1);
          if (strace)
          {
            strace->rows_before = plan->input->table->row_count;
            strace->strategy_reason = str8_lit("condition is always false");
          }
        }
        else if (plan->scan_strategy == PLAN_ScanStrategy_Cpu)
        {
          if (strace)
          {
            strace->strategy = QE_TraceStrategy_CpuScan;
            strace->strategy_reason = plan->scan_reason;
          }
          scan_result = qe_cpu_scan_filter(arena, plan->input->table, plan->condition, strace);
        }
        else if (plan->scan_strategy == PLAN_ScanStrategy_Gpu)
        {
          if (strace)
          {
            strace->strategy = QE_TraceStrategy_GpuScan;
          }
          scan_result = qe_scan_filter(arena, database, plan->input->table, plan->condition, strace);
        }
        else if (plan->scan_strategy == PLAN_ScanStrategy_Index && qe_index_scan_with_leaf(arena, plan->input->table, plan->condition, plan->index_leaf, &scan_result))
        {
          if (strace)
          {
            strace->strategy = QE_TraceStrategy_IndexScan;
            strace->rows_before = plan->input->table->row_count;
            strace->rows_after = scan_result.count;
          }
        }
        else if (qe_try_index_scan(arena, plan->input->table, plan->condition, &scan_result))
        {
          // tec: index hit
          if (strace)
          {
            strace->strategy = QE_TraceStrategy_IndexScan;
            strace->rows_before = plan->input->table->row_count;
            strace->rows_after = scan_result.count;
          }
        }
        // tec: then a NULL-aware CPU scan if the table has any NULLs
        else if (gdb_table_may_have_nulls(plan->input->table))
        {
          if (strace)
          {
            strace->strategy = QE_TraceStrategy_CpuScan;
            strace->strategy_reason = str8_lit("table has NULLs");
          }
          scan_result = qe_cpu_scan_filter(arena, plan->input->table, plan->condition, strace);
        }
        // tec: and then fall back to the normal GPU scan
        else
        {
          if (strace) strace->strategy = QE_TraceStrategy_GpuScan;
          scan_result = qe_scan_filter(arena, database, plan->input->table, plan->condition, strace);
        }
        result = plan_wrap_scan_result(arena, plan->input->table, plan->input->alias, scan_result);
      }
      else if (plan->input && plan->input->type == PLAN_NodeType_Join)
      {
        // tec: plan->condition is the Where IR node itself - ->first is its actual condition
        // tree root (same convention as Having, see qe_apply_having)
        IR_Node* where_root = plan->condition ? plan->condition->first : NULL;
        result = plan_execute_join(arena, database, plan->input, select_ir_node, where_root, trace);
      }
      else
      {
        log_error("plan_execute: filtering over this input has no kernel yet (task #6) - query cannot execute");
      }
    } break;
    
    case PLAN_NodeType_Join:
    case PLAN_NodeType_SemiJoin:
    case PLAN_NodeType_AntiJoin:
    {
      result = plan_execute_join(arena, database, plan, select_ir_node, NULL, trace);
    } break;
    
    case PLAN_NodeType_Aggregate:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node, trace);
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
          QE_NodeTrace* node_trace = qe_trace_record_begin(trace, plan, PLAN_NodeType_Aggregate);
          QE_AggregateHints hints = {0};
          optimizer_aggregate_hints(plan, &hints);
          PLAN_Materialized materialized = qe_aggregate(arena, database, &result.rows, plan->group_by, plan->column_list, having_ir, &hints, node_trace ? &node_trace->aggregate : NULL);
          
          result.rows = (PLAN_RowSet){0};
          result.is_materialized = 1;
          result.materialized = materialized;
        }
      }
    } break;
    
    case PLAN_NodeType_Having:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node, trace);
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
      result = plan_execute(arena, database, plan->input, select_ir_node, trace);
    } break;
    
    case PLAN_NodeType_Window:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node, trace);
      if (result.supported)
      {
        if (result.is_materialized)
        {
          log_error("plan_execute: window functions can't be combined with GROUP BY or aggregates in the same select - compute the aggregate in a subquery ('from (select ...) t') and apply the window function over it");
          result.supported = 0;
        }
        else
        {
          PLAN_Materialized materialized = {0};
          if (plan_window_apply(arena, &result.rows, plan->column_list, plan->order_by, &materialized))
          {
            result.rows = (PLAN_RowSet){0};
            result.is_materialized = 1;
            result.materialized = materialized;
          }
          else
          {
            result.supported = 0;
          }
        }
      }
    } break;
    
    case PLAN_NodeType_Sort:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node, trace);
      if (result.supported && plan->sort_strategy == PLAN_SortStrategy_Presorted)
      {
        if (plan->order_descending && !result.is_materialized)
        {
          result.rows = plan_reverse_rowset(arena, &result.rows);
        }
      }
      else if (result.supported)
      {
        QE_SortHints hints = {0};
        optimizer_sort_hints(plan, &hints);
        if (result.is_materialized)
        {
          result.materialized = qe_sort_materialized(arena, &result.materialized, plan->order_by, &hints);
        }
        else
        {
          QE_NodeTrace* node_trace = qe_trace_record_begin(trace, plan, PLAN_NodeType_Sort);
          result.rows = qe_sort_rows(arena, &result.rows, plan->order_by, &hints, node_trace ? &node_trace->sort : NULL);
        }
      }
    } break;

    case PLAN_NodeType_Limit:
    {
      result = plan_execute(arena, database, plan->input, select_ir_node, trace);
      if (result.supported)
      {
        plan_apply_limit(&result, plan->offset_node, plan->limit_node);
      }
    } break;

    case PLAN_NodeType_TopN:
    {
      result = plan_execute_top_n(arena, database, plan, select_ir_node, trace);
    } break;
  }
  
  return result;
}

internal void
plan_apply_limit(PLAN_ExecResult* result, IR_Node* offset_node, IR_Node* limit_node)
{
  U64 offset = 0;
  if (offset_node)
  {
    offset = u64_from_str8(offset_node->value, 10);
  }
  U64 limit = max_U64;
  if (limit_node)
  {
    limit = u64_from_str8(limit_node->value, 10);
  }

  if (result->is_materialized)
  {
    PLAN_Materialized* m = &result->materialized;
    if (offset >= m->count)
    {
      m->count = 0;
      return;
    }

    U64 new_count = Min(m->count - offset, limit);
    for (U64 c = 0; c < m->column_count; c++)
    {
      PLAN_AggColumn* col = &m->columns[c];
      if (col->numeric_values)
      {
        col->numeric_values = col->numeric_values + offset;
      }
      if (col->string_values)
      {
        col->string_values = col->string_values + offset;
      }
      if (col->is_null)
      {
        col->is_null = col->is_null + offset;
      }
    }
    m->count = new_count;
    return;
  }

  PLAN_RowSet* rs = &result->rows;
  if (offset >= rs->count)
  {
    rs->count = 0;
    return;
  }

  U64 new_count = Min(rs->count - offset, limit);
  for (U64 t = 0; t < rs->table_count; t++)
  {
    rs->row_indices[t] = rs->row_indices[t] + offset;
  }
  rs->count = new_count;
}

internal PLAN_RowSet
plan_reverse_rowset(Arena* arena, PLAN_RowSet* rows)
{
  PLAN_RowSet reversed = *rows;
  reversed.row_indices = push_array(arena, U64*, rows->table_count);
  for (U64 table_index = 0; table_index < rows->table_count; table_index += 1)
  {
    reversed.row_indices[table_index] = push_array(arena, U64, Max(rows->count, (U64)1));
    for (U64 row = 0; row < rows->count; row += 1)
    {
      reversed.row_indices[table_index][row] = rows->row_indices[table_index][rows->count - 1 - row];
    }
  }

  if (rows->scores)
  {
    reversed.scores = push_array(arena, F64, Max(rows->count, (U64)1));
    for (U64 row = 0; row < rows->count; row += 1)
    {
      reversed.scores[row] = rows->scores[rows->count - 1 - row];
    }
  }
  return reversed;
}

// tec: an IndexWalk answers from the index alone, so its input plan is never executed
internal PLAN_ExecResult
plan_execute_top_n(Arena* arena, GDB_Database* database, PLAN_Node* plan, IR_Node* select_ir_node, QE_TraceCtx* trace)
{
  PLAN_ExecResult result = {0};

  QE_SortHints hints = {0};
  optimizer_sort_hints(plan, &hints);

  OPT_OrderSource source = {0};
  B32 walk = plan->sort_strategy == PLAN_SortStrategy_IndexWalk && plan->order_index && optimizer_find_order_source(plan->input, &source);
  if (walk)
  {
    QE_NodeTrace* node_trace = qe_trace_record_begin(trace, plan, PLAN_NodeType_TopN);
    QE_ScanResult walked = {0};
    qe_index_ordered_scan(arena, source.table, source.scan->alias, plan->order_index, source.condition, plan->order_descending, hints.top_k, &walked);
    if (node_trace)
    {
      node_trace->sort.row_count = walked.count;
      node_trace->sort.kept_rows = walked.count;
      node_trace->sort.used_cpu = 1;
    }
    result = plan_wrap_scan_result(arena, source.table, source.scan->alias, walked);
    plan_apply_limit(&result, plan->offset_node, plan->limit_node);
    return result;
  }

  result = plan_execute(arena, database, plan->input, select_ir_node, trace);
  if (!result.supported)
  {
    return result;
  }

  QE_NodeTrace* node_trace = qe_trace_record_begin(trace, plan, PLAN_NodeType_TopN);
  QE_SortTrace* sort_trace = node_trace ? &node_trace->sort : NULL;
  if (result.is_materialized)
  {
    U64 input_count = result.materialized.count;
    result.materialized = qe_sort_materialized(arena, &result.materialized, plan->order_by, &hints);
    if (sort_trace)
    {
      sort_trace->row_count = input_count;
      sort_trace->kept_rows = result.materialized.count;
      sort_trace->used_cpu = 1;
    }
  }
  else
  {
    result.rows = qe_sort_rows(arena, &result.rows, plan->order_by, &hints, sort_trace);
  }

  plan_apply_limit(&result, plan->offset_node, plan->limit_node);
  return result;
}

// tec: every row id in order, filled on the CPU because that is cheaper than a GPU pass that only lists them
internal PLAN_ExecResult
plan_execute_identity_scan(Arena* arena, PLAN_Node* scan, QE_ScanTrace* trace)
{
  U64 row_count = scan->table->row_count;

  QE_ScanResult scan_result = {0};
  scan_result.indices = push_array(arena, U64, Max(row_count, (U64)1));
  scan_result.count = row_count;
  for (U64 row = 0; row < row_count; row += 1)
  {
    scan_result.indices[row] = row;
  }

  if (trace)
  {
    trace->strategy = QE_TraceStrategy_Identity;
    trace->strategy_reason = scan->scan_reason;
    trace->rows_before = row_count;
    trace->rows_after = row_count;
  }
  return plan_wrap_scan_result(arena, scan->table, scan->alias, scan_result);
}

internal PLAN_RowSet
plan_make_empty_join_rowset(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias)
{
  PLAN_RowSet empty = {0};
  empty.table_count = left->table_count + 1;
  empty.tables = push_array(arena, GDB_Table*, empty.table_count);
  MemoryCopy(empty.tables, left->tables, left->table_count * sizeof(GDB_Table*));
  empty.tables[left->table_count] = right_table;
  
  empty.aliases = push_array(arena, String8, empty.table_count);
  if (left->aliases)
  {
    MemoryCopy(empty.aliases, left->aliases, left->table_count * sizeof(String8));
  }
  empty.aliases[left->table_count] = right_alias;
  
  empty.row_indices = push_array(arena, U64*, empty.table_count);
  for (U64 table_index = 0; table_index < empty.table_count; table_index += 1)
  {
    empty.row_indices[table_index] = push_array(arena, U64, 1);
  }
  empty.count = 0;
  return empty;
}

internal PLAN_ExecResult
plan_execute_join(Arena* arena, GDB_Database* database, PLAN_Node* join_plan, IR_Node* select_ir_node, IR_Node* residual_where_root, QE_TraceCtx* trace)
{
  PLAN_ExecResult result = {0};
  
  PLAN_ExecResult left_result = plan_execute(arena, database, join_plan->input, select_ir_node, trace);
  if (!left_result.supported || left_result.is_materialized)
  {
    log_error("plan_execute: join's left-hand input has no usable row set");
    return result;
  }
  
  PLAN_Node* right_plan = join_plan->input2;
  PLAN_Node* right_scan = right_plan;
  if (right_plan && right_plan->type == PLAN_NodeType_Filter)
  {
    right_scan = right_plan->input;
  }
  
  B32 right_is_table = right_scan && right_scan->type == PLAN_NodeType_Scan && right_scan->table;
  if (!right_is_table)
  {
    log_error("plan_execute: join's right-hand side must resolve to a real table");
    return result;
  }
  
  GDB_Table* right_table = right_scan->table;
  String8 right_alias = right_scan->alias;
  
  B32 is_filter_join = join_plan->type == PLAN_NodeType_SemiJoin || join_plan->type == PLAN_NodeType_AntiJoin;

  // tec: nothing on the left means nothing to join, for inner and left joins alike
  if (optimizer_enabled() && left_result.rows.count == 0)
  {
    result.rows = left_result.rows;
    if (!is_filter_join)
    {
      result.rows = plan_make_empty_join_rowset(arena, &left_result.rows, right_table, right_alias);
    }
    result.supported = 1;
    return result;
  }
  
  // tec: a NULL right_rows means the whole table, so a filtered right side that matched nothing needs a real pointer
  U64* right_rows = NULL;
  U64 right_count = 0;
  if (right_plan->type == PLAN_NodeType_Filter)
  {
    PLAN_ExecResult right_result = plan_execute(arena, database, right_plan, select_ir_node, trace);
    if (!right_result.supported || right_result.is_materialized || right_result.rows.table_count != 1)
    {
      log_error("plan_execute: join's filtered right-hand side has no usable row set");
      return result;
    }
    right_rows = right_result.rows.row_indices[0];
    right_count = right_result.rows.count;
    if (!right_rows)
    {
      right_rows = push_array(arena, U64, 1);
    }
    
    // tec: nothing on the right, so a semi join keeps nothing and an anti join keeps everything
    if (optimizer_enabled() && is_filter_join && right_count == 0)
    {
      result.rows = left_result.rows;
      if (join_plan->type == PLAN_NodeType_SemiJoin)
      {
        result.rows.count = 0;
      }
      result.supported = 1;
      return result;
    }

    B32 inner_join = !str8_match(join_plan->value, str8_lit("left"), StringMatchFlag_CaseInsensitive);
    if (optimizer_enabled() && inner_join && !is_filter_join && right_count == 0)
    {
      result.rows = plan_make_empty_join_rowset(arena, &left_result.rows, right_table, right_alias);
      result.supported = 1;
      return result;
    }
  }
  
  String8 join_type = join_plan->value;
  IR_Node* equi_condition = NULL;
  IR_Node* on_leaves[PLAN_JOIN_MAX_ON_CONDITIONS] = {0};
  U32 on_leaf_count = 0;
  B32 is_cross = str8_match(join_plan->value, str8_lit("cross"), 0);
  B32 is_left_join = str8_match(join_plan->value, str8_lit("left"), StringMatchFlag_CaseInsensitive);
  
  if (!is_cross)
  {
    equi_condition = qe_find_equi_condition(&left_result.rows, right_table, right_alias, join_plan->condition);
    on_leaf_count = qe_collect_and_leaves(join_plan->condition, on_leaves, 0, PLAN_JOIN_MAX_ON_CONDITIONS);
  }
  else
  {
    join_type = str8_lit("inner"); // tec: a comma-join's real predicate lives in WHERE, not here
    if (residual_where_root)
    {
      equi_condition = qe_find_equi_condition(&left_result.rows, right_table, right_alias, residual_where_root);
    }
  }
  
  if (!equi_condition)
  {
    log_error("plan_execute: only equi-joins are supported (no usable 'a = b' clause found between the joined tables)");
    return result;
  }
  
  IR_Node* residual_on_leaves[PLAN_JOIN_MAX_ON_CONDITIONS] = {0};
  U32 residual_on_count = 0;
  for (U32 leaf_index = 0; leaf_index < on_leaf_count; leaf_index += 1)
  {
    if (on_leaves[leaf_index] != equi_condition)
    {
      residual_on_leaves[residual_on_count] = on_leaves[leaf_index];
      residual_on_count += 1;
    }
  }
  
  // tec: a left join has to keep unmatched left rows, so it cannot post-filter its ON conditions
  if (is_left_join && residual_on_count > 0)
  {
    log_error("plan_execute: a LEFT JOIN supports one equality plus conditions on the joined table only, other ON conditions are not supported yet");
    return result;
  }
  
  QE_NodeTrace* node_trace = qe_trace_record_begin(trace, join_plan, PLAN_NodeType_Join);
  QE_JoinTrace* join_trace = node_trace ? &node_trace->join : NULL;
  QE_JoinHints join_hints = {0};
  optimizer_join_hints(join_plan, &join_hints);
  PLAN_RowSet joined_rows = qe_hash_join(arena, &left_result.rows, right_table, right_alias, right_rows, right_count, join_type, equi_condition, &join_hints, join_trace);
  
  for (U32 leaf_index = 0; leaf_index < residual_on_count; leaf_index += 1)
  {
    joined_rows = qe_filter_joined_rows(arena, &joined_rows, residual_on_leaves[leaf_index]);
  }
  
  if (residual_where_root)
  {
    result.rows = qe_filter_joined_rows(arena, &joined_rows, residual_where_root);
  }
  else
  {
    result.rows = joined_rows;
  }
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
    case PLAN_NodeType_Window: result = str8_lit("PLAN_NodeType_Window"); break;
    case PLAN_NodeType_TopN: result = str8_lit("PLAN_NodeType_TopN"); break;
    case PLAN_NodeType_SemiJoin: result = str8_lit("PLAN_NodeType_SemiJoin"); break;
    case PLAN_NodeType_AntiJoin: result = str8_lit("PLAN_NodeType_AntiJoin"); break;
  }
  return result;
}

internal String8
plan_format_rows(Arena* arena, F64 rows)
{
  B32 is_fractional = rows < 10.0 && rows != (F64)(U64)rows;
  if (is_fractional)
  {
    return push_str8f(arena, "%.1f", rows);
  }
  return push_str8f(arena, "%.0f", rows);
}

// tec: both sides are clamped to one row so an empty result does not divide by zero
internal F64
plan_q_error(F64 estimated, F64 actual)
{
  F64 estimate = Max(estimated, 1.0);
  F64 truth = Max(actual, 1.0);
  return Max(estimate / truth, truth / estimate);
}

internal String8
plan_estimate_suffix(Arena* arena, PLAN_Node* plan, B32 has_actual, U64 actual_rows)
{
  if (!plan->has_estimate)
  {
    return str8_lit("");
  }

  String8 estimate = plan_format_rows(arena, plan->est_rows);
  String8 trips = str8_lit("");
  if (plan->est_round_trips > 0.0)
  {
    trips = push_str8f(arena, " est_trips=%.0f", plan->est_round_trips);
  }
  if (!has_actual)
  {
    return push_str8f(arena, " est_rows=%.*s%.*s", str8_varg(estimate), str8_varg(trips));
  }

  F64 q_error = plan_q_error(plan->est_rows, (F64)actual_rows);
  return push_str8f(arena, " est_rows=%.*s q_error=%.2f%.*s", str8_varg(estimate), q_error, str8_varg(trips));
}

internal String8
plan_indent_string(Arena* arena, U64 depth)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List parts = {0};
  for (U64 level = 0; level < depth; level += 1)
  {
    str8_list_push(scratch.arena, &parts, str8_lit("  "));
  }
  String8 indent = str8_list_join(arena, &parts, 0);
  scratch_end(scratch);
  return indent;
}

internal String8
plan_scan_strategy_suffix(Arena* arena, PLAN_Node* plan)
{
  char* name = NULL;
  switch (plan->scan_strategy)
  {
    case PLAN_ScanStrategy_Identity: name = "identity"; break;
    case PLAN_ScanStrategy_Index: name = "index"; break;
    case PLAN_ScanStrategy_Cpu: name = "cpu"; break;
    case PLAN_ScanStrategy_Gpu: name = "gpu"; break;
    default: break;
  }
  if (!name)
  {
    return str8_lit("");
  }
  return push_str8f(arena, " strategy=%s (%.*s) cost=%.0fus", name, str8_varg(plan->scan_reason), plan->est_cost_us);
}

internal String8
plan_sort_strategy_suffix(Arena* arena, PLAN_Node* plan)
{
  char* name = NULL;
  switch (plan->sort_strategy)
  {
    case PLAN_SortStrategy_Presorted: name = "presorted"; break;
    case PLAN_SortStrategy_Heap: name = "heap"; break;
    case PLAN_SortStrategy_IndexWalk: name = "index walk"; break;
    default: break;
  }
  if (!name)
  {
    return str8_lit("");
  }
  return push_str8f(arena, " strategy=%s (%.*s)", name, str8_varg(plan->sort_reason));
}

internal void
plan_print_plain_line(Arena* arena, String8List* out, PLAN_Node* plan, String8 indent, String8 estimate_suffix)
{
  String8 strategy_suffix = plan_scan_strategy_suffix(arena, plan);
  String8 sort_suffix = plan_sort_strategy_suffix(arena, plan);
  estimate_suffix = push_str8f(arena, "%.*s%.*s%.*s", str8_varg(estimate_suffix), str8_varg(strategy_suffix), str8_varg(sort_suffix));
  String8 type_name = plan_node_type_to_string(plan->type);

  if (plan->type == PLAN_NodeType_Scan)
  {
    char* resolution = "resolved";
    if (!plan->table)
    {
      resolution = plan->has_estimate ? "derived" : "NOT FOUND";
    }
    str8_list_push(arena, out, push_str8f(arena, "%.*s- [%.*s] table='%.*s' (%s)%.*s\n", str8_varg(indent), str8_varg(type_name), str8_varg(plan->value),
                                          resolution, str8_varg(estimate_suffix)));
  }
  else if (plan_node_is_join(plan))
  {
    str8_list_push(arena, out, push_str8f(arena, "%.*s- [%.*s] type='%.*s'%.*s\n", str8_varg(indent), str8_varg(type_name), str8_varg(plan->value),
                                          str8_varg(estimate_suffix)));
  }
  else
  {
    str8_list_push(arena, out, push_str8f(arena, "%.*s- [%.*s]%.*s\n", str8_varg(indent), str8_varg(type_name), str8_varg(estimate_suffix)));
  }
}

internal void
plan_print(Arena* arena, String8List* out, PLAN_Node* plan, U64 depth)
{
  if (!plan)
  {
    return;
  }

  String8 indent = plan_indent_string(arena, depth);
  String8 estimate_suffix = plan_estimate_suffix(arena, plan, 0, 0);
  plan_print_plain_line(arena, out, plan, indent, estimate_suffix);

  plan_print(arena, out, plan->input, depth + 1);
  plan_print(arena, out, plan->input2, depth + 1);
}

internal F64
plan_us_to_ms(U64 us)
{
  return (F64)us / 1000.0;
}

internal void
plan_print_scan_trace(Arena* arena, String8List* out, PLAN_Node* plan, QE_NodeTrace* node_trace, String8 indent)
{
  String8 table_name = plan->value;
  if (plan->type == PLAN_NodeType_Filter && plan->input)
  {
    table_name = plan->input->value;
  }
  QE_ScanTrace* s = &node_trace->scan;

  String8List line = {0};
  str8_list_pushf(arena, &line, "%.*s- [Scan] table='%.*s' rows=%llu", str8_varg(indent), str8_varg(table_name), s->rows_before);
  if (s->rows_after != s->rows_before)
  {
    str8_list_pushf(arena, &line, "->%llu", s->rows_after);
  }
  if (s->zonemap_pruned_rows > 0)
  {
    F64 pct = (s->rows_before > 0) ? (100.0 * (F64)s->zonemap_pruned_rows / (F64)s->rows_before) : 0.0;
    if (s->zonemap_column_name.size > 0)
    {
      str8_list_pushf(arena, &line, " (%.0f%% pruned via zone map on '%.*s')", pct, str8_varg(s->zonemap_column_name));
    }
    else
    {
      str8_list_pushf(arena, &line, " (%.0f%% pruned via zone map)", pct);
    }
  }

  if (s->strategy == QE_TraceStrategy_Identity)
  {
    str8_list_pushf(arena, &line, " strategy=identity (%.*s)", str8_varg(s->strategy_reason));
  }
  else if (s->strategy == QE_TraceStrategy_IndexScan)
  {
    str8_list_pushf(arena, &line, " strategy=index scan");
  }
  else if (s->strategy == QE_TraceStrategy_CpuScan)
  {
    str8_list_pushf(arena, &line, " strategy=CPU scan (%.*s) time=%.1fms", str8_varg(s->strategy_reason), plan_us_to_ms(s->submit_wait_time_us));
  }
  else if (s->gpu_kernel_time_us > 0 || s->submit_wait_time_us > 0)
  {
    str8_list_pushf(arena, &line, " gpu=%.1fms submit/wait=%.1fms", plan_us_to_ms(s->gpu_kernel_time_us), plan_us_to_ms(s->submit_wait_time_us));
  }

  if (s->dict_decision_made)
  {
    if (s->dict_hit)
    {
      str8_list_pushf(arena, &line, " dict=hit (dict_size=%llu)", s->dict_size);
    }
    else
    {
      str8_list_pushf(arena, &line, " dict=miss (0 rows scanned)");
    }
  }

  if (s->gpu_cache_hit_count > 0)
  {
    str8_list_pushf(arena, &line, " gpu-cache=reused (%.1fMB)", (F64)s->gpu_cache_hit_bytes / (1024.0 * 1024.0));
  }
  else if (s->gpu_cache_miss_count > 0)
  {
    str8_list_pushf(arena, &line, " gpu-cache=miss, re-uploaded (%.1fMB)", (F64)s->gpu_cache_miss_bytes / (1024.0 * 1024.0));
  }

  B32 is_filter = plan->type == PLAN_NodeType_Filter;
  U64 actual_rows = is_filter ? s->rows_after : s->rows_before;
  String8 suffix = plan_estimate_suffix(arena, plan, 1, actual_rows);
  str8_list_pushf(arena, &line, "%.*s\n", str8_varg(suffix));
  str8_list_push(arena, out, str8_list_join(arena, &line, 0));
}

internal void
plan_print_analyzed(Arena* arena, String8List* out, PLAN_Node* plan, QE_TraceCtx* trace, U64 depth)
{
  if (!plan)
  {
    return;
  }

  String8 indent = plan_indent_string(arena, depth);
  QE_NodeTrace* node_trace = qe_trace_find(trace, plan);

  B32 is_scan_trace = node_trace && (plan->type == PLAN_NodeType_Scan || (plan->type == PLAN_NodeType_Filter && plan->input && plan->input->type == PLAN_NodeType_Scan));
  B32 filter_over_scan = plan->type == PLAN_NodeType_Filter && plan->input && plan->input->type == PLAN_NodeType_Scan;

  if (is_scan_trace)
  {
    plan_print_scan_trace(arena, out, plan, node_trace, indent);
  }
  else if (plan_node_is_join(plan) && node_trace)
  {
    QE_JoinTrace* j = &node_trace->join;
    String8 suffix = plan_estimate_suffix(arena, plan, 1, j->output_row_count);
    String8 join_label = str8_skip(plan_node_type_to_string(plan->type), str8_lit("PLAN_NodeType_").size);
    str8_list_push(arena, out, push_str8f(arena,
                                          "%.*s- [%.*s] type='%.*s' build_rows=%llu probe_rows=%llu -> %llu build=%.1fms probe=%.1fms download=%.1fms%.*s\n",
                                          str8_varg(indent), str8_varg(join_label), str8_varg(plan->value), j->build_row_count, j->probe_row_count, j->output_row_count,
                                          plan_us_to_ms(j->build_time_us), plan_us_to_ms(j->probe_dispatch_time_us), plan_us_to_ms(j->probe_download_time_us),
                                          str8_varg(suffix)));
  }
  else if (plan->type == PLAN_NodeType_Aggregate && node_trace)
  {
    QE_AggregateTrace* a = &node_trace->aggregate;
    String8 suffix = plan_estimate_suffix(arena, plan, 1, a->group_count);
    str8_list_push(arena, out, push_str8f(arena,
                                          "%.*s- [Aggregate] rows=%llu->%llu groups gather=%.1fms assign=%.1fms reduce=%.1fms combine=%.1fms%.*s\n",
                                          str8_varg(indent), a->input_row_count, a->group_count,
                                          plan_us_to_ms(a->gather_time_us), plan_us_to_ms(a->assign_time_us), plan_us_to_ms(a->reduce_time_us), plan_us_to_ms(a->combine_time_us),
                                          str8_varg(suffix)));
  }
  else if (plan->type == PLAN_NodeType_Sort && node_trace)
  {
    QE_SortTrace* s = &node_trace->sort;
    String8 suffix = plan_estimate_suffix(arena, plan, 1, s->row_count);
    String8 strategy = plan_sort_strategy_suffix(arena, plan);
    char* device = s->used_cpu ? "cpu" : "gpu";
    str8_list_push(arena, out, push_str8f(arena, "%.*s- [Sort] rows=%llu %s=%.1fms%.*s%.*s\n", str8_varg(indent), s->row_count, device, plan_us_to_ms(s->gpu_time_us),
                                          str8_varg(strategy), str8_varg(suffix)));
  }
  else if (plan->type == PLAN_NodeType_TopN && node_trace)
  {
    QE_SortTrace* s = &node_trace->sort;
    U64 actual_rows = s->kept_rows;
    if (plan->offset_node)
    {
      U64 offset = u64_from_str8(plan->offset_node->value, 10);
      actual_rows = (actual_rows > offset) ? actual_rows - offset : 0;
    }
    String8 suffix = plan_estimate_suffix(arena, plan, 1, actual_rows);
    String8 strategy = plan_sort_strategy_suffix(arena, plan);
    str8_list_push(arena, out, push_str8f(arena, "%.*s- [TopN] rows=%llu->%llu%.*s%.*s\n", str8_varg(indent), s->row_count, s->kept_rows,
                                          str8_varg(strategy), str8_varg(suffix)));
  }
  else if (node_trace && plan_node_records_rows_only(plan))
  {
    String8 type_name = plan_node_type_to_string(plan->type);
    String8 short_name = str8_skip(type_name, str8_lit("PLAN_NodeType_").size);
    String8 suffix = plan_estimate_suffix(arena, plan, 1, node_trace->rows.rows_out);
    str8_list_push(arena, out, push_str8f(arena, "%.*s- [%.*s] rows=%llu%.*s\n", str8_varg(indent), str8_varg(short_name), node_trace->rows.rows_out,
                                          str8_varg(suffix)));
  }
  else
  {
    String8 suffix = plan_estimate_suffix(arena, plan, 0, 0);
    plan_print_plain_line(arena, out, plan, indent, suffix);
  }

  // tec: the scan line above already covers the scan under a filter
  if (!(is_scan_trace && filter_over_scan))
  {
    plan_print_analyzed(arena, out, plan->input, trace, depth + 1);
  }
  plan_print_analyzed(arena, out, plan->input2, trace, depth + 1);
}

//~ tec: nested SELECT execution

internal String8
plan_output_column_name(Arena* arena, IR_Node* item)
{
  IR_Node* alias = ir_node_find_child(item, IR_NodeType_Alias);
  if (alias) return alias->value;
  
  if (item->type == IR_NodeType_Column)
  {
    String8 value = item->value;
    for (U64 i = value.size; i > 0; i--)
    {
      if (value.str[i - 1] == '.') return str8_skip(value, i);
    }
    return value;
  }
  return qe_column_list_item_display_name(arena, item);
}

internal B32
plan_materialize_rowset_item(Arena* arena, PLAN_RowSet* rows, IR_Node* item, U64 count, PLAN_AggColumn* out_col)
{
  B32 is_distance = 0;
  if (qe_ir_is_fuzzy_call(item, &is_distance))
  {
    out_col->type = GDB_ColumnType_F64;
    out_col->numeric_values = push_array(arena, F64, Max(count, 1));
    for (U64 i = 0; i < count; i++) 
    {
      out_col->numeric_values[i] = qe_row_eval_fuzzy_call(arena, rows, item, i, is_distance);
    }
    return 1;
  }
  
  if (item->type != IR_NodeType_Column)
  {
    log_error("this select item can't be used in a subquery result yet");
    return 0;
  }
  
  String8 bare_name = {0};
  U64 slot = max_U64;
  GDB_Table* table = qe_resolve_column_table(rows, item->value, &bare_name, &slot);
  GDB_Column* column = table ? gdb_table_find_column(table, bare_name) : NULL;
  if (!column) return 0;
  
  out_col->type = column->type;
  out_col->decimal_scale = column->decimal_scale;
  out_col->enum_type = column->enum_type;
  
  U64* table_rows = rows->row_indices[slot];
  for (U64 i = 0; i < count; i++)
  {
    U64 row = table_rows[i];
    if (row == PLAN_NULL_ROW || gdb_column_is_null(column, row))
    {
      if (!out_col->is_null) 
      {
        out_col->is_null = push_array(arena, U8, Max(count, 1));
      }
      out_col->is_null[i] = 1;
    }
  }
  
  if (column->type == GDB_ColumnType_String8)
  {
    GDB_StringDataChunk chunk = qe_gather_string_column(arena, rows, slot, column);
    out_col->string_values = push_array(arena, String8, Max(count, 1));
    for (U64 i = 0; i < count; i++)
    {
      out_col->string_values[i] = str8((U8*)chunk.data + chunk.offsets[i], chunk.offsets[i + 1] - chunk.offsets[i]);
    }
  }
  else
  {
    out_col->numeric_values = qe_gather_numeric_column(arena, rows, slot, column);
  }
  return 1;
}

internal B32
plan_materialize_result(Arena* arena, PLAN_ExecResult* result, IR_Node* column_list_ir, PLAN_Materialized* out)
{
  U64 column_count = 0;
  for (IR_Node* item = column_list_ir->first; item != NULL; item = item->next) column_count++;
  
  PLAN_Materialized m = {0};
  m.columns = push_array(arena, PLAN_AggColumn, Max(column_count, 1));
  m.column_count = column_count;
  m.count = result->is_materialized ? result->materialized.count : result->rows.count;
  
  U64 ci = 0;
  for (IR_Node* item = column_list_ir->first; item != NULL; item = item->next, ci++)
  {
    PLAN_AggColumn* out_col = &m.columns[ci];
    out_col->name = push_str8_copy(arena, plan_output_column_name(arena, item));
    
    if (result->is_materialized)
    {
      String8 lookup = qe_column_list_item_display_name(arena, item);
      PLAN_AggColumn* src = NULL;
      for (U64 c = 0; c < result->materialized.column_count; c++)
      {
        if (str8_match(result->materialized.columns[c].name, lookup, 0)) 
        { 
          src = &result->materialized.columns[c]; 
          break; 
        }
      }
      if (!src)
      {
        log_error("subquery result is missing column '%.*s'", str8_varg(lookup));
        return 0;
      }
      
      out_col->type = src->type;
      out_col->decimal_scale = src->decimal_scale;
      out_col->enum_type = src->enum_type;
      if (src->string_values)
      {
        out_col->string_values = push_array(arena, String8, Max(m.count, 1));
        for (U64 i = 0; i < m.count; i++)
        {
          out_col->string_values[i] = push_str8_copy(arena, src->string_values[i]);
        }
      }
      else
      {
        out_col->numeric_values = push_array(arena, F64, Max(m.count, 1));
        if (m.count)
        {
          MemoryCopy(out_col->numeric_values, src->numeric_values, m.count * sizeof(F64));
        }
      }
      if (src->is_null)
      {
        out_col->is_null = push_array(arena, U8, Max(m.count, 1));
        MemoryCopy(out_col->is_null, src->is_null, m.count);
      }
      continue;
    }
    
    if (!plan_materialize_rowset_item(arena, &result->rows, item, m.count, out_col))
    {
      return 0;
    }
  }
  
  *out = m;
  return 1;
}

internal void
plan_temp_column_append(GDB_Column* column, PLAN_AggColumn* src, U64 row)
{
  B32 is_null = src->is_null && src->is_null[row];
  F64 x = src->numeric_values ? src->numeric_values[row] : 0.0;
  
  union { 
    U32 u32; 
    U64 u64; 
    F32 f32; 
    F64 f64; 
    U8 u8; 
    S32 i32; 
    S64 i64; 
    String8 str; 
  } v = {0};
  
  switch (column->type)
  {
    case GDB_ColumnType_String8: v.str = src->string_values ? src->string_values[row] : str8_lit(""); break;
    case GDB_ColumnType_U32:
    case GDB_ColumnType_Enum: v.u32 = (U32)x; break;
    case GDB_ColumnType_U64: v.u64 = (U64)x; break;
    case GDB_ColumnType_F32: v.f32 = (F32)x; break;
    case GDB_ColumnType_F64: v.f64 = x; break;
    case GDB_ColumnType_Bool: v.u8 = (U8)x; break;
    case GDB_ColumnType_I32:
    case GDB_ColumnType_Date: v.i32 = (S32)x; break;
    case GDB_ColumnType_I64:
    case GDB_ColumnType_Timestamp:
    case GDB_ColumnType_Decimal: v.i64 = (S64)x; break;
    default: break;
  }
  gdb_column_add_data_maybe_null(column, &v, is_null);
}

internal GDB_Table*
plan_materialized_to_temp_table(GDB_Database* database, String8 name, PLAN_Materialized* m)
{
  GDB_Table* table = gdb_table_alloc(name);
  table->is_ephemeral = 1;
  U64 temp_index = database->temp_table_count;
  gdb_database_add_temp_table(database, table);
  
  for (U64 c = 0; c < m->column_count; c++)
  {
    for (U64 prev = 0; prev < c; prev++)
    {
      if (str8_match(m->columns[prev].name, m->columns[c].name, StringMatchFlag_CaseInsensitive))
      {
        log_error("'%.*s' has two columns named '%.*s' - give one of them an alias", str8_varg(name), str8_varg(m->columns[c].name));
        gdb_database_release_temp_tables_from(database, temp_index);
        return NULL;
      }
    }
    
    gdb_table_add_column(table, gdb_column_schema_create(m->columns[c].name, m->columns[c].type));
    GDB_Column* column = table->columns[c];
    column->decimal_scale = m->columns[c].decimal_scale;
    if (m->columns[c].type == GDB_ColumnType_Decimal)
    {
      column->decimal_precision = 18;
    }
    column->enum_type = m->columns[c].enum_type;
  }
  
  for (U64 c = 0; c < m->column_count; c++)
  {
    for (U64 r = 0; r < m->count; r++)
    {
      plan_temp_column_append(table->columns[c], &m->columns[c], r);
    }
  }
  table->row_count = m->count;
  return table;
}

// tec: runs the inner select into a temp table named after the alias, and turns the Table node into a plain reference to it
internal B32
plan_bind_derived_table(Arena* arena, GDB_Database* database, IR_Node* table_ir)
{
  IR_Node* sub_select = ir_node_find_child(table_ir, IR_NodeType_Select);
  if (!sub_select)
  {
    return 1;
  }
  
  IR_Node* alias = ir_node_find_child(table_ir, IR_NodeType_Alias);
  if (!alias)
  {
    log_error("a subquery in 'from' needs an alias");
    return 0;
  }
  
  PLAN_Materialized m = {0};
  if (!plan_run_select_materialized(arena, database, sub_select, &m)) 
  {
    return 0;
  }
  if (!plan_materialized_to_temp_table(database, alias->value, &m)) 
  {
    return 0;
  }
  
  table_ir->value = alias->value;
  table_ir->first = table_ir->last = alias;
  alias->prev = alias->next = NULL;
  return 1;
}

//~ tec: uncorrelated subqueries run first and are substituted into the IR as literals

// tec: returns 0 for a NULL cell
internal B32
plan_value_to_ir_node(Arena* arena, PLAN_AggColumn* col, U64 row, IR_Node* out)
{
  if (col->is_null && col->is_null[row]) 
  {
    return 0;
  }
  
  F64 x = col->numeric_values ? col->numeric_values[row] : 0.0;
  switch (col->type)
  {
    case GDB_ColumnType_String8:
    {
      out->type = IR_NodeType_Literal;
      out->value = push_str8_copy(arena, col->string_values[row]);
    } break;
    case GDB_ColumnType_Bool:
    case GDB_ColumnType_U32:
    case GDB_ColumnType_U64:
    {
      out->type = IR_NodeType_Numeric;
      out->value = push_str8f(arena, "%llu", (U64)x);
    } break;
    case GDB_ColumnType_I32:
    case GDB_ColumnType_I64:
    {
      out->type = IR_NodeType_Numeric;
      out->value = push_str8f(arena, "%lld", (S64)x);
    } break;
    case GDB_ColumnType_Date:
    {
      out->type = IR_NodeType_Literal;
      out->value = push_iso_date_string(arena, (S32)x);
    } break;
    case GDB_ColumnType_Timestamp:
    {
      out->type = IR_NodeType_Literal;
      out->value = push_iso_timestamp_string(arena, (S64)x);
    } break;
    case GDB_ColumnType_Decimal:
    {
      out->type = IR_NodeType_Numeric;
      out->value = decimal_to_str8(arena, (S64)x, col->decimal_scale);
    } break;
    case GDB_ColumnType_Enum:
    {
      out->type = IR_NodeType_Literal;
      out->value = col->enum_type ? push_str8_copy(arena, gdb_enum_type_label_from_code(col->enum_type, (U32)x)) : str8_lit("");
    } break;
    default:
    {
      out->type = IR_NodeType_Numeric;
      out->value = push_str8f(arena, "%.17g", x);
    } break;
  }
  return 1;
}

internal void
plan_make_constant_condition(Arena* arena, IR_Node* node, B32 value)
{
  node->type = IR_NodeType_Operator;
  node->value = str8_lit("=");
  node->first = node->last = NULL;
  ir_node_add_child(node, ir_node_make(arena, IR_NodeType_Numeric, str8_lit("1")));
  ir_node_add_child(node, ir_node_make(arena, IR_NodeType_Numeric, value ? str8_lit("1") : str8_lit("0")));
}

internal GDB_Table*
plan_find_table_quiet(GDB_Database* database, String8 name)
{
  for (U64 i = database->temp_table_count; i > 0; i--)
  {
    if (str8_match(database->temp_tables[i - 1]->name, name, 0)) 
    {
      return database->temp_tables[i - 1];
    }
  }
  for (U64 i = 0; i < database->table_count; i++)
  {
    if (str8_match(database->tables[i]->name, name, 0))
    {
      return database->tables[i];
    }
  }
  return NULL;
}

internal B32
plan_column_resolves(PLAN_SubqueryTable* tables, U64 table_count, String8 name)
{
  String8 qualifier = {0};
  String8 bare = name;
  for (U64 i = 0; i < name.size; i++)
  {
    if (name.str[i] == '.')
    {
      qualifier = str8_prefix(name, i);
      bare = str8_skip(name, i + 1);
      break;
    }
  }
  
  for (U64 t = 0; t < table_count; t++)
  {
    if (qualifier.size)
    {
      String8 label = tables[t].alias.size ? tables[t].alias : tables[t].table->name;
      if (!str8_match(label, qualifier, StringMatchFlag_CaseInsensitive))
      {
        continue;
      }
    }
    if (gdb_table_find_column(tables[t].table, bare)) 
    {
      return 1;
    }
  }
  return 0;
}

internal B32
plan_check_uncorrelated(Arena* arena, GDB_Database* database, IR_Node* select_ir)
{
  PLAN_SubqueryTable tables[16];
  U64 table_count = 0;
  
  for (IR_Node* child = select_ir->first; child != NULL; child = child->next)
  {
    IR_Node* table_ir = (child->type == IR_NodeType_Table) ? child :
    (child->type == IR_NodeType_Join && child->first && child->first->type == IR_NodeType_Table) ? child->first : NULL;
    if (!table_ir)
    {
      continue;
    }
    
    // tec: a derived table or the subquery's own WITH isnt known until it runs
    if (table_ir->value.size == 0 || ir_node_find_child(select_ir, IR_NodeType_CteList)) 
    {
      return 1;
    }
    
    GDB_Table* table = plan_find_table_quiet(database, table_ir->value);
    if (!table || table_count >= ArrayCount(tables)) 
    {
      return 1;
    }
    tables[table_count].table = table;
    tables[table_count].alias = plan_alias_from_table_ir(table_ir);
    table_count++;
  }
  if (table_count == 0) return 1;
  
  // tec: GROUP BY/HAVING/ORDER BY can name select aliases, so only the select list and WHERE are checked
  IR_Node* roots[2] = { ir_node_find_child(select_ir, IR_NodeType_ColumnList), ir_node_find_child(select_ir, IR_NodeType_Where) };
  for (U64 r = 0; r < ArrayCount(roots); r++)
  {
    if (!roots[r]) 
    {
      continue;
    }
    
    IR_Node* stack[64];
    U64 top = 0;
    stack[top++] = roots[r];
    while (top > 0)
    {
      IR_Node* node = stack[--top];
      
      // tec: a nested subquery has its own scope, and gets checked when it runs
      if (node->type == IR_NodeType_Subquery || node->type == IR_NodeType_Exists || node->type == IR_NodeType_Alias) continue;
      
      if (node->type == IR_NodeType_Column && !str8_match(node->value, str8_lit("*"), 0) &&
          !plan_column_resolves(tables, table_count, node->value))
      {
        log_error("correlated subqueries are not supported yet: column '%.*s' isn't in the subquery's own tables",
                  str8_varg(node->value));
        return 0;
      }
      
      for (IR_Node* c = node->first; c != NULL && top < ArrayCount(stack); c = c->next) 
      {
        stack[top++] = c;
      }
    }
  }
  return 1;
}

internal B32
plan_run_subquery_values(Arena* arena, GDB_Database* database, IR_Node* subquery_ir, PLAN_Materialized* out)
{
  IR_Node* select_ir = ir_node_find_child(subquery_ir, IR_NodeType_Select);
  if (!select_ir) return 0;
  
  if (!plan_check_uncorrelated(arena, database, select_ir)) return 0;
  if (!plan_run_select_materialized(arena, database, select_ir, out)) return 0;
  
  if (out->column_count != 1)
  {
    log_error("a subquery used as a value must return exactly one column, this one returns %llu", out->column_count);
    return 0;
  }
  return 1;
}

// tec: the subquery's values become the literals of an IN list
internal void
plan_apply_subquery_in_list(Arena* arena, IR_Node* node, IR_Node* subquery_node, PLAN_Materialized* values, B32 is_not_in)
{
  subquery_node->type = IR_NodeType_InList;
  subquery_node->first = subquery_node->last = NULL;

  B32 saw_null = 0;
  for (U64 row = 0; row < values->count; row += 1)
  {
    IR_Node* item = ir_node_make(arena, IR_NodeType_Literal, (String8){0});
    if (!plan_value_to_ir_node(arena, &values->columns[0], row, item))
    {
      saw_null = 1;
      continue;
    }
    ir_node_add_child(subquery_node, item);
  }

  // tec: x NOT IN (..., NULL) is never true
  if (saw_null && is_not_in)
  {
    plan_make_constant_condition(arena, node, 0);
  }
}

internal B32
plan_rewrite_predicates(Arena* arena, GDB_Database* database, IR_Node* node)
{
  if (!node) return 1;
  
  if (node->type == IR_NodeType_Exists)
  {
    IR_Node* select_ir = node->first;
    if (!select_ir || select_ir->type != IR_NodeType_Select) return 0;
    
    if (!plan_check_uncorrelated(arena, database, select_ir)) return 0;
    
    // tec: only whether a row exists matters
    if (!ir_node_find_child(select_ir, IR_NodeType_Limit))
    {
      ir_node_add_child(select_ir, ir_node_make(arena, IR_NodeType_Limit, str8_lit("1")));
    }
    
    PLAN_Materialized m = {0};
    if (!plan_run_select_materialized(arena, database, select_ir, &m)) return 0;
    
    B32 negate = str8_match(node->value, str8_lit("not"), 0);
    plan_make_constant_condition(arena, node, (m.count > 0) != negate);
    return 1;
  }
  
  if (node->type != IR_NodeType_Operator) return 1;
  
  String8 op = node->value;
  IR_Node* left = node->first;
  IR_Node* right = left ? left->next : NULL;
  
  if (str8_match(op, str8_lit("and"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    return plan_rewrite_predicates(arena, database, left) && plan_rewrite_predicates(arena, database, right);
  }
  
  if (str8_match(op, str8_lit("in"), StringMatchFlag_CaseInsensitive) ||
      str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive))
  {
    if (!right || right->type != IR_NodeType_Subquery) return 1; // tec: a literal list, nothing to run
    
    PLAN_Materialized m = {0};
    if (!plan_run_subquery_values(arena, database, right, &m)) return 0;

    B32 is_not_in = str8_match(op, str8_lit("not in"), StringMatchFlag_CaseInsensitive);
    plan_apply_subquery_in_list(arena, node, right, &m, is_not_in);
    return 1;
  }
  
  // tec: comparison with a scalar subquery on either side
  for (IR_Node* operand = left; operand != NULL; operand = operand->next)
  {
    if (operand->type != IR_NodeType_Subquery) continue;
    
    PLAN_Materialized m = {0};
    if (!plan_run_subquery_values(arena, database, operand, &m)) return 0;
    
    if (m.count > 1)
    {
      log_error("a scalar subquery returned %llu rows, it must return at most one", m.count);
      return 0;
    }
    
    operand->first = operand->last = NULL;
    if (m.count == 0 || !plan_value_to_ir_node(arena, &m.columns[0], 0, operand))
    {
      // tec: comparing against no row / NULL is never true
      plan_make_constant_condition(arena, node, 0);
      return 1;
    }
  }
  return 1;
}

//~ tec: window functions

// tec: NULLs sort as the smallest value, ahead of everything else
internal S32
plan_window_key_compare(PLAN_WindowKey* key, U64 a, U64 b)
{
  B32 a_null = key->is_null && key->is_null[a];
  B32 b_null = key->is_null && key->is_null[b];
  if (a_null || b_null) 
  {
    return (a_null == b_null) ? 0 : (a_null ? -1 : 1);
  }
  
  if (key->is_string) 
  {
    return qe_str8_compare(key->strs[a], key->strs[b]);
  }
  return (key->nums[a] < key->nums[b]) ? -1 : (key->nums[a] > key->nums[b]) ? 1 : 0;
}

internal S32
plan_window_row_compare(const void* pa, const void* pb)
{
  U64 a = *(const U64*)pa;
  U64 b = *(const U64*)pb;
  PLAN_WindowSortCtx* ctx = g_plan_window_sort_ctx;
  
  for (U64 k = 0; k < ctx->key_count; k++)
  {
    S32 cmp = plan_window_key_compare(&ctx->keys[k], a, b);
    if (cmp != 0) 
    {
      return ctx->keys[k].descending ? -cmp : cmp;
    }
  }
  return (a < b) ? -1 : (a > b) ? 1 : 0;
}

internal B32
plan_window_keys_equal(PLAN_WindowKey* keys, U64 first, U64 count, U64 a, U64 b)
{
  for (U64 k = first; k < first + count; k++)
  {
    if (plan_window_key_compare(&keys[k], a, b) != 0) 
    {
      return 0;
    }
  }
  return 1;
}

internal B32
plan_window_gather_key(Arena* arena, PLAN_RowSet* rows, String8 column_name, B32 descending, PLAN_WindowKey* out)
{
  IR_Node* item = ir_node_make(arena, IR_NodeType_Column, column_name);
  PLAN_AggColumn col = {0};
  if (!plan_materialize_rowset_item(arena, rows, item, rows->count, &col)) 
  {
    return 0;
  }
  
  out->is_string = (col.type == GDB_ColumnType_String8);
  out->descending = descending;
  out->nums = col.numeric_values;
  out->strs = col.string_values;
  out->is_null = col.is_null;
  return 1;
}

internal B32
plan_window_compute(Arena* arena, PLAN_RowSet* rows, IR_Node* call, PLAN_AggColumn* out_col)
{
  U64 n = rows->count;
  String8 name = call->value;
  
  //- tec: which function
  PLAN_WindowFunc func = PLAN_WindowFunc_Agg;
  U32 agg_code = 0;
  if (str8_match(name, str8_lit("row_number"), StringMatchFlag_CaseInsensitive)) 
  {
    func = PLAN_WindowFunc_RowNumber;
  }
  else if (str8_match(name, str8_lit("rank"), StringMatchFlag_CaseInsensitive)) 
  {
    func = PLAN_WindowFunc_Rank;
  }
  else if (str8_match(name, str8_lit("dense_rank"), StringMatchFlag_CaseInsensitive)) 
  {
    func = PLAN_WindowFunc_DenseRank;
  }
  else if (str8_match(name, str8_lit("lag"), StringMatchFlag_CaseInsensitive)) 
  {
    func = PLAN_WindowFunc_Lag;
  }
  else if (str8_match(name, str8_lit("lead"), StringMatchFlag_CaseInsensitive)) 
  {
    func = PLAN_WindowFunc_Lead;
  }
  else
  {
    if (!qe_agg_func_code_from_name(name, &agg_code) ||
        (agg_code != QE_AGG_FUNC_COUNT && agg_code != QE_AGG_FUNC_SUM && agg_code != QE_AGG_FUNC_AVG &&
         agg_code != QE_AGG_FUNC_MIN && agg_code != QE_AGG_FUNC_MAX))
    {
      log_error("'%.*s' is not a supported window function (supported: ROW_NUMBER, RANK, DENSE_RANK, LAG, LEAD, SUM, COUNT, AVG, MIN, MAX)", str8_varg(name));
      return 0;
    }
  }
  
  //- tec: the call's arguments
  IR_Node* args[3] = {0};
  U64 arg_count = 0;
  for (IR_Node* c = call->first; c != NULL; c = c->next)
  {
    if (c->type != IR_NodeType_Column && c->type != IR_NodeType_Numeric && c->type != IR_NodeType_Literal)
    {
      continue;
    }
    if (arg_count < ArrayCount(args)) 
    {
      args[arg_count] = c;
    }
    arg_count++;
  }
  
  B32 is_ranking = (func == PLAN_WindowFunc_RowNumber || func == PLAN_WindowFunc_Rank || func == PLAN_WindowFunc_DenseRank);
  if (is_ranking && arg_count != 0)
  {
    log_error("%.*s() takes no arguments", str8_varg(name));
    return 0;
  }
  if (!is_ranking && arg_count == 0)
  {
    log_error("%.*s() needs an argument", str8_varg(name));
    return 0;
  }
  if (arg_count > 3 || ((func == PLAN_WindowFunc_Agg) && arg_count != 1))
  {
    log_error("%.*s() was given the wrong number of arguments", str8_varg(name));
    return 0;
  }
  
  //- tec: partition and order keys
  IR_Node* window_ir = ir_node_find_child(call, IR_NodeType_Window);
  IR_Node* partition_ir = ir_node_find_child(window_ir, IR_NodeType_PartitionBy);
  IR_Node* order_ir = ir_node_find_child(window_ir, IR_NodeType_OrderBy);
  
  PLAN_WindowKey keys[PLAN_WINDOW_MAX_KEYS] = {0};
  U64 partition_key_count = 0;
  U64 order_key_count = 0;
  
  for (IR_Node* c = partition_ir ? partition_ir->first : NULL; c != NULL; c = c->next)
  {
    if (partition_key_count + order_key_count >= PLAN_WINDOW_MAX_KEYS)
    {
      log_error("a window can have at most %d partition and order columns in total", PLAN_WINDOW_MAX_KEYS);
      return 0;
    }
    if (!plan_window_gather_key(arena, rows, c->value, 0, &keys[partition_key_count]))
    {
      return 0;
    }
    partition_key_count++;
  }
  for (IR_Node* c = order_ir ? order_ir->first : NULL; c != NULL; c = c->next)
  {
    if (c->type != IR_NodeType_Column)
    {
      log_error("a window's ORDER BY can only sort by plain columns");
      return 0;
    }
    if (partition_key_count + order_key_count >= PLAN_WINDOW_MAX_KEYS)
    {
      log_error("a window can have at most %d partition and order columns in total", PLAN_WINDOW_MAX_KEYS);
      return 0;
    }
    B32 descending = c->first && c->first->type == IR_NodeType_Descending;
    if (!plan_window_gather_key(arena, rows, c->value, descending, &keys[partition_key_count + order_key_count])) 
    {
      return 0;
    }
    order_key_count++;
  }
  U64 key_count = partition_key_count + order_key_count;
  B32 has_order = order_key_count > 0;
  
  //- tec: the argument column, when the function reads one
  PLAN_AggColumn src = {0};
  GDB_Column* src_column = NULL;
  B32 count_star = 0;
  if (func == PLAN_WindowFunc_Agg && agg_code == QE_AGG_FUNC_COUNT && args[0]->type == IR_NodeType_Column &&
      str8_match(args[0]->value, str8_lit("*"), 0))
  {
    count_star = 1;
  }
  else if (arg_count > 0)
  {
    if (args[0]->type != IR_NodeType_Column)
    {
      log_error("%.*s()'s first argument must be a column", str8_varg(name));
      return 0;
    }
    if (!plan_materialize_rowset_item(arena, rows, args[0], n, &src)) 
    {
      return 0;
    }
    
    String8 bare = {0};
    U64 slot = max_U64;
    GDB_Table* table = qe_resolve_column_table(rows, args[0]->value, &bare, &slot);
    src_column = table ? gdb_table_find_column(table, bare) : NULL;
    
    if (func == PLAN_WindowFunc_Agg && agg_code != QE_AGG_FUNC_COUNT && src.type == GDB_ColumnType_String8)
    {
      log_error("%.*s() over a string column is not supported", str8_varg(name));
      return 0;
    }
  }
  
  //- tec: output type
  if (is_ranking)
  {
    out_col->type = GDB_ColumnType_U64;
  }
  else if (func == PLAN_WindowFunc_Lag || func == PLAN_WindowFunc_Lead)
  {
    out_col->type = src.type;
    out_col->decimal_scale = src.decimal_scale;
    out_col->enum_type = src.enum_type;
  }
  else
  {
    QE_AggExprInfo info = {0};
    info.func_code = agg_code;
    info.arg_column = count_star ? NULL : src_column;
    qe_agg_output_type_for_expr(&info, &out_col->type, &out_col->decimal_scale, &out_col->enum_type);
  }
  
  B32 out_is_string = (out_col->type == GDB_ColumnType_String8);
  if (out_is_string) out_col->string_values = push_array(arena, String8, Max(n, 1));
  else out_col->numeric_values = push_array(arena, F64, Max(n, 1));
  
#define PLAN_WINDOW_SET_NULL(row) do { if (!out_col->is_null) out_col->is_null = push_array(arena, U8, Max(n, 1)); out_col->is_null[(row)] = 1; } while (0)
  
  //- tec: LAG / LEAD offset and default
  S64 offset = 1;
  IR_Node* default_ir = NULL;
  if (func == PLAN_WindowFunc_Lag || func == PLAN_WindowFunc_Lead)
  {
    if (arg_count >= 2)
    {
      if (args[1]->type != IR_NodeType_Numeric)
      {
        log_error("%.*s()'s offset must be a number", str8_varg(name));
        return 0;
      }
      offset = (S64)f64_from_str8(args[1]->value);
      if (offset < 0)
      {
        log_error("%.*s()'s offset can't be negative", str8_varg(name));
        return 0;
      }
    }
    if (arg_count >= 3) 
    {
      default_ir = args[2];
    }
  }
  
  //- tec: sort a row permutation by (partition keys, order keys)
  U64* idx = push_array(arena, U64, Max(n, 1));
  for (U64 i = 0; i < n; i++) 
  {
    idx[i] = i;
  }
  
  PLAN_WindowSortCtx sort_ctx = { keys, key_count };
  if (n > 1 && key_count > 0)
  {
    g_plan_window_sort_ctx = &sort_ctx;
    quick_sort(idx, n, sizeof(U64), plan_window_row_compare);
    g_plan_window_sort_ctx = 0;
  }
  
  //- tec: one pass per partition
  U64 pos = 0;
  while (pos < n)
  {
    U64 pend = pos + 1;
    while (pend < n && plan_window_keys_equal(keys, 0, partition_key_count, idx[pos], idx[pend]))
    {
      pend++;
    }
    
    switch (func)
    {
      case PLAN_WindowFunc_RowNumber:
      {
        for (U64 i = pos; i < pend; i++)
        {
          out_col->numeric_values[idx[i]] = (F64)(i - pos + 1);
        }
      } break;
      
      case PLAN_WindowFunc_Rank:
      case PLAN_WindowFunc_DenseRank:
      {
        U64 group_start = pos;
        U64 dense = 1;
        while (group_start < pend)
        {
          U64 group_end = group_start + 1;
          while (group_end < pend && 
                 plan_window_keys_equal(keys, partition_key_count, order_key_count, idx[group_start], idx[group_end])) 
          {
            group_end++;
          }
          
          F64 value = (func == PLAN_WindowFunc_Rank) ? (F64)(group_start - pos + 1) : (F64)dense;
          for (U64 i = group_start; i < group_end; i++)
          {
            out_col->numeric_values[idx[i]] = value;
          }
          dense++;
          group_start = group_end;
        }
      } break;
      
      case PLAN_WindowFunc_Lag:
      case PLAN_WindowFunc_Lead:
      {
        for (U64 i = pos; i < pend; i++)
        {
          S64 j = (func == PLAN_WindowFunc_Lag) ? (S64)i - offset : (S64)i + offset;
          U64 dst = idx[i];
          
          if (j >= (S64)pos && j < (S64)pend)
          {
            U64 from = idx[j];
            if (src.is_null && src.is_null[from]) 
            {
              PLAN_WINDOW_SET_NULL(dst);
            }
            else if (out_is_string)
            {
              out_col->string_values[dst] = src.string_values[from];
            }
            else 
            {
              out_col->numeric_values[dst] = src.numeric_values[from];
            }
          }
          else if (default_ir)
          {
            if (out_is_string) 
            {
              out_col->string_values[dst] = default_ir->value;
            }
            else 
            {
              out_col->numeric_values[dst] = f64_from_str8(default_ir->value);
            }
          }
          else
          {
            PLAN_WINDOW_SET_NULL(dst);
          }
        }
      } break;
      
      case PLAN_WindowFunc_Agg:
      {
        F64 sum = 0.0, min_v = 0.0, max_v = 0.0;
        U64 non_null = 0, all_rows = 0;
        
        U64 group_start = pos;
        while (group_start < pend)
        {
          // tec: the frame ends at the last peer with an ORDER BY, else its the whole partition
          U64 group_end = pend;
          if (has_order)
          {
            group_end = group_start + 1;
            while (group_end < pend &&
                   plan_window_keys_equal(keys, partition_key_count, order_key_count, idx[group_start], idx[group_end])) 
            {
              group_end++;
            }
          }
          
          for (U64 i = group_start; i < group_end; i++)
          {
            U64 row = idx[i];
            all_rows++;
            if (count_star) 
            {
              continue;
            }
            if (src.is_null && src.is_null[row])
            {
              continue;
            }
            
            F64 v = src.numeric_values ? src.numeric_values[row] : 0.0;
            if (non_null == 0) 
            { 
              min_v = v; 
              max_v = v; 
            }
            else 
            { 
              if (v < min_v) min_v = v; 
              if (v > max_v) max_v = v; 
            }
            sum += v;
            non_null++;
          }
          
          B32 is_null_result = 0;
          F64 value = 0.0;
          switch (agg_code)
          {
            case QE_AGG_FUNC_COUNT: value = (F64)(count_star ? all_rows : non_null); break;
            case QE_AGG_FUNC_SUM: is_null_result = (non_null == 0); value = sum; break;
            case QE_AGG_FUNC_AVG: is_null_result = (non_null == 0); value = non_null ? sum / (F64)non_null : 0.0; break;
            case QE_AGG_FUNC_MIN: is_null_result = (non_null == 0); value = min_v; break;
            case QE_AGG_FUNC_MAX: is_null_result = (non_null == 0); value = max_v; break;
          }
          
          for (U64 i = group_start; i < group_end; i++)
          {
            if (is_null_result)
            {
              PLAN_WINDOW_SET_NULL(idx[i]);
            }
            else
            {
              out_col->numeric_values[idx[i]] = value;
            }
          }
          group_start = group_end;
        }
      } break;
    }
    
    pos = pend;
  }
  
#undef PLAN_WINDOW_SET_NULL
  return 1;
}

// tec: ORDER BY columns missing from the select list ride along as hidden trailing columns
internal B32
plan_window_apply(Arena* arena, PLAN_RowSet* rows, IR_Node* column_list_ir, IR_Node* order_by_ir, PLAN_Materialized* out)
{
  U64 visible_count = 0;
  for (IR_Node* item = column_list_ir->first; item != NULL; item = item->next) 
  {
    visible_count++;
  }
  
  IR_Node* hidden[QE_SORT_MAX_KEYS] = {0};
  U64 hidden_count = 0;
  for (IR_Node* key = order_by_ir ? order_by_ir->first : NULL; key != NULL; key = key->next)
  {
    if (key->type != IR_NodeType_Column)
    {
      continue;
    }
    
    B32 visible = 0;
    for (IR_Node* item = column_list_ir->first; item != NULL; item = item->next)
    {
      if (str8_match(qe_column_list_item_display_name(arena, item), key->value, 0)) 
      { 
        visible = 1; 
        break; 
      }
    }
    if (!visible && hidden_count < ArrayCount(hidden)) 
    {
      hidden[hidden_count++] = key;
    }
  }
  
  PLAN_Materialized m = {0};
  m.count = rows->count;
  m.column_count = visible_count + hidden_count;
  m.columns = push_array(arena, PLAN_AggColumn, Max(m.column_count, 1));
  
  U64 ci = 0;
  for (IR_Node* item = column_list_ir->first; item != NULL; item = item->next, ci++)
  {
    PLAN_AggColumn* col = &m.columns[ci];
    // tec: named as the output code looks columns up
    col->name = push_str8_copy(arena, qe_column_list_item_display_name(arena, item));
    
    if (plan_ir_is_window_call(item))
    {
      if (!plan_window_compute(arena, rows, item, col))
      {
        return 0;
      }
    }
    else if (!plan_materialize_rowset_item(arena, rows, item, m.count, col))
    {
      return 0;
    }
  }
  for (U64 h = 0; h < hidden_count; h++, ci++)
  {
    m.columns[ci].name = push_str8_copy(arena, hidden[h]->value);
    if (!plan_materialize_rowset_item(arena, rows, hidden[h], m.count, &m.columns[ci])) 
    {
      return 0;
    }
  }
  
  *out = m;
  return 1;
}

internal PLAN_ExecResult
plan_run_select(Arena* arena, GDB_Database* database, IR_Node* select_ir, QE_TraceCtx* trace)
{
  return plan_run_select_with_plan(arena, database, select_ir, trace, NULL);
}

// tec: leaves its temp tables registered, since a row set result still points into them
internal PLAN_ExecResult
plan_run_select_with_plan(Arena* arena, GDB_Database* database, IR_Node* select_ir, QE_TraceCtx* trace, PLAN_Node** out_plan)
{
  PLAN_ExecResult failed = {0};
  if (out_plan)
  {
    *out_plan = NULL;
  }
  
  if (optimizer_inline_enabled())
  {
    optimizer_inline_sources(arena, database, select_ir);
  }
  
  IR_Node* cte_list = ir_node_find_child(select_ir, IR_NodeType_CteList);
  if (cte_list)
  {
    // tec: in order, so a later CTE can read an earlier one
    for (IR_Node* cte = cte_list->first; cte != NULL; cte = cte->next)
    {
      PLAN_Materialized m = {0};
      if (!cte->first || !plan_run_select_materialized(arena, database, cte->first, &m)) 
      {
        return failed;
      }
      if (!plan_materialized_to_temp_table(database, cte->value, &m)) 
      {
        return failed;
      }
    }
  }
  
  for (IR_Node* child = select_ir->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_Table)
    {
      if (!plan_bind_derived_table(arena, database, child)) 
      {
        return failed;
      }
    }
    else if (child->type == IR_NodeType_Join && child->first && child->first->type == IR_NodeType_Table)
    {
      if (!plan_bind_derived_table(arena, database, child->first)) 
      {
        return failed;
      }
    }
  }
  
  if (!optimizer_extract_semi_joins(arena, database, select_ir, 1))
  {
    return failed;
  }
  
  IR_Node* where_ir = ir_node_find_child(select_ir, IR_NodeType_Where);
  if (where_ir && !plan_rewrite_predicates(arena, database, where_ir->first)) 
  {
    return failed;
  }
  IR_Node* having_ir = ir_node_find_child(select_ir, IR_NodeType_Having);
  if (having_ir && !plan_rewrite_predicates(arena, database, having_ir->first)) 
  {
    return failed;
  }
  
  ir_expand_star_to_columns(arena, database, select_ir);
  
  PLAN_Node* plan = plan_build_from_select(arena, database, select_ir);
  if (out_plan)
  {
    *out_plan = plan;
  }
  return plan_execute(arena, database, plan, select_ir, trace);
}

// tec: builds the plan EXPLAIN shows without running anything, CTEs and derived tables are estimated from their own plans
internal PLAN_Node*
plan_build_for_explain(Arena* arena, GDB_Database* database, IR_Node* select_ir, OPT_SourceEstimates* inherited)
{
  if (optimizer_inline_enabled())
  {
    optimizer_inline_sources(arena, database, select_ir);
  }
  
  OPT_SourceEstimates sources = *inherited;
  
  IR_Node* cte_list = ir_node_find_child(select_ir, IR_NodeType_CteList);
  if (cte_list)
  {
    for (IR_Node* cte = cte_list->first; cte != NULL; cte = cte->next)
    {
      if (!cte->first)
      {
        continue;
      }
      PLAN_Node* inner_plan = plan_build_for_explain(arena, database, cte->first, &sources);
      optimizer_annotate_plan(arena, inner_plan);
      if (inner_plan && inner_plan->has_estimate)
      {
        optimizer_source_estimates_add(&sources, cte->value, inner_plan->est_rows);
      }
    }
  }
  
  for (IR_Node* child = select_ir->first; child != NULL; child = child->next)
  {
    IR_Node* table_ir = NULL;
    if (child->type == IR_NodeType_Table)
    {
      table_ir = child;
    }
    else if (child->type == IR_NodeType_Join && child->first && child->first->type == IR_NodeType_Table)
    {
      table_ir = child->first;
    }
    if (!table_ir)
    {
      continue;
    }
    
    IR_Node* inner_select = ir_node_find_child(table_ir, IR_NodeType_Select);
    IR_Node* alias = ir_node_find_child(table_ir, IR_NodeType_Alias);
    if (!inner_select || !alias)
    {
      continue;
    }
    
    PLAN_Node* inner_plan = plan_build_for_explain(arena, database, inner_select, &sources);
    optimizer_annotate_plan(arena, inner_plan);
    if (inner_plan && inner_plan->has_estimate)
    {
      optimizer_source_estimates_add(&sources, alias->value, inner_plan->est_rows);
    }
  }
  
  optimizer_extract_semi_joins(arena, database, select_ir, 0);
  ir_expand_star_to_columns(arena, database, select_ir);
  PLAN_Node* plan = plan_build_from_select(arena, database, select_ir);
  optimizer_apply_source_estimates(plan, &sources);
  return plan;
}

internal B32
plan_run_select_materialized(Arena* arena, GDB_Database* database, IR_Node* select_ir, PLAN_Materialized* out)
{
  U64 temp_mark = database->temp_table_count;
  
  PLAN_ExecResult result = plan_run_select(arena, database, select_ir, NULL);
  B32 ok = result.supported;
  if (ok)
  {
    IR_Node* column_list_ir = ir_node_find_child(select_ir, IR_NodeType_ColumnList);
    ok = column_list_ir && plan_materialize_result(arena, &result, column_list_ir, out);
  }
  
  gdb_database_release_temp_tables_from(database, temp_mark);
  return ok;
}
