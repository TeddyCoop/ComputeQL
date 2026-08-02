#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

//~ tec: shared benchmark harness for comparing compute_ql against sqlite.
/*
methodology: for every timed operation we do 1 discarded warm-up run followed by BENCH_TIMED_RUNS recorded runs, 
and report min/median/avg per phase. row count + an order independent per row checksum are compared between engines as a correctness sanity check
the checksum sums a per row FNV-1a hash across rows, since compute_ql and sqlite are not guaranteed to return matching row order for the same query
*/

#define BENCH_WARMUP_RUNS 1
#define BENCH_TIMED_RUNS  20

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
// decimal text in two different engines cant flip the low digit and cause a false mismatch
internal U64
bench_scaled_round(F64 v)
{
  return (U64)(v * 100.0 + (v >= 0 ? 0.5 : -0.5));
}

//~ tec: timing stats

typedef struct Bench_Stats Bench_Stats;
struct Bench_Stats
{
  F64 prepare_min, prepare_median, prepare_avg;
  F64 execute_min, execute_median, execute_avg;
  F64 total_min, total_median, total_avg;
};

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

//~ tec: compute_ql query runner 
// drives tokenize->parse->ir->plan (timed as 'prepare') then 
// plan_execute + result materialization (timed as 'execute')

internal U64
bench_gdb_consume_result(PLAN_ExecResult* result, IR_Node* select_node, U64* out_row_count)
{
  U64 checksum = 0;
  U64 result_count = result->is_materialized ? result->materialized.count : result->rows.count;
  IR_Node* select_output_columns = ir_node_find_child(select_node, IR_NodeType_ColumnList);

  if (result->supported && select_output_columns)
  {
    Temp scratch = scratch_begin(0, 0);

    for (U64 i = 0; i < result_count; i++)
    {
      U64 row_hash = BENCH_FNV_OFFSET_BASIS;

      for (IR_Node* column_node = select_output_columns->first; column_node != NULL; column_node = column_node->next)
      {
        if (result->is_materialized)
        {
          String8 name = qe_column_list_item_display_name(scratch.arena, column_node);
          PLAN_AggColumn* col = NULL;
          for (U64 c = 0; c < result->materialized.column_count; c++)
          {
            if (str8_match(result->materialized.columns[c].name, name, 0))
            {
              col = &result->materialized.columns[c];
              break;
            }
          }
          if (!col) continue;

          if (col->type == GDB_ColumnType_String8)
          {
            row_hash = bench_fnv_mix_string(row_hash, col->string_values[i]);
          }
          else
          {
            row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round(col->numeric_values[i]));
          }
        }
        else
        {
          String8 bare_name = {0};
          GDB_Table* col_table = qe_resolve_column_table(&result->rows, column_node->value, &bare_name);
          if (!col_table) continue;

          U64 table_slot = qe_rowset_table_slot(&result->rows, col_table);
          U64 row_index = result->rows.row_indices[table_slot][i];
          if (row_index == PLAN_NULL_ROW) continue;

          GDB_Column* column = gdb_table_find_column(col_table, bare_name);
          void* data = gdb_column_get_data(column, row_index);

          switch (column->type)
          {
            case GDB_ColumnType_U32: row_hash = bench_fnv_mix_u64(row_hash, *(U32*)data); break;
            case GDB_ColumnType_U64: row_hash = bench_fnv_mix_u64(row_hash, *(U64*)data); break;
            case GDB_ColumnType_F32: row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round(*(F32*)data)); break;
            case GDB_ColumnType_F64: row_hash = bench_fnv_mix_u64(row_hash, bench_scaled_round(*(F64*)data)); break;
            case GDB_ColumnType_String8:
            {
              String8 str = gdb_column_get_string(scratch.arena, column, row_index);
              row_hash = bench_fnv_mix_string(row_hash, str);
            } break;
            default: break;
          }
        }
      }

      checksum += row_hash;
    }

    scratch_end(scratch);
  }

  *out_row_count = result_count;
  return checksum;
}

internal Bench_Stats
bench_run_gdb_query(GDB_Database* database, String8 sql_text, U64* out_row_count, U64* out_checksum)
{
  for (U64 run = 0; run < BENCH_WARMUP_RUNS; run++)
  {
    Temp scratch = scratch_begin(0, 0);
    Arena* arena = scratch.arena;

    SQL_TokenizeResult tok = sql_tokenize_from_text(arena, sql_text);
    SQL_Node* ast = sql_parse(arena, tok.tokens, tok.count, sql_text);
    IR_Query* ir_query = ir_generate_from_ast(arena, ast);
    IR_Node* select_node = ir_query->execution_nodes;
    ir_expand_star_to_columns(arena, database, select_node);
    PLAN_Node* plan = plan_build_from_select(arena, database, select_node);
    PLAN_ExecResult result = plan_execute(arena, database, plan, select_node);
    U64 row_count = 0;
    bench_gdb_consume_result(&result, select_node, &row_count);

    scratch_end(scratch);
  }

  F64 prepare_samples[BENCH_TIMED_RUNS];
  F64 execute_samples[BENCH_TIMED_RUNS];
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
    ir_expand_star_to_columns(arena, database, select_node);
    PLAN_Node* plan = plan_build_from_select(arena, database, select_node);

    U64 t1 = os_now_microseconds();

    PLAN_ExecResult result = plan_execute(arena, database, plan, select_node);
    U64 row_count = 0;
    U64 checksum = bench_gdb_consume_result(&result, select_node, &row_count);

    U64 t2 = os_now_microseconds();

    prepare_samples[run] = (F64)(t1 - t0) / 1000.0;
    execute_samples[run] = (F64)(t2 - t1) / 1000.0;
    total_samples[run] = (F64)(t2 - t0) / 1000.0;
    last_row_count = row_count;
    last_checksum = checksum;

    scratch_end(scratch);
  }

  Bench_Stats stats = {0};
  bench_reduce(prepare_samples, BENCH_TIMED_RUNS, &stats.prepare_min, &stats.prepare_median, &stats.prepare_avg);
  bench_reduce(execute_samples, BENCH_TIMED_RUNS, &stats.execute_min, &stats.execute_median, &stats.execute_avg);
  bench_reduce(total_samples, BENCH_TIMED_RUNS, &stats.total_min, &stats.total_median, &stats.total_avg);

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
  for (U64 run = 0; run < BENCH_WARMUP_RUNS; run++)
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
  bench_reduce(prepare_samples, BENCH_TIMED_RUNS, &stats.prepare_min, &stats.prepare_median, &stats.prepare_avg);
  bench_reduce(execute_samples, BENCH_TIMED_RUNS, &stats.execute_min, &stats.execute_median, &stats.execute_avg);
  bench_reduce(total_samples, BENCH_TIMED_RUNS, &stats.total_min, &stats.total_median, &stats.total_avg);

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

//~ tec: console report table

internal void
bench_print_table_header(char* title)
{
  printf("\n=== %s ===\n", title);
  printf("%-28s %-8s %10s %10s %10s %10s %10s %10s %10s %10s %10s\n",
         "query", "engine", "rows", "checksum",
         "prep min", "prep med", "prep avg",
         "exec min", "exec med", "exec avg",
         "tot avg");
}

internal void
bench_print_table_row(String8 query_label, char* engine, U64 row_count, U64 checksum, Bench_Stats* s)
{
  printf("%-28.*s %-8s %10llu %10llu %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f\n",
         str8_varg(query_label), engine, row_count, checksum,
         s->prepare_min, s->prepare_median, s->prepare_avg,
         s->execute_min, s->execute_median, s->execute_avg,
         s->total_avg);
}

internal void
bench_check_match(String8 query_label, U64 gdb_rows, U64 gdb_checksum, U64 sqlite_rows, U64 sqlite_checksum)
{
  if (gdb_rows != sqlite_rows || gdb_checksum != sqlite_checksum)
  {
    printf("  !! MISMATCH on '%.*s': compute_ql rows=%llu checksum=%llu vs sqlite rows=%llu checksum=%llu\n",
           str8_varg(query_label), gdb_rows, gdb_checksum, sqlite_rows, sqlite_checksum);
  }
}

//~ tec: shared query suite runner

typedef struct Bench_QueryCase Bench_QueryCase;
struct Bench_QueryCase
{
  String8 label;
  String8 gdb_sql;
  String8 sqlite_sql;
};

internal void
bench_run_query_suite(Arena* arena, U64 row_count, char* label)
{
  printf("\n########## query suite: %s (%llu rows) ##########\n", label, row_count);

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
  GDB_Database* database = gdb_database_alloc(str8_lit("bench_db"));
  gdb_add_database(database);
  GDB_Table* table = gdb_table_import_csv_streaming(database, table_name, csv_path);
  gdb_database_add_table(database, table);

  //- tec: seed sqlite with the exact same rows
  sqlite3* sqlite_db = NULL;
  sqlite3_open(":memory:", &sqlite_db);
  bench_sqlite_create_schema(sqlite_db, table_name);
  bench_sqlite_bulk_insert(sqlite_db, table_name, rows, row_count);

  //- tec: build the 4 query cases from the actual generated data so theyre always resolvable
  U64 mid_index = row_count / 2;
  U32 mid_id = rows[mid_index].id;
  U32 threshold_id = (U32)((row_count * 9) / 10);
  String8 target_name = rows[mid_index].name;
  String8 substr = str8_cstring(g_bench_words[3]); // "delta"

  Bench_QueryCase cases[4];
  cases[0].label = str8_lit("id = (int equality)");
  cases[0].gdb_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id = %u;", str8_varg(table_name), mid_id);
  cases[0].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id = %u;", str8_varg(table_name), mid_id);

  cases[1].label = str8_lit("id > (int range)");
  cases[1].gdb_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id > %u;", str8_varg(table_name), threshold_id);
  cases[1].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE id > %u;", str8_varg(table_name), threshold_id);

  cases[2].label = str8_lit("name = (string equality)");
  cases[2].gdb_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name = '%.*s';", str8_varg(table_name), str8_varg(target_name));
  cases[2].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name = '%.*s';", str8_varg(table_name), str8_varg(target_name));

  cases[3].label = str8_lit("name contains (string search)");
  cases[3].gdb_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name contains '%.*s';", str8_varg(table_name), str8_varg(substr));
  cases[3].sqlite_sql = push_str8f(scratch.arena, "SELECT * FROM %.*s WHERE name LIKE '%%%.*s%%';", str8_varg(table_name), str8_varg(substr));

  bench_print_table_header(label);
  for (U64 i = 0; i < ArrayCount(cases); i++)
  {
    U64 gdb_rows = 0, gdb_checksum = 0;
    Bench_Stats gdb_stats = bench_run_gdb_query(database, cases[i].gdb_sql, &gdb_rows, &gdb_checksum);
    bench_print_table_row(cases[i].label, "gdb", gdb_rows, gdb_checksum, &gdb_stats);

    U64 sqlite_rows = 0, sqlite_checksum = 0;
    Bench_Stats sqlite_stats = bench_run_sqlite_query(sqlite_db, cases[i].sqlite_sql, &sqlite_rows, &sqlite_checksum);
    bench_print_table_row(cases[i].label, "sqlite", sqlite_rows, sqlite_checksum, &sqlite_stats);

    bench_check_match(cases[i].label, gdb_rows, gdb_checksum, sqlite_rows, sqlite_checksum);
  }

  sqlite3_close(sqlite_db);
  scratch_end(scratch);
}

#endif //BENCH_COMMON_H
