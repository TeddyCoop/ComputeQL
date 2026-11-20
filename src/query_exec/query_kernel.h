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
//   QE_ROUTE_CONJUNCTIONS:1     a conjunction of plain comparisons runs on the precompiled path the first time its shape is seen and gets a generated kernel from the second execution on
//   QE_GEN_FIXED_WIDTH:1        the generated numeric AND kernel gets the column width, type flags and bias of every comparison as constants of its source (a column encoded differently gets another kernel)
//   QE_SCAN_SPEC:1              numeric AND scans use one precompiled module, scan_spec.comp, with the comparisons given as specialization constants when the pipeline is built, and no per query GLSL
//   QE_COLLISION_CHECK:0        a kernel is identified by the 64 bit hash of its source alone (the default adds a second hash of a different kind and the length, and the name carries both)
//
// settings that exist for tests only:
//   QE_TEST_FAIL_SHADERC:N      every Nth generated source fails to compile (1 is every one), the query falls back to the precompiled path
//   QE_TEST_FAIL_PIPELINE:N     every Nth pipeline of a generated kernel fails to build
//   QE_TEST_KEY_BITS:B          the first hash of a generated source is cut to its low B bits, so that distinct sources collide

#define QK_FAILED_KEY_CAP 64
#define QK_MAX_STACK 64
#define QK_ROUTED_KEY_CAP 256

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
  U64 check;
  char text[48];
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
  // tec: shapes the conjunction router has seen once
  U64 routed_keys[QK_ROUTED_KEY_CAP];
  U32 routed_key_count;
  U32 routed_next;
  U64 routed_deferrals;
  
  QK_Name names[QK_NAME_CAP];
  U32 name_count;
  U32 name_next;

  // tec: guards the compiler, the failed keys and the name table
  OS_Handle lock;
  U64 disk_cache_hits;

  U64 test_shaderc_count;
  String8 spec_spirv;
  B32 spec_spirv_tried;
  String8 count_spec_spirv;
  B32 count_spec_spirv_tried;
  U64 count_spec_requests;
  String8 filter_spec_spirv;
  B32 filter_spec_spirv_tried;
  String8 filter_spec8_spirv;
  B32 filter_spec8_spirv_tried;
  U64 collisions_avoided;
  U64 spec_requests;
  U64 kernels_requested;
  U64 kernels_compiled;
  U64 compile_failures;
  U64 compile_microseconds;
  // tec: sizes of the last source handed to shaderc and of the SPIR-V it returned (or that the disk cache held)
  U64 last_source_bytes;
  U64 last_spirv_bytes;
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

//~ tec: a global aggregate fused with the filter. the kernel evaluates the WHERE clause per row and folds the matching rows into partials, no row ids are made.
// partials per workgroup: matching row count, then sum, min and max of each aggregate column. slot is the column's binding, as for the filter columns

#define QK_FUSED_MAX_COLUMNS 4
#define QK_GROUP_MAX_KEYS 3

typedef struct QK_FusedAgg QK_FusedAgg;
struct QK_FusedAgg
{
  U32 column_count;
  U32 slots[QK_FUSED_MAX_COLUMNS];
  U32 coltypes[QK_FUSED_MAX_COLUMNS];
  // tec: GROUP BY whole number or dictionary coded columns whose ranges are small.
  // the arguments are U32 or I32 columns. per group the output holds 2 + 4 * column_count words: row count, smallest row, then per argument the sum (low, high word), the minimum and the maximum.
  // an I32 value is added and compared with its sign bit flipped, the host takes the bias out
  B32 grouped;
  // tec: up to 3 keys. a key's value minus its smallest value (push constant slot 7 low and high word, slot 6 high word) is below its range,
  // the group is the keys combined as digits, group_count is the product of the ranges
  U32 key_count;
  U32 key_slots[QK_GROUP_MAX_KEYS];
  U32 key_types[QK_GROUP_MAX_KEYS];
  U32 key_ranges[QK_GROUP_MAX_KEYS];
  U32 group_count;
};

#define QK_GROUP_WORDS(columns) (2 + 4 * (columns))

internal void qk_init(void);
internal String8 qk_library_spirv(Arena* arena, String8 name);
internal String8 qk_kernel_name(U64 key, U64 check);
internal B32 qk_enabled(U64 row_count);
internal B32 qk_compiler_available(void);
internal B32 qk_scan_source_build(Arena* arena, QE_BytecodeProgram* prog, QK_HybridInfo* hybrid, QK_FusedAgg* fuse, String8* out_source);
internal String8 qk_spirv_compile(Arena* arena, String8 source);
// tec: hybrid may be 0. when it is given with allow set and the filter has comparisons that qualify, the kernel takes scan_simple's bindings and hybrid says what to upload
internal GPU_Kernel* qk_scan_kernel(Arena* arena, QE_BytecodeProgram* prog, QK_HybridInfo* hybrid, String8* out_name);
// tec: scan_simple's bindings and leaf words with the column, type and operator of each comparison in the source. words is a count then 6 words per comparison
internal GPU_Kernel* qk_simple_kernel(Arena* arena, U32* words, String8* out_name);
internal GPU_Kernel* qk_fused_aggregate_kernel(Arena* arena, QE_BytecodeProgram* prog, QK_FusedAgg* fuse, String8* out_name);
// tec: bitonic sort of up to 4 numeric keys, built for the key count, directions (bit k of dir_mask is key k descending) and key width.
// binding 0 holds key_count keys per element, binding 1 the row each element came from. U64 push constant slots: 1 real count, 2 mode, 3 stage, 4 stride
// mode 0 sorts every tile and numbers the rows itself, mode 1 is one pass of a stride at least a tile long, mode 2 finishes a stage inside the tiles
internal U32 qk_sort_tile(U32 key_count, B32 narrow);
internal GPU_Kernel* qk_sort_kernel(Arena* arena, U32 key_count, U32 dir_mask, B32 narrow, String8* out_name);
// tec: hash join passes built for the key type (and, for the probe, for a left or anti join). same bindings and push constants as hash_join_build_count.comp and hash_join_probe.comp
internal GPU_Kernel* qk_join_build_kernel(Arena* arena, B32 string_key, String8* out_name);
internal GPU_Kernel* qk_join_probe_hash_kernel(Arena* arena, B32 string_key, B32 emit_unmatched, String8* out_name);
// tec: the streamed fact table probe of join_probe_native.comp with the column widths, aggregate functions and their arguments fixed in the source
internal GPU_Kernel* qk_join_probe_native_kernel(Arena* arena, U64 shader_widths, U32 arg_count, U32 num_exprs, U32 func_codes, U32 arg_index_codes, U32 arg_signed_bits, String8* out_name);
// tec: a HAVING condition as one kernel, see qk_having_kernel in query_kernel.c for the bindings
internal GPU_Kernel* qk_having_kernel(Arena* arena, String8 condition, String8* out_name);
// tec: the group slot assignment of aggregate_assign.comp for a number of key columns of which some are strings. bindings and push constants as that kernel
internal GPU_Kernel* qk_hash_assign_kernel(Arena* arena, U32 num_group_cols, U32 string_mask, String8* out_name);
// tec: the atomic hash aggregation, F32 arguments (fixed 0) or F64 arguments scaled by powers of ten (fixed 1). bindings and push constants as aggregate_hash_reduce_f32.comp and _fixed.comp
internal GPU_Kernel* qk_member_reduce_kernel(Arena* arena, B32 narrow, U32 num_exprs, U32 func_codes, String8* out_name);
#define QK_COMPACT_BLOCK_SLOTS 1024
internal GPU_Kernel* qk_compact_kernel(Arena* arena, String8* out_name);
internal GPU_Kernel* qk_hash_reduce_kernel(Arena* arena, B32 fixed, U32 num_exprs, U32 func_codes, U32 scales, String8* out_name);
// tec: selection_gather.comp for one mode (0 64 bit column, 1 32 bit column, 2 rows of positions, 3 resolve through a map, 4 resolve with an offset)
internal GPU_Kernel* qk_gather_kernel(Arena* arena, U32 mode, String8* out_name);
// tec: the window rank flags over keys sorted by qk_sort_kernel. bindings as window_rank_flags.comp, the key layout is the sort kernel's. push constant slot 0 is the row count
internal GPU_Kernel* qk_window_flags_kernel(Arena* arena, U32 key_count, U32 partition_count, B32 narrow, String8* out_name);
internal U64 qk_routed_deferrals(void);
internal void qk_last_sizes(U64* out_source_bytes, U64* out_spirv_bytes);
internal U64 qk_spec_request_count(void);
internal U64 qk_count_spec_request_count(void);
// tec: QE_SCAN_FILTER_SPEC, an AND/OR tree of up to 8 numeric or string comparisons on one precompiled module, scan_filter_spec.comp, the tree is specialization constants. returns 0 when the program is not such a tree. variant 2 is scan_filter_spec8.comp, which takes 8 rows per thread
internal GPU_Kernel* qk_filter_spec_kernel(U32* words, U32 word_count, U32 variant, String8* out_name);
internal GPU_Kernel* qk_fused_count_spec_kernel(U32 leaf_count, U32* slots, U32* types, U32* ops, String8* out_name);
internal void qk_stats(U64* out_requested, U64* out_compiled, U64* out_failures, U64* out_compile_microseconds);

#endif //QUERY_KERNEL_H
