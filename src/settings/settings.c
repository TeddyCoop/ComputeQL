internal void
settings_init(void)
{
  Arena* arena = arena_alloc(.reserve_size=MB(1), .commit_size=KB(16));
  g_settings_state = push_array(arena, Settings_State, 1);
  g_settings_state->arena = arena;
  g_settings_state->slot_count = 256;
  g_settings_state->slots = push_array(arena, Settings_Entry*, g_settings_state->slot_count);
  
  settings_set(str8_lit("GPU_VULKAN_MAX_CACHED_KERNELS"), str8_lit("16"));
  settings_set(str8_lit("GPU_VULKAN_MAX_POOLED_BUFFERS"), str8_lit("128"));
  settings_set(str8_lit("GPU_VULKAN_POOLED_BUFFER_HASH_SLOTS"), str8_lit("256"));
  settings_set(str8_lit("GPU_BATCH_MAX_PENDING_READS"), str8_lit("8"));
  settings_set(str8_lit("GPU_VULKAN_MEM_BLOCK_SIZE"), str8_lit("1mb"));
  settings_set(str8_lit("GPU_VULKAN_MAX_MEM_BLOCKS"), str8_lit("64"));
  settings_set(str8_lit("QE_BYTECODE_MAX_WORDS"), str8_lit("4096"));
  settings_set(str8_lit("QE_MAX_NUMERIC_CONSTS"), str8_lit("256"));
  settings_set(str8_lit("QE_STRING_CONST_POOL_SIZE"), str8_lit("64kb"));
  settings_set(str8_lit("QE_SCAN_OUTPUT_DEFAULT_CAP_ROWS"), str8_lit("65536"));
  settings_set(str8_lit("QE_AGG_ROWS_PER_CHUNK"), str8_lit("4096"));
  settings_set(str8_lit("QE_INDEX_SCAN_MAX_AND_LEAVES"), str8_lit("16"));
  settings_set(str8_lit("APP_THREAD_POOL_WORKER_COUNT"), str8_lit("0"));
}

internal Settings_Entry**
settings_slot_from_key(String8 key_lower)
{
  U64 hash = u64_hash_from_str8(key_lower);
  return &g_settings_state->slots[hash % g_settings_state->slot_count];
}

internal void
settings_set(String8 key, String8 value)
{
  String8 key_lower = lower_from_str8(g_settings_state->arena, key);
  Settings_Entry** slot = settings_slot_from_key(key_lower);
  
  for (Settings_Entry* entry = *slot; entry != 0; entry = entry->hash_next)
  {
    if (str8_match(entry->key, key_lower, 0))
    {
      entry->value = push_str8_copy(g_settings_state->arena, value);
      return;
    }
  }
  
  Settings_Entry* entry = push_array(g_settings_state->arena, Settings_Entry, 1);
  entry->key = key_lower;
  entry->value = push_str8_copy(g_settings_state->arena, value);
  entry->hash_next = *slot;
  *slot = entry;
}

internal B32
settings_try_get(String8 key, String8* out_value)
{
  if (!g_settings_state) return 0;
  
  Temp scratch = scratch_begin(0, 0);
  String8 key_lower = lower_from_str8(scratch.arena, key);
  Settings_Entry** slot = settings_slot_from_key(key_lower);
  
  B32 found = 0;
  for (Settings_Entry* entry = *slot; entry != 0; entry = entry->hash_next)
  {
    if (str8_match(entry->key, key_lower, 0))
    {
      *out_value = entry->value;
      found = 1;
      break;
    }
  }
  
  scratch_end(scratch);
  return found;
}

internal void
settings_load_from_string(String8 text)
{
  if (!g_settings_state) { settings_init(); }
  
  String8List lines = str8_split_by_string_chars(g_settings_state->arena, text, str8_lit("\n"), 0);
  
  for (String8Node* line_node = lines.first; line_node != 0; line_node = line_node->next)
  {
    String8 line = str8_skip_chop_whitespace(line_node->string);
    if (line.size > 0 && line.str[line.size - 1] == '\r')
    {
      line = str8_chop(line, 1);
    }
    
    if (line.size == 0) continue;
    if (line.str[0] == '#') continue;
    if (line.size >= 2 && line.str[0] == '/' && line.str[1] == '/') continue;
    
    U64 colon_pos = str8_find_needle(line, 0, str8_lit(":"), 0);
    if (colon_pos >= line.size) continue;
    
    String8 key = str8_skip_chop_whitespace(str8_prefix(line, colon_pos));
    String8 value = str8_skip_chop_whitespace(str8_skip(line, colon_pos + 1));
    
    B32 is_quoted = value.size >= 2 && (value.str[0] == '"' || value.str[0] == '\'') && value.str[value.size - 1] == value.str[0];
    
    if (is_quoted)
    {
      value = str8_chop(str8_skip(value, 1), 1);
    }
    else
    {
      // tec: strip a trailing '# ...' or '// ...' comment off an unquoted value
      U64 comment_pos = str8_find_needle(value, 0, str8_lit("#"), 0);
      U64 line_comment_pos = str8_find_needle(value, 0, str8_lit("//"), 0);
      comment_pos = Min(comment_pos, line_comment_pos);
      if (comment_pos < value.size)
      {
        value = str8_skip_chop_whitespace(str8_prefix(value, comment_pos));
      }
    }
    
    if (key.size == 0) continue;
    
    settings_set(key, value);
  }
}

internal void
settings_load_from_file(String8 path)
{
  if (!g_settings_state) { settings_init(); }
  
  if (!os_file_path_exists(path))
  {
    log_info("settings: no settings file at '%.*s', using built-in defaults", str8_varg(path));
    return;
  }
  
  Temp scratch = scratch_begin(0, 0);
  String8 text = os_data_from_file_path(scratch.arena, path);
  settings_load_from_string(text);
  scratch_end(scratch);
  
  log_info("settings: loaded '%.*s'", str8_varg(path));
}

internal String8
settings_string(String8 key, String8 default_value)
{
  String8 value = default_value;
  settings_try_get(key, &value);
  return value;
}

internal U64
settings_u64(String8 key, U64 default_value)
{
  String8 value = {0};
  if (!settings_try_get(key, &value)) return default_value;
  
  String8 trimmed = str8_skip_chop_whitespace(value);
  U64 multiplier = 1;
  
  if (str8_ends_with(trimmed, str8_lit("gb"), StringMatchFlag_CaseInsensitive)) 
  { 
    multiplier = GB(1); 
    trimmed = str8_chop(trimmed, 2);
  }
  else if (str8_ends_with(trimmed, str8_lit("mb"), StringMatchFlag_CaseInsensitive))
  { 
    multiplier = MB(1);
    trimmed = str8_chop(trimmed, 2);
  }
  else if (str8_ends_with(trimmed, str8_lit("kb"), StringMatchFlag_CaseInsensitive)) 
  { 
    multiplier = KB(1); 
    trimmed = str8_chop(trimmed, 2);
  }
  else if (str8_ends_with(trimmed, str8_lit("g"),  StringMatchFlag_CaseInsensitive)) 
  { 
    multiplier = GB(1);
    trimmed = str8_chop(trimmed, 1);
  }
  else if (str8_ends_with(trimmed, str8_lit("m"),  StringMatchFlag_CaseInsensitive)) 
  { 
    multiplier = MB(1);
    trimmed = str8_chop(trimmed, 1); 
  }
  else if (str8_ends_with(trimmed, str8_lit("k"),  StringMatchFlag_CaseInsensitive)) 
  { 
    multiplier = KB(1); 
    trimmed = str8_chop(trimmed, 1);
  }
  
  trimmed = str8_skip_chop_whitespace(trimmed);
  if (!str8_is_integer(trimmed, 10))
  {
    log_error("settings: '%.*s' value '%.*s' is not a valid integer/size, using default", str8_varg(key), str8_varg(value));
    return default_value;
  }
  
  return u64_from_str8(trimmed, 10) * multiplier;
}

internal F64
settings_f64(String8 key, F64 default_value)
{
  String8 value = { 0 };
  if (!settings_try_get(key, &value)) return default_value;
  return f64_from_str8(value);
}

internal B32
settings_bool(String8 key, B32 default_value)
{
  String8 value = { 0 };
  if (!settings_try_get(key, &value)) return default_value;
  
  if (str8_match(value, str8_lit("true"), StringMatchFlag_CaseInsensitive) || 
      str8_match(value, str8_lit("1"), 0))
  {
    return 1;
  }
  if (str8_match(value, str8_lit("false"), StringMatchFlag_CaseInsensitive) ||
      str8_match(value, str8_lit("0"), 0)) 
  {
    return 0;
  }
  
  log_error("settings: '%.*s' value '%.*s' is not a valid boolean, using default", str8_varg(key), str8_varg(value));
  return default_value;
}
