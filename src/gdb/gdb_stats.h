#ifndef GDB_STATS_H
#define GDB_STATS_H

//- tec: statistics
typedef struct GDB_StatsTaskResult GDB_StatsTaskResult;
struct GDB_StatsTaskResult
{
  U8* hll_registers;
  U64 non_null_count;
  U64 null_count;
  B32 has_range;
  F64 min_value;
  F64 max_value;
};

typedef struct GDB_StatsScanCtx GDB_StatsScanCtx;
struct GDB_StatsScanCtx
{
  GDB_Column* column;
  B32 is_string;
  void* base_ptr;
  GDB_StringDataChunk string_chunk;
  Rng1U64* ranges;
  GDB_StatsTaskResult* results;
};

internal U64 gdb_stats_hash_u64(U64 value);
internal U64 gdb_stats_hash_f64(F64 value);
internal U64 gdb_stats_hash_bytes(U8* bytes, U64 size);
internal void gdb_stats_hll_add(U8* registers, U64 hash);
internal U64 gdb_stats_hll_estimate(U8* registers);
internal int gdb_stats_compare_f64(const void* a, const void* b);
internal int gdb_stats_compare_u64(const void* a, const void* b);
internal THREAD_POOL_TASK_FUNC(gdb_stats_scan_task);
internal U64 gdb_stats_build_mcv(GDB_ColumnStats* stats, U64* sorted_keys, U64 key_count);
internal void gdb_stats_build_histogram(GDB_ColumnStats* stats, F64* sorted_values, U64 value_count);
internal B32 gdb_column_stats_is_current(GDB_Column* column);
internal void gdb_column_ensure_stats(GDB_Column* column);
internal void gdb_table_ensure_stats(GDB_Table* table);
internal U64 gdb_column_stats_record_size(GDB_Column* column);
internal U8* gdb_column_stats_write_record(U8* out, GDB_Column* column);
internal B32 gdb_column_stats_read_record(U8** read_ptr, U8* end, String8* out_name, GDB_ColumnStats* out_stats);

#endif //GDB_STATS_H
