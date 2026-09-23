
//~ tec: IR tree helpers

internal B32
optimizer_ir_is_operator(IR_Node* node, String8 name)
{
  if (!node)
  {
    return 0;
  }
  if (node->type != IR_NodeType_Operator)
  {
    return 0;
  }
  return str8_match(node->value, name, StringMatchFlag_CaseInsensitive);
}

internal IR_Node*
optimizer_ir_clone(Arena* arena, IR_Node* node)
{
  if (!node)
  {
    return NULL;
  }
  
  IR_Node* copy = ir_node_make(arena, node->type, node->value);
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    IR_Node* child_copy = optimizer_ir_clone(arena, child);
    ir_node_add_child(copy, child_copy);
  }
  return copy;
}

// tec: returns the real conjunct count, which can exceed capacity, only the first capacity are stored
internal U32
optimizer_ir_collect_conjuncts(IR_Node* root, IR_Node** out_conjuncts, U32 count, U32 capacity)
{
  if (!root)
  {
    return count;
  }
  
  if (optimizer_ir_is_operator(root, str8_lit("and")))
  {
    IR_Node* left = root->first;
    IR_Node* right = left ? left->next : NULL;
    count = optimizer_ir_collect_conjuncts(left, out_conjuncts, count, capacity);
    count = optimizer_ir_collect_conjuncts(right, out_conjuncts, count, capacity);
    return count;
  }
  
  if (count < capacity)
  {
    out_conjuncts[count] = root;
  }
  return count + 1;
}

// tec: left nested like the parser builds it, and every conjunct is cloned so the source tree keeps its links
internal IR_Node*
optimizer_ir_and_chain(Arena* arena, IR_Node** conjuncts, U32 count)
{
  if (count == 0)
  {
    return NULL;
  }
  
  IR_Node* chain = optimizer_ir_clone(arena, conjuncts[0]);
  for (U32 index = 1; index < count; index += 1)
  {
    IR_Node* and_node = ir_node_make(arena, IR_NodeType_Operator, str8_lit("and"));
    IR_Node* next_conjunct = optimizer_ir_clone(arena, conjuncts[index]);
    ir_node_add_child(and_node, chain);
    ir_node_add_child(and_node, next_conjunct);
    chain = and_node;
  }
  return chain;
}

internal IR_Node*
optimizer_ir_make_where(Arena* arena, IR_Node* expression)
{
  IR_Node* where_node = ir_node_make(arena, IR_NodeType_Where, (String8){0});
  ir_node_add_child(where_node, expression);
  return where_node;
}

internal void
optimizer_ir_remove_child(IR_Node* parent, IR_Node* child)
{
  if (child->prev)
  {
    child->prev->next = child->next;
  }
  else
  {
    parent->first = child->next;
  }
  
  if (child->next)
  {
    child->next->prev = child->prev;
  }
  else
  {
    parent->last = child->prev;
  }
  
  child->prev = NULL;
  child->next = NULL;
  child->parent = NULL;
}

internal void
optimizer_ir_strip_column_qualifiers(IR_Node* node)
{
  if (!node)
  {
    return;
  }
  
  // tec: a subquery is its own scope, its aliases mean nothing to the select around it
  if (node->type == IR_NodeType_Subquery || node->type == IR_NodeType_Exists)
  {
    return;
  }
  
  if (node->type == IR_NodeType_Column)
  {
    node->value = qe_bare_column_name(node->value);
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    optimizer_ir_strip_column_qualifiers(child);
  }
}

//~ tec: relation references

// tec: a column that matches no relation, or more than one, is reported as OPT_NO_RELATION
internal U32
optimizer_resolve_column(OPT_Relation* relations, U32 relation_count, String8 column_name)
{
  U32 match_count = 0;
  U32 matched_index = OPT_NO_RELATION;
  
  for (U32 index = 0; index < relation_count; index += 1)
  {
    B32 belongs = qe_column_belongs_to_table(relations[index].table, relations[index].alias, column_name);
    if (belongs)
    {
      match_count += 1;
      matched_index = index;
    }
  }
  
  if (match_count != 1)
  {
    return OPT_NO_RELATION;
  }
  return matched_index;
}

internal void
optimizer_collect_relation_refs(OPT_Relation* relations, U32 relation_count, IR_Node* node, U64* out_mask, B32* out_unresolved)
{
  switch (node->type)
  {
    case IR_NodeType_Column:
    {
      U32 relation_index = optimizer_resolve_column(relations, relation_count, node->value);
      if (relation_index == OPT_NO_RELATION)
      {
        *out_unresolved = 1;
      }
      else
      {
        *out_mask |= (U64)1 << relation_index;
      }
    } break;
    
    case IR_NodeType_Numeric:
    case IR_NodeType_Literal:
    {
    } break;
    
    case IR_NodeType_Operator:
    case IR_NodeType_InList:
    case IR_NodeType_AggregateCall:
    {
      for (IR_Node* child = node->first; child != NULL; child = child->next)
      {
        optimizer_collect_relation_refs(relations, relation_count, child, out_mask, out_unresolved);
      }
    } break;
    
    default:
    {
      *out_unresolved = 1;
    } break;
  }
}

internal U32
optimizer_mask_popcount(U64 mask)
{
  U32 count = 0;
  while (mask != 0)
  {
    count += (U32)(mask & 1);
    mask = mask >> 1;
  }
  return count;
}

internal U32
optimizer_mask_highest(U64 mask)
{
  U32 highest = 0;
  U32 bit = 0;
  while (mask != 0)
  {
    if (mask & 1)
    {
      highest = bit;
    }
    mask = mask >> 1;
    bit += 1;
  }
  return highest;
}

internal B32
optimizer_rejects_null(OPT_Relation* relations, U32 relation_count, IR_Node* node, U32 relation_index)
{
  if (!node)
  {
    return 0;
  }
  if (node->type != IR_NodeType_Operator)
  {
    return 0;
  }
  
  IR_Node* left = node->first;
  IR_Node* right = left ? left->next : NULL;
  
  if (optimizer_ir_is_operator(node, str8_lit("and")))
  {
    B32 left_rejects = optimizer_rejects_null(relations, relation_count, left, relation_index);
    B32 right_rejects = optimizer_rejects_null(relations, relation_count, right, relation_index);
    return left_rejects || right_rejects;
  }
  
  if (optimizer_ir_is_operator(node, str8_lit("or")))
  {
    B32 left_rejects = optimizer_rejects_null(relations, relation_count, left, relation_index);
    B32 right_rejects = optimizer_rejects_null(relations, relation_count, right, relation_index);
    return left_rejects && right_rejects;
  }
  
  if (optimizer_ir_is_operator(node, str8_lit("is null")))
  {
    return 0;
  }
  
  B32 left_is_column = left && left->type == IR_NodeType_Column;
  B32 left_is_relation = 0;
  if (left_is_column)
  {
    left_is_relation = optimizer_resolve_column(relations, relation_count, left->value) == relation_index;
  }
  
  if (optimizer_ir_is_operator(node, str8_lit("is not null")) ||
      optimizer_ir_is_operator(node, str8_lit("in")) ||
      optimizer_ir_is_operator(node, str8_lit("not in")))
  {
    return left_is_relation;
  }
  
  B32 is_comparison = optimizer_ir_is_operator(node, str8_lit("=")) ||
    optimizer_ir_is_operator(node, str8_lit("==")) ||
    optimizer_ir_is_operator(node, str8_lit("!=")) ||
    optimizer_ir_is_operator(node, str8_lit("<")) ||
    optimizer_ir_is_operator(node, str8_lit("<=")) ||
    optimizer_ir_is_operator(node, str8_lit(">")) ||
    optimizer_ir_is_operator(node, str8_lit(">=")) ||
    optimizer_ir_is_operator(node, str8_lit("equals")) ||
    optimizer_ir_is_operator(node, str8_lit("contains"));
  if (!is_comparison)
  {
    return 0;
  }
  
  B32 right_is_relation = 0;
  if (right && right->type == IR_NodeType_Column)
  {
    right_is_relation = optimizer_resolve_column(relations, relation_count, right->value) == relation_index;
  }
  return left_is_relation || right_is_relation;
}

//~ tec: rewrite rules (pushdown, join assembly, LEFT JOIN conversion)

internal B32
optimizer_enabled(void)
{
  B32 master = settings_bool(str8_lit("QE_OPTIMIZER"), 1);
  B32 pushdown = settings_bool(str8_lit("QE_OPT_PUSHDOWN"), 1);
  return master && pushdown;
}

// tec: only a left-deep tree of scans qualifies, which is the shape plan_build_from_select produces
internal B32
optimizer_flatten_from(PLAN_Node* node, OPT_Relation* relations, OPT_JoinStep* steps, U32* io_count)
{
  if (!node)
  {
    return 0;
  }
  if (*io_count >= OPT_MAX_RELATIONS)
  {
    return 0;
  }
  
  if (node->type == PLAN_NodeType_Scan)
  {
    if (!node->table)
    {
      return 0;
    }
    U32 index = *io_count;
    relations[index].scan = node;
    relations[index].table = node->table;
    relations[index].alias = node->alias;
    *io_count += 1;
    return 1;
  }
  
  if (node->type != PLAN_NodeType_Join)
  {
    return 0;
  }
  if (!node->input2 || node->input2->type != PLAN_NodeType_Scan)
  {
    return 0;
  }
  
  B32 left_ok = optimizer_flatten_from(node->input, relations, steps, io_count);
  if (!left_ok)
  {
    return 0;
  }
  
  B32 right_ok = optimizer_flatten_from(node->input2, relations, steps, io_count);
  if (!right_ok)
  {
    return 0;
  }
  
  U32 step_index = *io_count - 1;
  steps[step_index].type = node->value;
  steps[step_index].on_condition = node->condition;
  steps[step_index].null_supplying = str8_match(node->value, str8_lit("left"), StringMatchFlag_CaseInsensitive);
  return 1;
}

// tec: WHERE conjuncts first, then every ON condition. returns 0 when there are too many to track
internal U32
optimizer_load_conjuncts(IR_Node* where_ir, OPT_JoinStep* steps, U32 relation_count, OPT_Relation* relations, OPT_Conjunct* out_conjuncts)
{
  IR_Node* leaves[OPT_MAX_CONJUNCTS] = {0};
  U32 conjunct_count = 0;
  
  if (where_ir && where_ir->first)
  {
    U32 leaf_count = optimizer_ir_collect_conjuncts(where_ir->first, leaves, 0, OPT_MAX_CONJUNCTS);
    if (leaf_count > OPT_MAX_CONJUNCTS)
    {
      return 0;
    }
    for (U32 leaf_index = 0; leaf_index < leaf_count; leaf_index += 1)
    {
      out_conjuncts[conjunct_count].node = leaves[leaf_index];
      out_conjuncts[conjunct_count].from_where = 1;
      conjunct_count += 1;
    }
  }
  
  for (U32 step_index = 1; step_index < relation_count; step_index += 1)
  {
    if (!steps[step_index].on_condition)
    {
      continue;
    }
    
    U32 leaf_count = optimizer_ir_collect_conjuncts(steps[step_index].on_condition, leaves, 0, OPT_MAX_CONJUNCTS);
    if (conjunct_count + leaf_count > OPT_MAX_CONJUNCTS)
    {
      return 0;
    }
    for (U32 leaf_index = 0; leaf_index < leaf_count; leaf_index += 1)
    {
      out_conjuncts[conjunct_count].node = leaves[leaf_index];
      out_conjuncts[conjunct_count].from_where = 0;
      out_conjuncts[conjunct_count].step = step_index;
      conjunct_count += 1;
    }
  }
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    U64 mask = 0;
    B32 unresolved = 0;
    optimizer_collect_relation_refs(relations, relation_count, out_conjuncts[index].node, &mask, &unresolved);
    out_conjuncts[index].mask = mask;
    out_conjuncts[index].unresolved = unresolved;
  }
  
  return conjunct_count;
}

// tec: a WHERE condition that rejects NULL on a LEFT JOIN's right side makes that join an inner join
internal void
optimizer_convert_left_joins(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count)
{
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    OPT_Conjunct* conjunct = &conjuncts[index];
    if (!conjunct->from_where || conjunct->unresolved)
    {
      continue;
    }
    
    for (U32 relation_index = 1; relation_index < relation_count; relation_index += 1)
    {
      B32 references_relation = (conjunct->mask & ((U64)1 << relation_index)) != 0;
      if (!references_relation || !steps[relation_index].null_supplying)
      {
        continue;
      }
      
      B32 rejects = optimizer_rejects_null(relations, relation_count, conjunct->node, relation_index);
      if (rejects)
      {
        steps[relation_index].null_supplying = 0;
      }
    }
  }
}

internal void
optimizer_assign_destinations(U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count)
{
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    OPT_Conjunct* conjunct = &conjuncts[index];
    if (conjunct->destination == OPT_Destination_Dropped)
    {
      continue;
    }
    
    U32 touched = optimizer_mask_popcount(conjunct->mask);
    B32 unusable = conjunct->unresolved || touched == 0;
    
    if (conjunct->from_where)
    {
      if (unusable)
      {
        conjunct->destination = OPT_Destination_Residual;
        continue;
      }
      
      if (touched == 1)
      {
        U32 relation_index = optimizer_mask_highest(conjunct->mask);
        B32 blocked = relation_index > 0 && steps[relation_index].null_supplying;
        if (blocked)
        {
          conjunct->destination = OPT_Destination_Residual;
        }
        else
        {
          conjunct->destination = OPT_Destination_Filter;
          conjunct->target = relation_index;
        }
        continue;
      }
      
      U32 join_step = optimizer_mask_highest(conjunct->mask);
      if (steps[join_step].null_supplying)
      {
        conjunct->destination = OPT_Destination_Residual;
      }
      else
      {
        conjunct->destination = OPT_Destination_OnClause;
        conjunct->target = join_step;
      }
      continue;
    }
    
    // tec: from here on the conjunct came from the ON clause of conjunct->step
    conjunct->destination = OPT_Destination_OnClause;
    conjunct->target = conjunct->step;
    
    // tec: a constant ON condition decides whether the joined table matches at all, so it belongs on that table
    if (!conjunct->unresolved && touched == 0)
    {
      conjunct->destination = OPT_Destination_Filter;
      conjunct->target = conjunct->step;
      continue;
    }
    
    if (unusable || touched > 1)
    {
      continue;
    }
    
    U32 relation_index = optimizer_mask_highest(conjunct->mask);
    if (relation_index > conjunct->step)
    {
      continue;
    }
    
    // tec: filtering the joined table before a LEFT JOIN is safe, filtering the preserved side is not
    if (relation_index == conjunct->step)
    {
      conjunct->destination = OPT_Destination_Filter;
      conjunct->target = relation_index;
      continue;
    }
    
    B32 step_is_left = str8_match(steps[conjunct->step].type, str8_lit("left"), StringMatchFlag_CaseInsensitive);
    B32 relation_is_null_supplying = relation_index > 0 && steps[relation_index].null_supplying;
    if (!step_is_left && !relation_is_null_supplying)
    {
      conjunct->destination = OPT_Destination_Filter;
      conjunct->target = relation_index;
    }
  }
}

internal PLAN_Node*
optimizer_build_relation_plan(Arena* arena, OPT_Relation* relation, U32 relation_index, OPT_Conjunct* conjuncts, U32 conjunct_count)
{
  IR_Node* filter_nodes[OPT_CONJUNCT_CAPACITY] = {0};
  U32 filter_count = 0;
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    B32 is_for_relation = conjuncts[index].destination == OPT_Destination_Filter && conjuncts[index].target == relation_index;
    if (is_for_relation)
    {
      filter_nodes[filter_count] = conjuncts[index].node;
      filter_count += 1;
    }
  }
  
  if (filter_count == 0)
  {
    return relation->scan;
  }
  
  IR_Node* chain = optimizer_ir_and_chain(arena, filter_nodes, filter_count);
  optimizer_ir_strip_column_qualifiers(chain);
  PLAN_Node* filter = plan_node_make(arena, PLAN_NodeType_Filter);
  filter->input = relation->scan;
  filter->condition = optimizer_ir_make_where(arena, chain);
  return filter;
}

internal String8
optimizer_final_join_type(OPT_JoinStep* step, B32 has_on_conditions)
{
  if (step->null_supplying)
  {
    return str8_lit("left");
  }
  
  B32 was_cross = str8_match(step->type, str8_lit("cross"), StringMatchFlag_CaseInsensitive);
  if (was_cross && !has_on_conditions)
  {
    return str8_lit("cross");
  }
  return str8_lit("inner");
}

internal PLAN_Node*
optimizer_rewrite_from_where(Arena* arena, PLAN_Node* from_plan, IR_Node* where_ir, IR_Node* select_ir)
{
  ProfBeginFunction();
  
  OPT_Relation relations[OPT_MAX_RELATIONS] = {0};
  OPT_JoinStep steps[OPT_MAX_RELATIONS] = {0};
  U32 relation_count = 0;
  
  B32 flattened = optimizer_flatten_from(from_plan, relations, steps, &relation_count);
  if (!flattened || relation_count < 2)
  {
    ProfEnd();
    return NULL;
  }
  
  OPT_Conjunct conjuncts[OPT_CONJUNCT_CAPACITY] = {0};
  U32 conjunct_count = optimizer_load_conjuncts(where_ir, steps, relation_count, relations, conjuncts);
  
  // tec: zero is only a failure when there was something to load
  B32 had_conditions = (where_ir && where_ir->first) != 0;
  for (U32 step_index = 1; step_index < relation_count; step_index += 1)
  {
    if (steps[step_index].on_condition)
    {
      had_conditions = 1;
    }
  }
  if (conjunct_count == 0 && had_conditions)
  {
    ProfEnd();
    return NULL;
  }
  
  B32 always_empty = optimizer_fold_constants(steps, conjuncts, conjunct_count);
  if (!always_empty)
  {
    B32 constants_conflict = 0;
    conjunct_count = optimizer_propagate_constants(arena, relations, relation_count, steps, conjuncts, conjunct_count, &constants_conflict);
    always_empty = constants_conflict || optimizer_detect_contradiction(relations, relation_count, steps, conjuncts, conjunct_count);
  }
  
  optimizer_convert_left_joins(relations, relation_count, steps, conjuncts, conjunct_count);
  optimizer_assign_destinations(relation_count, steps, conjuncts, conjunct_count);
  
  // tec: the base relation is never null-supplying, so an impossible filter there empties the whole join
  if (always_empty)
  {
    OPT_Conjunct* impossible = &conjuncts[conjunct_count];
    MemoryZeroStruct(impossible);
    impossible->node = optimizer_make_constant_false(arena);
    impossible->from_where = 1;
    impossible->destination = OPT_Destination_Filter;
    impossible->target = 0;
    conjunct_count += 1;
  }
  
  // tec: the written order unless the join search found a clearly cheaper one
  U32 order[OPT_MAX_RELATIONS] = {0};
  for (U32 index = 0; index < relation_count; index += 1)
  {
    order[index] = index;
  }
  OPT_JoinGraph graph = {0};
  B32 reordered = 0;
  if (!always_empty && optimizer_join_enabled())
  {
    reordered = optimizer_plan_join_order(arena, select_ir, relations, relation_count, steps, conjuncts, conjunct_count, order, &graph);
  }
  U32 reorder_group_size = reordered ? optimizer_reorderable_group_size(steps, relation_count) : 0;
  
  U32 position_of[OPT_MAX_RELATIONS] = {0};
  for (U32 position = 0; position < relation_count; position += 1)
  {
    position_of[order[position]] = position;
  }
  
  PLAN_Node* joined = optimizer_build_relation_plan(arena, &relations[order[0]], order[0], conjuncts, conjunct_count);
  
  for (U32 position = 1; position < relation_count; position += 1)
  {
    U32 relation_index = order[position];
    IR_Node* on_nodes[OPT_CONJUNCT_CAPACITY + 1] = {0};
    U32 on_count = 0;
    for (U32 index = 0; index < conjunct_count; index += 1)
    {
      if (conjuncts[index].destination != OPT_Destination_OnClause)
      {
        continue;
      }
      U32 step_position = optimizer_step_position_for_conjunct(&conjuncts[index], position_of, reorder_group_size);
      if (step_position == position)
      {
        on_nodes[on_count] = conjuncts[index].node;
        on_count += 1;
      }
    }
    
    // tec: a relation moved next to one it was never written beside joins on the equality the two written ones imply
    B32 in_reordered_group = reordered && position < reorder_group_size;
    if (in_reordered_group)
    {
      B32 has_written_equality = 0;
      for (U32 earlier = 0; earlier < position && !has_written_equality; earlier += 1)
      {
        has_written_equality = optimizer_join_has_edge_between(&graph, relation_index, order[earlier], 1, NULL);
      }
      
      for (U32 earlier = 0; earlier < position && !has_written_equality; earlier += 1)
      {
        OPT_JoinEdge* edge = NULL;
        if (optimizer_join_has_edge_between(&graph, relation_index, order[earlier], 0, &edge))
        {
          on_nodes[on_count] = optimizer_make_derived_equality(arena, relations, edge);
          on_count += 1;
          break;
        }
      }
    }
    
    PLAN_Node* right_plan = optimizer_build_relation_plan(arena, &relations[relation_index], relation_index, conjuncts, conjunct_count);
    
    PLAN_Node* join = plan_node_make(arena, PLAN_NodeType_Join);
    join->input = joined;
    join->input2 = right_plan;
    if (in_reordered_group)
    {
      join->value = str8_lit("inner");
    }
    else
    {
      join->value = optimizer_final_join_type(&steps[position], on_count > 0);
    }
    join->condition = optimizer_ir_and_chain(arena, on_nodes, on_count);
    joined = join;
  }
  
  IR_Node* residual_nodes[OPT_CONJUNCT_CAPACITY] = {0};
  U32 residual_count = 0;
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    if (conjuncts[index].destination == OPT_Destination_Residual)
    {
      residual_nodes[residual_count] = conjuncts[index].node;
      residual_count += 1;
    }
  }
  
  PLAN_Node* top = joined;
  if (residual_count > 0)
  {
    IR_Node* chain = optimizer_ir_and_chain(arena, residual_nodes, residual_count);
    PLAN_Node* filter = plan_node_make(arena, PLAN_NodeType_Filter);
    filter->input = joined;
    filter->condition = optimizer_ir_make_where(arena, chain);
    top = filter;
  }
  
  ProfEnd();
  return top;
}

//~ tec: constant folding, contradiction and constant propagation

//~ tec: constant folding

internal B32
optimizer_number_from_literal(IR_Node* node, F64* out_value)
{
  if (!node)
  {
    return 0;
  }
  if (node->type != IR_NodeType_Numeric)
  {
    return 0;
  }
  *out_value = f64_from_str8(node->value);
  return 1;
}

internal B32
optimizer_compare_numbers(String8 op, F64 left, F64 right, B32* out_result)
{
  if (str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0))
  {
    *out_result = left == right;
    return 1;
  }
  if (str8_match(op, str8_lit("!="), 0))
  {
    *out_result = left != right;
    return 1;
  }
  if (str8_match(op, str8_lit("<"), 0))
  {
    *out_result = left < right;
    return 1;
  }
  if (str8_match(op, str8_lit("<="), 0))
  {
    *out_result = left <= right;
    return 1;
  }
  if (str8_match(op, str8_lit(">"), 0))
  {
    *out_result = left > right;
    return 1;
  }
  if (str8_match(op, str8_lit(">="), 0))
  {
    *out_result = left >= right;
    return 1;
  }
  return 0;
}

internal B32
optimizer_fold_comparison(IR_Node* node, B32* out_result)
{
  if (!node || node->type != IR_NodeType_Operator)
  {
    return 0;
  }
  
  IR_Node* left = node->first;
  IR_Node* right = left ? left->next : NULL;
  if (!left || !right)
  {
    return 0;
  }
  
  F64 left_number = 0.0;
  F64 right_number = 0.0;
  B32 left_is_number = optimizer_number_from_literal(left, &left_number);
  B32 right_is_number = optimizer_number_from_literal(right, &right_number);
  if (left_is_number && right_is_number)
  {
    return optimizer_compare_numbers(node->value, left_number, right_number, out_result);
  }
  
  B32 both_strings = left->type == IR_NodeType_Literal && right->type == IR_NodeType_Literal;
  if (both_strings)
  {
    B32 same = str8_match(left->value, right->value, 0);
    if (str8_match(node->value, str8_lit("="), 0) || str8_match(node->value, str8_lit("=="), 0))
    {
      *out_result = same;
      return 1;
    }
    if (str8_match(node->value, str8_lit("!="), 0))
    {
      *out_result = !same;
      return 1;
    }
  }
  return 0;
}

internal IR_Node*
optimizer_make_constant_false(Arena* arena)
{
  IR_Node* comparison = ir_node_make(arena, IR_NodeType_Operator, str8_lit("="));
  IR_Node* zero = ir_node_make(arena, IR_NodeType_Numeric, str8_lit("0"));
  IR_Node* one = ir_node_make(arena, IR_NodeType_Numeric, str8_lit("1"));
  ir_node_add_child(comparison, zero);
  ir_node_add_child(comparison, one);
  return comparison;
}

internal B32
optimizer_ir_is_constant_false(IR_Node* root)
{
  if (!root)
  {
    return 0;
  }
  
  if (optimizer_ir_is_operator(root, str8_lit("and")))
  {
    IR_Node* left = root->first;
    IR_Node* right = left ? left->next : NULL;
    B32 left_false = optimizer_ir_is_constant_false(left);
    B32 right_false = optimizer_ir_is_constant_false(right);
    return left_false || right_false;
  }
  
  B32 result = 1;
  B32 folded = optimizer_fold_comparison(root, &result);
  return folded && !result;
}

internal B32
optimizer_fold_constants(OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count)
{
  B32 always_empty = 0;
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    OPT_Conjunct* conjunct = &conjuncts[index];
    B32 result = 0;
    B32 folded = optimizer_fold_comparison(conjunct->node, &result);
    if (!folded)
    {
      continue;
    }
    
    if (result)
    {
      conjunct->destination = OPT_Destination_Dropped;
      continue;
    }
    
    // tec: a false ON condition on a LEFT JOIN still keeps every left row
    B32 on_left_join = !conjunct->from_where && steps[conjunct->step].null_supplying;
    if (!on_left_join)
    {
      always_empty = 1;
    }
  }
  
  return always_empty;
}

//~ tec: contradiction detection

internal B32
optimizer_column_type_is_comparable_number(GDB_ColumnType type)
{
  B32 comparable = type == GDB_ColumnType_U32 ||
    type == GDB_ColumnType_U64 ||
    type == GDB_ColumnType_I32 ||
    type == GDB_ColumnType_I64 ||
    type == GDB_ColumnType_F64 ||
    type == GDB_ColumnType_Bool;
  return comparable;
}

internal B32
optimizer_column_operand(OPT_Relation* relations, U32 relation_count, IR_Node* node, U32* out_relation, String8* out_column, GDB_ColumnType* out_type)
{
  if (!node || node->type != IR_NodeType_Column)
  {
    return 0;
  }
  
  U32 relation_index = optimizer_resolve_column(relations, relation_count, node->value);
  if (relation_index == OPT_NO_RELATION)
  {
    return 0;
  }
  
  String8 bare_name = qe_bare_column_name(node->value);
  GDB_Column* column = gdb_table_find_column(relations[relation_index].table, bare_name);
  if (!column)
  {
    return 0;
  }
  
  *out_relation = relation_index;
  *out_column = bare_name;
  *out_type = column->type;
  return 1;
}

internal String8
optimizer_flip_comparison(String8 op)
{
  if (str8_match(op, str8_lit("<"), 0))
  {
    return str8_lit(">");
  }
  if (str8_match(op, str8_lit(">"), 0))
  {
    return str8_lit("<");
  }
  if (str8_match(op, str8_lit("<="), 0))
  {
    return str8_lit(">=");
  }
  if (str8_match(op, str8_lit(">="), 0))
  {
    return str8_lit("<=");
  }
  return op;
}

internal OPT_ColumnBounds*
optimizer_find_bounds(OPT_ColumnBounds* bounds, U32* io_count, U32 relation, String8 column)
{
  for (U32 index = 0; index < *io_count; index += 1)
  {
    B32 same_relation = bounds[index].relation == relation;
    B32 same_column = str8_match(bounds[index].column, column, StringMatchFlag_CaseInsensitive);
    if (same_relation && same_column)
    {
      return &bounds[index];
    }
  }
  
  if (*io_count >= OPT_MAX_CONJUNCTS)
  {
    return NULL;
  }
  
  OPT_ColumnBounds* created = &bounds[*io_count];
  *io_count += 1;
  created->relation = relation;
  created->column = column;
  return created;
}

internal B32
optimizer_bounds_are_contradictory(OPT_ColumnBounds* bounds)
{
  if (bounds->has_lower && bounds->has_upper)
  {
    if (bounds->lower > bounds->upper)
    {
      return 1;
    }
    if (bounds->lower == bounds->upper && (bounds->lower_strict || bounds->upper_strict))
    {
      return 1;
    }
  }
  
  if (bounds->has_equal && bounds->has_lower)
  {
    if (bounds->equal < bounds->lower)
    {
      return 1;
    }
    if (bounds->equal == bounds->lower && bounds->lower_strict)
    {
      return 1;
    }
  }
  
  if (bounds->has_equal && bounds->has_upper)
  {
    if (bounds->equal > bounds->upper)
    {
      return 1;
    }
    if (bounds->equal == bounds->upper && bounds->upper_strict)
    {
      return 1;
    }
  }
  
  return 0;
}

// tec: returns true when this comparison makes the bounds impossible
internal B32
optimizer_bounds_apply(OPT_ColumnBounds* bounds, String8 op, IR_Node* literal, GDB_ColumnType type)
{
  B32 is_equal_op = str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0);
  
  if (type == GDB_ColumnType_String8)
  {
    if (literal->type != IR_NodeType_Literal || !is_equal_op)
    {
      return 0;
    }
    if (bounds->has_equal_string && !str8_match(bounds->equal_string, literal->value, 0))
    {
      return 1;
    }
    bounds->has_equal_string = 1;
    bounds->equal_string = literal->value;
    return 0;
  }
  
  F64 value = 0.0;
  if (!optimizer_number_from_literal(literal, &value))
  {
    return 0;
  }
  
  if (is_equal_op)
  {
    if (bounds->has_equal && bounds->equal != value)
    {
      return 1;
    }
    bounds->has_equal = 1;
    bounds->equal = value;
  }
  else if (str8_match(op, str8_lit(">"), 0))
  {
    if (!bounds->has_lower || value >= bounds->lower)
    {
      bounds->has_lower = 1;
      bounds->lower = value;
      bounds->lower_strict = 1;
    }
  }
  else if (str8_match(op, str8_lit(">="), 0))
  {
    if (!bounds->has_lower || value > bounds->lower)
    {
      bounds->has_lower = 1;
      bounds->lower = value;
      bounds->lower_strict = 0;
    }
  }
  else if (str8_match(op, str8_lit("<"), 0))
  {
    if (!bounds->has_upper || value <= bounds->upper)
    {
      bounds->has_upper = 1;
      bounds->upper = value;
      bounds->upper_strict = 1;
    }
  }
  else if (str8_match(op, str8_lit("<="), 0))
  {
    if (!bounds->has_upper || value < bounds->upper)
    {
      bounds->has_upper = 1;
      bounds->upper = value;
      bounds->upper_strict = 0;
    }
  }
  
  return optimizer_bounds_are_contradictory(bounds);
}

internal B32
optimizer_detect_contradiction(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count)
{
  OPT_ColumnBounds bounds[OPT_MAX_CONJUNCTS] = {0};
  U32 bounds_count = 0;
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    OPT_Conjunct* conjunct = &conjuncts[index];
    if (!conjunct->from_where || conjunct->unresolved || conjunct->destination == OPT_Destination_Dropped)
    {
      continue;
    }
    if (optimizer_mask_popcount(conjunct->mask) != 1)
    {
      continue;
    }
    
    IR_Node* node = conjunct->node;
    if (node->type != IR_NodeType_Operator || !node->first || !node->first->next)
    {
      continue;
    }
    
    IR_Node* column_node = node->first;
    IR_Node* literal_node = node->first->next;
    String8 op = node->value;
    if (column_node->type != IR_NodeType_Column)
    {
      column_node = node->first->next;
      literal_node = node->first;
      op = optimizer_flip_comparison(op);
    }
    
    U32 relation_index = 0;
    String8 column_name = {0};
    GDB_ColumnType column_type = GDB_ColumnType_Invalid;
    B32 resolved = optimizer_column_operand(relations, relation_count, column_node, &relation_index, &column_name, &column_type);
    if (!resolved)
    {
      continue;
    }
    if (relation_index > 0 && steps[relation_index].null_supplying)
    {
      continue;
    }
    
    B32 string_pair = column_type == GDB_ColumnType_String8 && literal_node->type == IR_NodeType_Literal;
    B32 number_pair = optimizer_column_type_is_comparable_number(column_type) && literal_node->type == IR_NodeType_Numeric;
    if (!string_pair && !number_pair)
    {
      continue;
    }
    
    OPT_ColumnBounds* entry = optimizer_find_bounds(bounds, &bounds_count, relation_index, column_name);
    if (!entry)
    {
      continue;
    }
    
    if (optimizer_bounds_apply(entry, op, literal_node, column_type))
    {
      return 1;
    }
  }
  
  return 0;
}

//~ tec: constant propagation across equality joins

internal U32
optimizer_column_key_index(OPT_ColumnKey* keys, U32* io_count, U32 relation, String8 column)
{
  for (U32 index = 0; index < *io_count; index += 1)
  {
    B32 same_relation = keys[index].relation == relation;
    B32 same_column = str8_match(keys[index].column, column, StringMatchFlag_CaseInsensitive);
    if (same_relation && same_column)
    {
      return index;
    }
  }
  
  if (*io_count >= OPT_MAX_COLUMN_KEYS)
  {
    return OPT_NO_RELATION;
  }
  
  U32 created = *io_count;
  *io_count += 1;
  keys[created].relation = relation;
  keys[created].column = column;
  keys[created].parent = created;
  return created;
}

internal U32
optimizer_column_key_root(OPT_ColumnKey* keys, U32 index)
{
  U32 root = index;
  while (keys[root].parent != root)
  {
    root = keys[root].parent;
  }
  return root;
}

internal B32
optimizer_equality_is_join_key(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjunct, U32* out_left_relation, String8* out_left_column, U32* out_right_relation, String8* out_right_column)
{
  if (conjunct->unresolved || conjunct->destination == OPT_Destination_Dropped)
  {
    return 0;
  }
  
  B32 on_left_join = !conjunct->from_where && steps[conjunct->step].null_supplying;
  if (on_left_join)
  {
    return 0;
  }
  
  IR_Node* node = conjunct->node;
  B32 is_equality = optimizer_ir_is_operator(node, str8_lit("=")) || optimizer_ir_is_operator(node, str8_lit("=="));
  if (!is_equality || !node->first || !node->first->next)
  {
    return 0;
  }
  
  GDB_ColumnType left_type = GDB_ColumnType_Invalid;
  GDB_ColumnType right_type = GDB_ColumnType_Invalid;
  B32 left_ok = optimizer_column_operand(relations, relation_count, node->first, out_left_relation, out_left_column, &left_type);
  B32 right_ok = optimizer_column_operand(relations, relation_count, node->first->next, out_right_relation, out_right_column, &right_type);
  if (!left_ok || !right_ok || *out_left_relation == *out_right_relation)
  {
    return 0;
  }
  
  B32 left_null_supplying = *out_left_relation > 0 && steps[*out_left_relation].null_supplying;
  B32 right_null_supplying = *out_right_relation > 0 && steps[*out_right_relation].null_supplying;
  if (left_null_supplying || right_null_supplying)
  {
    return 0;
  }
  
  B32 both_numbers = optimizer_column_type_is_comparable_number(left_type) && optimizer_column_type_is_comparable_number(right_type);
  B32 both_strings = left_type == GDB_ColumnType_String8 && right_type == GDB_ColumnType_String8;
  return both_numbers || both_strings;
}

internal B32
optimizer_constant_equality(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjunct, U32* out_relation, String8* out_column, IR_Node** out_literal)
{
  if (!conjunct->from_where || conjunct->unresolved || conjunct->destination == OPT_Destination_Dropped)
  {
    return 0;
  }
  
  IR_Node* node = conjunct->node;
  B32 is_equality = optimizer_ir_is_operator(node, str8_lit("=")) || optimizer_ir_is_operator(node, str8_lit("=="));
  if (!is_equality || !node->first || !node->first->next)
  {
    return 0;
  }
  
  IR_Node* column_node = node->first;
  IR_Node* literal_node = node->first->next;
  if (column_node->type != IR_NodeType_Column)
  {
    column_node = node->first->next;
    literal_node = node->first;
  }
  
  GDB_ColumnType column_type = GDB_ColumnType_Invalid;
  B32 resolved = optimizer_column_operand(relations, relation_count, column_node, out_relation, out_column, &column_type);
  if (!resolved)
  {
    return 0;
  }
  if (*out_relation > 0 && steps[*out_relation].null_supplying)
  {
    return 0;
  }
  
  B32 string_pair = column_type == GDB_ColumnType_String8 && literal_node->type == IR_NodeType_Literal;
  B32 number_pair = optimizer_column_type_is_comparable_number(column_type) && literal_node->type == IR_NodeType_Numeric;
  if (!string_pair && !number_pair)
  {
    return 0;
  }
  
  *out_literal = literal_node;
  return 1;
}

internal B32
optimizer_constants_conflict(IR_Node* first, IR_Node* second)
{
  if (first->type != second->type)
  {
    return 0;
  }
  
  if (first->type == IR_NodeType_Numeric)
  {
    F64 first_number = f64_from_str8(first->value);
    F64 second_number = f64_from_str8(second->value);
    return first_number != second_number;
  }
  
  if (first->type == IR_NodeType_Literal)
  {
    return !str8_match(first->value, second->value, 0);
  }
  return 0;
}

internal U32
optimizer_propagate_constants(Arena* arena, OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count, B32* out_conflict)
{
  OPT_ColumnKey keys[OPT_MAX_COLUMN_KEYS] = {0};
  U32 key_count = 0;
  *out_conflict = 0;
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    U32 left_relation = 0;
    U32 right_relation = 0;
    String8 left_column = {0};
    String8 right_column = {0};
    B32 is_join_key = optimizer_equality_is_join_key(relations, relation_count, steps, &conjuncts[index], &left_relation, &left_column, &right_relation, &right_column);
    if (!is_join_key)
    {
      continue;
    }
    
    U32 left_key = optimizer_column_key_index(keys, &key_count, left_relation, left_column);
    U32 right_key = optimizer_column_key_index(keys, &key_count, right_relation, right_column);
    if (left_key == OPT_NO_RELATION || right_key == OPT_NO_RELATION)
    {
      continue;
    }
    
    U32 left_root = optimizer_column_key_root(keys, left_key);
    U32 right_root = optimizer_column_key_root(keys, right_key);
    if (left_root != right_root)
    {
      keys[right_root].parent = left_root;
    }
  }
  
  if (key_count == 0)
  {
    return conjunct_count;
  }
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    U32 relation_index = 0;
    String8 column_name = {0};
    IR_Node* literal = NULL;
    B32 is_constant = optimizer_constant_equality(relations, relation_count, steps, &conjuncts[index], &relation_index, &column_name, &literal);
    if (!is_constant)
    {
      continue;
    }
    
    // tec: a column that only has a constant and no join partner is not in the key table, and does not need to be
    for (U32 key_index = 0; key_index < key_count; key_index += 1)
    {
      B32 same_relation = keys[key_index].relation == relation_index;
      B32 same_column = str8_match(keys[key_index].column, column_name, StringMatchFlag_CaseInsensitive);
      if (same_relation && same_column && !keys[key_index].constant)
      {
        keys[key_index].constant = literal;
      }
    }
  }
  
  IR_Node* class_constants[OPT_MAX_COLUMN_KEYS] = {0};
  for (U32 key_index = 0; key_index < key_count; key_index += 1)
  {
    U32 root = optimizer_column_key_root(keys, key_index);
    if (keys[key_index].constant && !class_constants[root])
    {
      class_constants[root] = keys[key_index].constant;
    }
  }
  
  for (U32 key_index = 0; key_index < key_count; key_index += 1)
  {
    U32 root = optimizer_column_key_root(keys, key_index);
    IR_Node* class_constant = class_constants[root];
    if (keys[key_index].constant && class_constant)
    {
      if (optimizer_constants_conflict(keys[key_index].constant, class_constant))
      {
        *out_conflict = 1;
      }
    }
  }
  
  for (U32 key_index = 0; key_index < key_count; key_index += 1)
  {
    U32 root = optimizer_column_key_root(keys, key_index);
    IR_Node* constant = class_constants[root];
    if (keys[key_index].constant || !constant)
    {
      continue;
    }
    if (conjunct_count >= OPT_MAX_CONJUNCTS)
    {
      break;
    }
    
    OPT_Relation* relation = &relations[keys[key_index].relation];
    GDB_Column* column = gdb_table_find_column(relation->table, keys[key_index].column);
    if (!column)
    {
      continue;
    }
    B32 string_pair = column->type == GDB_ColumnType_String8 && constant->type == IR_NodeType_Literal;
    B32 number_pair = optimizer_column_type_is_comparable_number(column->type) && constant->type == IR_NodeType_Numeric;
    if (!string_pair && !number_pair)
    {
      continue;
    }
    
    String8 qualifier = relation->alias.size ? relation->alias : relation->table->name;
    String8 qualified_name = push_str8f(arena, "%.*s.%.*s", str8_varg(qualifier), str8_varg(keys[key_index].column));
    
    IR_Node* comparison = ir_node_make(arena, IR_NodeType_Operator, str8_lit("="));
    IR_Node* column_node = ir_node_make(arena, IR_NodeType_Column, qualified_name);
    IR_Node* literal_node = optimizer_ir_clone(arena, constant);
    ir_node_add_child(comparison, column_node);
    ir_node_add_child(comparison, literal_node);
    
    OPT_Conjunct* derived = &conjuncts[conjunct_count];
    MemoryZeroStruct(derived);
    derived->node = comparison;
    derived->mask = (U64)1 << keys[key_index].relation;
    derived->from_where = 1;
    conjunct_count += 1;
  }
  
  return conjunct_count;
}

//~ tec: CTE / derived table inlining

internal B32
optimizer_inline_enabled(void)
{
  B32 master = settings_bool(str8_lit("QE_OPTIMIZER"), 1);
  B32 inline_sources = settings_bool(str8_lit("QE_OPT_INLINE"), 1);
  return master && inline_sources;
}

//~ tec: IR list edits and walks

internal void
optimizer_ir_unlink(IR_Node* node)
{
  IR_Node* parent = node->parent;
  
  if (node->prev)
  {
    node->prev->next = node->next;
  }
  else if (parent)
  {
    parent->first = node->next;
  }
  
  if (node->next)
  {
    node->next->prev = node->prev;
  }
  else if (parent)
  {
    parent->last = node->prev;
  }
  
  node->prev = NULL;
  node->next = NULL;
  node->parent = NULL;
}

internal U32
optimizer_count_table_references(IR_Node* node, String8 name)
{
  if (!node)
  {
    return 0;
  }
  
  U32 count = 0;
  if (node->type == IR_NodeType_Table && str8_match(node->value, name, StringMatchFlag_CaseInsensitive))
  {
    count += 1;
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    count += optimizer_count_table_references(child, name);
  }
  return count;
}

internal U32
optimizer_count_from_relations(IR_Node* select_ir)
{
  U32 count = 0;
  for (IR_Node* child = select_ir->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_Table || child->type == IR_NodeType_Join)
    {
      count += 1;
    }
  }
  return count;
}

internal B32
optimizer_ir_is_plain_predicate(IR_Node* node)
{
  if (!node)
  {
    return 1;
  }
  
  switch (node->type)
  {
    case IR_NodeType_Column:
    case IR_NodeType_Numeric:
    case IR_NodeType_Literal:
    case IR_NodeType_Operator:
    case IR_NodeType_InList:
    {
    } break;
    
    case IR_NodeType_AggregateCall:
    {
      if (!qe_ir_is_fuzzy_call(node, 0))
      {
        return 0;
      }
    } break;
    
    default:
    {
      return 0;
    }
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    if (!optimizer_ir_is_plain_predicate(child))
    {
      return 0;
    }
  }
  return 1;
}

internal B32
optimizer_is_bare_star(IR_Node* column_list)
{
  if (!column_list || !column_list->first)
  {
    return 0;
  }
  if (column_list->first != column_list->last)
  {
    return 0;
  }
  IR_Node* item = column_list->first;
  return item->type == IR_NodeType_Column && str8_match(item->value, str8_lit("*"), 0);
}

//~ tec: deciding whether a source can be inlined

internal B32
optimizer_list_has_column(IR_Node* column_list, String8 bare_name)
{
  for (IR_Node* item = column_list->first; item != NULL; item = item->next)
  {
    String8 item_name = qe_bare_column_name(item->value);
    if (str8_match(item_name, bare_name, StringMatchFlag_CaseInsensitive))
    {
      return 1;
    }
  }
  return 0;
}

internal B32
optimizer_inner_columns_belong_to_base(IR_Node* node, GDB_Table* base_table, String8 inner_alias)
{
  if (!node)
  {
    return 1;
  }
  
  if (node->type == IR_NodeType_Column)
  {
    return qe_column_belongs_to_table(base_table, inner_alias, node->value);
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    if (!optimizer_inner_columns_belong_to_base(child, base_table, inner_alias))
    {
      return 0;
    }
  }
  return 1;
}

internal B32
optimizer_describe_inline_source(GDB_Database* database, IR_Node* inner_select, OPT_InlineSource* out_source)
{
  MemoryZeroStruct(out_source);
  out_source->select = inner_select;
  
  U32 table_count = 0;
  for (IR_Node* child = inner_select->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_ColumnList)
    {
      out_source->column_list = child;
    }
    else if (child->type == IR_NodeType_Table)
    {
      out_source->table = child;
      table_count += 1;
    }
    else if (child->type == IR_NodeType_Where)
    {
      out_source->where = child;
    }
    else
    {
      return 0;
    }
  }
  
  if (table_count != 1 || !out_source->column_list || !out_source->column_list->first)
  {
    return 0;
  }
  if (ir_node_find_child(out_source->table, IR_NodeType_Select))
  {
    return 0;
  }
  
  GDB_Table* base_table = gdb_database_find_table(database, out_source->table->value);
  GDB_Table* visible_table = gdb_database_find_table_or_catalog(database, out_source->table->value);
  if (!base_table || base_table != visible_table)
  {
    return 0;
  }
  out_source->base_table = base_table;
  
  out_source->is_star = optimizer_is_bare_star(out_source->column_list);
  if (!out_source->is_star)
  {
    for (IR_Node* item = out_source->column_list->first; item != NULL; item = item->next)
    {
      B32 plain_column = item->type == IR_NodeType_Column && item->first == NULL;
      if (!plain_column || !gdb_table_find_column(base_table, qe_bare_column_name(item->value)))
      {
        return 0;
      }
    }
  }
  
  if (out_source->where && out_source->where->first)
  {
    IR_Node* root = out_source->where->first;
    String8 inner_alias = plan_alias_from_table_ir(out_source->table);
    if (!optimizer_ir_is_plain_predicate(root))
    {
      return 0;
    }
    if (!optimizer_inner_columns_belong_to_base(root, base_table, inner_alias))
    {
      return 0;
    }
  }
  
  return 1;
}

internal B32
optimizer_column_reference_blocks(IR_Node* node, OPT_InlineSource* source, String8 exposed_name)
{
  if (!node)
  {
    return 0;
  }
  
  if (node->type == IR_NodeType_Column)
  {
    if (source->is_star || str8_match(node->value, str8_lit("*"), 0))
    {
      return 0;
    }
    
    String8 bare_name = qe_bare_column_name(node->value);
    B32 is_qualified = bare_name.size != node->value.size;
    if (is_qualified)
    {
      String8 qualifier = str8_prefix(node->value, node->value.size - bare_name.size - 1);
      B32 names_source = str8_match(qualifier, exposed_name, StringMatchFlag_CaseInsensitive);
      return names_source && !optimizer_list_has_column(source->column_list, bare_name);
    }
    
    B32 hidden_base_column = gdb_table_find_column(source->base_table, bare_name) != NULL && !optimizer_list_has_column(source->column_list, bare_name);
    return hidden_base_column;
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    if (optimizer_column_reference_blocks(child, source, exposed_name))
    {
      return 1;
    }
  }
  return 0;
}

// tec: inlining exposes every column of the base table, so a name the source hid must not start resolving to it
internal B32
optimizer_outer_blocks_inline(IR_Node* outer_select, OPT_InlineSource* source, String8 exposed_name)
{
  IR_Node* outer_list = ir_node_find_child(outer_select, IR_NodeType_ColumnList);
  if (optimizer_is_bare_star(outer_list) && !source->is_star)
  {
    return 1;
  }
  
  for (IR_Node* child = outer_select->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_Table || child->type == IR_NodeType_CteList)
    {
      continue;
    }
    
    if (child->type == IR_NodeType_Join)
    {
      for (IR_Node* join_child = child->first ? child->first->next : NULL; join_child != NULL; join_child = join_child->next)
      {
        if (optimizer_column_reference_blocks(join_child, source, exposed_name))
        {
          return 1;
        }
      }
      continue;
    }
    
    if (optimizer_column_reference_blocks(child, source, exposed_name))
    {
      return 1;
    }
  }
  return 0;
}

//~ tec: rewriting

internal IR_Node*
optimizer_clone_with_alias(Arena* arena, IR_Node* node, String8 exposed_name, B32 qualify)
{
  IR_Node* copy = ir_node_make(arena, node->type, node->value);
  
  if (node->type == IR_NodeType_Column)
  {
    String8 bare_name = qe_bare_column_name(node->value);
    if (qualify)
    {
      copy->value = push_str8f(arena, "%.*s.%.*s", str8_varg(exposed_name), str8_varg(bare_name));
    }
    else
    {
      copy->value = bare_name;
    }
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    IR_Node* child_copy = optimizer_clone_with_alias(arena, child, exposed_name, qualify);
    ir_node_add_child(copy, child_copy);
  }
  return copy;
}

internal void
optimizer_merge_where(Arena* arena, IR_Node* outer_select, IR_Node* condition)
{
  IR_Node* where = ir_node_find_child(outer_select, IR_NodeType_Where);
  if (!where)
  {
    where = ir_node_make(arena, IR_NodeType_Where, (String8){0});
    ir_node_add_child(outer_select, where);
  }
  
  if (!where->first)
  {
    ir_node_add_child(where, condition);
    return;
  }
  
  IR_Node* outer_root = where->first;
  where->first = NULL;
  where->last = NULL;
  outer_root->parent = NULL;
  outer_root->prev = NULL;
  outer_root->next = NULL;
  
  IR_Node* and_node = ir_node_make(arena, IR_NodeType_Operator, str8_lit("and"));
  ir_node_add_child(and_node, outer_root);
  ir_node_add_child(and_node, condition);
  ir_node_add_child(where, and_node);
}

internal void
optimizer_inline_source(Arena* arena, IR_Node* outer_select, IR_Node* table_ir, OPT_InlineSource* source, String8 exposed_name, B32 qualify)
{
  table_ir->value = source->table->value;
  
  IR_Node* alias = ir_node_find_child(table_ir, IR_NodeType_Alias);
  if (!alias)
  {
    alias = ir_node_make(arena, IR_NodeType_Alias, exposed_name);
  }
  alias->prev = NULL;
  alias->next = NULL;
  alias->parent = table_ir;
  table_ir->first = alias;
  table_ir->last = alias;
  
  if (source->where && source->where->first)
  {
    IR_Node* condition = optimizer_clone_with_alias(arena, source->where->first, exposed_name, qualify);
    optimizer_merge_where(arena, outer_select, condition);
  }
}

internal B32
optimizer_source_is_in_left_join(IR_Node* outer_select, IR_Node* table_ir)
{
  IR_Node* parent = table_ir->parent;
  if (!parent || parent == outer_select || parent->type != IR_NodeType_Join)
  {
    return 0;
  }
  return str8_match(parent->value, str8_lit("left"), StringMatchFlag_CaseInsensitive);
}

internal B32
optimizer_try_inline_reference(Arena* arena, GDB_Database* database, IR_Node* select_ir, IR_Node* table_ir, IR_Node* inner_select, String8 default_name)
{
  OPT_InlineSource source = {0};
  if (!optimizer_describe_inline_source(database, inner_select, &source))
  {
    return 0;
  }
  
  IR_Node* alias = ir_node_find_child(table_ir, IR_NodeType_Alias);
  String8 exposed_name = alias ? alias->value : default_name;
  
  // tec: a left joined source with its own filter would lose rows that should be kept as NULL rows
  B32 has_filter = source.where && source.where->first;
  if (has_filter && optimizer_source_is_in_left_join(select_ir, table_ir))
  {
    return 0;
  }
  if (optimizer_outer_blocks_inline(select_ir, &source, exposed_name))
  {
    return 0;
  }
  
  B32 qualify = optimizer_count_from_relations(select_ir) > 1;
  optimizer_inline_source(arena, select_ir, table_ir, &source, exposed_name, qualify);
  return 1;
}

internal IR_Node*
optimizer_find_from_reference(IR_Node* select_ir, String8 name)
{
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
    
    if (!table_ir || ir_node_find_child(table_ir, IR_NodeType_Select))
    {
      continue;
    }
    if (str8_match(table_ir->value, name, StringMatchFlag_CaseInsensitive))
    {
      return table_ir;
    }
  }
  return NULL;
}

internal B32
optimizer_name_is_cte(IR_Node* cte_list, String8 name)
{
  if (!cte_list)
  {
    return 0;
  }
  for (IR_Node* cte = cte_list->first; cte != NULL; cte = cte->next)
  {
    if (str8_match(cte->value, name, StringMatchFlag_CaseInsensitive))
    {
      return 1;
    }
  }
  return 0;
}

internal void
optimizer_inline_sources(Arena* arena, GDB_Database* database, IR_Node* select_ir)
{
  if (!select_ir)
  {
    return;
  }
  
  IR_Node* cte_list = ir_node_find_child(select_ir, IR_NodeType_CteList);
  if (cte_list)
  {
    IR_Node* cte = cte_list->first;
    while (cte)
    {
      IR_Node* next_cte = cte->next;
      U32 references = optimizer_count_table_references(select_ir, cte->value);
      
      if (references == 0)
      {
        optimizer_ir_unlink(cte);
      }
      else if (references == 1 && cte->first)
      {
        IR_Node* table_ir = optimizer_find_from_reference(select_ir, cte->value);
        IR_Node* inner_select = cte->first;
        IR_Node* inner_table = ir_node_find_child(inner_select, IR_NodeType_Table);
        
        // tec: a CTE reading another CTE stays materialized, the base table check cannot see through it
        B32 reads_cte = inner_table && optimizer_name_is_cte(cte_list, inner_table->value);
        if (table_ir && !reads_cte)
        {
          if (optimizer_try_inline_reference(arena, database, select_ir, table_ir, inner_select, cte->value))
          {
            optimizer_ir_unlink(cte);
          }
        }
      }
      
      cte = next_cte;
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
    
    IR_Node* inner_table = ir_node_find_child(inner_select, IR_NodeType_Table);
    B32 reads_cte = inner_table && optimizer_name_is_cte(cte_list, inner_table->value);
    if (!reads_cte)
    {
      optimizer_try_inline_reference(arena, database, select_ir, table_ir, inner_select, alias->value);
    }
  }
}

//~ tec: selectivity and cardinality estimation

//~ tec: statistics lookups

internal F64
optimizer_clamp_selectivity(F64 selectivity)
{
  if (selectivity < 0.0)
  {
    return 0.0;
  }
  if (selectivity > 1.0)
  {
    return 1.0;
  }
  return selectivity;
}

// tec: computing statistics reads the whole column, so a column over the row budget only gets them once something else computed them
internal B32
optimizer_stats_available(GDB_Column* column)
{
  if (gdb_column_stats_is_current(column))
  {
    return 1;
  }
  
  U64 max_rows = settings_u64(str8_lit("QE_OPT_STATS_MAX_ROWS"), 4000000);
  if (column->row_count > max_rows)
  {
    return 0;
  }
  gdb_column_ensure_stats(column);
  return 1;
}

internal F64
optimizer_non_null_fraction(GDB_Column* column)
{
  if (!optimizer_stats_available(column))
  {
    return 1.0;
  }
  GDB_ColumnStats* stats = &column->stats;
  if (stats->row_count == 0)
  {
    return 0.0;
  }
  F64 null_fraction = (F64)stats->null_count / (F64)stats->row_count;
  return 1.0 - null_fraction;
}

internal F64
optimizer_distinct_count(GDB_Column* column)
{
  if (!optimizer_stats_available(column))
  {
    return Max(1.0, (F64)column->row_count);
  }
  F64 distinct = (F64)column->stats.distinct_count;
  if (distinct < 1.0)
  {
    return 1.0;
  }
  return distinct;
}

//~ tec: single predicate selectivity

// tec: a share of the non-null rows, a top value is looked up directly and the rest share what is left evenly
internal F64
optimizer_equality_selectivity(GDB_Column* column, B32 is_string, F64 target_numeric, String8 target_string)
{
  if (!optimizer_stats_available(column))
  {
    return OPT_DEFAULT_SELECTIVITY_EQUALITY;
  }
  GDB_ColumnStats* stats = &column->stats;
  if (stats->row_count <= stats->null_count)
  {
    return 0.0;
  }
  
  if (!is_string && stats->has_range)
  {
    if (target_numeric < stats->min_value || target_numeric > stats->max_value)
    {
      return 0.0;
    }
  }
  
  U64 key = 0;
  if (is_string)
  {
    key = gdb_stats_hash_bytes(target_string.str, target_string.size);
  }
  else
  {
    key = gdb_stats_hash_f64(target_numeric);
  }
  
  F64 top_value_total = 0.0;
  for (U32 index = 0; index < stats->mcv_count; index += 1)
  {
    if (stats->mcv[index].key == key)
    {
      return stats->mcv[index].fraction;
    }
    top_value_total += stats->mcv[index].fraction;
  }
  
  F64 remaining_distinct = (F64)stats->distinct_count - (F64)stats->mcv_count;
  if (remaining_distinct < 1.0)
  {
    remaining_distinct = 1.0;
  }
  F64 remaining_fraction = 1.0 - top_value_total;
  if (remaining_fraction < 0.0)
  {
    remaining_fraction = 0.0;
  }
  return remaining_fraction / remaining_distinct;
}

// tec: returns a negative number when the column has no histogram
internal F64
optimizer_histogram_fraction_below(GDB_ColumnStats* stats, F64 value)
{
  U32 bucket_count = stats->histogram_bucket_count;
  if (bucket_count == 0)
  {
    return -1.0;
  }
  
  F64* bounds = stats->histogram_bounds;
  if (value <= bounds[0])
  {
    return 0.0;
  }
  if (value > bounds[bucket_count])
  {
    return 1.0;
  }
  
  for (U32 bucket = 0; bucket < bucket_count; bucket += 1)
  {
    if (value <= bounds[bucket + 1])
    {
      F64 width = bounds[bucket + 1] - bounds[bucket];
      F64 position = 0.5;
      if (width > 0.0)
      {
        position = (value - bounds[bucket]) / width;
      }
      return ((F64)bucket + position) / (F64)bucket_count;
    }
  }
  return 1.0;
}

internal F64
optimizer_range_selectivity(GDB_Column* column, B32 is_lt, B32 is_le, B32 is_gt, B32 is_ge, F64 target_numeric)
{
  F64 non_null = optimizer_non_null_fraction(column);
  if (!optimizer_stats_available(column))
  {
    return OPT_DEFAULT_SELECTIVITY_RANGE * non_null;
  }
  F64 below = optimizer_histogram_fraction_below(&column->stats, target_numeric);
  if (below < 0.0)
  {
    return OPT_DEFAULT_SELECTIVITY_RANGE * non_null;
  }
  
  F64 equal = optimizer_equality_selectivity(column, 0, target_numeric, (String8){0});
  F64 fraction = 0.0;
  if (is_lt)
  {
    fraction = below;
  }
  else if (is_le)
  {
    fraction = below + equal;
  }
  else if (is_gt)
  {
    fraction = 1.0 - below - equal;
  }
  else if (is_ge)
  {
    fraction = 1.0 - below;
  }
  return optimizer_clamp_selectivity(fraction) * non_null;
}

// tec: both nodes are cloned, so the source tree keeps its parent and sibling links
internal IR_Node*
optimizer_make_comparison(Arena* arena, String8 op, IR_Node* column_node, IR_Node* literal_node)
{
  IR_Node* comparison = ir_node_make(arena, IR_NodeType_Operator, op);
  IR_Node* column_copy = optimizer_ir_clone(arena, column_node);
  IR_Node* literal_copy = optimizer_ir_clone(arena, literal_node);
  ir_node_add_child(comparison, column_copy);
  ir_node_add_child(comparison, literal_copy);
  return comparison;
}

internal F64
optimizer_comparison_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf)
{
  IR_Node* left = leaf->first;
  IR_Node* right = left ? left->next : NULL;
  if (!left || !right)
  {
    return OPT_DEFAULT_SELECTIVITY;
  }
  
  String8 op = leaf->value;
  IR_Node* column_node = left;
  IR_Node* literal_node = right;
  if (left->type != IR_NodeType_Column && right->type == IR_NodeType_Column)
  {
    column_node = right;
    literal_node = left;
    op = optimizer_flip_comparison(op);
  }
  if (column_node->type != IR_NodeType_Column)
  {
    return OPT_DEFAULT_SELECTIVITY;
  }
  
  U32 relation_index = optimizer_resolve_column(relations, relation_count, column_node->value);
  if (relation_index == OPT_NO_RELATION)
  {
    return OPT_DEFAULT_SELECTIVITY;
  }
  
  B32 is_not_equal = str8_match(op, str8_lit("!="), 0);
  B32 is_equals_keyword = str8_match(op, str8_lit("equals"), StringMatchFlag_CaseInsensitive);
  if (is_not_equal || is_equals_keyword)
  {
    op = str8_lit("=");
  }
  
  IR_Node* comparison = optimizer_make_comparison(arena, op, column_node, literal_node);
  
  GDB_Column* column = NULL;
  B32 is_eq = 0;
  B32 is_lt = 0;
  B32 is_le = 0;
  B32 is_gt = 0;
  B32 is_ge = 0;
  B32 is_string = 0;
  F64 target_numeric = 0.0;
  String8 target_string = {0};
  B32 resolved = qe_resolve_leaf_comparison(relations[relation_index].table, comparison, &column, &is_eq, &is_lt, &is_le, &is_gt, &is_ge, &is_string, &target_numeric, &target_string);
  if (!resolved)
  {
    if (is_not_equal)
    {
      return 1.0 - OPT_DEFAULT_SELECTIVITY_EQUALITY;
    }
    if (is_eq || str8_match(op, str8_lit("="), 0) || str8_match(op, str8_lit("=="), 0))
    {
      return OPT_DEFAULT_SELECTIVITY_EQUALITY;
    }
    return OPT_DEFAULT_SELECTIVITY_RANGE;
  }
  
  F64 non_null = optimizer_non_null_fraction(column);
  if (is_eq)
  {
    F64 equal = optimizer_equality_selectivity(column, is_string, target_numeric, target_string);
    if (is_not_equal)
    {
      return optimizer_clamp_selectivity(1.0 - equal) * non_null;
    }
    return equal * non_null;
  }
  
  if (is_string)
  {
    return OPT_DEFAULT_SELECTIVITY_RANGE * non_null;
  }
  return optimizer_range_selectivity(column, is_lt, is_le, is_gt, is_ge, target_numeric);
}

internal F64
optimizer_in_list_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf)
{
  IR_Node* column_node = leaf->first;
  IR_Node* list_node = column_node ? column_node->next : NULL;
  if (!column_node || column_node->type != IR_NodeType_Column || !list_node || list_node->type != IR_NodeType_InList)
  {
    return OPT_DEFAULT_SELECTIVITY;
  }
  
  U32 relation_index = optimizer_resolve_column(relations, relation_count, column_node->value);
  if (relation_index == OPT_NO_RELATION)
  {
    return OPT_DEFAULT_SELECTIVITY;
  }
  
  F64 total = 0.0;
  for (IR_Node* item = list_node->first; item != NULL; item = item->next)
  {
    IR_Node* comparison = optimizer_make_comparison(arena, str8_lit("="), column_node, item);
    total += optimizer_comparison_selectivity(arena, relations, relation_count, comparison);
  }
  total = optimizer_clamp_selectivity(total);
  
  B32 is_not_in = optimizer_ir_is_operator(leaf, str8_lit("not in"));
  if (is_not_in)
  {
    GDB_Column* column = gdb_table_find_column(relations[relation_index].table, qe_bare_column_name(column_node->value));
    F64 non_null = column ? optimizer_non_null_fraction(column) : 1.0;
    return optimizer_clamp_selectivity(non_null - total);
  }
  return total;
}

internal F64
optimizer_null_test_selectivity(OPT_Relation* relations, U32 relation_count, IR_Node* leaf)
{
  B32 is_not_null = optimizer_ir_is_operator(leaf, str8_lit("is not null"));
  IR_Node* column_node = leaf->first;
  
  F64 null_fraction = 0.1;
  if (column_node && column_node->type == IR_NodeType_Column)
  {
    U32 relation_index = optimizer_resolve_column(relations, relation_count, column_node->value);
    if (relation_index != OPT_NO_RELATION)
    {
      GDB_Column* column = gdb_table_find_column(relations[relation_index].table, qe_bare_column_name(column_node->value));
      if (column)
      {
        null_fraction = 1.0 - optimizer_non_null_fraction(column);
      }
    }
  }
  
  if (is_not_null)
  {
    return 1.0 - null_fraction;
  }
  return null_fraction;
}

// tec: column against column is one over the larger distinct count for equality
internal F64
optimizer_column_pair_selectivity(OPT_Relation* relations, U32 relation_count, IR_Node* leaf)
{
  B32 is_equality = optimizer_ir_is_operator(leaf, str8_lit("=")) || optimizer_ir_is_operator(leaf, str8_lit("=="));
  if (!is_equality)
  {
    return OPT_DEFAULT_SELECTIVITY_RANGE;
  }
  
  IR_Node* left = leaf->first;
  IR_Node* right = left->next;
  U32 left_relation = optimizer_resolve_column(relations, relation_count, left->value);
  U32 right_relation = optimizer_resolve_column(relations, relation_count, right->value);
  if (left_relation == OPT_NO_RELATION || right_relation == OPT_NO_RELATION)
  {
    return OPT_DEFAULT_SELECTIVITY_EQUALITY;
  }
  
  GDB_Column* left_column = gdb_table_find_column(relations[left_relation].table, qe_bare_column_name(left->value));
  GDB_Column* right_column = gdb_table_find_column(relations[right_relation].table, qe_bare_column_name(right->value));
  if (!left_column || !right_column)
  {
    return OPT_DEFAULT_SELECTIVITY_EQUALITY;
  }
  
  F64 left_distinct = optimizer_distinct_count(left_column);
  F64 right_distinct = optimizer_distinct_count(right_column);
  F64 larger = Max(left_distinct, right_distinct);
  return 1.0 / larger;
}

internal F64
optimizer_leaf_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf)
{
  if (!leaf || leaf->type != IR_NodeType_Operator)
  {
    return OPT_DEFAULT_SELECTIVITY;
  }
  
  if (optimizer_ir_is_operator(leaf, str8_lit("is null")) || optimizer_ir_is_operator(leaf, str8_lit("is not null")))
  {
    return optimizer_null_test_selectivity(relations, relation_count, leaf);
  }
  if (optimizer_ir_is_operator(leaf, str8_lit("in")) || optimizer_ir_is_operator(leaf, str8_lit("not in")))
  {
    return optimizer_in_list_selectivity(arena, relations, relation_count, leaf);
  }
  if (optimizer_ir_is_operator(leaf, str8_lit("contains")))
  {
    return OPT_DEFAULT_SELECTIVITY_PATTERN;
  }
  
  B32 is_comparison = optimizer_ir_is_operator(leaf, str8_lit("=")) ||
    optimizer_ir_is_operator(leaf, str8_lit("==")) ||
    optimizer_ir_is_operator(leaf, str8_lit("!=")) ||
    optimizer_ir_is_operator(leaf, str8_lit("<")) ||
    optimizer_ir_is_operator(leaf, str8_lit("<=")) ||
    optimizer_ir_is_operator(leaf, str8_lit(">")) ||
    optimizer_ir_is_operator(leaf, str8_lit(">=")) ||
    optimizer_ir_is_operator(leaf, str8_lit("equals"));
  if (!is_comparison || !leaf->first || !leaf->first->next)
  {
    return OPT_DEFAULT_SELECTIVITY;
  }
  
  IR_Node* left = leaf->first;
  IR_Node* right = left->next;
  if (left->type == IR_NodeType_AggregateCall || right->type == IR_NodeType_AggregateCall)
  {
    return OPT_DEFAULT_SELECTIVITY_PATTERN;
  }
  if (left->type == IR_NodeType_Column && right->type == IR_NodeType_Column)
  {
    return optimizer_column_pair_selectivity(relations, relation_count, leaf);
  }
  return optimizer_comparison_selectivity(arena, relations, relation_count, leaf);
}

//~ tec: combining predicates

// tec: exponential backoff, the most selective terms count fully and each later one counts with half the exponent
internal F64
optimizer_combine_and(F64* selectivities, U32 count)
{
  F64 sorted[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  U32 used = Min(count, (U32)OPT_ESTIMATE_LEAF_CAPACITY);
  for (U32 index = 0; index < used; index += 1)
  {
    sorted[index] = selectivities[index];
  }
  
  for (U32 outer = 1; outer < used; outer += 1)
  {
    F64 value = sorted[outer];
    U32 position = outer;
    while (position > 0 && sorted[position - 1] > value)
    {
      sorted[position] = sorted[position - 1];
      position -= 1;
    }
    sorted[position] = value;
  }
  
  F64 combined = 1.0;
  F64 exponent = 1.0;
  U32 terms = Min(used, (U32)OPT_BACKOFF_TERM_COUNT);
  for (U32 index = 0; index < terms; index += 1)
  {
    combined *= pow(sorted[index], exponent);
    exponent *= 0.5;
  }
  return optimizer_clamp_selectivity(combined);
}

// tec: a numeric column compared to a literal with < <= > or >=
internal B32
optimizer_range_leaf_info(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf, GDB_Column** out_column, B32* out_is_lower)
{
  if (!leaf || leaf->type != IR_NodeType_Operator || !leaf->first || !leaf->first->next)
  {
    return 0;
  }
  
  IR_Node* left = leaf->first;
  IR_Node* right = left->next;
  String8 op = leaf->value;
  IR_Node* column_node = left;
  IR_Node* literal_node = right;
  if (left->type != IR_NodeType_Column && right->type == IR_NodeType_Column)
  {
    column_node = right;
    literal_node = left;
    op = optimizer_flip_comparison(op);
  }
  if (column_node->type != IR_NodeType_Column)
  {
    return 0;
  }
  
  U32 relation_index = optimizer_resolve_column(relations, relation_count, column_node->value);
  if (relation_index == OPT_NO_RELATION)
  {
    return 0;
  }
  
  IR_Node* comparison = optimizer_make_comparison(arena, op, column_node, literal_node);
  GDB_Column* column = NULL;
  B32 is_eq = 0;
  B32 is_lt = 0;
  B32 is_le = 0;
  B32 is_gt = 0;
  B32 is_ge = 0;
  B32 is_string = 0;
  F64 target_numeric = 0.0;
  String8 target_string = {0};
  B32 resolved = qe_resolve_leaf_comparison(relations[relation_index].table, comparison, &column, &is_eq, &is_lt, &is_le, &is_gt, &is_ge, &is_string, &target_numeric, &target_string);
  if (!resolved || is_string || is_eq)
  {
    return 0;
  }
  
  *out_column = column;
  *out_is_lower = is_gt || is_ge;
  return 1;
}

// tec: a lower and an upper bound on one column select the interval between them, not the product of two ranges
internal void
optimizer_merge_range_pairs(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node** leaves, F64* selectivities, U32 count)
{
  GDB_Column* columns[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  B32 is_lower[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  B32 is_range[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  
  for (U32 index = 0; index < count; index += 1)
  {
    is_range[index] = optimizer_range_leaf_info(arena, relations, relation_count, leaves[index], &columns[index], &is_lower[index]);
  }
  
  for (U32 first = 0; first < count; first += 1)
  {
    if (!is_range[first])
    {
      continue;
    }
    
    for (U32 second = first + 1; second < count; second += 1)
    {
      B32 pairs_up = is_range[second] && columns[second] == columns[first] && is_lower[second] != is_lower[first];
      if (!pairs_up)
      {
        continue;
      }
      
      F64 non_null = optimizer_non_null_fraction(columns[first]);
      F64 interval = selectivities[first] + selectivities[second] - non_null;
      selectivities[first] = optimizer_clamp_selectivity(interval);
      selectivities[second] = 1.0;
      is_range[first] = 0;
      is_range[second] = 0;
      break;
    }
  }
}

internal F64
optimizer_condition_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* root)
{
  if (!root)
  {
    return 1.0;
  }
  
  if (optimizer_ir_is_operator(root, str8_lit("and")))
  {
    IR_Node* leaves[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
    U32 leaf_count = optimizer_ir_collect_conjuncts(root, leaves, 0, OPT_ESTIMATE_LEAF_CAPACITY);
    leaf_count = Min(leaf_count, (U32)OPT_ESTIMATE_LEAF_CAPACITY);
    
    F64 selectivities[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
    for (U32 index = 0; index < leaf_count; index += 1)
    {
      selectivities[index] = optimizer_condition_selectivity(arena, relations, relation_count, leaves[index]);
    }
    optimizer_merge_range_pairs(arena, relations, relation_count, leaves, selectivities, leaf_count);
    return optimizer_combine_and(selectivities, leaf_count);
  }
  
  if (optimizer_ir_is_operator(root, str8_lit("or")))
  {
    IR_Node* left = root->first;
    IR_Node* right = left ? left->next : NULL;
    F64 left_selectivity = optimizer_condition_selectivity(arena, relations, relation_count, left);
    F64 right_selectivity = optimizer_condition_selectivity(arena, relations, relation_count, right);
    return optimizer_clamp_selectivity(left_selectivity + right_selectivity - left_selectivity * right_selectivity);
  }
  
  F64 leaf_selectivity = optimizer_leaf_selectivity(arena, relations, relation_count, root);
  return optimizer_clamp_selectivity(leaf_selectivity);
}

//~ tec: plan annotation

internal U32
optimizer_collect_relations(PLAN_Node* subtree, OPT_Relation* out_relations, U32 count)
{
  if (!subtree)
  {
    return count;
  }
  
  if (subtree->type == PLAN_NodeType_Scan)
  {
    if (subtree->table && count < OPT_MAX_RELATIONS)
    {
      out_relations[count].scan = subtree;
      out_relations[count].table = subtree->table;
      out_relations[count].alias = subtree->alias;
      count += 1;
    }
    return count;
  }
  
  count = optimizer_collect_relations(subtree->input, out_relations, count);
  B32 hides_right_side = subtree->type == PLAN_NodeType_SemiJoin || subtree->type == PLAN_NodeType_AntiJoin;
  if (!hides_right_side)
  {
    count = optimizer_collect_relations(subtree->input2, out_relations, count);
  }
  return count;
}

// tec: out_hash_rows is what the hash join itself emits, the residual ON conditions filter that afterwards
internal F64
optimizer_estimate_join_rows(Arena* arena, PLAN_Node* join, F64* out_hash_rows)
{
  F64 left_rows = join->input->est_rows;
  F64 right_rows = join->input2->est_rows;
  
  OPT_Relation relations[OPT_MAX_RELATIONS] = {0};
  U32 left_count = optimizer_collect_relations(join->input, relations, 0);
  U32 total_count = optimizer_collect_relations(join->input2, relations, left_count);
  
  IR_Node* leaves[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  U32 leaf_count = 0;
  if (join->condition)
  {
    leaf_count = optimizer_ir_collect_conjuncts(join->condition, leaves, 0, OPT_ESTIMATE_LEAF_CAPACITY);
    leaf_count = Min(leaf_count, (U32)OPT_ESTIMATE_LEAF_CAPACITY);
  }
  
  IR_Node* key_leaf = NULL;
  GDB_Column* left_key = NULL;
  GDB_Column* right_key = NULL;
  for (U32 index = 0; index < leaf_count && !key_leaf; index += 1)
  {
    IR_Node* leaf = leaves[index];
    B32 is_equality = optimizer_ir_is_operator(leaf, str8_lit("=")) || optimizer_ir_is_operator(leaf, str8_lit("=="));
    if (!is_equality || !leaf->first || !leaf->first->next)
    {
      continue;
    }
    if (leaf->first->type != IR_NodeType_Column || leaf->first->next->type != IR_NodeType_Column)
    {
      continue;
    }
    
    U32 first_relation = optimizer_resolve_column(relations, total_count, leaf->first->value);
    U32 second_relation = optimizer_resolve_column(relations, total_count, leaf->first->next->value);
    if (first_relation == OPT_NO_RELATION || second_relation == OPT_NO_RELATION)
    {
      continue;
    }
    
    B32 first_is_left = first_relation < left_count;
    B32 second_is_left = second_relation < left_count;
    if (first_is_left == second_is_left)
    {
      continue;
    }
    
    IR_Node* left_node = first_is_left ? leaf->first : leaf->first->next;
    IR_Node* right_node = first_is_left ? leaf->first->next : leaf->first;
    U32 left_relation = first_is_left ? first_relation : second_relation;
    U32 right_relation = first_is_left ? second_relation : first_relation;
    left_key = gdb_table_find_column(relations[left_relation].table, qe_bare_column_name(left_node->value));
    right_key = gdb_table_find_column(relations[right_relation].table, qe_bare_column_name(right_node->value));
    if (left_key && right_key)
    {
      key_leaf = leaf;
    }
  }
  
  F64 rows = left_rows * right_rows;
  if (key_leaf)
  {
    F64 left_distinct = Min(optimizer_distinct_count(left_key), Max(left_rows, 1.0));
    F64 right_distinct = Min(optimizer_distinct_count(right_key), Max(right_rows, 1.0));
    F64 larger = Max(left_distinct, right_distinct);
    rows = rows / larger;
  }
  
  B32 is_left_join = str8_match(join->value, str8_lit("left"), StringMatchFlag_CaseInsensitive);
  F64 hash_rows = rows;
  if (is_left_join)
  {
    hash_rows = Max(hash_rows, left_rows);
  }
  *out_hash_rows = hash_rows;
  
  F64 residual_selectivities[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  U32 residual_count = 0;
  for (U32 index = 0; index < leaf_count; index += 1)
  {
    if (leaves[index] == key_leaf)
    {
      continue;
    }
    residual_selectivities[residual_count] = optimizer_condition_selectivity(arena, relations, total_count, leaves[index]);
    residual_count += 1;
  }
  if (residual_count > 0)
  {
    rows *= optimizer_combine_and(residual_selectivities, residual_count);
  }
  
  if (is_left_join)
  {
    rows = Max(rows, left_rows);
  }
  return rows;
}

// tec: the share of left keys with a partner is the right side's distinct keys over the left side's, capped at all of them
internal F64
optimizer_estimate_semi_rows(Arena* arena, PLAN_Node* join)
{
  F64 left_rows = join->input->est_rows;
  F64 right_rows = join->input2->est_rows;
  
  OPT_Relation relations[OPT_MAX_RELATIONS] = {0};
  U32 left_count = optimizer_collect_relations(join->input, relations, 0);
  U32 total_count = optimizer_collect_relations(join->input2, relations, left_count);
  
  F64 matching_fraction = OPT_DEFAULT_SELECTIVITY;
  IR_Node* condition = join->condition;
  IR_Node* first_key = condition ? condition->first : NULL;
  IR_Node* second_key = first_key ? first_key->next : NULL;
  if (first_key && second_key)
  {
    U32 first_relation = optimizer_resolve_column(relations, total_count, first_key->value);
    U32 second_relation = optimizer_resolve_column(relations, total_count, second_key->value);
    B32 resolved = first_relation != OPT_NO_RELATION && second_relation != OPT_NO_RELATION;
    if (resolved)
    {
      B32 first_is_left = first_relation < left_count;
      IR_Node* left_node = first_is_left ? first_key : second_key;
      IR_Node* right_node = first_is_left ? second_key : first_key;
      U32 left_relation = first_is_left ? first_relation : second_relation;
      U32 right_relation = first_is_left ? second_relation : first_relation;
      GDB_Column* left_column = gdb_table_find_column(relations[left_relation].table, qe_bare_column_name(left_node->value));
      GDB_Column* right_column = gdb_table_find_column(relations[right_relation].table, qe_bare_column_name(right_node->value));
      if (left_column && right_column)
      {
        F64 left_distinct = Min(optimizer_distinct_count(left_column), Max(left_rows, 1.0));
        F64 right_distinct = Min(optimizer_distinct_count(right_column), Max(right_rows, 1.0));
        matching_fraction = Min(1.0, right_distinct / Max(left_distinct, 1.0));
      }
    }
  }
  
  if (join->type == PLAN_NodeType_AntiJoin)
  {
    return left_rows * (1.0 - matching_fraction);
  }
  return left_rows * matching_fraction;
}

internal F64
optimizer_estimate_group_count(PLAN_Node* aggregate, F64 input_rows)
{
  IR_Node* group_by = aggregate->group_by;
  if (!group_by || !group_by->first)
  {
    return 1.0;
  }
  
  OPT_Relation relations[OPT_MAX_RELATIONS] = {0};
  U32 relation_count = optimizer_collect_relations(aggregate->input, relations, 0);
  
  F64 groups = 1.0;
  for (IR_Node* key = group_by->first; key != NULL; key = key->next)
  {
    F64 distinct = OPT_DEFAULT_GROUP_KEY_DISTINCT;
    if (key->type == IR_NodeType_Column)
    {
      U32 relation_index = optimizer_resolve_column(relations, relation_count, key->value);
      if (relation_index != OPT_NO_RELATION)
      {
        GDB_Column* column = gdb_table_find_column(relations[relation_index].table, qe_bare_column_name(key->value));
        if (column)
        {
          distinct = optimizer_distinct_count(column);
        }
      }
    }
    groups *= Min(distinct, Max(input_rows, 1.0));
  }
  
  groups = Min(groups, input_rows);
  if (input_rows > 0.0 && groups < 1.0)
  {
    groups = 1.0;
  }
  return groups;
}

internal F64
optimizer_estimate_limit_rows(PLAN_Node* limit, F64 input_rows)
{
  F64 offset = 0.0;
  if (limit->offset_node)
  {
    offset = (F64)u64_from_str8(limit->offset_node->value, 10);
  }
  
  F64 remaining = input_rows - offset;
  if (remaining < 0.0)
  {
    remaining = 0.0;
  }
  
  if (limit->limit_node)
  {
    F64 limit_rows = (F64)u64_from_str8(limit->limit_node->value, 10);
    remaining = Min(remaining, limit_rows);
  }
  return remaining;
}

internal void
optimizer_annotate_plan(Arena* arena, PLAN_Node* plan)
{
  if (!plan)
  {
    return;
  }
  
  optimizer_annotate_plan(arena, plan->input);
  optimizer_annotate_plan(arena, plan->input2);
  
  F64 input_rows = 0.0;
  B32 input_known = 1;
  if (plan->input)
  {
    input_rows = plan->input->est_rows;
    input_known = plan->input->has_estimate;
  }
  
  switch (plan->type)
  {
    case PLAN_NodeType_Scan:
    {
      if (plan->table)
      {
        plan->est_rows = (F64)plan->table->row_count;
        plan->has_estimate = 1;
      }
    } break;
    
    case PLAN_NodeType_Filter:
    {
      OPT_Relation relations[OPT_MAX_RELATIONS] = {0};
      U32 relation_count = optimizer_collect_relations(plan->input, relations, 0);
      IR_Node* root = plan->condition ? plan->condition->first : NULL;
      F64 selectivity = optimizer_condition_selectivity(arena, relations, relation_count, root);
      plan->est_rows = input_rows * selectivity;
      plan->has_estimate = input_known;
    } break;
    
    case PLAN_NodeType_Join:
    {
      B32 both_known = input_known && plan->input2 && plan->input2->has_estimate;
      if (both_known)
      {
        plan->est_rows = optimizer_estimate_join_rows(arena, plan, &plan->est_hash_rows);
      }
      plan->has_estimate = both_known;
    } break;
    
    case PLAN_NodeType_SemiJoin:
    case PLAN_NodeType_AntiJoin:
    {
      B32 both_known = input_known && plan->input2 && plan->input2->has_estimate;
      if (both_known)
      {
        plan->est_rows = optimizer_estimate_semi_rows(arena, plan);
      }
      plan->has_estimate = both_known;
    } break;
    
    case PLAN_NodeType_Aggregate:
    {
      plan->est_rows = optimizer_estimate_group_count(plan, input_rows);
      plan->has_estimate = input_known;
    } break;
    
    case PLAN_NodeType_Having:
    {
      plan->est_rows = input_rows * OPT_DEFAULT_SELECTIVITY_HAVING;
      plan->has_estimate = input_known;
    } break;
    
    case PLAN_NodeType_Limit:
    case PLAN_NodeType_TopN:
    {
      plan->est_rows = optimizer_estimate_limit_rows(plan, input_rows);
      plan->has_estimate = input_known;
    } break;
    
    case PLAN_NodeType_Project:
    case PLAN_NodeType_Sort:
    case PLAN_NodeType_Window:
    {
      plan->est_rows = input_rows;
      plan->has_estimate = input_known;
    } break;
  }
  
  plan->est_round_trips = optimizer_node_round_trips(plan);
}

//~ tec: estimates for sources that are not executed

internal void
optimizer_source_estimates_add(OPT_SourceEstimates* sources, String8 name, F64 rows)
{
  if (sources->count >= OPT_SOURCE_ESTIMATE_CAPACITY)
  {
    return;
  }
  sources->names[sources->count] = name;
  sources->rows[sources->count] = rows;
  sources->count += 1;
}

internal void
optimizer_apply_source_estimates(PLAN_Node* plan, OPT_SourceEstimates* sources)
{
  if (!plan)
  {
    return;
  }
  
  if (plan->type == PLAN_NodeType_Scan && !plan->table)
  {
    String8 name = plan->alias.size ? plan->alias : plan->value;
    for (U32 index = sources->count; index > 0; index -= 1)
    {
      if (str8_match(sources->names[index - 1], name, StringMatchFlag_CaseInsensitive))
      {
        plan->est_rows = sources->rows[index - 1];
        plan->has_estimate = 1;
        break;
      }
    }
  }
  
  optimizer_apply_source_estimates(plan->input, sources);
  optimizer_apply_source_estimates(plan->input2, sources);
}

//~ tec: cost model

internal OPT_CostModel
optimizer_cost_model_load(void)
{
  OPT_CostModel model = {0};
  model.gpu_submit_us = settings_f64(str8_lit("QE_COST_GPU_SUBMIT_US"), 140.0);
  model.gpu_scan_per_row_us = settings_f64(str8_lit("QE_COST_GPU_SCAN_PER_ROW_US"), 0.0006);
  model.gpu_upload_fixed_us = settings_f64(str8_lit("QE_COST_GPU_UPLOAD_FIXED_US"), 100.0);
  model.gpu_upload_per_row_us = settings_f64(str8_lit("QE_COST_GPU_UPLOAD_PER_ROW_US"), 0.0009);
  
  model.cpu_scan_fixed_us = settings_f64(str8_lit("QE_COST_CPU_SCAN_FIXED_US"), 28.0);
  model.cpu_scan_per_row_us = settings_f64(str8_lit("QE_COST_CPU_SCAN_PER_ROW_US"), 0.0215);
  model.cpu_scan_per_extra_condition_us = settings_f64(str8_lit("QE_COST_CPU_SCAN_PER_EXTRA_CONDITION_US"), 0.0078);
  model.cpu_reference_workers = settings_f64(str8_lit("QE_COST_CPU_REFERENCE_WORKERS"), 12.0);
  
  model.index_fixed_us = settings_f64(str8_lit("QE_COST_INDEX_FIXED_US"), 2.0);
  model.index_per_match_us = settings_f64(str8_lit("QE_COST_INDEX_PER_MATCH_US"), 0.0013);
  model.filter_per_row_us = settings_f64(str8_lit("QE_COST_FILTER_PER_ROW_US"), 0.115);
  model.fill_per_row_us = settings_f64(str8_lit("QE_COST_FILL_PER_ROW_US"), 0.001);
  
  model.join_build_per_row_us = settings_f64(str8_lit("QE_COST_JOIN_BUILD_PER_ROW_US"), 0.0049);
  model.join_probe_per_row_us = settings_f64(str8_lit("QE_COST_JOIN_PROBE_PER_ROW_US"), 0.0045);
  model.join_output_per_row_us = settings_f64(str8_lit("QE_COST_JOIN_OUTPUT_PER_ROW_US"), 0.0045);
  
  model.sort_cpu_per_row_log_us = settings_f64(str8_lit("QE_COST_SORT_CPU_PER_ROW_LOG_US"), 0.0095);
  model.sort_gpu_fixed_us = settings_f64(str8_lit("QE_COST_SORT_GPU_FIXED_US"), 1100.0);
  model.sort_gpu_per_row_us = settings_f64(str8_lit("QE_COST_SORT_GPU_PER_ROW_US"), 0.11);
  model.top_n_per_row_us = settings_f64(str8_lit("QE_COST_TOP_N_PER_ROW_US"), 0.012);
  
  model.aggregate_cpu_per_row_us = settings_f64(str8_lit("QE_COST_AGGREGATE_CPU_PER_ROW_US"), 0.075);
  model.aggregate_gpu_fixed_us = settings_f64(str8_lit("QE_COST_AGGREGATE_GPU_FIXED_US"), 700.0);
  model.aggregate_gpu_per_row_us = settings_f64(str8_lit("QE_COST_AGGREGATE_GPU_PER_ROW_US"), 0.008);
  return model;
}

// tec: the CPU scan splits rows across the thread pool, so fewer workers than the calibration ran with cost proportionally more
internal F64
optimizer_cost_worker_scale(OPT_CostModel* model)
{
  F64 workers = (F64)app_thread_pool()->worker_count;
  if (workers < 1.0)
  {
    workers = 1.0;
  }
  return model->cpu_reference_workers / workers;
}

internal F64
optimizer_cost_gpu_scan(OPT_CostModel* model, F64 rows, U32 cold_column_count)
{
  F64 cost = optimizer_scan_round_trips() * model->gpu_submit_us + rows * model->gpu_scan_per_row_us;
  if (cold_column_count > 0)
  {
    cost += model->gpu_upload_fixed_us + rows * model->gpu_upload_per_row_us * (F64)cold_column_count;
  }
  return cost;
}

internal F64
optimizer_cost_cpu_scan(OPT_CostModel* model, F64 rows, U32 condition_count)
{
  F64 extra_conditions = 0.0;
  if (condition_count > 1)
  {
    extra_conditions = (F64)(condition_count - 1);
  }
  F64 per_row = model->cpu_scan_per_row_us + extra_conditions * model->cpu_scan_per_extra_condition_us;
  return model->cpu_scan_fixed_us + rows * per_row * optimizer_cost_worker_scale(model);
}

internal F64
optimizer_cost_index_scan(OPT_CostModel* model, F64 matches, U32 residual_condition_count)
{
  F64 per_match = model->index_per_match_us + (F64)residual_condition_count * model->filter_per_row_us;
  return model->index_fixed_us + matches * per_match;
}

internal F64
optimizer_cost_identity(OPT_CostModel* model, F64 rows)
{
  return rows * model->fill_per_row_us;
}

internal F64
optimizer_cost_hash_join(OPT_CostModel* model, F64 build_rows, F64 probe_rows, F64 output_rows)
{
  F64 fixed = optimizer_join_round_trips() * model->gpu_submit_us;
  return fixed + build_rows * model->join_build_per_row_us + probe_rows * model->join_probe_per_row_us + output_rows * model->join_output_per_row_us;
}

internal F64
optimizer_cost_sort_cpu(OPT_CostModel* model, F64 rows)
{
  if (rows < 2.0)
  {
    return 0.0;
  }
  return rows * log2(rows) * model->sort_cpu_per_row_log_us;
}

internal F64
optimizer_cost_sort_gpu(OPT_CostModel* model, F64 rows)
{
  return model->sort_gpu_fixed_us + rows * model->sort_gpu_per_row_us;
}

internal F64
optimizer_cost_top_n(OPT_CostModel* model, F64 rows)
{
  return rows * model->top_n_per_row_us;
}

internal F64
optimizer_cost_aggregate_cpu(OPT_CostModel* model, F64 rows)
{
  return rows * model->aggregate_cpu_per_row_us;
}

internal F64
optimizer_cost_aggregate_gpu(OPT_CostModel* model, F64 rows)
{
  return model->aggregate_gpu_fixed_us + rows * model->aggregate_gpu_per_row_us;
}

// tec: bisects between the last size the CPU wins at and the first the GPU does
internal U64
optimizer_crossover_rows(OPT_CostModel* model, OPT_CostFn* cpu_cost, OPT_CostFn* gpu_cost)
{
  U64 low = 0;
  U64 high = 16;
  while (high < OPT_SMALL_INPUT_SEARCH_LIMIT && cpu_cost(model, (F64)high) < gpu_cost(model, (F64)high))
  {
    low = high;
    high *= 2;
  }
  
  for (U32 step = 0; step < 12; step += 1)
  {
    U64 middle = low + (high - low) / 2;
    if (cpu_cost(model, (F64)middle) < gpu_cost(model, (F64)middle))
    {
      low = middle;
    }
    else
    {
      high = middle;
    }
  }
  return high;
}

internal U64
optimizer_sort_gpu_min_rows(OPT_CostModel* model)
{
  return optimizer_crossover_rows(model, optimizer_cost_sort_cpu, optimizer_cost_sort_gpu);
}

internal U64
optimizer_aggregate_cpu_max_rows(OPT_CostModel* model)
{
  return optimizer_crossover_rows(model, optimizer_cost_aggregate_cpu, optimizer_cost_aggregate_gpu);
}

//~ tec: physical scan strategy

internal B32
optimizer_scan_costing_enabled(void)
{
  B32 master = settings_bool(str8_lit("QE_OPTIMIZER"), 1);
  B32 scan_costing = settings_bool(str8_lit("QE_OPT_SCAN_COST"), 1);
  return master && scan_costing;
}

//~ tec: what a scan condition reads

// tec: a name that is not a column of this table, or too many columns to track, marks the whole condition unresolved
internal void
optimizer_collect_condition_columns(GDB_Table* table, IR_Node* node, GDB_Column** out_columns, U32* io_count, U32 capacity, B32* out_unresolved)
{
  if (!node)
  {
    return;
  }
  
  if (node->type == IR_NodeType_Column)
  {
    if (str8_match(node->value, str8_lit("*"), 0))
    {
      return;
    }
    
    GDB_Column* column = gdb_table_find_column(table, qe_bare_column_name(node->value));
    if (!column)
    {
      *out_unresolved = 1;
      return;
    }
    
    for (U32 index = 0; index < *io_count; index += 1)
    {
      if (out_columns[index] == column)
      {
        return;
      }
    }
    
    if (*io_count >= capacity)
    {
      *out_unresolved = 1;
      return;
    }
    out_columns[*io_count] = column;
    *io_count += 1;
    return;
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    optimizer_collect_condition_columns(table, child, out_columns, io_count, capacity, out_unresolved);
  }
}

// tec: a column that ever held a NULL keeps a placeholder value in its data that the GPU scan would compare as a real value
internal B32
optimizer_columns_have_nulls(GDB_Column** columns, U32 count)
{
  for (U32 index = 0; index < count; index += 1)
  {
    if (columns[index]->null_flags != NULL)
    {
      return 1;
    }
  }
  return 0;
}

internal U32
optimizer_count_cold_columns(GDB_Column** columns, U32 count)
{
  U32 cold = 0;
  for (U32 index = 0; index < count; index += 1)
  {
    if (columns[index]->gpu_upload_generation != columns[index]->write_generation)
    {
      cold += 1;
    }
  }
  return cold;
}

//~ tec: ordering the conditions of a CPU scan

// tec: the CPU scan stops at the first false condition, so the most selective one goes first, measured at 1.4x to 1.6x on three conditions
internal void
optimizer_order_conjuncts_for_cpu(Arena* arena, PLAN_Node* filter, IR_Node** leaves, U32 leaf_count)
{
  OPT_Relation relation = {0};
  relation.scan = filter->input;
  relation.table = filter->input->table;
  relation.alias = filter->input->alias;
  
  F64 selectivities[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  IR_Node* ordered[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  for (U32 index = 0; index < leaf_count; index += 1)
  {
    selectivities[index] = optimizer_condition_selectivity(arena, &relation, 1, leaves[index]);
    ordered[index] = leaves[index];
  }
  
  B32 changed = 0;
  for (U32 outer = 1; outer < leaf_count; outer += 1)
  {
    F64 selectivity = selectivities[outer];
    IR_Node* leaf = ordered[outer];
    U32 position = outer;
    while (position > 0 && selectivities[position - 1] > selectivity)
    {
      selectivities[position] = selectivities[position - 1];
      ordered[position] = ordered[position - 1];
      position -= 1;
      changed = 1;
    }
    selectivities[position] = selectivity;
    ordered[position] = leaf;
  }
  
  if (!changed)
  {
    return;
  }
  
  IR_Node* chain = optimizer_ir_and_chain(arena, ordered, leaf_count);
  filter->condition = optimizer_ir_make_where(arena, chain);
}

//~ tec: choosing how a scan runs

internal void
optimizer_choose_filter_scan(Arena* arena, OPT_CostModel* model, PLAN_Node* filter)
{
  GDB_Table* table = filter->input->table;
  IR_Node* root = filter->condition ? filter->condition->first : NULL;
  if (!table || !root)
  {
    return;
  }
  
  F64 rows = (F64)table->row_count;
  
  GDB_Column* columns[OPT_SCAN_COLUMN_CAPACITY] = {0};
  U32 column_count = 0;
  B32 unresolved = 0;
  optimizer_collect_condition_columns(table, root, columns, &column_count, OPT_SCAN_COLUMN_CAPACITY, &unresolved);
  
  IR_Node* leaves[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  U32 total_leaf_count = optimizer_ir_collect_conjuncts(root, leaves, 0, OPT_ESTIMATE_LEAF_CAPACITY);
  U32 leaf_count = Min(total_leaf_count, (U32)OPT_ESTIMATE_LEAF_CAPACITY);
  
  B32 has_nulls = optimizer_columns_have_nulls(columns, column_count);
  B32 gpu_allowed = !unresolved && !has_nulls;
  
  F64 best_cost = optimizer_cost_cpu_scan(model, rows, Max(leaf_count, (U32)1));
  PLAN_ScanStrategy best = PLAN_ScanStrategy_Cpu;
  IR_Node* best_leaf = NULL;
  String8 reason = str8_lit("cheaper than a GPU dispatch");
  if (has_nulls)
  {
    reason = str8_lit("a referenced column has NULLs");
  }
  
  if (gpu_allowed)
  {
    U32 cold_columns = optimizer_count_cold_columns(columns, column_count);
    F64 gpu_cost = optimizer_cost_gpu_scan(model, rows, cold_columns);
    if (gpu_cost < best_cost)
    {
      best_cost = gpu_cost;
      best = PLAN_ScanStrategy_Gpu;
      reason = str8_lit("cheaper than a CPU scan");
    }
  }
  
  for (U32 index = 0; index < leaf_count; index += 1)
  {
    U64 matches = 0;
    if (!qe_index_leaf_match_count(arena, table, leaves[index], &matches))
    {
      continue;
    }
    
    U32 residual_conditions = leaf_count > 1 ? leaf_count : 0;
    F64 index_cost = optimizer_cost_index_scan(model, (F64)matches, residual_conditions);
    if (index_cost < best_cost)
    {
      best_cost = index_cost;
      best = PLAN_ScanStrategy_Index;
      best_leaf = leaves[index];
      reason = push_str8f(arena, "index narrows to %llu rows", matches);
    }
  }
  
  // tec: the statistics this needs cost about one pass over the column, so a large table keeps its written order
  U64 reorder_max_rows = settings_u64(str8_lit("QE_OPT_REORDER_MAX_ROWS"), 4000000);
  B32 can_reorder = best == PLAN_ScanStrategy_Cpu && leaf_count > 1 && leaf_count == total_leaf_count && table->row_count <= reorder_max_rows;
  if (can_reorder)
  {
    optimizer_order_conjuncts_for_cpu(arena, filter, leaves, leaf_count);
  }
  
  filter->scan_strategy = best;
  filter->index_leaf = best_leaf;
  filter->est_cost_us = best_cost;
  filter->scan_reason = reason;
}

internal void
optimizer_choose_identity_scan(OPT_CostModel* model, PLAN_Node* scan)
{
  scan->scan_strategy = PLAN_ScanStrategy_Identity;
  scan->est_cost_us = optimizer_cost_identity(model, (F64)scan->table->row_count);
  scan->scan_reason = str8_lit("no predicate");
}

// tec: a bare scan on the right of a join is never executed, the join reads the whole table itself
internal void
optimizer_choose_scan_strategies_in(Arena* arena, OPT_CostModel* model, PLAN_Node* plan, B32 is_join_right)
{
  if (!plan)
  {
    return;
  }
  
  if (plan->type == PLAN_NodeType_Filter && plan->input && plan->input->type == PLAN_NodeType_Scan)
  {
    optimizer_choose_filter_scan(arena, model, plan);
    return;
  }
  
  if (plan->type == PLAN_NodeType_Scan)
  {
    if (plan->table && !is_join_right)
    {
      optimizer_choose_identity_scan(model, plan);
    }
    return;
  }
  
  optimizer_choose_scan_strategies_in(arena, model, plan->input, 0);
  optimizer_choose_scan_strategies_in(arena, model, plan->input2, 1);
}

internal void
optimizer_choose_scan_strategies(Arena* arena, PLAN_Node* plan)
{
  OPT_CostModel model = optimizer_cost_model_load();
  optimizer_choose_scan_strategies_in(arena, &model, plan, 0);
}

//~ tec: join ordering

internal B32
optimizer_join_enabled(void)
{
  B32 master = settings_bool(str8_lit("QE_OPTIMIZER"), 1);
  B32 join_order = settings_bool(str8_lit("QE_OPT_JOIN_ORDER"), 1);
  return master && join_order;
}

//~ tec: building the graph

internal B32
optimizer_join_add_edge(OPT_JoinGraph* graph, U32 relation_a, GDB_Column* column_a, String8 name_a, U32 relation_b, GDB_Column* column_b, String8 name_b, B32 is_derived)
{
  for (U32 index = 0; index < graph->edge_count; index += 1)
  {
    OPT_JoinEdge* existing = &graph->edges[index];
    B32 same_direction = existing->relation_a == relation_a && existing->relation_b == relation_b &&
      str8_match(existing->name_a, name_a, StringMatchFlag_CaseInsensitive) && str8_match(existing->name_b, name_b, StringMatchFlag_CaseInsensitive);
    B32 opposite_direction = existing->relation_a == relation_b && existing->relation_b == relation_a &&
      str8_match(existing->name_a, name_b, StringMatchFlag_CaseInsensitive) && str8_match(existing->name_b, name_a, StringMatchFlag_CaseInsensitive);
    if (same_direction || opposite_direction)
    {
      return 1;
    }
  }
  
  if (graph->edge_count >= OPT_JOIN_MAX_EDGES)
  {
    return 0;
  }
  
  OPT_JoinEdge* edge = &graph->edges[graph->edge_count];
  edge->relation_a = relation_a;
  edge->relation_b = relation_b;
  edge->column_a = column_a;
  edge->column_b = column_b;
  edge->name_a = name_a;
  edge->name_b = name_b;
  edge->is_derived = is_derived;
  graph->edge_count += 1;
  return 1;
}

// tec: only equalities that were joins in the written query, non equality conditions never connect anything
internal void
optimizer_join_collect_edges(OPT_Relation* relations, U32 relation_count, OPT_Conjunct* conjuncts, U32 conjunct_count, U32 group_size, OPT_JoinGraph* graph)
{
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    OPT_Conjunct* conjunct = &conjuncts[index];
    if (conjunct->destination != OPT_Destination_OnClause || conjunct->target >= group_size || conjunct->unresolved)
    {
      continue;
    }
    if (optimizer_mask_popcount(conjunct->mask) != 2)
    {
      continue;
    }
    
    IR_Node* node = conjunct->node;
    B32 is_equality = optimizer_ir_is_operator(node, str8_lit("=")) || optimizer_ir_is_operator(node, str8_lit("=="));
    if (!is_equality || !node->first || !node->first->next)
    {
      continue;
    }
    
    U32 relation_a = 0;
    U32 relation_b = 0;
    String8 name_a = {0};
    String8 name_b = {0};
    GDB_ColumnType type_a = GDB_ColumnType_Invalid;
    GDB_ColumnType type_b = GDB_ColumnType_Invalid;
    B32 a_ok = optimizer_column_operand(relations, relation_count, node->first, &relation_a, &name_a, &type_a);
    B32 b_ok = optimizer_column_operand(relations, relation_count, node->first->next, &relation_b, &name_b, &type_b);
    if (!a_ok || !b_ok || relation_a == relation_b)
    {
      continue;
    }
    
    B32 both_numbers = optimizer_column_type_is_comparable_number(type_a) && optimizer_column_type_is_comparable_number(type_b);
    B32 both_strings = type_a == GDB_ColumnType_String8 && type_b == GDB_ColumnType_String8;
    if (!both_numbers && !both_strings)
    {
      continue;
    }
    
    GDB_Column* column_a = gdb_table_find_column(relations[relation_a].table, name_a);
    GDB_Column* column_b = gdb_table_find_column(relations[relation_b].table, name_b);
    optimizer_join_add_edge(graph, relation_a, column_a, name_a, relation_b, column_b, name_b, 0);
  }
}

// tec: a = b and b = c imply a = c, which lets a relation join to one it was never written next to
internal void
optimizer_join_add_derived_edges(OPT_JoinGraph* graph)
{
  OPT_ColumnKey keys[OPT_JOIN_MAX_EDGES * 2] = {0};
  GDB_Column* key_columns[OPT_JOIN_MAX_EDGES * 2] = {0};
  U32 key_count = 0;
  
  U32 original_edge_count = graph->edge_count;
  for (U32 index = 0; index < original_edge_count; index += 1)
  {
    OPT_JoinEdge* edge = &graph->edges[index];
    U32 key_a = optimizer_column_key_index(keys, &key_count, edge->relation_a, edge->name_a);
    U32 key_b = optimizer_column_key_index(keys, &key_count, edge->relation_b, edge->name_b);
    if (key_a == OPT_NO_RELATION || key_b == OPT_NO_RELATION)
    {
      continue;
    }
    key_columns[key_a] = edge->column_a;
    key_columns[key_b] = edge->column_b;
    
    U32 root_a = optimizer_column_key_root(keys, key_a);
    U32 root_b = optimizer_column_key_root(keys, key_b);
    if (root_a != root_b)
    {
      keys[root_b].parent = root_a;
    }
  }
  
  for (U32 first = 0; first < key_count; first += 1)
  {
    for (U32 second = first + 1; second < key_count; second += 1)
    {
      B32 same_class = optimizer_column_key_root(keys, first) == optimizer_column_key_root(keys, second);
      B32 different_relations = keys[first].relation != keys[second].relation;
      if (!same_class || !different_relations)
      {
        continue;
      }
      optimizer_join_add_edge(graph, keys[first].relation, key_columns[first], keys[first].column, keys[second].relation, key_columns[second], keys[second].column, 1);
    }
  }
}

internal B32
optimizer_join_has_edge_between(OPT_JoinGraph* graph, U32 relation_a, U32 relation_b, B32 original_only, OPT_JoinEdge** out_edge)
{
  for (U32 index = 0; index < graph->edge_count; index += 1)
  {
    OPT_JoinEdge* edge = &graph->edges[index];
    if (original_only && edge->is_derived)
    {
      continue;
    }
    
    B32 forward = edge->relation_a == relation_a && edge->relation_b == relation_b;
    B32 backward = edge->relation_a == relation_b && edge->relation_b == relation_a;
    if (forward || backward)
    {
      if (out_edge)
      {
        *out_edge = edge;
      }
      return 1;
    }
  }
  return 0;
}

//~ tec: cardinality

// tec: one over the larger distinct count, each capped by how many rows of that side are left
internal F64
optimizer_join_edge_selectivity(OPT_JoinEdge* edge, F64 rows_a, F64 rows_b)
{
  F64 distinct_a = Min(optimizer_distinct_count(edge->column_a), Max(rows_a, 1.0));
  F64 distinct_b = Min(optimizer_distinct_count(edge->column_b), Max(rows_b, 1.0));
  F64 larger = Max(Max(distinct_a, distinct_b), 1.0);
  return 1.0 / larger;
}

// tec: returns false when no edge ties the next relation to the set, the executor cannot join without an equality
internal B32
optimizer_join_extend_rows(OPT_JoinGraph* graph, U64 subset_mask, F64 subset_rows, U32 next, F64* out_rows)
{
  F64 selectivities[OPT_ESTIMATE_LEAF_CAPACITY] = {0};
  U32 selectivity_count = 0;
  
  for (U32 index = 0; index < graph->edge_count; index += 1)
  {
    OPT_JoinEdge* edge = &graph->edges[index];
    B32 a_is_next = edge->relation_a == next && ((subset_mask >> edge->relation_b) & 1) != 0;
    B32 b_is_next = edge->relation_b == next && ((subset_mask >> edge->relation_a) & 1) != 0;
    if (!a_is_next && !b_is_next)
    {
      continue;
    }
    if (selectivity_count >= OPT_ESTIMATE_LEAF_CAPACITY)
    {
      break;
    }
    
    // tec: the edge stores its columns in the order they were written, so which side has the subset's rows depends on that order
    F64 rows_a = a_is_next ? graph->rows[next] : subset_rows;
    F64 rows_b = a_is_next ? subset_rows : graph->rows[next];
    selectivities[selectivity_count] = optimizer_join_edge_selectivity(edge, rows_a, rows_b);
    selectivity_count += 1;
  }
  
  if (selectivity_count == 0)
  {
    return 0;
  }
  
  F64 combined = optimizer_combine_and(selectivities, selectivity_count);
  *out_rows = subset_rows * graph->rows[next] * combined;
  return 1;
}

internal F64
optimizer_join_order_cost(OPT_JoinGraph* graph, OPT_CostModel* model, U32* order, U32 count, B32* out_valid)
{
  *out_valid = 1;
  F64 rows = graph->rows[order[0]];
  U64 mask = (U64)1 << order[0];
  F64 total = 0.0;
  
  for (U32 position = 1; position < count; position += 1)
  {
    U32 next = order[position];
    F64 output_rows = 0.0;
    if (!optimizer_join_extend_rows(graph, mask, rows, next, &output_rows))
    {
      *out_valid = 0;
      return 0.0;
    }
    total += optimizer_cost_hash_join(model, graph->rows[next], rows, output_rows);
    rows = output_rows;
    mask |= (U64)1 << next;
  }
  return total;
}

//~ tec: search

// tec: Selinger style, every connected left-deep order over subsets of relations, best cost per subset
internal B32
optimizer_join_order_dp(OPT_JoinGraph* graph, OPT_CostModel* model, U32 count, U32* out_order, F64* out_cost)
{
  if (count < 2 || count > OPT_JOIN_DP_MAX_RELATIONS)
  {
    return 0;
  }
  
  OPT_JoinState states[1 << OPT_JOIN_DP_MAX_RELATIONS] = {0};
  U64 full_mask = ((U64)1 << count) - 1;
  
  for (U32 relation = 0; relation < count; relation += 1)
  {
    U64 mask = (U64)1 << relation;
    states[mask].valid = 1;
    states[mask].cost = 0.0;
    states[mask].rows = graph->rows[relation];
    states[mask].last = relation;
    states[mask].previous_mask = 0;
  }
  
  for (U64 mask = 1; mask <= full_mask; mask += 1)
  {
    if (!states[mask].valid)
    {
      continue;
    }
    
    for (U32 next = 0; next < count; next += 1)
    {
      if ((mask >> next) & 1)
      {
        continue;
      }
      
      F64 output_rows = 0.0;
      if (!optimizer_join_extend_rows(graph, mask, states[mask].rows, next, &output_rows))
      {
        continue;
      }
      
      F64 step_cost = optimizer_cost_hash_join(model, graph->rows[next], states[mask].rows, output_rows);
      F64 new_cost = states[mask].cost + step_cost;
      U64 new_mask = mask | ((U64)1 << next);
      if (!states[new_mask].valid || new_cost < states[new_mask].cost)
      {
        states[new_mask].valid = 1;
        states[new_mask].cost = new_cost;
        states[new_mask].rows = output_rows;
        states[new_mask].last = next;
        states[new_mask].previous_mask = mask;
      }
    }
  }
  
  if (!states[full_mask].valid)
  {
    return 0;
  }
  
  U64 mask = full_mask;
  for (U32 position = count; position > 0; position -= 1)
  {
    out_order[position - 1] = states[mask].last;
    mask = states[mask].previous_mask;
  }
  *out_cost = states[full_mask].cost;
  return 1;
}

// tec: for more relations than the search can hold, start from the smallest and always add the connected relation that keeps the result smallest
internal B32
optimizer_join_order_greedy(OPT_JoinGraph* graph, OPT_CostModel* model, U32 count, U32* out_order, F64* out_cost)
{
  if (count < 2)
  {
    return 0;
  }
  
  U32 start = OPT_NO_RELATION;
  for (U32 relation = 0; relation < count; relation += 1)
  {
    B32 has_edge = 0;
    for (U32 other = 0; other < count && !has_edge; other += 1)
    {
      has_edge = other != relation && optimizer_join_has_edge_between(graph, relation, other, 0, NULL);
    }
    if (has_edge && (start == OPT_NO_RELATION || graph->rows[relation] < graph->rows[start]))
    {
      start = relation;
    }
  }
  if (start == OPT_NO_RELATION)
  {
    return 0;
  }
  
  U64 mask = (U64)1 << start;
  F64 rows = graph->rows[start];
  F64 total = 0.0;
  out_order[0] = start;
  
  for (U32 position = 1; position < count; position += 1)
  {
    U32 best = OPT_NO_RELATION;
    F64 best_rows = 0.0;
    for (U32 next = 0; next < count; next += 1)
    {
      if ((mask >> next) & 1)
      {
        continue;
      }
      F64 output_rows = 0.0;
      if (!optimizer_join_extend_rows(graph, mask, rows, next, &output_rows))
      {
        continue;
      }
      if (best == OPT_NO_RELATION || output_rows < best_rows)
      {
        best = next;
        best_rows = output_rows;
      }
    }
    
    if (best == OPT_NO_RELATION)
    {
      return 0;
    }
    total += optimizer_cost_hash_join(model, graph->rows[best], rows, best_rows);
    rows = best_rows;
    mask |= (U64)1 << best;
    out_order[position] = best;
  }
  *out_cost = total;
  return 1;
}

internal B32
optimizer_choose_join_order(OPT_JoinGraph* graph, OPT_CostModel* model, U32 count, U32* out_order)
{
  U32 written[OPT_MAX_RELATIONS] = {0};
  for (U32 index = 0; index < count; index += 1)
  {
    written[index] = index;
    out_order[index] = index;
  }
  
  B32 written_is_valid = 0;
  F64 written_cost = optimizer_join_order_cost(graph, model, written, count, &written_is_valid);
  
  U32 candidate[OPT_MAX_RELATIONS] = {0};
  F64 candidate_cost = 0.0;
  B32 found = 0;
  if (count <= OPT_JOIN_DP_MAX_RELATIONS)
  {
    found = optimizer_join_order_dp(graph, model, count, candidate, &candidate_cost);
  }
  else
  {
    found = optimizer_join_order_greedy(graph, model, count, candidate, &candidate_cost);
  }
  if (!found)
  {
    return 0;
  }
  
  // tec: estimates are noisy, so a valid written order stays unless the new one is clearly better
  if (written_is_valid && candidate_cost >= written_cost * OPT_JOIN_IMPROVEMENT_FACTOR)
  {
    return 0;
  }
  
  B32 changed = 0;
  for (U32 index = 0; index < count; index += 1)
  {
    out_order[index] = candidate[index];
    if (candidate[index] != index)
    {
      changed = 1;
    }
  }
  return changed;
}

//~ tec: deciding whether the written order may change at all

// tec: relations before the first LEFT JOIN commute with each other, the LEFT JOIN and everything after it stay where they were written
internal U32
optimizer_reorderable_group_size(OPT_JoinStep* steps, U32 relation_count)
{
  for (U32 step_index = 1; step_index < relation_count; step_index += 1)
  {
    if (steps[step_index].null_supplying)
    {
      return step_index;
    }
  }
  return relation_count;
}

// tec: an unqualified name that two tables share resolves to the first one in the row set, so moving tables around could change which one it means
internal B32
optimizer_node_has_ambiguous_column(IR_Node* node, OPT_Relation* relations, U32 relation_count)
{
  if (!node)
  {
    return 0;
  }
  
  if (node->type == IR_NodeType_Column)
  {
    if (str8_match(node->value, str8_lit("*"), 0))
    {
      return 0;
    }
    
    String8 bare_name = qe_bare_column_name(node->value);
    B32 is_qualified = bare_name.size != node->value.size;
    if (is_qualified)
    {
      return 0;
    }
    
    U32 match_count = 0;
    for (U32 index = 0; index < relation_count; index += 1)
    {
      if (qe_column_belongs_to_table(relations[index].table, relations[index].alias, node->value))
      {
        match_count += 1;
      }
    }
    return match_count > 1;
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    if (optimizer_node_has_ambiguous_column(child, relations, relation_count))
    {
      return 1;
    }
  }
  return 0;
}

internal B32
optimizer_select_has_ambiguous_columns(IR_Node* select_ir, OPT_Relation* relations, U32 relation_count)
{
  for (IR_Node* child = select_ir->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_Table || child->type == IR_NodeType_CteList)
    {
      continue;
    }
    
    if (child->type == IR_NodeType_Join)
    {
      for (IR_Node* join_child = child->first ? child->first->next : NULL; join_child != NULL; join_child = join_child->next)
      {
        if (optimizer_node_has_ambiguous_column(join_child, relations, relation_count))
        {
          return 1;
        }
      }
      continue;
    }
    
    if (optimizer_node_has_ambiguous_column(child, relations, relation_count))
    {
      return 1;
    }
  }
  return 0;
}

// tec: a condition the optimizer could not tie to relations has no place to move to
internal B32
optimizer_conjuncts_allow_reorder(OPT_Conjunct* conjuncts, U32 conjunct_count, U32 group_size)
{
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    OPT_Conjunct* conjunct = &conjuncts[index];
    B32 in_group = conjunct->destination == OPT_Destination_OnClause && conjunct->target < group_size;
    if (in_group && (conjunct->unresolved || optimizer_mask_popcount(conjunct->mask) < 2))
    {
      return 0;
    }
  }
  return 1;
}

//~ tec: planning an order and turning it into join conditions

// tec: table rows times the selectivity of whatever was pushed onto it
internal void
optimizer_join_estimate_relation_rows(Arena* arena, OPT_Relation* relations, U32 relation_count, OPT_Conjunct* conjuncts, U32 conjunct_count, F64* out_rows)
{
  for (U32 relation_index = 0; relation_index < relation_count; relation_index += 1)
  {
    F64 rows = (F64)relations[relation_index].table->row_count;
    
    IR_Node* filter_nodes[OPT_CONJUNCT_CAPACITY] = {0};
    U32 filter_count = 0;
    for (U32 index = 0; index < conjunct_count; index += 1)
    {
      B32 is_for_relation = conjuncts[index].destination == OPT_Destination_Filter && conjuncts[index].target == relation_index;
      if (is_for_relation)
      {
        filter_nodes[filter_count] = conjuncts[index].node;
        filter_count += 1;
      }
    }
    
    if (filter_count > 0)
    {
      IR_Node* chain = optimizer_ir_and_chain(arena, filter_nodes, filter_count);
      OPT_Relation single = relations[relation_index];
      rows *= optimizer_condition_selectivity(arena, &single, 1, chain);
    }
    out_rows[relation_index] = rows;
  }
}

internal B32
optimizer_plan_join_order(Arena* arena, IR_Node* select_ir, OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count, U32* out_order, OPT_JoinGraph* out_graph)
{
  U32 group_size = optimizer_reorderable_group_size(steps, relation_count);
  if (group_size < 2 || !select_ir)
  {
    return 0;
  }
  if (optimizer_select_has_ambiguous_columns(select_ir, relations, relation_count))
  {
    return 0;
  }
  if (!optimizer_conjuncts_allow_reorder(conjuncts, conjunct_count, group_size))
  {
    return 0;
  }
  
  MemoryZeroStruct(out_graph);
  out_graph->relation_count = group_size;
  optimizer_join_estimate_relation_rows(arena, relations, group_size, conjuncts, conjunct_count, out_graph->rows);
  optimizer_join_collect_edges(relations, relation_count, conjuncts, conjunct_count, group_size, out_graph);
  optimizer_join_add_derived_edges(out_graph);
  
  OPT_CostModel model = optimizer_cost_model_load();
  U32 group_order[OPT_MAX_RELATIONS] = {0};
  B32 changed = optimizer_choose_join_order(out_graph, &model, group_size, group_order);
  if (!changed)
  {
    return 0;
  }
  
  for (U32 index = 0; index < group_size; index += 1)
  {
    out_order[index] = group_order[index];
  }
  return 1;
}

// tec: a conjunct joins the step that brings in its last relation, relations at or after the group keep their written step
internal U32
optimizer_step_position_for_conjunct(OPT_Conjunct* conjunct, U32* position_of, U32 group_size)
{
  if (group_size == 0 || conjunct->target >= group_size)
  {
    return conjunct->target;
  }
  
  U32 last_position = 0;
  for (U32 relation_index = 0; relation_index < group_size; relation_index += 1)
  {
    B32 in_conjunct = (conjunct->mask & ((U64)1 << relation_index)) != 0;
    if (in_conjunct && position_of[relation_index] > last_position)
    {
      last_position = position_of[relation_index];
    }
  }
  return last_position;
}

internal IR_Node*
optimizer_make_derived_equality(Arena* arena, OPT_Relation* relations, OPT_JoinEdge* edge)
{
  OPT_Relation* relation_a = &relations[edge->relation_a];
  OPT_Relation* relation_b = &relations[edge->relation_b];
  String8 qualifier_a = relation_a->alias.size ? relation_a->alias : relation_a->table->name;
  String8 qualifier_b = relation_b->alias.size ? relation_b->alias : relation_b->table->name;
  
  String8 qualified_a = push_str8f(arena, "%.*s.%.*s", str8_varg(qualifier_a), str8_varg(edge->name_a));
  String8 qualified_b = push_str8f(arena, "%.*s.%.*s", str8_varg(qualifier_b), str8_varg(edge->name_b));
  
  IR_Node* comparison = ir_node_make(arena, IR_NodeType_Operator, str8_lit("="));
  IR_Node* left = ir_node_make(arena, IR_NodeType_Column, qualified_a);
  IR_Node* right = ir_node_make(arena, IR_NodeType_Column, qualified_b);
  ir_node_add_child(comparison, left);
  ir_node_add_child(comparison, right);
  return comparison;
}

//~ tec: sizing hints and physical operator choices (top-K, presorted, CPU crossover)

internal B32
optimizer_sizing_enabled(void)
{
  B32 master = optimizer_enabled();
  B32 sizing = settings_bool(str8_lit("QE_OPT_SIZING"), 1);
  return master && sizing;
}

internal B32
optimizer_small_cpu_enabled(void)
{
  B32 master = optimizer_enabled();
  B32 small_cpu = settings_bool(str8_lit("QE_OPT_SMALL_CPU"), 1);
  return master && small_cpu;
}

internal B32
optimizer_top_n_enabled(void)
{
  B32 master = optimizer_enabled();
  B32 top_n = settings_bool(str8_lit("QE_OPT_TOP_N"), 1);
  return master && top_n;
}

internal B32
optimizer_sort_elimination_enabled(void)
{
  B32 master = optimizer_enabled();
  B32 elimination = settings_bool(str8_lit("QE_OPT_SORT_ELIMINATION"), 1);
  return master && elimination;
}

// tec: stages with no CPU work between them share one GPU submit
internal B32
optimizer_fusion_enabled(void)
{
  B32 master = optimizer_enabled();
  B32 fusion = settings_bool(str8_lit("QE_OPT_FUSE"), 1);
  return master && fusion;
}

// tec: dispatch plus the download of the rows, the download rides along when the optimizer fuses
internal F64
optimizer_scan_round_trips(void)
{
  if (optimizer_fusion_enabled())
  {
    return 1.0;
  }
  return 2.0;
}

// tec: build, probe and download unfused, with the prefix sum on the GPU and an output estimate the whole join is one submit
internal F64
optimizer_join_round_trips(void)
{
  if (!optimizer_fusion_enabled())
  {
    return 3.0;
  }
  if (optimizer_sizing_enabled())
  {
    return 1.0;
  }
  return 2.0;
}

internal B32
optimizer_aggregate_uses_approx(IR_Node* column_list)
{
  if (!column_list)
  {
    return 0;
  }
  
  for (IR_Node* item = column_list->first; item != NULL; item = item->next)
  {
    if (item->type == IR_NodeType_AggregateCall)
    {
      U32 func_code = 0;
      B32 known = qe_agg_func_code_from_name(item->value, &func_code);
      if (known && (func_code == QE_AGG_FUNC_APPROX_COUNT_DISTINCT || func_code == QE_AGG_FUNC_APPROX_PERCENTILE))
      {
        return 1;
      }
    }
  }
  return 0;
}

// tec: a group by is an assign dispatch and then a scatter and reduce, without one the assign is skipped
internal F64
optimizer_aggregate_round_trips(PLAN_Node* aggregate)
{
  B32 has_group_by = aggregate->group_by && aggregate->group_by->first;
  B32 exact_functions = !optimizer_aggregate_uses_approx(aggregate->column_list);
  if (optimizer_small_cpu_enabled() && exact_functions && aggregate->input && aggregate->input->has_estimate)
  {
    OPT_CostModel model = optimizer_cost_model_load();
    if (aggregate->input->est_rows <= (F64)optimizer_aggregate_cpu_max_rows(&model))
    {
      return 0.0;
    }
  }
  
  F64 trips = has_group_by ? 3.0 : 2.0;
  if (optimizer_fusion_enabled())
  {
    trips -= 1.0;
  }
  return trips;
}

internal B32
optimizer_subtree_has_aggregate(PLAN_Node* plan)
{
  if (!plan)
  {
    return 0;
  }
  if (plan->type == PLAN_NodeType_Aggregate)
  {
    return 1;
  }
  return optimizer_subtree_has_aggregate(plan->input) || optimizer_subtree_has_aggregate(plan->input2);
}

// tec: the bitonic sort is one submit, but a string key, a small input and anything over an aggregate sorts on the CPU
internal F64
optimizer_sort_round_trips(PLAN_Node* sort)
{
  if (sort->type != PLAN_NodeType_Sort || !sort->input || !sort->input->has_estimate)
  {
    return 0.0;
  }
  if (optimizer_subtree_has_aggregate(sort->input))
  {
    return 0.0;
  }
  if (sort->sort_strategy == PLAN_SortStrategy_Presorted)
  {
    return 0.0;
  }
  
  OPT_Relation relations[OPT_MAX_RELATIONS] = {0};
  U32 relation_count = optimizer_collect_relations(sort->input, relations, 0);
  for (IR_Node* key = sort->order_by ? sort->order_by->first : NULL; key != NULL; key = key->next)
  {
    U32 relation_index = optimizer_resolve_column(relations, relation_count, key->value);
    if (relation_index == OPT_NO_RELATION)
    {
      return 0.0;
    }
    GDB_Column* column = gdb_table_find_column(relations[relation_index].table, qe_bare_column_name(key->value));
    if (!column || column->type == GDB_ColumnType_String8)
    {
      return 0.0;
    }
  }
  
  if (optimizer_small_cpu_enabled())
  {
    OPT_CostModel model = optimizer_cost_model_load();
    if (sort->input->est_rows < (F64)optimizer_sort_gpu_min_rows(&model))
    {
      return 0.0;
    }
  }
  return 1.0;
}

internal F64
optimizer_node_round_trips(PLAN_Node* plan)
{
  switch (plan->type)
  {
    case PLAN_NodeType_Filter:
    {
      B32 over_scan = plan->input && plan->input->type == PLAN_NodeType_Scan;
      B32 impossible = plan->condition && optimizer_ir_is_constant_false(plan->condition->first);
      if (over_scan && !impossible && plan->scan_strategy == PLAN_ScanStrategy_Gpu)
      {
        return optimizer_scan_round_trips();
      }
      return 0.0;
    }
    
    case PLAN_NodeType_Join:
    case PLAN_NodeType_SemiJoin:
    case PLAN_NodeType_AntiJoin:
    {
      return optimizer_join_round_trips();
    }
    
    case PLAN_NodeType_Aggregate:
    {
      return optimizer_aggregate_round_trips(plan);
    }
    
    case PLAN_NodeType_Sort:
    {
      return optimizer_sort_round_trips(plan);
    }
    
    default:
    {
      return 0.0;
    }
  }
}

internal B32
optimizer_plan_uses_estimates(PLAN_Node* plan)
{
  if (!plan)
  {
    return 0;
  }
  
  B32 reads_estimate = plan->type == PLAN_NodeType_Join || plan->type == PLAN_NodeType_Aggregate || plan->type == PLAN_NodeType_Sort;
  reads_estimate = reads_estimate || plan->type == PLAN_NodeType_SemiJoin || plan->type == PLAN_NodeType_AntiJoin;
  if (reads_estimate)
  {
    return 1;
  }
  return optimizer_plan_uses_estimates(plan->input) || optimizer_plan_uses_estimates(plan->input2);
}

internal U64
optimizer_rows_to_hint(F64 rows)
{
  if (!(rows > 0.0))
  {
    return 0;
  }
  if (rows > OPT_HINT_MAX_ROWS)
  {
    rows = OPT_HINT_MAX_ROWS;
  }
  return (U64)ceil_f64(rows);
}

//~ tec: hints

internal void
optimizer_join_hints(PLAN_Node* join, QE_JoinHints* out_hints)
{
  MemoryZeroStruct(out_hints);
  out_hints->fuse_round_trips = optimizer_fusion_enabled();
  if (!optimizer_sizing_enabled() || !join->has_estimate)
  {
    return;
  }
  out_hints->output_rows = optimizer_rows_to_hint(join->est_hash_rows);
}

internal void
optimizer_aggregate_hints(PLAN_Node* aggregate, QE_AggregateHints* out_hints)
{
  MemoryZeroStruct(out_hints);
  out_hints->fuse_round_trips = optimizer_fusion_enabled();
  if (optimizer_sizing_enabled() && aggregate->has_estimate)
  {
    out_hints->group_count = optimizer_rows_to_hint(aggregate->est_rows);
  }
  if (optimizer_small_cpu_enabled())
  {
    OPT_CostModel model = optimizer_cost_model_load();
    out_hints->cpu_max_rows = optimizer_aggregate_cpu_max_rows(&model);
  }
}

internal void
optimizer_sort_hints(PLAN_Node* sort, QE_SortHints* out_hints)
{
  MemoryZeroStruct(out_hints);
  if (optimizer_small_cpu_enabled())
  {
    OPT_CostModel model = optimizer_cost_model_load();
    out_hints->gpu_min_rows = optimizer_sort_gpu_min_rows(&model);
  }
  
  if (sort->type == PLAN_NodeType_TopN)
  {
    U64 keep = 0;
    if (optimizer_limit_keep_count(sort, &keep))
    {
      out_hints->top_k = keep;
    }
  }
}

//~ tec: ordering

// tec: offset plus limit, the number of rows in order the node has to produce
internal B32
optimizer_limit_keep_count(PLAN_Node* node, U64* out_keep)
{
  if (!node->limit_node)
  {
    return 0;
  }
  
  U64 limit = u64_from_str8(node->limit_node->value, 10);
  U64 offset = 0;
  if (node->offset_node)
  {
    offset = u64_from_str8(node->offset_node->value, 10);
  }
  *out_keep = limit + offset;
  return 1;
}

internal B32
optimizer_find_order_source(PLAN_Node* input, OPT_OrderSource* out_source)
{
  MemoryZeroStruct(out_source);
  
  PLAN_Node* node = input;
  while (node && node->type == PLAN_NodeType_Project)
  {
    node = node->input;
  }
  if (!node)
  {
    return 0;
  }
  
  PLAN_Node* scan = NULL;
  if (node->type == PLAN_NodeType_Filter && node->input && node->input->type == PLAN_NodeType_Scan)
  {
    out_source->filter = node;
    if (node->condition)
    {
      out_source->condition = node->condition->first;
    }
    scan = node->input;
  }
  else if (node->type == PLAN_NodeType_Scan)
  {
    scan = node;
  }
  
  if (!scan || !scan->table)
  {
    return 0;
  }
  out_source->scan = scan;
  out_source->table = scan->table;
  return 1;
}

// tec: one plain column with no NULLs, which is what an index orders the same way the sort does
internal B32
optimizer_order_key(OPT_OrderSource* source, IR_Node* order_by, GDB_Column** out_column, B32* out_descending)
{
  if (!order_by || !order_by->first || order_by->first->next)
  {
    return 0;
  }
  
  IR_Node* key = order_by->first;
  if (key->type != IR_NodeType_Column)
  {
    return 0;
  }
  if (!qe_column_belongs_to_table(source->table, source->scan->alias, key->value))
  {
    return 0;
  }
  
  GDB_Column* column = gdb_table_find_column(source->table, qe_bare_column_name(key->value));
  if (!column || column->null_flags != NULL)
  {
    return 0;
  }
  
  *out_column = column;
  *out_descending = key->first && key->first->type == IR_NodeType_Descending;
  return 1;
}

// tec: an index scan hands its rows over in key order, so a sort on that same column has nothing left to do
internal B32
optimizer_choose_presorted(Arena* arena, PLAN_Node* sort, OPT_OrderSource* source, GDB_Column* key_column, B32 descending)
{
  PLAN_Node* filter = source->filter;
  if (!filter || filter->scan_strategy != PLAN_ScanStrategy_Index || !filter->index_leaf)
  {
    return 0;
  }
  
  GDB_Column* leaf_columns[OPT_SCAN_COLUMN_CAPACITY] = {0};
  U32 leaf_column_count = 0;
  B32 unresolved = 0;
  optimizer_collect_condition_columns(source->table, filter->index_leaf, leaf_columns, &leaf_column_count, OPT_SCAN_COLUMN_CAPACITY, &unresolved);
  if (unresolved || leaf_column_count != 1 || leaf_columns[0] != key_column)
  {
    return 0;
  }
  
  GDB_Index* index = gdb_table_find_index_on_column(source->table, key_column);
  if (!index)
  {
    return 0;
  }
  
  sort->sort_strategy = PLAN_SortStrategy_Presorted;
  sort->order_index = index;
  sort->order_descending = descending;
  sort->sort_reason = push_str8f(arena, "index scan on '%.*s' already returns this order", str8_varg(key_column->name));
  return 1;
}

// tec: reading the index in order costs the rows walked until K pass the filter, against reading the whole input and keeping K
internal B32
optimizer_choose_index_walk(Arena* arena, OPT_CostModel* model, PLAN_Node* top_n, OPT_OrderSource* source, GDB_Column* key_column, B32 descending, U64 keep)
{
  GDB_Index* index = gdb_table_find_index_on_column(source->table, key_column);
  F64 rows = (F64)source->table->row_count;
  if (!index || rows < 1.0)
  {
    return 0;
  }
  
  F64 selectivity = 1.0;
  F64 input_cost = optimizer_cost_identity(model, rows);
  F64 walk_per_row = model->fill_per_row_us;
  if (source->filter)
  {
    if (!source->filter->has_estimate)
    {
      return 0;
    }
    selectivity = Max(source->filter->est_rows / rows, 1.0 / rows);
    input_cost = source->filter->est_cost_us;
    walk_per_row = model->filter_per_row_us;
  }
  
  F64 walked_rows = Min(rows, (F64)keep / selectivity);
  F64 walk_cost = model->index_fixed_us + walked_rows * walk_per_row;
  F64 heap_cost = input_cost + optimizer_cost_top_n(model, rows * selectivity);
  if (walk_cost >= heap_cost)
  {
    return 0;
  }
  
  top_n->sort_strategy = PLAN_SortStrategy_IndexWalk;
  top_n->order_index = index;
  top_n->order_descending = descending;
  top_n->est_cost_us = walk_cost;
  top_n->sort_reason = push_str8f(arena, "walks index on '%.*s' for about %.0f rows instead of reading %.0f", str8_varg(key_column->name), walked_rows, rows);
  return 1;
}

// tec: the ordering nodes are always the top of a select's plan, a Sort or a Sort under a Limit
internal PLAN_Node*
optimizer_plan_ordering(Arena* arena, PLAN_Node* root)
{
  if (!root || !optimizer_enabled())
  {
    return root;
  }
  
  PLAN_Node* limit = NULL;
  PLAN_Node* sort = root;
  if (root->type == PLAN_NodeType_Limit && root->input)
  {
    limit = root;
    sort = root->input;
  }
  if (sort->type != PLAN_NodeType_Sort)
  {
    return root;
  }
  
  OPT_OrderSource source = {0};
  GDB_Column* key_column = NULL;
  B32 descending = 0;
  B32 has_source = optimizer_find_order_source(sort->input, &source);
  B32 has_key = has_source && optimizer_order_key(&source, sort->order_by, &key_column, &descending);
  
  if (has_key && optimizer_sort_elimination_enabled() && optimizer_choose_presorted(arena, sort, &source, key_column, descending))
  {
    return root;
  }
  
  U64 keep = 0;
  U64 max_keep = settings_u64(str8_lit("QE_OPT_TOP_N_MAX_K"), OPT_TOP_N_DEFAULT_MAX_K);
  B32 can_fuse = limit && optimizer_top_n_enabled() && optimizer_limit_keep_count(limit, &keep);
  if (!can_fuse || keep == 0 || keep > max_keep)
  {
    return root;
  }
  
  PLAN_Node* top_n = plan_node_make(arena, PLAN_NodeType_TopN);
  top_n->input = sort->input;
  top_n->order_by = sort->order_by;
  top_n->limit_node = limit->limit_node;
  top_n->offset_node = limit->offset_node;
  top_n->has_estimate = limit->has_estimate;
  top_n->est_rows = limit->est_rows;
  top_n->sort_strategy = PLAN_SortStrategy_Heap;
  top_n->sort_reason = push_str8f(arena, "keeps the first %llu rows in a heap", keep);
  
  if (has_key)
  {
    OPT_CostModel model = optimizer_cost_model_load();
    optimizer_choose_index_walk(arena, &model, top_n, &source, key_column, descending, keep);
  }
  return top_n;
}

//~ tec: semi / anti join extraction (EXISTS, IN over a subquery)

internal B32
optimizer_semi_join_enabled(void)
{
  B32 master = optimizer_enabled();
  B32 semi = settings_bool(str8_lit("QE_OPT_SEMI_JOIN"), 1);
  return master && semi;
}

internal String8
optimizer_outer_table_label(OPT_OuterTable* table)
{
  if (table->alias.size > 0)
  {
    return table->alias;
  }
  return table->table->name;
}

internal U32
optimizer_semi_match_tables(OPT_OuterTable* tables, U32 count, String8 name, U32* out_first)
{
  String8 qualifier = {0};
  String8 bare = name;
  for (U64 index = 0; index < name.size; index += 1)
  {
    if (name.str[index] == '.')
    {
      qualifier = str8_prefix(name, index);
      bare = str8_skip(name, index + 1);
      break;
    }
  }
  
  U32 matches = 0;
  for (U32 table_index = 0; table_index < count; table_index += 1)
  {
    if (qualifier.size > 0)
    {
      String8 label = optimizer_outer_table_label(&tables[table_index]);
      if (!str8_match(label, qualifier, StringMatchFlag_CaseInsensitive))
      {
        continue;
      }
    }
    if (gdb_table_find_column(tables[table_index].table, bare))
    {
      if (matches == 0)
      {
        *out_first = table_index;
      }
      matches += 1;
    }
  }
  return matches;
}

// tec: a derived table or a table that is not there yet is not known here, so the whole select is left to the old path
internal B32
optimizer_collect_outer_tables(GDB_Database* database, IR_Node* select_ir, OPT_OuterTable* out_tables, U32* out_count)
{
  U32 count = 0;
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
    
    GDB_Table* table = NULL;
    if (table_ir->value.size > 0)
    {
      table = plan_find_table_quiet(database, table_ir->value);
    }
    if (!table || count >= OPT_SEMI_OUTER_CAPACITY)
    {
      return 0;
    }
    out_tables[count].table = table;
    out_tables[count].alias = plan_alias_from_table_ir(table_ir);
    count += 1;
  }
  *out_count = count;
  return count > 0;
}

internal void
optimizer_semi_classify_columns(IR_Node* node, OPT_OuterTable* inner, OPT_OuterTable* outer, U32 outer_count, U32* io_inner_refs, U32* io_outer_refs, B32* out_unsupported)
{
  if (!node)
  {
    return;
  }
  
  if (node->type == IR_NodeType_Subquery || node->type == IR_NodeType_Exists)
  {
    *out_unsupported = 1;
    return;
  }
  
  if (node->type == IR_NodeType_Column && !str8_match(node->value, str8_lit("*"), 0))
  {
    U32 first_table = 0;
    if (optimizer_semi_match_tables(inner, 1, node->value, &first_table) > 0)
    {
      *io_inner_refs += 1;
    }
    else if (optimizer_semi_match_tables(outer, outer_count, node->value, &first_table) == 1)
    {
      *io_outer_refs += 1;
    }
    else
    {
      *out_unsupported = 1;
    }
    return;
  }
  
  for (IR_Node* child = node->first; child != NULL; child = child->next)
  {
    optimizer_semi_classify_columns(child, inner, outer, outer_count, io_inner_refs, io_outer_refs, out_unsupported);
  }
}

internal IR_Node*
optimizer_make_semi_node(Arena* arena, String8 kind, String8 outer_key, String8 inner_key, String8 table_name, String8 alias, IR_Node* filter)
{
  IR_Node* semi = ir_node_make(arena, IR_NodeType_SemiJoin, kind);
  ir_node_add_child(semi, ir_node_make(arena, IR_NodeType_Column, outer_key));
  ir_node_add_child(semi, ir_node_make(arena, IR_NodeType_Column, inner_key));
  
  IR_Node* table_ir = ir_node_make(arena, IR_NodeType_Table, table_name);
  ir_node_add_child(table_ir, ir_node_make(arena, IR_NodeType_Alias, alias));
  ir_node_add_child(semi, table_ir);
  
  if (filter)
  {
    ir_node_add_child(semi, filter);
  }
  return semi;
}

// tec: the join kernels know nothing about NULL, so a key that can hold one is left to the old path
internal B32
optimizer_semi_keys_compatible(GDB_Column* outer_key, GDB_Column* inner_key)
{
  if (outer_key->null_flags != NULL || inner_key->null_flags != NULL)
  {
    return 0;
  }
  B32 outer_is_string = outer_key->type == GDB_ColumnType_String8;
  B32 inner_is_string = inner_key->type == GDB_ColumnType_String8;
  return outer_is_string == inner_is_string;
}

internal B32
optimizer_try_exists_semi(Arena* arena, GDB_Database* database, IR_Node* exists_node, OPT_OuterTable* outer, U32 outer_count, IR_Node** out_semi)
{
  IR_Node* inner_select = exists_node->first;
  if (!inner_select || inner_select->type != IR_NodeType_Select)
  {
    return 0;
  }
  
  IR_Node* table_ir = NULL;
  IR_Node* where_ir = NULL;
  for (IR_Node* child = inner_select->first; child != NULL; child = child->next)
  {
    if (child->type == IR_NodeType_Table)
    {
      if (table_ir)
      {
        return 0;
      }
      table_ir = child;
    }
    else if (child->type == IR_NodeType_Where)
    {
      where_ir = child;
    }
    else if (child->type == IR_NodeType_ColumnList)
    {
      // tec: an aggregate without GROUP BY always returns a row, so EXISTS over one is always true
      if (plan_ir_contains_aggregate(child->first))
      {
        return 0;
      }
    }
    else if (child->type != IR_NodeType_OrderBy)
    {
      return 0;
    }
  }
  if (!table_ir || !where_ir || !where_ir->first || table_ir->value.size == 0 || ir_node_find_child(table_ir, IR_NodeType_Select))
  {
    return 0;
  }
  
  GDB_Table* inner_table = plan_find_table_quiet(database, table_ir->value);
  if (!inner_table)
  {
    return 0;
  }
  OPT_OuterTable inner = {0};
  inner.table = inner_table;
  inner.alias = plan_alias_from_table_ir(table_ir);
  
  IR_Node* conjuncts[OPT_CONJUNCT_CAPACITY] = {0};
  U32 conjunct_count = optimizer_ir_collect_conjuncts(where_ir->first, conjuncts, 0, OPT_MAX_CONJUNCTS);
  if (conjunct_count > OPT_MAX_CONJUNCTS)
  {
    return 0;
  }
  
  IR_Node* residuals[OPT_CONJUNCT_CAPACITY] = {0};
  U32 residual_count = 0;
  IR_Node* inner_key_node = NULL;
  IR_Node* outer_key_node = NULL;
  U32 outer_table_index = 0;
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    IR_Node* conjunct = conjuncts[index];
    U32 inner_refs = 0;
    U32 outer_refs = 0;
    B32 unsupported = 0;
    optimizer_semi_classify_columns(conjunct, &inner, outer, outer_count, &inner_refs, &outer_refs, &unsupported);
    if (unsupported)
    {
      return 0;
    }
    
    if (outer_refs == 0)
    {
      residuals[residual_count] = conjunct;
      residual_count += 1;
      continue;
    }
    
    // tec: the only correlation supported is one equality between an inner column and an outer one
    IR_Node* first = conjunct->first;
    IR_Node* second = first ? first->next : NULL;
    B32 is_equality = optimizer_ir_is_operator(conjunct, str8_lit("="));
    B32 shaped = is_equality && first && second && !second->next && first->type == IR_NodeType_Column && second->type == IR_NodeType_Column;
    if (!shaped || inner_key_node)
    {
      return 0;
    }
    
    U32 unused_table = 0;
    B32 first_is_inner = optimizer_semi_match_tables(&inner, 1, first->value, &unused_table) > 0;
    B32 second_is_inner = optimizer_semi_match_tables(&inner, 1, second->value, &unused_table) > 0;
    if (first_is_inner == second_is_inner)
    {
      return 0;
    }
    inner_key_node = first_is_inner ? first : second;
    outer_key_node = first_is_inner ? second : first;
    if (optimizer_semi_match_tables(outer, outer_count, outer_key_node->value, &outer_table_index) != 1)
    {
      return 0;
    }
  }
  
  // tec: no correlation means the subquery is a constant, which the old path already handles
  if (!inner_key_node)
  {
    return 0;
  }
  
  GDB_Column* inner_key = gdb_table_find_column(inner_table, qe_bare_column_name(inner_key_node->value));
  GDB_Column* outer_key = gdb_table_find_column(outer[outer_table_index].table, qe_bare_column_name(outer_key_node->value));
  if (!inner_key || !outer_key || !optimizer_semi_keys_compatible(outer_key, inner_key))
  {
    return 0;
  }
  
  IR_Node* filter = NULL;
  if (residual_count > 0)
  {
    IR_Node* chain = optimizer_ir_and_chain(arena, residuals, residual_count);
    optimizer_ir_strip_column_qualifiers(chain);
    filter = optimizer_ir_make_where(arena, chain);
  }
  
  g_optimizer_semi_serial += 1;
  String8 alias = push_str8f(arena, "__semi_%llu", g_optimizer_semi_serial);
  String8 outer_name = push_str8f(arena, "%.*s.%.*s", str8_varg(optimizer_outer_table_label(&outer[outer_table_index])), str8_varg(outer_key->name));
  String8 inner_name = push_str8f(arena, "%.*s.%.*s", str8_varg(alias), str8_varg(inner_key->name));
  B32 is_negated = str8_match(exists_node->value, str8_lit("not"), 0);
  String8 kind = is_negated ? str8_lit("anti") : str8_lit("semi");
  *out_semi = optimizer_make_semi_node(arena, kind, outer_name, inner_name, table_ir->value, alias, filter);
  return 1;
}

// tec: a list that fits the GPU scan is one dispatch, past that the CPU scan costs a pass over the outer rows and the semi join costs a hash join
internal B32
optimizer_semi_pays_off(OPT_CostModel* model, U64 list_count, F64 outer_rows)
{
  U64 gpu_list_limit = settings_u64(str8_lit("QE_IN_LIST_GPU_MAX_ITEMS"), 32);
  if (list_count <= gpu_list_limit)
  {
    return 0;
  }
  
  F64 scan_cost = optimizer_cost_cpu_scan(model, outer_rows, 1);
  F64 join_cost = optimizer_cost_hash_join(model, (F64)list_count, outer_rows, outer_rows * OPT_SEMI_DEFAULT_MATCH_FRACTION);
  return join_cost < scan_cost;
}

internal B32
optimizer_try_in_semi(Arena* arena, GDB_Database* database, IR_Node* in_node, OPT_OuterTable* outer, U32 outer_count, IR_Node** out_semi, B32* out_failed)
{
  B32 is_in = str8_match(in_node->value, str8_lit("in"), StringMatchFlag_CaseInsensitive);
  B32 is_not_in = str8_match(in_node->value, str8_lit("not in"), StringMatchFlag_CaseInsensitive);
  IR_Node* left = in_node->first;
  IR_Node* right = left ? left->next : NULL;
  B32 shaped = (is_in || is_not_in) && left && right && left->type == IR_NodeType_Column && right->type == IR_NodeType_Subquery;
  if (!shaped)
  {
    return 0;
  }
  
  U32 outer_table_index = 0;
  if (optimizer_semi_match_tables(outer, outer_count, left->value, &outer_table_index) != 1)
  {
    return 0;
  }
  GDB_Table* outer_table = outer[outer_table_index].table;
  GDB_Column* outer_key = gdb_table_find_column(outer_table, qe_bare_column_name(left->value));
  if (!outer_key || outer_key->null_flags != NULL)
  {
    return 0;
  }
  
  PLAN_Materialized values = {0};
  if (!plan_run_subquery_values(arena, database, right, &values))
  {
    *out_failed = 1;
    return 0;
  }
  
  B32 saw_null = 0;
  PLAN_AggColumn* value_column = &values.columns[0];
  if (value_column->is_null)
  {
    for (U64 row = 0; row < values.count; row += 1)
    {
      if (value_column->is_null[row])
      {
        saw_null = 1;
      }
    }
  }
  
  B32 outer_is_string = outer_key->type == GDB_ColumnType_String8;
  B32 value_is_string = value_column->type == GDB_ColumnType_String8;
  OPT_CostModel model = optimizer_cost_model_load();
  B32 pays_off = optimizer_semi_pays_off(&model, values.count, (F64)outer_table->row_count);
  if (saw_null || outer_is_string != value_is_string || !pays_off)
  {
    plan_apply_subquery_in_list(arena, in_node, right, &values, is_not_in);
    return 0;
  }
  
  g_optimizer_semi_serial += 1;
  String8 table_name = push_str8f(arena, "__semi_%llu", g_optimizer_semi_serial);
  GDB_Table* temp_table = plan_materialized_to_temp_table(database, table_name, &values);
  if (!temp_table)
  {
    *out_failed = 1;
    return 0;
  }
  
  String8 outer_name = push_str8f(arena, "%.*s.%.*s", str8_varg(optimizer_outer_table_label(&outer[outer_table_index])), str8_varg(outer_key->name));
  String8 inner_name = push_str8f(arena, "%.*s.%.*s", str8_varg(table_name), str8_varg(temp_table->columns[0]->name));
  String8 kind = is_not_in ? str8_lit("anti") : str8_lit("semi");
  *out_semi = optimizer_make_semi_node(arena, kind, outer_name, inner_name, table_name, table_name, NULL);
  return 1;
}

internal B32
optimizer_extract_semi_joins(Arena* arena, GDB_Database* database, IR_Node* select_ir, B32 may_execute)
{
  if (!optimizer_semi_join_enabled())
  {
    return 1;
  }
  
  IR_Node* where_ir = ir_node_find_child(select_ir, IR_NodeType_Where);
  if (!where_ir || !where_ir->first)
  {
    return 1;
  }
  
  OPT_OuterTable outer[OPT_SEMI_OUTER_CAPACITY] = {0};
  U32 outer_count = 0;
  if (!optimizer_collect_outer_tables(database, select_ir, outer, &outer_count))
  {
    return 1;
  }
  
  IR_Node* conjuncts[OPT_CONJUNCT_CAPACITY] = {0};
  U32 conjunct_count = optimizer_ir_collect_conjuncts(where_ir->first, conjuncts, 0, OPT_MAX_CONJUNCTS);
  if (conjunct_count > OPT_MAX_CONJUNCTS)
  {
    return 1;
  }
  
  IR_Node* semi_nodes[OPT_CONJUNCT_CAPACITY] = {0};
  U32 semi_count = 0;
  B32 removed[OPT_CONJUNCT_CAPACITY] = {0};
  
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    IR_Node* conjunct = conjuncts[index];
    IR_Node* semi = NULL;
    B32 extracted = 0;
    B32 failed = 0;
    
    if (conjunct->type == IR_NodeType_Exists)
    {
      extracted = optimizer_try_exists_semi(arena, database, conjunct, outer, outer_count, &semi);
    }
    else if (may_execute && conjunct->type == IR_NodeType_Operator)
    {
      extracted = optimizer_try_in_semi(arena, database, conjunct, outer, outer_count, &semi, &failed);
    }
    
    if (failed)
    {
      return 0;
    }
    if (extracted)
    {
      semi_nodes[semi_count] = semi;
      semi_count += 1;
      removed[index] = 1;
    }
  }
  
  if (semi_count == 0)
  {
    return 1;
  }
  
  IR_Node* kept[OPT_CONJUNCT_CAPACITY] = {0};
  U32 kept_count = 0;
  for (U32 index = 0; index < conjunct_count; index += 1)
  {
    if (!removed[index])
    {
      kept[kept_count] = conjuncts[index];
      kept_count += 1;
    }
  }
  
  if (kept_count == 0)
  {
    optimizer_ir_remove_child(select_ir, where_ir);
  }
  else
  {
    IR_Node* chain = optimizer_ir_and_chain(arena, kept, kept_count);
    where_ir->first = NULL;
    where_ir->last = NULL;
    ir_node_add_child(where_ir, chain);
  }
  
  for (U32 index = 0; index < semi_count; index += 1)
  {
    ir_node_add_child(select_ir, semi_nodes[index]);
  }
  return 1;
}
