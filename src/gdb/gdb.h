#ifndef GDB_H
#define GDB_H

#define GDB_FILE_FORMAT_VERSION_MAJOR 0
#define GDB_FILE_FORMAT_VERSION_MINOR 1

#ifndef GDB_STATE_ARENA_RESERVE_SIZE
#define GDB_STATE_ARENA_RESERVE_SIZE GB(2)
#endif
#ifndef GDB_STATE_ARENA_COMMIT_SIZE
#define GDB_STATE_ARENA_COMMIT_SIZE MB(32)
#endif

#ifndef GDB_DATABASE_ARENA_RESERVE_SIZE
#define GDB_DATABASE_ARENA_RESERVE_SIZE KB(64)
#endif
#ifndef GDB_DATABASE_ARENA_COMMIT_SIZE
#define GDB_DATABASE_ARENA_COMMIT_SIZE KB(4)
#endif

#ifndef GDB_TABLE_ARENA_RESERVE_SIZE
#define GDB_TABLE_ARENA_RESERVE_SIZE MB(1)
#endif
#ifndef GDB_TABLE_ARENA_COMMIT_SIZE
#define GDB_TABLE_ARENA_COMMIT_SIZE KB(32)
#endif
#ifndef GDB_TABLE_EXPAND_FACTOR
#define GDB_TABLE_EXPAND_FACTOR 2.0f
#endif

#ifndef GDB_COLUMN_EXPAND_COUNT
#define GDB_COLUMN_EXPAND_COUNT 64
#endif
#ifndef GDB_COLUMN_ARENA_RESERVE_SIZE
#define GDB_COLUMN_ARENA_RESERVE_SIZE MB(256)
#endif
#ifndef GDB_COLUMN_ARENA_COMMIT_SIZE
#define GDB_COLUMN_ARENA_COMMIT_SIZE MB(32)
#endif
#ifndef GDB_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE
#define GDB_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE KB(4)
#endif
#ifndef GDB_COLUMN_MAX_GROW_BY_SIZE
#define GDB_COLUMN_MAX_GROW_BY_SIZE MB(32)
#endif

#ifndef GDB_DISK_BACKED_THRESHOLD_SIZE
#define GDB_DISK_BACKED_THRESHOLD_SIZE MB(64)
#endif

global U64 g_gdb_disk_backed_threshold_size = GDB_DISK_BACKED_THRESHOLD_SIZE;
global U64 g_gdb_column_expand_count = GDB_COLUMN_EXPAND_COUNT;
global U64 g_gdb_column_variable_capacity_alloc_size = GDB_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE;
global U64 g_gdb_column_max_grow_by_size = GDB_COLUMN_MAX_GROW_BY_SIZE;
global F64 g_gdb_table_expand_factor = GDB_TABLE_EXPAND_FACTOR;

// tec: reserved name for the column catalog table
#define GDB_COLUMN_CATALOG_TABLE_NAME str8_lit("column_catalog")

//~ tec: column information 
typedef U32 GDB_ColumnType;
enum
{
  GDB_ColumnType_Invalid,
  GDB_ColumnType_U32,
  GDB_ColumnType_U64,
  GDB_ColumnType_F32,
  GDB_ColumnType_F64,
  GDB_ColumnType_String8,
  GDB_ColumnType_Bool,
  GDB_ColumnType_I32,
  GDB_ColumnType_I64,
  GDB_ColumnType_Date,
  GDB_ColumnType_Timestamp,
  GDB_ColumnType_Decimal,
  GDB_ColumnType_Enum,
  GDB_ColumnType_COUNT
};

global U64 g_gdb_column_type_size[GDB_ColumnType_COUNT] =
{
  0,
  sizeof(U32),
  sizeof(U64),
  sizeof(F32),
  sizeof(F64),
  sizeof(String8),
  sizeof(U8),
  sizeof(S32),
  sizeof(S64),
  sizeof(S32),
  sizeof(S64),
  sizeof(S64),
  sizeof(U32),
};

typedef struct GDB_ColumnSchema GDB_ColumnSchema;
struct GDB_ColumnSchema
{
  String8 name;
  GDB_ColumnType type;
  U64 size;
};

typedef struct GDB_StringDataChunk GDB_StringDataChunk;
struct GDB_StringDataChunk
{
  void* data;
  U64* offsets;
  U64 size;
  U64 row_count;
};

typedef struct GDB_EnumType GDB_EnumType;
struct GDB_EnumType
{
  String8 name;
  String8* value_labels;
  U32 value_count;
};

//~ tec: declare db structs
typedef struct GDB_Column GDB_Column;
typedef struct GDB_Table GDB_Table;
typedef struct GDB_Database GDB_Database;

typedef struct IR_Node IR_Node;

struct GDB_Column
{
  Arena* arena;
  
  String8 name;
  GDB_ColumnType type;
  U64 size;
  U64 capacity;
  U64 variable_capacity;
  U64 row_count;
  
  //- tec: for decimal columns
  U32 decimal_precision;
  U32 decimal_scale;
  
  //- tec: for enum cols
  GDB_EnumType* enum_type;
  
  //- tec: data storage
  U8 *data;
  U64 *offsets;
  
  //- tec: NULL tracking
  U8* null_flags;
  U64 null_flags_capacity;
  
  //- tec: constraints, single column only
  B32 not_null;        // tec: NOT NULL or PRIMARY KEY
  B32 is_unique;       // tec: UNIQUE or PRIMARY KEY
  B32 is_primary_key;  // tec: implies not_null && is_unique
  
  B32 has_foreign_key;
  String8 fk_ref_table_name;
  String8 fk_ref_column_name;
  
  // tec: CHECK(...), check_text is saved and reparsed into check_expr on table load
  B32 has_check;
  String8 check_text;
  IR_Node* check_expr;
  
  //- tec: io
  B32 is_disk_backed;
  B32 disk_backed_offset_initialized;
  String8 disk_path;
  OS_Handle file;
  OS_Handle file_map;
  // tec: kept open only for as long as file_map is cached, independent of `file`
  OS_Handle file_map_backing_file; 
  U64 mapped_size;
  void* mapped_ptr;
  Rng1U64 current_mapped_range;
  U64 write_generation;
  U64 mapped_generation;
  
  GDB_Table* parent_table;
};

typedef struct GDB_Index GDB_Index;
struct GDB_Index
{
  String8 name;
  String8 column_name;
  GDB_Column* column;
  
  U64* order;
  U64 order_count;
  U64 order_capacity;
};

struct GDB_Table
{
  Arena* arena;
  
  String8 name;
  U64 column_count;
  U64 column_capacity;
  U64 row_count;
  GDB_Column** columns;
  
  U64 index_count;
  U64 index_capacity;
  GDB_Index** indexes;
  
  GDB_Database* parent_database;
};

struct GDB_Database
{
  Arena* arena;
  
  String8 name;
  U64 table_count;
  U64 table_capacity;
  GDB_Table** tables;
  
  U64 enum_type_count;
  U64 enum_type_capacity;
  GDB_EnumType** enum_types;
};

//~ tec: csv loading
typedef struct GDB_CSV_ParsedField GDB_CSV_ParsedField;
struct GDB_CSV_ParsedField
{
  B32 present;
  B32 is_null;
  String8 str_value; 
  U64 numeric_bits;
};

typedef struct GDB_CSV_ParseTask GDB_CSV_ParseTask;
struct GDB_CSV_ParseTask
{
  Rng1U64* ranges;
  String8* lines;
  GDB_CSV_ParsedField* parsed;
  GDB_Table* table;
  U64 column_count;
};

//~ tec: state
typedef struct GDB_State GDB_State;
struct GDB_State
{
  Arena* arena;
  
  GDB_Database** databases;
  U64 database_count;
  U64 database_capacity;
  
  OS_Handle rw_mutex;
};

global GDB_State* g_gdb_state = 0;

internal void gdb_init(void);
internal void gdb_add_database(GDB_Database* database);
internal GDB_Database* gdb_state_find_database_by_name(String8 name);

//~ tec: databases

internal GDB_Database* gdb_database_alloc(String8 name);
internal void gdb_database_release(GDB_Database* database);
internal void gdb_database_add_table(GDB_Database* database, GDB_Table* table);
internal void gdb_database_replace_table(GDB_Database* database, GDB_Table* new_table);
internal B32 gdb_database_save(GDB_Database* database, String8 directory);
internal GDB_Database* gdb_database_load(String8 directory);
internal void gdb_database_close(GDB_Database* database);
internal GDB_Table* gdb_database_find_table(GDB_Database* database, String8 table_name);
internal GDB_Table* gdb_database_build_column_catalog(GDB_Database* database);
internal GDB_Table* gdb_database_find_table_or_catalog(GDB_Database* database, String8 name);

//~ tec: enum types
internal void gdb_database_add_enum_type(GDB_Database* database, GDB_EnumType* enum_type);
internal GDB_EnumType* gdb_database_find_enum_type(GDB_Database* database, String8 name);
internal B32 gdb_enum_type_code_from_label(GDB_EnumType* enum_type, String8 label, U32* out_code);
internal String8 gdb_enum_type_label_from_code(GDB_EnumType* enum_type, U32 code);

//~ tec: tables
internal GDB_Table* gdb_table_alloc(String8 name);
internal void gdb_table_release(GDB_Table* table);
internal void gdb_table_add_column(GDB_Table* table, GDB_ColumnSchema schema);
internal void gdb_table_remove_column(GDB_Table* table, GDB_Column* column);
internal void gdb_table_add_row(GDB_Table* table, void** row_data, B32* null_flags);
internal void gdb_table_remove_row(GDB_Table* table, U64 row_index);
internal B32 gdb_table_may_have_nulls(GDB_Table* table);
internal B32 gdb_table_save(GDB_Table* table, String8 table_dir);
internal B32 gdb_table_export_csv(GDB_Table* table, String8 path);
internal GDB_Table* gdb_table_load(GDB_Database* database, String8 table_dir, String8 meta_path);
internal GDB_Table* gdb_table_import_csv(GDB_Database* database, String8 path);
internal GDB_Table* gdb_table_import_csv_streaming(GDB_Database *db, String8 table_name, String8 path);
internal GDB_Column* gdb_table_find_column(GDB_Table* table, String8 column_name);

//~ tec: indexes
internal GDB_Index* gdb_table_create_index(GDB_Table* table, String8 index_name, GDB_Column* column);
internal GDB_Index* gdb_table_register_index(GDB_Table* table, String8 index_name, GDB_Column* column);
internal void gdb_table_drop_index(GDB_Table* table, String8 index_name);
internal GDB_Index* gdb_table_find_index(GDB_Table* table, String8 index_name);
internal GDB_Index* gdb_table_find_index_on_column(GDB_Table* table, GDB_Column* column);

internal void gdb_index_build_order(GDB_Index* index);
internal void gdb_index_insert_row(GDB_Index* index, U64 new_row_index);
internal void gdb_index_remove_row(GDB_Index* index, U64 removed_row_index);
internal void gdb_index_save(GDB_Index* index, String8 table_dir);
internal B32 gdb_index_load(GDB_Index* index, String8 table_dir);
internal String8 gdb_generate_disk_path_for_index(Arena* arena, GDB_Table* table, String8 index_name);

//~ tec: constraints
internal B32 gdb_column_has_any_constraint(GDB_Column* column);

//~ tec: columns
internal GDB_Column* gdb_column_alloc(String8 name, GDB_ColumnType type, U64 size);
internal void gdb_column_release(GDB_Column* column);
internal void gdb_column_close(GDB_Column* column);

internal String8 gdb_column_get_string(Arena* arena, GDB_Column* column, U64 index);
internal U64 gdb_column_get_total_size(GDB_Column* column);

internal void gdb_column_add_data_disk_backed(GDB_Column* column, void* data);
internal void gdb_column_materialize_to_memory(GDB_Column* column);
internal void gdb_column_add_data(GDB_Column* column, void* data);
internal void gdb_column_add_data_maybe_null(GDB_Column* column, void* data, B32 is_null);
internal void* gdb_column_get_data(GDB_Column* column, U64 index);
internal void gdb_column_remove_data(GDB_Column* column, U64 row_index);
internal B32 gdb_column_is_null(GDB_Column* column, U64 row_index);
internal void* gdb_column_get_data_range(Arena* arena, GDB_Column* column, Rng1U64 row_range, U64* out_size);
internal GDB_StringDataChunk gdb_column_get_string_chunk(Arena* arena, GDB_Column* column, Rng1U64 row_range);

internal String8 gdb_generate_disk_path_for_column(Arena* arena, GDB_Column* column);
internal void gdb_column_convert_to_disk_backed(GDB_Column* column);

//~ tec: utils
internal GDB_ColumnType gdb_column_type_from_string(GDB_Database* database, String8 str);
internal String8 string_from_gdb_column_type(GDB_ColumnType type);
internal GDB_ColumnSchema gdb_column_schema_create(String8 name, GDB_ColumnType type);
internal GDB_ColumnType gdb_infer_column_type(String8 value);
internal GDB_ColumnType gdb_promote_type(GDB_ColumnType existing, GDB_ColumnType new_type);
internal String8 gdb_column_type_display_name(Arena* arena, GDB_Column* column);

internal B32 decimal_from_str8(String8 str, U32 scale, S64* out_raw);
internal String8 decimal_to_str8(Arena* arena, S64 raw, U32 scale);

#endif //GDB_H
