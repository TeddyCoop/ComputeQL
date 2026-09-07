@echo off
pushd build
call gdb.exe --gpu="vulkan" --query=""
popd