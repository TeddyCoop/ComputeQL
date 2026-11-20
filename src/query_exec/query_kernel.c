// tec: GLSL helpers for the string and fuzzy operators, copies of the functions in scan_filter.comp. keep them in step with it
global char* qk_glsl_read_col_byte =
  "uint\n"
  "read_col_byte(uint slot, uint byte_index)\n"
  "{\n"
  "  uint word = read_col_word(slot, byte_index / 4u);\n"
  "  uint shift = (byte_index % 4u) * 8u;\n"
  "  return (word >> shift) & 0xffu;\n"
  "}\n"
  ;

global char* qk_glsl_read_str_const_byte =
  "uint\n"
  "read_str_const_byte(uint word_offset, uint byte_index)\n"
  "{\n"
  "  uint word = str_consts[word_offset + byte_index / 4u];\n"
  "  uint shift = (byte_index % 4u) * 8u;\n"
  "  return (word >> shift) & 0xffu;\n"
  "}\n"
  ;

global char* qk_glsl_read_str_offset =
  "uint\n"
  "read_str_offset(uint offsets_slot, uint row)\n"
  "{\n"
  "  return read_col_word(offsets_slot, row * 2u);\n"
  "}\n"
  ;

global char* qk_glsl_str_eq =
  "bool\n"
  "str_eq(uint slot, uint row, uint const_word_offset, uint const_len)\n"
  "{\n"
  "  uint start = read_str_offset(slot + 1u, row);\n"
  "  uint end = read_str_offset(slot + 1u, row + 1u);\n"
  "  uint len = end - start;\n"
  "\n"
  "  if (len != const_len) return false;\n"
  "\n"
  "  for (uint i = 0u; i < len; i++)\n"
  "  {\n"
  "    if (read_col_byte(slot, start + i) != read_str_const_byte(const_word_offset, i))\n"
  "    {\n"
  "      return false;\n"
  "    }\n"
  "  }\n"
  "  return true;\n"
  "}\n"
  ;

global char* qk_glsl_str_cmp3 =
  "int\n"
  "str_cmp3(uint slot, uint row, uint const_word_offset, uint const_len)\n"
  "{\n"
  "  uint start = read_str_offset(slot + 1u, row);\n"
  "  uint end = read_str_offset(slot + 1u, row + 1u);\n"
  "  uint len = end - start;\n"
  "\n"
  "  uint min_len = min(len, const_len);\n"
  "  for (uint i = 0u; i < min_len; i++)\n"
  "  {\n"
  "    uint a = read_col_byte(slot, start + i);\n"
  "    uint b = read_str_const_byte(const_word_offset, i);\n"
  "    if (a != b) \n"
  "    {\n"
  "      return (a < b) ? -1 : 1;\n"
  "    }\n"
  "  }\n"
  "  if (len < const_len) return -1;\n"
  "  if (len > const_len) return 1;\n"
  "  return 0;\n"
  "}\n"
  ;

global char* qk_glsl_str_contains =
  "bool\n"
  "str_contains(uint slot, uint row, uint const_word_offset, uint const_len)\n"
  "{\n"
  "  uint start = read_str_offset(slot + 1u, row);\n"
  "  uint end = read_str_offset(slot + 1u, row + 1u);\n"
  "  uint len = end - start;\n"
  "\n"
  "  if (const_len == 0u) return true;\n"
  "  if (len > MAX_SANE_STRING_LEN) return false;\n"
  "  if (const_len > len) return false;\n"
  "\n"
  "  for (uint window = 0u; window <= len - const_len; window++)\n"
  "  {\n"
  "    bool match = true;\n"
  "    for (uint i = 0u; i < const_len; i++)\n"
  "    {\n"
  "      if (read_col_byte(slot, start + window + i) != read_str_const_byte(const_word_offset, i))\n"
  "      {\n"
  "        match = false;\n"
  "        break;\n"
  "      }\n"
  "    }\n"
  "    if (match) return true;\n"
  "  }\n"
  "  return false;\n"
  "}\n"
  ;

global char* qk_glsl_trigram =
  "double\n"
  "trigram_similarity(uint slot, uint row, uint const_word_offset, uint const_len, uint n)\n"
  "{\n"
  "  uint start = read_str_offset(slot + 1u, row);\n"
  "  uint end = read_str_offset(slot + 1u, row + 1u);\n"
  "  uint len = end - start;\n"
  "\n"
  "  if (len < n || const_len < n)\n"
  "  {\n"
  "    if (len != const_len) return 0.0LF;\n"
  "    for (uint i = 0u; i < len; i++)\n"
  "    {\n"
  "      if (read_col_byte(slot, start + i) != read_str_const_byte(const_word_offset, i))\n"
  "      {\n"
  "        return 0.0LF;\n"
  "      }\n"
  "    }\n"
  "    return 1.0LF;\n"
  "  }\n"
  "  if (len > MAX_SANE_STRING_LEN) return 0.0LF;\n"
  "\n"
  "  uint needle_grams = min(const_len - n + 1u, QE_FUZZY_MAX_NEEDLE_LEN);\n"
  "  bool needle_used[QE_FUZZY_MAX_NEEDLE_LEN];\n"
  "  for (uint j = 0u; j < needle_grams; j++) needle_used[j] = false;\n"
  "\n"
  "  uint haystack_grams = len - n + 1u;\n"
  "  uint shared_grams = 0u;\n"
  "  for (uint i = 0u; i < haystack_grams; i++)\n"
  "  {\n"
  "    for (uint j = 0u; j < needle_grams; j++)\n"
  "    {\n"
  "      if (needle_used[j]) continue;\n"
  "      bool match = true;\n"
  "      for (uint k = 0u; k < n; k++)\n"
  "      {\n"
  "        if (read_col_byte(slot, start + i + k) != read_str_const_byte(const_word_offset, j + k))\n"
  "        {\n"
  "          match = false;\n"
  "          break;\n"
  "        }\n"
  "      }\n"
  "      if (match)\n"
  "      {\n"
  "        needle_used[j] = true;\n"
  "        shared_grams++;\n"
  "        break;\n"
  "      }\n"
  "    }\n"
  "  }\n"
  "\n"
  "  uint union_size = haystack_grams + needle_grams - shared_grams;\n"
  "  return (union_size == 0u) ? 0.0LF : double(shared_grams) / double(union_size);\n"
  "}\n"
  ;

global char* qk_glsl_edit_distance =
  "double\n"
  "edit_distance(uint slot, uint row, uint const_word_offset, uint const_len)\n"
  "{\n"
  "  uint start = read_str_offset(slot + 1u, row);\n"
  "  uint end = read_str_offset(slot + 1u, row + 1u);\n"
  "  uint len = end - start;\n"
  "\n"
  "  if (len > MAX_SANE_STRING_LEN) return double(MAX_SANE_STRING_LEN);\n"
  "\n"
  "  uint needle_len = min(const_len, QE_FUZZY_MAX_NEEDLE_LEN);\n"
  "  uint dp[QE_FUZZY_MAX_NEEDLE_LEN + 1u];\n"
  "  for (uint j = 0u; j <= needle_len; j++) dp[j] = j;\n"
  "\n"
  "  for (uint i = 1u; i <= len; i++)\n"
  "  {\n"
  "    uint prev_diag = dp[0];\n"
  "    dp[0] = i;\n"
  "    uint haystack_byte = read_col_byte(slot, start + i - 1u);\n"
  "    for (uint j = 1u; j <= needle_len; j++)\n"
  "    {\n"
  "      uint tmp = dp[j];\n"
  "      uint needle_byte = read_str_const_byte(const_word_offset, j - 1u);\n"
  "      uint cost = (haystack_byte == needle_byte) ? 0u : 1u;\n"
  "      uint del = dp[j] + 1u;\n"
  "      uint ins = dp[j - 1u] + 1u;\n"
  "      uint sub = prev_diag + cost;\n"
  "      dp[j] = min(min(del, ins), sub);\n"
  "      prev_diag = tmp;\n"
  "    }\n"
  "  }\n"
  "\n"
  "  return double(dp[needle_len]);\n"
  "}\n"
  ;


// tec: the second hash of a source, FNV-1a with the length folded in. the first one (u64_hash_from_str8) is a multiply by 33 and add, which is easy to collide
internal U64
qk_check_hash(String8 source)
{
  U64 hash = 0xcbf29ce484222325ull;
  for (U64 i = 0; i < source.size; i++)
  {
    hash ^= source.str[i];
    hash *= 0x100000001b3ull;
  }
  hash ^= source.size;
  hash *= 0x100000001b3ull;
  return hash;
}

// tec: a kernel is named by two hashes of its source. QE_TEST_KEY_BITS cuts the first so that tests can make sources collide
internal U64
qk_source_key(String8 source, U64* out_check)
{
  U64 key = u64_hash_from_str8(source);
  U64 bits = settings_u64(str8_lit("QE_TEST_KEY_BITS"), 0);
  if (bits > 0 && bits < 64)
  {
    key &= (1ull << bits) - 1;
  }
  *out_check = (settings_u64(str8_lit("QE_COLLISION_CHECK"), 1) != 0) ? qk_check_hash(source) : 0;
  return key;
}

// tec: every Nth call returns true, for the failure tests
internal B32
qk_test_fail_due(char* setting, U64* counter)
{
  U64 every = settings_u64(str8_cstring(setting), 0);
  if (every == 0)
  {
    return 0;
  }
  *counter += 1;
  return (*counter % every) == 0;
}

internal String8 qk_kernel_name_locked(U64 key, U64 check);

internal String8
qk_kernel_name(U64 key, U64 check)
{
  QK_State* state = &g_qk_state;
  os_mutex_take(state->lock);
  String8 name = qk_kernel_name_locked(key, check);
  os_mutex_drop(state->lock);
  return name;
}

internal String8
qk_kernel_name_locked(U64 key, U64 check)
{
  QK_State* state = &g_qk_state;
  for (U32 i = 0; i < state->name_count; i++)
  {
    if (state->names[i].key == key && state->names[i].check == check)
    {
      return str8_cstring(state->names[i].text);
    }
  }
  U32 slot = state->name_count;
  if (slot < QK_NAME_CAP)
  {
    state->name_count += 1;
  }
  else
  {
    slot = state->name_next;
    state->name_next = (state->name_next + 1) % QK_NAME_CAP;
  }
  state->names[slot].key = key;
  state->names[slot].check = check;
  snprintf(state->names[slot].text, sizeof(state->names[slot].text), "qk_scan_%016llx%016llx", key, check);
  return str8_cstring(state->names[slot].text);
}

internal B32
qk_enabled(U64 row_count)
{
  if (settings_u64(str8_lit("QE_PERQUERY_KERNELS"), 1) == 0)
  {
    return 0;
  }
  if (row_count < settings_u64(str8_lit("QE_PERQUERY_MIN_ROWS"), 0))
  {
    return 0;
  }
  return qk_compiler_available();
}

internal B32
qk_compiler_available(void)
{
  QK_State* state = &g_qk_state;
  if (state->library_tried)
  {
    return state->library_loaded;
  }
  state->library_tried = 1;
  state->arena = arena_alloc();

  Temp scratch = scratch_begin(0, 0);
  String8 exe_dir = os_get_process_info()->binary_path;
  String8 candidates[3] = {0};
  candidates[0] = push_str8f(scratch.arena, "%.*s/shaderc_shared.dll", str8_varg(exe_dir));
  candidates[1] = str8_lit("shaderc_shared.dll");
  char* sdk = getenv("VULKAN_SDK");
  if (sdk)
  {
    candidates[2] = push_str8f(scratch.arena, "%s/Bin/shaderc_shared.dll", sdk);
  }
  for (U32 i = 0; i < ArrayCount(candidates) && !state->library_loaded; i++)
  {
    if (candidates[i].size == 0)
    {
      continue;
    }
    OS_Handle library = os_library_open(candidates[i]);
    if (library.u64[0] != 0)
    {
      state->library = library;
      state->library_loaded = 1;
      log_debug("per query kernels: loaded %.*s", str8_varg(candidates[i]));
    }
  }
  scratch_end(scratch);

  if (!state->library_loaded)
  {
    log_error("per query kernels: shaderc_shared.dll was not found");
    return 0;
  }

  state->compiler_init = (QK_CompilerInitProc*)os_library_load_proc(state->library, str8_lit("shaderc_compiler_initialize"));
  state->compiler_release = (QK_CompilerReleaseProc*)os_library_load_proc(state->library, str8_lit("shaderc_compiler_release"));
  state->options_init = (QK_OptionsInitProc*)os_library_load_proc(state->library, str8_lit("shaderc_compile_options_initialize"));
  state->options_release = (QK_OptionsReleaseProc*)os_library_load_proc(state->library, str8_lit("shaderc_compile_options_release"));
  state->options_set_optimization = (QK_OptionsSetOptimizationProc*)os_library_load_proc(state->library, str8_lit("shaderc_compile_options_set_optimization_level"));
  state->options_set_target_env = (QK_OptionsSetTargetEnvProc*)os_library_load_proc(state->library, str8_lit("shaderc_compile_options_set_target_env"));
  state->compile = (QK_CompileProc*)os_library_load_proc(state->library, str8_lit("shaderc_compile_into_spv"));
  state->result_release = (QK_ResultReleaseProc*)os_library_load_proc(state->library, str8_lit("shaderc_result_release"));
  state->result_length = (QK_ResultLengthProc*)os_library_load_proc(state->library, str8_lit("shaderc_result_get_length"));
  state->result_bytes = (QK_ResultBytesProc*)os_library_load_proc(state->library, str8_lit("shaderc_result_get_bytes"));
  state->result_status = (QK_ResultStatusProc*)os_library_load_proc(state->library, str8_lit("shaderc_result_get_compilation_status"));
  state->result_message = (QK_ResultMessageProc*)os_library_load_proc(state->library, str8_lit("shaderc_result_get_error_message"));

  B32 complete = state->compiler_init && state->compiler_release && state->options_init && state->options_release &&
    state->options_set_optimization && state->options_set_target_env && state->compile && state->result_release &&
    state->result_length && state->result_bytes && state->result_status && state->result_message;
  if (complete)
  {
    state->compiler = state->compiler_init();
    state->options = state->options_init();
  }
  if (!complete || !state->compiler || !state->options)
  {
    log_error("per query kernels: shaderc_shared.dll is missing entry points or failed to start");
    state->library_loaded = 0;
    return 0;
  }
  state->options_set_optimization(state->options, shaderc_optimization_level_performance);
  state->options_set_target_env(state->options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
  return 1;
}

// tec: compiled SPIR-V is kept in shaders/cache/<hash of the source>.spv, so a source that was built before is not built again after a restart
internal String8
qk_spirv_cache_path(Arena* arena, String8 source)
{
  U64 key = u64_hash_from_str8(source) ^ 0x5bd1e9955bd1e995ull;
  U64 check = qk_check_hash(source);
  return push_str8f(arena, "%.*s/shaders/cache/%016llx%016llx.spv", str8_varg(os_get_process_info()->binary_path), key, check);
}

internal String8
qk_spirv_compile(Arena* arena, String8 source)
{
  String8 spirv = {0};
  if (!qk_compiler_available())
  {
    return spirv;
  }
  QK_State* state = &g_qk_state;
  state->last_source_bytes = source.size;
  B32 use_disk_cache = settings_u64(str8_lit("QE_SPIRV_DISK_CACHE"), 1) != 0;
  Temp scratch = scratch_begin(&arena, 1);
  String8 cache_path = use_disk_cache ? qk_spirv_cache_path(scratch.arena, source) : str8_zero();
  if (use_disk_cache && os_file_path_exists(cache_path))
  {
    OS_Handle file = os_file_open(OS_AccessFlag_Read, cache_path);
    U64 file_size = os_properties_from_file(file).size;
    if (file_size >= 20 && (file_size & 3) == 0)
    {
      String8 data = os_string_from_file_range(arena, file, r1u64(0, file_size));
      // tec: the SPIR-V magic number, anything else is a damaged file and is built again
      if (data.size == file_size && *(U32*)data.str == 0x07230203u)
      {
        os_file_close(file);
        state->last_spirv_bytes = data.size;
        os_mutex_take(state->lock);
        state->disk_cache_hits += 1;
        os_mutex_drop(state->lock);
        scratch_end(scratch);
        return data;
      }
    }
    os_file_close(file);
  }
  os_mutex_take(state->lock);
  U64 start = os_now_microseconds();
  void* result = state->compile(state->compiler, (char*)source.str, source.size, shaderc_compute_shader, "per_query_scan.comp", "main", state->options);
  state->compile_microseconds += os_now_microseconds() - start;
  if (!result)
  {
    os_mutex_drop(state->lock);
    log_error("per query kernels: shaderc returned no result");
    scratch_end(scratch);
    return spirv;
  }
  if (state->result_status(result) != shaderc_compilation_status_success)
  {
    log_error("per query kernels: shader compile failed: %s", state->result_message(result));
    state->result_release(result);
    os_mutex_drop(state->lock);
    scratch_end(scratch);
    return spirv;
  }
  U64 length = state->result_length(result);
  spirv.str = push_array_no_zero(arena, U8, length);
  spirv.size = length;
  state->last_spirv_bytes = length;
  MemoryCopy(spirv.str, state->result_bytes(result), length);
  state->result_release(result);
  os_mutex_drop(state->lock);
  if (use_disk_cache)
  {
    os_make_directory(push_str8f(scratch.arena, "%.*s/shaders/cache", str8_varg(os_get_process_info()->binary_path)));
    os_write_data_to_file_path(cache_path, spirv);
  }
  scratch_end(scratch);
  return spirv;
}

//~ tec: the kernel library
// every named kernel (aggregates, joins, sorts, window ranks, gathers) is built at run time from shaders/<name>.comp, next to the executable
// shaderc is required, there are no precompiled kernels to fall back on

internal String8
qk_library_spirv(Arena* arena, String8 name)
{
  String8 result = {0};
  Temp scratch = scratch_begin(&arena, 1);
  String8 exe_dir = os_get_process_info()->binary_path;
  String8 path = push_str8f(scratch.arena, "%.*s/shaders/%.*s.comp", str8_varg(exe_dir), str8_varg(name));
  if (os_file_path_exists(path))
  {
    OS_Handle file = os_file_open(OS_AccessFlag_Read, path);
    U64 file_size = os_properties_from_file(file).size;
    if (file_size > 0)
    {
      String8 source = os_string_from_file_range(scratch.arena, file, r1u64(0, file_size));
      result = qk_spirv_compile(arena, source);
    }
    os_file_close(file);
  }
  scratch_end(scratch);
  return result;
}

// tec: call once after gpu_init
internal void
qk_init(void)
{
  if (g_qk_state.lock.u64[0] == 0)
  {
    g_qk_state.lock = os_mutex_alloc();
  }
  if (!qk_compiler_available())
  {
    log_error("shaderc_shared.dll was not found. every GPU kernel is built at run time, put it next to the executable or set VULKAN_SDK");
    os_abort(1);
  }
  gpu_set_spirv_provider(qk_library_spirv);
}

//~ tec: the numeric AND kernel
// tec: the same bindings, leaf words and 8 rows per thread as scan_simple.comp, with the column slot, type and operator of every comparison written into the source.
// the width of a column (narrow, sequence or whole) is only known when the scan runs, so it stays in the leaf words.
// the helpers below are copies of the ones in scan_simple.comp. keep them in step with it

global char* qk_glsl_simple_helpers =
  "#define ROWS_PER_THREAD 8u\n"
  "#define COLTYPE_U32 1\n"
  "#define COLTYPE_F32 3\n"
  "#define COLTYPE_F64 4\n"
  "#define COLTYPE_I32 7\n"
  "double\n"
  "widen_word(uint coltype, uint word)\n"
  "{\n"
  "  if (coltype == COLTYPE_U32) { return double(word); }\n"
  "  else if (coltype == COLTYPE_I32) { return double(int(word)); }\n"
  "  return double(uintBitsToFloat(word));\n"
  "}\n"
  "uint\n"
  "narrow_value(uint slot, uint row, uint width, uint base)\n"
  "{\n"
  "  if (width == 1u)\n"
  "  {\n"
  "    return base + ((read_col_word(slot, row >> 2u) >> ((row & 3u) * 8u)) & 0xffu);\n"
  "  }\n"
  "  return base + ((read_col_word(slot, row >> 1u) >> ((row & 1u) * 16u)) & 0xffffu);\n"
  "}\n"
  "uint\n"
  "compare_rows_int(uint slot, uint width, uint bias, uint lo, uint span, uint first_row, uint valid)\n"
  "{\n"
  "  uint bits = 0u;\n"
  "  if (width == 3u)\n"
  "  {\n"
  "    uint chunk_start = pc.words[10];\n"
  "    for (uint k = 0u; k < valid; k++)\n"
  "    {\n"
  "      bits |= (chunk_start + first_row + k - lo <= span ? 1u : 0u) << k;\n"
  "    }\n"
  "    return bits;\n"
  "  }\n"
  "  if (valid == ROWS_PER_THREAD && width == 1u)\n"
  "  {\n"
  "    uint lo_word = read_col_word(slot, first_row / 4u);\n"
  "    uint hi_word = read_col_word(slot, first_row / 4u + 1u);\n"
  "    for (uint k = 0u; k < 4u; k++)\n"
  "    {\n"
  "      bits |= (((lo_word >> (k * 8u)) & 0xffu) - lo <= span ? 1u : 0u) << k;\n"
  "      bits |= (((hi_word >> (k * 8u)) & 0xffu) - lo <= span ? 1u : 0u) << (k + 4u);\n"
  "    }\n"
  "    return bits;\n"
  "  }\n"
  "  if (valid == ROWS_PER_THREAD && width == 2u)\n"
  "  {\n"
  "    uvec4 words = read_col_vec(slot, first_row / 8u);\n"
  "    bits |= ((words.x & 0xffffu) - lo <= span ? 1u : 0u) << 0u;\n"
  "    bits |= ((words.x >> 16u) - lo <= span ? 1u : 0u) << 1u;\n"
  "    bits |= ((words.y & 0xffffu) - lo <= span ? 1u : 0u) << 2u;\n"
  "    bits |= ((words.y >> 16u) - lo <= span ? 1u : 0u) << 3u;\n"
  "    bits |= ((words.z & 0xffffu) - lo <= span ? 1u : 0u) << 4u;\n"
  "    bits |= ((words.z >> 16u) - lo <= span ? 1u : 0u) << 5u;\n"
  "    bits |= ((words.w & 0xffffu) - lo <= span ? 1u : 0u) << 6u;\n"
  "    bits |= ((words.w >> 16u) - lo <= span ? 1u : 0u) << 7u;\n"
  "    return bits;\n"
  "  }\n"
  "  if (valid == ROWS_PER_THREAD && width != 1u && width != 2u)\n"
  "  {\n"
  "    uint base = first_row / 4u;\n"
  "    for (uint v = 0u; v < ROWS_PER_THREAD / 4u; v++)\n"
  "    {\n"
  "      uvec4 words = read_col_vec(slot, base + v);\n"
  "      bits |= (((words.x ^ bias) - lo) <= span ? 1u : 0u) << (v * 4u + 0u);\n"
  "      bits |= (((words.y ^ bias) - lo) <= span ? 1u : 0u) << (v * 4u + 1u);\n"
  "      bits |= (((words.z ^ bias) - lo) <= span ? 1u : 0u) << (v * 4u + 2u);\n"
  "      bits |= (((words.w ^ bias) - lo) <= span ? 1u : 0u) << (v * 4u + 3u);\n"
  "    }\n"
  "    return bits;\n"
  "  }\n"
  "  for (uint k = 0u; k < valid; k++)\n"
  "  {\n"
  "    uint code;\n"
  "    if (width == 1u)\n"
  "    {\n"
  "      code = (read_col_word(slot, (first_row + k) >> 2u) >> (((first_row + k) & 3u) * 8u)) & 0xffu;\n"
  "    }\n"
  "    else if (width == 2u)\n"
  "    {\n"
  "      code = (read_col_word(slot, (first_row + k) >> 1u) >> (((first_row + k) & 1u) * 16u)) & 0xffffu;\n"
  "    }\n"
  "    else\n"
  "    {\n"
  "      code = read_col_word(slot, first_row + k) ^ bias;\n"
  "    }\n"
  "    bits |= (code - lo <= span ? 1u : 0u) << k;\n"
  "  }\n"
  "  return bits;\n"
  "}\n"
  "uint\n"
  "compare_rows(uint slot, uint coltype, uint op, double target, uint first_row, uint valid, uint width, uint enc_base)\n"
  "{\n"
  "  uint bits = 0u;\n"
  "  if (valid == ROWS_PER_THREAD && width == 1u)\n"
  "  {\n"
  "    uint lo_word = read_col_word(slot, first_row / 4u);\n"
  "    uint hi_word = read_col_word(slot, first_row / 4u + 1u);\n"
  "    for (uint k = 0u; k < 4u; k++)\n"
  "    {\n"
  "      uint a = enc_base + ((lo_word >> (k * 8u)) & 0xffu);\n"
  "      uint b = enc_base + ((hi_word >> (k * 8u)) & 0xffu);\n"
  "      bits |= (compare(op, widen_word(coltype, a), target) ? 1u : 0u) << k;\n"
  "      bits |= (compare(op, widen_word(coltype, b), target) ? 1u : 0u) << (k + 4u);\n"
  "    }\n"
  "    return bits;\n"
  "  }\n"
  "  if (valid == ROWS_PER_THREAD && width == 2u)\n"
  "  {\n"
  "    uvec4 words = read_col_vec(slot, first_row / 8u);\n"
  "    for (uint k = 0u; k < 4u; k++)\n"
  "    {\n"
  "      uint word = (k == 0u) ? words.x : ((k == 1u) ? words.y : ((k == 2u) ? words.z : words.w));\n"
  "      uint a = enc_base + (word & 0xffffu);\n"
  "      uint b = enc_base + (word >> 16u);\n"
  "      bits |= (compare(op, widen_word(coltype, a), target) ? 1u : 0u) << (k * 2u);\n"
  "      bits |= (compare(op, widen_word(coltype, b), target) ? 1u : 0u) << (k * 2u + 1u);\n"
  "    }\n"
  "    return bits;\n"
  "  }\n"
  "  if (valid == ROWS_PER_THREAD)\n"
  "  {\n"
  "    if (coltype == COLTYPE_F64)\n"
  "    {\n"
  "      uint base = first_row / 2u;\n"
  "      for (uint v = 0u; v < ROWS_PER_THREAD / 2u; v++)\n"
  "      {\n"
  "        uvec4 words = read_col_vec(slot, base + v);\n"
  "        double a = packDouble2x32(uvec2(words.x, words.y));\n"
  "        double b = packDouble2x32(uvec2(words.z, words.w));\n"
  "        bits |= (compare(op, a, target) ? 1u : 0u) << (v * 2u + 0u);\n"
  "        bits |= (compare(op, b, target) ? 1u : 0u) << (v * 2u + 1u);\n"
  "      }\n"
  "    }\n"
  "    else\n"
  "    {\n"
  "      uint base = first_row / 4u;\n"
  "      for (uint v = 0u; v < ROWS_PER_THREAD / 4u; v++)\n"
  "      {\n"
  "        uvec4 words = read_col_vec(slot, base + v);\n"
  "        bits |= (compare(op, widen_word(coltype, words.x), target) ? 1u : 0u) << (v * 4u + 0u);\n"
  "        bits |= (compare(op, widen_word(coltype, words.y), target) ? 1u : 0u) << (v * 4u + 1u);\n"
  "        bits |= (compare(op, widen_word(coltype, words.z), target) ? 1u : 0u) << (v * 4u + 2u);\n"
  "        bits |= (compare(op, widen_word(coltype, words.w), target) ? 1u : 0u) << (v * 4u + 3u);\n"
  "      }\n"
  "    }\n"
  "    return bits;\n"
  "  }\n"
  "  for (uint k = 0u; k < valid; k++)\n"
  "  {\n"
  "    double value;\n"
  "    if (width == 1u || width == 2u)\n"
  "    {\n"
  "      value = widen_word(coltype, narrow_value(slot, first_row + k, width, enc_base));\n"
  "    }\n"
  "    else if (coltype == COLTYPE_F64)\n"
  "    {\n"
  "      uint lo = read_col_word(slot, (first_row + k) * 2u + 0u);\n"
  "      uint hi = read_col_word(slot, (first_row + k) * 2u + 1u);\n"
  "      value = packDouble2x32(uvec2(lo, hi));\n"
  "    }\n"
  "    else\n"
  "    {\n"
  "      value = widen_word(coltype, read_col_word(slot, first_row + k));\n"
  "    }\n"
  "    bits |= (compare(op, value, target) ? 1u : 0u) << k;\n"
  "  }\n"
  "  return bits;\n"
  "}\n"
  ;

global char* qk_glsl_simple_compare =
  "bool\n"
  "compare(uint op, double value, double target)\n"
  "{\n"
  "  if (op == 0u)      { return value == target; }\n"
  "  else if (op == 1u) { return value < target; }\n"
  "  else if (op == 2u) { return value <= target; }\n"
  "  else if (op == 3u) { return value > target; }\n"
  "  return value >= target;\n"
  "}\n"
  ;

// tec: a comparison of a 32 bit column goes through the whole number form when the column is U32 or I32, the host makes the same choice when it writes the leaf words
internal B32
qk_leaf_is_int(U32 coltype)
{
  return (coltype == (U32)CQL_ColumnType_U32 || coltype == (U32)CQL_ColumnType_I32) && settings_u64(str8_lit("QE_SCAN_INT_COMPARE"), 1) != 0;
}

internal void
qk_emit_simple_header(Arena* arena, String8List* out)
{
  str8_list_pushf(arena, out,
                  "#version 450\n"
                  "#extension GL_ARB_gpu_shader_fp64 : enable\n"
                  "// generated by query_kernel.c, same bindings and push constants as scan_simple.comp\n"
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) readonly buffer LeafBuf { uint leaf_words[]; };\n"
                  "layout(std430, binding = 3) buffer OutIndicesBuf { uint out_indices[]; };\n"
                  "layout(std430, binding = 4) buffer OutCountBuf { uint out_count[]; };\n"
                  "layout(std430, binding = 5) readonly buffer SkipBuf { uint skip_bits[]; };\n");
}

// tec: every used column is bound as words, and as 16 byte vectors too when the comparisons read it 8 rows at a time
internal void
qk_emit_simple_columns(Arena* arena, String8List* out, B32* slot_used, B32* vec_used)
{
  for (U32 slot = 0; slot < QE_MAX_COLUMN_BINDINGS; slot++)
  {
    U32 binding = QE_BINDING_COLUMN_BASE + slot;
    if (slot_used[slot])
    {
      str8_list_pushf(arena, out, "layout(std430, binding = %u) readonly buffer ColBuf%u { uint col_data%u[]; };\n", binding, binding, binding);
    }
    if (vec_used[slot])
    {
      str8_list_pushf(arena, out, "layout(std430, binding = %u) readonly buffer VecBuf%u { uvec4 col_vec%u[]; };\n", binding, binding, binding);
    }
  }
  str8_list_pushf(arena, out,
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "shared uint s_match_count;\n"
                  "shared uint s_match_base;\n"
                  "uint\nread_col_word(uint slot, uint word_index)\n{\n  switch (slot)\n  {\n");
  for (U32 slot = 0; slot < QE_MAX_COLUMN_BINDINGS; slot++)
  {
    if (slot_used[slot])
    {
      str8_list_pushf(arena, out, "    case %uu: return col_data%u[word_index];\n", slot, QE_BINDING_COLUMN_BASE + slot);
    }
  }
  str8_list_pushf(arena, out, "  }\n  return 0u;\n}\n\nuvec4\nread_col_vec(uint slot, uint vec_index)\n{\n  switch (slot)\n  {\n");
  for (U32 slot = 0; slot < QE_MAX_COLUMN_BINDINGS; slot++)
  {
    if (vec_used[slot])
    {
      str8_list_pushf(arena, out, "    case %uu: return col_vec%u[vec_index];\n", slot, QE_BINDING_COLUMN_BASE + slot);
    }
  }
  str8_list_pushf(arena, out, "  }\n  return uvec4(0u);\n}\n\n");
}

// tec: the start of main, up to the comparisons. live has a bit per row of the thread that can still match
internal void
qk_emit_simple_prologue(Arena* arena, String8List* out)
{
  str8_list_pushf(arena, out,
                  "void\nmain()\n{\n"
                  "  uint row_count = pc.words[0];\n"
                  "  uint first_row = gl_GlobalInvocationID.x * ROWS_PER_THREAD;\n"
                  "  uint valid = (first_row < row_count) ? min(ROWS_PER_THREAD, row_count - first_row) : 0u;\n"
                  "  uint live = (valid == 32u) ? 0xffffffffu : ((1u << valid) - 1u);\n"
                  "  if (valid != 0u && pc.words[8] != 0u)\n"
                  "  {\n"
                  "    uint skip_chunk = first_row >> pc.words[6];\n"
                  "    if ((skip_bits[skip_chunk >> 5u] & (1u << (skip_chunk & 31u))) != 0u)\n"
                  "    {\n"
                  "      live = 0u;\n"
                  "    }\n"
                  "  }\n"
                  "  if (gl_LocalInvocationIndex == 0u)\n"
                  "  {\n"
                  "    s_match_count = 0u;\n"
                  "  }\n");
}

// tec: comparison l, its words start at 1 + l * 6 in the leaf words
internal void
qk_emit_simple_leaf(Arena* arena, String8List* out, U32 l, U32 slot, U32 coltype, U32 op, B32 is_int, B32 fixed, U32 fixed_flags)
{
  U32 base = 1 + l * 6;
  if (fixed)
  {
    // tec: QE_GEN_FIXED_WIDTH, the flags word of the comparison is known when the source is written, so the compiler can drop the width tests
    str8_list_pushf(arena, out,
                    "  if (live != 0u)\n"
                    "  {\n"
                    "    uint flags = %uu;\n"
                    "    uint width = (flags >> 8u) & 0xffu;\n",
                    fixed_flags);
  }
  else
  {
    str8_list_pushf(arena, out,
                    "  if (live != 0u)\n"
                    "  {\n"
                    "    uint flags = leaf_words[%uu];\n"
                    "    uint width = (flags >> 8u) & 0xffu;\n",
                    base + 1);
  }
  if (is_int)
  {
    str8_list_pushf(arena, out,
                    "    if ((flags & 0x20000u) != 0u)\n"
                    "    {\n"
                    "      live = 0u;\n"
                    "    }\n"
                    "    else\n"
                    "    {\n"
                    "      uint bias = ((flags & 0x40000u) != 0u) ? 0x80000000u : 0u;\n"
                    "      live &= compare_rows_int(%uu, width, bias, leaf_words[%uu], leaf_words[%uu], first_row, valid);\n"
                    "    }\n",
                    slot, base + 3, base + 4);
  }
  else
  {
    str8_list_pushf(arena, out,
                    "    double target = packDouble2x32(uvec2(leaf_words[%uu], leaf_words[%uu]));\n"
                    "    live &= compare_rows(%uu, %uu, %uu, target, first_row, valid, width, leaf_words[%uu]);\n",
                    base + 3, base + 4, slot, coltype, op, base + 5);
  }
  str8_list_pushf(arena, out, "  }\n");
}

// tec: counts the matches of the workgroup and writes their row ids
internal void
qk_emit_simple_epilogue(Arena* arena, String8List* out)
{
  str8_list_pushf(arena, out,
                  "  uint match_count = bitCount(live);\n"
                  "  barrier();\n"
                  "  uint local_position = 0u;\n"
                  "  if (match_count != 0u)\n"
                  "  {\n"
                  "    local_position = atomicAdd(s_match_count, match_count);\n"
                  "  }\n"
                  "  barrier();\n"
                  "  if (gl_LocalInvocationIndex == 0u)\n"
                  "  {\n"
                  "    s_match_base = (s_match_count > 0u) ? atomicAdd(out_count[0], s_match_count) : 0u;\n"
                  "  }\n"
                  "  barrier();\n"
                  "  uint capacity = out_indices.length() / 2u;\n"
                  "  uint pos = s_match_base + local_position;\n"
                  "  while (live != 0u)\n"
                  "  {\n"
                  "    uint k = uint(findLSB(live));\n"
                  "    live &= live - 1u;\n"
                  "    if (pos < capacity)\n"
                  "    {\n"
                  "      out_indices[pos * 2u + 0u] = first_row + k;\n"
                  "      out_indices[pos * 2u + 1u] = 0u;\n"
                  "    }\n"
                  "    pos++;\n"
                  "  }\n"
                  "}\n");
}

// tec: words is the leaf array scan_simple would be given: a count, then 6 words per comparison
internal B32
qk_simple_source_build(Arena* arena, U32* words, String8* out_source)
{
  U32 leaf_count = words[0];
  if (leaf_count == 0 || leaf_count > QE_CPU_SCAN_MAX_LEAVES)
  {
    return 0;
  }
  Temp scratch = scratch_begin(&arena, 1);
  B32 slot_used[QE_MAX_COLUMN_BINDINGS] = {0};
  for (U32 l = 0; l < leaf_count; l++)
  {
    U32 slot = words[1 + l * 6];
    if (slot >= QE_MAX_COLUMN_BINDINGS)
    {
      scratch_end(scratch);
      return 0;
    }
    slot_used[slot] = 1;
  }

  B32 fixed_width = settings_u64(str8_lit("QE_GEN_FIXED_WIDTH"), 0) != 0;
  String8List out = {0};
  qk_emit_simple_header(scratch.arena, &out);
  qk_emit_simple_columns(scratch.arena, &out, slot_used, slot_used);
  str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_simple_compare));
  str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_simple_helpers));
  qk_emit_simple_prologue(scratch.arena, &out);
  for (U32 l = 0; l < leaf_count; l++)
  {
    U32 base = 1 + l * 6;
    qk_emit_simple_leaf(scratch.arena, &out, l, words[base + 0], words[base + 1] & 0xffu, words[base + 2], (words[base + 1] & QE_SCAN_INT_FLAG) != 0, fixed_width, words[base + 1]);
  }
  qk_emit_simple_epilogue(scratch.arena, &out);
  *out_source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return 1;
}

//~ tec: the generated scan with the scan_filter layout, or with scan_simple's when some AND terms are plain comparisons

internal QK_Value
qk_value_make(String8 text, B32 is_bool)
{
  QK_Value value = {0};
  value.text = text;
  value.is_bool = is_bool;
  value.candidate = -1;
  value.col_slot = -1;
  value.const_index = -1;
  return value;
}

internal String8
qk_value_as_double(Arena* arena, QK_Value value)
{
  if (value.is_bool)
  {
    return push_str8f(arena, "(%.*s ? 1.0LF : 0.0LF)", str8_varg(value.text));
  }
  return value.text;
}

internal String8
qk_value_as_bool(Arena* arena, QK_Value value)
{
  if (value.is_bool)
  {
    return value.text;
  }
  return push_str8f(arena, "(%.*s != 0.0LF)", str8_varg(value.text));
}

typedef struct QK_Candidate QK_Candidate;
struct QK_Candidate
{
  U32 slot;
  U32 coltype;
  U32 op;
  U32 const_index;
};

#define QK_MAX_CANDIDATES 64

// tec: scan_simple.comp reads in memory 32 bit and 64 bit float columns, as the host checks before it takes that kernel
internal B32
qk_candidate_column_ok(QE_BytecodeProgram* prog, U32 slot, U32 coltype)
{
  for (U32 index = 0; index < prog->binding_count; index++)
  {
    QE_ColumnBinding* binding = &prog->bindings[index];
    if (binding->first_slot != slot)
    {
      continue;
    }
    B32 numeric = binding->type == CQL_ColumnType_U32 || binding->type == CQL_ColumnType_I32 || binding->type == CQL_ColumnType_F32 || binding->type == CQL_ColumnType_F64;
    return numeric && binding->slot_count == 1 && !binding->use_dict_codes && (U32)binding->type == coltype && binding->column && !binding->column->is_disk_backed;
  }
  return 0;
}

// tec: the AND terms of a value. a value that is not an AND is one term
internal U32
qk_value_terms(QK_Value* value, String8 bool_text, QK_Conj* storage, QK_Conj** out_terms)
{
  if (value->conj)
  {
    *out_terms = value->conj;
    return value->conj_count;
  }
  storage->text = bool_text;
  storage->candidate = value->is_bool ? value->candidate : -1;
  storage->slot_mask = value->slot_mask;
  *out_terms = storage;
  return 1;
}

internal B32
qk_scan_source_build(Arena* arena, QE_BytecodeProgram* prog, QK_HybridInfo* hybrid, QK_FusedAgg* fuse, String8* out_source)
{
  Temp scratch = scratch_begin(&arena, 1);

  B32 slot_used[QE_MAX_COLUMN_BINDINGS] = {0};
  // tec: bit t of loader_used[slot] is set when the column is read as column type t
  U32 loader_used[QE_MAX_COLUMN_BINDINGS] = {0};
  B32 uses_num_consts = 0;
  B32 uses_str_consts = 0;
  B32 uses_str = 0, uses_eq = 0, uses_cmp3 = 0, uses_contains = 0, uses_trigram = 0, uses_edit = 0;

  QK_Candidate candidates[QK_MAX_CANDIDATES];
  U32 candidate_count = 0;
  B32 hybrid_allowed = hybrid && hybrid->allow && !prog->has_score_output && !fuse;
  if (hybrid)
  {
    hybrid->is_hybrid = 0;
    hybrid->leaf_count = 0;
    hybrid->wide_slot_mask = 0;
  }

  String8List statements = {0};
  QK_Value stack[QK_MAX_STACK];
  U32 sp = 0;
  U32 temp_count = 0;
  B32 ok = 1;

  U64 ip = 0;
  B32 running = 1;
  while (ok && running && ip < prog->word_count)
  {
    U32 opcode = prog->words[ip++];
    switch (opcode)
    {
      case QE_Opcode_Halt:
      {
        running = 0;
      } break;

      case QE_Opcode_PushTrue:
      case QE_Opcode_PushFalse:
      {
        if (sp >= QK_MAX_STACK) { ok = 0; break; }
        stack[sp] = qk_value_make(opcode == QE_Opcode_PushTrue ? str8_lit("true") : str8_lit("false"), 1);
        sp += 1;
      } break;

      case QE_Opcode_LoadNumCol:
      {
        if (sp >= QK_MAX_STACK || ip >= prog->word_count) { ok = 0; break; }
        U32 operand = prog->words[ip++];
        U32 slot = operand & 0xffu;
        U32 coltype = (operand >> 8) & 0xffu;
        if (slot >= QE_MAX_COLUMN_BINDINGS || coltype >= 32) { ok = 0; break; }
        slot_used[slot] = 1;
        loader_used[slot] |= 1u << coltype;
        stack[sp] = qk_value_make(push_str8f(scratch.arena, "ld%u_%u(row)", slot, coltype), 0);
        stack[sp].col_slot = (S32)slot;
        stack[sp].col_type = coltype;
        stack[sp].slot_mask = 1u << slot;
        sp += 1;
      } break;

      case QE_Opcode_PushConst:
      {
        if (sp >= QK_MAX_STACK || ip >= prog->word_count) { ok = 0; break; }
        U32 index = prog->words[ip++];
        uses_num_consts = 1;
        stack[sp] = qk_value_make(push_str8f(scratch.arena, "packDouble2x32(uvec2(num_consts[%uu], num_consts[%uu]))", index * 2u, index * 2u + 1u), 0);
        stack[sp].const_index = (S32)index;
        sp += 1;
      } break;

      case QE_Opcode_CmpEq:
      case QE_Opcode_CmpNe:
      case QE_Opcode_CmpLt:
      case QE_Opcode_CmpGt:
      case QE_Opcode_CmpLe:
      case QE_Opcode_CmpGe:
      {
        if (sp < 2) { ok = 0; break; }
        char* op = "==";
        U32 simple_op = 0;
        if (opcode == QE_Opcode_CmpNe) { op = "!="; simple_op = max_U32; }
        if (opcode == QE_Opcode_CmpLt) { op = "<"; simple_op = 1; }
        if (opcode == QE_Opcode_CmpLe) { op = "<="; simple_op = 2; }
        if (opcode == QE_Opcode_CmpGt) { op = ">"; simple_op = 3; }
        if (opcode == QE_Opcode_CmpGe) { op = ">="; simple_op = 4; }
        QK_Value bv = stack[--sp];
        QK_Value av = stack[--sp];
        String8 b = qk_value_as_double(scratch.arena, bv);
        String8 a = qk_value_as_double(scratch.arena, av);
        stack[sp] = qk_value_make(push_str8f(scratch.arena, "(%.*s %s %.*s)", str8_varg(a), op, str8_varg(b)), 1);
        stack[sp].slot_mask = av.slot_mask | bv.slot_mask;
        if (hybrid_allowed && simple_op != max_U32 && candidate_count < QK_MAX_CANDIDATES && !av.is_bool && !bv.is_bool &&
            av.col_slot >= 0 && bv.const_index >= 0 && qk_candidate_column_ok(prog, (U32)av.col_slot, av.col_type) && (U64)bv.const_index < prog->const_count)
        {
          stack[sp].candidate = (S32)candidate_count;
          candidates[candidate_count].slot = (U32)av.col_slot;
          candidates[candidate_count].coltype = av.col_type;
          candidates[candidate_count].op = simple_op;
          candidates[candidate_count].const_index = (U32)bv.const_index;
          candidate_count += 1;
        }
        sp += 1;
      } break;

      case QE_Opcode_And:
      case QE_Opcode_Or:
      {
        if (sp < 2) { ok = 0; break; }
        QK_Value bv = stack[--sp];
        QK_Value av = stack[--sp];
        String8 b = qk_value_as_bool(scratch.arena, bv);
        String8 a = qk_value_as_bool(scratch.arena, av);
        stack[sp] = qk_value_make(push_str8f(scratch.arena, "(%.*s %s %.*s)", str8_varg(a), opcode == QE_Opcode_And ? "&&" : "||", str8_varg(b)), 1);
        stack[sp].slot_mask = av.slot_mask | bv.slot_mask;
        if (opcode == QE_Opcode_And && hybrid_allowed)
        {
          QK_Conj a_storage, b_storage;
          QK_Conj* a_terms = 0;
          QK_Conj* b_terms = 0;
          U32 a_count = qk_value_terms(&av, a, &a_storage, &a_terms);
          U32 b_count = qk_value_terms(&bv, b, &b_storage, &b_terms);
          if (a_count + b_count <= QK_MAX_CONJUNCTS)
          {
            QK_Conj* terms = push_array_no_zero(scratch.arena, QK_Conj, a_count + b_count);
            MemoryCopy(terms, a_terms, a_count * sizeof(QK_Conj));
            MemoryCopy(terms + a_count, b_terms, b_count * sizeof(QK_Conj));
            stack[sp].conj = terms;
            stack[sp].conj_count = a_count + b_count;
          }
        }
        sp += 1;
      } break;

      case QE_Opcode_StrEq:
      case QE_Opcode_StrContains:
      case QE_Opcode_StrNe:
      case QE_Opcode_StrLt:
      case QE_Opcode_StrGt:
      case QE_Opcode_StrLe:
      case QE_Opcode_StrGe:
      case QE_Opcode_TrigramSim:
      case QE_Opcode_EditDistance:
      {
        if (sp >= QK_MAX_STACK || ip + 3 > prog->word_count) { ok = 0; break; }
        U32 slot = prog->words[ip++];
        U32 offset = prog->words[ip++];
        U32 length = prog->words[ip++];
        if (slot + 1 >= QE_MAX_COLUMN_BINDINGS) { ok = 0; break; }
        slot_used[slot] = 1;
        slot_used[slot + 1] = 1;
        uses_str = 1;
        uses_str_consts = 1;
        stack[sp] = qk_value_make(str8_zero(), 1);
        stack[sp].slot_mask = 3u << slot;
        if (opcode == QE_Opcode_StrEq || opcode == QE_Opcode_StrNe)
        {
          uses_eq = 1;
          stack[sp].text = push_str8f(scratch.arena, "%sstr_eq(%uu, row, %uu, %uu)", opcode == QE_Opcode_StrNe ? "!" : "", slot, offset, length);
        }
        else if (opcode == QE_Opcode_StrContains)
        {
          uses_contains = 1;
          stack[sp].text = push_str8f(scratch.arena, "str_contains(%uu, row, %uu, %uu)", slot, offset, length);
        }
        else if (opcode == QE_Opcode_TrigramSim || opcode == QE_Opcode_EditDistance)
        {
          // tec: fuzzy scores are computed in program order, every one of them, since the last one decides the score written beside the row
          String8 name = push_str8f(scratch.arena, "score%u", temp_count++);
          if (opcode == QE_Opcode_TrigramSim)
          {
            uses_trigram = 1;
            str8_list_pushf(scratch.arena, &statements, "    double %.*s = trigram_similarity(%uu, row, %uu, %uu, trigram_n);\n    aux_score_bits = floatBitsToUint(float(%.*s));\n",
                            str8_varg(name), slot, offset, length, str8_varg(name));
          }
          else
          {
            uses_edit = 1;
            str8_list_pushf(scratch.arena, &statements, "    double %.*s = edit_distance(%uu, row, %uu, %uu);\n    aux_score_bits = uint(%.*s);\n",
                            str8_varg(name), slot, offset, length, str8_varg(name));
          }
          stack[sp].text = name;
          stack[sp].is_bool = 0;
        }
        else
        {
          uses_cmp3 = 1;
          char* op = "<";
          if (opcode == QE_Opcode_StrGt) op = ">";
          if (opcode == QE_Opcode_StrLe) op = "<=";
          if (opcode == QE_Opcode_StrGe) op = ">=";
          stack[sp].text = push_str8f(scratch.arena, "(str_cmp3(%uu, row, %uu, %uu) %s 0)", slot, offset, length, op);
        }
        sp += 1;
      } break;

      default:
      {
        // tec: an opcode the interpreter would not know either
        ok = 0;
      } break;
    }
  }

  if (!ok)
  {
    scratch_end(scratch);
    return 0;
  }

  // tec: the aggregate arguments are read by the same loaders as the filter columns
  if (fuse)
  {
    for (U32 k = 0; k < fuse->column_count; k++)
    {
      if (fuse->slots[k] >= QE_MAX_COLUMN_BINDINGS || fuse->coltypes[k] >= 32)
      {
        scratch_end(scratch);
        return 0;
      }
      slot_used[fuse->slots[k]] = 1;
      loader_used[fuse->slots[k]] |= 1u << fuse->coltypes[k];
    }
    if (fuse->grouped)
    {
      if (fuse->key_count == 0 || fuse->key_count > QK_GROUP_MAX_KEYS || fuse->group_count == 0)
      {
        scratch_end(scratch);
        return 0;
      }
      for (U32 i = 0; i < fuse->key_count; i++)
      {
        if (fuse->key_slots[i] >= QE_MAX_COLUMN_BINDINGS || fuse->key_types[i] >= 32 || fuse->key_ranges[i] == 0)
        {
          scratch_end(scratch);
          return 0;
        }
        slot_used[fuse->key_slots[i]] = 1;
        loader_used[fuse->key_slots[i]] |= 1u << fuse->key_types[i];
      }
    }
  }

  String8 final_expression = str8_lit("false");
  // tec: the terms that are not comparisons for scan_simple's layout, and the columns they read
  String8 complex_expression = str8_lit("true");
  B32 is_hybrid = 0;
  B32 leaf_slot[QE_MAX_COLUMN_BINDINGS] = {0};
  if (sp > 0)
  {
    final_expression = qk_value_as_bool(scratch.arena, stack[sp - 1]);
    // tec: a string term walks bytes per row, which 8 rows on one thread serialize, so those filters keep one row per thread
    if (hybrid_allowed && !uses_str && statements.first == 0 && candidate_count > 0)
    {
      QK_Conj top_storage;
      QK_Conj* terms = 0;
      U32 term_count = qk_value_terms(&stack[sp - 1], final_expression, &top_storage, &terms);
      U32 leaf_count = 0;
      U32 wide_slot_mask = 0;
      B32 have_complex = 0;
      for (U32 t = 0; t < term_count; t++)
      {
        if (terms[t].candidate >= 0 && leaf_count < QK_HYBRID_MAX_LEAVES)
        {
          QK_Candidate* candidate = &candidates[terms[t].candidate];
          QK_HybridLeaf* leaf = &hybrid->leaves[leaf_count++];
          leaf->slot = candidate->slot;
          leaf->coltype = candidate->coltype;
          leaf->op = candidate->op;
          MemoryCopy(&leaf->target, &prog->consts[candidate->const_index * 2], sizeof(F64));
          leaf_slot[candidate->slot] = 1;
        }
        else
        {
          wide_slot_mask |= terms[t].slot_mask;
          complex_expression = have_complex ? push_str8f(scratch.arena, "(%.*s && %.*s)", str8_varg(complex_expression), str8_varg(terms[t].text)) : terms[t].text;
          have_complex = 1;
        }
      }
      if (leaf_count > 0)
      {
        is_hybrid = 1;
        hybrid->is_hybrid = 1;
        hybrid->leaf_count = leaf_count;
        hybrid->wide_slot_mask = wide_slot_mask;
      }
    }
  }

  String8List out = {0};
  if (is_hybrid)
  {
    qk_emit_simple_header(scratch.arena, &out);
    str8_list_pushf(scratch.arena, &out, "#define QE_FUZZY_MAX_NEEDLE_LEN 64\n#define MAX_SANE_STRING_LEN 1048576u\n");
  }
  else
  {
    str8_list_pushf(scratch.arena, &out,
                    "#version 450\n"
                    "#extension GL_ARB_gpu_shader_fp64 : enable\n"
                    "// generated by query_kernel.c, same bindings and push constants as scan_filter.comp\n"
                    "layout(local_size_x = 256) in;\n"
                    "#define QE_FUZZY_MAX_NEEDLE_LEN 64\n"
                    "#define MAX_SANE_STRING_LEN 1048576u\n");
    if (fuse && fuse->grouped)
    {
      str8_list_pushf(scratch.arena, &out, "layout(std430, binding = 3) buffer GroupsBuf { uint out_words[]; };\n");
    }
    else if (fuse)
    {
      str8_list_pushf(scratch.arena, &out, "layout(std430, binding = 3) buffer PartialsBuf { double partials[]; };\n");
    }
    else
    {
      str8_list_pushf(scratch.arena, &out,
                      "layout(std430, binding = 3) buffer OutIndicesBuf { uint out_indices[]; };\n"
                      "layout(std430, binding = 4) buffer OutCountBuf { uint out_count[]; };\n");
    }
    str8_list_pushf(scratch.arena, &out, "layout(std430, binding = 5) readonly buffer SkipBuf { uint skip_bits[]; };\n");
  }
  if (uses_num_consts)
  {
    str8_list_pushf(scratch.arena, &out, "layout(std430, binding = 1) readonly buffer NumConstsBuf { uint num_consts[]; };\n");
  }
  if (uses_str_consts)
  {
    str8_list_pushf(scratch.arena, &out, "layout(std430, binding = 2) readonly buffer StrConstsBuf { uint str_consts[]; };\n");
  }
  if (is_hybrid)
  {
    qk_emit_simple_columns(scratch.arena, &out, slot_used, leaf_slot);
  }
  else
  {
    for (U32 slot = 0; slot < QE_MAX_COLUMN_BINDINGS; slot++)
    {
      if (slot_used[slot])
      {
        U32 binding = QE_BINDING_COLUMN_BASE + slot;
        str8_list_pushf(scratch.arena, &out, "layout(std430, binding = %u) readonly buffer ColBuf%u { uint col_data%u[]; };\n", binding, binding, binding);
      }
    }
    str8_list_pushf(scratch.arena, &out,
                    "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                    "shared uint s_match_count;\n"
                    "shared uint s_match_base;\n");
  }

  if (uses_str)
  {
    if (!is_hybrid)
    {
      str8_list_pushf(scratch.arena, &out, "uint\nread_col_word(uint slot, uint word_index)\n{\n  switch (slot)\n  {\n");
      for (U32 slot = 0; slot < QE_MAX_COLUMN_BINDINGS; slot++)
      {
        if (slot_used[slot])
        {
          str8_list_pushf(scratch.arena, &out, "    case %uu: return col_data%u[word_index];\n", slot, QE_BINDING_COLUMN_BASE + slot);
        }
      }
      str8_list_pushf(scratch.arena, &out, "  }\n  return 0u;\n}\n\n");
    }
    str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_read_col_byte));
    str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_read_str_const_byte));
    str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_read_str_offset));
    if (uses_eq) str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_str_eq));
    if (uses_cmp3) str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_str_cmp3));
    if (uses_contains) str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_str_contains));
    if (uses_trigram) str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_trigram));
    if (uses_edit) str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_edit_distance));
  }
  if (is_hybrid)
  {
    str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_simple_compare));
    str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_simple_helpers));
  }

  // tec: one loader per column and type, the same arithmetic as read_num_column_value in scan_filter.comp
  for (U32 slot = 0; slot < QE_MAX_COLUMN_BINDINGS; slot++)
  {
    for (U32 type = 0; type < 32; type++)
    {
      if (!(loader_used[slot] & (1u << type)))
      {
        continue;
      }
      U32 binding = QE_BINDING_COLUMN_BASE + slot;
      str8_list_pushf(scratch.arena, &out, "double\nld%u_%u(uint row)\n{\n", slot, type);
      if (type == 1 || type == COLTYPE_ENUM)
      {
        str8_list_pushf(scratch.arena, &out, "  return double(col_data%u[row]);\n", binding);
      }
      else if (type == 2)
      {
        str8_list_pushf(scratch.arena, &out, "  return double(col_data%u[row * 2u + 1u]) * 4294967296.0LF + double(col_data%u[row * 2u]);\n", binding, binding);
      }
      else if (type == 3)
      {
        str8_list_pushf(scratch.arena, &out, "  return double(uintBitsToFloat(col_data%u[row]));\n", binding);
      }
      else if (type == 4)
      {
        str8_list_pushf(scratch.arena, &out, "  return packDouble2x32(uvec2(col_data%u[row * 2u], col_data%u[row * 2u + 1u]));\n", binding, binding);
      }
      else if (type == COLTYPE_BOOL)
      {
        str8_list_pushf(scratch.arena, &out, "  return double((col_data%u[row / 4u] >> ((row %% 4u) * 8u)) & 0xffu);\n", binding);
      }
      else if (type == COLTYPE_I32 || type == COLTYPE_DATE)
      {
        str8_list_pushf(scratch.arena, &out, "  return double(int(col_data%u[row]));\n", binding);
      }
      else
      {
        str8_list_pushf(scratch.arena, &out,
                        "  uint lo = col_data%u[row * 2u];\n"
                        "  uint hi = col_data%u[row * 2u + 1u];\n"
                        "  double unsigned_value = double(hi) * 4294967296.0LF + double(lo);\n"
                        "  bool is_negative = (hi & 0x80000000u) != 0u;\n"
                        "  return is_negative ? unsigned_value - 18446744073709551616.0LF : unsigned_value;\n", binding, binding);
      }
      str8_list_pushf(scratch.arena, &out, "}\n\n");
    }
  }

  if (is_hybrid)
  {
    qk_emit_simple_prologue(scratch.arena, &out);
    for (U32 l = 0; l < hybrid->leaf_count; l++)
    {
      QK_HybridLeaf* leaf = &hybrid->leaves[l];
      qk_emit_simple_leaf(scratch.arena, &out, l, leaf->slot, leaf->coltype, leaf->op, qk_leaf_is_int(leaf->coltype), 0, 0);
    }
    // tec: the other terms only look at the rows the comparisons left
    str8_list_pushf(scratch.arena, &out,
                    "  uint remaining = live;\n"
                    "  while (remaining != 0u)\n"
                    "  {\n"
                    "    uint k = uint(findLSB(remaining));\n"
                    "    remaining &= remaining - 1u;\n"
                    "    uint row = first_row + k;\n"
                    "    if (!(%.*s))\n"
                    "    {\n"
                    "      live &= ~(1u << k);\n"
                    "    }\n"
                    "  }\n",
                    str8_varg(complex_expression));
    qk_emit_simple_epilogue(scratch.arena, &out);
    *out_source = str8_list_join(arena, &out, 0);
    scratch_end(scratch);
    return 1;
  }

  if (fuse && fuse->grouped)
  {
    // tec: filter and GROUP BY in one pass. each workgroup counts its rows into shared memory with integer atomics, then merges its groups into the output once
    U32 k_count = fuse->column_count;
    U32 g_count = fuse->group_count;
    U32 width = QK_GROUP_WORDS(k_count);
    str8_list_pushf(scratch.arena, &out, "#define GROUPS %uu\n#define WIDTH %uu\nshared uint s_acc[%uu];\n", g_count, width, g_count * width);
    str8_list_pushf(scratch.arena, &out,
                    "void\nmain()\n{\n"
                    "  uint row_count = pc.words[0];\n"
                    "  uint group_count = pc.words[12];\n"
                    "  uint tid = gl_LocalInvocationIndex;\n"
                    "  uint stride = group_count * 256u;\n"
                    "  for (uint i = tid; i < GROUPS * WIDTH; i += 256u)\n"
                    "  {\n"
                    "    uint field = i %% WIDTH;\n"
                    "    bool is_min = field == 1u || (field >= 2u && ((field - 2u) %% 4u) == 2u);\n"
                    "    s_acc[i] = is_min ? 0xffffffffu : 0u;\n"
                    "  }\n"
                    "  barrier();\n"
                    "  for (uint row = gl_GlobalInvocationID.x; row < row_count; row += stride)\n"
                    "  {\n"
                    "    uint score_flags = pc.words[2];\n"
                    "    bool has_aux_score = (score_flags & 1u) != 0u;\n"
                    "    uint trigram_n = max(pc.words[4], 1u);\n"
                    "    uint aux_score_bits = 0u;\n");
    for (String8Node* node = statements.first; node; node = node->next)
    {
      str8_list_push(scratch.arena, &out, node->string);
    }
    str8_list_pushf(scratch.arena, &out,
                    "    if (%.*s)\n"
                    "    {\n"
                    "      uint g = 0u;\n"
                    "      bool key_ok = true;\n",
                    str8_varg(final_expression));
    // tec: the smallest values of the keys travel in the push constants, key 0 and 1 in the two words of slot 7, key 2 in the high word of slot 6
    U32 min_words[QK_GROUP_MAX_KEYS] = { 14, 15, 13 };
    for (U32 i = 0; i < fuse->key_count; i++)
    {
      str8_list_pushf(scratch.arena, &out,
                      "      {\n"
                      "        int part = int(ld%u_%u(row)) - int(pc.words[%uu]);\n"
                      "        key_ok = key_ok && part >= 0 && uint(part) < %uu;\n"
                      "        g = g * %uu + uint(part);\n"
                      "      }\n",
                      fuse->key_slots[i], fuse->key_types[i], min_words[i], fuse->key_ranges[i], fuse->key_ranges[i]);
    }
    str8_list_pushf(scratch.arena, &out,
                    "      if (key_ok && g < GROUPS)\n"
                    "      {\n"
                    "        uint b = g * WIDTH;\n"
                    "        atomicAdd(s_acc[b], 1u);\n"
                    "        atomicMin(s_acc[b + 1u], row);\n");
    for (U32 k = 0; k < k_count; k++)
    {
      B32 is_signed = fuse->coltypes[k] == 7;
      str8_list_pushf(scratch.arena, &out,
                      "        {\n"
                      "          double vd = ld%u_%u(row);\n"
                      "          uint u = %s;\n"
                      "          uint old = atomicAdd(s_acc[b + %uu], u);\n"
                      "          if (old + u < old)\n"
                      "          {\n"
                      "            atomicAdd(s_acc[b + %uu], 1u);\n"
                      "          }\n"
                      "          atomicMin(s_acc[b + %uu], u);\n"
                      "          atomicMax(s_acc[b + %uu], u);\n"
                      "        }\n",
                      fuse->slots[k], fuse->coltypes[k], is_signed ? "uint(int(vd)) ^ 0x80000000u" : "uint(vd)", 2 + 4 * k, 3 + 4 * k, 4 + 4 * k, 5 + 4 * k);
    }
    str8_list_pushf(scratch.arena,
                    &out,
                    "      }\n"
                    "    }\n"
                    "  }\n"
                    "  barrier();\n"
                    "  for (uint g = tid; g < GROUPS; g += 256u)\n"
                    "  {\n"
                    "    uint b = g * WIDTH;\n"
                    "    if (s_acc[b] == 0u)\n"
                    "    {\n"
                    "      continue;\n"
                    "    }\n"
                    "    atomicAdd(out_words[b], s_acc[b]);\n"
                    "    atomicMin(out_words[b + 1u], s_acc[b + 1u]);\n");
    for (U32 k = 0; k < k_count; k++)
    {
      str8_list_pushf(scratch.arena, &out,
                      "    {\n"
                      "      uint lo = s_acc[b + %uu];\n"
                      "      uint old = atomicAdd(out_words[b + %uu], lo);\n"
                      "      uint carry = (old + lo < old) ? 1u : 0u;\n"
                      "      atomicAdd(out_words[b + %uu], s_acc[b + %uu] + carry);\n"
                      "      atomicMin(out_words[b + %uu], s_acc[b + %uu]);\n"
                      "      atomicMax(out_words[b + %uu], s_acc[b + %uu]);\n"
                      "    }\n",
                      2 + 4 * k, 2 + 4 * k, 3 + 4 * k, 3 + 4 * k, 4 + 4 * k, 4 + 4 * k, 5 + 4 * k, 5 + 4 * k);
    }
    str8_list_pushf(scratch.arena, &out, "  }\n}\n");
    *out_source = str8_list_join(arena, &out, 0);
    scratch_end(scratch);
    return 1;
  }

  if (fuse)
  {
    // tec: the filter and the aggregate in one pass. a thread folds the matching rows of its share into registers, a workgroup reduces once
    // partials per workgroup: matching row count, then sum, min and max of each aggregate column
    U32 k_count = fuse->column_count;
    str8_list_pushf(scratch.arena, &out, "shared double sh_reduce[256];\n");
    str8_list_pushf(scratch.arena, &out,
                    "void\nmain()\n{\n"
                    "  uint row_count = pc.words[0];\n"
                    "  uint group_count = pc.words[12];\n"
                    "  uint tid = gl_LocalInvocationIndex;\n"
                    "  uint stride = group_count * 256u;\n"
                    "  double biggest = packDouble2x32(uvec2(0xffffffffu, 0x7fefffffu));\n"
                    "  double cnt = 0.0LF;\n");
    for (U32 k = 0; k < k_count; k++)
    {
      str8_list_pushf(scratch.arena, &out, "  double sum%u = 0.0LF;\n  double mn%u = biggest;\n  double mx%u = -biggest;\n", k, k, k);
    }
    str8_list_pushf(scratch.arena, &out,
                    "  for (uint row = gl_GlobalInvocationID.x; row < row_count; row += stride)\n"
                    "  {\n"
                    "    uint score_flags = pc.words[2];\n"
                    "    bool has_aux_score = (score_flags & 1u) != 0u;\n"
                    "    uint trigram_n = max(pc.words[4], 1u);\n"
                    "    uint aux_score_bits = 0u;\n");
    for (String8Node* node = statements.first; node; node = node->next)
    {
      str8_list_push(scratch.arena, &out, node->string);
    }
    str8_list_pushf(scratch.arena, &out, "    if (%.*s)\n    {\n      cnt += 1.0LF;\n", str8_varg(final_expression));
    for (U32 k = 0; k < k_count; k++)
    {
      str8_list_pushf(scratch.arena, &out,
                      "      double v%u = ld%u_%u(row);\n"
                      "      sum%u += v%u;\n"
                      "      mn%u = min(mn%u, v%u);\n"
                      "      mx%u = max(mx%u, v%u);\n",
                      k, fuse->slots[k], fuse->coltypes[k], k, k, k, k, k, k, k, k);
    }
    str8_list_pushf(scratch.arena, &out, "    }\n  }\n");
    U32 width = 1 + 3 * k_count;
    for (U32 q = 0; q < width; q++)
    {
      char value[16];
      char* combine = "sh_reduce[tid] += sh_reduce[tid + step];";
      if (q == 0)
      {
        snprintf(value, sizeof(value), "cnt");
      }
      else
      {
        U32 k = (q - 1) / 3;
        U32 part = (q - 1) % 3;
        snprintf(value, sizeof(value), "%s%u", part == 0 ? "sum" : (part == 1 ? "mn" : "mx"), k);
        if (part == 1) combine = "sh_reduce[tid] = min(sh_reduce[tid], sh_reduce[tid + step]);";
        if (part == 2) combine = "sh_reduce[tid] = max(sh_reduce[tid], sh_reduce[tid + step]);";
      }
      str8_list_pushf(scratch.arena, &out,
                      "  sh_reduce[tid] = %s;\n"
                      "  barrier();\n"
                      "  for (uint step = 128u; step > 0u; step >>= 1u)\n"
                      "  {\n"
                      "    if (tid < step)\n"
                      "    {\n"
                      "      %s\n"
                      "    }\n"
                      "    barrier();\n"
                      "  }\n"
                      "  if (tid == 0u)\n"
                      "  {\n"
                      "    partials[gl_WorkGroupID.x * %uu + %uu] = sh_reduce[0];\n"
                      "  }\n"
                      "  barrier();\n",
                      value, combine, width, q);
    }
    str8_list_pushf(scratch.arena, &out, "}\n");
    *out_source = str8_list_join(arena, &out, 0);
    scratch_end(scratch);
    return 1;
  }

  str8_list_pushf(scratch.arena, &out,
                  "void\nmain()\n{\n"
                  "  uint row_count = pc.words[0];\n"
                  "  uint row = gl_GlobalInvocationID.x;\n"
                  "  bool in_range = row < row_count;\n"
                  "  if (in_range && pc.words[8] != 0u)\n"
                  "  {\n"
                  "    uint skip_chunk = row >> pc.words[6];\n"
                  "    in_range = (skip_bits[skip_chunk >> 5u] & (1u << (skip_chunk & 31u))) == 0u;\n"
                  "  }\n"
                  "  if (gl_LocalInvocationIndex == 0u)\n"
                  "  {\n"
                  "    s_match_count = 0u;\n"
                  "  }\n"
                  "  uint score_flags = pc.words[2];\n"
                  "  bool has_aux_score = (score_flags & 1u) != 0u;\n"
                  "  uint trigram_n = max(pc.words[4], 1u);\n"
                  "  uint aux_score_bits = 0u;\n"
                  "  bool matched = false;\n"
                  "  if (in_range)\n"
                  "  {\n");
  for (String8Node* node = statements.first; node; node = node->next)
  {
    str8_list_push(scratch.arena, &out, node->string);
  }
  str8_list_pushf(scratch.arena, &out, "    matched = %.*s;\n  }\n", str8_varg(final_expression));
  str8_list_pushf(scratch.arena, &out,
                  "  barrier();\n"
                  "  uint local_position = 0u;\n"
                  "  if (matched)\n"
                  "  {\n"
                  "    local_position = atomicAdd(s_match_count, 1u);\n"
                  "  }\n"
                  "  barrier();\n"
                  "  if (gl_LocalInvocationIndex == 0u)\n"
                  "  {\n"
                  "    s_match_base = (s_match_count > 0u) ? atomicAdd(out_count[0], s_match_count) : 0u;\n"
                  "  }\n"
                  "  barrier();\n"
                  "  if (matched)\n"
                  "  {\n"
                  "    uint pos = s_match_base + local_position;\n"
                  "    if (pos < out_indices.length() / 2u)\n"
                  "    {\n"
                  "      out_indices[pos * 2u + 0u] = row;\n"
                  "      out_indices[pos * 2u + 1u] = has_aux_score ? aux_score_bits : 0u;\n"
                  "    }\n"
                  "  }\n"
                  "}\n");

  *out_source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return 1;
}

internal GPU_Kernel*
qk_kernel_from_source(Arena* arena, String8 source, String8* out_name)
{
  QK_State* state = &g_qk_state;
  U64 check = 0;
  U64 source_key = qk_source_key(source, &check);
  U64 key = source_key ^ (check * 0x9e3779b97f4a7c15ull);
  os_mutex_take(state->lock);
  for (U32 i = 0; i < state->failed_key_count; i++)
  {
    if (state->failed_keys[i] == key)
    {
      os_mutex_drop(state->lock);
      return 0;
    }
  }
  os_mutex_drop(state->lock);

  String8 name = qk_kernel_name(source_key, check);
  GPU_Kernel* kernel = gpu_kernel_alloc_from_spirv(name, str8_zero());
  if (!kernel)
  {
    if (settings_u64(str8_lit("QE_PERQUERY_LOG_SOURCE"), 0) != 0)
    {
      log_debug("per query kernels: source for %.*s\n%.*s", str8_varg(name), str8_varg(source));
    }
    String8 spirv = {0};
    os_mutex_take(state->lock);
    B32 inject = qk_test_fail_due("QE_TEST_FAIL_SHADERC", &state->test_shaderc_count);
    os_mutex_drop(state->lock);
    if (!inject)
    {
      spirv = qk_spirv_compile(arena, source);
    }
    if (spirv.size > 0)
    {
      kernel = gpu_kernel_alloc_from_spirv(name, spirv);
    }
    if (!kernel)
    {
      os_mutex_take(state->lock);
      state->compile_failures += 1;
      if (state->failed_key_count < QK_FAILED_KEY_CAP)
      {
        state->failed_keys[state->failed_key_count++] = key;
      }
      os_mutex_drop(state->lock);
      return 0;
    }
    os_mutex_take(state->lock);
    state->kernels_compiled += 1;
    os_mutex_drop(state->lock);
  }
  if (out_name)
  {
    *out_name = name;
  }
  return kernel;
}

// tec: a conjunction of plain comparisons has a precompiled path (scan_simple, or the interpreter for the terms it does not cover), and generating for it costs a median of 85.7 ms on first use.
// with QE_ROUTE_CONJUNCTIONS:1 the first execution of such a shape is left to the precompiled path and the kernel is generated when the shape comes back
internal B32
qk_route_defer(U64 key)
{
  if (settings_u64(str8_lit("QE_ROUTE_CONJUNCTIONS"), 0) == 0)
  {
    return 0;
  }
  QK_State* state = &g_qk_state;
  B32 defer = 0;
  os_mutex_take(state->lock);
  B32 seen = 0;
  for (U32 i = 0; i < state->routed_key_count && !seen; i++)
  {
    seen = state->routed_keys[i] == key;
  }
  if (!seen)
  {
    // tec: a full table forgets the oldest key, so that shape is deferred once more
    U32 slot = 0;
    if (state->routed_key_count < QK_ROUTED_KEY_CAP)
    {
      slot = state->routed_key_count;
      state->routed_key_count += 1;
    }
    else
    {
      slot = state->routed_next;
      state->routed_next = (state->routed_next + 1) % QK_ROUTED_KEY_CAP;
    }
    state->routed_keys[slot] = key;
    state->routed_deferrals += 1;
    defer = 1;
  }
  os_mutex_drop(state->lock);
  return defer;
}

internal U64
qk_routed_deferrals(void)
{
  return g_qk_state.routed_deferrals;
}

// tec: QE_SCAN_SPEC, one precompiled module with the comparisons as specialization constants. the pipeline is built per combination, there is no per query source and no shaderc call after the first
internal GPU_Kernel*
qk_spec_kernel(Arena* arena, U32* words, String8* out_name)
{
  QK_State* state = &g_qk_state;
  U32 leaf_count = words[0];
  if (leaf_count == 0 || leaf_count > 8)
  {
    return 0;
  }
  U32 values[25] = {0};
  state->spec_requests += 1;
  values[0] = leaf_count;
  for (U32 l = 0; l < leaf_count; l++)
  {
    U32 base = 1 + l * 6;
    if (words[base + 0] >= QE_MAX_COLUMN_BINDINGS)
    {
      return 0;
    }
    values[1 + l] = words[base + 0];
    values[9 + l] = words[base + 1];
    values[17 + l] = words[base + 2];
  }
  String8 bytes = str8((U8*)values, sizeof(values));
  U64 key = u64_hash_from_str8(bytes) ^ 0x7e5ca1ed5ca1ab1eull;
  U64 check = qk_check_hash(bytes);
  String8 name = qk_kernel_name(key, check);
  GPU_Kernel* kernel = gpu_kernel_alloc_from_spirv_spec(name, str8_zero(), values, ArrayCount(values));
  if (!kernel)
  {
    os_mutex_take(state->lock);
    B32 tried = state->spec_spirv_tried;
    state->spec_spirv_tried = 1;
    os_mutex_drop(state->lock);
    if (!tried)
    {
      state->spec_spirv = qk_library_spirv(state->arena, str8_lit("scan_spec"));
    }
    if (state->spec_spirv.size == 0)
    {
      return 0;
    }
    kernel = gpu_kernel_alloc_from_spirv_spec(name, state->spec_spirv, values, ArrayCount(values));
    if (!kernel)
    {
      os_mutex_take(state->lock);
      state->compile_failures += 1;
      os_mutex_drop(state->lock);
      return 0;
    }
    os_mutex_take(state->lock);
    state->kernels_compiled += 1;
    os_mutex_drop(state->lock);
  }
  if (out_name)
  {
    *out_name = name;
  }
  return kernel;
}

// tec: QE_FUSE_COUNT_SPEC, COUNT(*) over a numeric conjunction on one precompiled module, fused_count_spec.comp, the comparisons are specialization constants and the literals are read from a buffer
internal GPU_Kernel*
qk_fused_count_spec_kernel(U32 leaf_count, U32* slots, U32* types, U32* ops, String8* out_name)
{
  QK_State* state = &g_qk_state;
  if (leaf_count == 0 || leaf_count > 8)
  {
    return 0;
  }
  U32 values[25] = {0};
  state->count_spec_requests += 1;
  values[0] = leaf_count;
  for (U32 l = 0; l < leaf_count; l++)
  {
    if (slots[l] >= QE_MAX_COLUMN_BINDINGS)
    {
      return 0;
    }
    values[1 + l] = slots[l];
    values[9 + l] = types[l];
    values[17 + l] = ops[l];
  }
  String8 bytes = str8((U8*)values, sizeof(values));
  U64 key = u64_hash_from_str8(bytes) ^ 0x0c0a7c0a7c0a7c0aull;
  U64 check = qk_check_hash(bytes);
  String8 name = qk_kernel_name(key, check);
  GPU_Kernel* kernel = gpu_kernel_alloc_from_spirv_spec(name, str8_zero(), values, ArrayCount(values));
  if (!kernel)
  {
    os_mutex_take(state->lock);
    B32 tried = state->count_spec_spirv_tried;
    state->count_spec_spirv_tried = 1;
    os_mutex_drop(state->lock);
    if (!tried)
    {
      state->count_spec_spirv = qk_library_spirv(state->arena, str8_lit("fused_count_spec"));
    }
    if (state->count_spec_spirv.size == 0)
    {
      return 0;
    }
    kernel = gpu_kernel_alloc_from_spirv_spec(name, state->count_spec_spirv, values, ArrayCount(values));
    if (!kernel)
    {
      os_mutex_take(state->lock);
      state->compile_failures += 1;
      os_mutex_drop(state->lock);
      return 0;
    }
    os_mutex_take(state->lock);
    state->kernels_compiled += 1;
    os_mutex_drop(state->lock);
  }
  if (out_name)
  {
    *out_name = name;
  }
  return kernel;
}

// tec: QE_SCAN_FILTER_SPEC, an AND/OR tree of up to 8 numeric or string comparisons on one precompiled module, scan_filter_spec.comp. the bytecode is read back into leaves and postfix tokens, which become the specialization constants, the literals stay in the buffers of the interpreter
internal GPU_Kernel*
qk_filter_spec_kernel(U32* words, U32 word_count, String8* out_name)
{
  QK_State* state = &g_qk_state;
  U32 values[27] = {0};
  U32 leaf_count = 0;
  U32 token_count = 0;
  U32 tokens = 0;
  U32 depth = 0;
  U32 ip = 0;
  while (ip < word_count)
  {
    U32 opcode = words[ip++];
    U32 kind = 0, a_slot = 0, a_type = 0, a_op = 0, b = 0, c = 0;
    B32 leaf = 0;
    if (opcode == QE_Opcode_PushTrue || opcode == QE_Opcode_PushFalse)
    {
      kind = opcode == QE_Opcode_PushTrue ? 9 : 10;
      leaf = 1;
    }
    else if (opcode == QE_Opcode_LoadNumCol && ip + 3 < word_count + 0 && words[ip + 1] == QE_Opcode_PushConst && words[ip + 3] >= QE_Opcode_CmpEq && words[ip + 3] <= QE_Opcode_CmpGe)
    {
      a_slot = words[ip] & 0xffu;
      a_type = (words[ip] >> 8) & 0xffu;
      b = words[ip + 2];
      a_op = words[ip + 3];
      kind = 1;
      ip += 4;
      leaf = 1;
    }
    else if (opcode == QE_Opcode_PushConst && ip + 3 < word_count + 0 && words[ip + 1] == QE_Opcode_LoadNumCol && words[ip + 3] >= QE_Opcode_CmpEq && words[ip + 3] <= QE_Opcode_CmpGe)
    {
      // tec: literal on the left, the comparison is mirrored
      a_slot = words[ip + 2] & 0xffu;
      a_type = (words[ip + 2] >> 8) & 0xffu;
      b = words[ip];
      U32 cmp = words[ip + 3];
      a_op = cmp == QE_Opcode_CmpLt ? QE_Opcode_CmpGt : cmp == QE_Opcode_CmpGt ? QE_Opcode_CmpLt : cmp == QE_Opcode_CmpLe ? QE_Opcode_CmpGe : cmp == QE_Opcode_CmpGe ? QE_Opcode_CmpLe : cmp;
      kind = 1;
      ip += 4;
      leaf = 1;
    }
    else if ((opcode == QE_Opcode_StrEq || opcode == QE_Opcode_StrNe || opcode == QE_Opcode_StrContains || (opcode >= QE_Opcode_StrLt && opcode <= QE_Opcode_StrGe)) && ip + 2 < word_count + 0)
    {
      a_slot = words[ip] & 0xffu;
      b = words[ip + 1];
      c = words[ip + 2];
      kind = opcode == QE_Opcode_StrEq ? 2 : opcode == QE_Opcode_StrNe ? 3 : opcode == QE_Opcode_StrContains ? 4 : 5 + (opcode - QE_Opcode_StrLt);
      ip += 3;
      leaf = 1;
    }
    if (leaf)
    {
      if (leaf_count >= 8 || token_count >= 15 || a_slot >= QE_MAX_COLUMN_BINDINGS)
      {
        return 0;
      }
      values[1 + 3 * leaf_count + 0] = kind | (a_slot << 8) | (a_type << 16) | (a_op << 24);
      values[1 + 3 * leaf_count + 1] = b;
      values[1 + 3 * leaf_count + 2] = c;
      leaf_count += 1;
      tokens |= 1u << (2 * token_count);
      token_count += 1;
      depth += 1;
    }
    else if (opcode == QE_Opcode_And || opcode == QE_Opcode_Or)
    {
      if (depth < 2 || token_count >= 15)
      {
        return 0;
      }
      tokens |= (opcode == QE_Opcode_And ? 2u : 3u) << (2 * token_count);
      token_count += 1;
      depth -= 1;
    }
    else if (opcode == QE_Opcode_Halt)
    {
      break;
    }
    else
    {
      return 0;
    }
  }
  if (leaf_count == 0 || depth != 1)
  {
    return 0;
  }
  values[0] = leaf_count;
  values[25] = tokens;
  values[26] = token_count;
  String8 bytes = str8((U8*)values, sizeof(values));
  U64 key = u64_hash_from_str8(bytes) ^ 0x5fc1f17e25bec000ull;
  U64 check = qk_check_hash(bytes);
  String8 name = qk_kernel_name(key, check);
  GPU_Kernel* kernel = gpu_kernel_alloc_from_spirv_spec(name, str8_zero(), values, ArrayCount(values));
  if (!kernel)
  {
    os_mutex_take(state->lock);
    B32 tried = state->filter_spec_spirv_tried;
    state->filter_spec_spirv_tried = 1;
    os_mutex_drop(state->lock);
    if (!tried)
    {
      state->filter_spec_spirv = qk_library_spirv(state->arena, str8_lit("scan_filter_spec"));
    }
    if (state->filter_spec_spirv.size == 0)
    {
      return 0;
    }
    kernel = gpu_kernel_alloc_from_spirv_spec(name, state->filter_spec_spirv, values, ArrayCount(values));
    if (!kernel)
    {
      os_mutex_take(state->lock);
      state->compile_failures += 1;
      os_mutex_drop(state->lock);
      return 0;
    }
    os_mutex_take(state->lock);
    state->kernels_compiled += 1;
    os_mutex_drop(state->lock);
  }
  if (out_name)
  {
    *out_name = name;
  }
  return kernel;
}

internal U64
qk_count_spec_request_count(void)
{
  return g_qk_state.count_spec_requests;
}

internal GPU_Kernel*
qk_simple_kernel(Arena* arena, U32* words, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  if (settings_u64(str8_lit("QE_SCAN_SPEC"), 0) != 0)
  {
    return qk_spec_kernel(arena, words, out_name);
  }
  String8 source = {0};
  if (!qk_simple_source_build(arena, words, &source))
  {
    return 0;
  }
  if (qk_route_defer(u64_hash_from_str8(source)))
  {
    return 0;
  }
  return qk_kernel_from_source(arena, source, out_name);
}

internal GPU_Kernel*
qk_scan_kernel(Arena* arena, QE_BytecodeProgram* prog, QK_HybridInfo* hybrid, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  QK_State* state = &g_qk_state;
  state->kernels_requested += 1;

  String8 source = {0};
  if (!qk_scan_source_build(arena, prog, hybrid, 0, &source))
  {
    log_debug("per query kernels: the bytecode has an operator the generator does not handle, using the interpreter");
    return 0;
  }
  if (hybrid && hybrid->is_hybrid && qk_route_defer(u64_hash_from_str8(source)))
  {
    // tec: the caller reads is_hybrid and the wide slot mask after this call, and the interpreter runs without them
    MemoryZeroStruct(hybrid);
    return 0;
  }
  return qk_kernel_from_source(arena, source, out_name);
}

internal GPU_Kernel*
qk_fused_aggregate_kernel(Arena* arena, QE_BytecodeProgram* prog, QK_FusedAgg* fuse, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  String8 source = {0};
  if (!qk_scan_source_build(arena, prog, 0, fuse, &source))
  {
    return 0;
  }
  return qk_kernel_from_source(arena, source, out_name);
}

internal U32
qk_sort_tile(U32 key_count, B32 narrow)
{
  // tec: a tile of keys and rows has to fit in 16 KB of shared memory, the least Vulkan guarantees
  U32 tile = 1024;
  U32 bytes_per_element = key_count * (narrow ? 4 : 8) + 4;
  while (tile > 64 && tile * bytes_per_element > 16384)
  {
    tile >>= 1;
  }
  return tile;
}

internal B32
qk_sort_source_build(Arena* arena, U32 key_count, U32 dir_mask, B32 narrow, String8* out_source)
{
  if (key_count == 0 || key_count > 4)
  {
    return 0;
  }
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  U32 tile = qk_sort_tile(key_count, narrow);
  U32 log_tile = 0;
  while ((1u << log_tile) < tile)
  {
    log_tile += 1;
  }
  char* type_name = narrow ? "float" : "double";

  str8_list_pushf(scratch.arena, &out, "#version 450\n");
  if (!narrow)
  {
    str8_list_pushf(scratch.arena, &out, "#extension GL_ARB_gpu_shader_fp64 : enable\n");
  }
  str8_list_pushf(scratch.arena, &out,
                  "layout(local_size_x = %u) in;\n"
                  "layout(std430, binding = 0) buffer KeyBuf { %s keys[]; };\n"
                  "layout(std430, binding = 1) buffer RowBuf { uint rows[]; };\n"
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "#define KEYS %uu\n#define TILE %uu\n#define LOG_TILE %uu\n"
                  "shared %s s_key[TILE * KEYS];\nshared uint s_row[TILE];\n",
                  tile / 2, type_name, key_count, tile, log_tile, type_name);

  // tec: true when a sorts after b. rows past the real count are padding and sort last, equal keys keep their input order
  str8_list_pushf(scratch.arena, &out, "bool\ngreater(");
  for (U32 k = 0; k < key_count; k++)
  {
    str8_list_pushf(scratch.arena, &out, "%s a%u, ", type_name, k);
  }
  str8_list_pushf(scratch.arena, &out, "uint a_row, ");
  for (U32 k = 0; k < key_count; k++)
  {
    str8_list_pushf(scratch.arena, &out, "%s b%u, ", type_name, k);
  }
  str8_list_pushf(scratch.arena, &out,
                  "uint b_row)\n{\n"
                  "  bool a_valid = a_row < pc.words[2];\n"
                  "  bool b_valid = b_row < pc.words[2];\n"
                  "  if (a_valid != b_valid)\n  {\n    return !a_valid;\n  }\n");
  for (U32 k = 0; k < key_count; k++)
  {
    B32 desc = (dir_mask >> k) & 1;
    str8_list_pushf(scratch.arena, &out, "  if (a%u != b%u)\n  {\n    return a%u %s b%u;\n  }\n", k, k, k, desc ? "<" : ">", k);
  }
  str8_list_pushf(scratch.arena, &out, "  return a_row > b_row;\n}\n");

  // tec: exchange_shared and exchange_global differ only in where the pair lives
  char* spaces[2] = { "shared", "global" };
  for (U32 which = 0; which < 2; which++)
  {
    str8_list_pushf(scratch.arena, &out, "void\nexchange_%s(uint position, uint stride, bool ascending)\n{\n  uint partner = position + stride;\n", spaces[which]);
    char* array_name = which == 0 ? "s_key" : "keys";
    char* row_name = which == 0 ? "s_row" : "rows";
    for (U32 k = 0; k < key_count; k++)
    {
      str8_list_pushf(scratch.arena, &out, "  %s a%u = %s[position * KEYS + %uu];\n  %s b%u = %s[partner * KEYS + %uu];\n",
                      type_name, k, array_name, k, type_name, k, array_name, k);
    }
    str8_list_pushf(scratch.arena, &out, "  uint a_row = %s[position];\n  uint b_row = %s[partner];\n  if (greater(", row_name, row_name);
    for (U32 k = 0; k < key_count; k++)
    {
      str8_list_pushf(scratch.arena, &out, "a%u, ", k);
    }
    str8_list_pushf(scratch.arena, &out, "a_row, ");
    for (U32 k = 0; k < key_count; k++)
    {
      str8_list_pushf(scratch.arena, &out, "b%u, ", k);
    }
    str8_list_pushf(scratch.arena, &out, "b_row) == ascending)\n  {\n");
    for (U32 k = 0; k < key_count; k++)
    {
      str8_list_pushf(scratch.arena, &out, "    %s[position * KEYS + %uu] = b%u;\n    %s[partner * KEYS + %uu] = a%u;\n", array_name, k, k, array_name, k, k);
    }
    str8_list_pushf(scratch.arena, &out, "    %s[position] = b_row;\n    %s[partner] = a_row;\n  }\n}\n", row_name, row_name);
  }

  // tec: mode 0 sorts each tile from the input order, mode 1 is one pass over the whole array for strides longer than a tile, mode 2 finishes a stage inside the tiles
  str8_list_pushf(scratch.arena, &out,
                  "uint\npair_position(uint thread, uint stride)\n{\n"
                  "  uint low = thread & (stride - 1u);\n"
                  "  return ((thread - low) << 1u) + low;\n}\n"
                  "void\nmain()\n{\n"
                  "  uint mode = pc.words[4];\n"
                  "  uint stage = pc.words[6];\n"
                  "  uint stride = pc.words[8];\n"
                  "  uint thread = gl_LocalInvocationID.x;\n"
                  "  if (mode == 1u)\n  {\n"
                  "    uint position = pair_position(gl_GlobalInvocationID.x, stride);\n"
                  "    exchange_global(position, stride, (position & (1u << stage)) == 0u);\n"
                  "    return;\n  }\n"
                  "  uint base = gl_WorkGroupID.x * TILE;\n"
                  "  for (uint half_index = 0u; half_index < 2u; half_index++)\n  {\n"
                  "    uint at = thread + half_index * (TILE / 2u);\n");
  str8_list_pushf(scratch.arena, &out, "    if (mode == 0u)\n    {\n      s_row[at] = base + at;\n    }\n    else\n    {\n      s_row[at] = rows[base + at];\n    }\n");
  str8_list_pushf(scratch.arena, &out, "    for (uint k = 0u; k < KEYS; k++)\n    {\n      s_key[at * KEYS + k] = keys[(base + at) * KEYS + k];\n    }\n  }\n"
                  "  barrier();\n");
  str8_list_pushf(scratch.arena, &out,
                  "  if (mode == 0u)\n  {\n"
                  "    for (uint k = 1u; k <= LOG_TILE; k++)\n    {\n"
                  "      for (uint j = 1u << (k - 1u); j > 0u; j >>= 1u)\n      {\n"
                  "        uint position = pair_position(thread, j);\n"
                  "        exchange_shared(position, j, ((base + position) & (1u << k)) == 0u);\n"
                  "        barrier();\n      }\n    }\n  }\n"
                  "  else\n  {\n"
                  "    for (uint j = TILE / 2u; j > 0u; j >>= 1u)\n    {\n"
                  "      uint position = pair_position(thread, j);\n"
                  "      exchange_shared(position, j, ((base + position) & (1u << stage)) == 0u);\n"
                  "      barrier();\n    }\n  }\n"
                  "  for (uint half_index = 0u; half_index < 2u; half_index++)\n  {\n"
                  "    uint at = thread + half_index * (TILE / 2u);\n"
                  "    rows[base + at] = s_row[at];\n"
                  "    for (uint k = 0u; k < KEYS; k++)\n    {\n      keys[(base + at) * KEYS + k] = s_key[at * KEYS + k];\n    }\n  }\n}\n");

  *out_source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return 1;
}

internal GPU_Kernel*
qk_sort_kernel(Arena* arena, U32 key_count, U32 dir_mask, B32 narrow, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  String8 source = {0};
  if (!qk_sort_source_build(arena, key_count, dir_mask, narrow, &source))
  {
    return 0;
  }
  return qk_kernel_from_source(arena, source, out_name);
}

internal void
qk_join_hash_source(Arena* arena, String8List* out)
{
  str8_list_pushf(arena, out,
                  "uint\nhash_u32(uint x)\n{\n"
                  "  x ^= x >> 16u;\n  x *= 0x7feb352du;\n  x ^= x >> 15u;\n  x *= 0x846ca68bu;\n  x ^= x >> 16u;\n  return x;\n}\n");
}

// tec: the hash join's build pass for a string or a number key, same bindings and hash as hash_join_build_count.comp
internal GPU_Kernel*
qk_join_build_kernel(Arena* arena, B32 string_key, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  str8_list_pushf(scratch.arena, &out,
                  "#version 450\n#extension GL_ARB_gpu_shader_fp64 : enable\n"
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) readonly buffer BuildDataBuf { uint build_data[]; };\n"
                  "layout(std430, binding = 1) readonly buffer BuildOffBuf { uint build_off[]; };\n"
                  "layout(std430, binding = 2) buffer BucketCountBuf { uint bucket_count[]; };\n"
                  "layout(std430, binding = 3) buffer RowBucketBuf { uint row_bucket_out[]; };\n"
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n");
  qk_join_hash_source(scratch.arena, &out);
  str8_list_pushf(scratch.arena, &out, "void\nmain()\n{\n  uint row = gl_GlobalInvocationID.x;\n  if (row >= pc.words[0])\n  {\n    return;\n  }\n  uint h;\n");
  if (string_key)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uint start = build_off[row * 2u];\n  uint end = build_off[(row + 1u) * 2u];\n  h = 2166136261u;\n"
                    "  for (uint i = start; i < end; i++)\n  {\n"
                    "    h = (h ^ ((build_data[i / 4u] >> ((i %% 4u) * 8u)) & 0xffu)) * 16777619u;\n  }\n");
  }
  else
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uvec2 bits = uvec2(build_data[row * 2u + 0u], build_data[row * 2u + 1u]);\n  h = hash_u32(bits.x ^ hash_u32(bits.y));\n");
  }
  str8_list_pushf(scratch.arena, &out, "  uint bucket = h %% pc.words[2];\n  row_bucket_out[row] = bucket;\n  atomicAdd(bucket_count[bucket], 1u);\n}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: the hash join's probe pass, same bindings and push constants as hash_join_probe.comp. a left or anti join emits the row that found nothing
internal GPU_Kernel*
qk_join_probe_hash_kernel(Arena* arena, B32 string_key, B32 emit_unmatched, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  str8_list_pushf(scratch.arena, &out,
                  "#version 450\n#extension GL_ARB_gpu_shader_fp64 : enable\n"
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) readonly buffer BuildDataBuf { uint build_data[]; };\n"
                  "layout(std430, binding = 1) readonly buffer BuildOffBuf { uint build_off[]; };\n"
                  "layout(std430, binding = 2) readonly buffer BucketOffsetsBuf { uint bucket_offsets[]; };\n"
                  "layout(std430, binding = 3) readonly buffer BucketRowsBuf { uint bucket_rows[]; };\n"
                  "layout(std430, binding = 4) readonly buffer ProbeDataBuf { uint probe_data[]; };\n"
                  "layout(std430, binding = 5) readonly buffer ProbeOffBuf { uint probe_off[]; };\n"
                  "layout(std430, binding = 6) buffer OutPairsBuf { uint out_pairs[]; };\n"
                  "layout(std430, binding = 7) buffer OutCountBuf { uint out_count[]; };\n"
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "#define BUILD_NULL 0xffffffffu\n");
  qk_join_hash_source(scratch.arena, &out);
  if (string_key)
  {
    str8_list_pushf(scratch.arena, &out,
                    "uint build_byte(uint i) { return (build_data[i / 4u] >> ((i %% 4u) * 8u)) & 0xffu; }\n"
                    "uint probe_byte(uint i) { return (probe_data[i / 4u] >> ((i %% 4u) * 8u)) & 0xffu; }\n"
                    "bool\nkeys_equal(uint build_row, uint probe_row)\n{\n"
                    "  uint bs = build_off[build_row * 2u];\n  uint blen = build_off[(build_row + 1u) * 2u] - bs;\n"
                    "  uint ps = probe_off[probe_row * 2u];\n  uint plen = probe_off[(probe_row + 1u) * 2u] - ps;\n"
                    "  if (blen != plen)\n  {\n    return false;\n  }\n"
                    "  for (uint i = 0u; i < blen; i++)\n  {\n    if (build_byte(bs + i) != probe_byte(ps + i))\n    {\n      return false;\n    }\n  }\n  return true;\n}\n");
  }
  else
  {
    str8_list_pushf(scratch.arena, &out,
                    "bool\nkeys_equal(uint build_row, uint probe_row)\n{\n"
                    "  return packDouble2x32(uvec2(build_data[build_row * 2u], build_data[build_row * 2u + 1u])) == "
                    "packDouble2x32(uvec2(probe_data[probe_row * 2u], probe_data[probe_row * 2u + 1u]));\n}\n");
  }
  str8_list_pushf(scratch.arena, &out,
                  "void\nmain()\n{\n"
                  "  uint probe_row = gl_GlobalInvocationID.x;\n"
                  "  if (probe_row >= pc.words[0])\n  {\n    return;\n  }\n"
                  "  uint out_capacity = pc.words[8];\n  uint h;\n");
  if (string_key)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uint start = probe_off[probe_row * 2u];\n  uint end = probe_off[(probe_row + 1u) * 2u];\n  h = 2166136261u;\n"
                    "  for (uint i = start; i < end; i++)\n  {\n    h = (h ^ probe_byte(i)) * 16777619u;\n  }\n");
  }
  else
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uvec2 bits = uvec2(probe_data[probe_row * 2u], probe_data[probe_row * 2u + 1u]);\n  h = hash_u32(bits.x ^ hash_u32(bits.y));\n");
  }
  str8_list_pushf(scratch.arena, &out,
                  "  uint bucket = h %% pc.words[2];\n"
                  "  uint first = bucket_offsets[bucket];\n  uint last = bucket_offsets[bucket + 1u];\n"
                  "  bool found_any = false;\n"
                  "  for (uint m = first; m < last; m++)\n  {\n"
                  "    uint build_row = bucket_rows[m];\n"
                  "    if (keys_equal(build_row, probe_row))\n    {\n"
                  "      found_any = true;\n"
                  "      uint pos = atomicAdd(out_count[0], 1u);\n"
                  "      if (pos < out_capacity)\n      {\n"
                  "        out_pairs[pos * 4u + 0u] = probe_row;\n        out_pairs[pos * 4u + 1u] = 0u;\n"
                  "        out_pairs[pos * 4u + 2u] = build_row;\n        out_pairs[pos * 4u + 3u] = 0u;\n      }\n    }\n  }\n");
  if (emit_unmatched)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  if (!found_any)\n  {\n"
                    "    uint pos = atomicAdd(out_count[0], 1u);\n"
                    "    if (pos < out_capacity)\n    {\n"
                    "      out_pairs[pos * 4u + 0u] = probe_row;\n      out_pairs[pos * 4u + 1u] = 0u;\n"
                    "      out_pairs[pos * 4u + 2u] = BUILD_NULL;\n      out_pairs[pos * 4u + 3u] = BUILD_NULL;\n    }\n  }\n");
  }
  str8_list_pushf(scratch.arena, &out, "}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: the streamed probe of a big table against a small one's hash table, fused with its aggregates, same bindings and push constants as join_probe_native.comp.
// the encoding widths of the key and the argument columns, the aggregate functions, the argument of each and which arguments are signed are fixed in the source.
// the encoding bases, the table size and the bloom filter size stay in push constants
internal GPU_Kernel*
qk_join_probe_native_kernel(Arena* arena, U64 shader_widths, U32 arg_count, U32 num_exprs, U32 func_codes, U32 arg_index_codes, U32 arg_signed_bits, String8* out_name)
{
  if (!qk_compiler_available() || num_exprs > 8 || arg_count > 4)
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  str8_list_pushf(scratch.arena, &out,
                  "#version 450\n#extension GL_EXT_control_flow_attributes : enable\n"
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) readonly buffer KeyBuf { uint keys[]; };\n"
                  "layout(std430, binding = 1) readonly buffer Arg0Buf { uint arg0[]; };\n"
                  "layout(std430, binding = 2) readonly buffer Arg1Buf { uint arg1[]; };\n"
                  "layout(std430, binding = 3) readonly buffer Arg2Buf { uint arg2[]; };\n"
                  "layout(std430, binding = 4) readonly buffer Arg3Buf { uint arg3[]; };\n"
                  "layout(std430, binding = 5) readonly buffer TableKeyBuf { uint table_keys[]; };\n"
                  "layout(std430, binding = 6) readonly buffer TableRowBuf { uint table_rows[]; };\n"
                  "layout(std430, binding = 7) buffer CountBuf { uint counts[]; };\n"
                  "layout(std430, binding = 8) buffer SumBuf { uint sum_words[]; };\n"
                  "layout(std430, binding = 9) buffer MinBuf { int min_out[]; };\n"
                  "layout(std430, binding = 10) buffer MaxBuf { int max_out[]; };\n"
                  "layout(std430, binding = 11) readonly buffer BloomBuf { uint bloom[]; };\n"
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "#define UNROLL 8u\n#define ROWS_PER_THREAD 32u\n#define EXPRS %uu\n", num_exprs);

  // tec: one loader per column, the width is 1 (8 bit), 2 (16 bit) or anything else (32 bit)
  U32 base_words[5] = { 10, 3, 5, 9, 11 };
  char* names[5] = { "keys", "arg0", "arg1", "arg2", "arg3" };
  for (U32 c = 0; c < 5; c++)
  {
    U32 width = (U32)((shader_widths >> (c == 0 ? 0 : 4 + 4 * (c - 1))) & 0xf);
    if (c > 0 && c - 1 >= arg_count)
    {
      width = 0;
    }
    if (width == 1)
    {
      str8_list_pushf(scratch.arena, &out, "uint load_%u(uint row)\n{\n  return pc.words[%u] + ((%s[row >> 2u] >> ((row & 3u) * 8u)) & 0xffu);\n}\n", c, base_words[c], names[c]);
    }
    else if (width == 2)
    {
      str8_list_pushf(scratch.arena, &out, "uint load_%u(uint row)\n{\n  return pc.words[%u] + ((%s[row >> 1u] >> ((row & 1u) * 16u)) & 0xffffu);\n}\n", c, base_words[c], names[c]);
    }
    else
    {
      str8_list_pushf(scratch.arena, &out, "uint load_%u(uint row)\n{\n  return %s[row];\n}\n", c, names[c]);
    }
  }
  str8_list_pushf(scratch.arena, &out,
                  "bool\nbloom_maybe(uint key, uint bloom_log2)\n{\n"
                  "  uint word = (key * 0x9E3779B1u) >> (32u - bloom_log2);\n"
                  "  uint h2 = key * 0x85EBCA6Bu;\n"
                  "  uint bits = (1u << ((h2 >> 27u) & 31u)) | (1u << ((h2 >> 22u) & 31u));\n"
                  "  return (bloom[word] & bits) == bits;\n}\n"
                  "uint\nlookup(uint key, uint mask, uint log2_size)\n{\n"
                  "  uint slot = (key * 0x9E3779B1u) >> (32u - log2_size);\n"
                  "  for (;;)\n  {\n"
                  "    uint row_plus_one = table_rows[slot];\n"
                  "    if (row_plus_one == 0u)\n    {\n      return 0u;\n    }\n"
                  "    if (table_keys[slot] == key)\n    {\n      return row_plus_one;\n    }\n"
                  "    slot = (slot + 1u) & mask;\n  }\n}\n"
                  "void\nadd_match(uint dim_row, uint row)\n{\n  atomicAdd(counts[dim_row], 1u);\n");
  for (U32 e = 0; e < num_exprs; e++)
  {
    U32 func = (func_codes >> (e * 4)) & 0xf;
    U32 arg = (arg_index_codes >> (e * 4)) & 0xf;
    if (func == 0)
    {
      continue;
    }
    if (arg >= arg_count)
    {
      scratch_end(scratch);
      return 0;
    }
    B32 is_signed = (arg_signed_bits >> arg) & 1;
    str8_list_pushf(scratch.arena, &out, "  {\n    uint word = load_%u(row);\n    uint index = dim_row * EXPRS + %uu;\n", arg + 1, e);
    if (func == 1 || func == 2)
    {
      str8_list_pushf(scratch.arena, &out,
                      "    uint before = atomicAdd(sum_words[index * 2u], word);\n"
                      "    uint high_add = %s + ((before + word < before) ? 1u : 0u);\n"
                      "    if (high_add != 0u)\n    {\n      atomicAdd(sum_words[index * 2u + 1u], high_add);\n    }\n",
                      is_signed ? "((int(word) < 0) ? 0xffffffffu : 0u)" : "0u");
    }
    else if (func == 3)
    {
      str8_list_pushf(scratch.arena, &out, "    atomicMin(min_out[index], int(word));\n");
    }
    else if (func == 4)
    {
      str8_list_pushf(scratch.arena, &out, "    atomicMax(max_out[index], int(word));\n");
    }
    else
    {
      scratch_end(scratch);
      return 0;
    }
    str8_list_pushf(scratch.arena, &out, "  }\n");
  }
  str8_list_pushf(scratch.arena, &out,
                  "}\n"
                  "void\nmain()\n{\n"
                  "  uint row_count = pc.words[0];\n"
                  "  uint mask = pc.words[8];\n  uint log2_size = pc.words[12];\n  uint bloom_log2 = pc.words[13];\n"
                  "  uint first = gl_WorkGroupID.x * (256u * ROWS_PER_THREAD) + gl_LocalInvocationID.x;\n"
                  "  uint last = min(gl_WorkGroupID.x * (256u * ROWS_PER_THREAD) + 256u * ROWS_PER_THREAD, row_count);\n"
                  "  for (uint row = first; row < last; row += 256u * UNROLL)\n  {\n"
                  "    uint keys_in_flight[UNROLL];\n"
                  "    [[unroll]] for (uint u = 0u; u < UNROLL; u++)\n    {\n"
                  "      uint at = row + u * 256u;\n      keys_in_flight[u] = (at < last) ? load_0(at) : 0u;\n    }\n"
                  "    bool maybe[UNROLL];\n"
                  "    [[unroll]] for (uint u = 0u; u < UNROLL; u++)\n    {\n"
                  "      maybe[u] = (row + u * 256u < last) && bloom_maybe(keys_in_flight[u], bloom_log2);\n    }\n"
                  "    [[unroll]] for (uint u = 0u; u < UNROLL; u++)\n    {\n"
                  "      if (maybe[u])\n      {\n"
                  "        uint found = lookup(keys_in_flight[u], mask, log2_size);\n"
                  "        if (found != 0u)\n        {\n          add_match(found - 1u, row + u * 256u);\n        }\n      }\n    }\n  }\n}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: a HAVING condition over the grouped result, evaluated for every group at once. keep[i] is 1 when group i passes. binding 0 keep, binding 1 the literals (doubles),
// bindings 2 to 9 the result columns the condition compares (doubles). condition is a GLSL bool expression over v0..v7 (the column values of group i) and lit[0..15].
// the literals are in a buffer so conditions that differ only in their constants share a kernel. push constant slot 0 is the group count
internal GPU_Kernel*
qk_having_kernel(Arena* arena, String8 condition, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  str8_list_pushf(scratch.arena, &out,
                  "#version 450\n#extension GL_ARB_gpu_shader_fp64 : enable\n"
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) buffer KeepBuf { uint keep[]; };\n"
                  "layout(std430, binding = 1) readonly buffer LitBuf { double lit[]; };\n");
  for (U32 c = 0; c < 8; c++)
  {
    str8_list_pushf(scratch.arena, &out, "layout(std430, binding = %u) readonly buffer Col%uBuf { double col%u[]; };\n", 2 + c, c, c);
  }
  str8_list_pushf(scratch.arena, &out,
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "void\nmain()\n{\n"
                  "  uint i = gl_GlobalInvocationID.x;\n"
                  "  if (i >= pc.words[0])\n  {\n    return;\n  }\n");
  for (U32 c = 0; c < 8; c++)
  {
    str8_list_pushf(scratch.arena, &out, "  double v%u = col%u[i];\n", c, c);
  }
  str8_list_pushf(scratch.arena, &out, "  keep[i] = (%.*s) ? 1u : 0u;\n}\n", str8_varg(condition));
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: the group slot assignment of aggregate_assign.comp, an open addressed hash table of group keys claimed with compare and swap.
// the number of key columns and which are strings are in the source, so the hash and the comparison of every column are straight code. same bindings and push constants
internal GPU_Kernel*
qk_hash_assign_kernel(Arena* arena, U32 num_group_cols, U32 string_mask, String8* out_name)
{
  if (!qk_compiler_available() || num_group_cols == 0 || num_group_cols > 4)
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  str8_list_pushf(scratch.arena, &out,
                  "#version 450\n#extension GL_ARB_gpu_shader_fp64 : enable\n"
                  "layout(local_size_x = 256) in;\n#define EMPTY 0xffffffffu\n"
                  "layout(std430, binding = 0) buffer OwnerBuf { uint owner_row_index[]; };\n"
                  "layout(std430, binding = 1) buffer CountBuf { uint slot_row_count[]; };\n"
                  "layout(std430, binding = 2) buffer RowSlotBuf { uint row_slot_out[]; };\n"
                  "layout(std430, binding = 3) buffer OverflowBuf { uint overflow_count[]; };\n");
  for (U32 c = 0; c < 4; c++)
  {
    str8_list_pushf(scratch.arena, &out,
                    "layout(std430, binding = %u) readonly buffer Col%uDataBuf { uint col%u_data[]; };\n"
                    "layout(std430, binding = %u) readonly buffer Col%uOffBuf { uint col%u_off[]; };\n",
                    4 + c * 2, c, c, 5 + c * 2, c, c);
  }
  str8_list_pushf(scratch.arena, &out,
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "uint\nhash_u32(uint x)\n{\n  x ^= x >> 16u;\n  x *= 0x7feb352du;\n  x ^= x >> 15u;\n  x *= 0x846ca68bu;\n  x ^= x >> 16u;\n  return x;\n}\n");
  for (U32 c = 0; c < num_group_cols; c++)
  {
    if ((string_mask >> c) & 1)
    {
      str8_list_pushf(scratch.arena, &out,
                      "uint byte_%u(uint i) { return (col%u_data[i / 4u] >> ((i %% 4u) * 8u)) & 0xffu; }\n"
                      "uint hash_%u(uint row)\n{\n"
                      "  uint start = col%u_off[row * 2u];\n  uint end = col%u_off[(row + 1u) * 2u];\n  uint hh = 2166136261u;\n"
                      "  for (uint i = start; i < end; i++)\n  {\n    hh = (hh ^ byte_%u(i)) * 16777619u;\n  }\n  return hh;\n}\n"
                      "bool equal_%u(uint a, uint b)\n{\n"
                      "  uint start_a = col%u_off[a * 2u];\n  uint len_a = col%u_off[(a + 1u) * 2u] - start_a;\n"
                      "  uint start_b = col%u_off[b * 2u];\n  uint len_b = col%u_off[(b + 1u) * 2u] - start_b;\n"
                      "  if (len_a != len_b)\n  {\n    return false;\n  }\n"
                      "  for (uint i = 0u; i < len_a; i++)\n  {\n    if (byte_%u(start_a + i) != byte_%u(start_b + i))\n    {\n      return false;\n    }\n  }\n  return true;\n}\n",
                      c, c, c, c, c, c, c, c, c, c, c, c, c);
    }
    else
    {
      str8_list_pushf(scratch.arena, &out,
                      "double num_%u(uint row) { return packDouble2x32(uvec2(col%u_data[row * 2u], col%u_data[row * 2u + 1u])); }\n"
                      "uint hash_%u(uint row)\n{\n  uvec2 bits = unpackDouble2x32(num_%u(row));\n  return hash_u32(bits.x ^ hash_u32(bits.y));\n}\n"
                      "bool equal_%u(uint a, uint b)\n{\n  return num_%u(a) == num_%u(b);\n}\n",
                      c, c, c, c, c, c, c, c);
    }
  }
  str8_list_pushf(scratch.arena, &out,
                  "void\nmain()\n{\n"
                  "  uint row = gl_GlobalInvocationID.x;\n"
                  "  if (row >= pc.words[0])\n  {\n    return;\n  }\n"
                  "  uint K = pc.words[4];\n  uint h = 2166136261u;\n");
  for (U32 c = 0; c < num_group_cols; c++)
  {
    str8_list_pushf(scratch.arena, &out, "  h = hash_u32(h ^ hash_%u(row));\n", c);
  }
  str8_list_pushf(scratch.arena, &out,
                  "  uint probe_start = (h %% pc.words[2]) * K;\n"
                  "  uint claimed_slot = EMPTY;\n"
                  "  for (uint p = 0u; p < K; p++)\n  {\n"
                  "    uint slot = probe_start + p;\n"
                  "    uint prev = atomicCompSwap(owner_row_index[slot], EMPTY, row);\n"
                  "    if (prev == EMPTY)\n    {\n      claimed_slot = slot;\n      break;\n    }\n"
                  "    bool same = true");
  for (U32 c = 0; c < num_group_cols; c++)
  {
    str8_list_pushf(scratch.arena, &out, " && equal_%u(row, prev)", c);
  }
  str8_list_pushf(scratch.arena, &out,
                  ";\n    if (same)\n    {\n      claimed_slot = slot;\n      break;\n    }\n  }\n"
                  "  if (claimed_slot == EMPTY)\n  {\n    atomicAdd(overflow_count[0], 1u);\n    return;\n  }\n"
                  "  row_slot_out[row] = claimed_slot;\n  atomicAdd(slot_row_count[claimed_slot], 1u);\n}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: the atomic hash aggregation of aggregate_hash_reduce_f32.comp (fixed false, F32 arguments, float sums by compare and swap) and aggregate_hash_reduce_fixed.comp (fixed true,
// F64 arguments scaled to whole numbers by their power of ten). same bindings and push constants. the expression count, the function of each, and for fixed the power of ten of each are in the source
internal GPU_Kernel*
qk_hash_reduce_kernel(Arena* arena, B32 fixed, U32 num_exprs, U32 func_codes, U32 scales, String8* out_name)
{
  if (!qk_compiler_available() || num_exprs == 0 || num_exprs > 8)
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  char* arg_type = fixed ? "double" : "float";
  str8_list_pushf(scratch.arena, &out, "#version 450\n");
  if (fixed)
  {
    str8_list_pushf(scratch.arena, &out, "#extension GL_ARB_gpu_shader_fp64 : enable\n");
  }
  str8_list_pushf(scratch.arena, &out,
                  "layout(local_size_x = 256) in;\n"
                  "#define EMPTY 0xffffffffu\n"
                  "layout(std430, binding = 0) readonly buffer RowSlotBuf { uint row_slot[]; };\n"
                  "layout(std430, binding = 1) readonly buffer SlotGroupBuf { uint slot_group[]; };\n"
                  "layout(std430, binding = 2) buffer SumBuf { uint sum_words[]; };\n"
                  "layout(std430, binding = 3) buffer CountBuf { uint count_out[]; };\n"
                  "layout(std430, binding = 4) buffer MinBuf { %s min_out[]; };\n"
                  "layout(std430, binding = 5) buffer MaxBuf { %s max_out[]; };\n",
                  fixed ? "int" : "uint", fixed ? "int" : "uint");
  for (U32 e = 0; e < 8; e++)
  {
    str8_list_pushf(scratch.arena, &out, "layout(std430, binding = %u) readonly buffer Arg%uBuf { %s arg%u[]; };\n", 6 + e, e, arg_type, e);
  }
  str8_list_pushf(scratch.arena, &out, "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n");
  if (!fixed)
  {
    str8_list_pushf(scratch.arena, &out,
                    "void\natomic_add_f32(uint index, float val)\n{\n"
                    "  uint old = sum_words[index];\n  uint assumed;\n"
                    "  do\n  {\n    assumed = old;\n    old = atomicCompSwap(sum_words[index], assumed, floatBitsToUint(uintBitsToFloat(assumed) + val));\n  } while (assumed != old);\n}\n"
                    "void\natomic_min_f32(uint index, float val)\n{\n"
                    "  uint old = min_out[index];\n  uint assumed;\n"
                    "  do\n  {\n    if (val >= uintBitsToFloat(old))\n    {\n      return;\n    }\n    assumed = old;\n    old = atomicCompSwap(min_out[index], assumed, floatBitsToUint(val));\n  } while (assumed != old);\n}\n"
                    "void\natomic_max_f32(uint index, float val)\n{\n"
                    "  uint old = max_out[index];\n  uint assumed;\n"
                    "  do\n  {\n    if (val <= uintBitsToFloat(old))\n    {\n      return;\n    }\n    assumed = old;\n    old = atomicCompSwap(max_out[index], assumed, floatBitsToUint(val));\n  } while (assumed != old);\n}\n");
  }
  str8_list_pushf(scratch.arena, &out,
                  "void\nmain()\n{\n"
                  "  uint row = gl_GlobalInvocationID.x;\n"
                  "  if (row >= pc.words[0])\n  {\n    return;\n  }\n"
                  "  uint slot = row_slot[row];\n"
                  "  if (slot == EMPTY)\n  {\n    return;\n  }\n"
                  "  uint g = slot_group[slot];\n"
                  "  if (g == EMPTY)\n  {\n    return;\n  }\n");
  for (U32 e = 0; e < num_exprs; e++)
  {
    U32 func = (func_codes >> (e * 4)) & 0xf;
    if (func > 4)
    {
      scratch_end(scratch);
      return 0;
    }
    str8_list_pushf(scratch.arena, &out, "  {\n    uint index = g * %uu + %uu;\n    atomicAdd(count_out[index], 1u);\n", num_exprs, e);
    if (func == 1 || func == 2 || func == 3 || func == 4)
    {
      if (fixed)
      {
        U32 exponent = (scales >> (e * 4)) & 0xf;
        if (exponent > 6)
        {
          scratch_end(scratch);
          return 0;
        }
        F64 factor = 1.0;
        for (U32 p = 0; p < exponent; p++)
        {
          factor *= 10.0;
        }
        str8_list_pushf(scratch.arena, &out, "    int q = int(floor(arg%u[row] * %.1flf + 0.5lf));\n", e, factor);
        if (func == 1 || func == 2)
        {
          str8_list_pushf(scratch.arena, &out,
                          "    uint low = uint(q);\n"
                          "    uint before = atomicAdd(sum_words[index * 2u], low);\n"
                          "    uint high_add = ((q < 0) ? 0xffffffffu : 0u) + ((before + low < before) ? 1u : 0u);\n"
                          "    if (high_add != 0u)\n    {\n      atomicAdd(sum_words[index * 2u + 1u], high_add);\n    }\n");
        }
        else if (func == 3)
        {
          str8_list_pushf(scratch.arena, &out, "    atomicMin(min_out[index], q);\n");
        }
        else
        {
          str8_list_pushf(scratch.arena, &out, "    atomicMax(max_out[index], q);\n");
        }
      }
      else
      {
        char* op = func == 3 ? "atomic_min_f32" : (func == 4 ? "atomic_max_f32" : "atomic_add_f32");
        str8_list_pushf(scratch.arena, &out, "    %s(index, arg%u[row]);\n", op, e);
      }
    }
    str8_list_pushf(scratch.arena, &out, "  }\n");
  }
  str8_list_pushf(scratch.arena, &out, "}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: the member list reduce of aggregate_reduce.comp and aggregate_reduce_f32.comp (narrow) for chunks of a few rows. the precompiled kernel gives each chunk a workgroup of 256
// threads and folds their partials through shared memory once per expression, which costs far more than the work when a group has a handful of rows. here one thread reduces a whole chunk
// from registers. the bindings, push constants and the partials layout are the same, so the host dispatches total_chunks threads instead of total_chunks workgroups.
// COUNT, SUM, AVG, MIN and MAX only, a group with the approximate functions keeps the precompiled kernel
internal GPU_Kernel*
qk_member_reduce_kernel(Arena* arena, B32 narrow, U32 num_exprs, U32 func_codes, String8* out_name)
{
  if (!qk_compiler_available() || num_exprs == 0 || num_exprs > 8)
  {
    return 0;
  }
  for (U32 e = 0; e < num_exprs; e++)
  {
    if (((func_codes >> (e * 4)) & 0xf) > 4)
    {
      return 0;
    }
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  str8_list_pushf(scratch.arena, &out,
                  "#version 450\n"
                  "#extension GL_ARB_gpu_shader_fp64 : enable\n"
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) readonly buffer MembersBuf { uint group_member_rows[]; };\n"
                  "layout(std430, binding = 1) readonly buffer ChunkRangeBuf { uint chunk_range[]; };\n"
                  "layout(std430, binding = 2) readonly buffer ChunkGroupBuf { uint chunk_group[]; };\n"
                  "layout(std430, binding = 3) buffer ReprBuf { uint representative_row_out[]; };\n"
                  "layout(std430, binding = 4) buffer PartialsBuf { double partials_out[]; };\n");
  for (U32 e = 0; e < num_exprs; e++)
  {
    str8_list_pushf(scratch.arena, &out, "layout(std430, binding = %u) readonly buffer Arg%uBuf { %s arg%u[]; };\n", 5 + e, e, narrow ? "float" : "double", e);
  }
  str8_list_pushf(scratch.arena, &out,
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "void\nmain()\n{\n"
                  "  uint chunk_id = gl_GlobalInvocationID.x;\n"
                  "  if (chunk_id >= pc.words[0])\n  {\n    return;\n  }\n"
                  "  uint start = chunk_range[chunk_id * 2u];\n"
                  "  uint end = chunk_range[chunk_id * 2u + 1u];\n"
                  "  representative_row_out[chunk_group[chunk_id]] = group_member_rows[start];\n");
  for (U32 e = 0; e < num_exprs; e++)
  {
    str8_list_pushf(scratch.arena, &out, "  double acc%u = 0.0lf;\n  double mn%u = 1.0e300lf;\n  double mx%u = -1.0e300lf;\n", e, e, e);
  }
  str8_list_pushf(scratch.arena, &out, "  for (uint m = start; m < end; m++)\n  {\n    uint row = group_member_rows[m];\n");
  for (U32 e = 0; e < num_exprs; e++)
  {
    U32 func = (func_codes >> (e * 4)) & 0xf;
    if (func == 1 || func == 2)
    {
      str8_list_pushf(scratch.arena, &out, "    acc%u += double(arg%u[row]);\n", e, e);
    }
    else if (func == 3)
    {
      str8_list_pushf(scratch.arena, &out, "    mn%u = min(mn%u, double(arg%u[row]));\n", e, e, e);
    }
    else if (func == 4)
    {
      str8_list_pushf(scratch.arena, &out, "    mx%u = max(mx%u, double(arg%u[row]));\n", e, e, e);
    }
  }
  str8_list_pushf(scratch.arena, &out, "  }\n  double count = double(end - start);\n");
  for (U32 e = 0; e < num_exprs; e++)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  {\n    uint base = (chunk_id * %uu + %uu) * 4u;\n"
                    "    partials_out[base] = acc%u;\n    partials_out[base + 1u] = count;\n    partials_out[base + 2u] = mn%u;\n    partials_out[base + 3u] = mx%u;\n  }\n",
                    num_exprs, e, e, e, e);
  }
  str8_list_pushf(scratch.arena, &out, "}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: the slot compaction of aggregate_compact.comp, same bindings, push constants and modes. each thread takes QK_COMPACT_PER_THREAD neighbouring slots instead of one, so a block of 256
// threads covers QK_COMPACT_BLOCK slots, which divides the barriers and the block totals by that factor. the dense ids written are the same. the host dispatches one workgroup per block
#define QK_COMPACT_PER_THREAD 4
#define QK_COMPACT_BLOCK (256 * QK_COMPACT_PER_THREAD)
internal GPU_Kernel*
qk_compact_kernel(Arena* arena, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  String8 source = push_str8f(arena,
    "#version 450\n"
    "layout(local_size_x = 256) in;\n"
    "#define PER %u\n"
    "#define BLOCK %u\n"
    "layout(std430, binding = 0) readonly buffer CountBuf { uint slot_row_count[]; };\n"
    "layout(std430, binding = 1) readonly buffer OwnerBuf { uint owner_row_index[]; };\n"
    "layout(std430, binding = 2) buffer OffsetsBuf { uint offsets[]; };\n"
    "layout(std430, binding = 3) buffer BlockSumBuf { uint block_sums[]; };\n"
    "layout(std430, binding = 4) buffer OutOwnerBuf { uint out_owner[]; };\n"
    "layout(std430, binding = 5) buffer OutCountBuf { uint out_count[]; };\n"
    "layout(std430, binding = 6) buffer TotalBuf { uint total[]; };\n"
    "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
    "shared uint scan[256];\n"
    "void\nscan_shared(uint thread)\n{\n"
    "  for (uint stride = 1u; stride < 256u; stride <<= 1u)\n  {\n"
    "    uint added = (thread >= stride) ? scan[thread - stride] : 0u;\n    barrier();\n"
    "    scan[thread] += added;\n    barrier();\n  }\n}\n"
    "void\nmain()\n{\n"
    "  uint n = pc.words[0];\n"
    "  uint mode = pc.words[2];\n"
    "  uint thread = gl_LocalInvocationID.x;\n"
    "  uint block_count = (n + BLOCK - 1u) / BLOCK;\n"
    "  if (mode == 0u)\n  {\n"
    "    if (gl_WorkGroupID.x >= block_count)\n    {\n      return;\n    }\n"
    "    uint first = gl_WorkGroupID.x * BLOCK + thread * PER;\n"
    "    uint flags[PER];\n    uint sum = 0u;\n"
    "    for (uint i = 0u; i < PER; i++)\n    {\n"
    "      uint index = first + i;\n"
    "      flags[i] = (index < n && slot_row_count[index] != 0u) ? 1u : 0u;\n      sum += flags[i];\n    }\n"
    "    scan[thread] = sum;\n    barrier();\n    scan_shared(thread);\n"
    "    uint inclusive = scan[thread];\n"
    "    uint running = inclusive - sum;\n"
    "    for (uint i = 0u; i < PER; i++)\n    {\n"
    "      uint index = first + i;\n"
    "      if (index < n)\n      {\n        offsets[index] = running;\n      }\n"
    "      running += flags[i];\n    }\n"
    "    if (thread == 255u)\n    {\n      block_sums[gl_WorkGroupID.x] = inclusive;\n    }\n  }\n"
    "  else if (mode == 1u)\n  {\n"
    "    uint chunk = (block_count + 255u) / 256u;\n"
    "    uint start = min(thread * chunk, block_count);\n"
    "    uint end = min(start + chunk, block_count);\n"
    "    uint sum = 0u;\n"
    "    for (uint i = start; i < end; i++)\n    {\n      sum += block_sums[i];\n    }\n"
    "    scan[thread] = sum;\n    barrier();\n    scan_shared(thread);\n"
    "    uint inclusive = scan[thread];\n    uint running = inclusive - sum;\n"
    "    for (uint i = start; i < end; i++)\n    {\n"
    "      uint block_total = block_sums[i];\n      block_sums[i] = running;\n      running += block_total;\n    }\n"
    "    if (thread == 255u)\n    {\n      offsets[n] = inclusive;\n      total[0] = inclusive;\n    }\n  }\n"
    "  else\n  {\n"
    "    if (gl_WorkGroupID.x >= block_count)\n    {\n      return;\n    }\n"
    "    uint base = block_sums[gl_WorkGroupID.x];\n"
    "    uint first = gl_WorkGroupID.x * BLOCK + thread * PER;\n"
    "    for (uint i = 0u; i < PER; i++)\n    {\n"
    "      uint index = first + i;\n"
    "      if (index < n)\n      {\n"
    "        uint id = offsets[index] + base;\n        offsets[index] = id;\n"
    "        uint count = slot_row_count[index];\n"
    "        if (count != 0u)\n        {\n          out_owner[id] = owner_row_index[index];\n          out_count[id] = count;\n        }\n"
    "      }\n    }\n  }\n}\n",
    (U32)QK_COMPACT_PER_THREAD, (U32)QK_COMPACT_BLOCK);
  return qk_kernel_from_source(arena, source, out_name);
}

// tec: the device side column gathers of selection_gather.comp, one kernel per mode. mode 0 reads a 64 bit value per row, 1 a 32 bit one, 2 the rows of a few positions,
// 3 resolves rows through a map and 4 adds an offset to rows. bindings and push constants as that kernel, the mode push constant is not read
internal GPU_Kernel*
qk_gather_kernel(Arena* arena, U32 mode, String8* out_name)
{
  if (!qk_compiler_available() || mode > 4)
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  str8_list_pushf(scratch.arena, &out,
                  "#version 450\n"
                  "layout(local_size_x = 256) in;\n"
                  "#define NO_ROW 0xffffffffu\n"
                  "layout(std430, binding = 0) readonly buffer RowsBuf { uint rows[]; };\n"
                  "layout(std430, binding = 1) readonly buffer SourceBuf { uint source[]; };\n"
                  "layout(std430, binding = 2) buffer DestBuf { uint dest[]; };\n"
                  "layout(std430, binding = 3) readonly buffer PositionBuf { uint positions[]; };\n"
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "void\nmain()\n{\n"
                  "  uint i = gl_GlobalInvocationID.x;\n"
                  "  if (i >= pc.words[0])\n  {\n    return;\n  }\n"
                  "  uint stride_words = pc.words[2];\n  uint row_offset = pc.words[4];\n  uint word_offset = pc.words[10];\n");
  if (mode == 2)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uint index = rows[positions[i] * stride_words + word_offset];\n"
                    "  dest[i] = (index == NO_ROW) ? NO_ROW : (index + row_offset);\n");
  }
  else if (mode == 4)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uint index = rows[i * stride_words + word_offset];\n"
                    "  dest[i] = (index == NO_ROW) ? NO_ROW : (index + row_offset);\n");
  }
  else if (mode == 3)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uint index = rows[i * stride_words + word_offset];\n"
                    "  uint mapped = (index == NO_ROW) ? NO_ROW : source[index * pc.words[8]];\n"
                    "  dest[i] = (mapped == NO_ROW) ? NO_ROW : (mapped + pc.words[12]);\n");
  }
  else if (mode == 0)
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uint index = rows[i * stride_words + word_offset];\n"
                    "  bool has_row = index != NO_ROW;\n  uint row = index + row_offset;\n"
                    "  dest[i * 2u + 0u] = has_row ? source[row * 2u + 0u] : 0u;\n"
                    "  dest[i * 2u + 1u] = has_row ? source[row * 2u + 1u] : 0u;\n");
  }
  else
  {
    str8_list_pushf(scratch.arena, &out,
                    "  uint index = rows[i * stride_words + word_offset];\n"
                    "  dest[i] = (index != NO_ROW) ? source[index + row_offset] : 0u;\n");
  }
  str8_list_pushf(scratch.arena, &out, "}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

internal GPU_Kernel*
qk_window_flags_kernel(Arena* arena, U32 key_count, U32 partition_count, B32 narrow, String8* out_name)
{
  if (!qk_compiler_available() || key_count == 0 || key_count > 4 || partition_count > key_count)
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  Temp scratch = scratch_begin(&arena, 1);
  String8List out = {0};
  char* type_name = narrow ? "float" : "double";
  str8_list_pushf(scratch.arena, &out, "#version 450\n");
  if (!narrow)
  {
    str8_list_pushf(scratch.arena, &out, "#extension GL_ARB_gpu_shader_fp64 : enable\n");
  }
  str8_list_pushf(scratch.arena, &out,
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) readonly buffer KeyBuf { %s keys[]; };\n"
                  "layout(std430, binding = 1) buffer PartitionFlagBuf { uint partition_flag[]; };\n"
                  "layout(std430, binding = 2) buffer PeerFlagBuf { uint peer_flag[]; };\n"
                  "layout(push_constant) uniform PushConstants { uint words[16]; } pc;\n"
                  "void\nmain()\n{\n"
                  "  uint i = gl_GlobalInvocationID.x;\n"
                  "  if (i >= pc.words[0])\n  {\n    return;\n  }\n"
                  "  if (i == 0u)\n  {\n    partition_flag[i] = 1u;\n    peer_flag[i] = 1u;\n    return;\n  }\n"
                  "  bool part_diff = false;\n  bool order_diff = false;\n",
                  type_name);
  for (U32 k = 0; k < key_count; k++)
  {
    str8_list_pushf(scratch.arena, &out, "  if (keys[i * %uu + %uu] != keys[(i - 1u) * %uu + %uu])\n  {\n    %s = true;\n  }\n",
                    key_count, k, key_count, k, k < partition_count ? "part_diff" : "order_diff");
  }
  str8_list_pushf(scratch.arena, &out, "  partition_flag[i] = part_diff ? 1u : 0u;\n  peer_flag[i] = (part_diff || order_diff) ? 1u : 0u;\n}\n");
  String8 source = str8_list_join(arena, &out, 0);
  scratch_end(scratch);
  return qk_kernel_from_source(arena, source, out_name);
}

internal void
qk_last_sizes(U64* out_source_bytes, U64* out_spirv_bytes)
{
  *out_source_bytes = g_qk_state.last_source_bytes;
  *out_spirv_bytes = g_qk_state.last_spirv_bytes;
}

internal void
qk_stats(U64* out_requested, U64* out_compiled, U64* out_failures, U64* out_compile_microseconds)
{
  *out_requested = g_qk_state.kernels_requested;
  *out_compiled = g_qk_state.kernels_compiled;
  *out_failures = g_qk_state.compile_failures;
  *out_compile_microseconds = g_qk_state.compile_microseconds;
}

internal U64
qk_spec_request_count(void)
{
  return g_qk_state.spec_requests;
}
