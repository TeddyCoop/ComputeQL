@echo off
setlocal

:: tec: builds and runs the host to VRAM transfer benchmark (src\tests\scale\bench_gpu_transfer.c). extra args pass through, e.g. run_gpu_transfer.bat --only=pipelines --mb=4096
if not exist build mkdir build
if not exist build\shaders mkdir build\shaders

"%VULKAN_SDK%\Bin\glslc.exe" src\tests\scale\transfer_read.comp -o build\shaders\transfer_read.spv || exit /b 1

:: tec: cl by default, set CQL_CLANG=1 to use clang
call .\bench_cc.bat

pushd build
%cc_compile% ..\src\tests\scale\bench_gpu_transfer.c %cc_link_vk% %cc_exe_out%bench_gpu_transfer.exe || (popd & exit /b 1)
call .\bench_gpu_transfer.exe %*
set run_status=%errorlevel%
popd

endlocal & exit /b %run_status%
