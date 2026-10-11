@echo off
setlocal

:: tec: builds and runs the select scale test at 10M, 100M and 1B rows against duckdb, one process per size so each run starts with free memory.
:: duckdb at 1B rows holds its own copy of the table next to cql's, if that is too much for the machine pass --duckdb_max_rows=100000000
:: extra args pass through to every run, e.g. run_select_scale.bat --no_duckdb. pass --rows=N to run a single size
if not exist build\sqlite3.obj (
  echo ERROR: build\sqlite3.obj missing, run build_and_run_tests.bat once first.
  exit /b 1
)

copy /y src\third_party\duckdb\duckdb.dll build\duckdb.dll >nul

pushd build
call cl /O2 /DNDEBUG /I..\src\ /I..\src\third_party\vulkan\ /nologo /FC /Z7 ..\src\tests\scale\bench_select_scale.c sqlite3.obj /link ..\src\third_party\vulkan\vulkan-1.lib ..\src\third_party\duckdb\duckdb.lib /out:bench_select_scale.exe || (popd & exit /b 1)

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
