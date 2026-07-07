internal void
tp_run_tasks(TP_Context *pool, TP_Worker *worker)
{
  while (1)
  {
    S64 task_left = ins_atomic_u64_dec_eval(&pool->task_left);
    if (task_left < 0)
    {
      break;
    }
    
    Arena *arena = pool->task_arena ? pool->task_arena->v[worker->id] : 0;
    U64 task_id = pool->task_count - (task_left+1);
    pool->task_func(arena, worker->id, task_id, pool->task_data);
    
    U64 task_count = pool->task_count;
    
    U64 task_done = ins_atomic_u64_inc_eval(&pool->task_done);
    if (task_done == task_count) 
    {
      os_semaphore_drop(pool->main_semaphore);
    }
  }
}

internal void
tp_worker_main(void *raw_worker)
{
  TCTX tctx_;
  tctx_init_and_equip(&tctx_);
  TP_Worker *worker = raw_worker;
  TP_Context *pool = worker->pool;
  while (pool->is_live) 
  {
    if (os_semaphore_take(pool->task_semaphore, max_U64)) 
    {
      tp_run_tasks(pool, worker);
    }
  }
}

internal void
tp_worker_main_shared(void *raw_worker)
{
  TCTX tctx_;
  tctx_init_and_equip(&tctx_);
  TP_Worker  *worker = raw_worker;
  TP_Context *pool = worker->pool;
  while (pool->is_live)
  {
    if (os_semaphore_take(pool->exec_semaphore, max_U64))
    {
      if (os_semaphore_take(pool->task_semaphore, max_U64))
      {
        tp_run_tasks(pool, worker);
      }
    }
  }
}

internal TP_Context * 
tp_alloc(Arena *arena, U32 worker_count, U32 max_worker_count, String8 name)
{
  AssertAlways(worker_count > 0);
  
  B32 is_shared = (name.size > 0);
  
  OS_Handle main_semaphore = {0};
  OS_Handle task_semaphore = {0};
  OS_Handle exec_semaphore = {0};
  if (worker_count > 1)
  {
    main_semaphore = os_semaphore_alloc(0, 1, str8_zero());
    if (is_shared) 
    {
      AssertAlways(worker_count <= max_worker_count);
      task_semaphore = os_semaphore_alloc(0, max_worker_count, name);
      exec_semaphore = os_semaphore_alloc(0, worker_count, str8_zero());
    }
    else 
    {
      task_semaphore = os_semaphore_alloc(0, worker_count, str8_zero());
    }
  }
  
  void *worker_entry = is_shared ? tp_worker_main_shared : tp_worker_main;
  
  TP_Context *pool = push_array(arena, TP_Context, 1);
  pool->exec_semaphore = exec_semaphore;
  pool->task_semaphore = task_semaphore;
  pool->main_semaphore = main_semaphore;
  pool->is_live = 1;
  pool->worker_count = worker_count;
  pool->worker_arr = push_array(arena, TP_Worker, worker_count);
  
  for (U64 i = 0; i < worker_count; i += 1) 
  {
    TP_Worker *worker = &pool->worker_arr[i];
    worker->id = i;
    worker->pool = pool;
  }
  
  for (U64 i = 1; i < worker_count; i += 1) 
  {
    TP_Worker *worker = &pool->worker_arr[i];
    worker->handle = os_thread_launch(worker_entry, worker, 0);
  }
  
  return pool;
}

internal void
tp_release(TP_Context *pool)
{
  pool->is_live = 0;
  
  B32 is_shared = !os_handle_match(pool->exec_semaphore, os_handle_zero());
  if (is_shared) 
  {
    for (U64 i = 0; i < pool->worker_count; ++i)
    {
      os_semaphore_drop(pool->exec_semaphore);
    }
  }
  for (U64 i = 0; i < pool->worker_count; ++i) 
  {
    os_semaphore_drop(pool->task_semaphore);
  }
  for (U64 i = 1; i < pool->worker_count; i += 1) 
  {
    os_thread_detach(pool->worker_arr[i].handle);
  }
  if (is_shared)
  {
    os_semaphore_release(pool->exec_semaphore);
  }
  os_semaphore_release(pool->task_semaphore);
  os_semaphore_release(pool->main_semaphore);
  
  MemoryZeroStruct(pool);
}

internal TP_Arena *
tp_arena_alloc(TP_Context *pool)
{
  Temp scratch = scratch_begin(0,0);
  Arena **arr = push_array(scratch.arena, Arena *, pool->worker_count);
  for (U64 i = 0; i < pool->worker_count; ++i) 
  {
    arr[i] = arena_alloc(.reserve_size=MB(256), .commit_size=MB(16));
  }
  Arena **dst = push_array(arr[0], Arena *, pool->worker_count);
  MemoryCopy(dst, arr, sizeof(Arena*) * pool->worker_count);
  TP_Arena *worker_arena_arr = push_array(arr[0], TP_Arena, 1);
  worker_arena_arr->count = pool->worker_count;
  worker_arena_arr->v = dst;
  scratch_end(scratch);
  return worker_arena_arr;
}

internal void
tp_arena_release(TP_Arena **arena_ptr)
{
  for (U64 i = 1; i < (*arena_ptr)->count; ++i)
  {
    arena_release((*arena_ptr)->v[i]);
  }
  arena_release((*arena_ptr)->v[0]);
  *arena_ptr = NULL;
}

internal TP_Temp
tp_temp_begin(TP_Arena *arena)
{
  Temp first_temp = temp_begin(arena->v[0]);
  
  TP_Temp temp;
  temp.count = arena->count;
  temp.v = push_array_no_zero(first_temp.arena, Temp, arena->count);
  
  temp.v[0] = first_temp;
  
  for (U64 arena_idx = 1; arena_idx < arena->count; arena_idx += 1)
  {
    temp.v[arena_idx] = temp_begin(arena->v[arena_idx]);
  }
  
  return temp;
}

internal void
tp_temp_end(TP_Temp temp)
{
  for (U64 temp_idx = temp.count - 1; temp_idx > 0; temp_idx -= 1) 
  {
    temp_end(temp.v[temp_idx]);
  }
}

internal void
tp_for_parallel(TP_Context *pool, TP_Arena *task_arena, U64 task_count, TP_TaskFunc *task_func, void *task_data)
{
  if (task_count > 0) 
  {
    pool->task_arena = task_arena;
    pool->task_func = task_func;
    pool->task_data = task_data;
    pool->task_count = task_count;
    pool->task_done = 0;
    ins_atomic_u64_eval_assign(&pool->task_left, task_count);
    
    U64 drop_count = Min(task_count, pool->worker_count);
    
    // tec: if in shared mode ping the local semaphore
    if (!os_handle_match(pool->exec_semaphore, os_handle_zero())) 
    {
      for (U64 worker_idx = 0; worker_idx < drop_count; worker_idx +=1)
      {
        os_semaphore_drop(pool->exec_semaphore);
      }
    }
    
    // tec: ping shared
    for (U64 worker_idx = 0; worker_idx < drop_count; worker_idx += 1)
    {
      os_semaphore_drop(pool->task_semaphore);
    }
    
    tp_run_tasks(pool, &pool->worker_arr[0]);
    
    os_semaphore_take(pool->main_semaphore, max_U64);
  }
}

internal Rng1U64 *
tp_divide_work(Arena *arena, U64 item_count, U32 worker_count)
{
  U64 per_count = CeilIntegerDiv(item_count, worker_count);
  Rng1U64 *range_arr = push_array_no_zero(arena, Rng1U64, worker_count + 1);
  for (U64 i = 0; i < worker_count; i += 1) 
  {
    range_arr[i] = rng_1u64(Min(item_count, i * per_count), 
                            Min(item_count, i * per_count + per_count));
  }
  
  range_arr[worker_count] = rng_1u64(item_count, item_count);
  
  return range_arr;
}