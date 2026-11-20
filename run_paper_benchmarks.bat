@echo off
setlocal

:: tec: runs every benchmark and test that the per query kernel paper takes its numbers from.
:: build the benchmarks first with build_and_run_tests.bat, run_scale_bench.bat and run_select_scale.bat (cl or clang).
::
:: usage: run_paper_benchmarks.bat                runs everything
::        run_paper_benchmarks.bat clear          deletes the engine's shader and pipeline caches first, then runs everything
::        run_paper_benchmarks.bat no1b           skips every run at 1B rows
::        run_paper_benchmarks.bat clear no1b     both
::        run_paper_benchmarks.bat only1b         runs only the 1B row runs (the complement of no1b)
::        run_paper_benchmarks.bat scaling        runs only the new-shape cost at 5M and 100M rows
::        run_paper_benchmarks.bat router         runs only the conjunction router (QE_ROUTE_CONJUNCTIONS:1): the 40 shapes cold at 5M, 20M and 100M rows and the 37-query workload
::        run_paper_benchmarks.bat routerfilled   runs only the router on the 37-query workload with the engine caches filled by an earlier process
::        run_paper_benchmarks.bat overnight      runs coldcost, sweepmore, libmore and families in one go (no 1B rows)
::        run_paper_benchmarks.bat coldcost       the itemized cold cost of one process (shaderc, pipeline creation, the rest) at 5M, 20M and 100M rows, caches empty and filled
::        run_paper_benchmarks.bat sweepmore      the selectivity sweep with a warm-up query, and at 1M, 5M and 20M rows
::        run_paper_benchmarks.bat libmore        new shapes against library kernels at 5M and 100M rows
::        run_paper_benchmarks.bat specialize     generated scan, fixed widths and specialization constants against the precompiled scan, and the single comparison gap
::        run_paper_benchmarks.bat datashapes     the 40 shapes with other selectivities, skew, clustering and outputs, and the row-number filter in four forms
::        run_paper_benchmarks.bat fusedcount     COUNT(*) over numeric conjunctions, generated fused kernel against the precompiled fused count with specialization constants
::        run_paper_benchmarks.bat steadyagain    the 40 shapes with a second steady measurement after the last shape
::        run_paper_benchmarks.bat libtouch       new shapes against library kernels with the columns read first
::        run_paper_benchmarks.bat duckdbsep      the one billion row scale test with cql and duckdb in separate processes
::        run_paper_benchmarks.bat spinwarm       the steady measurements again with a 300 ms busy warm-up before the timed runs
::        run_paper_benchmarks.bat spinspec       fusion off, fixed width and scan specialization with the busy warm-up
::        run_paper_benchmarks.bat spinshapes     the other selectivities, skewed and clustered data and select lists with the busy warm-up
::        run_paper_benchmarks.bat treespec       the interpreter with the WHERE clause as specialization constants (QE_SCAN_FILTER_SPEC:1) against the interpreter and generation
::        run_paper_benchmarks.bat spinmore       the sweep at other sizes and the specialized module with a warm driver cache, with the busy warm-up
::        run_paper_benchmarks.bat treegap        the 40 shapes with generated kernels that evaluate every leaf (QE_GEN_NO_SHORT_CIRCUIT:1) next to the specialized module
::        run_paper_benchmarks.bat failure        failures of generation and name collisions on the 40 scan shapes (not on aggregation queries)
::        run_paper_benchmarks.bat families       the workloads with one generation switch off at a time
::
:: every run that measures a cold execution starts with the engine's caches deleted and with the graphics driver's own pipeline cache switched off
:: (AMD_VK_USE_PIPELINE_CACHE=0, which is the switch of the AMD driver; another driver has its own, for example MESA_SHADER_CACHE_DISABLE=true for Mesa)
set "run_1b=1"
set "only_1b=0"
set "only_scaling=0"
set "only_router=0"
set "only_routerfilled=0"
set "only_interleaved=0"
set "only_profile=0"
set "only_sweep=0"
set "only_libnew=0"
set "only_overnight=0"
set "only_coldcost=0"
set "only_sweepmore=0"
set "only_libmore=0"
set "only_families=0"
set "only_specialize=0"
set "only_failure=0"
set "only_spinshapes=0"
if /i "%~1"=="spinshapes" (
  set "only_spinshapes=1"
)
set "only_spinmore=0"
if /i "%~1"=="spinmore" (
  set "only_spinmore=1"
)
set "only_treegap=0"
if /i "%~1"=="treegap" (
  set "only_treegap=1"
)
set "only_treespec=0"
if /i "%~1"=="treespec" (
  set "only_treespec=1"
)
set "only_spinspec=0"
if /i "%~1"=="spinspec" (
  set "only_spinspec=1"
)
set "only_spinwarm=0"
if /i "%~1"=="spinwarm" (
  set "only_spinwarm=1"
)
set "only_duckdbsep=0"
if /i "%~1"=="duckdbsep" (
  set "only_duckdbsep=1"
)
set "only_libtouch=0"
if /i "%~1"=="libtouch" (
  set "only_libtouch=1"
)
set "only_steadyagain=0"
if /i "%~1"=="steadyagain" (
  set "only_steadyagain=1"
)
set "only_fusedcount=0"
if /i "%~1"=="fusedcount" (
  set "only_fusedcount=1"
)
set "only_datashapes=0"
if /i "%~1"=="datashapes" (
  set "only_datashapes=1"
)
if /i "%~1"=="specialize" (
  set "only_specialize=1"
)
if /i "%~1"=="failure" (
  set "only_failure=1"
)
if /i "%~1"=="overnight" (
  set "only_overnight=1"
)
if /i "%~1"=="coldcost" (
  set "only_coldcost=1"
)
if /i "%~1"=="sweepmore" (
  set "only_sweepmore=1"
)
if /i "%~1"=="libmore" (
  set "only_libmore=1"
)
if /i "%~1"=="families" (
  set "only_families=1"
)
if /i "%~1"=="libnew" (
  set "only_libnew=1"
)
if /i "%~1"=="sweep" (
  set "only_sweep=1"
)
if /i "%~1"=="interleaved" (
  set "only_interleaved=1"
)
if /i "%~1"=="profile" (
  set "only_profile=1"
)
set "only_defaultfilled=0"
if /i "%~1"=="defaultfilled" (
  set "only_defaultfilled=1"
)
if /i "%~1"=="routerfilled" (
  set "only_routerfilled=1"
)
if /i "%~1"=="router" (
  set "only_router=1"
)
if /i "%~2"=="router" (
  set "only_router=1"
)
if /i "%~1"=="scaling" (
  set "only_scaling=1"
)
if /i "%~2"=="scaling" (
  set "only_scaling=1"
)
if /i "%~1"=="only1b" (
  set "only_1b=1"
)
if /i "%~2"=="only1b" (
  set "only_1b=1"
)
if /i "%~1"=="no1b" (
  set "run_1b=0"
)
if /i "%~2"=="no1b" (
  set "run_1b=0"
)
if /i "%~1"=="clear" (
  call :clear
)
if /i "%~2"=="clear" (
  call :clear
)

pushd build || exit /b 1

for %%e in (bench_query_xlarge bench_select_scale bench_query_perquery bench_perquery_compile) do (
  if not exist %%e.exe (
    echo ERROR: build\%%e.exe is missing, build the benchmarks first
    popd
    exit /b 1
  )
)

copy /y ..\src\third_party\duckdb\duckdb.dll duckdb.dll >nul

set "OUT=%CD%\paper_runs"
set "failed="
set "clear_caches=1"
set "driver_cache=0"
if not defined SEED (
  set "SEED=1794614196"
)

:: the three configurations of the ablation: generation off with library kernels, generation off with the interpreter, storage features off
set "LIB=QE_PERQUERY_KERNELS:0,GPU_VULKAN_MAX_CACHED_KERNELS:64"
set "INTERP=QE_PERQUERY_KERNELS:0,QE_SCAN_SIMPLE_KERNEL:0,GPU_VULKAN_MAX_CACHED_KERNELS:64"
set "NOSTORE=QE_ENCODE:0,QE_ENCODE_SEQUENCE:0,QE_ZONEMAP:0"
:: the router: the first execution of a conjunction of plain comparisons runs on the precompiled path, the kernel is generated on the second
set "ROUTE=QE_ROUTE_CONJUNCTIONS:1,GPU_VULKAN_MAX_CACHED_KERNELS:64"

if "%only_overnight%"=="1" (
  call :coldcost
  call :sweepmore
  call :libmore
  call :families
  goto :finish
)

if "%only_specialize%"=="1" (
  call :specialize
  goto :finish
)

if "%only_steadyagain%"=="1" (
  call :steadyagain
  goto :finish
)

if "%only_libtouch%"=="1" (
  call :libtouch
  goto :finish
)

if "%only_duckdbsep%"=="1" (
  call :duckdbsep
  goto :finish
)

if "%only_spinmore%"=="1" (
  call :spinmore
  goto :finish
)

if "%only_treegap%"=="1" (
  call :treegap
  goto :finish
)

if "%only_treespec%"=="1" (
  call :treespec
  goto :finish
)

if "%only_spinspec%"=="1" (
  call :spinspec
  goto :finish
)

if "%only_spinshapes%"=="1" (
  call :spinshapes
  goto :finish
)

if "%only_spinwarm%"=="1" (
  call :spinwarm
  goto :finish
)

if "%only_fusedcount%"=="1" (
  call :fusedcount
  goto :finish
)

if "%only_datashapes%"=="1" (
  call :datashapes
  goto :finish
)

if "%only_failure%"=="1" (
  call :failure
  goto :finish
)

if "%only_coldcost%"=="1" (
  call :coldcost
  goto :finish
)

if "%only_sweepmore%"=="1" (
  call :sweepmore
  goto :finish
)

if "%only_libmore%"=="1" (
  call :libmore
  goto :finish
)

if "%only_families%"=="1" (
  call :families
  goto :finish
)

if "%only_1b%"=="1" (
  call :billion
  goto :finish
)

if "%only_scaling%"=="1" (
  call :scaling
  goto :finish
)

if "%only_router%"=="1" (
  call :router
  goto :finish
)

if "%only_routerfilled%"=="1" (
  call :filled route "%ROUTE%"
  goto :finish
)

if "%only_interleaved%"=="1" (
  call :interleaved
  goto :finish
)

if "%only_libnew%"=="1" (
  call :libnew
  goto :finish
)

if "%only_sweep%"=="1" (
  call :sweep
  goto :finish
)

if "%only_profile%"=="1" (
  call :profile
  goto :finish
)

if "%only_defaultfilled%"=="1" (
  call :filled default "GPU_VULKAN_MAX_CACHED_KERNELS:64"
  goto :finish
)

call :correctness
call :suites
call :workloads
call :compressed

call :ablate lib "%LIB%" 1
call :ablate interp "%INTERP%" 0
call :ablate nostore "%NOSTORE%" 1

call :filled lib "%LIB%"
call :filled interp "%INTERP%"

call :compile
call :router

:finish
popd

echo.
echo reports and logs are in build\paper_runs
if "%failed%"=="" (
  echo every run exited with 0
  endlocal
  exit /b 0
)
echo runs that failed:%failed%
endlocal & exit /b 1


::~
:: correctness, each generated kernel against the reference path (row counts and checksums are compared inside the tests).
:: the tests do not need empty caches
:correctness
set "clear_caches=0"

call :run correctness perquery_default bench_reports\perquery_report.md "bench_query_perquery.exe --rows=3000000"
call :run correctness perquery_streamed bench_reports\perquery_report.md "bench_query_perquery.exe --rows=3000000 --setting=GPU_MAX_BUFFER_SIZE:4000000"
call :run correctness perquery_narrow bench_reports\perquery_report.md "bench_query_perquery.exe --rows=3000000 --narrow --setting=GPU_MAX_BUFFER_SIZE:4000000"
call :run correctness perquery_20m bench_reports\perquery_report.md "bench_query_perquery.exe --rows=20000000"

call :run correctness perquery_edge_default bench_reports\perquery_edge_report.md "bench_query_perquery.exe --edge --rows=3000000"
call :run correctness perquery_edge_streamed bench_reports\perquery_edge_report.md "bench_query_perquery.exe --edge --rows=3000000 --setting=GPU_MAX_BUFFER_SIZE:4000000"
call :run correctness perquery_edge_narrow bench_reports\perquery_edge_report.md "bench_query_perquery.exe --edge --rows=3000000 --narrow --setting=GPU_MAX_BUFFER_SIZE:4000000"

call :run correctness fused_3m bench_reports\fused_report.md "bench_query_fused.exe --rows=3000000"
call :run correctness fused_20m bench_reports\fused_report.md "bench_query_fused.exe --rows=20000000"
call :run correctness fused_narrow_3m bench_reports\fused_report.md "bench_query_fused.exe --rows=3000000 --narrow"
call :run correctness fused_narrow_20m bench_reports\fused_report.md "bench_query_fused.exe --rows=20000000 --narrow"

call :run correctness sort_3m x "bench_query_sort.exe --rows=3000000"
call :run correctness sort_20m x "bench_query_sort.exe --rows=20000000"
call :run correctness window_rank_200k x "bench_query_window_rank.exe --rows=200000"
call :run correctness window_rank_5m x "bench_query_window_rank.exe --rows=5000000"
call :run correctness join_gen_3m x "bench_query_join_gen.exe --rows=3000000"
call :run correctness join_gen_20m x "bench_query_join_gen.exe --rows=20000000"
call :run correctness join_gen_small_buffers x "bench_query_join_gen.exe --rows=20000000 --small-buffers"
call :run correctness hash_group_3m x "bench_query_hash_group.exe --rows=3000000"
call :run correctness having_3m x "bench_query_having.exe --rows=3000000"
call :run correctness member_reduce_3m x "bench_query_member_reduce.exe --rows=3000000"
call :run correctness compact_3m x "bench_query_compact.exe --rows=3000000"

set "clear_caches=1"
exit /b 0


::~
:: the other test programs, a failing one is a regression to look at
:suites
set "clear_caches=0"

for %%t in (small large string_cmp string_null fuzzy_search window cte subquery wide_int agg_typing agg_resident approx_agg join_scaling optimizer) do (
  if exist bench_query_%%t.exe (
    call :run suites %%t x "bench_query_%%t.exe"
  ) else (
    call :run suites %%t x "bench_%%t.exe"
  )
)

call :run suites explain_analyze x "bench_explain_analyze.exe"
call :run suites alloc_fault x "bench_alloc_fault.exe"
call :run suites typed_interp x "bench_typed_interp.exe"
call :run suites chunked_hash x "bench_query_chunked_hash.exe"

set "clear_caches=1"
exit /b 0


::~
:: the 37 query workload (5 runs), the scale test at 10M and 100M rows (3 runs each) and at 1B rows (2 runs), all against duckdb
:workloads
for /l %%i in (1,1,5) do (
  call :run xlarge run_%%i bench_reports\xlarge_report.md "bench_query_xlarge.exe --no_sqlite --samples"
)

for %%n in (10000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run scale %%n_%%i bench_reports\select_scale_%%n_report.md "bench_select_scale.exe --rows=%%n --samples --no_bandwidth"
  )
)

if "%run_1b%"=="1" (
  for /l %%i in (1,1,2) do (
    call :run scale 1000000000_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples"
  )
)
exit /b 0


::~
:: only the runs at 1B rows: the workload and the two ablation passes that have them
:billion
for /l %%i in (1,1,2) do (
  call :run scale 1000000000_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples"
)

for /l %%i in (1,1,2) do (
  call :run ablation\lib scale_1000000000_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples --setting=%LIB%"
)

for /l %%i in (1,1,2) do (
  call :run ablation\nostore scale_1000000000_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples --setting=%NOSTORE%"
)
exit /b 0


::~
:: the same workloads with duckdb's tables compressed (no 1B rows)
:compressed
for /l %%i in (1,1,5) do (
  call :run xlarge_tuned run_%%i bench_reports\xlarge_report.md "bench_query_xlarge.exe --no_sqlite --samples --duckdb_checkpoint --duckdb_threads=%NUMBER_OF_PROCESSORS%"
)

for %%n in (10000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run scale_tuned %%n_%%i bench_reports\select_scale_%%n_report.md "bench_select_scale.exe --rows=%%n --samples --no_bandwidth --duckdb_checkpoint --duckdb_narrow_only --duckdb_threads=%NUMBER_OF_PROCESSORS%"
  )
)
exit /b 0


:: the four configurations in one queue, in five rounds, the order rotated each round so that no configuration always runs first or last: the 37-query workload and the scale test at 10M and 100M rows,
:: then, when run_1b is set, five rounds of the 1B row scale test on default, library kernels and storage features off
:interleaved
call :iround 1 default lib interp nostore
call :iround 2 lib interp nostore default
call :iround 3 interp nostore default lib
call :iround 4 nostore default lib interp
call :iround 5 nostore interp lib default
if "%run_1b%"=="1" (
  for /l %%r in (1,1,5) do (
    for %%c in (default lib nostore) do (
      call :irun interleaved1b %%c scale_1000000000_%%r bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples"
    )
  )
)
exit /b 0


:: iround <round> <configuration> x4
:iround
for %%c in (%2 %3 %4 %5) do (
  call :irun interleaved %%c xlarge_%1 bench_reports\xlarge_report.md "bench_query_xlarge.exe --no_sqlite --samples"
  for %%n in (10000000 100000000) do (
    call :irun interleaved %%c scale_%%n_%1 bench_reports\select_scale_%%n_report.md "bench_select_scale.exe --rows=%%n --samples --no_bandwidth"
  )
)
exit /b 0


:: irun <folder> <configuration> <name> <report> "<command>"
:irun
call :icfg %2
call :run %1\%2 %3 %4 "%~5 --setting=%ICFG%"
exit /b 0


:: icfg <configuration>: sets ICFG to its settings
:icfg
set "ICFG=GPU_VULKAN_MAX_CACHED_KERNELS:64"
if "%1"=="lib" set "ICFG=%LIB%"
if "%1"=="interp" set "ICFG=%INTERP%"
if "%1"=="nostore" set "ICFG=%NOSTORE%"
exit /b 0


::~
:: the row-number filter, SELECT COUNT(*) WHERE id > 1000 AND id <= 900000000: the interpreter, the precompiled kernel and the generated kernel timed on the one query, with the generated pipeline's registers and instruction lines,
:: at 10M and 100M rows, three processes each, and the same with the sequence encoding off, where id is a column that is read
:profile
for %%n in (10000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run profile idrange_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
    call :run profile idrange_noseq_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1,QE_ENCODE_SEQUENCE:0"
  )
)
exit /b 0


::~
:: selectivity sweep of fixed numeric shapes and string and dictionary shapes, each as SELECT COUNT(*): interpreter, precompiled and generated steady times, three processes at each size
:sweep
for %%n in (10000000 50000000) do (
  for /l %%i in (1,1,3) do (
    call :run sweep sweep_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
  )
)
exit /b 0


::~
:: new shapes against library kernels, three processes of each state, 40 shapes at 20M rows:
:: gen_empty, lib_empty: no caches (the engine's disk caches off, the driver's cache off)
:: gen_filled, lib_filled: the engine's disk caches and the driver's cache filled by the process before, which has the same arguments
:libnew
for /l %%i in (1,1,3) do (
  call :run libnew gen_empty_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
  call :run libnew lib_empty_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --lib --setting=QE_SPIRV_DISK_CACHE:0"
)
call :clear
set "clear_caches=0"
call :run libnew gen_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED%"
for /l %%i in (1,1,3) do (
  call :run libnew gen_filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED%"
)
call :clear
call :run libnew lib_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --lib"
for /l %%i in (1,1,3) do (
  call :run libnew lib_filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --lib"
)
set "clear_caches=1"
exit /b 0


::~
:: the cold cost of one process itemized by probe (shaderc, pipeline creation, the rest), three processes at each size with empty caches,
:: then at 20M rows with the engine's disk caches and the driver's cache filled by the process before
:coldcost
for %%n in (5000000 20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run coldcost empty_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --cold --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
call :clear
set "clear_caches=0"
set "driver_cache=1"
call :run coldcost filled_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --cold --seed=%SEED%"
for /l %%i in (1,1,3) do (
  call :run coldcost filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --cold --seed=%SEED%"
)
set "clear_caches=1"
set "driver_cache=0"
exit /b 0


::~
:: the sweep with a throwaway query first (does the first literal carry a first-query effect), and at other table sizes
:sweepmore
for %%n in (10000000 50000000) do (
  for /l %%i in (1,1,3) do (
    call :run sweep_warm warm_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
  )
)
for %%n in (1000000 5000000 20000000) do (
  for /l %%i in (1,1,3) do (
    call :run sweep_rows rows_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
  )
)
exit /b 0


::~
:: new shapes against library kernels at other table sizes, the layout of :libnew
:libmore
for %%n in (5000000 100000000) do (
  call :libsize %%n
)
exit /b 0


:libsize
set "clear_caches=1"
for /l %%i in (1,1,3) do (
  call :run libnew_%1 gen_empty_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
  call :run libnew_%1 lib_empty_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --lib --setting=QE_SPIRV_DISK_CACHE:0"
)
call :clear
set "clear_caches=0"
set "driver_cache=1"
call :run libnew_%1 gen_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED%"
for /l %%i in (1,1,3) do (
  call :run libnew_%1 gen_filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED%"
)
call :clear
call :run libnew_%1 lib_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --lib"
for /l %%i in (1,1,3) do (
  call :run libnew_%1 lib_filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --lib"
)
set "clear_caches=1"
set "driver_cache=0"
exit /b 0


::~
:: the steady and cold cost of the numeric conjunctions with the fused count off: generated scan, fixed widths in the source, specialization constants
:specialize
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run spec sw_default_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
    call :run spec2 sw_default_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,GPU_VULKAN_SHADER_INFO:1"
    call :run spec2 sw_fixed_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_GEN_FIXED_WIDTH:1,GPU_VULKAN_SHADER_INFO:1"
    call :run spec2 sw_spec_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_SCAN_SPEC:1,GPU_VULKAN_SHADER_INFO:1"
  )
)
for /l %%i in (1,1,3) do (
  call :run spec3 cold_default_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --cold --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0"
  call :run spec3 cold_spec_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --cold --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_SCAN_SPEC:1"
  call :run gap g_on_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=10000000 --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
  call :run gap g_off_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=10000000 --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,GPU_VULKAN_SHADER_INFO:1"
)
exit /b 0


::~
:: the 40 shapes, each measured a second time after the last shape (--resteady) with every kernel built, at 5M, 20M and 100M rows
:steadyagain
for %%n in (5000000 20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run steady again_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --shapes=40 --seed=%SEED% --resteady --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
exit /b 0


::~
:: new shapes against library kernels as in :libmore, with every column read once before the first shape (--touch), so that the upload of a column is not in the first run of the shape that uses it first
:libtouch
for %%n in (5000000 20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run libtouch_%%n lib_empty_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --shapes=40 --seed=%SEED% --lib --touch --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
for %%n in (5000000 20000000 100000000) do (
  call :run libtouch_%%n lib_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --shapes=40 --seed=%SEED% --lib --touch"
  for /l %%i in (1,1,3) do (
    call :run libtouch_%%n lib_filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --shapes=40 --seed=%SEED% --lib --touch"
  )
)
exit /b 0


::~
:: the one billion row scale test with each engine in a process of its own, so neither holds the other's copy of the table in memory
:duckdbsep
for /l %%i in (1,1,2) do (
  call :run sep1b cql_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples --no_duckdb"
  call :run sep1b duckdb_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples --no_bandwidth --duckdb_only"
)
exit /b 0


::~
:: the steady measurements again with a 300 ms busy warm-up before the timed runs of every steady time (--spinwarm): the 40 shapes, the sweep and the row-number range with the fused kernels, and the library comparison
:spinwarm
for %%n in (5000000 20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run spin again_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --shapes=40 --seed=%SEED% --resteady --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run spin sw_gen_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
    call :run spin sw_spec_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_COUNT_SPEC:1"
    call :run spin id_gen_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
    call :run spin id_spec_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_COUNT_SPEC:1"
  )
)
for %%n in (5000000 20000000 100000000) do (
  call :spinlibsize %%n
)
exit /b 0


:spinlibsize
set "clear_caches=1"
for /l %%i in (1,1,3) do (
  call :run spin_lib_%1 gen_empty_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
  call :run spin_lib_%1 lib_empty_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --lib --touch --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
)
call :clear
set "clear_caches=0"
set "driver_cache=1"
call :run spin_lib_%1 gen_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --spinwarm"
for /l %%i in (1,1,3) do (
  call :run spin_lib_%1 gen_filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --spinwarm"
  call :run spin_lib_%1 lib_filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%1 --shapes=40 --seed=%SEED% --lib --touch --spinwarm"
)
set "clear_caches=1"
set "driver_cache=0"
exit /b 0


::~
:: the fusion off, fixed width and scan specialization runs of the sweep and the row-number range, and the single comparison at 10M, with the busy warm-up (--spinwarm)
:spinspec
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run spin2 sw_off_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,GPU_VULKAN_SHADER_INFO:1"
    call :run spin2 id_off_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0"
    call :run spin2 sw_fixed_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_GEN_FIXED_WIDTH:1,GPU_VULKAN_SHADER_INFO:1"
    call :run spin2 id_fixed_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_GEN_FIXED_WIDTH:1"
    call :run spin2 sw_scan_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_SCAN_SPEC:1,GPU_VULKAN_SHADER_INFO:1"
    call :run spin2 id_scan_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_SCAN_SPEC:1"
  )
)
for /l %%i in (1,1,3) do (
    call :run spin2 g_on_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=10000000 --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
    call :run spin2 g_off_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=10000000 --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,GPU_VULKAN_SHADER_INFO:1"
)
exit /b 0


::~
:: the other selectivities, skewed and clustered data and other select lists with the busy warm-up
:spinshapes
for %%f in (0.001 0.01 0.1) do (
  for /l %%i in (1,1,3) do (
    call :run spin_frac f%%f_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --fraction=%%f --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
for /l %%i in (1,1,3) do (
  call :run spin_data skew3_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --skew=3 --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run spin_data cluster_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --cluster --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run spin_data sum_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --select=SUM(e) --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run spin_data wide_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --select=id,a,e --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run spin_data minmax_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --select=MIN(a),MAX(e),SUM(d) --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
)
exit /b 0


::~
:: the 40 shapes with every leaf evaluated in the generated kernels, to test whether the short circuit explains the steady gap of the specialized module
:treegap
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run spin_gap nosc_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --shapes=40 --seed=%SEED% --treespec --spinwarm --setting=QE_SPIRV_DISK_CACHE:0,QE_GEN_NO_SHORT_CIRCUIT:1"
  )
)
exit /b 0


::~
:: the sweep at five sizes with the busy warm-up, and the specialized module with the SPIR-V and driver caches filled
:spinmore
for %%n in (1000000 5000000 10000000 20000000 50000000) do (
  for /l %%i in (1,1,3) do (
    call :run spin3 sweep_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
  )
)
call :clear
set "clear_caches=0"
set "driver_cache=1"
call :run spin_treecache fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --treespec --spinwarm"
for /l %%i in (1,1,3) do (
  call :run spin_treecache filled_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --treespec --spinwarm"
)
set "clear_caches=1"
set "driver_cache=0"
exit /b 0


::~
:: the interpreter with the WHERE clause as specialization constants, the 40 shapes at three sizes and the sweep, with the busy warm-up
:treespec
for %%n in (5000000 20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run spin_tree shapes_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --shapes=40 --seed=%SEED% --treespec --spinwarm --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
for /l %%i in (1,1,3) do (
  call :run spin_tree sweep_20000000_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --sweep --warmup --treespec --spinwarm --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
)
exit /b 0


::~
:: COUNT(*) over numeric conjunctions with the generated fused kernel against one precompiled fused count module with specialization constants (QE_FUSE_COUNT_SPEC:1), steady and first use
:fusedcount
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run fcs sw_gen_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"
    call :run fcs sw_spec_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --sweep --warmup --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_COUNT_SPEC:1,GPU_VULKAN_SHADER_INFO:1"
    call :run fcs id_gen_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
    call :run fcs id_spec_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_COUNT_SPEC:1"
  )
)
for /l %%i in (1,1,3) do (
  call :run fcs cold_gen_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --cold --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
  call :run fcs cold_spec_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --cold --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_COUNT_SPEC:1"
)
exit /b 0


::~
:: the 40 shapes at 20M rows with other selectivities (--fraction), skewed and clustered data and other select lists, and the row-number filter with generation in four forms.
:: the ISA sizes of Table D9 are not made by this script: they come from amdllpc.exe --gfxip=gfx1031 --auto-layout-desc -filetype=asm on the .spv files of build\shaders\cache
:datashapes
for %%f in (0.001 0.01 0.1) do (
  for /l %%i in (1,1,3) do (
    call :run frac f%%f_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --fraction=%%f --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
for /l %%i in (1,1,3) do (
  call :run data skew3_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --skew=3 --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run data cluster_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --cluster --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run data sum_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --select=SUM(e) --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run data wide_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --select=id,a,e --setting=QE_SPIRV_DISK_CACHE:0"
)
for /l %%i in (1,1,3) do (
  call :run data minmax_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --select=MIN(a),MAX(e),SUM(d) --setting=QE_SPIRV_DISK_CACHE:0"
)
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run idr fused_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
  )
)
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run idr off_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0"
  )
)
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run idr fixed_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_GEN_FIXED_WIDTH:1"
  )
)
for %%n in (20000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run idr spec_%%n_%%i bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=%%n --idrange --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,QE_FUSE_AGGREGATE:0,QE_SCAN_SPEC:1"
  )
)
exit /b 0


::~
:: failures of generation injected into the 40 scan shapes, and forced kernel name collisions with and without the second hash.
:: aggregation queries are NOT run with these settings: QE_TEST_FAIL_SHADERC:1 on the 37 queries hung the GPU on "group by user (1M)" and crashed the machine
:failure
call :run failure base bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=2000000 --shapes=40 --seed=1 --setting=QE_SPIRV_DISK_CACHE:0"
call :run failure shaderc bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=2000000 --shapes=40 --seed=1 --setting=QE_SPIRV_DISK_CACHE:0,QE_TEST_FAIL_SHADERC:1"
call :run failure pipeline bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=2000000 --shapes=40 --seed=1 --setting=QE_SPIRV_DISK_CACHE:0,QE_TEST_FAIL_PIPELINE:1"
call :run failure shaderc3 bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=2000000 --shapes=40 --seed=1 --setting=QE_SPIRV_DISK_CACHE:0,QE_TEST_FAIL_SHADERC:3"
call :run failure pipeline3 bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=2000000 --shapes=40 --seed=1 --setting=QE_SPIRV_DISK_CACHE:0,QE_TEST_FAIL_PIPELINE:3"
call :run failure collide_on bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=2000000 --shapes=40 --seed=1 --setting=QE_SPIRV_DISK_CACHE:0,QE_TEST_KEY_BITS:2"
:: the next run is expected to exit with code 1, wrong counts are the result
call :run failure collide_off bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=2000000 --shapes=40 --seed=1 --setting=QE_SPIRV_DISK_CACHE:0,QE_TEST_KEY_BITS:2,QE_COLLISION_CHECK:0"
exit /b 0


::~
:: the workloads with one generation switch off at a time, and a default pass beside them, in folders ablation\fam_<name>
:families
call :ablate fam_default "GPU_VULKAN_MAX_CACHED_KERNELS:64" 0
for %%g in (SORT JOIN GATHER COMPACT HASH_REDUCE HAVING MEMBER_REDUCE) do (
  call :ablate fam_%%g "GPU_VULKAN_MAX_CACHED_KERNELS:64,QE_GENERATE_%%g:0" 0
)
call :ablate fam_FUSE_GROUP_BY "GPU_VULKAN_MAX_CACHED_KERNELS:64,QE_FUSE_GROUP_BY:0" 0
call :ablate fam_FUSE_AGGREGATE "GPU_VULKAN_MAX_CACHED_KERNELS:64,QE_FUSE_AGGREGATE:0" 0
exit /b 0


::~
:: ablate <name> "<settings>" <1 to also run 1B rows>
:: the same workloads with one configuration of settings, without the 1B rows for the interpreter
:ablate
for /l %%i in (1,1,5) do (
  call :run ablation\%~1 xlarge_%%i bench_reports\xlarge_report.md "bench_query_xlarge.exe --no_sqlite --samples --setting=%~2"
)

for %%n in (10000000 100000000) do (
  for /l %%i in (1,1,3) do (
    call :run ablation\%~1 scale_%%n_%%i bench_reports\select_scale_%%n_report.md "bench_select_scale.exe --rows=%%n --samples --no_bandwidth --setting=%~2"
  )
)

if "%~3"=="1" if "%run_1b%"=="1" (
  for /l %%i in (1,1,2) do (
    call :run ablation\%~1 scale_1000000000_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples --setting=%~2"
  )
)
exit /b 0


::~
:: filled <name> "<settings>"
:: cold first execution with the engine's caches filled by an earlier process: the caches are deleted once, then six processes keep them
:filled
call :clear
set "clear_caches=0"

for %%i in (fill 1 2 3 4 5) do (
  call :run cold_filled\%~1 xlarge_%%i bench_reports\xlarge_report.md "bench_query_xlarge.exe --no_sqlite --samples --setting=%~2"
)

set "clear_caches=1"
exit /b 0


::~
:: what a new query shape costs: cold (no caches), driver (a new process, the driver's cache warm),
:: then with the engine's disk caches filled by the run before and used
:compile
:: cold: no caches at all, the driver's cache is off
call :run compile cold bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"

:: sizes of the generated source and of the SPIR-V, and the registers and instructions of each pipeline (VK_AMD_shader_info, the driver's cache is off)
call :run compile stats bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0,GPU_VULKAN_SHADER_INFO:1"

:: driver: the driver's cache is on, one process builds the shapes into it and the next one reads them
set "driver_cache=1"
call :run compile driver_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
call :run compile driver bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
call :run compile disk_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED%"

set "clear_caches=0"
call :run compile disk bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED%"
set "clear_caches=1"
set "driver_cache=0"
call :scaling
exit /b 0


::~
:: the cold new-shape cost of the same 40 shapes at other table sizes, to see whether the cost depends on the number of rows
:scaling
call :run compile rows_5m bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=5000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
call :run compile rows_100m bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=100000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
exit /b 0


::~
:: the conjunction router. the cost study with --route: the "cold first run" column of a routed conjunction is its run on the precompiled path, the "second run" column the run that generates.
:: the 37-query workload and the scale test at 10M and 100M rows run with the router on (the cold times are the router's first executions, the warm times are after the kernels exist)
:router
call :run router compile_cold bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --route --setting=QE_SPIRV_DISK_CACHE:0"
call :run router compile_rows_5m bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=5000000 --shapes=40 --seed=%SEED% --route --setting=QE_SPIRV_DISK_CACHE:0"
call :run router compile_rows_100m bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=100000000 --shapes=40 --seed=%SEED% --route --setting=QE_SPIRV_DISK_CACHE:0"
call :ablate route "%ROUTE%" 0
call :filled route "%ROUTE%"
call :filled default "GPU_VULKAN_MAX_CACHED_KERNELS:64"
exit /b 0


::~
:: run <folder> <name> <report the benchmark writes, relative to build, or x for none> "<command line>"
:run
echo === %~1\%~2

if not exist "%OUT%\%~1" (
  mkdir "%OUT%\%~1"
)

if not "%~3"=="x" (
  if exist "%~3" (
    del "%~3"
  )
)

if "%clear_caches%"=="1" (
  call :clear
  if not "%driver_cache%"=="1" (
    set "AMD_VK_USE_PIPELINE_CACHE=0"
  )
)

.\%~4 > "%OUT%\%~1\%~2.log" 2>&1
set "rc=%errorlevel%"
set "AMD_VK_USE_PIPELINE_CACHE="

if not "%rc%"=="0" (
  echo     failed with exit code %rc%
  set "failed=%failed% [%~1\%~2]"
)

if not "%~3"=="x" (
  if exist "%~3" (
    copy /y "%~3" "%OUT%\%~1\%~2.md" >nul
  )
)
exit /b 0


::~
:: deletes the engine's shader and pipeline caches (shaders\cache and shaders\pipeline_cache.bin, next to the exes in build, or below build when called from the repo root)
:clear
if exist shaders\cache (
  rd /s /q shaders\cache
)
if exist shaders\pipeline_cache.bin (
  del shaders\pipeline_cache.bin
)
if exist build\shaders\cache (
  rd /s /q build\shaders\cache
)
if exist build\shaders\pipeline_cache.bin (
  del build\shaders\pipeline_cache.bin
)
exit /b 0
