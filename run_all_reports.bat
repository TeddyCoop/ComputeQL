@echo off
setlocal

:: tec: rebuilds and runs everything that writes a report, in order, and prints which steps failed at the end. a failing step does not stop the ones after it.
:: run it from a prompt where cl is on the path (a VS developer prompt). extra args go to the scale suites only, e.g. run_all_reports.bat --no_duckdb
::
::   1. build_and_run_tests.bat    every bench_*.c and net_*.c test        -> build\bench_reports\*.md
::   2. run_scale_bench.bat        the large scale suite (bench_query_xlarge) -> build\bench_reports\xlarge_report.md
::   3. run_select_scale.bat       select scale test at 10M, 100M, 1B rows   -> build\bench_reports\select_scale_<rows>_report.md
::   4. run_gpu_transfer.bat       host to VRAM transfer benchmark           -> build\bench_reports\gpu_transfer_report.txt

set failed=
set steps_run=0

call :step "tests" build_and_run_tests.bat
call :step "scale bench" run_scale_bench.bat %*
call :step "select scale" run_select_scale.bat %*
call :step "gpu transfer" run_gpu_transfer.bat

echo.
echo === reports in build\bench_reports ===
if exist build\bench_reports dir /b /o:d build\bench_reports

echo.
if "%failed%"=="" (
  echo all %steps_run% steps finished without a failure exit code
  endlocal
  exit /b 0
)
echo steps that failed:%failed%
endlocal & exit /b 1

:step
echo.
echo ==============================================================
echo === %~1
echo ==============================================================
set /a steps_run+=1
call "%~dp0%~2" %3 %4 %5 %6 %7 %8 %9
if errorlevel 1 set failed=%failed% [%~1]
exit /b 0
