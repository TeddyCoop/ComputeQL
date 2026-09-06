/* date = January 23rd 2025 10:14 pm */

#ifndef GPU_INC_H
#define GPU_INC_H

#if !defined(GPU_BACKEND_VULKAN) && !defined(GPU_BACKEND_HIP) && \
    !defined(GPU_BACKEND_CUDA)  && !defined(GPU_BACKEND_DX12) && \
    !defined(GPU_BACKEND_METAL)
#define GPU_BACKEND_VULKAN 1
#endif

#include "gpu.h"
#include "gpu_backend.h"

#if GPU_BACKEND_VULKAN
#include "vulkan/gpu_vulkan.h"
#endif

#endif //GPU_INC_H
