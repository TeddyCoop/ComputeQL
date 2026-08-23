internal B32
sql_is_string_keyword(String8 string)
{
  for (U32 i = 0; i < ArrayCount(g_sql_keywords); i++)
  {
    if (str8_match(g_sql_keywords[i], string, StringMatchFlag_CaseInsensitive))
    {
      return 1;
    }
  }
  return 0;
}

internal B32
sql_is_string_operator(String8 string)
{
  for (U32 i = 0; i < ArrayCount(g_sql_operators); i++)
  {
    if (str8_match(g_sql_operators[i], string, StringMatchFlag_CaseInsensitive))
    {
      return 1;
    }
  }
  return 0;
}

internal B32
sql_is_string_symbol(String8 string)
{
  for (U32 i = 0; i < ArrayCount(g_sql_symbols); i++)
  {
    if (str8_match(g_sql_symbols[i], string, StringMatchFlag_CaseInsensitive))
    {
      return 1;
    }
  }
  return 0;
}

internal SQL_TokenizeResult
sql_tokenize_from_text(Arena* arena, String8 text)
{
  ProfBeginFunction();
  
  SQL_Token* tokens = push_array(arena, SQL_Token, 2056);
  U64 token_count = 0;
  
  U64 pos = 0;
  while (pos < text.size)
  {
    while (pos < text.size && char_is_space(text.str[pos]))
    {
      pos++;
    }
    
    if (pos >= text.size) break;
    
    U64 start = pos;
    
    if (text.str[pos] == '\'')
    {
      pos++;
      start = pos;
      
      while (pos < text.size && text.str[pos] != '\'')
      {
        pos++;
      }
      
      if (pos >= text.size)
      {
        log_error("Unterminated string literal.");
        break;
      }
      
      String8 token_value = str8_substr(text, r1u64(start, pos));
      tokens[token_count++] = (SQL_Token)
      {
        .type = SQL_TokenType_String,
        .value = token_value,
        .range = r1u64(start - 1, pos + 1)
      };
      
      pos++;
    }
    else if (char_is_digit(text.str[pos], 10))
    {
      B32 has_dot = 0;
      
      while (pos < text.size && (char_is_digit(text.str[pos], 10) || (!has_dot && text.str[pos] == '.')))
      {
        if (text.str[pos] == '.')
        {
          has_dot = 1;
          if (pos + 1 >= text.size || !char_is_digit(text.str[pos + 1], 10))
          {
            break;
          }
        }
        pos++;
      }
      
      String8 token_value = str8_substr(text, r1u64(start, pos));
      tokens[token_count++] = (SQL_Token)
      {
        .type = SQL_TokenType_Number,
        .value = token_value,
        .range = r1u64(start, pos)
      };
    }
    //- tec: keywords and identifiers
    else if (char_is_alpha(text.str[pos]))
    {
      while (pos < text.size && ((char_is_digit(text.str[pos], 10) || char_is_alpha(text.str[pos])) || text.str[pos] == '_'))
      {
        pos++;
      }
      String8 token_value = str8_substr(text, r1u64(start, pos));
      tokens[token_count++] = (SQL_Token)
      {
        .type = sql_is_string_keyword(token_value) ? SQL_TokenType_Keyword : SQL_TokenType_Identifier,
        .value = token_value,
        .range = r1u64(start, pos),
      };
    }
    //- tec: symbols
    else if (sql_is_string_symbol(str8_substr(text, r1u64(pos, pos+1))))
    {
      tokens[token_count++] = (SQL_Token)
      {
        .type = SQL_TokenType_Symbol,
        .value = str8_substr(text, r1u64(pos, pos + 1)),
        .range = r1u64(pos, pos + 1)
      };
      pos++;
    }
    //- tec: operators and other tokens
    else
    {
      while (pos < text.size &&
             !char_is_space(text.str[pos]) &&
             !char_is_alpha(text.str[pos]) && !char_is_digit(text.str[pos], 10) && !sql_is_string_symbol(str8_substr(text, r1u64(pos, pos+1))))
      {
        pos++;
      }
      String8 token_value = str8_substr(text, r1u64(start, pos));
      if (sql_is_string_operator(token_value))
      {
        tokens[token_count++] = (SQL_Token)
        {
          .type = SQL_TokenType_Operator,
          .value = token_value,
          .range = r1u64(start, pos)
        };
      }
    }
  }
  
  SQL_TokenizeResult result = { 0 };
  result.tokens = tokens;
  result.count = token_count;
  
  ProfEnd();
  return result;
}

internal void
sql_tokens_print(SQL_TokenizeResult tokens)
{
  for (U64 i = 0; i < tokens.count; i++)
  {
    SQL_Token* token = &tokens.tokens[i];
    
    String8 token_type = str8_lit("no type");
    switch (token->type)
    {
      case SQL_TokenType_Keyword: token_type = str8_lit("SQL_TokenType_Keyword"); break;
      case SQL_TokenType_Identifier: token_type = str8_lit("SQL_TokenType_Identifier"); break;
      case SQL_TokenType_Operator: token_type = str8_lit("SQL_TokenType_Operator"); break;
      case SQL_TokenType_Symbol: token_type = str8_lit("SQL_TokenType_Symbol"); break;
      case SQL_TokenType_Number: token_type = str8_lit("SQL_TokenType_Number"); break;
      case SQL_TokenType_String: token_type = str8_lit("SQL_TokenType_String"); break;
    }
    
    printf("\'%.*s\' : index=%llu : type=%.*s\n", (int)token->value.size, token->value.str,
           i, (int)token_type.size, token_type.str);
  }
}

//~ tec: parse error reporting

internal Rng1U64
sql_token_range_at(SQL_Token *tokens, U64 token_index, U64 token_count)
{
  if (token_count == 0) return r1u64(0, 0);
  if (token_index < token_count) return tokens[token_index].range;
  return tokens[token_count - 1].range;
}

internal String8
sql_token_text_or_eof(SQL_Token *tokens, U64 token_index, U64 token_count)
{
  return (token_index < token_count) ? tokens[token_index].value : str8_lit("<end of input>");
}

internal void
sql_parse_error_at(Rng1U64 range, char *fmt, ...)
{
  // tec: keep only the first error - once the token stream is out of sync, every
  // subsequent parse function tends to fail too, and those are just noise.
  if (g_sql_parse_error.has_error) return;
  
  Temp scratch = scratch_begin(0, 0);
  
  va_list args;
  va_start(args, fmt);
  String8 message = push_str8fv(scratch.arena, fmt, args);
  va_end(args);
  
  U64 clamped_min = Min(range.min, g_sql_source_text.size);
  U64 line = 1, col = 1, line_start = 0;
  for (U64 i = 0; i < clamped_min; i++)
  {
    if (g_sql_source_text.str[i] == '\n')
    {
      line++;
      col = 1;
      line_start = i + 1;
    }
    else
    {
      col++;
    }
  }
  
  U64 line_end = line_start;
  while (line_end < g_sql_source_text.size && g_sql_source_text.str[line_end] != '\n')
  {
    line_end++;
  }
  String8 line_text = str8_substr(g_sql_source_text, r1u64(line_start, line_end));
  
  log_error("sql syntax error (line %llu, col %llu): %.*s", line, col, str8_varg(message));
  if (line_text.size > 0)
  {
    log_error("  %.*s", str8_varg(line_text));
    log_error("  %*s^", (int)(col > 0 ? col - 1 : 0), "");
  }
  
  g_sql_parse_error.has_error = 1;
  g_sql_parse_error.range = range;
  
  scratch_end(scratch);
}

//~ tec: ast
internal SQL_Node*
sql_parse(Arena* arena, SQL_Token* tokens, U64 token_count, String8 source_text)
{
  ProfBeginFunction();
  
  g_sql_parse_error = (SQL_ParseError){0};
  g_sql_source_text = source_text;
  
  U64 token_index = 0;
  SQL_Node *root = NULL;
  SQL_Node *current_node = NULL;
  SQL_Node *last_select_node = NULL;
  
  while (token_index < token_count)
  {
    SQL_Token *token = &tokens[token_index];
    B32 attach_to_select = 0;
    
    if (token->type == SQL_TokenType_Symbol && str8_match(token->value, str8_lit(";"), 0))
    {
      token_index++;
      last_select_node = NULL;
      continue;
    }
    
    if (token->type == SQL_TokenType_Keyword)
    {
      SQL_Node *new_node = NULL;
      
      if (str8_match(token->value, str8_lit("use"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_use_clause(arena, &tokens, &token_index, token_count);
      }
      else if (str8_match(token->value, str8_lit("select"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_select_clause(arena, &tokens, &token_index, token_count);
        last_select_node = new_node;
      }
      else if (str8_match(token->value, str8_lit("from"), StringMatchFlag_CaseInsensitive))
      {
        if (!last_select_node)
        {
          sql_parse_error_at(token->range, "'from' clause without a preceding 'select'");
        }
        else
        {
          // tec: attaches tables/joins directly onto last_select_node - a FROM clause can add more than one sibling node
          sql_parse_from_clause(arena, &tokens, &token_index, token_count, last_select_node);
        }
        attach_to_select = 1;
      }
      else if (str8_match(token->value, str8_lit("where"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_where_clause(arena, &tokens, &token_index, token_count);
        attach_to_select = 1;
      }
      else if (str8_match(token->value, str8_lit("group"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_group_by_clause(arena, &tokens, &token_index, token_count);
        attach_to_select = 1;
      }
      else if (str8_match(token->value, str8_lit("having"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_having_clause(arena, &tokens, &token_index, token_count);
        attach_to_select = 1;
      }
      else if (str8_match(token->value, str8_lit("order"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_order_by_clause(arena, &tokens, &token_index, token_count);
        attach_to_select = 1;
      }
      else if (str8_match(token->value, str8_lit("limit"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_limit_clause(arena, &tokens, &token_index, token_count);
        attach_to_select = 1;
      }
      else if (str8_match(token->value, str8_lit("offset"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_offset_clause(arena, &tokens, &token_index, token_count);
        attach_to_select = 1;
      }
      else if (str8_match(token->value, str8_lit("insert"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_insert_clause(arena, &tokens, &token_index, token_count);
      }
      else if (str8_match(token->value, str8_lit("import"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_import_clause(arena, &tokens, &token_index, token_count);
      }
      else if (str8_match(token->value, str8_lit("create"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_create_clause(arena, &tokens, &token_index, token_count);
      }
      else if (str8_match(token->value, str8_lit("alter"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_alter_clause(arena, &tokens, &token_index, token_count);
      }
      else if (str8_match(token->value, str8_lit("drop"), StringMatchFlag_CaseInsensitive))
      {
        // tec: only DROP INDEX is implemented, but DROP TABLE/DATABASE don't exist yet
        new_node = sql_parse_drop_index_clause(arena, &tokens, &token_index, token_count);
      }
      else if (str8_match(token->value, str8_lit("delete"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_delete_clause(arena, &tokens, &token_index, token_count);
        last_select_node = new_node;
      }
      else if (str8_match(token->value, str8_lit("describe"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_describe_clause(arena, &tokens, &token_index, token_count);
      }
      else
      {
        sql_parse_error_at(token->range, "unexpected keyword '%.*s'", str8_varg(token->value));
      }
      
      if (attach_to_select)
      {
        if (new_node && !last_select_node)
        {
          sql_parse_error_at(token->range, "clause requires a preceding 'select'");
        }
        else if (new_node)
        {
          new_node->parent = last_select_node;
          DLLPushBack(last_select_node->first, last_select_node->last, new_node);
        }
      }
      else if (new_node)
      {
        if (!root)
        {
          root = new_node;
          current_node = new_node;
        }
        else
        {
          current_node->next = new_node;
          new_node->prev = current_node;
          current_node = new_node;
        }
      }
      
      if (g_sql_parse_error.has_error)
      {
        ProfEnd();
        return NULL;
      }
    }
    else
    {
      sql_parse_error_at(token->range, "unexpected token '%.*s'", str8_varg(token->value));
      ProfEnd();
      return NULL;
    }
  }
  
  ProfEnd();
  return root;
}

internal SQL_Node*
sql_parse_use_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  (*token_index)++; // move past 'use'
  
  SQL_Node* use_node = push_array(arena, SQL_Node, 1);
  use_node->type = SQL_NodeType_Use;
  
  SQL_Node* tail = NULL;
  
  while (*token_index < token_count)
  {
    SQL_Token *token = &(*tokens)[*token_index];
    
    if (token->type == SQL_TokenType_Identifier)
    {
      SQL_Node *column_node = push_array(arena, SQL_Node, 1);
      column_node->type = SQL_NodeType_Database;
      column_node->value = token->value;
      column_node->parent = use_node;
      
      if (!use_node->first)
      {
        use_node->first = use_node->last = column_node;
      }
      else
      {
        tail->next = column_node;
        column_node->prev = tail;
        use_node->last = column_node;
      }
      
      tail = column_node;
      (*token_index)++;
      
      if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
          str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
      {
        (*token_index)++;
      }
      else
      {
        break;
      }
    }
    else
    {
      sql_parse_error_at(token->range, "expected database name in 'use' clause, found '%.*s'",
                         str8_varg(token->value));
      return NULL;
    }
  }
  
  return use_node;
}

internal SQL_Node*
sql_parse_describe_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  (*token_index)++; // move past 'describe'

  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected table name after 'describe', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }

  SQL_Node* describe_node = push_array(arena, SQL_Node, 1);
  describe_node->type = SQL_NodeType_Describe;

  SQL_Node* table_node = push_array(arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = (*tokens)[*token_index].value;
  table_node->parent = describe_node;
  describe_node->first = describe_node->last = table_node;

  (*token_index)++;

  return describe_node;
}

//~ tec: shared helpers used by SELECT / FROM / WHERE / GROUP BY / ORDER BY

internal SQL_Node*
sql_parse_column_ref(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected column name, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Token *first = &(*tokens)[*token_index];
  String8 name = first->value;
  Rng1U64 range = first->range;
  (*token_index)++;
  
  // tec: optional table-qualified 'table.column' - the qualifier isn't resolved here, it's just carried through as one string.
  if (*token_index + 1 < token_count &&
      (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
      str8_match((*tokens)[*token_index].value, str8_lit("."), 0) &&
      (*tokens)[*token_index + 1].type == SQL_TokenType_Identifier)
  {
    SQL_Token *second = &(*tokens)[*token_index + 1];
    name = push_str8f(arena, "%.*s.%.*s", str8_varg(name), str8_varg(second->value));
    range = r1u64(range.min, second->range.max);
    *token_index += 2;
  }
  
  SQL_Node *node = push_array(arena, SQL_Node, 1);
  node->type = SQL_NodeType_Column;
  node->value = name;
  return node;
}

internal SQL_Node*
sql_parse_table_ref(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected table name, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node *table_node = push_array(arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = (*tokens)[*token_index].value;
  (*token_index)++;
  
  // tec: optional 'AS alias'
  if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
      str8_match((*tokens)[*token_index].value, str8_lit("as"), StringMatchFlag_CaseInsensitive))
  {
    (*token_index)++; // move past 'as'
    
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected alias name after 'as', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    
    SQL_Node *alias_node = push_array(arena, SQL_Node, 1);
    alias_node->type = SQL_NodeType_Alias;
    alias_node->value = (*tokens)[*token_index].value;
    alias_node->parent = table_node;
    table_node->first = table_node->last = alias_node;
    (*token_index)++;
  }
  
  return table_node;
}

internal SQL_Node*
sql_parse_select_item(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected column name in 'select' clause, but reached end of input");
    return NULL;
  }
  
  SQL_Token *token = &(*tokens)[*token_index];
  
  // tec: bare '*' - can't be aliased or combined with other select items
  if (token->type == SQL_TokenType_Symbol && str8_match(token->value, str8_lit("*"), 0))
  {
    SQL_Node *star_node = push_array(arena, SQL_Node, 1);
    star_node->type = SQL_NodeType_Column;
    star_node->value = token->value;
    (*token_index)++;
    return star_node;
  }
  
  if (token->type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(token->range, "expected column name or aggregate function in 'select' clause, found '%.*s'",
                       str8_varg(token->value));
    return NULL;
  }
  
  SQL_Node *item = NULL;
  
  // tec: identifier immediately followed by '(' -> aggregate/function call, e.g. COUNT(*), SUM(amount)
  if (*token_index + 1 < token_count &&
      (*tokens)[*token_index + 1].type == SQL_TokenType_Symbol &&
      str8_match((*tokens)[*token_index + 1].value, str8_lit("("), 0))
  {
    String8 func_name = token->value;
    (*token_index) += 2; // move past function name and '('
    
    SQL_Node *operand = NULL;
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
        str8_match((*tokens)[*token_index].value, str8_lit("*"), 0))
    {
      operand = push_array(arena, SQL_Node, 1);
      operand->type = SQL_NodeType_Column;
      operand->value = str8_lit("*");
      (*token_index)++;
    }
    else
    {
      operand = sql_parse_column_ref(arena, tokens, token_index, token_count);
      if (!operand) return NULL;
    }
    
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected ')' after aggregate function argument, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // move past ')'
    
    item = push_array(arena, SQL_Node, 1);
    item->type = SQL_NodeType_AggregateCall;
    item->value = func_name;
    item->first = item->last = operand;
    operand->parent = item;
  }
  else
  {
    item = sql_parse_column_ref(arena, tokens, token_index, token_count);
    if (!item) return NULL;
  }
  
  // tec: optional alias
  if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
      str8_match((*tokens)[*token_index].value, str8_lit("as"), StringMatchFlag_CaseInsensitive))
  {
    (*token_index)++; // move past 'as'
    
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected alias name after 'as', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    
    SQL_Node *alias_node = push_array(arena, SQL_Node, 1);
    alias_node->type = SQL_NodeType_Alias;
    alias_node->value = (*tokens)[*token_index].value;
    alias_node->parent = item;
    
    if (item->last)
    {
      item->last->next = alias_node;
      alias_node->prev = item->last;
      item->last = alias_node;
    }
    else
    {
      item->first = item->last = alias_node;
    }
    (*token_index)++;
  }
  
  return item;
}

internal SQL_Node*
sql_parse_select_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  (*token_index)++; // Move past 'select'
  
  SQL_Node* select_node = push_array(arena, SQL_Node, 1);
  select_node->type = SQL_NodeType_Select;
  
  SQL_Node* column_list = push_array(arena, SQL_Node, 1);
  column_list->type = SQL_NodeType_ColumnList;
  column_list->parent = select_node;
  select_node->first = select_node->last = column_list;
  
  for (;;)
  {
    SQL_Node *item = sql_parse_select_item(arena, tokens, token_index, token_count);
    if (!item) return NULL;
    
    item->parent = column_list;
    DLLPushBack(column_list->first, column_list->last, item);
    
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
        str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
    {
      (*token_index)++;
      continue;
    }
    break;
  }
  
  return select_node;
}

internal SQL_Node*
sql_parse_join_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  String8 join_type = str8_lit("inner");
  
  if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
      str8_match((*tokens)[*token_index].value, str8_lit("inner"), StringMatchFlag_CaseInsensitive))
  {
    (*token_index)++; // move past 'inner'
  }
  else if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
           str8_match((*tokens)[*token_index].value, str8_lit("left"), StringMatchFlag_CaseInsensitive))
  {
    join_type = str8_lit("left");
    (*token_index)++; // move past 'left'
    
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
        str8_match((*tokens)[*token_index].value, str8_lit("outer"), StringMatchFlag_CaseInsensitive))
    {
      (*token_index)++; // tec: 'outer' is a no-op synonym alongside 'left'
    }
  }
  
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("join"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'join' keyword, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // move past 'join'
  
  SQL_Node *table_node = sql_parse_table_ref(arena, tokens, token_index, token_count);
  if (!table_node) return NULL;
  
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("on"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'on' after joined table, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // move past 'on'
  
  SQL_Node *condition = sql_parse_logical_expression(arena, tokens, token_index, token_count);
  if (!condition) return NULL;
  
  SQL_Node *join_node = push_array(arena, SQL_Node, 1);
  join_node->type = SQL_NodeType_Join;
  join_node->value = join_type;
  join_node->first = table_node;
  join_node->last = condition;
  
  table_node->parent = join_node;
  table_node->next = condition;
  condition->prev = table_node;
  condition->parent = join_node;
  
  return join_node;
}

internal SQL_Node*
sql_parse_from_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count, SQL_Node *select_node)
{
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("from"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'from' keyword after 'select' clause");
    return NULL;
  }
  (*token_index)++; // move past 'from'
  
  SQL_Node *primary_table = sql_parse_table_ref(arena, tokens, token_index, token_count);
  if (!primary_table) return NULL;
  
  primary_table->parent = select_node;
  DLLPushBack(select_node->first, select_node->last, primary_table);
  
  // tec: additional comma-separated tables (implicit cross join) and/or JOIN clauses
  for (;;)
  {
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
        str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
    {
      (*token_index)++; // move past ','
      
      SQL_Node *extra_table = sql_parse_table_ref(arena, tokens, token_index, token_count);
      if (!extra_table) return NULL;
      
      extra_table->parent = select_node;
      DLLPushBack(select_node->first, select_node->last, extra_table);
      continue;
    }
    
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
        (str8_match((*tokens)[*token_index].value, str8_lit("join"), StringMatchFlag_CaseInsensitive) ||
         str8_match((*tokens)[*token_index].value, str8_lit("inner"), StringMatchFlag_CaseInsensitive) ||
         str8_match((*tokens)[*token_index].value, str8_lit("left"), StringMatchFlag_CaseInsensitive)))
    {
      SQL_Node *join_node = sql_parse_join_clause(arena, tokens, token_index, token_count);
      if (!join_node) return NULL;
      
      join_node->parent = select_node;
      DLLPushBack(select_node->first, select_node->last, join_node);
      continue;
    }
    
    break;
  }
  
  return primary_table;
}

internal SQL_Node*
sql_parse_comparison_expression(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  // tec: expressions inside parentheses
  if (*token_index < token_count &&
      (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
      str8_match((*tokens)[*token_index].value, str8_lit("("), 0))
  {
    (*token_index)++; // move past '('
    
    SQL_Node *expr = sql_parse_logical_expression(arena, tokens, token_index, token_count);
    if (!expr)
    {
      return NULL;
    }
    
    // tec: expect closing ')'
    if (*token_index >= token_count ||
        (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected ')' after expression, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    
    (*token_index)++; // move past ')'
    return expr;
  }
  
  // tec: left hand side column/literal
  SQL_Node *left = sql_parse_expression(arena, tokens, token_index, token_count);
  if (!left)
  {
    return NULL;
  }

  // tec: 'IS NULL' / 'IS NOT NULL'
  if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
      str8_match((*tokens)[*token_index].value, str8_lit("is"), StringMatchFlag_CaseInsensitive))
  {
    (*token_index)++; // tec: move past 'is'

    B32 negate = 0;
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
        str8_match((*tokens)[*token_index].value, str8_lit("not"), StringMatchFlag_CaseInsensitive))
    {
      negate = 1;
      (*token_index)++; // tec: move past 'not'
    }

    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
        !str8_match((*tokens)[*token_index].value, str8_lit("null"), StringMatchFlag_CaseInsensitive))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected 'null' after 'is%s', found '%.*s'", negate ? " not" : "",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past 'null'

    SQL_Node *is_null_node = push_array(arena, SQL_Node, 1);
    is_null_node->type = SQL_NodeType_Operator;
    is_null_node->value = negate ? str8_lit("is not null") : str8_lit("is null");
    is_null_node->first = left;
    is_null_node->last = left;
    left->parent = is_null_node;

    return is_null_node;
  }

  // Expect comparison operator
  if (*token_index >= token_count || !((*tokens)[*token_index].type == SQL_TokenType_Operator ||
                                       (*tokens)[*token_index].type == SQL_TokenType_Keyword))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected comparison operator, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  // Create comparison operator node
  SQL_Node *operator_node = push_array(arena, SQL_Node, 1);
  operator_node->type = SQL_NodeType_Operator;
  operator_node->value = (*tokens)[*token_index].value;
  (*token_index)++; // Move past operator
  
  // tec: right hand side column/literal
  SQL_Node *right = sql_parse_expression(arena, tokens, token_index, token_count);
  if (!right)
  {
    return NULL;
  }
  
  // tec: link
  operator_node->first = left;
  operator_node->last = right;
  left->next = right;
  right->prev = left;
  left->parent = operator_node;
  right->parent = operator_node;
  
  return operator_node;
}

internal SQL_Node*
sql_parse_and_expression(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  SQL_Node *left = sql_parse_comparison_expression(arena, tokens, token_index, token_count);
  if (!left)
  {
    return NULL;
  }
  
  while (*token_index < token_count &&
         (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
         str8_match((*tokens)[*token_index].value, str8_lit("and"), StringMatchFlag_CaseInsensitive))
  {
    SQL_Node *operator_node = push_array(arena, SQL_Node, 1);
    operator_node->type = SQL_NodeType_Operator;
    operator_node->value = (*tokens)[*token_index].value;
    (*token_index)++; // tec: move past 'and'
    
    SQL_Node *right = sql_parse_comparison_expression(arena, tokens, token_index, token_count);
    if (!right)
    {
      return NULL;
    }
    
    operator_node->first = left;
    operator_node->last = right;
    left->next = right;
    right->prev = left;
    left->parent = operator_node;
    right->parent = operator_node;
    
    left = operator_node;
  }
  
  return left;
}

internal SQL_Node*
sql_parse_logical_expression(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  // tec: 'and' binds tighter than 'or', so 'or' is the outer loop here rather than one flat equal-precedence chain
  SQL_Node *left = sql_parse_and_expression(arena, tokens, token_index, token_count);
  if (!left)
  {
    return NULL;
  }
  
  while (*token_index < token_count &&
         (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
         str8_match((*tokens)[*token_index].value, str8_lit("or"), StringMatchFlag_CaseInsensitive))
  {
    // tec: create logical operator node
    SQL_Node *operator_node = push_array(arena, SQL_Node, 1);
    operator_node->type = SQL_NodeType_Operator;
    operator_node->value = (*tokens)[*token_index].value;
    (*token_index)++; // tec: move past 'or'
    
    SQL_Node *right = sql_parse_and_expression(arena, tokens, token_index, token_count);
    if (!right)
    {
      return NULL;
    }
    
    // tec: link
    operator_node->first = left;
    operator_node->last = right;
    left->next = right;
    right->prev = left;
    left->parent = operator_node;
    right->parent = operator_node;
    
    // tec: update left to be the new root
    left = operator_node;
  }
  
  return left;
}

internal SQL_Node*
sql_parse_where_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("where"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count), "expected 'where' keyword");
    return NULL;
  }
  
  SQL_Node* where_node = push_array(arena, SQL_Node, 1);
  where_node->type = SQL_NodeType_Where;
  
  (*token_index)++; // tec: move past 'where'
  
  SQL_Node* logic_node = sql_parse_logical_expression(arena, tokens, token_index, token_count);
  if (!logic_node)
  {
    return NULL;
  }
  logic_node->parent = where_node;
  where_node->first = logic_node;
  where_node->last = logic_node;
  
  return where_node;
}

internal SQL_Node*
sql_parse_having_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("having"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count), "expected 'having' keyword");
    return NULL;
  }
  
  SQL_Node* having_node = push_array(arena, SQL_Node, 1);
  having_node->type = SQL_NodeType_Having;
  
  (*token_index)++; // tec: move past 'having'
  
  SQL_Node* logic_node = sql_parse_logical_expression(arena, tokens, token_index, token_count);
  if (!logic_node)
  {
    return NULL;
  }
  logic_node->parent = having_node;
  having_node->first = logic_node;
  having_node->last = logic_node;
  
  return having_node;
}

internal SQL_Node*
sql_parse_group_by_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("group"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count), "expected 'group' keyword");
    return NULL;
  }
  (*token_index)++; // move past 'group'
  
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("by"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'by' after 'group', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // move past 'by'
  
  SQL_Node* group_by_root = push_array(arena, SQL_Node, 1);
  group_by_root->type = SQL_NodeType_GroupBy;
  
  for (;;)
  {
    SQL_Node *column_node = sql_parse_column_ref(arena, tokens, token_index, token_count);
    if (!column_node) return NULL;
    
    column_node->parent = group_by_root;
    DLLPushBack(group_by_root->first, group_by_root->last, column_node);
    
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
        str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
    {
      (*token_index)++;
      continue;
    }
    break;
  }
  
  return group_by_root;
}

internal SQL_Node*
sql_parse_limit_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  (*token_index)++; // move past 'limit'
  
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Number)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected a number after 'limit', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* limit_node = push_array(arena, SQL_Node, 1);
  limit_node->type = SQL_NodeType_Limit;
  limit_node->value = (*tokens)[*token_index].value;
  (*token_index)++;
  
  return limit_node;
}

internal SQL_Node*
sql_parse_offset_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  (*token_index)++; // move past 'offset'
  
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Number)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected a number after 'offset', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* offset_node = push_array(arena, SQL_Node, 1);
  offset_node->type = SQL_NodeType_Offset;
  offset_node->value = (*tokens)[*token_index].value;
  (*token_index)++;
  
  return offset_node;
}

internal SQL_Node*
sql_parse_insert_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  SQL_Node* insert_node = push_array(arena, SQL_Node, 1);
  insert_node->type = SQL_NodeType_Insert;
  
  (*token_index)++; // tec: move past 'insert'
  
  // tec: expect 'into'
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("into"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'into' keyword in 'insert' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past "into"
  
  // tec: expect table name
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected table name in 'insert' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* table_node = push_array(arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = (*tokens)[*token_index].value;
  (*token_index)++;
  
  insert_node->first = table_node;
  table_node->parent = insert_node;
  
  // tec: optional column list
  SQL_Node* column_list_node = NULL;
  if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
      str8_match((*tokens)[*token_index].value, str8_lit("("), 0))
  {
    (*token_index)++; // tec: move past '('
    
    column_list_node = push_array(arena, SQL_Node, 1);
    column_list_node->type = SQL_NodeType_ColumnList;
    column_list_node->parent = insert_node;
    
    while (*token_index < token_count &&
           (*tokens)[*token_index].type != SQL_TokenType_Symbol &&
           !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      if ((*tokens)[*token_index].type != SQL_TokenType_Identifier)
      {
        sql_parse_error_at((*tokens)[*token_index].range,
                           "expected column name in 'insert' statement, found '%.*s'",
                           str8_varg((*tokens)[*token_index].value));
        return NULL;
      }
      
      SQL_Node* column_node = push_array(arena, SQL_Node, 1);
      column_node->type = SQL_NodeType_Column;
      column_node->value = (*tokens)[*token_index].value;
      column_node->parent = column_list_node;
      (*token_index)++;
      
      if (!column_list_node->first)
      {
        column_list_node->first = column_node;
      }
      else
      {
        column_list_node->last->next = column_node;
        column_node->prev = column_list_node->last;
      }
      column_list_node->last = column_node;
      
      // tec: skip comma
      if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
          str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
      {
        (*token_index)++;
      }
    }
    
    // tec: expect closing ')'
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected closing ')' in column list, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past ')'
    
    // tec: attach column list node to insert_node
    table_node->next = column_list_node;
    insert_node->last = column_list_node;
  }
  
  // tec: expect 'values' keyword
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("values"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'values' keyword in 'insert' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past 'values'
  
  // tec: parse 'values' clause
  SQL_Node* values_node = sql_parse_values_clause(arena, tokens, token_index, token_count);
  if (!values_node)
  {
    return NULL;
  }
  
  // tec: link values to the insert node
  SQL_Node* last_child = insert_node->last ? insert_node->last : table_node;
  last_child->next = values_node;
  values_node->prev = last_child;
  values_node->parent = insert_node;
  insert_node->last = values_node;
  
  return insert_node;
}

internal SQL_Node*
sql_parse_import_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  SQL_Node* import_node = push_array(arena, SQL_Node, 1);
  import_node->type = SQL_NodeType_Import;
  
  (*token_index)++; // tec: move past 'IMPORT'
  
  // tec: expect 'INTO'
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("into"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'into' keyword in 'import' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past 'INTO'
  
  // tec: expect table name
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected table name after 'into' in 'import' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* table_node = push_array(arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = (*tokens)[*token_index].value;
  (*token_index)++;
  
  import_node->first = table_node;
  table_node->parent = import_node;
  
  // tec: expect 'FROM'
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("from"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'from' keyword in 'import' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past 'FROM'
  
  // tec: expect file path as string literal
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_String)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected file path after 'from' in 'import' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* path_node = push_array(arena, SQL_Node, 1);
  path_node->type = SQL_NodeType_Literal;
  path_node->value = (*tokens)[*token_index].value;
  (*token_index)++;
  
  table_node->next = path_node;
  path_node->prev = table_node;
  path_node->parent = import_node;
  import_node->last = path_node;
  
  return import_node;
}

internal SQL_Node*
sql_parse_create_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  SQL_Node* create_node = push_array(arena, SQL_Node, 1);
  create_node->type = SQL_NodeType_Create;
  
  (*token_index)++; // tec: move past 'create'
  
  // tec: expect 'table' or 'database'
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'table' or 'database' keyword in 'create' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  String8 keyword = (*tokens)[*token_index].value;
  (*token_index)++; // tec: move past keyword
  
  if (str8_match(keyword, str8_lit("database"), StringMatchFlag_CaseInsensitive))
  {
    // tec: expect database name
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected database name in 'create' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    
    SQL_Node* database_node = push_array(arena, SQL_Node, 1);
    database_node->type = SQL_NodeType_Database;
    database_node->value = (*tokens)[*token_index].value;
    (*token_index)++;
    
    create_node->first = database_node;
    create_node->last = database_node;
    database_node->parent = create_node;
    
  }
  else if (str8_match(keyword, str8_lit("table"), StringMatchFlag_CaseInsensitive))
  {
    // tec: expect table name
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected table name in 'create' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    
    SQL_Node* table_node = push_array(arena, SQL_Node, 1);
    table_node->type = SQL_NodeType_Table;
    table_node->value = (*tokens)[*token_index].value;
    (*token_index)++;
    
    create_node->first = table_node;
    create_node->last = table_node;
    table_node->parent = create_node;
    
    // tec: expect column definitions
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit("("), 0))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected '(' in 'create table' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past '('
    
    SQL_Node* prev_column = NULL;
    while (*token_index < token_count && (*tokens)[*token_index].type != SQL_TokenType_Symbol &&
           !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      // tec: expect column name
      if ((*tokens)[*token_index].type != SQL_TokenType_Identifier)
      {
        sql_parse_error_at((*tokens)[*token_index].range,
                           "expected column name in 'create table' statement, found '%.*s'",
                           str8_varg((*tokens)[*token_index].value));
        return NULL;
      }
      
      SQL_Node* column_node = push_array(arena, SQL_Node, 1);
      column_node->type = SQL_NodeType_Column;
      column_node->value = (*tokens)[*token_index].value;
      (*token_index)++;
      
      // tec: expect column type
      if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword)
      {
        sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                           "expected column type in 'create table' statement, found '%.*s'",
                           str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
        return NULL;
      }
      
      SQL_Node* type_node = push_array(arena, SQL_Node, 1);
      type_node->type = SQL_NodeType_Type;
      type_node->value = (*tokens)[*token_index].value;
      (*token_index)++;
      
      column_node->first = type_node;
      column_node->last = type_node;
      type_node->parent = column_node;

      // tec: zero or more column constraints, in any order, until a ',' or ')'
      for (;;)
      {
        if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword) break;
        String8 constraint_kw = (*tokens)[*token_index].value;

        if (str8_match(constraint_kw, str8_lit("not"), StringMatchFlag_CaseInsensitive))
        {
          (*token_index)++; // tec: move past 'not'
          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
              !str8_match((*tokens)[*token_index].value, str8_lit("null"), StringMatchFlag_CaseInsensitive))
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected 'null' after 'not' in column constraint, found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          (*token_index)++; // tec: move past 'null'

          SQL_Node* c = push_array(arena, SQL_Node, 1);
          c->type = SQL_NodeType_NotNull;
          c->parent = column_node;
          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("unique"), StringMatchFlag_CaseInsensitive))
        {
          (*token_index)++; // tec: move past 'unique'

          SQL_Node* c = push_array(arena, SQL_Node, 1);
          c->type = SQL_NodeType_Unique;
          c->parent = column_node;
          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("primary"), StringMatchFlag_CaseInsensitive))
        {
          (*token_index)++; // tec: move past 'primary'
          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
              !str8_match((*tokens)[*token_index].value, str8_lit("key"), StringMatchFlag_CaseInsensitive))
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected 'key' after 'primary' in column constraint, found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          (*token_index)++; // tec: move past 'key'

          SQL_Node* c = push_array(arena, SQL_Node, 1);
          c->type = SQL_NodeType_PrimaryKey;
          c->parent = column_node;
          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("references"), StringMatchFlag_CaseInsensitive))
        {
          (*token_index)++; // tec: move past 'references'
          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected referenced table name after 'references', found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          String8 ref_table = (*tokens)[*token_index].value;
          (*token_index)++;

          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
              !str8_match((*tokens)[*token_index].value, str8_lit("("), 0))
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected '(' after referenced table name, found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          (*token_index)++; // tec: move past '('

          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected referenced column name, found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          String8 ref_column = (*tokens)[*token_index].value;
          (*token_index)++;

          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
              !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected ')' after referenced column name, found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          (*token_index)++; // tec: move past ')'

          SQL_Node* c = push_array(arena, SQL_Node, 1);
          c->type = SQL_NodeType_ForeignKey;
          c->value = ref_table;
          c->parent = column_node;

          SQL_Node* ref_col_node = push_array(arena, SQL_Node, 1);
          ref_col_node->type = SQL_NodeType_Column;
          ref_col_node->value = ref_column;
          ref_col_node->parent = c;
          c->first = c->last = ref_col_node;

          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("check"), StringMatchFlag_CaseInsensitive))
        {
          (*token_index)++; // tec: move past 'check'
          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
              !str8_match((*tokens)[*token_index].value, str8_lit("("), 0))
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected '(' after 'check', found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          (*token_index)++; // tec: move past '('

          // tec: capture the raw source text (not just the parsed tree) so it can be saved and reparsed on table load
          U64 expr_start_token = *token_index;
          Rng1U64 expr_start_range = sql_token_range_at(*tokens, expr_start_token, token_count);

          SQL_Node* expr = sql_parse_logical_expression(arena, tokens, token_index, token_count);
          if (!expr) return NULL;

          Rng1U64 expr_end_range = sql_token_range_at(*tokens, *token_index > 0 ? *token_index - 1 : 0, token_count);
          String8 expr_text = str8_substr(g_sql_source_text, r1u64(expr_start_range.min, expr_end_range.max));

          if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
              !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
          {
            sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                               "expected ')' after 'check' expression, found '%.*s'",
                               str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
            return NULL;
          }
          (*token_index)++; // tec: move past ')'

          SQL_Node* c = push_array(arena, SQL_Node, 1);
          c->type = SQL_NodeType_Check;
          c->value = expr_text;
          c->parent = column_node;
          c->first = c->last = expr;
          expr->parent = c;

          DLLPushBack(column_node->first, column_node->last, c);
        }
        else
        {
          break;
        }
      }

      column_node->parent = table_node;
      
      if (prev_column)
      {
        prev_column->next = column_node;
        column_node->prev = prev_column;
      }
      else
      {
        table_node->first = column_node;
      }
      table_node->last = column_node;
      
      prev_column = column_node;
      
      // tec: skip comma
      if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
          str8_match((*tokens)[*token_index].value, str8_lit(","), StringMatchFlag_CaseInsensitive))
      {
        (*token_index)++;
      }
    }
    
    // tec: expect closing ')'
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit(")"), StringMatchFlag_CaseInsensitive))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected closing ')' in 'create table' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past ')'
    
  }
  else if (str8_match(keyword, str8_lit("index"), StringMatchFlag_CaseInsensitive))
  {
    // tec: CREATE INDEX idx_name ON table_name (column_name);
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected index name in 'create index' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }

    SQL_Node* index_node = push_array(arena, SQL_Node, 1);
    index_node->type = SQL_NodeType_Index;
    index_node->value = (*tokens)[*token_index].value;
    (*token_index)++;

    create_node->first = index_node;
    create_node->last = index_node;
    index_node->parent = create_node;

    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
        !str8_match((*tokens)[*token_index].value, str8_lit("on"), StringMatchFlag_CaseInsensitive))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected 'on' in 'create index' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past 'on'

    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected table name in 'create index' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }

    SQL_Node* table_node = push_array(arena, SQL_Node, 1);
    table_node->type = SQL_NodeType_Table;
    table_node->value = (*tokens)[*token_index].value;
    table_node->parent = index_node;
    index_node->first = table_node;
    index_node->last = table_node;
    (*token_index)++;

    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit("("), 0))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected '(' in 'create index' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past '('

    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected column name in 'create index' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }

    SQL_Node* column_node = push_array(arena, SQL_Node, 1);
    column_node->type = SQL_NodeType_Column;
    column_node->value = (*tokens)[*token_index].value;
    column_node->parent = index_node;
    table_node->next = column_node;
    column_node->prev = table_node;
    index_node->last = column_node;
    (*token_index)++;

    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected closing ')' in 'create index' statement, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past ')'
  }
  else
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index - 1, token_count),
                       "unexpected keyword '%.*s' in 'create' statement, expected 'table', 'database', or 'index'",
                       str8_varg(keyword));
    return NULL;
  }

  return create_node;
}

internal SQL_Node*
sql_parse_drop_index_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  // tec: DROP INDEX idx_name ON table_name;
  (*token_index)++; // tec: move past 'drop'

  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("index"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'index' after 'drop', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past 'index'

  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected index name in 'drop index' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }

  SQL_Node* drop_index_node = push_array(arena, SQL_Node, 1);
  drop_index_node->type = SQL_NodeType_DropIndex;
  drop_index_node->value = (*tokens)[*token_index].value;
  (*token_index)++;

  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("on"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'on' in 'drop index' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past 'on'

  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected table name in 'drop index' statement, found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }

  SQL_Node* table_node = push_array(arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = (*tokens)[*token_index].value;
  table_node->parent = drop_index_node;
  drop_index_node->first = table_node;
  drop_index_node->last = table_node;
  (*token_index)++;

  return drop_index_node;
}

internal SQL_Node*
sql_parse_alter_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("alter"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count), "expected 'alter' keyword");
    return NULL;
  }
  (*token_index)++; // tec: move past 'alter'
  
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("table"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'table' keyword after 'alter', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past 'table'
  
  // tec: expect table name
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected table name after 'alter table', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* alter_node = push_array(arena, SQL_Node, 1);
  alter_node->type = SQL_NodeType_Alter;
  
  SQL_Node* table_node = push_array(arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = (*tokens)[*token_index].value;
  (*token_index)++; // tec: move past table name
  
  table_node->parent = alter_node;
  alter_node->first = table_node;
  
  // tec: expect alter operation
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected operation after 'alter table <table>', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* operation_node = push_array(arena, SQL_Node, 1);
  operation_node->parent = alter_node;
  alter_node->first->next = operation_node;
  
  // tec: alter operations
  if (str8_match((*tokens)[*token_index].value, str8_lit("add"), StringMatchFlag_CaseInsensitive))
  {
    (*token_index)++; // tec: move past 'add'
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
        !str8_match((*tokens)[*token_index].value, str8_lit("column"), StringMatchFlag_CaseInsensitive))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected 'column' after 'add' in 'alter table', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past 'column'
    
    // tec: expect column name
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected column name after 'add column', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    operation_node->type = SQL_NodeType_Alter_AddColumn;
    
    SQL_Node* column_node = push_array(arena, SQL_Node, 1);
    column_node->type = SQL_NodeType_Column;
    column_node->parent = alter_node;
    column_node->value = (*tokens)[*token_index].value;
    (*token_index)++; // tec: move past column name
    
    operation_node->first = column_node;
    
    // tec: optional column type
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword)
    {
      SQL_Node* type_node = push_array(arena, SQL_Node, 1);
      type_node->type = SQL_NodeType_Type;
      type_node->value = (*tokens)[*token_index].value;
      type_node->parent = operation_node;
      operation_node->first->next = type_node;
      (*token_index)++; // tec: move past column type
    }
  }
  else if (str8_match((*tokens)[*token_index].value, str8_lit("drop"), StringMatchFlag_CaseInsensitive))
  {
    (*token_index)++; // Move past 'DROP'
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
        !str8_match((*tokens)[*token_index].value, str8_lit("column"), StringMatchFlag_CaseInsensitive))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected 'column' after 'drop' in 'alter table', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past 'column'
    
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected column name after 'drop column', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    operation_node->type = SQL_NodeType_Alter_DropColumn;
    operation_node->value = (*tokens)[*token_index].value;
    (*token_index)++; // tec: move past column name
  }
  else if (str8_match((*tokens)[*token_index].value, str8_lit("rename"), StringMatchFlag_CaseInsensitive))
  {
    (*token_index)++; // tec: move past 'rename'
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
        !str8_match((*tokens)[*token_index].value, str8_lit("to"), StringMatchFlag_CaseInsensitive))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected 'to' after 'rename' in 'alter table', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past 'to'
    
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected new table name after 'rename to', found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    operation_node->type = SQL_NodeType_Alter_Rename;
    operation_node->value = (*tokens)[*token_index].value;
    (*token_index)++; // tec: move past new table name
  }
  else
  {
    sql_parse_error_at((*tokens)[*token_index].range,
                       "unknown 'alter table' operation '%.*s'",
                       str8_varg((*tokens)[*token_index].value));
    return NULL;
  }
  
  return alter_node;
}

internal SQL_Node*
sql_parse_values_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  SQL_Node* values_root = push_array(arena, SQL_Node, 1);
  values_root->type = SQL_NodeType_Value;
  
  SQL_Node* prev_value_group = NULL;
  
  while (*token_index < token_count)
  {
    // tec: expect opening '('
    if ((*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit("("), 0))
    {
      sql_parse_error_at((*tokens)[*token_index].range,
                         "expected '(' before values in 'values' clause, found '%.*s'",
                         str8_varg((*tokens)[*token_index].value));
      return NULL;
    }
    (*token_index)++; // tec: move past '('
    
    SQL_Node* value_group = push_array(arena, SQL_Node, 1);
    value_group->type = SQL_NodeType_ValueGroup;
    
    SQL_Node* prev_value = NULL;
    
    while (*token_index < token_count &&
           (*tokens)[*token_index].type != SQL_TokenType_Symbol &&
           !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      B32 is_null_token = (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
                          str8_match((*tokens)[*token_index].value, str8_lit("null"), StringMatchFlag_CaseInsensitive);

      if ((*tokens)[*token_index].type != SQL_TokenType_Number &&
          (*tokens)[*token_index].type != SQL_TokenType_String &&
          !is_null_token)
      {
        sql_parse_error_at((*tokens)[*token_index].range,
                           "expected a literal value in 'values' clause, found '%.*s'",
                           str8_varg((*tokens)[*token_index].value));
        return NULL;
      }

      SQL_Node* value_node = push_array(arena, SQL_Node, 1);
      value_node->type = is_null_token ? SQL_NodeType_Null :
        ((*tokens)[*token_index].type == SQL_TokenType_Number ? SQL_NodeType_Numeric : SQL_NodeType_Literal);
      value_node->value = (*tokens)[*token_index].value;
      (*token_index)++;
      
      if (!value_group->first)
      {
        value_group->first = value_node;
      }
      if (prev_value)
      {
        prev_value->next = value_node;
        value_node->prev = prev_value;
      }
      prev_value = value_node;
      
      if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol)
      {
        if (str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
        {
          (*token_index)++; // tec: skip ','
          continue;
        }
      }
    }
    value_group->last = prev_value;
    
    // tec: expect closing ')'
    if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
        !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
    {
      sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                         "expected closing ')' after values in 'values' clause, found '%.*s'",
                         str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
      return NULL;
    }
    (*token_index)++; // tec: move past ')'
    
    if (prev_value_group)
    {
      prev_value_group->next = value_group;
      value_group->prev = prev_value_group;
    }
    else
    {
      values_root->first = value_group;
    }
    
    prev_value_group = value_group;
    
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
        str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
    {
      (*token_index)++; // tec: move past ',' to next value group
    }
    else
    {
      break;
    }
  }
  
  values_root->last = prev_value_group;
  return values_root;
}

internal SQL_Node*
sql_parse_order_by_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("order"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count), "expected 'order' keyword");
    return NULL;
  }
  (*token_index)++;
  
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("by"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'by' after 'order', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++;
  
  SQL_Node* order_by_root = push_array(arena, SQL_Node, 1);
  order_by_root->type = SQL_NodeType_OrderBy;
  
  for (;;)
  {
    SQL_Node* column_node = sql_parse_column_ref(arena, tokens, token_index, token_count);
    if (!column_node) return NULL;
    
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Keyword)
    {
      SQL_Node* sort_node = push_array(arena, SQL_Node, 1);
      sort_node->value = (*tokens)[*token_index].value;
      
      if (str8_match((*tokens)[*token_index].value, str8_lit("asc"), StringMatchFlag_CaseInsensitive))
      {
        sort_node->type = SQL_NodeType_Ascending;
        column_node->first = sort_node;
        sort_node->parent = column_node;
        (*token_index)++;
      }
      else if (str8_match((*tokens)[*token_index].value, str8_lit("desc"), StringMatchFlag_CaseInsensitive))
      {
        sort_node->type = SQL_NodeType_Descending;
        column_node->first = sort_node;
        sort_node->parent = column_node;
        (*token_index)++;
      }
    }
    
    column_node->parent = order_by_root;
    DLLPushBack(order_by_root->first, order_by_root->last, column_node);
    
    if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
        str8_match((*tokens)[*token_index].value, str8_lit(","), 0))
    {
      (*token_index)++;
      continue;
    }
    else
    {
      break;
    }
  }
  
  return order_by_root;
}

internal SQL_Node*
sql_parse_delete_clause(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("delete"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count), "expected 'delete' keyword");
    return NULL;
  }
  (*token_index)++; // tec: move past 'DELETE'
  
  if (*token_index >= token_count ||
      (*tokens)[*token_index].type != SQL_TokenType_Keyword ||
      !str8_match((*tokens)[*token_index].value, str8_lit("from"), StringMatchFlag_CaseInsensitive))
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected 'from' keyword after 'delete', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  (*token_index)++; // tec: move past 'FROM'
  
  // tec: expect table name
  if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "expected table name after 'delete from', found '%.*s'",
                       str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
    return NULL;
  }
  
  SQL_Node* delete_node = push_array(arena, SQL_Node, 1);
  delete_node->type = SQL_NodeType_Delete;
  delete_node->value = (*tokens)[*token_index].value;
  
  SQL_Node* table_node = push_array(arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = (*tokens)[*token_index].value;
  table_node->parent = delete_node;
  
  delete_node->first = delete_node->last = table_node;
  
  (*token_index)++; // tec: move past table name
  
  // tec: check for optional WHERE clause
  if (*token_index < token_count &&
      (*tokens)[*token_index].type == SQL_TokenType_Keyword &&
      str8_match((*tokens)[*token_index].value, str8_lit("where"), StringMatchFlag_CaseInsensitive))
  {
    SQL_Node* where_clause = sql_parse_where_clause(arena, tokens, token_index, token_count);
    if (!where_clause)
    {
      return NULL;
    }
    
    table_node->next = where_clause;
    where_clause->prev = table_node;
    where_clause->parent = delete_node;
    delete_node->last = where_clause;
  }
  
  return delete_node;
}

internal SQL_Node*
sql_parse_expression(Arena* arena, SQL_Token **tokens, U64 *token_index, U64 token_count)
{
  if (*token_index >= token_count)
  {
    sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                       "unexpected end of input in expression");
    return NULL;
  }
  
  SQL_Token *token = &(*tokens)[*token_index];
  
  if (token->type == SQL_TokenType_Identifier)
  {
    // tec: identifier immediately followed by '(' -> aggregate/function call, e.g. HAVING COUNT(*) > 1
    if (*token_index + 1 < token_count &&
        (*tokens)[*token_index + 1].type == SQL_TokenType_Symbol &&
        str8_match((*tokens)[*token_index + 1].value, str8_lit("("), 0))
    {
      String8 func_name = token->value;
      (*token_index) += 2; // move past function name and '('
      
      SQL_Node *operand = NULL;
      if (*token_index < token_count && (*tokens)[*token_index].type == SQL_TokenType_Symbol &&
          str8_match((*tokens)[*token_index].value, str8_lit("*"), 0))
      {
        operand = push_array(arena, SQL_Node, 1);
        operand->type = SQL_NodeType_Column;
        operand->value = str8_lit("*");
        (*token_index)++;
      }
      else
      {
        operand = sql_parse_column_ref(arena, tokens, token_index, token_count);
        if (!operand) return NULL;
      }
      
      if (*token_index >= token_count || (*tokens)[*token_index].type != SQL_TokenType_Symbol ||
          !str8_match((*tokens)[*token_index].value, str8_lit(")"), 0))
      {
        sql_parse_error_at(sql_token_range_at(*tokens, *token_index, token_count),
                           "expected ')' after aggregate function argument, found '%.*s'",
                           str8_varg(sql_token_text_or_eof(*tokens, *token_index, token_count)));
        return NULL;
      }
      (*token_index)++; // move past ')'
      
      SQL_Node *call_node = push_array(arena, SQL_Node, 1);
      call_node->type = SQL_NodeType_AggregateCall;
      call_node->value = func_name;
      call_node->first = call_node->last = operand;
      operand->parent = call_node;
      return call_node;
    }
    
    return sql_parse_column_ref(arena, tokens, token_index, token_count);
  }
  
  if (token->type == SQL_TokenType_Number || token->type == SQL_TokenType_String)
  {
    SQL_Node *node = push_array(arena, SQL_Node, 1);
    node->type = (token->type == SQL_TokenType_Number) ? SQL_NodeType_Numeric : SQL_NodeType_Literal;
    node->value = token->value;
    (*token_index)++;
    return node;
  }
  
  sql_parse_error_at(token->range, "unexpected token '%.*s' in expression", str8_varg(token->value));
  return NULL;
}


//~ tec: debug printing

internal String8
sql_node_type_to_string(SQL_NodeType type)
{
  String8 result = str8_lit("no type");
  
  switch (type)
  {
    case SQL_NodeType_Use: result = str8_lit("SQL_NodeType_Use"); break;
    case SQL_NodeType_Describe: result = str8_lit("SQL_NodeType_Describe"); break;
    case SQL_NodeType_Select: result = str8_lit("SQL_NodeType_Select"); break;
    case SQL_NodeType_Column: result = str8_lit("SQL_NodeType_Column"); break;
    case SQL_NodeType_ColumnList: result = str8_lit("SQL_NodeType_ColumnList"); break;
    case SQL_NodeType_Table: result = str8_lit("SQL_NodeType_Table"); break;
    case SQL_NodeType_Database: result = str8_lit("SQL_NodeType_Database"); break;
    case SQL_NodeType_Where: result = str8_lit("SQL_NodeType_Where"); break;
    case SQL_NodeType_Operator: result = str8_lit("SQL_NodeType_Operator"); break;
    case SQL_NodeType_Numeric: result = str8_lit("SQL_NodeType_Numeric"); break;
    case SQL_NodeType_Identifier: result = str8_lit("SQL_NodeType_Identifier"); break;
    case SQL_NodeType_Literal: result = str8_lit("SQL_NodeType_Literal"); break;
    case SQL_NodeType_Insert: result = str8_lit("SQL_NodeType_Insert"); break;
    case SQL_NodeType_Import: result = str8_lit("SQL_NodeType_Import"); break;
    case SQL_NodeType_Delete: result = str8_lit("SQL_NodeType_Delete"); break;
    case SQL_NodeType_Create: result = str8_lit("SQL_NodeType_Create"); break;
    case SQL_NodeType_Drop: result = str8_lit("SQL_NodeType_Drop"); break;
    case SQL_NodeType_Index: result = str8_lit("SQL_NodeType_Index"); break;
    case SQL_NodeType_DropIndex: result = str8_lit("SQL_NodeType_DropIndex"); break;
    case SQL_NodeType_Null: result = str8_lit("SQL_NodeType_Null"); break;
    case SQL_NodeType_NotNull: result = str8_lit("SQL_NodeType_NotNull"); break;
    case SQL_NodeType_Unique: result = str8_lit("SQL_NodeType_Unique"); break;
    case SQL_NodeType_PrimaryKey: result = str8_lit("SQL_NodeType_PrimaryKey"); break;
    case SQL_NodeType_ForeignKey: result = str8_lit("SQL_NodeType_ForeignKey"); break;
    case SQL_NodeType_Check: result = str8_lit("SQL_NodeType_Check"); break;
    case SQL_NodeType_Alter: result = str8_lit("SQL_NodeType_Alter"); break;
    case SQL_NodeType_Row: result = str8_lit("SQL_NodeType_Row"); break;
    case SQL_NodeType_Value: result = str8_lit("SQL_NodeType_Value"); break;
    case SQL_NodeType_ValueGroup: result = str8_lit("SQL_NodeType_ValueGroup"); break;
    case SQL_NodeType_Type: result = str8_lit("SQL_NodeType_Type"); break;
    case SQL_NodeType_OrderBy: result = str8_lit("SQL_NodeType_OrderBy"); break;
    case SQL_NodeType_Ascending: result = str8_lit("SQL_NodeType_Ascending"); break;
    case SQL_NodeType_Descending: result = str8_lit("SQL_NodeType_Descending"); break;
    case SQL_NodeType_Alter_AddColumn: result = str8_lit("SQL_NodeType_Alter_AddColumn"); break;
    case SQL_NodeType_Alter_ColumnType: result = str8_lit("SQL_NodeType_Alter_ColumnType"); break;
    case SQL_NodeType_Alter_DropColumn: result = str8_lit("SQL_NodeType_Alter_DropColumn"); break;
    case SQL_NodeType_Alter_Rename: result = str8_lit("SQL_NodeType_Alter_Rename"); break;
    case SQL_NodeType_Join: result = str8_lit("SQL_NodeType_Join"); break;
    case SQL_NodeType_Alias: result = str8_lit("SQL_NodeType_Alias"); break;
    case SQL_NodeType_GroupBy: result = str8_lit("SQL_NodeType_GroupBy"); break;
    case SQL_NodeType_Having: result = str8_lit("SQL_NodeType_Having"); break;
    case SQL_NodeType_Limit: result = str8_lit("SQL_NodeType_Limit"); break;
    case SQL_NodeType_Offset: result = str8_lit("SQL_NodeType_Offset"); break;
    case SQL_NodeType_AggregateCall: result = str8_lit("SQL_NodeType_AggregateCall"); break;
  }
  
  return result;
}

internal void
sql_print_node(SQL_Node *node, U64 depth)
{
  if (!node) return;
  
  for (U64 i = 0; i < depth; i++) printf("  ");
  
  String8 type = sql_node_type_to_string(node->type);
  printf("SQL_Node: type=%.*s, value=%.*s\n", (int)type.size, type.str, (int)node->value.size, node->value.str);
  
  if (node->first)
  {
    for (U64 i = 0; i < depth; i++)
      printf("  ");
    printf("Children:\n");
    sql_print_node(node->first, depth + 1);
  }
  
  if (node->next)
  {
    sql_print_node(node->next, depth);
  }
}

internal void
sql_print_ast(SQL_Node *root)
{
  printf("SQL AST:\n");
  sql_print_node(root, 0);
}
