// compute_ql vs sqlite vs duckdb: GROUP BY and join scaling at 50,000 and 2,000,000 fact rows with the normal 2 GB buffer limit
// the same suite as bench_query_chunked_hash, which runs it with the limit forced down to 4 MB to test the chunked paths
#define GPU_MAX_BUFFER_SIZE GB(2)
#define CH_REPORT_PATH "bench_reports/join_scaling_report.md"
#include "bench_query_chunked_hash.c"
