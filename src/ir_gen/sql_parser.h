/* date = February 5th 2025 4:20 pm */

#ifndef SQL_PARSER_H
#define SQL_PARSER_H

typedef enum SQL_TokenType
{
  SQL_TokenType_Keyword,
  SQL_TokenType_Identifier,
  SQL_TokenType_Operator,
  SQL_TokenType_Symbol,
  SQL_TokenType_Number,
  SQL_TokenType_String,
  SQL_TokenType_EOF,
} SQL_TokenType;

typedef struct SQL_Token SQL_Token;
struct SQL_Token
{
  SQL_TokenType type;
  String8 value;
  Rng1U64 range;
};

//~ tec: keywords and operators

global String8 g_sql_keywords[] =
{
  str8_lit_comp("select"),
  str8_lit_comp("from"),
  str8_lit_comp("where"),
  str8_lit_comp("and"),
  str8_lit_comp("or"),
  str8_lit_comp("join"),
  str8_lit_comp("on"),
  str8_lit_comp("to"),
  str8_lit_comp("group"),
  str8_lit_comp("order"),
  str8_lit_comp("by"),
  str8_lit_comp("asc"),
  str8_lit_comp("desc"),
  str8_lit_comp("having"),
  str8_lit_comp("limit"),
  str8_lit_comp("offset"),
  str8_lit_comp("use"),
  str8_lit_comp("into"),
  str8_lit_comp("insert"),
  str8_lit_comp("import"),
  str8_lit_comp("create"),
  str8_lit_comp("describe"),
  str8_lit_comp("explain"),
  str8_lit_comp("contains"),
  str8_lit_comp("equals"),
  str8_lit_comp("drop"),
  str8_lit_comp("table"),
  str8_lit_comp("database"),
  str8_lit_comp("delete"),
  str8_lit_comp("values"),
  str8_lit_comp("alter"),
  str8_lit_comp("add"),
  str8_lit_comp("rename"),
  str8_lit_comp("column"),
  str8_lit_comp("index"),
  str8_lit_comp("null"),
  str8_lit_comp("is"),
  str8_lit_comp("not"),
  str8_lit_comp("unique"),
  str8_lit_comp("primary"),
  str8_lit_comp("key"),
  str8_lit_comp("references"),
  str8_lit_comp("check"),
  str8_lit_comp("as"),
  str8_lit_comp("inner"),
  str8_lit_comp("left"),
  str8_lit_comp("outer"),
  str8_lit_comp("u32"),
  str8_lit_comp("u64"),
  str8_lit_comp("f32"),
  str8_lit_comp("f64"),
  str8_lit_comp("string8"),
};

global String8 g_sql_operators[] =
{
  str8_lit_comp("="),
  str8_lit_comp("=="),
  str8_lit_comp("!="),
  str8_lit_comp(">"),
  str8_lit_comp("<"),
  str8_lit_comp(">="),
  str8_lit_comp("<="),
};

global String8 g_sql_symbols[] =
{
  str8_lit_comp(","),
  str8_lit_comp("("),
  str8_lit_comp(")"),
  str8_lit_comp(";"),
  str8_lit_comp("*"),
  str8_lit_comp("."),
};

internal B32 sql_is_string_keyword(String8 string);
internal B32 sql_is_string_operator(String8 string);
internal B32 sql_is_string_symbol(String8 string);

typedef struct SQL_TokenizeResult SQL_TokenizeResult;
struct SQL_TokenizeResult
{
  SQL_Token* tokens;
  U64 count;
};

internal SQL_TokenizeResult sql_tokenize_from_text(Arena* arena, String8 text);
internal void sql_tokens_print(SQL_TokenizeResult tokens);

//~ tec: parse error reporting - only the first error is kept, since later ones are usually cascades once the token stream is out of sync

typedef struct SQL_ParseError SQL_ParseError;
struct SQL_ParseError
{
  B32 has_error;
  Rng1U64 range;
};

global SQL_ParseError g_sql_parse_error;
global String8 g_sql_source_text;

internal void sql_parse_error_at(Rng1U64 range, char *fmt, ...);

//~ tec: parse context
typedef struct SQL_ParseCtx SQL_ParseCtx;
struct SQL_ParseCtx
{
  Arena     *arena;
  SQL_Token *tokens;
  U64        count;
  U64        pos;
};

internal SQL_ParseCtx sql_parse_ctx_make(Arena *arena, SQL_Token *tokens, U64 count);

internal SQL_Token sql_peek(SQL_ParseCtx *ctx, U64 lookahead); 
internal SQL_Token sql_take(SQL_ParseCtx *ctx);
internal void      sql_advance(SQL_ParseCtx *ctx, U64 n);
internal B32       sql_at_end(SQL_ParseCtx *ctx);

// tec: value == {0} means "any value of this type"
internal B32 sql_check(SQL_ParseCtx *ctx, SQL_TokenType type, String8 value); // peek match, doesn't consume
internal B32 sql_match(SQL_ParseCtx *ctx, SQL_TokenType type, String8 value); // check + consume if it matched

// tec: scans forward from ctx->pos for a depth-0 match (respects '(' ')' nesting), without consuming
internal B32 sql_find(SQL_ParseCtx *ctx, SQL_TokenType type, String8 value, U64 *out_pos);

internal Rng1U64 sql_ctx_range_at(SQL_ParseCtx *ctx, U64 pos);
internal Rng1U64 sql_ctx_error_range(SQL_ParseCtx *ctx);
internal String8  sql_ctx_text_or_eof(SQL_ParseCtx *ctx);

//~ tec: sql ast
typedef enum SQL_NodeType
{
  SQL_NodeType_Use,
  SQL_NodeType_Select,
  SQL_NodeType_Column,
  SQL_NodeType_ColumnList,
  SQL_NodeType_Table,
  SQL_NodeType_Database,
  SQL_NodeType_Where,
  SQL_NodeType_Operator,
  SQL_NodeType_Numeric,
  SQL_NodeType_Identifier,
  SQL_NodeType_Literal,
  SQL_NodeType_Insert,
  SQL_NodeType_Import,
  SQL_NodeType_Delete,
  SQL_NodeType_Create,
  SQL_NodeType_Drop,
  SQL_NodeType_Index,
  SQL_NodeType_DropIndex,
  SQL_NodeType_Null,
  SQL_NodeType_NotNull,
  SQL_NodeType_Unique,
  SQL_NodeType_PrimaryKey,
  SQL_NodeType_ForeignKey,
  SQL_NodeType_Check,
  SQL_NodeType_Alter,
  SQL_NodeType_Row,
  SQL_NodeType_Value,
  SQL_NodeType_ValueGroup,
  SQL_NodeType_Type,
  SQL_NodeType_OrderBy,
  SQL_NodeType_Ascending,
  SQL_NodeType_Descending,
  SQL_NodeType_Alter_AddColumn,
  SQL_NodeType_Alter_ColumnType,
  SQL_NodeType_Alter_DropColumn,
  SQL_NodeType_Alter_Rename,
  SQL_NodeType_Join,
  SQL_NodeType_Alias,
  SQL_NodeType_GroupBy,
  SQL_NodeType_Having,
  SQL_NodeType_Limit,
  SQL_NodeType_Offset,
  SQL_NodeType_AggregateCall,
  SQL_NodeType_Describe,
  SQL_NodeType_Explain,
} SQL_NodeType;

typedef struct SQL_Node SQL_Node;
struct SQL_Node
{
  SQL_Node *next;
  SQL_Node *prev;
  SQL_Node *parent;
  SQL_Node *first;
  SQL_Node *last;
  
  String8 value;
  SQL_NodeType type;
};

internal SQL_Node* sql_parse_use_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_select_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_select_item(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_column_ref(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_table_ref(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_from_clause(SQL_ParseCtx *ctx, SQL_Node *select_node);
internal SQL_Node* sql_parse_join_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_where_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_insert_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_import_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_create_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_alter_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_delete_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_drop_index_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_values_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_expression(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_comparison_expression(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_and_expression(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_logical_expression(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_order_by_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_group_by_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_having_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_limit_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_offset_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_describe_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse_explain_clause(SQL_ParseCtx *ctx);
internal SQL_Node* sql_parse(Arena* arena, SQL_Token* tokens, U64 token_count, String8 source_text);

internal String8 sql_node_type_to_string(SQL_NodeType type);

internal void sql_print_node(SQL_Node *node, U64 depth);
internal void sql_print_ast(SQL_Node *root);

#endif //SQL_PARSER_H
