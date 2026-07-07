/* date = January 25th 2025 11:47 am */

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
#define GDB_DISK_BACKED_THRESHOLD_SIZE KB(4)
#endif

typedef U32 GDB_ColumnType;
enum
{
  GDB_ColumnType_Invalid,
  GDB_ColumnType_U32,
  GDB_ColumnType_U64,
  GDB_ColumnType_F32,
  GDB_ColumnType_F64,
  GDB_ColumnType_String8,
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

typedef struct GDB_Column GDB_Column;
struct GDB_Column
{
  Arena* arena;
  
  String8 name;
  GDB_ColumnType type;
  U64 size;
  U64 capacity;
  U64 variable_capacity;
  U64 row_count; 
  
  // tec: data storage
  U8 *data;
  U64 *offsets;
  
  // tec: disk backed
  B32 is_disk_backed;
  B32 disk_backed_offset_initialized;
  String8 disk_path;
  OS_Handle file;
  OS_Handle file_map;
  U64 mapped_size;
  void* mapped_ptr;
  Rng1U64 current_mapped_range;
  
  struct GDB_Table* parent_table;
};

typedef struct GDB_Table GDB_Table;
struct GDB_Table
{
  Arena* arena;
  
  String8 name;
  U64 column_count;
  U64 column_capacity;
  U64 row_count;
  GDB_Column** columns;
  
  struct GDB_Database* parent_database;
};

// tec: multithreading csv
typedef struct GDB_CSV_ThreadColumnData GDB_CSV_ThreadColumnData;
struct GDB_CSV_ThreadColumnData
{
  void** values;
  U64 count;
  GDB_ColumnType type;
};

typedef struct GDB_CSV_ThreadContext GDB_CSV_ThreadContext;
struct GDB_CSV_ThreadContext
{
  GDB_Table* table;
  Rng1U64 range;
  OS_Handle map;
  OS_Handle mutex;
  U64 offset_within_view;
  GDB_CSV_ThreadColumnData* columns;
  Arena* arena;
  U64 starting_row_index;
  U64 view_size;
};

typedef struct GDB_Database GDB_Database;
struct GDB_Database
{
  Arena* arena;
  
  String8 name;
  U64 table_count;
  U64 table_capacity;
  GDB_Table** tables;
};

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

//~ tec: databases

internal GDB_Database* gdb_database_alloc(String8 name);
internal void gdb_database_release(GDB_Database* database);
internal void gdb_database_add_table(GDB_Database* database, GDB_Table* table);
internal B32 gdb_database_save(GDB_Database* database, String8 directory);
internal GDB_Database* gdb_database_load(String8 directory);
internal void gdb_database_close(GDB_Database* database);
internal GDB_Table* gdb_database_find_table(GDB_Database* database, String8 table_name);

//~ tec: tables
internal GDB_Table* gdb_table_alloc(String8 name);
internal void gdb_table_release(GDB_Table* table);
internal void gdb_table_add_column(GDB_Table* table, GDB_ColumnSchema schema);
internal void gdb_table_add_row(GDB_Table* table, void** row_data);
internal void gdb_table_remove_row(GDB_Table* table, U64 row_index);
internal B32 gdb_table_save(GDB_Table* table, String8 table_dir);
internal B32 gdb_table_export_csv(GDB_Table* table, String8 path);
internal GDB_Table* gdb_table_load(String8 table_dir, String8 meta_path);
internal GDB_Table* gdb_table_import_csv(GDB_Database* database, String8 path);
internal GDB_Table* gdb_table_import_csv_streaming(GDB_Database *db, String8 table_name, String8 path);
internal GDB_Column* gdb_table_find_column(GDB_Table* table, String8 column_name);

//~ tec: columns
internal GDB_Column* gdb_column_alloc(String8 name, GDB_ColumnType type, U64 size);
internal void gdb_column_release(GDB_Column* column);
internal void gdb_column_close(GDB_Column* column);

internal String8 gdb_column_get_string(Arena* arena, GDB_Column* column, U64 index);
internal U64 gdb_column_get_total_size(GDB_Column* column);

internal void gdb_column_add_data_disk_backed(GDB_Column* column, void* data);
internal void gdb_column_add_data(GDB_Column* column, void* data);
internal void* gdb_column_get_data(GDB_Column* column, U64 index);
internal void gdb_column_remove_data(GDB_Column* column, U64 row_index);
internal void* gdb_column_get_data_range(Arena* arena, GDB_Column* column, Rng1U64 row_range, U64* out_size);
internal GDB_StringDataChunk gdb_column_get_string_chunk(Arena* arena, GDB_Column* column, Rng1U64 row_range);

internal String8 gdb_generate_disk_path_for_column(Arena* arena, GDB_Column* column);
internal void gdb_column_convert_to_disk_backed(GDB_Column* column);

//~ tec: utils
internal GDB_ColumnType gdb_column_type_from_string(String8 str);
internal String8 string_from_gdb_column_type(GDB_ColumnType type);
internal GDB_ColumnSchema gdb_column_schema_create(String8 name, GDB_ColumnType type);
internal GDB_ColumnType gdb_infer_column_type(String8 value);
internal GDB_ColumnType gdb_promote_type(GDB_ColumnType existing, GDB_ColumnType new_type);

#endif //GDB_H
