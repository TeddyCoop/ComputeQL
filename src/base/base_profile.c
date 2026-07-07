#if PROFILE_CUSTOM
struct ProfEvent
{
  ProfEventType type;
  U64 timestamp;
  String8 name;
  String8 file;
  U32 line;
  U32 thread_id;
};

struct ProfState
{
  Arena *arena;
  OS_Handle mutex;
  ProfEvent *events;
  U64 event_count;
  U64 event_capacity;
  
  U64 *begin_stack;
  U64 begin_stack_count;
  U64 begin_stack_capacity;
};

internal void
prof_alloc(void)
{
  Arena* arena = arena_alloc();
  g_prof = push_array(arena, ProfState, 1);
  g_prof->arena = arena;
  g_prof->mutex = os_mutex_alloc();
  
  g_prof->event_capacity = 8192*2;
  g_prof->events = push_array(arena, ProfEvent, g_prof->event_capacity);
  g_prof->event_count = 0;
}


internal void
prof_release(void)
{
  os_mutex_release(g_prof->mutex);
  prof_json_dump();
  arena_release(g_prof->arena);
}

internal void 
prof_json_dump()
{
  String8List list = { 0 };
  
  str8_list_push(g_prof->arena, &list, str8_lit("{ \"traceEvents\": [\n"));
  
  for (U64 i = 0; i < g_prof->event_count; i++)
  {
    ProfEvent *e = &g_prof->events[i];
    const char *ph = "";
    switch (e->type)
    {
      case ProfEventType_Begin: ph = "B"; break;
      case ProfEventType_End:   ph = "E"; break;
      case ProfEventType_Msg:   ph = "i"; break;
    }
    
    str8_list_pushf(g_prof->arena, &list, "  { \"name\": \"%.*s\", \"ph\": \"%s\", \"ts\": %llu, \"pid\": 0, \"tid\": %u",
                    str8_varg(e->name), ph, e->timestamp, e->thread_id);
    
    if (e->type == ProfEventType_Msg)
    {
      str8_list_push(g_prof->arena, &list, str8_lit(", \"s\": \"g\""));
    }
    
    str8_list_pushf(g_prof->arena, &list, " }%s\n", (i+1 < g_prof->event_count) ? "," : "");
  }
  
  str8_list_push(g_prof->arena, &list, str8_lit("] }\n"));
  
  os_write_data_list_to_file_path(str8_lit("profile.json"), list);
}

internal void
prof_record_event(ProfEventType type, const char *name, const char *file, int line)
{
  OS_MutexScope(g_prof->mutex)
  {
    
    if (g_prof->event_count >= g_prof->event_capacity) 
    {
      U64 new_capacity = g_prof->event_capacity ? g_prof->event_capacity * 2 : 1024;
      ProfEvent *new_events = push_array(g_prof->arena, ProfEvent, new_capacity);
      
      MemoryCopy(new_events, g_prof->events, sizeof(ProfEvent) * g_prof->event_count);
      
      g_prof->events = new_events;
      g_prof->event_capacity = new_capacity;
    }
    
    U64 idx = g_prof->event_count++;
    ProfEvent *ev = &g_prof->events[idx];
    ev->type = type;
    ev->timestamp = os_now_microseconds();
    ev->thread_id = os_tid();
    
    if (type == ProfEventType_Begin) 
    {
      ev->name = push_str8f(g_prof->arena, "%s", name);
      ev->file = push_str8f(g_prof->arena, "%s", file);
      ev->line = line;
      
      if (g_prof->begin_stack_count >= g_prof->begin_stack_capacity) 
      {
        g_prof->begin_stack_capacity = (g_prof->begin_stack_capacity ? g_prof->begin_stack_capacity * 2 : 128);
        g_prof->begin_stack = push_array(g_prof->arena, U64, g_prof->begin_stack_capacity);
      }
      g_prof->begin_stack[g_prof->begin_stack_count++] = idx;
    }
    else if (type == ProfEventType_End)
    {
      if (g_prof->begin_stack_count == 0)
      {
        ev->name = push_str8_copy(g_prof->arena, str8_lit("MISSING BEGIN"));
        ev->file = push_str8f(g_prof->arena, "%s", file);
        ev->line = line;
      } 
      else 
      {
        U64 begin_idx = g_prof->begin_stack[--g_prof->begin_stack_count];
        ProfEvent *begin_ev = &g_prof->events[begin_idx];
        
        ev->name = begin_ev->name;
        ev->file = begin_ev->file;
        ev->line = begin_ev->line;
      }
    }
    else if (type == ProfEventType_Msg)
    {
      ev->name = push_str8f(g_prof->arena, "%s", name);
      ev->file = push_str8f(g_prof->arena, "%s", file);
      ev->line = line;
    }
  }
}
#endif