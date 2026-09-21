/* date = July 26th 2024 7:15 pm */

#ifndef OS_CORE_WIN32_H
#define OS_CORE_WIN32_H

////////////////////////////////
//~ tec: Includes / Libraries

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <Shlobj.h>
#include <processthreadsapi.h>
#pragma comment(lib, "user32")
#pragma comment(lib, "winmm")
#pragma comment(lib, "shell32")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "rpcrt4")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "comctl32")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"") // this is required for loading correct comctl32 dll file

////////////////////////////////
//~ tec: File Iterator Types

typedef struct OS_W32_FileIter OS_W32_FileIter;
struct OS_W32_FileIter
{
  HANDLE handle;
  WIN32_FIND_DATAW find_data;
  B32 is_volume_iter;
  String8Array drive_strings;
  U64 drive_strings_iter_idx;
};
StaticAssert(sizeof(Member(OS_FileIter, memory)) >= sizeof(OS_W32_FileIter), file_iter_memory_size);

////////////////////////////////
//~ tec: Entity Types

typedef enum OS_W32_EntityKind
{
  OS_W32_EntityKind_Null,
  OS_W32_EntityKind_Thread,
  OS_W32_EntityKind_Mutex,
  OS_W32_EntityKind_RWMutex,
  OS_W32_EntityKind_ConditionVariable,
}
OS_W32_EntityKind;

typedef struct OS_W32_Entity OS_W32_Entity;
struct OS_W32_Entity
{
  OS_W32_Entity *next;
  OS_W32_EntityKind kind;
  union
  {
    struct
    {
      OS_ThreadFunctionType *func;
      void *ptr;
      HANDLE handle;
      DWORD tid;
    } thread;
    CRITICAL_SECTION mutex;
    SRWLOCK rw_mutex;
    CONDITION_VARIABLE cv;
  };
};

////////////////////////////////
//~ tec: State

typedef struct OS_W32_State OS_W32_State;
struct OS_W32_State
{
  Arena *arena;
  
  // tec: info
  OS_SystemInfo system_info;
  OS_ProcessInfo process_info;
  U64 microsecond_resolution;
  
  // tec: entity storage
  CRITICAL_SECTION entity_mutex;
  Arena *entity_arena;
  OS_W32_Entity *entity_free;
};

////////////////////////////////
//~ tec: Globals

global OS_W32_State os_w32_state = {0};

////////////////////////////////
//~ tec: File Info Conversion Helpers

internal FilePropertyFlags os_w32_file_property_flags_from_dwFileAttributes(DWORD dwFileAttributes);
internal void os_w32_file_properties_from_attribute_data(FileProperties *properties, WIN32_FILE_ATTRIBUTE_DATA *attributes);
internal B32 os_file_set_time(OS_Handle file, DateTime time);

////////////////////////////////
//~ tec: Time Conversion Helpers

internal void os_w32_date_time_from_system_time(DateTime *out, SYSTEMTIME *in);
internal void os_w32_system_time_from_date_time(SYSTEMTIME *out, DateTime *in);
internal void os_w32_dense_time_from_file_time(DenseTime *out, FILETIME *in);
internal U32 os_w32_sleep_ms_from_endt_us(U64 endt_us);

////////////////////////////////
//~ tec: Entity Functions

internal OS_W32_Entity *os_w32_entity_alloc(OS_W32_EntityKind kind);
internal void os_w32_entity_release(OS_W32_Entity *entity);

////////////////////////////////
//~ tec: Thread Entry Point

internal DWORD os_w32_thread_entry_point(void *ptr);

////////////////////////////////
//~ tec: Modern Windows SDK Functions
//
// (We must dynamically link to them, since they can be missing in older SDKs)

typedef HRESULT W32_SetThreadDescription_Type(HANDLE hThread, PCWSTR lpThreadDescription);
global W32_SetThreadDescription_Type *w32_SetThreadDescription_func = 0;

////////////////////////////////
//~ tec: Thread Naming

#pragma pack(push,8)
typedef struct THREADNAME_INFO THREADNAME_INFO;
struct THREADNAME_INFO
{
  U32 dwType;     // Must be 0x1000.
  char *szName;   // Pointer to name (in user addr space).
  U32 dwThreadID; // Thread ID (-1=caller thread).
  U32 dwFlags;    // Reserved for future use, must be zero.
};
#pragma pack(pop)

////////////////////////////////
//~ tec: Crash Handling

global B32 win32_g_is_quiet = 0;

internal HRESULT WINAPI win32_dialog_callback(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LONG_PTR data);
internal LONG WINAPI win32_exception_filter(EXCEPTION_POINTERS* exception_ptrs);

////////////////////////////////
//~ tec: Entry Point

internal void w32_entry_point_caller(int argc, WCHAR **wargv);

#endif //OS_CORE_WIN32_H
