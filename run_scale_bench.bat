@echo off
setlocal

:: tec: builds and runs the large scale suite (src\tests\scale). sqlite is left out because its joins take minutes per query at this size. extra args pass through, e.g. run_scale_bench.bat --rows=1000000 --no_duckdb
:: tec: cl by default, set CQL_CLANG=1 to use clang
call .\bench_cc.bat
if not exist build\%cc_sqlite_obj% (
  echo ERROR: build\%cc_sqlite_obj% missing, run build_and_run_tests.bat once first.
  exit /b 1
)

copy /y src\third_party\duckdb\duckdb.dll build\duckdb.dll >nul

pushd build
%cc_compile% ..\src\tests\scale\bench_query_xlarge.c %cc_sqlite_obj% %cc_link_vk_duck% %cc_exe_out%bench_query_xlarge.exe || (popd & exit /b 1)
call .\bench_query_xlarge.exe --no_sqlite %*
popd

endlocal
