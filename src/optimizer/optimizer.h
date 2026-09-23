#ifndef OPTIMIZER_H
#define OPTIMIZER_H

//~ tec: IR tree helpers

#define OPT_MAX_RELATIONS 32
#define OPT_MAX_CONJUNCTS 64
#define OPT_CONJUNCT_CAPACITY (OPT_MAX_CONJUNCTS + 1)
#define OPT_NO_RELATION max_U32

//~ tec: one FROM item, in textual order
typedef struct OPT_Relation OPT_Relation;
struct OPT_Relation
{
  PLAN_Node* scan;
  GDB_Table* table;
  String8 alias;
};

//~ tec: IR tree helpers
internal B32 optimizer_ir_is_operator(IR_Node* node, String8 name);
internal IR_Node* optimizer_ir_clone(Arena* arena, IR_Node* node);
internal U32 optimizer_ir_collect_conjuncts(IR_Node* root, IR_Node** out_conjuncts, U32 count, U32 capacity);
internal IR_Node* optimizer_ir_and_chain(Arena* arena, IR_Node** conjuncts, U32 count);
internal IR_Node* optimizer_ir_make_where(Arena* arena, IR_Node* expression);
internal void optimizer_ir_remove_child(IR_Node* parent, IR_Node* child);

// tec: a single-table scan binds bare column names, so 'alias.col' has to become 'col' before it moves there
internal void optimizer_ir_strip_column_qualifiers(IR_Node* node);

//~ tec: which relations an expression touches
internal U32 optimizer_resolve_column(OPT_Relation* relations, U32 relation_count, String8 column_name);
internal void optimizer_collect_relation_refs(OPT_Relation* relations, U32 relation_count, IR_Node* node, U64* out_mask, B32* out_unresolved);
internal U32 optimizer_mask_popcount(U64 mask);
internal U32 optimizer_mask_highest(U64 mask);

// tec: true when the expression can never be true for a row whose relation_index side is all NULL
internal B32 optimizer_rejects_null(OPT_Relation* relations, U32 relation_count, IR_Node* node, U32 relation_index);

//~ tec: rewrite rules (pushdown, join assembly, LEFT JOIN conversion)

//~ tec: one step of the left-deep FROM tree, steps[k] joins relations[k] to everything before it
typedef struct OPT_JoinStep OPT_JoinStep;
struct OPT_JoinStep
{
  String8 type;
  IR_Node* on_condition;
  B32 null_supplying;
};

typedef enum OPT_Destination
{
  OPT_Destination_Residual,
  OPT_Destination_Filter,
  OPT_Destination_OnClause,
  OPT_Destination_Dropped,
} OPT_Destination;

typedef struct OPT_Conjunct OPT_Conjunct;
struct OPT_Conjunct
{
  IR_Node* node;
  U64 mask;
  B32 unresolved;
  B32 from_where;
  U32 step;
  OPT_Destination destination;
  U32 target;
};

internal B32 optimizer_enabled(void);
internal B32 optimizer_flatten_from(PLAN_Node* node, OPT_Relation* relations, OPT_JoinStep* steps, U32* io_count);
internal U32 optimizer_load_conjuncts(IR_Node* where_ir, OPT_JoinStep* steps, U32 relation_count, OPT_Relation* relations, OPT_Conjunct* out_conjuncts);
internal void optimizer_convert_left_joins(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count);
internal void optimizer_assign_destinations(U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count);
internal PLAN_Node* optimizer_build_relation_plan(Arena* arena, OPT_Relation* relation, U32 relation_index, OPT_Conjunct* conjuncts, U32 conjunct_count);
internal String8 optimizer_final_join_type(OPT_JoinStep* step, B32 has_on_conditions);

// tec: returns the rewritten FROM and WHERE plan, or NULL when the query is left alone
internal PLAN_Node* optimizer_rewrite_from_where(Arena* arena, PLAN_Node* from_plan, IR_Node* where_ir, IR_Node* select_ir);

//~ tec: constant folding, contradiction and constant propagation

//~ tec: numeric range and equality seen so far for one column of one relation
typedef struct OPT_ColumnBounds OPT_ColumnBounds;
struct OPT_ColumnBounds
{
  U32 relation;
  String8 column;
  B32 has_lower;
  B32 lower_strict;
  F64 lower;
  B32 has_upper;
  B32 upper_strict;
  F64 upper;
  B32 has_equal;
  F64 equal;
  B32 has_equal_string;
  String8 equal_string;
};

//~ tec: equivalence classes of columns joined by equality, keyed by relation and bare column name
typedef struct OPT_ColumnKey OPT_ColumnKey;
struct OPT_ColumnKey
{
  U32 relation;
  String8 column;
  U32 parent;
  IR_Node* constant;
};

#define OPT_MAX_COLUMN_KEYS (OPT_MAX_CONJUNCTS * 2)

internal B32 optimizer_number_from_literal(IR_Node* node, F64* out_value);
internal B32 optimizer_compare_numbers(String8 op, F64 left, F64 right, B32* out_result);
internal B32 optimizer_fold_comparison(IR_Node* node, B32* out_result);
internal IR_Node* optimizer_make_constant_false(Arena* arena);

// tec: true for a root that is a constant false comparison, or an AND chain with one inside
internal B32 optimizer_ir_is_constant_false(IR_Node* root);

// tec: marks always-true conjuncts dropped, returns true when one is always false
internal B32 optimizer_fold_constants(OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count);

internal B32 optimizer_column_type_is_comparable_number(GDB_ColumnType type);
internal B32 optimizer_column_operand(OPT_Relation* relations, U32 relation_count, IR_Node* node, U32* out_relation, String8* out_column, GDB_ColumnType* out_type);
internal String8 optimizer_flip_comparison(String8 op);

internal OPT_ColumnBounds* optimizer_find_bounds(OPT_ColumnBounds* bounds, U32* io_count, U32 relation, String8 column);
internal B32 optimizer_bounds_are_contradictory(OPT_ColumnBounds* bounds);
internal B32 optimizer_bounds_apply(OPT_ColumnBounds* bounds, String8 op, IR_Node* literal, GDB_ColumnType type);

// tec: same column pinned to two values, or a range that cannot hold any value
internal B32 optimizer_detect_contradiction(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count);

internal U32 optimizer_column_key_index(OPT_ColumnKey* keys, U32* io_count, U32 relation, String8 column);
internal U32 optimizer_column_key_root(OPT_ColumnKey* keys, U32 index);
internal B32 optimizer_equality_is_join_key(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjunct, U32* out_left_relation, String8* out_left_column, U32* out_right_relation, String8* out_right_column);
internal B32 optimizer_constant_equality(OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjunct, U32* out_relation, String8* out_column, IR_Node** out_literal);

// tec: a = b together with a = 5 gives b = 5, only across relations that are not null-supplying
internal B32 optimizer_constants_conflict(IR_Node* first, IR_Node* second);

// tec: also reports when two columns of one equality class are pinned to different constants
internal U32 optimizer_propagate_constants(Arena* arena, OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count, B32* out_conflict);

//~ tec: CTE / derived table inlining

//~ tec: a CTE or derived table that only projects columns of one base table and filters it
typedef struct OPT_InlineSource OPT_InlineSource;
struct OPT_InlineSource
{
  IR_Node* select;
  IR_Node* table;
  IR_Node* column_list;
  IR_Node* where;
  GDB_Table* base_table;
  B32 is_star;
};

internal B32 optimizer_inline_enabled(void);

//~ tec: IR list edits and walks
internal void optimizer_ir_unlink(IR_Node* node);
internal U32 optimizer_count_table_references(IR_Node* node, String8 name);
internal U32 optimizer_count_from_relations(IR_Node* select_ir);
internal B32 optimizer_ir_is_plain_predicate(IR_Node* node);
internal B32 optimizer_is_bare_star(IR_Node* column_list);

//~ tec: deciding whether a source can be inlined
internal B32 optimizer_list_has_column(IR_Node* column_list, String8 bare_name);
internal B32 optimizer_inner_columns_belong_to_base(IR_Node* node, GDB_Table* base_table, String8 inner_alias);
internal B32 optimizer_describe_inline_source(GDB_Database* database, IR_Node* inner_select, OPT_InlineSource* out_source);
internal B32 optimizer_column_reference_blocks(IR_Node* node, OPT_InlineSource* source, String8 exposed_name);
internal B32 optimizer_outer_blocks_inline(IR_Node* outer_select, OPT_InlineSource* source, String8 exposed_name);

//~ tec: rewriting
internal IR_Node* optimizer_clone_with_alias(Arena* arena, IR_Node* node, String8 exposed_name, B32 qualify);
internal void optimizer_merge_where(Arena* arena, IR_Node* outer_select, IR_Node* condition);
internal void optimizer_inline_source(Arena* arena, IR_Node* outer_select, IR_Node* table_ir, OPT_InlineSource* source, String8 exposed_name, B32 qualify);
internal B32 optimizer_source_is_in_left_join(IR_Node* outer_select, IR_Node* table_ir);
internal B32 optimizer_try_inline_reference(Arena* arena, GDB_Database* database, IR_Node* select_ir, IR_Node* table_ir, IR_Node* inner_select, String8 default_name);
internal IR_Node* optimizer_find_from_reference(IR_Node* select_ir, String8 name);
internal B32 optimizer_name_is_cte(IR_Node* cte_list, String8 name);

// tec: drops unreferenced CTEs, inlines single-use CTEs and derived tables where that keeps the query's meaning
internal void optimizer_inline_sources(Arena* arena, GDB_Database* database, IR_Node* select_ir);

//~ tec: selectivity and cardinality estimation

//~ tec: fallbacks when a predicate cannot be tied to column statistics
#define OPT_DEFAULT_SELECTIVITY 0.3
#define OPT_DEFAULT_SELECTIVITY_EQUALITY 0.1
#define OPT_DEFAULT_SELECTIVITY_RANGE (1.0 / 3.0)
#define OPT_DEFAULT_SELECTIVITY_PATTERN 0.1
#define OPT_DEFAULT_SELECTIVITY_HAVING 0.3
#define OPT_DEFAULT_GROUP_KEY_DISTINCT 10.0
#define OPT_BACKOFF_TERM_COUNT 4
#define OPT_ESTIMATE_LEAF_CAPACITY 16

//~ tec: statistics lookups
internal B32 optimizer_stats_available(GDB_Column* column);
internal F64 optimizer_clamp_selectivity(F64 selectivity);
internal F64 optimizer_non_null_fraction(GDB_Column* column);
internal F64 optimizer_distinct_count(GDB_Column* column);

//~ tec: single predicate selectivity
internal F64 optimizer_equality_selectivity(GDB_Column* column, B32 is_string, F64 target_numeric, String8 target_string);
internal F64 optimizer_histogram_fraction_below(GDB_ColumnStats* stats, F64 value);
internal F64 optimizer_range_selectivity(GDB_Column* column, B32 is_lt, B32 is_le, B32 is_gt, B32 is_ge, F64 target_numeric);
internal IR_Node* optimizer_make_comparison(Arena* arena, String8 op, IR_Node* column_node, IR_Node* literal_node);
internal F64 optimizer_comparison_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf);
internal F64 optimizer_in_list_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf);
internal F64 optimizer_null_test_selectivity(OPT_Relation* relations, U32 relation_count, IR_Node* leaf);
internal F64 optimizer_column_pair_selectivity(OPT_Relation* relations, U32 relation_count, IR_Node* leaf);
internal F64 optimizer_leaf_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf);

//~ tec: combining predicates
internal F64 optimizer_combine_and(F64* selectivities, U32 count);
internal B32 optimizer_range_leaf_info(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* leaf, GDB_Column** out_column, B32* out_is_lower);
internal void optimizer_merge_range_pairs(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node** leaves, F64* selectivities, U32 count);
internal F64 optimizer_condition_selectivity(Arena* arena, OPT_Relation* relations, U32 relation_count, IR_Node* root);

//~ tec: plan annotation
internal U32 optimizer_collect_relations(PLAN_Node* subtree, OPT_Relation* out_relations, U32 count);
internal F64 optimizer_estimate_join_rows(Arena* arena, PLAN_Node* join, F64* out_hash_rows);
internal F64 optimizer_estimate_semi_rows(Arena* arena, PLAN_Node* join);
internal F64 optimizer_estimate_group_count(PLAN_Node* aggregate, F64 input_rows);
internal F64 optimizer_estimate_limit_rows(PLAN_Node* limit, F64 input_rows);
internal void optimizer_annotate_plan(Arena* arena, PLAN_Node* plan);

//~ tec: row estimates for CTEs and derived tables that are not executed, matched to Scan nodes by name
#define OPT_SOURCE_ESTIMATE_CAPACITY 16

typedef struct OPT_SourceEstimates OPT_SourceEstimates;
struct OPT_SourceEstimates
{
  U32 count;
  String8 names[OPT_SOURCE_ESTIMATE_CAPACITY];
  F64 rows[OPT_SOURCE_ESTIMATE_CAPACITY];
};

internal void optimizer_source_estimates_add(OPT_SourceEstimates* sources, String8 name, F64 rows);
internal void optimizer_apply_source_estimates(PLAN_Node* plan, OPT_SourceEstimates* sources);

//~ tec: cost model

//~ tec: microseconds per operation, measured by bench_calibrate on a 12 worker machine and overridable by QE_COST_* settings
typedef struct OPT_CostModel OPT_CostModel;
struct OPT_CostModel
{
  F64 gpu_submit_us;
  F64 gpu_scan_per_row_us;
  F64 gpu_upload_fixed_us;
  F64 gpu_upload_per_row_us;
  
  F64 cpu_scan_fixed_us;
  F64 cpu_scan_per_row_us;
  F64 cpu_scan_per_extra_condition_us;
  F64 cpu_reference_workers;
  
  F64 index_fixed_us;
  F64 index_per_match_us;
  F64 filter_per_row_us;
  F64 fill_per_row_us;
  
  F64 join_build_per_row_us;
  F64 join_probe_per_row_us;
  F64 join_output_per_row_us;
  
  F64 sort_cpu_per_row_log_us;
  F64 sort_gpu_fixed_us;
  F64 sort_gpu_per_row_us;
  F64 top_n_per_row_us;
  
  F64 aggregate_cpu_per_row_us;
  F64 aggregate_gpu_fixed_us;
  F64 aggregate_gpu_per_row_us;
};

internal OPT_CostModel optimizer_cost_model_load(void);
internal F64 optimizer_cost_worker_scale(OPT_CostModel* model);

// tec: a warm scan is one dispatch and one download, a cold one also uploads every referenced column
internal F64 optimizer_cost_gpu_scan(OPT_CostModel* model, F64 rows, U32 cold_column_count);
internal F64 optimizer_cost_cpu_scan(OPT_CostModel* model, F64 rows, U32 condition_count);

// tec: the residual conditions run serially over the rows the index returned
internal F64 optimizer_cost_index_scan(OPT_CostModel* model, F64 matches, U32 residual_condition_count);
internal F64 optimizer_cost_identity(OPT_CostModel* model, F64 rows);

// tec: a hash join is a build dispatch, a probe dispatch and a download, then per row work on each side and on the result
internal F64 optimizer_cost_hash_join(OPT_CostModel* model, F64 build_rows, F64 probe_rows, F64 output_rows);

// tec: the CPU sort is n log n comparisons, the GPU sort is a fixed round trip plus a per row cost
internal F64 optimizer_cost_sort_cpu(OPT_CostModel* model, F64 rows);
internal F64 optimizer_cost_sort_gpu(OPT_CostModel* model, F64 rows);
internal F64 optimizer_cost_top_n(OPT_CostModel* model, F64 rows);
internal F64 optimizer_cost_aggregate_cpu(OPT_CostModel* model, F64 rows);
internal F64 optimizer_cost_aggregate_gpu(OPT_CostModel* model, F64 rows);

// tec: the input size where the GPU starts to be cheaper, inputs below it stay on the CPU
#define OPT_SMALL_INPUT_SEARCH_LIMIT (4ull * 1024ull * 1024ull)
typedef F64 OPT_CostFn(OPT_CostModel* model, F64 rows);
internal U64 optimizer_crossover_rows(OPT_CostModel* model, OPT_CostFn* cpu_cost, OPT_CostFn* gpu_cost);
internal U64 optimizer_sort_gpu_min_rows(OPT_CostModel* model);
internal U64 optimizer_aggregate_cpu_max_rows(OPT_CostModel* model);

//~ tec: physical scan strategy

#define OPT_SCAN_COLUMN_CAPACITY 16

internal B32 optimizer_scan_costing_enabled(void);

//~ tec: what a scan condition reads
internal void optimizer_collect_condition_columns(GDB_Table* table, IR_Node* node, GDB_Column** out_columns, U32* io_count, U32 capacity, B32* out_unresolved);
internal B32 optimizer_columns_have_nulls(GDB_Column** columns, U32 count);
internal U32 optimizer_count_cold_columns(GDB_Column** columns, U32 count);

//~ tec: ordering the conditions of a CPU scan
internal void optimizer_order_conjuncts_for_cpu(Arena* arena, PLAN_Node* filter, IR_Node** leaves, U32 leaf_count);

//~ tec: choosing how a scan runs
internal void optimizer_choose_filter_scan(Arena* arena, OPT_CostModel* model, PLAN_Node* filter);
internal void optimizer_choose_identity_scan(OPT_CostModel* model, PLAN_Node* scan);
internal void optimizer_choose_scan_strategies_in(Arena* arena, OPT_CostModel* model, PLAN_Node* plan, B32 is_join_right);
internal void optimizer_choose_scan_strategies(Arena* arena, PLAN_Node* plan);

//~ tec: join ordering

#define OPT_JOIN_DP_MAX_RELATIONS 10
#define OPT_JOIN_MAX_EDGES 128
#define OPT_JOIN_IMPROVEMENT_FACTOR 0.9

//~ tec: an equality between one column of each of two relations, either written in the query or implied by two others
typedef struct OPT_JoinEdge OPT_JoinEdge;
struct OPT_JoinEdge
{
  U32 relation_a;
  U32 relation_b;
  GDB_Column* column_a;
  GDB_Column* column_b;
  String8 name_a;
  String8 name_b;
  B32 is_derived;
};

typedef struct OPT_JoinGraph OPT_JoinGraph;
struct OPT_JoinGraph
{
  U32 relation_count;
  F64 rows[OPT_MAX_RELATIONS];
  U32 edge_count;
  OPT_JoinEdge edges[OPT_JOIN_MAX_EDGES];
};

//~ tec: best way found so far to reach a set of relations
typedef struct OPT_JoinState OPT_JoinState;
struct OPT_JoinState
{
  B32 valid;
  F64 cost;
  F64 rows;
  U32 last;
  U64 previous_mask;
};

//~ tec: building the graph
internal B32 optimizer_join_enabled(void);
internal B32 optimizer_join_add_edge(OPT_JoinGraph* graph, U32 relation_a, GDB_Column* column_a, String8 name_a, U32 relation_b, GDB_Column* column_b, String8 name_b, B32 is_derived);
internal void optimizer_join_collect_edges(OPT_Relation* relations, U32 relation_count, OPT_Conjunct* conjuncts, U32 conjunct_count, U32 group_size, OPT_JoinGraph* graph);
internal void optimizer_join_add_derived_edges(OPT_JoinGraph* graph);
internal B32 optimizer_join_has_edge_between(OPT_JoinGraph* graph, U32 relation_a, U32 relation_b, B32 original_only, OPT_JoinEdge** out_edge);

//~ tec: cardinality
internal F64 optimizer_join_edge_selectivity(OPT_JoinEdge* edge, F64 rows_a, F64 rows_b);
internal B32 optimizer_join_extend_rows(OPT_JoinGraph* graph, U64 subset_mask, F64 subset_rows, U32 next, F64* out_rows);
internal F64 optimizer_join_order_cost(OPT_JoinGraph* graph, OPT_CostModel* model, U32* order, U32 count, B32* out_valid);

//~ tec: search
internal B32 optimizer_join_order_dp(OPT_JoinGraph* graph, OPT_CostModel* model, U32 count, U32* out_order, F64* out_cost);
internal B32 optimizer_join_order_greedy(OPT_JoinGraph* graph, OPT_CostModel* model, U32 count, U32* out_order, F64* out_cost);

// tec: returns true when the order in out_order differs from the written one, which is kept unless the new one is clearly cheaper
internal B32 optimizer_choose_join_order(OPT_JoinGraph* graph, OPT_CostModel* model, U32 count, U32* out_order);

//~ tec: deciding whether the written order may change at all
internal U32 optimizer_reorderable_group_size(OPT_JoinStep* steps, U32 relation_count);
internal B32 optimizer_node_has_ambiguous_column(IR_Node* node, OPT_Relation* relations, U32 relation_count);
internal B32 optimizer_select_has_ambiguous_columns(IR_Node* select_ir, OPT_Relation* relations, U32 relation_count);
internal B32 optimizer_conjuncts_allow_reorder(OPT_Conjunct* conjuncts, U32 conjunct_count, U32 group_size);

//~ tec: planning an order and turning it into join conditions
internal void optimizer_join_estimate_relation_rows(Arena* arena, OPT_Relation* relations, U32 relation_count, OPT_Conjunct* conjuncts, U32 conjunct_count, F64* out_rows);
internal B32 optimizer_plan_join_order(Arena* arena, IR_Node* select_ir, OPT_Relation* relations, U32 relation_count, OPT_JoinStep* steps, OPT_Conjunct* conjuncts, U32 conjunct_count, U32* out_order, OPT_JoinGraph* out_graph);
internal U32 optimizer_step_position_for_conjunct(OPT_Conjunct* conjunct, U32* position_of, U32 group_size);
internal IR_Node* optimizer_make_derived_equality(Arena* arena, OPT_Relation* relations, OPT_JoinEdge* edge);

//~ tec: sizing hints and physical operator choices (top-K, presorted, CPU crossover)

// tec: past this many rows a heap keeps up worse than one sort, so the limit stays a separate node
#define OPT_TOP_N_DEFAULT_MAX_K 4096

// tec: an estimate this large is a broken estimate, not a row count worth sizing a buffer for
#define OPT_HINT_MAX_ROWS 1.0e12

internal B32 optimizer_sizing_enabled(void);
internal B32 optimizer_small_cpu_enabled(void);
internal B32 optimizer_top_n_enabled(void);
internal B32 optimizer_sort_elimination_enabled(void);
internal B32 optimizer_fusion_enabled(void);

//~ tec: GPU submits per operator, the numbers the cost model and EXPLAIN use
internal F64 optimizer_scan_round_trips(void);
internal F64 optimizer_join_round_trips(void);
internal B32 optimizer_aggregate_uses_approx(IR_Node* column_list);
internal F64 optimizer_aggregate_round_trips(PLAN_Node* aggregate);
internal B32 optimizer_subtree_has_aggregate(PLAN_Node* plan);
internal F64 optimizer_sort_round_trips(PLAN_Node* sort);
internal F64 optimizer_node_round_trips(PLAN_Node* plan);

// tec: estimates cost stats lookups, so only plans with a node that reads one get annotated when they run
internal B32 optimizer_plan_uses_estimates(PLAN_Node* plan);
internal U64 optimizer_rows_to_hint(F64 rows);

//~ tec: what the executor sizes its buffers and picks its device from
internal void optimizer_join_hints(PLAN_Node* join, QE_JoinHints* out_hints);
internal void optimizer_aggregate_hints(PLAN_Node* aggregate, QE_AggregateHints* out_hints);
internal void optimizer_sort_hints(PLAN_Node* sort, QE_SortHints* out_hints);

//~ tec: the single table a sort can be answered from without executing its input
typedef struct OPT_OrderSource OPT_OrderSource;
struct OPT_OrderSource
{
  PLAN_Node* scan;
  PLAN_Node* filter;
  GDB_Table* table;
  IR_Node* condition;
};

internal B32 optimizer_limit_keep_count(PLAN_Node* node, U64* out_keep);
internal B32 optimizer_find_order_source(PLAN_Node* input, OPT_OrderSource* out_source);
internal B32 optimizer_order_key(OPT_OrderSource* source, IR_Node* order_by, GDB_Column** out_column, B32* out_descending);

//~ tec: choosing how a Sort, or a Sort under a Limit, gets its order
internal B32 optimizer_choose_presorted(Arena* arena, PLAN_Node* sort, OPT_OrderSource* source, GDB_Column* key_column, B32 descending);
internal B32 optimizer_choose_index_walk(Arena* arena, OPT_CostModel* model, PLAN_Node* top_n, OPT_OrderSource* source, GDB_Column* key_column, B32 descending, U64 keep);
internal PLAN_Node* optimizer_plan_ordering(Arena* arena, PLAN_Node* root);

//~ tec: semi / anti join extraction (EXISTS, IN over a subquery)

#define OPT_SEMI_OUTER_CAPACITY 16
#define OPT_SEMI_DEFAULT_MATCH_FRACTION 0.5

// tec: names the synthetic aliases and temp tables, unique for the life of the process so nested selects never collide
global U64 g_optimizer_semi_serial = 0;

internal B32 optimizer_semi_join_enabled(void);

//~ tec: the tables of the outer select, which an inner column can be matched against
typedef struct OPT_OuterTable OPT_OuterTable;
struct OPT_OuterTable
{
  GDB_Table* table;
  String8 alias;
};

internal String8 optimizer_outer_table_label(OPT_OuterTable* table);

// tec: how many of the tables a column name resolves in, out_first is the first of them
internal U32 optimizer_semi_match_tables(OPT_OuterTable* tables, U32 count, String8 name, U32* out_first);
internal B32 optimizer_collect_outer_tables(GDB_Database* database, IR_Node* select_ir, OPT_OuterTable* out_tables, U32* out_count);

// tec: counts the columns an expression reads from the inner table and from the outer ones, unsupported is set for anything else
internal void optimizer_semi_classify_columns(IR_Node* node, OPT_OuterTable* inner, OPT_OuterTable* outer, U32 outer_count, U32* io_inner_refs, U32* io_outer_refs, B32* out_unsupported);

//~ tec: building the IR node the planner turns into a semi or anti join
internal IR_Node* optimizer_make_semi_node(Arena* arena, String8 kind, String8 outer_key, String8 inner_key, String8 table_name, String8 alias, IR_Node* filter);
internal B32 optimizer_semi_keys_compatible(GDB_Column* outer_key, GDB_Column* inner_key);

//~ tec: EXISTS with an equality between the inner and outer tables, a correlated subquery the old path had to refuse
internal B32 optimizer_try_exists_semi(Arena* arena, GDB_Database* database, IR_Node* exists_node, OPT_OuterTable* outer, U32 outer_count, IR_Node** out_semi);

//~ tec: IN and NOT IN over an uncorrelated subquery, a semi join once the list is too long for a GPU scan
internal B32 optimizer_semi_pays_off(OPT_CostModel* model, U64 list_count, F64 outer_rows);
internal B32 optimizer_try_in_semi(Arena* arena, GDB_Database* database, IR_Node* in_node, OPT_OuterTable* outer, U32 outer_count, IR_Node** out_semi, B32* out_failed);

// tec: may_execute is off for a plain EXPLAIN, which must not run the IN subqueries it would need the size of
internal B32 optimizer_extract_semi_joins(Arena* arena, GDB_Database* database, IR_Node* select_ir, B32 may_execute);

#endif //OPTIMIZER_H
