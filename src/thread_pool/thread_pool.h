/* date = March 29th 2025 10:42 pm */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#define THREAD_POOL_TASK_FUNC(name) void name(Arena *arena, U64 worker_id, U64 task_id, void *raw_task)
typedef THREAD_POOL_TASK_FUNC(TP_TaskFunc);

typedef struct TP_Arena TP_Arena;
struct TP_Arena
{
  Arena** v;
  U64 count;
};

typedef struct TP_Temp TP_Temp;
struct TP_Temp
{
  Temp* v;
  U64 count;
};

typedef struct TP_Worker TP_Worker;
struct TP_Worker
{
  U64 id;
  OS_Handle handle;
  struct TP_Context* pool;
};

typedef struct TP_Context TP_Context;
struct TP_Context
{
  B32 is_live;
  OS_Handle exec_semaphore;
  OS_Handle task_semaphore;
  OS_Handle main_semaphore;
  
  U32 worker_count;
  TP_Worker *worker_arr;
  
  TP_Arena *task_arena;
  TP_TaskFunc *task_func;
  void *task_data;
  U64 task_count;
  U64 task_done;
  S64 task_left;
};

internal TP_Context * tp_alloc(Arena *arena, U32 worker_count, U32 max_worker_count, String8 name);
internal void         tp_release(TP_Context *pool);
internal TP_Arena *   tp_arena_alloc(TP_Context *pool);
internal void         tp_arena_release(TP_Arena **arena_ptr);
internal TP_Temp      tp_temp_begin(TP_Arena *arena);
internal void         tp_temp_end(TP_Temp temp);
internal void         tp_for_parallel(TP_Context *pool, TP_Arena *arena, U64 task_count, TP_TaskFunc *task_func, void *task_data);
internal Rng1U64 *    tp_divide_work(Arena *arena, U64 item_count, U32 worker_count);

#endif //THREAD_POOL_H
