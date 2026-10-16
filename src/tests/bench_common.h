#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

//~ tec: shared benchmark harness for comparing compute_ql against sqlite.
/*
methodology: for every timed operation we do BENCH_WARMUP_RUNS discarded warm-up runs (at least BENCH_WARMUP_MIN_MICROSECONDS of them if that is set) followed by BENCH_TIMED_RUNS recorded runs, 
and report min/median/avg per phase. row count + an order independent per row checksum are compared between engines as a correctness sanity check
the checksum sums a per row FNV-1a hash across rows, since compute_ql and sqlite are not guaranteed to return matching row order for the same query
*/

#ifndef BENCH_WARMUP_RUNS
# define BENCH_WARMUP_RUNS 1
#endif
// tec: a GPU idles at a low clock, a test that times it sets this to keep warming up until at least this long has passed
#ifndef BENCH_WARMUP_MIN_MICROSECONDS
# define BENCH_WARMUP_MIN_MICROSECONDS 0
#endif
#ifndef BENCH_TIMED_RUNS
# define BENCH_TIMED_RUNS 20
#endif

//~ tec: tiny deterministic PRNG (splitmix64)
// fixed seed so compute_ql and sqlite always see byte identical generated data across runs
typedef struct Bench_Rng Bench_Rng;
struct Bench_Rng
{
  U64 state;
};

internal U64
bench_rng_next(Bench_Rng* rng)
{
  U64 z = (rng->state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

//~ tec: dataset generation

global char* g_bench_words[] =
{
  "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
  "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa",
};

typedef struct Bench_Row Bench_Row;
struct Bench_Row
{
  U32 id;
  String8 name;
  F64 value;
};

internal Bench_Row*
bench_generate_rows(Arena* arena, U64 row_count)
{
  Bench_Row* rows = push_array(arena, Bench_Row, row_count);
  Bench_Rng rng = {0x1234567890ABCDEFULL};
  U64 word_count = ArrayCount(g_bench_words);
  
  for (U64 i = 0; i < row_count; i++)
  {
    rows[i].id = (U32)(i + 1);
    
    U64 w = bench_rng_next(&rng) % word_count;
    U64 suffix = bench_rng_next(&rng) % 1000000ULL;
    rows[i].name = push_str8f(arena, "%s_%06llu", g_bench_words[w], suffix);
    
    U64 vraw = bench_rng_next(&rng) % 10000000ULL;
    rows[i].value = (F64)vraw / 100.0;
  }
  
  return rows;
}

internal B32
bench_write_csv(String8 path, Bench_Row* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  
  String8List lines = {0};
  str8_list_pushf(scratch.arena, &lines, "id,name,value\n");
  for (U64 i = 0; i < row_count; i++)
  {
    str8_list_pushf(scratch.arena, &lines, "%u,%.*s,%.4f\n",
                    rows[i].id, str8_varg(rows[i].name), rows[i].value);
  }
  String8 content = str8_list_join(scratch.arena, &lines, 0);
  
  B32 ok = 0;
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if (!os_handle_match(file, os_handle_zero()))
  {
    os_file_write(file, r1u64(0, content.size), content.str);
    os_file_close(file);
    ok = 1;
  }
  
  scratch_end(scratch);
  return ok;
}

//~ tec: order independent correctness checksum (FNV-1a per row, summed across rows)

#define BENCH_FNV_OFFSET_BASIS 14695981039346656037ULL
#define BENCH_FNV_PRIME        1099511628211ULL

internal U64
bench_fnv_mix_u64(U64 h, U64 v)
{
  for (U32 b = 0; b < 8; b++)
  {
    h ^= (v >> (b * 8)) & 0xFF;
    h *= BENCH_FNV_PRIME;
  }
  return h;
}

internal U64
bench_fnv_mix_string(U64 h, String8 s)
{
  for (U64 i = 0; i < s.size; i++)
  {
    h ^= s.str[i];
    h *= BENCH_FNV_PRIME;
  }
  return h;
}

// tec: round to nearest before truncating so FP noise from independently reparsing the same
global F64 g_bench_round_scale = 100.0;

internal U64
bench_scaled_round(F64 v)
{
  return (U64)(v * g_bench_round_scale + (v >= 0 ? 0.5 : -0.5));
}

//~ tec: timing stats
typedef struct Bench_Stats Bench_Stats;
struct Bench_Stats
{
  F64 prepare_min;
  F64 prepare_median; 
  F64 prepare_avg;
  
  F64 execute_min;
  F64 execute_median;
  F64 execute_avg;
  F64 execute_max;
  
  F64 fetch_min;
  F64 fetch_median;
  F64 fetch_avg;
  F64 fetch_max;
  
  F64 total_min;
  F64 total_median;
  F64 total_avg;
  F64 total_max;
  B32 fetch_included;
};

// tec: --samples prints every timed run of a query, to see whether a mean and a median differ because the runs fall into two groups
global B32 g_bench_print_samples = 0;

internal void
bench_sort_f64(F64* a, U64 n)
{
  for (U64 i = 1; i < n; i++)
  {
    F64 key = a[i];
    S64 j = (S64)i - 1;
    while (j >= 0 && a[j] > key)
    {
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = key;
  }
}

internal void
bench_reduce(F64* samples, U64 n, F64* out_min, F64* out_median, F64* out_avg)
{
  bench_sort_f64(samples, n);
  F64 sum = 0;
  for (U64 i = 0; i < n; i++) sum += samples[i];
  *out_min = samples[0];
  *out_median = samples[n / 2];
  *out_avg = sum / (F64)n;
}

internal void
bench_print_samples(char* engine, char* phase, F64* samples, U64 n)
{
  printf("  samples %s %s (ms, run order):", engine, phase);
  for (U64 i = 0; i < n; i++)
  {
    printf(" %.3f", samples[i]);
  }
  printf("\n");
}

// tec: sorts the samples, so print them first if their order matters
internal void
bench_stats_from_samples(Bench_Stats* stats, F64* prepare, F64* execute, F64* fetch, F64* total, U64 n)
{
  bench_reduce(prepare, n, &stats->prepare_min, &stats->prepare_median, &stats->prepare_avg);
  bench_reduce(execute, n, &stats->execute_min, &stats->execute_median, &stats->execute_avg);
  stats->execute_max = execute[n - 1];
  bench_reduce(fetch, n, &stats->fetch_min, &stats->fetch_median, &stats->fetch_avg);
  stats->fetch_max = fetch[n - 1];
  bench_reduce(total, n, &stats->total_min, &stats->total_median, &stats->total_avg);
  stats->total_max = total[n - 1];
}

//~ tec: compute_ql query runner 
// drives tokenize->parse->ir->plan (timed as 'prepare') then 
// plan_execute + result materialization (timed as 'execute')

internal U64
bench_cql_consume_result(PLAN_ExecResult* result, IR_Node* select_node, U64* out_row_count)
{
  U64 checksum = 0;
  U64 result_count = result->is_materialized ? result->materialized.count : result->rows.count;
  IR_Node* select_output_columns = ir_node_find_child(select_node, IR_NodeType_ColumnList);
  
  if (result->supported && select_output_columns)
  {
    Temp scratch = scratch_begin(0, 0);
    
    if (result->is_materialized)
    {
      // tec: resolve the output columns once, not per cell
      U64 output_count = 0;
      for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next)
      {
        output_count += 1;
      }
      PLAN_AggColumn** output_columns = push_array(scratch.arena, PLAN_AggColumn*, Max(output_count, 1));
      U64 output_index = 0;
      for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next)
      {
        String8 name = qe_column_list_item_display_name(scratch.arena, column_node);
        for (U64 c = 0; c < result->materialized.column_count; c++)
        {
          if (str8_match(result->materialized.columns[c].name, name, 0))
          {
            output_columns[output_index] = &result->materialized.columns[c];
            break;
          }
        }
        output_index += 1;
      }
      
      for (U64 i = 0; i < result_count; i++)
      {
        U64 row_hash = BENCH_FNV_OFFSET_BASIS;
        
        for (U64 output = 0; output < output_count; output++)
        {
          PLAN_AggColumn* col = output_columns[output];
          if (!col) continue;
          
          if (col->type == CQL_ColumnType_String8)
          {
            row_hash = bench_fnv_mix_string(row_hash, col->string_values[i]);
          }
          else if (col->type == CQL_ColumnType_U32 || col->type == CQL_ColumnType_U64)
          {
            row_hash = bench_fnv_mix_u64(row_hash, (U64)col->numeric_values[i]);
          }
          else if (col->type == CQL_ColumnType_I32 || col->type == CQL_ColumnType_I64)
          {
            // tec: truncate to S64 (sign-preserving) before reinterpreting as U64
            row_hash = bench_fnv_mix_u64(row_hash, (U64)(S64)col->numeric_values[i]);
          }
          else
          {
            row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round(col->numeric_values[i]));
          }
        }
        
        checksum += row_hash;
      }
    }
    else
    {
      // tec: gather each selected column once instead of one cql_column_get_data/cql_column_get_string
      U64 column_count = 0;
      for (IR_Node* c = select_output_columns->first; c != NULL; c = c->next) column_count++;
      
      typedef struct BenchColGather BenchColGather;
      struct BenchColGather
      {
        B32 resolved;
        U64 table_slot;
        CQL_ColumnType type;
        F64* numeric_values;
        CQL_StringDataChunk strings;
      };
      
      BenchColGather* gathered = push_array(scratch.arena, BenchColGather, Max(column_count, 1));
      
      U64 ci = 0;
      for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next, ci++)
      {
        String8 bare_name = {0};
        U64 table_slot = max_U64;
        CQL_Table* col_table = qe_resolve_column_table(&result->rows, column_node->value, &bare_name, &table_slot);
        if (!col_table) continue;
        
        CQL_Column* column = cql_table_find_column(col_table, bare_name);
        if (!column) continue;
        
        gathered[ci].resolved = 1;
        gathered[ci].table_slot = table_slot;
        gathered[ci].type = column->type;
        
        if (column->type == CQL_ColumnType_String8)
        {
          gathered[ci].strings = qe_gather_string_column(scratch.arena, &result->rows, table_slot, column);
        }
        else
        {
          gathered[ci].numeric_values = qe_gather_numeric_column(scratch.arena, &result->rows, table_slot, column);
        }
      }
      
      for (U64 i = 0; i < result_count; i++)
      {
        U64 row_hash = BENCH_FNV_OFFSET_BASIS;
        
        ci = 0;
        for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next, ci++)
        {
          if (!gathered[ci].resolved) continue;
          
          U64 row_index = result->rows.row_indices[gathered[ci].table_slot][i];
          if (row_index == PLAN_NULL_ROW) continue;
          
          switch (gathered[ci].type)
          {
            case CQL_ColumnType_U32: row_hash = bench_fnv_mix_u64(row_hash, (U32)gathered[ci].numeric_values[i]); break;
            case CQL_ColumnType_U64: row_hash = bench_fnv_mix_u64(row_hash, (U64)gathered[ci].numeric_values[i]); break;
            case CQL_ColumnType_F32: row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round((F32)gathered[ci].numeric_values[i])); break;
            case CQL_ColumnType_F64: row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round(gathered[ci].numeric_values[i])); break;
            case CQL_ColumnType_String8:
            {
              CQL_StringDataChunk* chunk = &gathered[ci].strings;
              U64 start = chunk->offsets[i];
              U64 end = chunk->offsets[i + 1];
              String8 str = str8((U8*)chunk->data + start, end - start);
              row_hash = bench_fnv_mix_string(row_hash, str);
            } break;
            default: break;
          }
        }
        
        checksum += row_hash;
      }
    }
    
    scratch_end(scratch);
  }
  
  *out_row_count = result_count;
  return checksum;
}

// tec: the kernels named in the GPU kernel log since the last gpu_kernel_log_reset,
// "none" when the query ran no kernel at all
internal String8
bench_kernel_log_string(Arena* arena)
{
  if (g_gpu_kernel_log.count == 0)
  {
    return str8_lit("none");
  }
  String8List parts = {0};
  for (U32 i = 0; i < g_gpu_kernel_log.count; i++)
  {
    str8_list_pushf(arena, &parts, "%.*s x%llu", str8_varg(g_gpu_kernel_log.names[i]), g_gpu_kernel_log.uses[i]);
  }
  StringJoin join = { .sep = str8_lit(", ") };
  return str8_list_join(arena, &parts, &join);
}

// tec: every execution of a query, with warmup runs included, is compared with the first one.
// a query on unchanged data has to give the same result each time
global U64 g_bench_cql_unstable_runs = 0;

internal Bench_Stats
bench_run_cql_query(CQL_Database* database, String8 sql_text, U64* out_row_count, U64* out_checksum)
{
  U64 first_checksum = 0;
  B32 have_first_checksum = 0;
  U64 warmup_start = os_now_microseconds();
  for (U64 run = 0; run < BENCH_WARMUP_RUNS || (os_now_microseconds() - warmup_start) < BENCH_WARMUP_MIN_MICROSECONDS; run++)
  {
    Temp scratch = scratch_begin(0, 0);
    Arena* arena = scratch.arena;
    
    SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
    SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
    IR_Query* ir_query = ir_generate_from_ast(arena, ast);
    IR_Node* select_node = ir_query->execution_nodes;
    U64 temp_mark = database->temp_table_count;
    PLAN_ExecResult result = plan_run_select(arena, database, select_node, NULL);
    U64 row_count = 0;
    U64 warmup_checksum = bench_cql_consume_result(&result, select_node, &row_count);
    cql_database_release_temp_tables_from(database, temp_mark);
    if (!have_first_checksum)
    {
      first_checksum = warmup_checksum;
      have_first_checksum = 1;
    }
    else if (warmup_checksum != first_checksum)
    {
      g_bench_cql_unstable_runs += 1;
      printf("  !! UNSTABLE: warm-up run %llu gave checksum %llu, the first run gave %llu\n", run, warmup_checksum, first_checksum);
    }
    
    scratch_end(scratch);
  }
  
  F64 prepare_samples[BENCH_TIMED_RUNS];
  F64 execute_samples[BENCH_TIMED_RUNS];
  F64 fetch_samples[BENCH_TIMED_RUNS];
  F64 total_samples[BENCH_TIMED_RUNS];
  U64 last_row_count = 0;
  U64 last_checksum = 0;
  
  for (U64 run = 0; run < BENCH_TIMED_RUNS; run++)
  {
    Temp scratch = scratch_begin(0, 0);
    Arena* arena = scratch.arena;
    
    U64 t0 = os_now_microseconds();
    
    SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
    SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
    IR_Query* ir_query = ir_generate_from_ast(arena, ast);
    IR_Node* select_node = ir_query->execution_nodes;
    
    U64 t1 = os_now_microseconds();
    
    // tec: CTEs and derived tables run (and are timed) inside plan_run_select. a row set result is gathered column by column while it is read, so that gather is fetch time
    U64 temp_mark = database->temp_table_count;
    PLAN_ExecResult result = plan_run_select(arena, database, select_node, NULL);
    
    U64 t2 = os_now_microseconds();
    
    U64 row_count = 0;
    U64 checksum = bench_cql_consume_result(&result, select_node, &row_count);
    cql_database_release_temp_tables_from(database, temp_mark);
    
    U64 t3 = os_now_microseconds();
    
    prepare_samples[run] = (F64)(t1 - t0) / 1000.0;
    execute_samples[run] = (F64)(t2 - t1) / 1000.0;
    fetch_samples[run] = (F64)(t3 - t2) / 1000.0;
    total_samples[run] = (F64)(t3 - t0) / 1000.0;
    last_row_count = row_count;
    last_checksum = checksum;
    if (checksum != first_checksum)
    {
      g_bench_cql_unstable_runs += 1;
      printf("  !! UNSTABLE: timed run %llu gave checksum %llu, the first run gave %llu\n", run, checksum, first_checksum);
    }
    
    scratch_end(scratch);
  }
  
  if (g_bench_print_samples)
  {
    bench_print_samples("cql", "execute", execute_samples, BENCH_TIMED_RUNS);
    bench_print_samples("cql", "fetch", fetch_samples, BENCH_TIMED_RUNS);
    bench_print_samples("cql", "total", total_samples, BENCH_TIMED_RUNS);
  }
  Bench_Stats stats = {0};
  bench_stats_from_samples(&stats, prepare_samples, execute_samples, fetch_samples, total_samples, BENCH_TIMED_RUNS);
  
  *out_row_count = last_row_count;
  *out_checksum = last_checksum;
  return stats;
}

//~ tec: sqlite query runner
// sqlite3_prepare_v2 is timed as 'prepare', 
// the sqlite3_step loop (reading every column, same as compute_ql's materialization loop) is timed as 'execute'
internal U64
bench_sqlite_consume_result(sqlite3_stmt* stmt, U64* out_row_count)
{
  U64 checksum = 0;
  U64 row_count = 0;
  int col_count = sqlite3_column_count(stmt);
  
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    U64 row_hash = BENCH_FNV_OFFSET_BASIS;
    
    for (int c = 0; c < col_count; c++)
    {
      int col_type = sqlite3_column_type(stmt, c);
      if (col_type == SQLITE_TEXT)
      {
        const unsigned char* text = sqlite3_column_text(stmt, c);
        int len = sqlite3_column_bytes(stmt, c);
        row_hash = bench_fnv_mix_string(row_hash, str8((U8*)text, (U64)len));
      }
      else if (col_type == SQLITE_FLOAT)
      {
        row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round(sqlite3_column_double(stmt, c)));
      }
      else
      {
        row_hash = bench_fnv_mix_u64(row_hash, (U64)sqlite3_column_int64(stmt, c));
      }
    }
    
    checksum += row_hash;
    row_count++;
  }
  
  *out_row_count = row_count;
  return checksum;
}

internal Bench_Stats
bench_run_sqlite_query(sqlite3* db, String8 sql_text, U64* out_row_count, U64* out_checksum)
{
  U64 warmup_start = os_now_microseconds();
  for (U64 run = 0; run < BENCH_WARMUP_RUNS || (os_now_microseconds() - warmup_start) < BENCH_WARMUP_MIN_MICROSECONDS; run++)
  {
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, (const char*)sql_text.str, (int)sql_text.size, &stmt, NULL) == SQLITE_OK)
    {
      U64 row_count = 0;
      bench_sqlite_consume_result(stmt, &row_count);
    }
    sqlite3_finalize(stmt);
  }
  
  F64 prepare_samples[BENCH_TIMED_RUNS];
  F64 execute_samples[BENCH_TIMED_RUNS];
  F64 fetch_samples[BENCH_TIMED_RUNS] = {0};
  F64 total_samples[BENCH_TIMED_RUNS];
  U64 last_row_count = 0;
  U64 last_checksum = 0;
  
  for (U64 run = 0; run < BENCH_TIMED_RUNS; run++)
  {
    U64 t0 = os_now_microseconds();
    
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, (const char*)sql_text.str, (int)sql_text.size, &stmt, NULL);
    
    U64 t1 = os_now_microseconds();
    
    U64 row_count = 0;
    U64 checksum = 0;
    if (rc == SQLITE_OK)
    {
      checksum = bench_sqlite_consume_result(stmt, &row_count);
    }
    
    U64 t2 = os_now_microseconds();
    
    sqlite3_finalize(stmt);
    
    prepare_samples[run] = (F64)(t1 - t0) / 1000.0;
    execute_samples[run] = (F64)(t2 - t1) / 1000.0;
    total_samples[run] = (F64)(t2 - t0) / 1000.0;
    last_row_count = row_count;
    last_checksum = checksum;
  }
  
  Bench_Stats stats = {0};
  bench_stats_from_samples(&stats, prepare_samples, execute_samples, fetch_samples, total_samples, BENCH_TIMED_RUNS);
  stats.fetch_included = 1;
  
  *out_row_count = last_row_count;
  *out_checksum = last_checksum;
  return stats;
}

//~ tec: duckdb query runner
// duckdb_prepare/duckdb_execute_prepared are timed as 'prepare'/'execute', mirroring the sqlite runner above
internal B32 bench_duckdb_type_has_chunk_path(duckdb_type type);
internal U64 bench_duckdb_mix_cell(U64 row_hash, duckdb_type type, void* data, U64* validity, U64 row);
internal U64 bench_duckdb_consume_result(duckdb_result* result, U64* out_row_count);
internal U64 bench_duckdb_consume_result_by_value(duckdb_result* result, U64* out_row_count);

internal B32
bench_duckdb_type_has_chunk_path(duckdb_type type)
{
  switch (type)
  {
    case DUCKDB_TYPE_BOOLEAN:
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_UTINYINT:
    case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER:
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
    case DUCKDB_TYPE_VARCHAR:
    {
      return 1;
    }
    default:
    {
      return 0;
    }
  }
}

internal U64
bench_duckdb_mix_cell(U64 row_hash, duckdb_type type, void* data, U64* validity, U64 row)
{
  B32 valid = validity == NULL || duckdb_validity_row_is_valid(validity, row);
  if (type == DUCKDB_TYPE_VARCHAR)
  {
    if (valid)
    {
      duckdb_string_t* text = &((duckdb_string_t*)data)[row];
      U32 length = duckdb_string_t_length(*text);
      const char* bytes = duckdb_string_t_data(text);
      row_hash = bench_fnv_mix_string(row_hash, str8((U8*)bytes, length));
    }
    return row_hash;
  }
  if (type == DUCKDB_TYPE_FLOAT || type == DUCKDB_TYPE_DOUBLE)
  {
    F64 value = 0.0;
    if (valid)
    {
      value = (type == DUCKDB_TYPE_FLOAT) ? (F64)((F32*)data)[row] : ((F64*)data)[row];
    }
    return bench_fnv_mix_u64(row_hash, bench_scaled_round(value));
  }
  S64 value = 0;
  if (valid)
  {
    switch (type)
    {
      case DUCKDB_TYPE_BOOLEAN:   { value = ((U8*)data)[row] ? 1 : 0; } break;
      case DUCKDB_TYPE_TINYINT:   { value = ((S8*)data)[row]; } break;
      case DUCKDB_TYPE_SMALLINT:  { value = ((S16*)data)[row]; } break;
      case DUCKDB_TYPE_INTEGER:   { value = ((S32*)data)[row]; } break;
      case DUCKDB_TYPE_BIGINT:    { value = ((S64*)data)[row]; } break;
      case DUCKDB_TYPE_UTINYINT:  { value = ((U8*)data)[row]; } break;
      case DUCKDB_TYPE_USMALLINT: { value = ((U16*)data)[row]; } break;
      case DUCKDB_TYPE_UINTEGER:  { value = ((U32*)data)[row]; } break;
      case DUCKDB_TYPE_UBIGINT:   { value = (S64)((U64*)data)[row]; } break;
      case DUCKDB_TYPE_HUGEINT:   { value = (S64)((duckdb_hugeint*)data)[row].lower; } break;
      default: break;
    }
  }
  return bench_fnv_mix_u64(row_hash, (U64)value);
}

internal U64
bench_duckdb_consume_result(duckdb_result* result, U64* out_row_count)
{
  U64 col_count = duckdb_column_count(result);
  for (U64 col = 0; col < col_count; col++)
  {
    if (!bench_duckdb_type_has_chunk_path(duckdb_column_type(result, col)))
    {
      return bench_duckdb_consume_result_by_value(result, out_row_count);
    }
  }
  
  Temp scratch = scratch_begin(0, 0);
  duckdb_type* types = push_array(scratch.arena, duckdb_type, Max(col_count, 1));
  void** data = push_array(scratch.arena, void*, Max(col_count, 1));
  U64** validity = push_array(scratch.arena, U64*, Max(col_count, 1));
  for (U64 col = 0; col < col_count; col++)
  {
    types[col] = duckdb_column_type(result, col);
  }
  
  U64 checksum = 0;
  U64 row_count = 0;
  for (;;)
  {
    duckdb_data_chunk chunk = duckdb_fetch_chunk(*result);
    if (!chunk)
    {
      break;
    }
    U64 chunk_rows = duckdb_data_chunk_get_size(chunk);
    for (U64 col = 0; col < col_count; col++)
    {
      duckdb_vector vector = duckdb_data_chunk_get_vector(chunk, col);
      data[col] = duckdb_vector_get_data(vector);
      validity[col] = duckdb_vector_get_validity(vector);
    }
    for (U64 row = 0; row < chunk_rows; row++)
    {
      U64 row_hash = BENCH_FNV_OFFSET_BASIS;
      for (U64 col = 0; col < col_count; col++)
      {
        row_hash = bench_duckdb_mix_cell(row_hash, types[col], data[col], validity[col], row);
      }
      checksum += row_hash;
    }
    row_count += chunk_rows;
    duckdb_destroy_data_chunk(&chunk);
  }
  
  scratch_end(scratch);
  *out_row_count = row_count;
  return checksum;
}

internal U64
bench_duckdb_consume_result_by_value(duckdb_result* result, U64* out_row_count)
{
  U64 checksum = 0;
  U64 row_count = duckdb_row_count(result);
  U64 col_count = duckdb_column_count(result);
  
  for (U64 row = 0; row < row_count; row++)
  {
    U64 row_hash = BENCH_FNV_OFFSET_BASIS;
    
    for (U64 col = 0; col < col_count; col++)
    {
      duckdb_type col_type = duckdb_column_type(result, col);
      if (col_type == DUCKDB_TYPE_VARCHAR)
      {
        char* text = duckdb_value_varchar(result, col, row);
        if (text)
        {
          row_hash = bench_fnv_mix_string(row_hash, str8_cstring(text));
          duckdb_free(text);
        }
      }
      else if (col_type == DUCKDB_TYPE_FLOAT || col_type == DUCKDB_TYPE_DOUBLE)
      {
        row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round(duckdb_value_double(result, col, row)));
      }
      else
      {
        row_hash = bench_fnv_mix_u64(row_hash, (U64)duckdb_value_int64(result, col, row));
      }
    }
    
    checksum += row_hash;
  }
  
  *out_row_count = row_count;
  return checksum;
}

internal Bench_Stats
bench_run_duckdb_query(duckdb_connection conn, String8 sql_text, U64* out_row_count, U64* out_checksum)
{
  U64 warmup_start = os_now_microseconds();
  for (U64 run = 0; run < BENCH_WARMUP_RUNS || (os_now_microseconds() - warmup_start) < BENCH_WARMUP_MIN_MICROSECONDS; run++)
  {
    duckdb_prepared_statement stmt = NULL;
    if (duckdb_prepare(conn, (const char*)sql_text.str, &stmt) == DuckDBSuccess)
    {
      duckdb_result result = {0};
      if (duckdb_execute_prepared(stmt, &result) == DuckDBSuccess)
      {
        U64 row_count = 0;
        bench_duckdb_consume_result(&result, &row_count);
      }
      duckdb_destroy_result(&result);
    }
    duckdb_destroy_prepare(&stmt);
  }
  
  F64 prepare_samples[BENCH_TIMED_RUNS];
  F64 execute_samples[BENCH_TIMED_RUNS];
  F64 fetch_samples[BENCH_TIMED_RUNS];
  F64 total_samples[BENCH_TIMED_RUNS];
  U64 last_row_count = 0;
  U64 last_checksum = 0;
  
  for (U64 run = 0; run < BENCH_TIMED_RUNS; run++)
  {
    U64 t0 = os_now_microseconds();
    
    duckdb_prepared_statement stmt = NULL;
    duckdb_state rc = duckdb_prepare(conn, (const char*)sql_text.str, &stmt);
    
    U64 t1 = os_now_microseconds();
    
    duckdb_result result = {0};
    B32 executed = rc == DuckDBSuccess && duckdb_execute_prepared(stmt, &result) == DuckDBSuccess;
    
    U64 t2 = os_now_microseconds();
    
    U64 row_count = 0;
    U64 checksum = 0;
    if (executed)
    {
      checksum = bench_duckdb_consume_result(&result, &row_count);
    }
    
    U64 t3 = os_now_microseconds();
    
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&stmt);
    
    prepare_samples[run] = (F64)(t1 - t0) / 1000.0;
    execute_samples[run] = (F64)(t2 - t1) / 1000.0;
    fetch_samples[run] = (F64)(t3 - t2) / 1000.0;
    total_samples[run] = (F64)(t3 - t0) / 1000.0;
    last_row_count = row_count;
    last_checksum = checksum;
  }
  
  if (g_bench_print_samples)
  {
    bench_print_samples("duckdb", "execute", execute_samples, BENCH_TIMED_RUNS);
    bench_print_samples("duckdb", "fetch", fetch_samples, BENCH_TIMED_RUNS);
    bench_print_samples("duckdb", "total", total_samples, BENCH_TIMED_RUNS);
  }
  Bench_Stats stats = {0};
  bench_stats_from_samples(&stats, prepare_samples, execute_samples, fetch_samples, total_samples, BENCH_TIMED_RUNS);
  
  *out_row_count = last_row_count;
  *out_checksum = last_checksum;
  return stats;
}

//~ tec: sqlite bulk load helper

internal void
bench_sqlite_create_schema(sqlite3* db, String8 table_name)
{
  Temp scratch = scratch_begin(0, 0);
  String8 sql = push_str8f(scratch.arena, "CREATE TABLE %.*s (id INTEGER, name TEXT, value REAL);", str8_varg(table_name));
  char* err = NULL;
  if (sqlite3_exec(db, (const char*)sql.str, NULL, NULL, &err) != SQLITE_OK)
  {
    log_error("sqlite3_exec (CREATE TABLE) failed: %s", err ? err : "unknown error");
    sqlite3_free(err);
  }
  scratch_end(scratch);
}

internal void
bench_sqlite_bulk_insert(sqlite3* db, String8 table_name, Bench_Row* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  
  sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  
  String8 insert_sql = push_str8f(scratch.arena, "INSERT INTO %.*s (id, name, value) VALUES (?, ?, ?);", str8_varg(table_name));
  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(db, (const char*)insert_sql.str, (int)insert_sql.size, &stmt, NULL);
  
  for (U64 i = 0; i < row_count; i++)
  {
    sqlite3_bind_int64(stmt, 1, (S64)rows[i].id);
    sqlite3_bind_text(stmt, 2, (const char*)rows[i].name.str, (int)rows[i].name.size, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, rows[i].value);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  
  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
  
  scratch_end(scratch);
}

//~ tec: duckdb bulk load helper

internal void
bench_duckdb_create_schema(duckdb_connection conn, String8 table_name)
{
  Temp scratch = scratch_begin(0, 0);
  String8 sql = push_str8f(scratch.arena, "CREATE TABLE %.*s (id INTEGER, name VARCHAR, value DOUBLE);", str8_varg(table_name));
  duckdb_result result = {0};
  if (duckdb_query(conn, (const char*)sql.str, &result) != DuckDBSuccess)
  {
    log_error("duckdb_query (CREATE TABLE) failed for '%.*s': %s", str8_varg(table_name), duckdb_result_error(&result));
  }
  duckdb_destroy_result(&result);
  scratch_end(scratch);
}

internal void
bench_duckdb_bulk_insert(duckdb_connection conn, String8 table_name, Bench_Row* rows, U64 row_count)
{
  Temp scratch = scratch_begin(0, 0);
  char* table_cstr = (char*)push_str8_copy(scratch.arena, table_name).str;
  
  duckdb_appender appender = NULL;
  if (duckdb_appender_create(conn, NULL, table_cstr, &appender) == DuckDBSuccess)
  {
    for (U64 i = 0; i < row_count; i++)
    {
      duckdb_append_int32(appender, (S32)rows[i].id);
      duckdb_append_varchar_length(appender, (const char*)rows[i].name.str, rows[i].name.size);
      duckdb_append_double(appender, rows[i].value);
      duckdb_appender_end_row(appender);
    }
  }
  duckdb_appender_destroy(&appender);
  
  scratch_end(scratch);
}

//~ tec: markdown report

typedef struct Bench_Report Bench_Report;
struct Bench_Report
{
  Arena* arena;
  String8List lines;
  String8List row_cells;
  U64 row_cell_count;
  B32 in_table;
  String8List deferred_lines;
};

internal Bench_Report*
bench_report_alloc(Arena* arena, char* title)
{
  Bench_Report* report = push_array(arena, Bench_Report, 1);
  report->arena = arena;
  
  DateTime universal = os_now_universal_time();
  DateTime local = os_local_time_from_universal(&universal);
  String8 timestamp = push_date_time_string(arena, &local);
  
  str8_list_pushf(arena, &report->lines, "# %s\n\n", title);
  str8_list_pushf(arena, &report->lines, "generated %.*s\n\n", str8_varg(timestamp));
  return report;
}

// tec: a markdown table ends at the first non row line, so text ends it with a blank line and warnings wait until the table is complete
internal void
bench_report_table_close(Bench_Report* report)
{
  if (!report->in_table)
  {
    return;
  }
  str8_list_pushf(report->arena, &report->lines, "\n");
  for (String8Node* n = report->deferred_lines.first; n != NULL; n = n->next)
  {
    str8_list_push(report->arena, &report->lines, n->string);
  }
  report->deferred_lines = (String8List){0};
  report->in_table = 0;
}

internal void
bench_report_section(Bench_Report* report, char* fmt, ...)
{
  bench_report_table_close(report);
  va_list args;
  va_start(args, fmt);
  String8 title = push_str8fv(report->arena, fmt, args);
  va_end(args);
  str8_list_pushf(report->arena, &report->lines, "\n## %.*s\n\n", str8_varg(title));
}

internal void
bench_report_text(Bench_Report* report, char* fmt, ...)
{
  bench_report_table_close(report);
  va_list args;
  va_start(args, fmt);
  String8 text = push_str8fv(report->arena, fmt, args);
  va_end(args);
  str8_list_pushf(report->arena, &report->lines, "%.*s\n\n", str8_varg(text));
}

internal void
bench_report_warn(Bench_Report* report, char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  String8 text = push_str8fv(report->arena, fmt, args);
  va_end(args);
  String8List* destination = report->in_table ? &report->deferred_lines : &report->lines;
  str8_list_pushf(report->arena, destination, "> :warning: %.*s\n\n", str8_varg(text));
}

internal void
bench_report_row_begin(Bench_Report* report)
{
  report->row_cells = (String8List){0};
  report->row_cell_count = 0;
}

internal void
bench_report_cellf(Bench_Report* report, char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  String8 cell = push_str8fv(report->arena, fmt, args);
  va_end(args);
  str8_list_push(report->arena, &report->row_cells, cell);
  report->row_cell_count += 1;
}

internal void
bench_report_row_end(Bench_Report* report)
{
  str8_list_pushf(report->arena, &report->lines, "|");
  for (String8Node* n = report->row_cells.first; n != NULL; n = n->next)
  {
    str8_list_pushf(report->arena, &report->lines, " %.*s |", str8_varg(n->string));
  }
  str8_list_pushf(report->arena, &report->lines, "\n");
  report->in_table = 1;
}

// tec: call instead of bench_report_row_end for the header row
// also emits the markdown separator row required right after it
internal void
bench_report_table_header_end(Bench_Report* report)
{
  U64 column_count = report->row_cell_count;
  bench_report_table_close(report);
  bench_report_row_end(report);
  
  str8_list_pushf(report->arena, &report->lines, "|");
  for (U64 i = 0; i < column_count; i++)
  {
    str8_list_pushf(report->arena, &report->lines, " --- |");
  }
  str8_list_pushf(report->arena, &report->lines, "\n");
}

internal void
bench_report_write(Bench_Report* report, String8 path)
{
  bench_report_table_close(report);
  if (!os_write_data_list_to_file_path(path, report->lines))
  {
    log_error("failed to write bench report to '%.*s'", str8_varg(path));
  }
  else
  {
    printf("\nreport written to %.*s\n", str8_varg(path));
  }
}

//~ tec: console + report table helpers

internal void
bench_print_table_header(Bench_Report* report, char* title)
{
  printf("\n=== %s ===\n", title);
  printf("exec is the engine up to a readable result, fetch is reading every cell of it, total = prep + exec + fetch. sqlite cannot split the two, its fetch is inside exec\n");
  printf("%-28s %-8s %10s %10s %10s %10s %10s %10s %10s %10s %10s %10s %10s\n",
         "query", "engine", "rows", "checksum",
         "prep med",
         "exec min", "exec med", "exec max",
         "fetch med",
         "tot min", "tot med", "tot avg", "tot max");
  
  bench_report_row_begin(report);
  bench_report_cellf(report, "query");
  bench_report_cellf(report, "engine");
  bench_report_cellf(report, "rows");
  bench_report_cellf(report, "checksum");
  bench_report_cellf(report, "prep med (ms)");
  bench_report_cellf(report, "exec min (ms)");
  bench_report_cellf(report, "exec med (ms)");
  bench_report_cellf(report, "exec max (ms)");
  bench_report_cellf(report, "fetch med (ms)");
  bench_report_cellf(report, "total min (ms)");
  bench_report_cellf(report, "total med (ms)");
  bench_report_cellf(report, "total avg (ms)");
  bench_report_cellf(report, "total max (ms)");
  bench_report_table_header_end(report);
}

internal void
bench_print_table_row(Bench_Report* report, String8 query_label, char* engine, U64 row_count, U64 checksum, Bench_Stats* s)
{
  char fetch_text[32];
  if (s->fetch_included)
  {
    snprintf(fetch_text, sizeof(fetch_text), "in exec");
  }
  else
  {
    snprintf(fetch_text, sizeof(fetch_text), "%.4f", s->fetch_median);
  }
  
  printf("%-28.*s %-8s %10llu %10llu %10.4f %10.4f %10.4f %10.4f %10s %10.4f %10.4f %10.4f %10.4f\n",
         str8_varg(query_label), engine, row_count, checksum,
         s->prepare_median,
         s->execute_min, s->execute_median, s->execute_max,
         fetch_text,
         s->total_min, s->total_median, s->total_avg, s->total_max);
  
  bench_report_row_begin(report);
  bench_report_cellf(report, "%.*s", str8_varg(query_label));
  bench_report_cellf(report, "%s", engine);
  bench_report_cellf(report, "%llu", row_count);
  bench_report_cellf(report, "%llu", checksum);
  bench_report_cellf(report, "%.4f", s->prepare_median);
  bench_report_cellf(report, "%.4f", s->execute_min);
  bench_report_cellf(report, "%.4f", s->execute_median);
  bench_report_cellf(report, "%.4f", s->execute_max);
  bench_report_cellf(report, "%s", fetch_text);
  bench_report_cellf(report, "%.4f", s->total_min);
  bench_report_cellf(report, "%.4f", s->total_median);
  bench_report_cellf(report, "%.4f", s->total_avg);
  bench_report_cellf(report, "%.4f", s->total_max);
  bench_report_row_end(report);
}

internal void
bench_check_match(Bench_Report* report, String8 query_label,
                  char* base_engine, U64 base_rows, U64 base_checksum,
                  char* other_engine, U64 other_rows, U64 other_checksum)
{
  if (base_rows != other_rows || base_checksum != other_checksum)
  {
    printf("  !! MISMATCH on '%.*s': %s rows=%llu checksum=%llu vs %s rows=%llu checksum=%llu\n",
           str8_varg(query_label), base_engine, base_rows, base_checksum, other_engine, other_rows, other_checksum);
    bench_report_warn(report, "MISMATCH on '%.*s': %s rows=%llu checksum=%llu vs %s rows=%llu checksum=%llu",
                      str8_varg(query_label), base_engine, base_rows, base_checksum, other_engine, other_rows, other_checksum);
  }
}

//~ tec: shared query suite runner

typedef struct Bench_QueryCase Bench_QueryCase;
struct Bench_QueryCase
{
  String8 label;
  String8 cql_sql;
  String8 sqlite_sql;
  String8 duckdb_sql;
};

internal void
bench_run_query_suite(Arena* arena, Bench_Report* report, U64 row_count, char* label)
{
  printf("\n########## query suite: %s (%llu rows) ##########\n", label, row_count);
  bench_report_section(report, "query suite: %s (%llu rows)", label, row_count);
  
  String8 table_name = str8_lit("bench");
  Bench_Row* rows = bench_generate_rows(arena, row_count);
  
  Temp scratch = scratch_begin(&arena, 1);
  String8 csv_path = push_str8f(scratch.arena, "bench_data/query_%s.csv", label);
  if (!os_file_path_exists(str8_lit("bench_data/")))
  {
    os_make_directory(str8_lit("bench_data/"));
  }
  bench_write_csv(csv_path, rows, row_count);
  
  //- tec: seed compute_ql (in memory database, no USE/CREATE DATABASE SQL, no disk round trip)
  CQL_Database* database = cql_database_alloc(str8_lit("bench_db"));
  cql_add_database(database);
  CQL_Table* table = cql_table_import_csv_streaming(database, table_name, csv_path);
  cql_database_add_table(database, table);
  
  //- tec: seed sqlite with the exact same rows
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  bench_sqlite_create_schema(sqlite_db, table_name);
  bench_sqlite_bulk_insert(sqlite_db, table_name, rows, row_count);
  
  //- tec: seed duckdb with the exact same rows
  duckdb_database duckdb_db = NULL;
  duckdb_connection duckdb_conn = NULL;
  duckdb_open(NULL, &duckdb_db);
  duckdb_connect(duckdb_db, &duckdb_conn);
  bench_duckdb_create_schema(duckdb_conn, table_name);
  bench_duckdb_bulk_insert(duckdb_conn, table_name, rows, row_count);
  
  //- tec: build the 4 query cases from the actual generated data so theyre always resolvable
  U64 mid_index = row_count / 2;
  U32 mid_id = rows[mid_index].id;
  U32 threshold_id = (U32)((row_count * 9) / 10);
  String8 target_name = rows[mid_index].name;
  String8 substr = str8_cstring(g_bench_words[3]); // "delta"
  
  Bench_QueryCase cases[4];
  cases[0].label = str8_lit("id = (int equality)");
  cases[0].cql_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id = %u;", str8_varg(table_name), mid_id);
  cases[0].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id = %u;", str8_varg(table_name), mid_id);
  cases[0].duckdb_sql = cases[0].sqlite_sql;
  
  cases[1].label = str8_lit("id > (int range)");
  cases[1].cql_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id > %u;", str8_varg(table_name), threshold_id);
  cases[1].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id > %u;", str8_varg(table_name), threshold_id);
  cases[1].duckdb_sql = cases[1].sqlite_sql;
  
  cases[2].label = str8_lit("name = (string equality)");
  cases[2].cql_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name = '%.*s';", str8_varg(table_name), str8_varg(target_name));
  cases[2].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name = '%.*s';", str8_varg(table_name), str8_varg(target_name));
  cases[2].duckdb_sql = cases[2].sqlite_sql;
  
  cases[3].label = str8_lit("name contains (string search)");
  cases[3].cql_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name contains '%.*s';", str8_varg(table_name), str8_varg(substr));
  cases[3].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name LIKE '%%%.*s%%';", str8_varg(table_name), str8_varg(substr));
  cases[3].duckdb_sql = cases[3].sqlite_sql; // tec: duckdb supports the same LIKE syntax as sqlite
  
  bench_print_table_header(report, label);
  for (U64 i = 0; i < ArrayCount(cases); i++)
  {
    U64 cql_rows = 0, cql_checksum = 0;
    Bench_Stats cql_stats = bench_run_cql_query(database, cases[i].cql_sql, &cql_rows, &cql_checksum);
    bench_print_table_row(report, cases[i].label, "cql", cql_rows, cql_checksum, &cql_stats);
    
    U64 sqlite_rows = 0, sqlite_checksum = 0;
    Bench_Stats sqlite_stats = bench_run_sqlite_query(sqlite_db, cases[i].sqlite_sql, &sqlite_rows, &sqlite_checksum);
    bench_print_table_row(report, cases[i].label, "sqlite", sqlite_rows, sqlite_checksum, &sqlite_stats);
    
    U64 duckdb_rows = 0, duckdb_checksum = 0;
    Bench_Stats duckdb_stats = bench_run_duckdb_query(duckdb_conn, cases[i].duckdb_sql, &duckdb_rows, &duckdb_checksum);
    bench_print_table_row(report, cases[i].label, "duckdb", duckdb_rows, duckdb_checksum, &duckdb_stats);
    
    bench_check_match(report, cases[i].label, "cql", cql_rows, cql_checksum, "sqlite", sqlite_rows, sqlite_checksum);
    bench_check_match(report, cases[i].label, "cql", cql_rows, cql_checksum, "duckdb", duckdb_rows, duckdb_checksum);
  }
  
  sqlite3_close(sqlite_db);
  duckdb_disconnect(&duckdb_conn);
  duckdb_close(&duckdb_db);
  scratch_end(scratch);
}

#endif //BENCH_COMMON_H
