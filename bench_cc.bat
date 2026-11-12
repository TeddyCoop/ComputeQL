@echo off

:: tec: picks the compiler for the bench scripts, they call it from the repo root and use the variables from inside build\
:: cl is the default, set CQL_CLANG=1 before running a script to use clang with lld instead
::
::   cc_compile        compiler plus common flags
::   cc_c_only         flags to compile to an object only (sqlite3.c), clang also drops warnings for it
::   cc_obj_out        object output option, directly followed by the file name
::   cc_link_vk        link options for tests that only need vulkan
::   cc_link_vk_duck   link options for tests that also use duckdb
::   cc_exe_out        exe output option, directly followed by the file name, last on the line
::   cc_sqlite_obj     cached sqlite object, kept per compiler
::   cc_build_args     arguments for build.bat

if "%CQL_CLANG%"=="1" goto :use_clang

set cc_compile=call cl /O2 /DNDEBUG /I..\src\ /I..\src\third_party\vulkan\ /nologo /FC /Z7
set cc_c_only=/c
set cc_obj_out=/Fo
set cc_link_vk=/link ..\src\third_party\vulkan\vulkan-1.lib
set cc_link_vk_duck=/link ..\src\third_party\vulkan\vulkan-1.lib ..\src\third_party\duckdb\duckdb.lib
set "cc_exe_out=/out:"
set cc_sqlite_obj=sqlite3.obj
set cc_build_args=
exit /b 0

:use_clang
set cc_compile=call clang -g -O3 -DNDEBUG -I..\src\ -I..\src\third_party\vulkan\ -mpopcnt -mavx2 -Wno-microsoft-enum-forward-reference -gcodeview -fdiagnostics-absolute-paths -Wall -Wno-unknown-warning-option -Wno-initializer-overrides -Wno-missing-braces -Wno-unused-function -Wno-writable-strings -Wno-unused-value -Wno-unused-variable -Wno-unused-local-typedef -Wno-deprecated-register -Wno-deprecated-declarations -Wno-unused-but-set-variable -Wno-single-bit-bitfield-constant-conversion -Xclang -flto-visibility-public-std -D_USE_MATH_DEFINES -Dstrdup=_strdup -Dgnu_printf=printf
set "cc_c_only=-c -w"
set "cc_obj_out=-o "
set cc_link_vk=-fuse-ld=lld ..\src\third_party\vulkan\vulkan-1.lib
set cc_link_vk_duck=-fuse-ld=lld ..\src\third_party\vulkan\vulkan-1.lib ..\src\third_party\duckdb\duckdb.lib
set "cc_exe_out=-o "
set cc_sqlite_obj=sqlite3_clang.obj
set cc_build_args=clang
exit /b 0
