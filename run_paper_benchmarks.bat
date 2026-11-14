@echo off
setlocal

:: tec: runs every benchmark and test that the per query kernel paper takes its numbers from.
:: build the benchmarks first with build_and_run_tests.bat, run_scale_bench.bat and run_select_scale.bat (cl or clang).
::
:: usage: run_paper_benchmarks.bat          runs everything
::        run_paper_benchmarks.bat clear    deletes the engine's shader and pipeline caches first, then runs everything
if /i "%~1"=="clear" (
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
if not defined SEED (
  set "SEED=1794614196"
)

:: the three configurations of the ablation: generation off with library kernels, generation off with the interpreter, storage features off
set "LIB=QE_PERQUERY_KERNELS:0,GPU_VULKAN_MAX_CACHED_KERNELS:64"
set "INTERP=QE_PERQUERY_KERNELS:0,QE_SCAN_SIMPLE_KERNEL:0,GPU_VULKAN_MAX_CACHED_KERNELS:64"
set "NOSTORE=QE_ENCODE:0,QE_ENCODE_SEQUENCE:0,QE_ZONEMAP:0"

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

for /l %%i in (1,1,2) do (
  call :run scale 1000000000_%%i bench_reports\select_scale_1000000000_report.md "bench_select_scale.exe --rows=1000000000 --samples"
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

if "%~3"=="1" (
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
call :run compile cold bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
call :run compile driver bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED% --setting=QE_SPIRV_DISK_CACHE:0"
call :run compile disk_fill bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED%"

set "clear_caches=0"
call :run compile disk bench_reports\perquery_compile_report.md "bench_perquery_compile.exe --rows=20000000 --shapes=40 --seed=%SEED%"
set "clear_caches=1"
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
)

.\%~4 > "%OUT%\%~1\%~2.log" 2>&1
set "rc=%errorlevel%"

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
