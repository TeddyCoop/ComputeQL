//~ tec: hashing

// tec: splitmix64 finalizer
internal U64
gdb_stats_hash_u64(U64 value)
{
  U64 x = value + 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  x = x ^ (x >> 31);
  return x;
}

internal U64
gdb_stats_hash_f64(F64 value)
{
  F64 normalized = value;
  if (normalized == 0.0)
  {
    normalized = 0.0;
  }
  U64 bits = 0;
  MemoryCopy(&bits, &normalized, sizeof(bits));
  return gdb_stats_hash_u64(bits);
}

// tec: FNV-1a, then the finalizer above to spread the low bits
internal U64
gdb_stats_hash_bytes(U8* bytes, U64 size)
{
  U64 hash = 14695981039346656037ULL;
  for (U64 i = 0; i < size; i += 1)
  {
    hash = hash ^ bytes[i];
    hash = hash * 1099511628211ULL;
  }
  return gdb_stats_hash_u64(hash);
}

//~ tec: HyperLogLog

internal void
gdb_stats_hll_add(U8* registers, U64 hash)
{
  U64 register_index = hash >> (64 - GDB_STATS_HLL_PRECISION);
  U64 remaining = (hash << GDB_STATS_HLL_PRECISION) | ((U64)1 << (GDB_STATS_HLL_PRECISION - 1));

  U8 rank = 1;
  U64 top_bit = (U64)1 << 63;
  while ((remaining & top_bit) == 0)
  {
    rank += 1;
    remaining = remaining << 1;
  }

  if (registers[register_index] < rank)
  {
    registers[register_index] = rank;
  }
}

internal U64
gdb_stats_hll_estimate(U8* registers)
{
  F64 register_count = (F64)GDB_STATS_HLL_REGISTER_COUNT;
  F64 alpha = 0.7213 / (1.0 + 1.079 / register_count);

  F64 harmonic_sum = 0.0;
  U64 zero_registers = 0;
  for (U64 i = 0; i < GDB_STATS_HLL_REGISTER_COUNT; i += 1)
  {
    harmonic_sum += 1.0 / (F64)((U64)1 << registers[i]);
    if (registers[i] == 0)
    {
      zero_registers += 1;
    }
  }

  F64 estimate = alpha * register_count * register_count / harmonic_sum;

  // tec: linear counting corrects the small range
  if (estimate <= 2.5 * register_count && zero_registers > 0)
  {
    estimate = register_count * log(register_count / (F64)zero_registers);
  }

  return (U64)(estimate + 0.5);
}

//~ tec: sorting

internal int
gdb_stats_compare_f64(const void* a, const void* b)
{
  F64 left = *(const F64*)a;
  F64 right = *(const F64*)b;
  if (left < right)
  {
    return -1;
  }
  if (left > right)
  {
    return 1;
  }
  return 0;
}

internal int
gdb_stats_compare_u64(const void* a, const void* b)
{
  U64 left = *(const U64*)a;
  U64 right = *(const U64*)b;
  if (left < right)
  {
    return -1;
  }
  if (left > right)
  {
    return 1;
  }
  return 0;
}

//~ tec: scan

internal THREAD_POOL_TASK_FUNC(gdb_stats_scan_task)
{
  GDB_StatsScanCtx* ctx = (GDB_StatsScanCtx*)raw_task;
  GDB_StatsTaskResult* result = &ctx->results[task_id];
  Rng1U64 range = ctx->ranges[task_id];
  GDB_Column* column = ctx->column;

  for (U64 row = range.min; row < range.max; row += 1)
  {
    if (gdb_column_is_null(column, row))
    {
      result->null_count += 1;
      continue;
    }

    U64 hash = 0;
    if (ctx->is_string)
    {
      String8 value = gdb_dict_row_string(&ctx->string_chunk, row);
      hash = gdb_stats_hash_bytes(value.str, value.size);
    }
    else
    {
      U8* data = (U8*)ctx->base_ptr + row * column->size;
      U64 raw_bits = 0;
      MemoryCopy(&raw_bits, data, Min(column->size, (U64)sizeof(raw_bits)));
      hash = gdb_stats_hash_u64(raw_bits);

      F64 value = gdb_numeric_value_as_f64(column->type, data);
      if (!result->has_range)
      {
        result->min_value = value;
        result->max_value = value;
        result->has_range = 1;
      }
      else
      {
        if (value < result->min_value)
        {
          result->min_value = value;
        }
        if (value > result->max_value)
        {
          result->max_value = value;
        }
      }
    }

    gdb_stats_hll_add(result->hll_registers, hash);
    result->non_null_count += 1;
  }
}

//~ tec: summaries built from a sorted sample

// tec: returns the number of distinct keys seen in the sample
internal U64
gdb_stats_build_mcv(GDB_ColumnStats* stats, U64* sorted_keys, U64 key_count)
{
  stats->mcv_count = 0;
  U64 run_counts[GDB_STATS_MCV_COUNT] = {0};
  U64 distinct_keys = 0;

  U64 run_start = 0;
  while (run_start < key_count)
  {
    U64 run_end = run_start + 1;
    while (run_end < key_count && sorted_keys[run_end] == sorted_keys[run_start])
    {
      run_end += 1;
    }

    distinct_keys += 1;
    U64 run_length = run_end - run_start;

    if (run_length >= 2)
    {
      U32 insert_at = stats->mcv_count;
      while (insert_at > 0 && run_counts[insert_at - 1] < run_length)
      {
        insert_at -= 1;
      }

      if (insert_at < GDB_STATS_MCV_COUNT)
      {
        U32 last = Min(stats->mcv_count, (U32)(GDB_STATS_MCV_COUNT - 1));
        for (U32 shift = last; shift > insert_at; shift -= 1)
        {
          run_counts[shift] = run_counts[shift - 1];
          stats->mcv[shift] = stats->mcv[shift - 1];
        }
        run_counts[insert_at] = run_length;
        stats->mcv[insert_at].key = sorted_keys[run_start];
        stats->mcv[insert_at].fraction = (F64)run_length / (F64)key_count;
        if (stats->mcv_count < GDB_STATS_MCV_COUNT)
        {
          stats->mcv_count += 1;
        }
      }
    }

    run_start = run_end;
  }

  return distinct_keys;
}

internal void
gdb_stats_build_histogram(GDB_ColumnStats* stats, F64* sorted_values, U64 value_count)
{
  stats->histogram_bucket_count = 0;
  if (value_count == 0)
  {
    return;
  }

  // tec: bucket b starts at sample b * n / buckets, so a table with as few rows as buckets gets one value per bucket
  U64 bucket_count = Min((U64)GDB_STATS_HISTOGRAM_BUCKETS, value_count);
  for (U64 bucket = 0; bucket <= bucket_count; bucket += 1)
  {
    U64 sample_index = Min(bucket * value_count / bucket_count, value_count - 1);
    stats->histogram_bounds[bucket] = sorted_values[sample_index];
  }

  // tec: the scan saw every row, so the outer bounds are exact even when the sample is not
  stats->histogram_bounds[0] = stats->min_value;
  stats->histogram_bounds[bucket_count] = stats->max_value;
  stats->histogram_bucket_count = (U32)bucket_count;
}

//~ tec: entry points

internal B32
gdb_column_stats_is_current(GDB_Column* column)
{
  B32 is_current = column->stats.is_computed && column->stats.computed_generation == column->write_generation;
  return is_current;
}

internal void
gdb_column_ensure_stats(GDB_Column* column)
{
  ProfBeginFunction();

  if (gdb_column_stats_is_current(column))
  {
    ProfEnd();
    return;
  }

  GDB_ColumnStats* stats = &column->stats;
  MemoryZeroStruct(stats);
  stats->row_count = column->row_count;

  B32 is_string = column->type == GDB_ColumnType_String8;
  B32 is_scannable = column->type != GDB_ColumnType_Invalid;

  if (column->row_count > 0 && is_scannable)
  {
    Temp scratch = scratch_begin(0, 0);

    GDB_StatsScanCtx ctx = {0};
    ctx.column = column;
    ctx.is_string = is_string;
    if (is_string)
    {
      ctx.string_chunk = gdb_column_get_string_chunk(scratch.arena, column, r1u64(0, column->row_count));
    }
    else
    {
      U64 range_size = 0;
      ctx.base_ptr = gdb_column_get_data_range(scratch.arena, column, r1u64(0, column->row_count), &range_size);
    }

    TP_Context* pool = app_thread_pool();
    U64 task_count = Max((U64)1, Min((U64)pool->worker_count, column->row_count));
    ctx.ranges = tp_divide_work(scratch.arena, column->row_count, (U32)task_count);
    ctx.results = push_array(scratch.arena, GDB_StatsTaskResult, task_count);
    for (U64 task = 0; task < task_count; task += 1)
    {
      ctx.results[task].hll_registers = push_array(scratch.arena, U8, GDB_STATS_HLL_REGISTER_COUNT);
    }

    TP_Arena* pool_arena = app_thread_pool_arena();
    TP_Temp temp = tp_temp_begin(pool_arena);
    tp_for_parallel(pool, pool_arena, task_count, gdb_stats_scan_task, &ctx);
    tp_temp_end(temp);

    U8* merged_registers = ctx.results[0].hll_registers;
    U64 non_null_count = 0;
    for (U64 task = 0; task < task_count; task += 1)
    {
      GDB_StatsTaskResult* result = &ctx.results[task];
      non_null_count += result->non_null_count;
      stats->null_count += result->null_count;

      if (task > 0)
      {
        for (U64 i = 0; i < GDB_STATS_HLL_REGISTER_COUNT; i += 1)
        {
          if (merged_registers[i] < result->hll_registers[i])
          {
            merged_registers[i] = result->hll_registers[i];
          }
        }
      }

      if (result->has_range)
      {
        if (!stats->has_range)
        {
          stats->min_value = result->min_value;
          stats->max_value = result->max_value;
          stats->has_range = 1;
        }
        else
        {
          if (result->min_value < stats->min_value)
          {
            stats->min_value = result->min_value;
          }
          if (result->max_value > stats->max_value)
          {
            stats->max_value = result->max_value;
          }
        }
      }
    }

    // tec: systematic sample by row position
    U64 stride = Max((U64)1, column->row_count / GDB_STATS_SAMPLE_ROWS);
    U64 sample_capacity = column->row_count / stride + 1;
    U64 sample_count = 0;
    F64* numeric_sample = 0;
    U64* key_sample = 0;
    if (is_string)
    {
      key_sample = push_array(scratch.arena, U64, sample_capacity);
    }
    else
    {
      numeric_sample = push_array(scratch.arena, F64, sample_capacity);
    }

    for (U64 row = 0; row < column->row_count; row += stride)
    {
      if (gdb_column_is_null(column, row))
      {
        continue;
      }

      if (is_string)
      {
        String8 value = gdb_dict_row_string(&ctx.string_chunk, row);
        key_sample[sample_count] = gdb_stats_hash_bytes(value.str, value.size);
        sample_count += 1;
      }
      else
      {
        U8* data = (U8*)ctx.base_ptr + row * column->size;
        F64 value = gdb_numeric_value_as_f64(column->type, data);
        if (value == value)
        {
          numeric_sample[sample_count] = value;
          sample_count += 1;
        }
      }
    }

    U64 sample_distinct_count = 0;
    if (is_string)
    {
      quick_sort(key_sample, sample_count, sizeof(U64), gdb_stats_compare_u64);
      sample_distinct_count = gdb_stats_build_mcv(stats, key_sample, sample_count);
    }
    else
    {
      quick_sort(numeric_sample, sample_count, sizeof(F64), gdb_stats_compare_f64);
      gdb_stats_build_histogram(stats, numeric_sample, sample_count);

      U64* numeric_keys = push_array(scratch.arena, U64, Max(sample_count, (U64)1));
      for (U64 i = 0; i < sample_count; i += 1)
      {
        numeric_keys[i] = gdb_stats_hash_f64(numeric_sample[i]);
      }
      sample_distinct_count = gdb_stats_build_mcv(stats, numeric_keys, sample_count);
    }

    // tec: a sample that holds every row gives an exact count, otherwise fall back to HyperLogLog
    U64 distinct_count = 0;
    if (stride == 1)
    {
      distinct_count = sample_distinct_count;
    }
    else
    {
      distinct_count = gdb_stats_hll_estimate(merged_registers);
    }
    distinct_count = Min(distinct_count, non_null_count);
    if (non_null_count > 0)
    {
      distinct_count = Max(distinct_count, (U64)1);
    }

    B32 dict_is_current = column->has_dict && column->dict && column->dict_checked_generation == column->write_generation;
    if (dict_is_current)
    {
      distinct_count = column->dict->value_count;
    }
    if (column->is_unique || column->is_primary_key)
    {
      distinct_count = non_null_count;
    }
    stats->distinct_count = distinct_count;

    if (is_string)
    {
      gdb_column_close_string_chunk(column);
    }

    scratch_end(scratch);
  }

  stats->is_computed = 1;
  stats->computed_generation = column->write_generation;

  ProfEnd();
}

internal void
gdb_table_ensure_stats(GDB_Table* table)
{
  for (U64 column_index = 0; column_index < table->column_count; column_index += 1)
  {
    gdb_column_ensure_stats(table->columns[column_index]);
  }
}

//~ tec: persistence

internal U64
gdb_column_stats_record_size(GDB_Column* column)
{
  GDB_ColumnStats* stats = &column->stats;
  
  U64 size = sizeof(U64) + column->name.size;
  size += sizeof(U64) * 3;
  size += sizeof(U64) + sizeof(F64) * 2;
  size += sizeof(U64);
  if (stats->histogram_bucket_count > 0)
  {
    size += ((U64)stats->histogram_bucket_count + 1) * sizeof(F64);
  }
  size += sizeof(U64) + (U64)stats->mcv_count * (sizeof(U64) + sizeof(F64));
  return size;
}

internal U8*
gdb_column_stats_write_record(U8* out, GDB_Column* column)
{
  GDB_ColumnStats* stats = &column->stats;
  
  *(U64*)out = column->name.size;
  out += sizeof(U64);
  MemoryCopy(out, column->name.str, column->name.size);
  out += column->name.size;
  
  *(U64*)out = stats->row_count;
  out += sizeof(U64);
  *(U64*)out = stats->null_count;
  out += sizeof(U64);
  *(U64*)out = stats->distinct_count;
  out += sizeof(U64);
  
  *(U64*)out = stats->has_range;
  out += sizeof(U64);
  *(F64*)out = stats->min_value;
  out += sizeof(F64);
  *(F64*)out = stats->max_value;
  out += sizeof(F64);
  
  *(U64*)out = stats->histogram_bucket_count;
  out += sizeof(U64);
  if (stats->histogram_bucket_count > 0)
  {
    for (U32 bound = 0; bound <= stats->histogram_bucket_count; bound += 1)
    {
      *(F64*)out = stats->histogram_bounds[bound];
      out += sizeof(F64);
    }
  }
  
  *(U64*)out = stats->mcv_count;
  out += sizeof(U64);
  for (U32 entry = 0; entry < stats->mcv_count; entry += 1)
  {
    *(U64*)out = stats->mcv[entry].key;
    out += sizeof(U64);
    *(F64*)out = stats->mcv[entry].fraction;
    out += sizeof(F64);
  }
  
  return out;
}

// tec: returns 0 on a truncated or out of range record, read_ptr is then unusable
internal B32
gdb_column_stats_read_record(U8** read_ptr, U8* end, String8* out_name, GDB_ColumnStats* out_stats)
{
  U8* in = *read_ptr;
  
  if (in + sizeof(U64) > end)
  {
    return 0;
  }
  U64 name_size = *(U64*)in;
  in += sizeof(U64);
  if (name_size > (U64)(end - in))
  {
    return 0;
  }
  out_name->str = in;
  out_name->size = name_size;
  in += name_size;
  
  U64 fixed_size = sizeof(U64) * 3 + sizeof(U64) + sizeof(F64) * 2 + sizeof(U64);
  if (fixed_size > (U64)(end - in))
  {
    return 0;
  }
  
  MemoryZeroStruct(out_stats);
  out_stats->row_count = *(U64*)in;
  in += sizeof(U64);
  out_stats->null_count = *(U64*)in;
  in += sizeof(U64);
  out_stats->distinct_count = *(U64*)in;
  in += sizeof(U64);
  out_stats->has_range = (B32)*(U64*)in;
  in += sizeof(U64);
  out_stats->min_value = *(F64*)in;
  in += sizeof(F64);
  out_stats->max_value = *(F64*)in;
  in += sizeof(F64);
  
  U64 bucket_count = *(U64*)in;
  in += sizeof(U64);
  if (bucket_count > GDB_STATS_HISTOGRAM_BUCKETS)
  {
    return 0;
  }
  if (bucket_count > 0)
  {
    U64 bounds_size = (bucket_count + 1) * sizeof(F64);
    if (bounds_size > (U64)(end - in))
    {
      return 0;
    }
    for (U64 bound = 0; bound <= bucket_count; bound += 1)
    {
      out_stats->histogram_bounds[bound] = *(F64*)in;
      in += sizeof(F64);
    }
  }
  out_stats->histogram_bucket_count = (U32)bucket_count;
  
  if (sizeof(U64) > (U64)(end - in))
  {
    return 0;
  }
  U64 mcv_count = *(U64*)in;
  in += sizeof(U64);
  if (mcv_count > GDB_STATS_MCV_COUNT)
  {
    return 0;
  }
  if (mcv_count * (sizeof(U64) + sizeof(F64)) > (U64)(end - in))
  {
    return 0;
  }
  for (U64 entry = 0; entry < mcv_count; entry += 1)
  {
    out_stats->mcv[entry].key = *(U64*)in;
    in += sizeof(U64);
    out_stats->mcv[entry].fraction = *(F64*)in;
    in += sizeof(F64);
  }
  out_stats->mcv_count = (U32)mcv_count;
  
  *read_ptr = in;
  return 1;
}
