@echo off
pushd build
call cql.exe --gpu="vulkan" --query=""
popd