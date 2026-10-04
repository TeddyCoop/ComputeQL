/* date = January 25th 2025 11:47 am */

#ifndef BASE_LOG_H
#define BASE_LOG_H

internal void log_alloc(void);
internal void log_release(void);

typedef enum LogLevel
{
  LogLevel_Error,
  LogLevel_Warn,
  LogLevel_Info,
  LogLevel_Verbose,
  LogLevel_COUNT,
} LogLevel;

internal void log_set_level(LogLevel level);
internal B32 log_level_from_string(String8 string, LogLevel *out_level);
internal void log_logf(LogLevel level, const char *file, int line, const char* fmt, ...);
internal U64 log_error_count(void);
internal String8 log_last_error(Arena* arena);

#define log_info(fmt, ...)  log_logf(LogLevel_Info,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_logf(LogLevel_Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...) log_logf(LogLevel_Verbose, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log_logf(LogLevel_Warn,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_time(fmt, ...)  log_logf(LogLevel_Verbose,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
//#define log_data(fmt, ...)  log_logf("DATA",  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define log_time_block(name, block) do { \
U64 ___start = os_now_microseconds(); \
block; \
log_time("'%s' took %llu ms", name, (U64)(os_now_microseconds() - ___start) / 1000); \
} while (0)

#endif //BASE_LOG_H
