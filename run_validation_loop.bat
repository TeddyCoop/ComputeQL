@echo off
setlocal enabledelayedexpansion

:: tec: runs the streamed select scale test repeatedly with the Vulkan validation layers (GPU assisted + synchronization validation) and a GPU buffer far below the column size, so every scan is chunked
:: usage: run_validation_loop.bat [iterations] [rows] [max_buffer_bytes]   defaults are 200, 20000000, 10000000
:: validation makes each run slow, lower the row count if 200 iterations take too long
:: logs go to build\bench_reports\validation_loop, a run counts as bad when it exits non zero or prints a validation message or a verification failure
if not exist build\sqlite3.obj (
  echo ERROR: build\sqlite3.obj missing, run build_and_run_tests.bat once first.
  exit /b 1
)

set iterations=%1
if "%iterations%"=="" set iterations=200
set rows=%2
if "%rows%"=="" set rows=20000000
set max_buffer=%3
if "%max_buffer%"=="" set max_buffer=10000000

copy /y src\third_party\duckdb\duckdb.dll build\duckdb.dll >nul

pushd build
call cl /O2 /DNDEBUG /I..\src\ /I..\src\third_party\vulkan\ /nologo /FC /Z7 ..\src\tests\scale\bench_select_scale.c sqlite3.obj /link ..\src\third_party\vulkan\vulkan-1.lib ..\src\third_party\duckdb\duckdb.lib /out:bench_select_scale.exe || (popd & exit /b 1)
if not exist bench_reports\validation_loop mkdir bench_reports\validation_loop
del /q bench_reports\validation_loop\*.log >nul 2>&1

set bad=0
for /l %%i in (1,1,%iterations%) do (
  call .\bench_select_scale.exe --rows=%rows% --no_duckdb --no_bandwidth --setting=CQL_VULKAN_VALIDATION:true --setting=GPU_MAX_BUFFER_SIZE:%max_buffer% > bench_reports\validation_loop\run_%%i.log 2>&1
  set run_bad=0
  if errorlevel 1 set run_bad=1
  findstr /i /c:"VUID" /c:"Validation Error" /c:"SYNC-" /c:"MISMATCH" /r /c:"[1-9][0-9]* of [0-9]* queries failed verification" bench_reports\validation_loop\run_%%i.log >nul
  if not errorlevel 1 set run_bad=1
  if "!run_bad!"=="1" (
    set /a bad+=1
    echo run %%i: BAD, see build\bench_reports\validation_loop\run_%%i.log
  ) else (
    echo run %%i: ok
    del /q bench_reports\validation_loop\run_%%i.log
  )
)
popd

echo.
echo %bad% of %iterations% runs were bad
endlocal & exit /b %bad%
