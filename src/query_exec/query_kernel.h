#ifndef QUERY_KERNEL_H
#define QUERY_KERNEL_H

#include "shaderc/shaderc.h"

//~ tec: per query scan kernels
// the WHERE bytecode is turned into GLSL for exactly that query and built with shaderc, loaded from shaderc_shared.dll when it can be found.
// the kernel has the same bindings and push constants as scan_filter.comp, so every caller of that kernel can take it as is.
// string constants and numeric constants are still read from their buffers, so queries that differ only in literal values share one kernel.
//
// settings:
//   QE_PERQUERY_KERNELS:1       every GPU scan gets a generated kernel (2 means the same). numeric AND filters get one with scan_simple's bindings and narrow columns
//   QE_PERQUERY_KERNELS:0       scan_simple and the interpreter only
//   QE_PERQUERY_MIN_ROWS:0      tables with fewer rows keep the interpreter
//   QE_PERQUERY_LOG_SOURCE:1    log the generated GLSL at debug level

#define QK_FAILED_KEY_CAP 64
#define QK_MAX_STACK 64

typedef void* QK_CompilerInitProc(void);
typedef void QK_CompilerReleaseProc(void* compiler);
typedef void* QK_OptionsInitProc(void);
typedef void QK_OptionsReleaseProc(void* options);
typedef void QK_OptionsSetOptimizationProc(void* options, shaderc_optimization_level level);
typedef void QK_OptionsSetTargetEnvProc(void* options, shaderc_target_env target, uint32_t version);
typedef void* QK_CompileProc(void* compiler, const char* source, size_t source_size, shaderc_shader_kind kind, const char* file_name, const char* entry_point, void* options);
typedef void QK_ResultReleaseProc(void* result);
typedef size_t QK_ResultLengthProc(void* result);
typedef const char* QK_ResultBytesProc(void* result);
typedef shaderc_compilation_status QK_ResultStatusProc(void* result);
typedef const char* QK_ResultMessageProc(void* result);

#define QK_NAME_CAP 256

// tec: kernel names are handed to the GPU kernel log, which keeps the pointer, so they live in a table that is never freed
typedef struct QK_Name QK_Name;
struct QK_Name
{
  U64 key;
  char text[32];
};

typedef struct QK_State QK_State;
struct QK_State
{
  Arena* arena;
  B32 library_tried;
  B32 library_loaded;
  OS_Handle library;
  void* compiler;
  void* options;

  QK_CompilerInitProc* compiler_init;
  QK_CompilerReleaseProc* compiler_release;
  QK_OptionsInitProc* options_init;
  QK_OptionsReleaseProc* options_release;
  QK_OptionsSetOptimizationProc* options_set_optimization;
  QK_OptionsSetTargetEnvProc* options_set_target_env;
  QK_CompileProc* compile;
  QK_ResultReleaseProc* result_release;
  QK_ResultLengthProc* result_length;
  QK_ResultBytesProc* result_bytes;
  QK_ResultStatusProc* result_status;
  QK_ResultMessageProc* result_message;

  U64 failed_keys[QK_FAILED_KEY_CAP];
  U32 failed_key_count;
  
  QK_Name names[QK_NAME_CAP];
  U32 name_count;
  U32 name_next;

  U64 kernels_requested;
  U64 kernels_compiled;
  U64 compile_failures;
  U64 compile_microseconds;
};
global QK_State g_qk_state = {0};

// tec: one entry of the symbolic operand stack, `text` is a GLSL expression that is a bool when is_bool is set and a double otherwise
typedef struct QK_Conj QK_Conj;
struct QK_Conj
{
  String8 text;
  // tec: index into the comparisons that scan_simple's layout can run, -1 for any other conjunct
  S32 candidate;
  // tec: bit s is set when the text reads column slot s
  U32 slot_mask;
};

#define QK_MAX_CONJUNCTS 32

typedef struct QK_Value QK_Value;
struct QK_Value
{
  String8 text;
  B32 is_bool;
  U32 slot_mask;
  S32 candidate;
  // tec: set when the value is a plain column load or a constant, for spotting column op constant
  S32 col_slot;
  U32 col_type;
  S32 const_index;
  // tec: the top level AND terms of a bool value, 0 when the value is a single term
  QK_Conj* conj;
  U32 conj_count;
};

//~ tec: a filter that is a top level AND, some terms of which are numeric comparisons against a constant, runs on a kernel with scan_simple's bindings.
// those terms are checked for 8 rows at a time on narrow columns, the other terms only for the rows that are left

#define QK_HYBRID_MAX_LEAVES 8

typedef struct QK_HybridLeaf QK_HybridLeaf;
struct QK_HybridLeaf
{
  U32 slot;
  U32 coltype;
  U32 op;
  F64 target;
};

typedef struct QK_HybridInfo QK_HybridInfo;
struct QK_HybridInfo
{
  // tec: in, the caller can run a kernel with scan_simple's layout
  B32 allow;
  // tec: out, the kernel is one, and the comparisons it takes from the leaf words
  B32 is_hybrid;
  U32 leaf_count;
  QK_HybridLeaf leaves[QK_HYBRID_MAX_LEAVES];
  // tec: out, column slots the other terms read, these must be uploaded at their full width
  U32 wide_slot_mask;
};

internal String8 qk_kernel_name(U64 key);
internal B32 qk_enabled(U64 row_count);
internal B32 qk_compiler_available(void);
internal B32 qk_scan_source_build(Arena* arena, QE_BytecodeProgram* prog, QK_HybridInfo* hybrid, String8* out_source);
internal String8 qk_spirv_compile(Arena* arena, String8 source);
// tec: hybrid may be 0. when it is given with allow set and the filter has comparisons that qualify, the kernel takes scan_simple's bindings and hybrid says what to upload
internal GPU_Kernel* qk_scan_kernel(Arena* arena, QE_BytecodeProgram* prog, QK_HybridInfo* hybrid, String8* out_name);
// tec: scan_simple's bindings and leaf words with the column, type and operator of each comparison in the source. words is a count then 6 words per comparison
internal GPU_Kernel* qk_simple_kernel(Arena* arena, U32* words, String8* out_name);
internal void qk_stats(U64* out_requested, U64* out_compiled, U64* out_failures, U64* out_compile_microseconds);

#endif //QUERY_KERNEL_H
