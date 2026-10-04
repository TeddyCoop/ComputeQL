typedef struct Log Log;
struct Log
{
  Arena* arena;
  OS_Handle lock_mutex;
  OS_Handle log_file;
  U64 log_file_offset;

  LogLevel console_level;

  U64 error_count;
  U8 last_error_buf[1024];
  U64 last_error_size;
};

global Log* g_log = 0;

global char *g_log_level_names[LogLevel_COUNT] =
{
  "ERROR",
  "WARN",
  "INFO",
  "VERBOSE",
};

internal void
log_alloc(void)
{
  Arena* arena = arena_alloc();
  g_log = push_array(arena, Log, 1);
  g_log->arena = arena;
  g_log->console_level = LogLevel_Info;

  g_log->lock_mutex = os_rw_mutex_alloc();
  g_log->log_file = os_file_open(OS_AccessFlag_Write|OS_AccessFlag_ShareRead|OS_AccessFlag_ShareWrite, str8_lit("log.txt"));
}

internal void
log_release(void)
{
  os_file_close(g_log->log_file);
  os_mutex_release(g_log->lock_mutex);
  arena_release(g_log->arena);
}

internal void
log_set_level(LogLevel level)
{
  if (g_log)
  {
    g_log->console_level = level;
  }
}

internal B32
log_level_from_string(String8 string, LogLevel *out_level)
{
  B32 result = 0;
  for (U64 i = 0; i < LogLevel_COUNT; i += 1)
  {
    if (str8_match(string, str8_cstring(g_log_level_names[i]), StringMatchFlag_CaseInsensitive))
    {
      *out_level = (LogLevel)i;
      result = 1;
      break;
    }
  }
  return result;
}

internal void
log_logf(LogLevel level, const char *file, int line, const char* fmt, ...)
{
  OS_MutexScopeW(g_log->lock_mutex)
  {
    U8 buffer[2048];

    DateTime time = os_now_universal_time();

    U64 offset = 0;
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%02d:%02d:%02d ", time.hour, time.min, time.sec);
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s %s:%d: ", g_log_level_names[level], file, line);

    U64 message_offset = offset;

    va_list args;
    va_start(args, fmt);
    offset += vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, args);
    va_end(args);

    if (level == LogLevel_Error)
    {
      U64 message_size = offset - message_offset;
      message_size = Min(message_size, sizeof(g_log->last_error_buf));
      MemoryCopy(g_log->last_error_buf, buffer + message_offset, message_size);
      g_log->last_error_size = message_size;
      g_log->error_count += 1;
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n");

    if (!os_handle_match(os_handle_zero(), g_log->log_file))
    {
      os_file_write(g_log->log_file, r1u64(g_log->log_file_offset, g_log->log_file_offset + offset), buffer);
      g_log->log_file_offset += offset;
    }

    // tec: log.txt always gets everything, the level only filters the console
    if (level <= g_log->console_level)
    {
      fprintf(stderr, "%s", buffer);
    }
  }
}

internal U64
log_error_count(void)
{
  return g_log ? g_log->error_count : 0;
}

internal String8
log_last_error(Arena* arena)
{
  if (!g_log || g_log->last_error_size == 0) 
  {
    return str8_lit(""); 
  }
  return push_str8_copy(arena, str8(g_log->last_error_buf, g_log->last_error_size));
}