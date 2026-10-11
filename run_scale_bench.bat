@echo off
setlocal

:: tec: builds and runs the large scale suite (src\tests\scale). sqlite is left out because its joins take minutes per query at this size. extra args pass through, e.g. run_scale_bench.bat --rows=1000000 --no_duckdb
if not exist build\sqlite3.obj (
  echo ERROR: build\sqlite3.obj missing, run build_and_run_tests.bat once first.
  exit /b 1
)

copy /y src\third_party\duckdb\duckdb.dll build\duckdb.dll >nul

pushd build
call cl /O2 /DNDEBUG /I..\src\ /I..\src\third_party\vulkan\ /nologo /FC /Z7 ..\src\tests\scale\bench_query_xlarge.c sqlite3.obj /link ..\src\third_party\vulkan\vulkan-1.lib ..\src\third_party\duckdb\duckdb.lib /out:bench_query_xlarge.exe || (popd & exit /b 1)
call .\bench_query_xlarge.exe --no_sqlite %*
popd

endlocal
