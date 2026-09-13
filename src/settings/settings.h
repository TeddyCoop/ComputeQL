#ifndef SETTINGS_H
#define SETTINGS_H

typedef struct Settings_Entry Settings_Entry;
struct Settings_Entry
{
  Settings_Entry* hash_next;
  String8 key;
  String8 value;
};

typedef struct Settings_State Settings_State;
struct Settings_State
{
  Arena* arena;
  U64 slot_count;
  Settings_Entry** slots;
};

global Settings_State* g_settings_state = 0;

internal void settings_init(void);
internal void settings_load_from_file(String8 path);
internal void settings_load_from_string(String8 text);

internal Settings_Entry** settings_slot_from_key(String8 key_lower);
internal void settings_set(String8 key, String8 value);

internal B32     settings_try_get(String8 key, String8* out_value);
internal String8 settings_string(String8 key, String8 default_value);
internal U64     settings_u64(String8 key, U64 default_value);
internal F64     settings_f64(String8 key, F64 default_value);
internal B32     settings_bool(String8 key, B32 default_value);

#endif //SETTINGS_H
