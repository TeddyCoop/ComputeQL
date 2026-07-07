/* date = July 26th 2024 1:21 pm */

#ifndef BASE_PROFILE_H
#define BASE_PROFILE_H

////////////////////////////////
//~ tec: Zero Settings

#if !defined(PROFILE_CUSTOM)
# define PROFILE_CUSTOM 0
#endif

#if !defined(MARKUP_LAYER_COLOR)
# define MARKUP_LAYER_COLOR 1.00f, 0.00f, 1.00f
#endif

////////////////////////////////
//~ tec: Third Party Includes

//~ tec: custom
#if PROFILE_CUSTOM
typedef enum 
{
  ProfEventType_Begin,
  ProfEventType_End,
  ProfEventType_Msg
} ProfEventType;

typedef struct ProfEvent ProfEvent;
typedef struct ProfState ProfState;


global ProfState* g_prof = 0;

internal void prof_alloc(void);
internal void prof_release(void);
internal void prof_json_dump(void);
internal void prof_record_event(ProfEventType type, const char *name, const char *file, int line);

#define ProfBegin(...) do {                                   \
char buf[256];                                            \
snprintf(buf, sizeof(buf), __VA_ARGS__);                  \
prof_record_event(ProfEventType_Begin, buf, __FILE__, __LINE__); \
} while (0)

# define ProfBeginDynamic(...) \
char buf[256];                                            \
snprintf(buf, sizeof(buf), __VA_ARGS__);                  \
prof_record_event(ProfEventType_Begin, buf, __FILE__, __LINE__); \
} while (0)

#define ProfEnd() do {                                   \
prof_record_event(ProfEventType_End, NULL, __FILE__, __LINE__); \
} while (0)

# define ProfTick(...) (0)
# define ProfIsCapturing(...) (0)
# define ProfBeginCapture(...) prof_alloc();
# define ProfEndCapture(...) prof_release();
# define ProfThreadName(...) (0)
# define ProfMsg(...) (0)
# define ProfBeginLockWait(...)    (0)
# define ProfEndLockWait(...)      (0)
# define ProfLockTake(...)         (0)
# define ProfLockDrop(...)         (0)
# define ProfColor(...)            (0)
#endif

////////////////////////////////
//~ tec: Zeroify Undefined Defines

#if !defined(ProfBegin)
# define ProfBegin(...) (0)
# define ProfBeginDynamic(...) (0)
# define ProfEnd(...) (0)
# define ProfTick(...) (0)
# define ProfIsCapturing(...) (0)
# define ProfBeginCapture(...) (0)
# define ProfEndCapture(...) (0)
# define ProfThreadName(...) (0)
# define ProfMsg(...) (0)
# define ProfBeginLockWait(...)    (0)
# define ProfEndLockWait(...)      (0)
# define ProfLockTake(...)         (0)
# define ProfLockDrop(...)         (0)
# define ProfColor(...)            (0)
#endif

////////////////////////////////
//~ tec: Helper Wrappers

#define ProfBeginFunction() ProfBegin(this_function_name)
#define ProfScope(...) DeferLoop(ProfBeginDynamic(__VA_ARGS__), ProfEnd())

#endif //BASE_PROFILE_H
