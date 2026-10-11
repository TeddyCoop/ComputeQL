#ifndef CQL_H
#define CQL_H

#define CQL_FILE_FORMAT_VERSION_MAJOR 0
#define CQL_FILE_FORMAT_VERSION_MINOR 1

#ifndef CQL_STATE_ARENA_RESERVE_SIZE
#define CQL_STATE_ARENA_RESERVE_SIZE GB(2)
#endif
#ifndef CQL_STATE_ARENA_COMMIT_SIZE
#define CQL_STATE_ARENA_COMMIT_SIZE MB(32)
#endif

#ifndef CQL_DATABASE_ARENA_RESERVE_SIZE
#define CQL_DATABASE_ARENA_RESERVE_SIZE KB(64)
#endif
#ifndef CQL_DATABASE_ARENA_COMMIT_SIZE
#define CQL_DATABASE_ARENA_COMMIT_SIZE KB(4)
#endif

#ifndef CQL_TABLE_ARENA_RESERVE_SIZE
#define CQL_TABLE_ARENA_RESERVE_SIZE MB(1)
#endif
#ifndef CQL_TABLE_ARENA_COMMIT_SIZE
#define CQL_TABLE_ARENA_COMMIT_SIZE KB(32)
#endif
#ifndef CQL_TABLE_EXPAND_FACTOR
#define CQL_TABLE_EXPAND_FACTOR 2.0f
#endif

#ifndef CQL_COLUMN_EXPAND_COUNT
#define CQL_COLUMN_EXPAND_COUNT 64
#endif
#ifndef CQL_COLUMN_ARENA_RESERVE_SIZE
#define CQL_COLUMN_ARENA_RESERVE_SIZE MB(256)
#endif
#ifndef CQL_COLUMN_ARENA_COMMIT_SIZE
#define CQL_COLUMN_ARENA_COMMIT_SIZE MB(32)
#endif
#ifndef CQL_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE
#define CQL_COLUMN_VARIABLE_CAPACITY_ALLOC_SIZE KB(4)
#endif
#ifndef CQL_COLUMN_MAX_GROW_BY_SIZE
#define CQL_COLUMN_MAX_GROW_BY_SIZE MB(32)
#endif

// tec: appends to a disk backed column collect in memory and reach the file in one write per this many bytes or rows
#ifndef CQL_PENDING_APPEND_BYTES
#define CQL_PENDING_APPEND_BYTES MB(1)
#endif
#ifndef CQL_PENDING_APPEND_ROWS
#define CQL_PENDING_APPEND_ROWS 65536
#endif

#ifndef CQL_DISK_BACKED_THRESHOLD_SIZE
#define CQL_DISK_BACKED_THRESHOLD_SIZE MB(64)
#endif

#ifndef CQL_DICT_ENCODE_MIN_ROWS
#define CQL_DICT_ENCODE_MIN_ROWS 4096
#endif
#ifndef CQL_DICT_ENCODE_MAX_DISTINCT
#define CQL_DICT_ENCODE_MAX_DISTINCT 65536
#endif
#ifndef CQL_DICT_ENCODE_MAX_CARDINALITY_RATIO
#define CQL_DICT_ENCODE_MAX_CARDINALITY_RATIO 0.5
#endif

// tec: must be a power of two
#ifndef CQL_ZONEMAP_CHUNK_ROWS
#define CQL_ZONEMAP_CHUNK_ROWS 8192
#endif

// tec: reserved name for the column catalog table
#define CQL_COLUMN_CATALOG_TABLE_NAME str8_lit("column_catalog")

//~ tec: column information 
typedef U32 CQL_ColumnType;
enum
{
  CQL_ColumnType_Invalid,
  CQL_ColumnType_U32,
  CQL_ColumnType_U64,
  CQL_ColumnType_F32,
  CQL_ColumnType_F64,
  CQL_ColumnType_String8,
  CQL_ColumnType_Bool,
  CQL_ColumnType_I32,
  CQL_ColumnType_I64,
  CQL_ColumnType_Date,
  CQL_ColumnType_Timestamp,
  CQL_ColumnType_Decimal,
  CQL_ColumnType_Enum,
  CQL_ColumnType_COUNT
};

global U64 g_cql_column_type_size[CQL_ColumnType_COUNT] =
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

typedef struct CQL_ColumnSchema CQL_ColumnSchema;
struct CQL_ColumnSchema
{
  String8 name;
  CQL_ColumnType type;
  U64 size;
};

typedef struct CQL_StringDataChunk CQL_StringDataChunk;
struct CQL_StringDataChunk
{
  void* data;
  U64* offsets;
  U64 size;
  U64 row_count;
};

typedef struct CQL_EnumType CQL_EnumType;
struct CQL_EnumType
{
  String8 name;
  String8* value_labels;
  U32 value_count;
};

//~ tec: automatic dictionary encoding for low cardinality String8 columns
#define CQL_DICT_EMPTY_SLOT ((U32)0xFFFFFFFF)
#define CQL_DICT_NOT_FOUND  ((U32)0xFFFFFFFF)
#define CQL_DICT_MAX_PROBE  256
#define CQL_DICT_TABLE_CAPACITY_FACTOR 4

typedef struct CQL_StringDict CQL_StringDict;
struct CQL_StringDict
{
  Arena* arena;
  String8* values;
  U32 value_count;
  U32* index_codes;
  U64 index_capacity;
};

//~ tec: zone maps
typedef struct CQL_ZoneMapChunk CQL_ZoneMapChunk;
struct CQL_ZoneMapChunk
{
  F64 min;
  F64 max;
  B32 has_values;
};

//~ tec: column statistics
#define CQL_STATS_HISTOGRAM_BUCKETS 64
#define CQL_STATS_MCV_COUNT 8
#define CQL_STATS_SAMPLE_ROWS 65536
#define CQL_STATS_HLL_PRECISION 14
#define CQL_STATS_HLL_REGISTER_COUNT (1 << CQL_STATS_HLL_PRECISION)

typedef struct CQL_ColumnMcvEntry CQL_ColumnMcvEntry;
struct CQL_ColumnMcvEntry
{
  U64 key;
  F64 fraction; // tec: of non-null rows
};

typedef struct CQL_ColumnStats CQL_ColumnStats;
struct CQL_ColumnStats
{
  B32 is_computed;
  U64 computed_generation;
  U64 row_count;
  U64 null_count;
  U64 distinct_count;
  
  B32 has_range;
  F64 min_value;
  F64 max_value;
  
  // tec: every bucket will hold an equal share of non null rows
  U32 histogram_bucket_count;
  F64 histogram_bounds[CQL_STATS_HISTOGRAM_BUCKETS + 1];
  
  U32 mcv_count;
  CQL_ColumnMcvEntry mcv[CQL_STATS_MCV_COUNT];
};

//~ tec: declare db structs
typedef struct CQL_Column CQL_Column;
typedef struct CQL_Table CQL_Table;
typedef struct CQL_Database CQL_Database;

typedef struct IR_Node IR_Node;

struct CQL_Column
{
  Arena* arena;

  // tec: never reused, unlike the column's address, so a cache keyed by it can not mistake a new column for a freed one
  U64 uid;
  
  String8 name;
  CQL_ColumnType type;
  U64 size;
  U64 capacity;
  U64 variable_capacity;
  U64 row_count;
  
  //- tec: for decimal columns
  U32 decimal_precision;
  U32 decimal_scale;
  
  //- tec: for enum cols
  CQL_EnumType* enum_type;
  
  //- tec: dictionary encoding
  B32 has_dict;
  U64 dict_checked_generation;
  CQL_StringDict* dict;
  U32* dict_codes;
  
  //- tec: zone maps
  B32 has_zone_map;
  U64 zonemap_checked_generation;
  CQL_ZoneMapChunk* zone_map;
  U64 zone_map_chunk_count;
  U64 zone_map_capacity;
  
  //- tec: optimizer statistics, lazily computed
  CQL_ColumnStats stats;
  
  U64 gpu_upload_generation;
  U64 gpu_upload_data_size;
  
  //- tec: entire col gpu string data/offsets (non dict String8 columns only)
  U64 gpu_str_upload_generation;
  U64 gpu_str_upload_data_size;
  U64 gpu_str_upload_offsets_size;
  
  //- tec: dense device copies kept for qe_aggregate
  U64 agg_f64_generation;
  U64 agg_f32_generation;
  U64 agg_narrow_generation;
  B32 agg_narrow;
  F64 agg_int_bound;
  U64 agg_fixed_generation;
  S32 agg_fixed_scale;
  F64 agg_fixed_max;
  U64 agg_key_generation;
  B32 agg_key_whole;
  S64 agg_key_min;
  S64 agg_key_max;
  void* agg_f64_buffer;
  void* agg_f32_buffer;
  
  // tec: entire col GROUP BY keys for no dict eligble String8
  Arena* agg_str_arena;
  U64 agg_str_generation;
  void* agg_str_data;
  U64 agg_str_data_size;
  U64* agg_str_offsets;
  
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

  // tec: rows appended to a disk backed column that the file does not hold yet, every reader flushes them first
  U8* pending_data;
  U64 pending_data_size;
  U64 pending_data_capacity;
  U64* pending_offsets;
  U64 pending_rows;
  U64 mapped_generation;
  
  CQL_Table* parent_table;
};

typedef struct CQL_Index CQL_Index;
struct CQL_Index
{
  String8 name;
  String8 column_name;
  CQL_Column* column;
  
  U64* order;
  U64 order_count;
  U64 order_capacity;
};

struct CQL_Table
{
  Arena* arena;
  
  String8 name;
  U64 column_count;
  U64 column_capacity;
  U64 row_count;
  CQL_Column** columns;
  
  U64 index_count;
  U64 index_capacity;
  CQL_Index** indexes;
  
  CQL_Database* parent_database;
  
  // tec: query scoped tables (cte, temps, etc)
  B32 is_ephemeral;
};

struct CQL_Database
{
  Arena* arena;
  
  String8 name;
  U64 table_count;
  U64 table_capacity;
  CQL_Table** tables;
  
  U64 enum_type_count;
  U64 enum_type_capacity;
  CQL_EnumType** enum_types;
  
  U64 temp_table_count;
  U64 temp_table_capacity;
  CQL_Table** temp_tables;
};

//~ tec: csv loading
typedef struct CQL_CSV_ParsedField CQL_CSV_ParsedField;
struct CQL_CSV_ParsedField
{
  B32 present;
  B32 is_null;
  String8 str_value; 
  U64 numeric_bits;
};

typedef struct CQL_CSV_ParseTask CQL_CSV_ParseTask;
struct CQL_CSV_ParseTask
{
  Rng1U64* ranges;
  String8* lines;
  CQL_CSV_ParsedField* parsed;
  CQL_Table* table;
  U64 column_count;
};

internal U64 parse_csv_line(U8 *input, U64 len, String8 *fields, U64 max_fields);
internal THREAD_POOL_TASK_FUNC(cql_csv_parse_task);
internal void cql_csv_append_parsed_row(CQL_Table* table, CQL_CSV_ParsedField* row_fields, U64 column_count);

//~ tec: state
typedef struct CQL_State CQL_State;
struct CQL_State
{
  Arena* arena;
  
  CQL_Database** databases;
  U64 database_count;
  U64 database_capacity;
  
  OS_Handle rw_mutex;
  
  //- tec: settings derived tunables
  U64 disk_backed_threshold_size;
  U64 column_expand_count;
  U64 column_variable_capacity_alloc_size;
  U64 column_max_grow_by_size;
  F64 table_expand_factor;
  U64 dict_encode_min_rows;
  U64 dict_encode_max_distinct;
  F64 dict_encode_max_cardinality_ratio;
  U64 zonemap_chunk_rows;
  U64 zonemap_chunk_rows_log2; 
};

global CQL_State* g_cql_state = 0;

internal void cql_init(void);
internal void cql_add_database(CQL_Database* database);
internal CQL_Database* cql_state_find_database_by_name(String8 name);

internal void cql_release(void);

//~ tec: databases

internal CQL_Database* cql_database_alloc(String8 name);
internal void cql_database_release(CQL_Database* database);
internal void cql_database_add_table(CQL_Database* database, CQL_Table* table);
internal void cql_database_replace_table(CQL_Database* database, CQL_Table* new_table);
internal B32 cql_database_save(CQL_Database* database, String8 directory);
internal CQL_Database* cql_database_load(String8 directory);
internal void cql_database_close(CQL_Database* database);
internal CQL_Table* cql_database_find_table(CQL_Database* database, String8 table_name);
internal CQL_Table* cql_database_build_column_catalog(CQL_Database* database);
internal CQL_Table* cql_database_find_table_or_catalog(CQL_Database* database, String8 name);
internal void cql_database_add_temp_table(CQL_Database* database, CQL_Table* table);
internal void cql_database_release_temp_tables_from(CQL_Database* database, U64 first_temp_index);

global String8 g_cql_database_save_path = str8_lit_comp("cql_data/");

internal B32 cql_database_contains_table(CQL_Database* database, String8 table_name);

//~ tec: enum types
internal void cql_database_add_enum_type(CQL_Database* database, CQL_EnumType* enum_type);
internal CQL_EnumType* cql_database_find_enum_type(CQL_Database* database, String8 name);
internal B32 cql_enum_type_code_from_label(CQL_EnumType* enum_type, String8 label, U32* out_code);
internal String8 cql_enum_type_label_from_code(CQL_EnumType* enum_type, U32 code);

//~ tec: tables
internal CQL_Table* cql_table_alloc(String8 name);
internal void cql_table_release(CQL_Table* table);
internal void cql_table_add_column(CQL_Table* table, CQL_ColumnSchema schema);
internal void cql_table_remove_column(CQL_Table* table, CQL_Column* column);
internal void cql_table_add_row(CQL_Table* table, void** row_data, B32* null_flags);
internal void cql_table_remove_row(CQL_Table* table, U64 row_index);
internal B32 cql_table_may_have_nulls(CQL_Table* table);
internal B32 cql_table_save(CQL_Table* table, String8 table_dir);
internal B32 cql_table_export_csv(CQL_Table* table, String8 path);
internal CQL_Table* cql_table_load(CQL_Database* database, String8 table_dir, String8 meta_path);
internal CQL_Table* cql_table_import_csv(CQL_Database* database, String8 path);
internal CQL_Table* cql_table_import_csv_streaming(CQL_Database *db, String8 table_name, String8 path);
internal CQL_Column* cql_table_find_column(CQL_Table* table, String8 column_name);

//~ tec: indexes
internal CQL_Index* cql_table_create_index(CQL_Table* table, String8 index_name, CQL_Column* column);
internal CQL_Index* cql_table_register_index(CQL_Table* table, String8 index_name, CQL_Column* column);
internal void cql_table_drop_index(CQL_Table* table, String8 index_name);
internal CQL_Index* cql_table_find_index(CQL_Table* table, String8 index_name);
internal CQL_Index* cql_table_find_index_on_column(CQL_Table* table, CQL_Column* column);

internal String8 cql_generate_disk_path_for_index(Arena* arena, CQL_Table* table, String8 index_name);

//- tec: column index
typedef struct CQL_IndexBuildCtx CQL_IndexBuildCtx;
struct CQL_IndexBuildCtx
{
  B32 is_string;
  F64* numeric_keys; // tec: dense, indexed by row (0..row_count-1)
  CQL_StringDataChunk string_keys;
};

global CQL_IndexBuildCtx* g_cql_index_build_ctx = 0;

internal void cql_index_build_order(CQL_Index* index);
internal void cql_index_insert_row(CQL_Index* index, U64 new_row_index);
internal void cql_index_remove_row(CQL_Index* index, U64 removed_row_index);
internal void cql_index_save(CQL_Index* index, String8 table_dir);
internal B32 cql_index_load(CQL_Index* index, String8 table_dir);
internal S32 cql_str8_compare(String8 a, String8 b);
internal F64 cql_index_numeric_value(CQL_Column* column, U64 row_index);
internal int cql_index_build_compare(const void* a, const void* b);
internal void cql_index_ensure_order_capacity(CQL_Index* index, CQL_Table* table);

//~ tec: constraints
internal B32 cql_column_has_any_constraint(CQL_Column* column);

internal IR_Node* cql_parse_check_expression(Arena* arena, String8 check_text);

//~ tec: columns
internal CQL_Column* cql_column_alloc(String8 name, CQL_ColumnType type, U64 size);
internal void cql_column_release(CQL_Column* column);
internal void cql_column_close(CQL_Column* column);

internal String8 cql_column_get_string(Arena* arena, CQL_Column* column, U64 index);
internal U64 cql_column_get_total_size(CQL_Column* column);

internal void cql_column_add_data_disk_backed(CQL_Column* column, void* data);
internal void cql_column_flush_pending(CQL_Column* column);
internal void cql_column_drop_mapping(CQL_Column* column);
internal void cql_column_materialize_to_memory(CQL_Column* column);
internal void cql_column_add_data(CQL_Column* column, void* data);
internal void cql_column_add_data_maybe_null(CQL_Column* column, void* data, B32 is_null);
internal void* cql_column_alloc_fixed_rows(CQL_Column* column, U64 count);
internal B32 cql_column_fill_from_f64(CQL_Column* column, F64* values, U8* is_null, U64 count);
internal B32 cql_column_fill_from_strings(CQL_Column* column, String8* values, U8* is_null, U64 count);
internal void* cql_column_get_data(CQL_Column* column, U64 index);
internal void cql_column_remove_data(CQL_Column* column, U64 row_index);
internal B32 cql_column_is_null(CQL_Column* column, U64 row_index);
internal void* cql_column_get_data_range(Arena* arena, CQL_Column* column, Rng1U64 row_range, U64* out_size);
internal CQL_StringDataChunk cql_column_get_string_chunk(Arena* arena, CQL_Column* column, Rng1U64 row_range);

internal B32 cql_column_is_ephemeral(CQL_Column* column);
internal void cql_column_open(CQL_Column* column);
internal void cql_column_ensure_null_flags_capacity(CQL_Column* column, U64 needed_count);
internal void cql_column_close_string_chunk(CQL_Column* column);

//- tec: dictionary

typedef struct CQL_DictBuildCtx CQL_DictBuildCtx;
struct CQL_DictBuildCtx
{
  CQL_StringDataChunk chunk;
  Rng1U64* ranges;
  U32* owner_row;
  U64 capacity;
  U32 overflow_flag;
};

typedef struct CQL_DictFillCtx CQL_DictFillCtx;
struct CQL_DictFillCtx
{
  CQL_StringDataChunk chunk;
  Rng1U64* ranges;
  CQL_StringDict* dict;
  U32* dict_codes;
};

read_only global String8 g_cql_dict_empty_str8 = {0};

internal U32 cql_string_dict_code_or_sentinel(CQL_StringDict* dict, String8 value);
internal String8 cql_dict_row_string(CQL_StringDataChunk* chunk, U64 row);
internal void cql_column_ensure_string_dict(CQL_Column* column);
internal B32 cql_string_dict_code_from_value(CQL_StringDict* dict, String8 value, U32* out_code);

internal THREAD_POOL_TASK_FUNC(cql_dict_claim_task);
internal THREAD_POOL_TASK_FUNC(cql_dict_fill_codes_task);

//- tec: zone maps
typedef struct CQL_ZoneMapBuildCtx CQL_ZoneMapBuildCtx;
struct CQL_ZoneMapBuildCtx
{
  CQL_Column* column;
  CQL_ZoneMapChunk* zone_map;
  
  // tec: in CHUNK index units, not row units
  Rng1U64* chunk_ranges;
  
  U64 chunk_rows;
  void* base_ptr;
};

internal F64 cql_numeric_value_as_f64(CQL_ColumnType type, void* data);
internal B32 cql_column_type_is_zone_map_eligible(CQL_ColumnType type);
internal void cql_column_ensure_zone_map(CQL_Column* column);

internal String8 cql_generate_disk_path_for_column(Arena* arena, CQL_Column* column);
internal void cql_column_convert_to_disk_backed(CQL_Column* column);

internal void cql_column_zone_map_grow(CQL_Column* column, U64 needed_count);
internal void cql_column_zone_map_note_append(CQL_Column* column, void* data);
internal THREAD_POOL_TASK_FUNC(cql_zone_map_build_task);

//~ tec: utils
internal CQL_ColumnType cql_column_type_from_string(CQL_Database* database, String8 str);
internal String8 string_from_cql_column_type(CQL_ColumnType type);
internal CQL_ColumnSchema cql_column_schema_create(String8 name, CQL_ColumnType type);
internal CQL_ColumnType cql_infer_column_type(String8 value);
internal CQL_ColumnType cql_promote_type(CQL_ColumnType existing, CQL_ColumnType new_type);
internal String8 cql_column_type_display_name(Arena* arena, CQL_Column* column);

internal B32 decimal_from_str8(String8 str, U32 scale, S64* out_raw);
internal String8 decimal_to_str8(Arena* arena, S64 raw, U32 scale);

#endif //CQL_H
