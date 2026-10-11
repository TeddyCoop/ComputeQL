#ifndef CQL_STATS_H
#define CQL_STATS_H

//- tec: statistics
typedef struct CQL_StatsTaskResult CQL_StatsTaskResult;
struct CQL_StatsTaskResult
{
  U8* hll_registers;
  U64 non_null_count;
  U64 null_count;
  B32 has_range;
  F64 min_value;
  F64 max_value;
};

typedef struct CQL_StatsScanCtx CQL_StatsScanCtx;
struct CQL_StatsScanCtx
{
  CQL_Column* column;
  B32 is_string;
  void* base_ptr;
  CQL_StringDataChunk string_chunk;
  Rng1U64* ranges;
  CQL_StatsTaskResult* results;
};

internal U64 cql_stats_hash_u64(U64 value);
internal U64 cql_stats_hash_f64(F64 value);
internal U64 cql_stats_hash_bytes(U8* bytes, U64 size);
internal void cql_stats_hll_add(U8* registers, U64 hash);
internal U64 cql_stats_hll_estimate(U8* registers);
internal int cql_stats_compare_f64(const void* a, const void* b);
internal int cql_stats_compare_u64(const void* a, const void* b);
internal THREAD_POOL_TASK_FUNC(cql_stats_scan_task);
internal U64 cql_stats_build_mcv(CQL_ColumnStats* stats, U64* sorted_keys, U64 key_count);
internal void cql_stats_build_histogram(CQL_ColumnStats* stats, F64* sorted_values, U64 value_count);
internal B32 cql_column_stats_is_current(CQL_Column* column);
internal void cql_column_ensure_stats(CQL_Column* column);
internal void cql_table_ensure_stats(CQL_Table* table);
internal U64 cql_column_stats_record_size(CQL_Column* column);
internal U8* cql_column_stats_write_record(U8* out, CQL_Column* column);
internal B32 cql_column_stats_read_record(U8** read_ptr, U8* end, String8* out_name, CQL_ColumnStats* out_stats);

#endif //CQL_STATS_H
