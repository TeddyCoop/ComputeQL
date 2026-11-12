@echo off
setlocal

:: tec: sqlite amalgamation must be downloaded manually
if not exist src\third_party\sqlite\sqlite3.c goto :missing_sqlite
if not exist src\third_party\sqlite\sqlite3.h goto :missing_sqlite
goto :have_sqlite

:missing_sqlite
echo ERROR: src\third_party\sqlite\sqlite3.c / sqlite3.h not found.
echo Download the SQLite amalgamation from https://sqlite.org/download.html
echo and place sqlite3.c + sqlite3.h under src\third_party\sqlite\
exit /b 1

:have_sqlite

:: tec: duckdb C library must be downloaded manually
if not exist src\third_party\duckdb\duckdb.h goto :missing_duckdb
if not exist src\third_party\duckdb\duckdb.lib goto :missing_duckdb
if not exist src\third_party\duckdb\duckdb.dll goto :missing_duckdb
goto :have_duckdb

:missing_duckdb
echo ERROR: src\third_party\duckdb\duckdb.h / duckdb.lib / duckdb.dll not found.
echo Download libduckdb-windows-amd64.zip from https://github.com/duckdb/duckdb/releases
echo and place duckdb.h + duckdb.lib + duckdb.dll under src\third_party\duckdb\
exit /b 1

:have_duckdb

:: tec: build the main engine first
:: tec: cl by default, set CQL_CLANG=1 to build everything with clang instead
call .\bench_cc.bat
call .\build.bat %cc_build_args% || exit /b 1

if not exist build\%cc_sqlite_obj% (
  echo.
  echo === compiling sqlite3.c (one-time, cached as build\%cc_sqlite_obj%) ===
  pushd build
  %cc_compile% %cc_c_only% ..\src\third_party\sqlite\sqlite3.c %cc_obj_out%%cc_sqlite_obj% || exit /b 1
  popd
)

copy /y src\third_party\duckdb\duckdb.dll build\duckdb.dll >nul

:: tec: bench_query_chunked_hash causes a GPU crash that i havent fixed yet
set skip_tests=
::bench_query_chunked_hash

echo.
echo === compiling tests ===
for %%f in (src\tests\bench_*.c src\tests\net_*.c) do (
  echo %skip_tests% | findstr /i /c:"%%~nf" >nul
  if errorlevel 1 (
    echo compiling %%f
    pushd build
    %cc_compile% ..\%%f %cc_sqlite_obj% %cc_link_vk_duck% %cc_exe_out%%%~nf.exe || exit /b 1
    popd
  ) else (
    echo skipping %%f
  )
)

echo.
echo === running tests ===
for %%f in (src\tests\bench_*.c src\tests\net_*.c) do (
  echo %skip_tests% | findstr /i /c:"%%~nf" >nul
  if errorlevel 1 (
    echo.
    echo ===== running %%~nf =====
    pushd build
    call .\%%~nf.exe
    popd
  )
)

endlocal
