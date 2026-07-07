/* date = January 25th 2025 11:47 am */

#ifndef BASE_LOG_H
#define BASE_LOG_H

internal void log_alloc(void);
internal void log_release(void);

internal void log_logf(const char* level, const char *file, int line, const char* fmt, ...);

#define log_info(fmt, ...)  log_logf("INFO",  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_logf("ERROR", __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...) log_logf("DEBUG", __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log_logf("WARN",  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_time(fmt, ...)  log_logf("TIME",  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
//#define log_data(fmt, ...)  log_logf("DATA",  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define log_time_block(name, block) do { \
U64 ___start = os_now_microseconds(); \
block; \
log_time("'%s' took %llu ms", name, (U64)(os_now_microseconds() - ___start) / 1000); \
} while (0)

#endif //BASE_LOG_H
