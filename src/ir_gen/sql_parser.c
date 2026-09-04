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

      // tec: '' is an escaped literal quote, scan for the real terminator first
      B32 has_escape = 0;
      B32 terminated = 0;
      U64 scan = pos;
      while (scan < text.size)
      {
        if (text.str[scan] == '\'')
        {
          if (scan + 1 < text.size && text.str[scan + 1] == '\'')
          {
            has_escape = 1;
            scan += 2;
            continue;
          }
          terminated = 1;
          break;
        }
        scan++;
      }

      if (!terminated)
      {
        log_error("Unterminated string literal.");
        break;
      }

      pos = scan;

      String8 token_value;
      if (!has_escape)
      {
        token_value = str8_substr(text, r1u64(start, pos));
      }
      else
      {
        U8* buf = push_array_no_zero(arena, U8, pos - start);
        U64 buf_len = 0;
        for (U64 i = start; i < pos; i++)
        {
          buf[buf_len++] = text.str[i];
          if (text.str[i] == '\'') { i++; } // tec: skip the paired escaping quote
        }
        token_value = str8(buf, buf_len);
      }

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

//~ tec: parse contex

internal SQL_ParseCtx
sql_parse_ctx_make(Arena *arena, SQL_Token *tokens, U64 count)
{
  SQL_ParseCtx ctx = {0};
  ctx.arena = arena;
  ctx.tokens = tokens;
  ctx.count = count;
  return ctx;
}

internal SQL_Token
sql_peek(SQL_ParseCtx *ctx, U64 lookahead)
{
  U64 idx = ctx->pos + lookahead;
  if (idx < ctx->count)
  {
    return ctx->tokens[idx];
  }
  SQL_Token eof = {0};
  eof.type = SQL_TokenType_EOF;
  eof.value = str8_lit("<end of input>");
  eof.range = (ctx->count > 0) ? ctx->tokens[ctx->count - 1].range : r1u64(0, 0);
  return eof;
}

internal SQL_Token
sql_take(SQL_ParseCtx *ctx)
{
  SQL_Token tok = sql_peek(ctx, 0);
  if (ctx->pos < ctx->count)
  {
    ctx->pos++;
  }
  return tok;
}

internal void
sql_advance(SQL_ParseCtx *ctx, U64 n)
{
  ctx->pos = Min(ctx->pos + n, ctx->count);
}

internal B32
sql_at_end(SQL_ParseCtx *ctx)
{
  return ctx->pos >= ctx->count;
}

internal B32
sql_check(SQL_ParseCtx *ctx, SQL_TokenType type, String8 value)
{
  SQL_Token tok = sql_peek(ctx, 0);
  if (tok.type != type) return 0;
  if (value.size == 0) return 1;
  return str8_match(tok.value, value, StringMatchFlag_CaseInsensitive);
}

internal B32
sql_match(SQL_ParseCtx *ctx, SQL_TokenType type, String8 value)
{
  if (!sql_check(ctx, type, value)) return 0;
  sql_advance(ctx, 1);
  return 1;
}

internal B32
sql_find(SQL_ParseCtx *ctx, SQL_TokenType type, String8 value, U64 *out_pos)
{
  S64 depth = 0;
  for (U64 i = ctx->pos; i < ctx->count; i++)
  {
    SQL_Token *tok = &ctx->tokens[i];
    if (tok->type == SQL_TokenType_Symbol && str8_match(tok->value, str8_lit("("), 0))
    {
      depth++;
    }
    else if (tok->type == SQL_TokenType_Symbol && str8_match(tok->value, str8_lit(")"), 0))
    {
      if (depth == 0) return 0; // tec: hit an unmatched close before finding the target - search scope ended
      depth--;
    }
    else if (depth == 0 && tok->type == type &&
             (value.size == 0 || str8_match(tok->value, value, StringMatchFlag_CaseInsensitive)))
    {
      if (out_pos) *out_pos = i;
      return 1;
    }
  }
  return 0;
}

internal Rng1U64
sql_ctx_range_at(SQL_ParseCtx *ctx, U64 pos)
{
  if (ctx->count == 0) return r1u64(0, 0);
  if (pos < ctx->count) return ctx->tokens[pos].range;
  return ctx->tokens[ctx->count - 1].range;
}

internal Rng1U64
sql_ctx_error_range(SQL_ParseCtx *ctx)
{
  return sql_ctx_range_at(ctx, ctx->pos);
}

internal String8
sql_ctx_text_or_eof(SQL_ParseCtx *ctx)
{
  return sql_peek(ctx, 0).value;
}

//~ tec: ast
internal SQL_Node*
sql_parse(Arena* arena, SQL_Token* tokens, U64 token_count, String8 source_text)
{
  ProfBeginFunction();

  g_sql_parse_error = (SQL_ParseError){0};
  g_sql_source_text = source_text;

  SQL_ParseCtx ctx_ = sql_parse_ctx_make(arena, tokens, token_count);
  SQL_ParseCtx *ctx = &ctx_;

  SQL_Node *root = NULL;
  SQL_Node *current_node = NULL;
  SQL_Node *last_select_node = NULL;

  while (!sql_at_end(ctx))
  {
    SQL_Token token = sql_peek(ctx, 0);
    B32 attach_to_select = 0;

    if (token.type == SQL_TokenType_Symbol && str8_match(token.value, str8_lit(";"), 0))
    {
      sql_advance(ctx, 1);
      last_select_node = NULL;
      continue;
    }

    if (token.type == SQL_TokenType_Keyword)
    {
      SQL_Node *new_node = NULL;

      if (str8_match(token.value, str8_lit("use"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_use_clause(ctx);
      }
      else if (str8_match(token.value, str8_lit("select"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_select_clause(ctx);
        last_select_node = new_node;
      }
      else if (str8_match(token.value, str8_lit("from"), StringMatchFlag_CaseInsensitive))
      {
        if (!last_select_node)
        {
          sql_parse_error_at(token.range, "'from' clause without a preceding 'select'");
        }
        else
        {
          // tec: attaches tables/joins directly onto last_select_node - a FROM clause can add more than one sibling node
          sql_parse_from_clause(ctx, last_select_node);
        }
        attach_to_select = 1;
      }
      else if (str8_match(token.value, str8_lit("where"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_where_clause(ctx);
        attach_to_select = 1;
      }
      else if (str8_match(token.value, str8_lit("group"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_group_by_clause(ctx);
        attach_to_select = 1;
      }
      else if (str8_match(token.value, str8_lit("having"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_having_clause(ctx);
        attach_to_select = 1;
      }
      else if (str8_match(token.value, str8_lit("order"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_order_by_clause(ctx);
        attach_to_select = 1;
      }
      else if (str8_match(token.value, str8_lit("limit"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_limit_clause(ctx);
        attach_to_select = 1;
      }
      else if (str8_match(token.value, str8_lit("offset"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_offset_clause(ctx);
        attach_to_select = 1;
      }
      else if (str8_match(token.value, str8_lit("insert"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_insert_clause(ctx);
      }
      else if (str8_match(token.value, str8_lit("import"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_import_clause(ctx);
      }
      else if (str8_match(token.value, str8_lit("create"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_create_clause(ctx);
      }
      else if (str8_match(token.value, str8_lit("alter"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_alter_clause(ctx);
      }
      else if (str8_match(token.value, str8_lit("drop"), StringMatchFlag_CaseInsensitive))
      {
        // tec: only DROP INDEX is implemented, but DROP TABLE/DATABASE don't exist yet
        new_node = sql_parse_drop_index_clause(ctx);
      }
      else if (str8_match(token.value, str8_lit("delete"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_delete_clause(ctx);
        last_select_node = new_node;
      }
      else if (str8_match(token.value, str8_lit("describe"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_describe_clause(ctx);
      }
      else if (str8_match(token.value, str8_lit("explain"), StringMatchFlag_CaseInsensitive))
      {
        new_node = sql_parse_explain_clause(ctx);
        last_select_node = new_node ? new_node->first : NULL;
      }
      else
      {
        sql_parse_error_at(token.range, "unexpected keyword '%.*s'", str8_varg(token.value));
      }

      if (attach_to_select)
      {
        if (new_node && !last_select_node)
        {
          sql_parse_error_at(token.range, "clause requires a preceding 'select'");
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
      sql_parse_error_at(token.range, "unexpected token '%.*s'", str8_varg(token.value));
      ProfEnd();
      return NULL;
    }
  }

  ProfEnd();
  return root;
}

internal SQL_Node*
sql_parse_use_clause(SQL_ParseCtx *ctx)
{
  sql_advance(ctx, 1); // move past 'use'

  SQL_Node* use_node = push_array(ctx->arena, SQL_Node, 1);
  use_node->type = SQL_NodeType_Use;

  SQL_Node* tail = NULL;

  while (!sql_at_end(ctx))
  {
    SQL_Token token = sql_peek(ctx, 0);

    if (token.type == SQL_TokenType_Identifier)
    {
      SQL_Node *column_node = push_array(ctx->arena, SQL_Node, 1);
      column_node->type = SQL_NodeType_Database;
      column_node->value = token.value;
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
      sql_advance(ctx, 1);

      if (sql_match(ctx, SQL_TokenType_Symbol, str8_lit(",")))
      {
        continue;
      }
      else
      {
        break;
      }
    }
    else
    {
      sql_parse_error_at(token.range, "expected database name in 'use' clause, found '%.*s'",
                         str8_varg(token.value));
      return NULL;
    }
  }

  return use_node;
}

internal SQL_Node*
sql_parse_describe_clause(SQL_ParseCtx *ctx)
{
  sql_advance(ctx, 1); // move past 'describe'

  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected table name after 'describe', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* describe_node = push_array(ctx->arena, SQL_Node, 1);
  describe_node->type = SQL_NodeType_Describe;

  SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = sql_take(ctx).value;
  table_node->parent = describe_node;
  describe_node->first = describe_node->last = table_node;

  return describe_node;
}

internal SQL_Node*
sql_parse_explain_clause(SQL_ParseCtx *ctx)
{
  sql_advance(ctx, 1); // move past 'explain'

  if (!sql_check(ctx, SQL_TokenType_Keyword, str8_lit("select")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'select' after 'explain', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* select_node = sql_parse_select_clause(ctx);
  if (!select_node) return NULL;

  SQL_Node* explain_node = push_array(ctx->arena, SQL_Node, 1);
  explain_node->type = SQL_NodeType_Explain;
  explain_node->first = explain_node->last = select_node;
  select_node->parent = explain_node;

  return explain_node;
}

//~ tec: shared helpers used by SELECT / FROM / WHERE / GROUP BY / ORDER BY

internal SQL_Node*
sql_parse_column_ref(SQL_ParseCtx *ctx)
{
  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected column name, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Token first = sql_take(ctx);
  String8 name = first.value;
  Rng1U64 range = first.range;

  // tec: optional table-qualified 'table.column' - the qualifier isn't resolved here, it's just carried through as one string.
  if (sql_check(ctx, SQL_TokenType_Symbol, str8_lit(".")) && sql_peek(ctx, 1).type == SQL_TokenType_Identifier)
  {
    sql_advance(ctx, 1); // move past '.'
    SQL_Token second = sql_take(ctx);
    name = push_str8f(ctx->arena, "%.*s.%.*s", str8_varg(name), str8_varg(second.value));
    range = r1u64(range.min, second.range.max);
  }

  SQL_Node *node = push_array(ctx->arena, SQL_Node, 1);
  node->type = SQL_NodeType_Column;
  node->value = name;
  return node;
}

internal SQL_Node*
sql_parse_table_ref(SQL_ParseCtx *ctx)
{
  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected table name, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node *table_node = push_array(ctx->arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = sql_take(ctx).value;

  // tec: optional 'AS alias'
  if (sql_match(ctx, SQL_TokenType_Keyword, str8_lit("as")))
  {
    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected alias name after 'as', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node *alias_node = push_array(ctx->arena, SQL_Node, 1);
    alias_node->type = SQL_NodeType_Alias;
    alias_node->value = sql_take(ctx).value;
    alias_node->parent = table_node;
    table_node->first = table_node->last = alias_node;
  }

  return table_node;
}

internal SQL_Node*
sql_parse_select_item(SQL_ParseCtx *ctx)
{
  if (sql_at_end(ctx))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected column name in 'select' clause, but reached end of input");
    return NULL;
  }

  SQL_Token token = sql_peek(ctx, 0);

  // tec: bare '*' - can't be aliased or combined with other select items
  if (token.type == SQL_TokenType_Symbol && str8_match(token.value, str8_lit("*"), 0))
  {
    SQL_Node *star_node = push_array(ctx->arena, SQL_Node, 1);
    star_node->type = SQL_NodeType_Column;
    star_node->value = token.value;
    sql_advance(ctx, 1);
    return star_node;
  }

  if (token.type != SQL_TokenType_Identifier)
  {
    sql_parse_error_at(token.range, "expected column name or aggregate function in 'select' clause, found '%.*s'",
                       str8_varg(token.value));
    return NULL;
  }

  SQL_Node *item = NULL;

  // tec: identifier immediately followed by '(' -> aggregate/function call, e.g. COUNT(*), SUM(amount)
  if (sql_peek(ctx, 1).type == SQL_TokenType_Symbol && str8_match(sql_peek(ctx, 1).value, str8_lit("("), 0))
  {
    String8 func_name = token.value;
    sql_advance(ctx, 2); // move past function name and '('

    SQL_Node *operand = NULL;
    if (sql_check(ctx, SQL_TokenType_Symbol, str8_lit("*")))
    {
      operand = push_array(ctx->arena, SQL_Node, 1);
      operand->type = SQL_NodeType_Column;
      operand->value = str8_lit("*");
      sql_advance(ctx, 1);
    }
    else
    {
      operand = sql_parse_column_ref(ctx);
      if (!operand) return NULL;
    }

    if (!sql_check(ctx, SQL_TokenType_Symbol, str8_lit(")")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected ')' after aggregate function argument, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }
    sql_advance(ctx, 1); // move past ')'

    item = push_array(ctx->arena, SQL_Node, 1);
    item->type = SQL_NodeType_AggregateCall;
    item->value = func_name;
    item->first = item->last = operand;
    operand->parent = item;
  }
  else
  {
    item = sql_parse_column_ref(ctx);
    if (!item) return NULL;
  }

  // tec: optional alias
  if (sql_match(ctx, SQL_TokenType_Keyword, str8_lit("as")))
  {
    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected alias name after 'as', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node *alias_node = push_array(ctx->arena, SQL_Node, 1);
    alias_node->type = SQL_NodeType_Alias;
    alias_node->value = sql_take(ctx).value;
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
  }

  return item;
}

internal SQL_Node*
sql_parse_select_clause(SQL_ParseCtx *ctx)
{
  sql_advance(ctx, 1); // Move past 'select'

  SQL_Node* select_node = push_array(ctx->arena, SQL_Node, 1);
  select_node->type = SQL_NodeType_Select;

  SQL_Node* column_list = push_array(ctx->arena, SQL_Node, 1);
  column_list->type = SQL_NodeType_ColumnList;
  column_list->parent = select_node;
  select_node->first = select_node->last = column_list;

  for (;;)
  {
    SQL_Node *item = sql_parse_select_item(ctx);
    if (!item) return NULL;

    item->parent = column_list;
    DLLPushBack(column_list->first, column_list->last, item);

    if (sql_match(ctx, SQL_TokenType_Symbol, str8_lit(",")))
    {
      continue;
    }
    break;
  }

  return select_node;
}

internal SQL_Node*
sql_parse_join_clause(SQL_ParseCtx *ctx)
{
  String8 join_type = str8_lit("inner");

  if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("inner")))
  {
    sql_advance(ctx, 1); // move past 'inner'
  }
  else if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("left")))
  {
    join_type = str8_lit("left");
    sql_advance(ctx, 1); // move past 'left'

    if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("outer")))
    {
      sql_advance(ctx, 1); // tec: 'outer' is a no-op synonym alongside 'left'
    }
  }

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("join")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'join' keyword, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node *table_node = sql_parse_table_ref(ctx);
  if (!table_node) return NULL;

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("on")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'on' after joined table, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node *condition = sql_parse_logical_expression(ctx);
  if (!condition) return NULL;

  SQL_Node *join_node = push_array(ctx->arena, SQL_Node, 1);
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
sql_parse_from_clause(SQL_ParseCtx *ctx, SQL_Node *select_node)
{
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("from")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'from' keyword after 'select' clause");
    return NULL;
  }

  SQL_Node *primary_table = sql_parse_table_ref(ctx);
  if (!primary_table) return NULL;

  primary_table->parent = select_node;
  DLLPushBack(select_node->first, select_node->last, primary_table);

  // tec: additional comma-separated tables (implicit cross join) and/or JOIN clauses
  for (;;)
  {
    if (sql_match(ctx, SQL_TokenType_Symbol, str8_lit(",")))
    {
      SQL_Node *extra_table = sql_parse_table_ref(ctx);
      if (!extra_table) return NULL;

      extra_table->parent = select_node;
      DLLPushBack(select_node->first, select_node->last, extra_table);
      continue;
    }

    if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("join")) ||
        sql_check(ctx, SQL_TokenType_Keyword, str8_lit("inner")) ||
        sql_check(ctx, SQL_TokenType_Keyword, str8_lit("left")))
    {
      SQL_Node *join_node = sql_parse_join_clause(ctx);
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
sql_parse_comparison_expression(SQL_ParseCtx *ctx)
{
  // tec: expressions inside parentheses
  if (sql_match(ctx, SQL_TokenType_Symbol, str8_lit("(")))
  {
    SQL_Node *expr = sql_parse_logical_expression(ctx);
    if (!expr)
    {
      return NULL;
    }

    // tec: expect closing ')'
    if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit(")")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected ')' after expression, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    return expr;
  }

  // tec: left hand side column/literal
  SQL_Node *left = sql_parse_expression(ctx);
  if (!left)
  {
    return NULL;
  }

  // tec: 'IS NULL' / 'IS NOT NULL'
  if (sql_match(ctx, SQL_TokenType_Keyword, str8_lit("is")))
  {
    B32 negate = sql_match(ctx, SQL_TokenType_Keyword, str8_lit("not"));

    if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("null")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected 'null' after 'is%s', found '%.*s'", negate ? " not" : "",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node *is_null_node = push_array(ctx->arena, SQL_Node, 1);
    is_null_node->type = SQL_NodeType_Operator;
    is_null_node->value = negate ? str8_lit("is not null") : str8_lit("is null");
    is_null_node->first = left;
    is_null_node->last = left;
    left->parent = is_null_node;

    return is_null_node;
  }

  // Expect comparison operator
  SQL_Token op_token = sql_peek(ctx, 0);
  if (!(op_token.type == SQL_TokenType_Operator || op_token.type == SQL_TokenType_Keyword))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected comparison operator, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  // Create comparison operator node
  SQL_Node *operator_node = push_array(ctx->arena, SQL_Node, 1);
  operator_node->type = SQL_NodeType_Operator;
  operator_node->value = sql_take(ctx).value; // Move past operator

  // tec: right hand side column/literal
  SQL_Node *right = sql_parse_expression(ctx);
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
sql_parse_and_expression(SQL_ParseCtx *ctx)
{
  SQL_Node *left = sql_parse_comparison_expression(ctx);
  if (!left)
  {
    return NULL;
  }

  while (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("and")))
  {
    SQL_Node *operator_node = push_array(ctx->arena, SQL_Node, 1);
    operator_node->type = SQL_NodeType_Operator;
    operator_node->value = sql_take(ctx).value; // tec: move past 'and'

    SQL_Node *right = sql_parse_comparison_expression(ctx);
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
sql_parse_logical_expression(SQL_ParseCtx *ctx)
{
  // tec: 'and' binds tighter than 'or', so 'or' is the outer loop here rather than one flat equal-precedence chain
  SQL_Node *left = sql_parse_and_expression(ctx);
  if (!left)
  {
    return NULL;
  }

  while (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("or")))
  {
    // tec: create logical operator node
    SQL_Node *operator_node = push_array(ctx->arena, SQL_Node, 1);
    operator_node->type = SQL_NodeType_Operator;
    operator_node->value = sql_take(ctx).value; // tec: move past 'or'

    SQL_Node *right = sql_parse_and_expression(ctx);
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
sql_parse_where_clause(SQL_ParseCtx *ctx)
{
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("where")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx), "expected 'where' keyword");
    return NULL;
  }

  SQL_Node* where_node = push_array(ctx->arena, SQL_Node, 1);
  where_node->type = SQL_NodeType_Where;

  SQL_Node* logic_node = sql_parse_logical_expression(ctx);
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
sql_parse_having_clause(SQL_ParseCtx *ctx)
{
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("having")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx), "expected 'having' keyword");
    return NULL;
  }

  SQL_Node* having_node = push_array(ctx->arena, SQL_Node, 1);
  having_node->type = SQL_NodeType_Having;

  SQL_Node* logic_node = sql_parse_logical_expression(ctx);
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
sql_parse_group_by_clause(SQL_ParseCtx *ctx)
{
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("group")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx), "expected 'group' keyword");
    return NULL;
  }

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("by")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'by' after 'group', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* group_by_root = push_array(ctx->arena, SQL_Node, 1);
  group_by_root->type = SQL_NodeType_GroupBy;

  for (;;)
  {
    SQL_Node *column_node = sql_parse_column_ref(ctx);
    if (!column_node) return NULL;

    column_node->parent = group_by_root;
    DLLPushBack(group_by_root->first, group_by_root->last, column_node);

    if (sql_match(ctx, SQL_TokenType_Symbol, str8_lit(",")))
    {
      continue;
    }
    break;
  }

  return group_by_root;
}

internal SQL_Node*
sql_parse_limit_clause(SQL_ParseCtx *ctx)
{
  sql_advance(ctx, 1); // move past 'limit'

  if (!sql_check(ctx, SQL_TokenType_Number, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected a number after 'limit', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* limit_node = push_array(ctx->arena, SQL_Node, 1);
  limit_node->type = SQL_NodeType_Limit;
  limit_node->value = sql_take(ctx).value;

  return limit_node;
}

internal SQL_Node*
sql_parse_offset_clause(SQL_ParseCtx *ctx)
{
  sql_advance(ctx, 1); // move past 'offset'

  if (!sql_check(ctx, SQL_TokenType_Number, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected a number after 'offset', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* offset_node = push_array(ctx->arena, SQL_Node, 1);
  offset_node->type = SQL_NodeType_Offset;
  offset_node->value = sql_take(ctx).value;

  return offset_node;
}

internal SQL_Node*
sql_parse_insert_clause(SQL_ParseCtx *ctx)
{
  SQL_Node* insert_node = push_array(ctx->arena, SQL_Node, 1);
  insert_node->type = SQL_NodeType_Insert;

  sql_advance(ctx, 1); // tec: move past 'insert'

  // tec: expect 'into'
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("into")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'into' keyword in 'insert' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  // tec: expect table name
  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected table name in 'insert' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = sql_take(ctx).value;

  insert_node->first = table_node;
  table_node->parent = insert_node;

  // tec: optional column list
  SQL_Node* column_list_node = NULL;
  if (sql_match(ctx, SQL_TokenType_Symbol, str8_lit("(")))
  {
    column_list_node = push_array(ctx->arena, SQL_Node, 1);
    column_list_node->type = SQL_NodeType_ColumnList;
    column_list_node->parent = insert_node;

    while (!sql_at_end(ctx) && !sql_check(ctx, SQL_TokenType_Symbol, (String8){0}))
    {
      if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
      {
        SQL_Token bad = sql_peek(ctx, 0);
        sql_parse_error_at(bad.range,
                           "expected column name in 'insert' statement, found '%.*s'",
                           str8_varg(bad.value));
        return NULL;
      }

      SQL_Node* column_node = push_array(ctx->arena, SQL_Node, 1);
      column_node->type = SQL_NodeType_Column;
      column_node->value = sql_take(ctx).value;
      column_node->parent = column_list_node;

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
      sql_match(ctx, SQL_TokenType_Symbol, str8_lit(","));
    }

    // tec: expect closing ')'
    if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit(")")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected closing ')' in column list, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    // tec: attach column list node to insert_node
    table_node->next = column_list_node;
    insert_node->last = column_list_node;
  }

  // tec: expect 'values' keyword
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("values")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'values' keyword in 'insert' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  // tec: parse 'values' clause
  SQL_Node* values_node = sql_parse_values_clause(ctx);
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
sql_parse_import_clause(SQL_ParseCtx *ctx)
{
  SQL_Node* import_node = push_array(ctx->arena, SQL_Node, 1);
  import_node->type = SQL_NodeType_Import;

  sql_advance(ctx, 1); // tec: move past 'IMPORT'

  // tec: expect 'INTO'
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("into")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'into' keyword in 'import' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  // tec: expect table name
  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected table name after 'into' in 'import' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = sql_take(ctx).value;

  import_node->first = table_node;
  table_node->parent = import_node;

  // tec: expect 'FROM'
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("from")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'from' keyword in 'import' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  // tec: expect file path as string literal
  if (!sql_check(ctx, SQL_TokenType_String, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected file path after 'from' in 'import' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* path_node = push_array(ctx->arena, SQL_Node, 1);
  path_node->type = SQL_NodeType_Literal;
  path_node->value = sql_take(ctx).value;

  table_node->next = path_node;
  path_node->prev = table_node;
  path_node->parent = import_node;
  import_node->last = path_node;

  return import_node;
}

internal SQL_Node*
sql_parse_create_clause(SQL_ParseCtx *ctx)
{
  SQL_Node* create_node = push_array(ctx->arena, SQL_Node, 1);
  create_node->type = SQL_NodeType_Create;

  sql_advance(ctx, 1); // tec: move past 'create'

  // tec: expect 'table' or 'database'
  if (!sql_check(ctx, SQL_TokenType_Keyword, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'table' or 'database' keyword in 'create' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }
  SQL_Token keyword_tok = sql_take(ctx); // tec: move past keyword
  String8 keyword = keyword_tok.value;

  if (str8_match(keyword, str8_lit("database"), StringMatchFlag_CaseInsensitive))
  {
    // tec: expect database name
    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected database name in 'create' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node* database_node = push_array(ctx->arena, SQL_Node, 1);
    database_node->type = SQL_NodeType_Database;
    database_node->value = sql_take(ctx).value;

    create_node->first = database_node;
    create_node->last = database_node;
    database_node->parent = create_node;

  }
  else if (str8_match(keyword, str8_lit("table"), StringMatchFlag_CaseInsensitive))
  {
    // tec: expect table name
    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected table name in 'create' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
    table_node->type = SQL_NodeType_Table;
    table_node->value = sql_take(ctx).value;

    create_node->first = table_node;
    create_node->last = table_node;
    table_node->parent = create_node;

    // tec: expect column definitions
    if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit("(")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected '(' in 'create table' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node* prev_column = NULL;
    while (!sql_at_end(ctx) && !sql_check(ctx, SQL_TokenType_Symbol, (String8){0}))
    {
      // tec: expect column name
      if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
      {
        SQL_Token bad = sql_peek(ctx, 0);
        sql_parse_error_at(bad.range,
                           "expected column name in 'create table' statement, found '%.*s'",
                           str8_varg(bad.value));
        return NULL;
      }

      SQL_Node* column_node = push_array(ctx->arena, SQL_Node, 1);
      column_node->type = SQL_NodeType_Column;
      column_node->value = sql_take(ctx).value;

      // tec: expect column type
      if (!sql_check(ctx, SQL_TokenType_Keyword, (String8){0}))
      {
        sql_parse_error_at(sql_ctx_error_range(ctx),
                           "expected column type in 'create table' statement, found '%.*s'",
                           str8_varg(sql_ctx_text_or_eof(ctx)));
        return NULL;
      }

      SQL_Node* type_node = push_array(ctx->arena, SQL_Node, 1);
      type_node->type = SQL_NodeType_Type;
      type_node->value = sql_take(ctx).value;

      column_node->first = type_node;
      column_node->last = type_node;
      type_node->parent = column_node;

      // tec: zero or more column constraints, in any order, until a ',' or ')'
      for (;;)
      {
        if (!sql_check(ctx, SQL_TokenType_Keyword, (String8){0})) break;
        String8 constraint_kw = sql_peek(ctx, 0).value;

        if (str8_match(constraint_kw, str8_lit("not"), StringMatchFlag_CaseInsensitive))
        {
          sql_advance(ctx, 1); // tec: move past 'not'
          if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("null")))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected 'null' after 'not' in column constraint, found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }

          SQL_Node* c = push_array(ctx->arena, SQL_Node, 1);
          c->type = SQL_NodeType_NotNull;
          c->parent = column_node;
          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("unique"), StringMatchFlag_CaseInsensitive))
        {
          sql_advance(ctx, 1); // tec: move past 'unique'

          SQL_Node* c = push_array(ctx->arena, SQL_Node, 1);
          c->type = SQL_NodeType_Unique;
          c->parent = column_node;
          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("primary"), StringMatchFlag_CaseInsensitive))
        {
          sql_advance(ctx, 1); // tec: move past 'primary'
          if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("key")))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected 'key' after 'primary' in column constraint, found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }

          SQL_Node* c = push_array(ctx->arena, SQL_Node, 1);
          c->type = SQL_NodeType_PrimaryKey;
          c->parent = column_node;
          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("references"), StringMatchFlag_CaseInsensitive))
        {
          sql_advance(ctx, 1); // tec: move past 'references'
          if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected referenced table name after 'references', found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }
          String8 ref_table = sql_take(ctx).value;

          if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit("(")))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected '(' after referenced table name, found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }

          if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected referenced column name, found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }
          String8 ref_column = sql_take(ctx).value;

          if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit(")")))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected ')' after referenced column name, found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }

          SQL_Node* c = push_array(ctx->arena, SQL_Node, 1);
          c->type = SQL_NodeType_ForeignKey;
          c->value = ref_table;
          c->parent = column_node;

          SQL_Node* ref_col_node = push_array(ctx->arena, SQL_Node, 1);
          ref_col_node->type = SQL_NodeType_Column;
          ref_col_node->value = ref_column;
          ref_col_node->parent = c;
          c->first = c->last = ref_col_node;

          DLLPushBack(column_node->first, column_node->last, c);
        }
        else if (str8_match(constraint_kw, str8_lit("check"), StringMatchFlag_CaseInsensitive))
        {
          sql_advance(ctx, 1); // tec: move past 'check'
          if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit("(")))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected '(' after 'check', found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }

          // tec: capture the raw source text (not just the parsed tree) so it can be saved and reparsed on table load
          Rng1U64 expr_start_range = sql_ctx_range_at(ctx, ctx->pos);

          SQL_Node* expr = sql_parse_logical_expression(ctx);
          if (!expr) return NULL;

          Rng1U64 expr_end_range = sql_ctx_range_at(ctx, ctx->pos > 0 ? ctx->pos - 1 : 0);
          String8 expr_text = str8_substr(g_sql_source_text, r1u64(expr_start_range.min, expr_end_range.max));

          if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit(")")))
          {
            sql_parse_error_at(sql_ctx_error_range(ctx),
                               "expected ')' after 'check' expression, found '%.*s'",
                               str8_varg(sql_ctx_text_or_eof(ctx)));
            return NULL;
          }

          SQL_Node* c = push_array(ctx->arena, SQL_Node, 1);
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
      sql_match(ctx, SQL_TokenType_Symbol, str8_lit(","));
    }

    // tec: expect closing ')'
    if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit(")")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected closing ')' in 'create table' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

  }
  else if (str8_match(keyword, str8_lit("index"), StringMatchFlag_CaseInsensitive))
  {
    // tec: CREATE INDEX idx_name ON table_name (column_name);
    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected index name in 'create index' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node* index_node = push_array(ctx->arena, SQL_Node, 1);
    index_node->type = SQL_NodeType_Index;
    index_node->value = sql_take(ctx).value;

    create_node->first = index_node;
    create_node->last = index_node;
    index_node->parent = create_node;

    if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("on")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected 'on' in 'create index' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected table name in 'create index' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
    table_node->type = SQL_NodeType_Table;
    table_node->value = sql_take(ctx).value;
    table_node->parent = index_node;
    index_node->first = table_node;
    index_node->last = table_node;

    if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit("(")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected '(' in 'create index' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected column name in 'create index' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    SQL_Node* column_node = push_array(ctx->arena, SQL_Node, 1);
    column_node->type = SQL_NodeType_Column;
    column_node->value = sql_take(ctx).value;
    column_node->parent = index_node;
    table_node->next = column_node;
    column_node->prev = table_node;
    index_node->last = column_node;

    if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit(")")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected closing ')' in 'create index' statement, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }
  }
  else
  {
    sql_parse_error_at(keyword_tok.range,
                       "unexpected keyword '%.*s' in 'create' statement, expected 'table', 'database', or 'index'",
                       str8_varg(keyword));
    return NULL;
  }

  return create_node;
}

internal SQL_Node*
sql_parse_drop_index_clause(SQL_ParseCtx *ctx)
{
  // tec: DROP INDEX idx_name ON table_name;
  sql_advance(ctx, 1); // tec: move past 'drop'

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("index")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'index' after 'drop', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected index name in 'drop index' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* drop_index_node = push_array(ctx->arena, SQL_Node, 1);
  drop_index_node->type = SQL_NodeType_DropIndex;
  drop_index_node->value = sql_take(ctx).value;

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("on")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'on' in 'drop index' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected table name in 'drop index' statement, found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = sql_take(ctx).value;
  table_node->parent = drop_index_node;
  drop_index_node->first = table_node;
  drop_index_node->last = table_node;

  return drop_index_node;
}

internal SQL_Node*
sql_parse_alter_clause(SQL_ParseCtx *ctx)
{
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("alter")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx), "expected 'alter' keyword");
    return NULL;
  }

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("table")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'table' keyword after 'alter', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  // tec: expect table name
  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected table name after 'alter table', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* alter_node = push_array(ctx->arena, SQL_Node, 1);
  alter_node->type = SQL_NodeType_Alter;

  SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = sql_take(ctx).value; // tec: move past table name

  table_node->parent = alter_node;
  alter_node->first = table_node;

  // tec: expect alter operation
  if (!sql_check(ctx, SQL_TokenType_Keyword, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected operation after 'alter table <table>', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* operation_node = push_array(ctx->arena, SQL_Node, 1);
  operation_node->parent = alter_node;
  alter_node->first->next = operation_node;

  // tec: alter operations
  if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("add")))
  {
    sql_advance(ctx, 1); // tec: move past 'add'
    if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("column")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected 'column' after 'add' in 'alter table', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    // tec: expect column name
    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected column name after 'add column', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }
    operation_node->type = SQL_NodeType_Alter_AddColumn;

    SQL_Node* column_node = push_array(ctx->arena, SQL_Node, 1);
    column_node->type = SQL_NodeType_Column;
    column_node->parent = alter_node;
    column_node->value = sql_take(ctx).value; // tec: move past column name

    operation_node->first = column_node;

    // tec: optional column type
    if (sql_check(ctx, SQL_TokenType_Keyword, (String8){0}))
    {
      SQL_Node* type_node = push_array(ctx->arena, SQL_Node, 1);
      type_node->type = SQL_NodeType_Type;
      type_node->value = sql_take(ctx).value; // tec: move past column type
      type_node->parent = operation_node;
      operation_node->first->next = type_node;
    }
  }
  else if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("drop")))
  {
    sql_advance(ctx, 1); // Move past 'DROP'
    if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("column")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected 'column' after 'drop' in 'alter table', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected column name after 'drop column', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }
    operation_node->type = SQL_NodeType_Alter_DropColumn;
    operation_node->value = sql_take(ctx).value; // tec: move past column name
  }
  else if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("rename")))
  {
    sql_advance(ctx, 1); // tec: move past 'rename'
    if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("to")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected 'to' after 'rename' in 'alter table', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

    if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected new table name after 'rename to', found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }
    operation_node->type = SQL_NodeType_Alter_Rename;
    operation_node->value = sql_take(ctx).value; // tec: move past new table name
  }
  else
  {
    SQL_Token bad = sql_peek(ctx, 0);
    sql_parse_error_at(bad.range,
                       "unknown 'alter table' operation '%.*s'",
                       str8_varg(bad.value));
    return NULL;
  }

  return alter_node;
}

internal SQL_Node*
sql_parse_values_clause(SQL_ParseCtx *ctx)
{
  SQL_Node* values_root = push_array(ctx->arena, SQL_Node, 1);
  values_root->type = SQL_NodeType_Value;

  SQL_Node* prev_value_group = NULL;

  while (!sql_at_end(ctx))
  {
    // tec: expect opening '('
    if (!sql_check(ctx, SQL_TokenType_Symbol, str8_lit("(")))
    {
      SQL_Token bad = sql_peek(ctx, 0);
      sql_parse_error_at(bad.range,
                         "expected '(' before values in 'values' clause, found '%.*s'",
                         str8_varg(bad.value));
      return NULL;
    }
    sql_advance(ctx, 1); // tec: move past '('

    SQL_Node* value_group = push_array(ctx->arena, SQL_Node, 1);
    value_group->type = SQL_NodeType_ValueGroup;

    SQL_Node* prev_value = NULL;

    while (!sql_at_end(ctx) && !sql_check(ctx, SQL_TokenType_Symbol, (String8){0}))
    {
      SQL_Token tok = sql_peek(ctx, 0);
      B32 is_null_token = tok.type == SQL_TokenType_Keyword &&
                          str8_match(tok.value, str8_lit("null"), StringMatchFlag_CaseInsensitive);

      if (tok.type != SQL_TokenType_Number && tok.type != SQL_TokenType_String && !is_null_token)
      {
        sql_parse_error_at(tok.range,
                           "expected a literal value in 'values' clause, found '%.*s'",
                           str8_varg(tok.value));
        return NULL;
      }

      SQL_Node* value_node = push_array(ctx->arena, SQL_Node, 1);
      value_node->type = is_null_token ? SQL_NodeType_Null :
        (tok.type == SQL_TokenType_Number ? SQL_NodeType_Numeric : SQL_NodeType_Literal);
      value_node->value = tok.value;
      sql_advance(ctx, 1);

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

      if (sql_check(ctx, SQL_TokenType_Symbol, str8_lit(",")))
      {
        sql_advance(ctx, 1); // tec: skip ','
        continue;
      }
    }
    value_group->last = prev_value;

    // tec: expect closing ')'
    if (!sql_match(ctx, SQL_TokenType_Symbol, str8_lit(")")))
    {
      sql_parse_error_at(sql_ctx_error_range(ctx),
                         "expected closing ')' after values in 'values' clause, found '%.*s'",
                         str8_varg(sql_ctx_text_or_eof(ctx)));
      return NULL;
    }

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

    if (sql_check(ctx, SQL_TokenType_Symbol, str8_lit(",")))
    {
      sql_advance(ctx, 1); // tec: move past ',' to next value group
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
sql_parse_order_by_clause(SQL_ParseCtx *ctx)
{
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("order")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx), "expected 'order' keyword");
    return NULL;
  }

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("by")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'by' after 'order', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Node* order_by_root = push_array(ctx->arena, SQL_Node, 1);
  order_by_root->type = SQL_NodeType_OrderBy;

  for (;;)
  {
    SQL_Node* column_node = sql_parse_column_ref(ctx);
    if (!column_node) return NULL;

    if (sql_check(ctx, SQL_TokenType_Keyword, (String8){0}))
    {
      SQL_Token tok = sql_peek(ctx, 0);
      SQL_Node* sort_node = push_array(ctx->arena, SQL_Node, 1);
      sort_node->value = tok.value;

      if (str8_match(tok.value, str8_lit("asc"), StringMatchFlag_CaseInsensitive))
      {
        sort_node->type = SQL_NodeType_Ascending;
        column_node->first = sort_node;
        sort_node->parent = column_node;
        sql_advance(ctx, 1);
      }
      else if (str8_match(tok.value, str8_lit("desc"), StringMatchFlag_CaseInsensitive))
      {
        sort_node->type = SQL_NodeType_Descending;
        column_node->first = sort_node;
        sort_node->parent = column_node;
        sql_advance(ctx, 1);
      }
    }

    column_node->parent = order_by_root;
    DLLPushBack(order_by_root->first, order_by_root->last, column_node);

    if (sql_match(ctx, SQL_TokenType_Symbol, str8_lit(",")))
    {
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
sql_parse_delete_clause(SQL_ParseCtx *ctx)
{
  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("delete")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx), "expected 'delete' keyword");
    return NULL;
  }

  if (!sql_match(ctx, SQL_TokenType_Keyword, str8_lit("from")))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected 'from' keyword after 'delete', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  // tec: expect table name
  if (!sql_check(ctx, SQL_TokenType_Identifier, (String8){0}))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "expected table name after 'delete from', found '%.*s'",
                       str8_varg(sql_ctx_text_or_eof(ctx)));
    return NULL;
  }

  SQL_Token table_tok = sql_peek(ctx, 0);

  SQL_Node* delete_node = push_array(ctx->arena, SQL_Node, 1);
  delete_node->type = SQL_NodeType_Delete;
  delete_node->value = table_tok.value;

  SQL_Node* table_node = push_array(ctx->arena, SQL_Node, 1);
  table_node->type = SQL_NodeType_Table;
  table_node->value = table_tok.value;
  table_node->parent = delete_node;

  delete_node->first = delete_node->last = table_node;

  sql_advance(ctx, 1); // tec: move past table name

  // tec: check for optional WHERE clause
  if (sql_check(ctx, SQL_TokenType_Keyword, str8_lit("where")))
  {
    SQL_Node* where_clause = sql_parse_where_clause(ctx);
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
sql_parse_expression(SQL_ParseCtx *ctx)
{
  if (sql_at_end(ctx))
  {
    sql_parse_error_at(sql_ctx_error_range(ctx),
                       "unexpected end of input in expression");
    return NULL;
  }

  SQL_Token token = sql_peek(ctx, 0);

  if (token.type == SQL_TokenType_Identifier)
  {
    // tec: identifier immediately followed by '(' -> aggregate/function call, e.g. HAVING COUNT(*) > 1
    if (sql_peek(ctx, 1).type == SQL_TokenType_Symbol && str8_match(sql_peek(ctx, 1).value, str8_lit("("), 0))
    {
      String8 func_name = token.value;
      sql_advance(ctx, 2); // move past function name and '('

      SQL_Node *operand = NULL;
      if (sql_check(ctx, SQL_TokenType_Symbol, str8_lit("*")))
      {
        operand = push_array(ctx->arena, SQL_Node, 1);
        operand->type = SQL_NodeType_Column;
        operand->value = str8_lit("*");
        sql_advance(ctx, 1);
      }
      else
      {
        operand = sql_parse_column_ref(ctx);
        if (!operand) return NULL;
      }

      if (!sql_check(ctx, SQL_TokenType_Symbol, str8_lit(")")))
      {
        sql_parse_error_at(sql_ctx_error_range(ctx),
                           "expected ')' after aggregate function argument, found '%.*s'",
                           str8_varg(sql_ctx_text_or_eof(ctx)));
        return NULL;
      }
      sql_advance(ctx, 1); // move past ')'

      SQL_Node *call_node = push_array(ctx->arena, SQL_Node, 1);
      call_node->type = SQL_NodeType_AggregateCall;
      call_node->value = func_name;
      call_node->first = call_node->last = operand;
      operand->parent = call_node;
      return call_node;
    }

    return sql_parse_column_ref(ctx);
  }

  if (token.type == SQL_TokenType_Number || token.type == SQL_TokenType_String)
  {
    SQL_Node *node = push_array(ctx->arena, SQL_Node, 1);
    node->type = (token.type == SQL_TokenType_Number) ? SQL_NodeType_Numeric : SQL_NodeType_Literal;
    node->value = token.value;
    sql_advance(ctx, 1);
    return node;
  }

  sql_parse_error_at(token.range, "unexpected token '%.*s' in expression", str8_varg(token.value));
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
    case SQL_NodeType_Explain: result = str8_lit("SQL_NodeType_Explain"); break;
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
