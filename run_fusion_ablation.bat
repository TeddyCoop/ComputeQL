@echo off
setlocal

:: tec: runs bench_query_xlarge with QE_OPT_FUSE off and on at each row count, one process per run, and keeps a copy of each report in build\bench_reports\fusion_ablation
:: usage: run_fusion_ablation.bat [rows ...]   default rows are 20000 2000000 10000000
:: the on/off reports of a row count are the ones to compare, rerun the pair a few times to see the run to run noise
if not exist build\sqlite3.obj (
  echo ERROR: build\sqlite3.obj missing, run build_and_run_tests.bat once first.
  exit /b 1
)

set rows_list=%*
if "%rows_list%"=="" set rows_list=20000 2000000 10000000

copy /y src\third_party\duckdb\duckdb.dll build\duckdb.dll >nul

pushd build
call cl /O2 /DNDEBUG /I..\src\ /I..\src\third_party\vulkan\ /nologo /FC /Z7 ..\src\tests\scale\bench_query_xlarge.c sqlite3.obj /link ..\src\third_party\vulkan\vulkan-1.lib ..\src\third_party\duckdb\duckdb.lib /out:bench_query_xlarge.exe || (popd & exit /b 1)
if not exist bench_reports\fusion_ablation mkdir bench_reports\fusion_ablation

set overall=0
for %%r in (%rows_list%) do (
  for %%f in (false true) do (
    echo.
    echo === fusion %%f, %%r rows ===
    call .\bench_query_xlarge.exe --no_sqlite --no_duckdb --rows=%%r --setting=QE_OPT_FUSE:%%f
    if errorlevel 1 set overall=1
    copy /y bench_reports\xlarge_report.md bench_reports\fusion_ablation\xlarge_%%r_fuse_%%f.md >nul
  )
)
popd

echo.
echo reports are in build\bench_reports\fusion_ablation
endlocal & exit /b %overall%
