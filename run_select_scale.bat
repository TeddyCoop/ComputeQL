@echo off
setlocal

:: tec: builds and runs the select scale test at 10M, 100M and 1B rows against duckdb, one process per size so each run starts with free memory.
:: duckdb at 1B rows holds its own copy of the table next to cql's, if that is too much for the machine pass --duckdb_max_rows=100000000
:: extra args pass through to every run, e.g. run_select_scale.bat --no_duckdb. pass --rows=N to run a single size
:: tec: cl by default, set CQL_CLANG=1 to use clang
call .\bench_cc.bat
if not exist build\%cc_sqlite_obj% (
  echo ERROR: build\%cc_sqlite_obj% missing, run build_and_run_tests.bat once first.
  exit /b 1
)

copy /y src\third_party\duckdb\duckdb.dll build\duckdb.dll >nul

pushd build
%cc_compile% ..\src\tests\scale\bench_select_scale.c %cc_sqlite_obj% %cc_link_vk_duck% %cc_exe_out%bench_select_scale.exe || (popd & exit /b 1)

echo %* | findstr /i /c:"--rows" >nul
if not errorlevel 1 (
  call .\bench_select_scale.exe %*
  set run_status=%errorlevel%
  popd
  exit /b %run_status%
)

set overall=0
for %%r in (10000000 100000000 1000000000) do (
  echo.
  echo === select scale: %%r rows ===
  call .\bench_select_scale.exe --rows=%%r %*
  if errorlevel 1 set overall=1
)
popd

endlocal & exit /b %overall%
