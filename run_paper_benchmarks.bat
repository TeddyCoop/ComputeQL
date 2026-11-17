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
