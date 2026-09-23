#ifndef QUERY_EXEC_H
#define QUERY_EXEC_H

//~ tec: fixed descriptor binding layout shared with scan_filter.comp
//
//  binding 0: bytecode buffer            (uint[])
//  binding 1: numeric constant pool      (uint[], 2 words per constant: lo, hi)
//  binding 2: string constant pool       (uint[], packed bytes, 4 per word)
//  binding 3: output row indices         (uint[], 2 words per index: lo, hi)
//  binding 4: output match count         (uint[1], atomicAdd on word 0)
//  binding 5: reserved
//  binding 6..15: dynamically bound column buffers (up to QE_MAX_COLUMN_BINDINGS slots)
//
//  push constant U64 slot 0: row_count
//  push constant U64 slot 1: fuzzy-search score flags (bit0 has_aux_score, bit1 score_is_distance)
//  push constant U64 slot 2: trigram N (gram length)

#define QE_BINDING_BYTECODE     0
#define QE_BINDING_NUM_CONSTS   1
#define QE_BINDING_STR_CONSTS   2
#define QE_BINDING_OUT_INDICES  3
#define QE_BINDING_OUT_COUNT    4
#define QE_BINDING_COLUMN_BASE  6

#define QE_MAX_COLUMN_BINDINGS  10

// tec: must match every kernel .comp's `layout(local_size_x = ...)`
#define QE_GPU_WORKGROUP_SIZE 256

#define QE_PUSH_CONSTANT_ROW_COUNT  0
// tec: bit0 = has_aux_scorebit1 = score_is_distance (0 = similarity float score, 1 = edit distance)
#define QE_PUSH_CONSTANT_SCORE_FLAGS 1
#define QE_PUSH_CONSTANT_TRIGRAM_N   2

// tec: must match scan_filter.comp's MAX_STACK exactly. settings configurable,
#define QE_SCAN_MAX_STACK 8

// tec: must match GDB_ColumnType's enum and scan_filter.comp's COLTYPE_* defines exactly
#define COLTYPE_BOOL      6
#define COLTYPE_I32       7
#define COLTYPE_I64       8
#define COLTYPE_DATE      9
#define COLTYPE_TIMESTAMP 10
#define COLTYPE_DECIMAL   11
#define COLTYPE_ENUM      12

internal B32 qe_resolve_date_literal_value(GDB_ColumnType col_type, IR_Node* literal_node, F64* out_value);
internal B32 qe_resolve_decimal_literal_value(GDB_Column* column, IR_Node* literal_node, F64* out_value);
internal B32 qe_resolve_enum_literal_value(GDB_Column* column, IR_Node* literal_node, F64* out_value);

typedef enum QE_Opcode
{
  QE_Opcode_PushTrue    = 0,
  QE_Opcode_LoadNumCol  = 1,
  QE_Opcode_PushConst   = 2,
  QE_Opcode_CmpEq       = 3,
  QE_Opcode_CmpNe       = 4,
  QE_Opcode_CmpLt       = 5,
  QE_Opcode_CmpGt       = 6,
  QE_Opcode_CmpLe       = 7,
  QE_Opcode_CmpGe       = 8,
  QE_Opcode_And         = 9,
  QE_Opcode_Or          = 10,
  QE_Opcode_StrEq       = 11,
  QE_Opcode_StrContains = 12,
  QE_Opcode_Halt        = 13,
  QE_Opcode_PushFalse   = 14,
  QE_Opcode_TrigramSim  = 15,
  QE_Opcode_EditDistance = 16,
  QE_Opcode_StrNe       = 17,
  QE_Opcode_StrLt       = 18,
  QE_Opcode_StrGt       = 19,
  QE_Opcode_StrLe       = 20,
  QE_Opcode_StrGe       = 21,
} QE_Opcode;

typedef struct QE_ColumnBinding QE_ColumnBinding;
struct QE_ColumnBinding
{
  String8 name;
  GDB_Column* column;
  GDB_ColumnType type;
  U32 first_slot; // tec: relative slot (0..QE_MAX_COLUMN_BINDINGS-1), maps to descriptor binding QE_BINDING_COLUMN_BASE+first_slot
  U32 slot_count; // tec: 1 for numeric columns, 2 (data+offsets) for string columns
  B32 use_dict_codes;
};

typedef struct QE_BytecodeProgram QE_BytecodeProgram;
struct QE_BytecodeProgram
{
  Arena* arena;
  
  U32* words;
  U64 word_count;
  U64 words_cap;
  
  U32* consts;
  U64 const_count;
  U64 consts_cap;
  
  U8* str_const_pool;
  U64 str_const_pool_size;
  U64 str_const_pool_cap;
  
  QE_ColumnBinding bindings[QE_MAX_COLUMN_BINDINGS];
  U32 binding_count;
  U32 next_slot;
  
  //- tec: used when a SIMILARITY()/EDIT_DISTANCE() call is compiled
  B32 has_score_output;
  B32 score_is_distance; // tec: 0 = similarity (float score), 1 = edit distance (integer score)
  String8 score_column_name;
  String8 score_needle;
  
  B32 requires_cpu_scan;
  String8 cpu_scan_reason;
};

typedef struct QE_StringConstRef QE_StringConstRef;
struct QE_StringConstRef
{
  U32 word_offset;
  U32 byte_len;
};

//~ tec: EXPLAIN ANALYZE trace/stats
typedef enum QE_TraceStrategy
{
  QE_TraceStrategy_None,
  QE_TraceStrategy_IndexScan,
  QE_TraceStrategy_CpuScan,
  QE_TraceStrategy_GpuScan,
  QE_TraceStrategy_Identity,
} QE_TraceStrategy;

typedef struct QE_ScanTrace QE_ScanTrace;
struct QE_ScanTrace
{
  QE_TraceStrategy strategy;
  String8 strategy_reason;
  
  U64 rows_before, rows_after;
  U64 zonemap_pruned_rows, zonemap_chunk_count;
  String8 zonemap_column_name; 
  
  // dict_hit=0 with dict_decision_made=1 means "literal not in dict, 0 rows scanned"
  B32 dict_decision_made;
  B32 dict_hit; 
  U64 dict_size;
  
  U64 gpu_cache_hit_bytes;
  U64 gpu_cache_hit_count;
  U64 gpu_cache_miss_bytes;
  U64 gpu_cache_miss_count;
  
  U64 gpu_kernel_time_us;
  U64 submit_wait_time_us;
  U64 load_from_disk_time_us;
  U64 buffer_alloc_time_us;
  U64 prefetch_stall_time_us;
  U64 chunk_count;
};

typedef struct QE_JoinTrace QE_JoinTrace;
struct QE_JoinTrace
{
  U64 build_row_count;
  U64 probe_row_count;
  U64 output_row_count;
  U64 build_time_us;
  U64 probe_dispatch_time_us;
  U64 probe_download_time_us;
  U32 probe_passes;
  U64 output_capacity;
};

typedef struct QE_AggregateTrace QE_AggregateTrace;
struct QE_AggregateTrace
{
  U64 input_row_count;
  U64 group_count;
  U64 gather_time_us;
  U64 assign_time_us;
  U64 reduce_time_us;
  U64 combine_time_us;
  U32 assign_passes;
  B32 used_cpu;
};

typedef struct QE_SortTrace QE_SortTrace;
struct QE_SortTrace
{
  U64 row_count;
  U64 gpu_time_us;
  B32 used_cpu;
  U64 kept_rows;
};

//~ tec: what the optimizer passes down so an operator can size its buffers and pick a device before it runs, zero means unknown
typedef struct QE_JoinHints QE_JoinHints;
struct QE_JoinHints
{
  U64 output_rows;
  B32 fuse_round_trips;
};

typedef struct QE_AggregateHints QE_AggregateHints;
struct QE_AggregateHints
{
  U64 group_count;
  U64 cpu_max_rows;
  B32 fuse_round_trips;
};

typedef struct QE_SortHints QE_SortHints;
struct QE_SortHints
{
  U64 top_k;
  U64 gpu_min_rows;
};

// tec: nodes with no operator of their own to time, only the rows they produced
typedef struct QE_RowsTrace QE_RowsTrace;
struct QE_RowsTrace
{
  U64 rows_out;
};

typedef struct QE_NodeTrace QE_NodeTrace;
struct QE_NodeTrace
{
  QE_NodeTrace* next;
  PLAN_Node* plan_node;
  PLAN_NodeType node_type;
  union
  {
    QE_ScanTrace scan;
    QE_JoinTrace join;
    QE_AggregateTrace aggregate;
    QE_SortTrace sort;
    QE_RowsTrace rows;
  };
};

typedef struct QE_TraceCtx QE_TraceCtx;
struct QE_TraceCtx
{
  Arena* arena;
  QE_NodeTrace* records;
  QE_NodeTrace* records_last;
};

internal QE_TraceCtx* qe_trace_ctx_alloc(Arena* arena);
internal QE_NodeTrace* qe_trace_record_begin(QE_TraceCtx* trace, PLAN_Node* plan_node, PLAN_NodeType node_type);
internal QE_NodeTrace* qe_trace_find(QE_TraceCtx* trace, PLAN_Node* plan_node);

internal void qe_bytecode_emit(QE_BytecodeProgram* prog, U32 word);
internal U32 qe_add_numeric_const(QE_BytecodeProgram* prog, F64 value);
internal QE_StringConstRef qe_add_string_const(QE_BytecodeProgram* prog, String8 str);
internal QE_ColumnBinding* qe_find_binding(QE_BytecodeProgram* prog, String8 column_name);
internal QE_ColumnBinding* qe_bind_column(QE_BytecodeProgram* prog, GDB_Table* table, String8 column_name);
internal QE_Opcode qe_opcode_from_comparison_operator(String8 op);
internal void qe_compile_load_value(QE_BytecodeProgram* prog, GDB_Table* table, IR_Node* node);
internal void qe_compile_condition(QE_BytecodeProgram* prog, GDB_Table* table, IR_Node* condition, QE_ScanTrace* out_trace);

//~ tec: fuzzy search SIMILARITY()/EDIT_DISTANCE()
internal B32 qe_ir_is_fuzzy_call(IR_Node* node, B32* out_is_distance);
internal F64 qe_str8_trigram_similarity(String8 a, String8 b);
internal U64 qe_str8_edit_distance(String8 a, String8 b);

typedef struct QE_ScanResult QE_ScanResult;
struct QE_ScanResult
{
  U64* indices;
  U64 count;
  F64* scores;
  B32 score_is_distance;
  String8 score_column_name;
  String8 score_needle;
};

internal void qe_bytecode_program_build(Arena* arena, QE_BytecodeProgram* prog, GDB_Database* database, GDB_Table* table, IR_Node* root_node, IR_Node* where_clause, QE_ScanTrace* out_trace);
internal U32 qe_bytecode_program_max_stack_depth(QE_BytecodeProgram* prog);
internal QE_ScanResult qe_scan_filter(Arena* arena, GDB_Database* database, GDB_Table* table, IR_Node* where_clause, QE_ScanTrace* out_trace);
internal B32 qe_try_index_scan(Arena* arena, GDB_Table* table, IR_Node* where_clause, QE_ScanResult* out_result);
internal QE_ScanResult qe_cpu_scan_filter(Arena* arena, GDB_Table* table, IR_Node* where_clause, QE_ScanTrace* out_trace);

internal B32 qe_resolve_leaf_comparison(GDB_Table* table, IR_Node* condition,
                                        GDB_Column** out_column,
                                        B32* out_is_eq, B32* out_is_lt, B32* out_is_le, B32* out_is_gt, B32* out_is_ge,
                                        B32* out_is_string, F64* out_target_numeric, String8* out_target_string);
internal Rng1U64* qe_scan_build_dispatch_ranges(Arena* arena, GDB_Table* table, IR_Node* where_clause,
                                                U64 rows_per_chunk, U64* out_range_count, U64* out_pruned_rows,
                                                String8* out_pruned_column_name);

//~ tec: shared query-result representation
#define PLAN_NULL_ROW max_U64 // tec: unmatched side of a LEFT JOIN - treat a column read against this as NULL

typedef struct PLAN_RowSet PLAN_RowSet;
struct PLAN_RowSet
{
  GDB_Table** tables;
  String8* aliases;
  U64 table_count;
  U64** row_indices;
  U64 count;
  F64* scores;
  B32 score_is_distance;
  String8 score_column_name;
  String8 score_needle;
};

typedef struct PLAN_AggColumn PLAN_AggColumn;
struct PLAN_AggColumn
{
  String8 name;
  GDB_ColumnType type;
  U32 decimal_scale;
  GDB_EnumType* enum_type;
  F64* numeric_values;
  String8* string_values;
  U8* is_null;
};

typedef struct PLAN_Materialized PLAN_Materialized;
struct PLAN_Materialized
{
  PLAN_AggColumn* columns;
  U64 column_count;
  U64 count; // tec: number of result rows (groups)
};

typedef struct PLAN_ExecResult PLAN_ExecResult;
struct PLAN_ExecResult
{
  B32 supported;
  B32 is_materialized;
  PLAN_RowSet rows;
  PLAN_Materialized materialized;
};

internal String8 qe_column_list_item_display_name(Arena* arena, IR_Node* item);

typedef struct QE_ResultChunk QE_ResultChunk;
struct QE_ResultChunk
{
  U64* indices;
  U64 count;
  F64* scores;
  QE_ResultChunk* next;
};

//~ tec: multi-table column qualifier resolution
internal GDB_Table* qe_resolve_column_table(PLAN_RowSet* rows, String8 column_name, String8* out_bare_name, U64* out_slot);

internal U64 qe_rowset_table_slot(PLAN_RowSet* rows, GDB_Table* table);

internal F64* qe_gather_numeric_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column);
internal GDB_StringDataChunk qe_gather_string_column(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column);

internal F64 qe_row_eval_fuzzy_call(Arena* arena, PLAN_RowSet* rows, IR_Node* call, U64 output_row, B32 is_distance);

typedef struct QE_GatherNumericTask QE_GatherNumericTask;
struct QE_GatherNumericTask
{
  Rng1U64* ranges;
  U64* table_rows;
  void* base_ptr;
  U64 min_row;
  GDB_ColumnType column_type;
  U64 column_size;
  F64* values;
};

internal F64 qe_read_numeric_as_f64(GDB_Column* column, U64 row_index);
internal THREAD_POOL_TASK_FUNC(qe_gather_numeric_task);

//- tec: dictionary codes
typedef struct QE_GatherDictCodesTask QE_GatherDictCodesTask;
struct QE_GatherDictCodesTask
{
  Rng1U64* ranges;
  U64* table_rows;
  U32* dict_codes;
  F64* values;
};

typedef struct QE_DictLookupTask QE_DictLookupTask;
struct QE_DictLookupTask
{
  Rng1U64* ranges;
  GDB_StringDataChunk chunk;
  GDB_StringDict* dict;
  F64* values;
};

typedef struct QE_DenseDictCodesTask QE_DenseDictCodesTask;
struct QE_DenseDictCodesTask
{
  Rng1U64* ranges;
  U32* codes;
  F64* values;
};

internal QE_ColumnBinding* qe_bind_column_dict_codes(QE_BytecodeProgram* prog, GDB_Table* table, String8 column_name);
internal THREAD_POOL_TASK_FUNC(qe_gather_dict_codes_task);
internal F64* qe_gather_string_dict_codes(Arena* arena, PLAN_RowSet* rows, U64 table_slot, GDB_Column* column);
internal THREAD_POOL_TASK_FUNC(qe_dict_lookup_task);
internal THREAD_POOL_TASK_FUNC(qe_dense_dict_codes_task);
internal F64* qe_dict_codes_to_f64_dense(Arena* arena, U32* codes, U64 count);
internal F64* qe_dict_codes_from_string_chunk(Arena* arena, GDB_StringDataChunk* chunk, GDB_StringDict* dict);

//- tec: f32 narrowing
typedef struct QE_NarrowCheckTask QE_NarrowCheckTask;
struct QE_NarrowCheckTask
{
  Rng1U64* ranges;
  F64* values;
  B32* task_narrow;
};

typedef struct QE_NarrowConvertTask QE_NarrowConvertTask;
struct QE_NarrowConvertTask
{
  Rng1U64* ranges;
  F64* src;
  F32* dst;
};

internal THREAD_POOL_TASK_FUNC(qe_narrow_check_task);
internal B32 qe_values_round_trip_f32(F64* values, U64 count);
internal THREAD_POOL_TASK_FUNC(qe_narrow_convert_task);
internal F32* qe_values_to_f32(Arena* arena, F64* values, U64 count);

//~ tec: sort

// tec: baked into bitonic_sort.comp/bitonic_sort_f32.comp's PAYLOAD_STRIDE and key indexing
// so they cant be configured via settings
#define QE_SORT_MAX_KEYS   4
#define QE_SORT_MAX_TABLES 4
internal PLAN_RowSet qe_sort_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* order_by_ir, QE_SortHints* hints, QE_SortTrace* out_trace);
internal PLAN_Materialized qe_sort_materialized(Arena* arena, PLAN_Materialized* m, IR_Node* order_by_ir, QE_SortHints* hints);

//- tec: ordering row positions through a comparison callback, used for the top-K heap and the stable merge
typedef B32 QE_OrderLessFn(void* context, U64 a, U64 b);

internal B32 qe_order_before(QE_OrderLessFn* less, void* context, U64 a, U64 b);
internal void qe_order_merge_sort(U64* order, U64* buffer, U64 count, QE_OrderLessFn* less, void* context);
internal void qe_order_sift_down(U64* heap, U64 count, U64 root, QE_OrderLessFn* less, void* context);
internal U64 qe_order_select_top(U64* order, U64* buffer, U64 count, U64 keep, QE_OrderLessFn* less, void* context);
internal B32 qe_sort_rows_less(void* context, U64 a, U64 b);

typedef struct QE_MaterializedOrder QE_MaterializedOrder;
struct QE_MaterializedOrder
{
  PLAN_Materialized* materialized;
  IR_Node* order_by;
};

internal B32 qe_materialized_order_less(void* context, U64 a, U64 b);

typedef struct QE_SortRowsCtx QE_SortRowsCtx;
struct QE_SortRowsCtx
{
  U32 num_keys;
  B32 key_is_string[QE_SORT_MAX_KEYS];
  B32 key_desc[QE_SORT_MAX_KEYS];
  F64* numeric_keys[QE_SORT_MAX_KEYS];        // tec: dense, indexed by output-row position (0..count-1)
  GDB_StringDataChunk string_keys[QE_SORT_MAX_KEYS];
};

global QE_SortRowsCtx* g_qe_sort_rows_ctx = 0;

internal S32 qe_str8_compare(String8 a, String8 b);
internal int qe_sort_rows_compare(const void* a, const void* b);
internal B32 qe_materialized_row_less(PLAN_Materialized* m, IR_Node* order_by_ir, U64 a, U64 b);

//~ tec: aggregate

#define QE_AGG_MAX_GROUP_COLS 4
#define QE_AGG_MAX_EXPRS      8

// tec: aggregate func_codes. must stay in sync with aggregate_reduce.comp/aggregate_reduce_f32.comp's FUNC_* defines
#define QE_AGG_FUNC_COUNT                  0
#define QE_AGG_FUNC_SUM                    1
#define QE_AGG_FUNC_AVG                    2
#define QE_AGG_FUNC_MIN                    3
#define QE_AGG_FUNC_MAX                    4
#define QE_AGG_FUNC_APPROX_COUNT_DISTINCT  5
#define QE_AGG_FUNC_APPROX_PERCENTILE      6
#define QE_AGG_FUNC_COUNT_CODES            7 // tec: one past the last valid func_code, for policy table sizing

typedef struct QE_AggFuncPolicy QE_AggFuncPolicy;
struct QE_AggFuncPolicy
{
  GDB_ColumnType output_type;
};

global QE_AggFuncPolicy qe_agg_func_policy[QE_AGG_FUNC_COUNT_CODES] =
{
  { GDB_ColumnType_U64 }, // QE_AGG_FUNC_COUNT
  { GDB_ColumnType_F64 }, // QE_AGG_FUNC_SUM
  { GDB_ColumnType_F64 }, // QE_AGG_FUNC_AVG
  { GDB_ColumnType_F64 }, // QE_AGG_FUNC_MIN
  { GDB_ColumnType_F64 }, // QE_AGG_FUNC_MAX
  { GDB_ColumnType_U64 }, // QE_AGG_FUNC_APPROX_COUNT_DISTINCT
  { GDB_ColumnType_F64 }, // QE_AGG_FUNC_APPROX_PERCENTILE
};

internal F64 qe_hll_estimate_cardinality(U32* registers, U64 num_registers);

internal PLAN_Materialized qe_aggregate(Arena* arena, GDB_Database* database, PLAN_RowSet* input, IR_Node* group_by_ir, IR_Node* column_list_ir, IR_Node* having_ir, QE_AggregateHints* hints, QE_AggregateTrace* out_trace);
internal PLAN_Materialized qe_apply_having(Arena* arena, PLAN_Materialized* m, IR_Node* having_ir);

typedef struct QE_AggExprInfo QE_AggExprInfo;
struct QE_AggExprInfo
{
  String8 display_name;
  U32 func_code;
  GDB_Table* arg_table;
  U64 arg_slot;
  GDB_Column* arg_column;
  // tec: APPROX_PERCENTILE's fraction argument
  F64 f64_param;   
};

internal B32 qe_agg_func_code_from_name(String8 name, U32* out_func_code);
internal B32 qe_aggregate_collect_exprs(Arena* arena, PLAN_RowSet* input, IR_Node* node, QE_AggExprInfo* exprs, U32* num_exprs);
internal void qe_agg_output_type_for_expr(QE_AggExprInfo* expr, GDB_ColumnType* out_type, U32* out_decimal_scale, GDB_EnumType** out_enum_type);
internal PLAN_Materialized qe_aggregate_build_output(Arena* arena, PLAN_RowSet* input, IR_Node* column_list_ir, QE_AggExprInfo* exprs, U32 num_exprs, U64 num_groups, U64* representative_readback, F64* results_readback);

//- tec: aggregation on the CPU for inputs too small to pay for three GPU round trips
typedef struct QE_AggregateKeys QE_AggregateKeys;
struct QE_AggregateKeys
{
  U32 column_count;
  F64* numeric[QE_AGG_MAX_GROUP_COLS];
  GDB_StringDataChunk strings[QE_AGG_MAX_GROUP_COLS];
  U32 string_mask;
};

typedef struct QE_AggregateAccumulator QE_AggregateAccumulator;
struct QE_AggregateAccumulator
{
  F64 sum;
  U64 count;
  F64 min;
  F64 max;
};

internal B32 qe_aggregate_cpu_supported(QE_AggExprInfo* exprs, U32 num_exprs);
internal U64 qe_aggregate_hash_key(QE_AggregateKeys* keys, U64 row);
internal B32 qe_aggregate_keys_equal(QE_AggregateKeys* keys, U64 a, U64 b);
internal F64 qe_aggregate_finish(QE_AggExprInfo* expr, QE_AggregateAccumulator* accumulator);
internal U64 qe_aggregate_cpu_reduce(Arena* arena, U64 row_count, QE_AggregateKeys* keys, QE_AggExprInfo* exprs, U32 num_exprs, F64** expr_args, U64** out_representatives, F64** out_results);

//- tec: t-digest
// tec: a t-digest centroid - a (mean, weight) pair.
typedef struct QE_TDigestCentroid QE_TDigestCentroid;
struct QE_TDigestCentroid
{
  F32 mean;
  F32 weight;
};

internal int qe_tdigest_centroid_compare(const void* a, const void* b);
internal F64 qe_tdigest_estimate_percentile(QE_TDigestCentroid* centroids, U64 count, F64 fraction);

//- tec: having
internal F64 qe_having_load_value(PLAN_Materialized* m, IR_Node* node, U64 row, B32* out_is_string, String8* out_string);
internal B32 qe_having_eval(PLAN_Materialized* m, IR_Node* condition, U64 row);

//~ tec: GPU build/probe equi-join
internal PLAN_RowSet qe_hash_join(Arena* arena, PLAN_RowSet* left, GDB_Table* right_table, String8 right_alias, U64* right_rows, U64 right_count, String8 join_type, IR_Node* condition, QE_JoinHints* hints, QE_JoinTrace* out_trace);

// tec: the pair buffer a hinted join allocates up front is capped so a wild overestimate cannot exhaust device memory
#define QE_JOIN_HINT_MAX_PAIRS (16ull * 1024ull * 1024ull)

// tec: the most pairs a join copies back speculatively in the probe submit, past that a wrong estimate would cost more than the submit it saves
#define QE_JOIN_SPECULATIVE_MAX_PAIRS (256ull * 1024ull)

// tec: the block totals are scanned by one workgroup, 256 threads each taking a chunk, so this bounds how many blocks that chunk covers
#define QE_JOIN_GPU_PREFIX_MAX_BUCKETS (4ull * 1024ull * 1024ull)
internal U64 qe_join_output_capacity(U64 probe_rows, U64 build_rows, QE_JoinHints* hints, U64 hard_max_capacity);

//- tec: semi and anti joins keep or drop left rows by whether the key has a match, join_type "semi" or "anti"
internal B32 qe_join_type_is_filter(String8 join_type);
internal U64 qe_key_hash(GDB_Column* column, U64 row);
internal B32 qe_key_equal(GDB_Column* column, U64 row_a, U64 row_b);
internal U64* qe_distinct_key_rows(Arena* arena, GDB_Column* key_column, U64* rows, U64 row_count, U64* out_count);
internal PLAN_RowSet qe_join_filter_left_rows(Arena* arena, PLAN_RowSet* left, U32* pairs, U64 pair_count, B32 keep_matched);
internal B32 qe_column_belongs_to_table(GDB_Table* table, String8 alias, String8 column_name);
internal IR_Node* qe_find_equi_condition(PLAN_RowSet* left_rows, GDB_Table* right_table, String8 right_alias, IR_Node* condition);
internal PLAN_RowSet qe_filter_joined_rows(Arena* arena, PLAN_RowSet* rows, IR_Node* condition);

internal B32 qe_column_belongs_to_rowset(PLAN_RowSet* rows, String8 column_name);
internal IR_Node* qe_validate_equi_condition(PLAN_RowSet* left_rows, GDB_Table* right_table, String8 right_alias, IR_Node* condition);

//~ tec: column prefetch
typedef struct QE_PrefetchBindingResult QE_PrefetchBindingResult;
struct QE_PrefetchBindingResult
{
  B32 valid;
  B32 is_string;
  B32 cached;
  void* data_ptr;
  U64 size;
  GDB_StringDataChunk str_chunk;
};

typedef struct QE_PrefetchSlot QE_PrefetchSlot;
struct QE_PrefetchSlot
{
  Arena* arena;
  Rng1U64 chunk_range;
  U64 chunk_rows;
  QE_PrefetchBindingResult bindings[QE_MAX_COLUMN_BINDINGS];
};

typedef struct QE_PrefetchCtx QE_PrefetchCtx;
struct QE_PrefetchCtx
{
  QE_BytecodeProgram* prog;
  QE_PrefetchSlot slots[2];
  
  OS_Handle worker;
  // tec: main -> worker, "a request is pending" (mailbox depth 1)
  OS_Handle request_sem; 
  // tec: worker -> main, "the requested slot is filled"
  OS_Handle ready_sem;   
  
  U32 pending_slot;
  Rng1U64 pending_range;
  U64 pending_rows;
  B32 stop;
};

internal void qe_prefetch_read_slot(QE_PrefetchSlot* slot, QE_BytecodeProgram* prog, Rng1U64 range, U64 rows);
internal void qe_prefetch_worker_main(void* raw_ctx);
internal QE_PrefetchCtx* qe_prefetch_start(Arena* arena, QE_BytecodeProgram* prog);
internal void qe_prefetch_request(QE_PrefetchCtx* ctx, U32 slot_index, Rng1U64 range, U64 rows);
internal QE_PrefetchSlot* qe_prefetch_wait(QE_PrefetchCtx* ctx, U32 slot_index);
internal void qe_prefetch_stop(QE_PrefetchCtx* ctx);

//~ tec: index range scan
internal String8 qe_bare_column_name(String8 name);
internal S32 qe_index_row_cmp_target(Arena* arena, GDB_Column* column, B32 is_string, U64 row, F64 target_numeric, String8 target_string);
internal U64 qe_index_lower_bound(Arena* arena, GDB_Column* column, B32 is_string, U64* order, U64 count, F64 target_numeric, String8 target_string);
internal U64 qe_index_upper_bound(Arena* arena, GDB_Column* column, B32 is_string, U64* order, U64 count, F64 target_numeric, String8 target_string);
internal B32 qe_index_range_for_leaf(Arena* arena, GDB_Table* table, IR_Node* condition, QE_ScanResult* out_result);
internal U32 qe_collect_and_leaves(IR_Node* condition, IR_Node** out_leaves, U32 count, U32 max_leaves);
internal B32 qe_index_leaf_range(Arena* arena, GDB_Table* table, IR_Node* condition, GDB_Index** out_index, U64* out_range_lo, U64* out_range_hi);
internal B32 qe_index_leaf_match_count(Arena* arena, GDB_Table* table, IR_Node* condition, U64* out_match_count);
internal B32 qe_index_narrow_and_filter(Arena* arena, GDB_Table* table, IR_Node* root, IR_Node* leaf, QE_ScanResult* out_result);
internal B32 qe_index_scan_with_leaf(Arena* arena, GDB_Table* table, IR_Node* where_clause, IR_Node* leaf, QE_ScanResult* out_result);
internal void qe_index_ordered_scan(Arena* arena, GDB_Table* table, String8 alias, GDB_Index* index, IR_Node* condition, B32 descending, U64 wanted, QE_ScanResult* out_result);

//~ tec: zone map pruning
typedef struct QE_ZonemapLeaf QE_ZonemapLeaf;
struct QE_ZonemapLeaf
{
  GDB_Column* column;
  B32 is_eq, is_lt, is_le, is_gt, is_ge;
  F64 target;
};

internal B32 qe_zonemap_chunk_is_prunable(QE_ZonemapLeaf* leaf, GDB_ZoneMapChunk* chunk);

//~ tec: row condition evaluation
internal B32 qe_str8_contains(String8 haystack, String8 needle);
internal F64 qe_row_load_value(Arena* arena, PLAN_RowSet* rows, IR_Node* node, U64 output_row, B32* out_is_string, String8* out_string, B32* out_is_null);
internal B32 qe_row_condition_eval(Arena* arena, PLAN_RowSet* rows, IR_Node* condition, U64 output_row);

//- tec: IN lists
typedef struct QE_InSet QE_InSet;
struct QE_InSet
{
  QE_InSet* next;
  IR_Node* list_node;
  F64* nums;
  U64 num_count;
  String8* strs;
  U64 str_count;
};

global QE_InSet* g_qe_in_sets = 0;

internal S32 qe_f64_compare_for_sort(const void* a, const void* b);
internal S32 qe_str8_compare_for_sort(const void* a, const void* b);
internal B32 qe_in_item_resolve(GDB_Column* column, IR_Node* item, B32* out_is_string, F64* out_num, String8* out_str);
internal GDB_Column* qe_in_left_column(PLAN_RowSet* rows, IR_Node* left);
internal void qe_in_sets_register(Arena* arena, PLAN_RowSet* rows, IR_Node* condition);
internal void qe_in_sets_unregister_all(void);
internal B32 qe_in_list_contains(PLAN_RowSet* rows, IR_Node* left, IR_Node* list_node, B32 lstr, F64 lv, String8 ls);

//~ tec: cpu scan
typedef struct QE_CpuScanTask QE_CpuScanTask;
struct QE_CpuScanTask
{
  Rng1U64* ranges;
  PLAN_RowSet* rows;
  IR_Node* condition_root;
  U64* task_matched_counts;
  U64** task_matched_indices;
};

internal THREAD_POOL_TASK_FUNC(qe_cpu_scan_filter_task);

#endif //QUERY_EXEC_H
