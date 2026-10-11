@echo off
setlocal

:: tec: builds and runs the host to VRAM transfer benchmark (src\tests\scale\bench_gpu_transfer.c). extra args pass through, e.g. run_gpu_transfer.bat --only=pipelines --mb=4096
if not exist build mkdir build
if not exist build\shaders mkdir build\shaders

"%VULKAN_SDK%\Bin\glslc.exe" src\tests\scale\transfer_read.comp -o build\shaders\transfer_read.spv || exit /b 1

pushd build
call cl /O2 /DNDEBUG /I..\src\ /I..\src\third_party\vulkan\ /nologo /FC /Z7 ..\src\tests\scale\bench_gpu_transfer.c /link ..\src\third_party\vulkan\vulkan-1.lib /out:bench_gpu_transfer.exe || (popd & exit /b 1)
call .\bench_gpu_transfer.exe %*
set run_status=%errorlevel%
popd

endlocal & exit /b %run_status%
