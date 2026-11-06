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


internal String8
qk_kernel_name(U64 key)
{
  QK_State* state = &g_qk_state;
  for (U32 i = 0; i < state->name_count; i++)
  {
    if (state->names[i].key == key)
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
  snprintf(state->names[slot].text, sizeof(state->names[slot].text), "qk_scan_%016llx", key);
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
    log_info("per query kernels: shaderc_shared.dll was not found, scans keep using the interpreter");
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

internal String8
qk_spirv_compile(Arena* arena, String8 source)
{
  String8 spirv = {0};
  if (!qk_compiler_available())
  {
    return spirv;
  }
  QK_State* state = &g_qk_state;
  U64 start = os_now_microseconds();
  void* result = state->compile(state->compiler, (char*)source.str, source.size, shaderc_compute_shader, "per_query_scan.comp", "main", state->options);
  state->compile_microseconds += os_now_microseconds() - start;
  if (!result)
  {
    log_error("per query kernels: shaderc returned no result");
    return spirv;
  }
  if (state->result_status(result) != shaderc_compilation_status_success)
  {
    log_error("per query kernels: shader compile failed: %s", state->result_message(result));
    state->result_release(result);
    return spirv;
  }
  U64 length = state->result_length(result);
  spirv.str = push_array_no_zero(arena, U8, length);
  spirv.size = length;
  MemoryCopy(spirv.str, state->result_bytes(result), length);
  state->result_release(result);
  return spirv;
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
qk_emit_simple_leaf(Arena* arena, String8List* out, U32 l, U32 slot, U32 coltype, U32 op, B32 is_int)
{
  U32 base = 1 + l * 6;
  str8_list_pushf(arena, out,
                  "  if (live != 0u)\n"
                  "  {\n"
                  "    uint flags = leaf_words[%uu];\n"
                  "    uint width = (flags >> 8u) & 0xffu;\n",
                  base + 1);
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

  String8List out = {0};
  qk_emit_simple_header(scratch.arena, &out);
  qk_emit_simple_columns(scratch.arena, &out, slot_used, slot_used);
  str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_simple_compare));
  str8_list_push(scratch.arena, &out, str8_cstring(qk_glsl_simple_helpers));
  qk_emit_simple_prologue(scratch.arena, &out);
  for (U32 l = 0; l < leaf_count; l++)
  {
    U32 base = 1 + l * 6;
    qk_emit_simple_leaf(scratch.arena, &out, l, words[base + 0], words[base + 1] & 0xffu, words[base + 2], (words[base + 1] & QE_SCAN_INT_FLAG) != 0);
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
qk_scan_source_build(Arena* arena, QE_BytecodeProgram* prog, QK_HybridInfo* hybrid, String8* out_source)
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
  B32 hybrid_allowed = hybrid && hybrid->allow && !prog->has_score_output;
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
    str8_list_pushf(scratch.arena, &out,
                    "layout(std430, binding = 3) buffer OutIndicesBuf { uint out_indices[]; };\n"
                    "layout(std430, binding = 4) buffer OutCountBuf { uint out_count[]; };\n"
                    "layout(std430, binding = 5) readonly buffer SkipBuf { uint skip_bits[]; };\n");
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
      qk_emit_simple_leaf(scratch.arena, &out, l, leaf->slot, leaf->coltype, leaf->op, qk_leaf_is_int(leaf->coltype));
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
  U64 key = u64_hash_from_str8(source);
  for (U32 i = 0; i < state->failed_key_count; i++)
  {
    if (state->failed_keys[i] == key)
    {
      return 0;
    }
  }

  String8 name = qk_kernel_name(key);
  GPU_Kernel* kernel = gpu_kernel_alloc_from_spirv(name, str8_zero());
  if (!kernel)
  {
    if (settings_u64(str8_lit("QE_PERQUERY_LOG_SOURCE"), 0) != 0)
    {
      log_debug("per query kernels: source for %.*s\n%.*s", str8_varg(name), str8_varg(source));
    }
    String8 spirv = qk_spirv_compile(arena, source);
    if (spirv.size > 0)
    {
      kernel = gpu_kernel_alloc_from_spirv(name, spirv);
    }
    if (!kernel)
    {
      state->compile_failures += 1;
      if (state->failed_key_count < QK_FAILED_KEY_CAP)
      {
        state->failed_keys[state->failed_key_count++] = key;
      }
      return 0;
    }
    state->kernels_compiled += 1;
  }
  if (out_name)
  {
    *out_name = name;
  }
  return kernel;
}

internal GPU_Kernel*
qk_simple_kernel(Arena* arena, U32* words, String8* out_name)
{
  if (!qk_compiler_available())
  {
    return 0;
  }
  g_qk_state.kernels_requested += 1;
  String8 source = {0};
  if (!qk_simple_source_build(arena, words, &source))
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
  if (!qk_scan_source_build(arena, prog, hybrid, &source))
  {
    log_debug("per query kernels: the bytecode has an operator the generator does not handle, using the interpreter");
    return 0;
  }
  return qk_kernel_from_source(arena, source, out_name);
}

internal void
qk_stats(U64* out_requested, U64* out_compiled, U64* out_failures, U64* out_compile_microseconds)
{
  *out_requested = g_qk_state.kernels_requested;
  *out_compiled = g_qk_state.kernels_compiled;
  *out_failures = g_qk_state.compile_failures;
  *out_compile_microseconds = g_qk_state.compile_microseconds;
}
