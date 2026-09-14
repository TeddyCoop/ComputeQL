# Multi-backend GPU plan (Vulkan / HIP / CUDA / DX12 / Metal)

## Current state

`src/gpu/gpu.h` declares one flat, free-function API (`gpu_init`, `gpu_buffer_alloc`,
`gpu_kernel_execute`, batching, etc). `src/gpu/gpu_inc.h`/`gpu_inc.c` just
`#include`s the Vulkan implementation directly — there is no indirection at all.
The functions in `gpu_vulkan.c` *are* `gpu_init`/`gpu_buffer_alloc`/etc, compiled
straight into `main.c`'s unity build. `GPU_Buffer`/`GPU_Kernel`/`GPU_Batch`/`GPU_State`
are defined by the backend header, and `query_exec.c` reaches into the Vulkan
global directly in 7 places (`g_vulkan_state->arena`) to build pooled-buffer
name keys — a real leak of backend internals into caller code.

There is a `src/gpu/cuda/` directory but it's empty (placeholder only).

The GPU-side work is a small, fixed set of 7 compute kernels
(`src/gpu/vulkan/shaders/*.comp`): `scan_filter`, `bitonic_sort`,
`aggregate_assign`, `aggregate_reduce`, `csr_scatter`,
`hash_join_build_count`, `hash_join_probe`. `scan_filter` is a bytecode
*interpreter* (compiled once, fed a per-query program buffer), not
per-query-codegen'd — so porting a new backend means porting these 7 kernels,
not an open-ended number of shaders.

Build is a single `build.bat` that does an MSVC/clang unity build of
`main.c`, plus a loop that runs `glslc` over `*.comp` → `build/shaders/*.spv`
(precompiled, never built at runtime).

## Recommendation

Do this in two phases, because they solve different problems and the second
is a strict superset of effort:

1. **Vtable phase** — turn the free-function API into a `GPU_Backend` struct
   of function pointers, resolved once at `gpu_init()` and dispatched through
   for every call. Each backend becomes an optional compile unit gated by a
   `#define GPU_BACKEND_VULKAN` / `_HIP` / `_CUDA` / `_DX12` / `_METAL`, so a
   dev who only sets `GPU_BACKEND_VULKAN=1` never touches ROCm/CUDA/DX/Metal
   headers or libs. Runtime selection becomes "which of the *compiled-in*
   backends do I activate" via a cmdline flag/env var. This alone gets you
   "switch at runtime" for anyone who built multiple backends in, and costs
   comparatively little — it's mostly a mechanical refactor of code that
   already behaves like a singleton backend.

2. **DLL phase** — once the vtable boundary exists and is proven (kernel set
   is opaque, `GPU_Buffer*`/`GPU_Kernel*` are truly opaque pointers, no
   backend internals leak to callers), give each backend its own build
   producing `gpu_vulkan.dll` / `gpu_hip.dll` / `gpu_cuda.dll` / etc, each
   exporting one C entry point that returns a `GPU_Backend*`. The main exe
   `LoadLibrary`s only the backend(s) actually present/selected at runtime,
   with zero link-time dependency on SDKs it doesn't use. This is what
   actually satisfies "a developer should not have to build for all of those
   backends" at the *binary* level (not just source level) and is what
   lets you ship one `gdb.exe` that works on an NVIDIA box, an AMD box, or a
   Mac, loading only the one backend DLL that matches the machine.

Phase 1 is not wasted work if you stop there — it's a legitimate, simpler
end state (multi-backend, single static binary, runtime-selectable among
whatever was compiled in). Phase 2 is additive on top of it. I'd build phase
1 for Vulkan + HIP first (proves the abstraction with two real backends,
including two different kernel languages), then decide whether phase 2 is
worth it based on how painful the DLL boundary turns out to be.

## Phase 1 — backend vtable

### New files

- `src/gpu/gpu.h` — stays the public, backend-neutral header: opaque
  `GPU_Buffer`/`GPU_Kernel`/`GPU_Batch` (just forward-declared structs, never
  defined here), `GPU_BufferFlags`, and the free-function API signatures
  unchanged (callers — `query_exec.c` etc — do not change at all).
- `src/gpu/gpu_backend.h` — new. Defines `GPU_Backend`, a struct of function
  pointers mirroring every function currently in `gpu.h` 1:1 (see below), plus
  a `GPU_BackendKind` enum (`GPU_BackendKind_Vulkan`, `_HIP`, `_CUDA`, `_DX12`,
  `_Metal`) and a small registration API:
  `internal void gpu_backend_register(GPU_BackendKind kind, GPU_Backend* backend, String8 name);`
- `src/gpu/gpu_dispatch.c` — new. Implements the free functions from `gpu.h`
  as one-line dispatches through `g_active_backend->fn(...)`, implements
  `gpu_backend_register`, and implements backend *selection* (see below).
  This is the only file that knows about the vtable; everything else still
  calls plain `gpu_buffer_alloc(...)` etc.

### `GPU_Backend` shape

```c
typedef struct GPU_Backend GPU_Backend;
struct GPU_Backend
{
  // lifecycle
  void   (*init)(void);
  void   (*release)(void);
  void   (*wait)(void);
  U64    (*get_executed_kernel_time_microseconds)(void);
  U64    (*device_total_memory)(void);
  U64    (*device_free_memory)(void);
  U64    (*device_max_storage_buffer_range)(void);
  B32    (*device_lost)(void);

  // buffers
  GPU_Buffer* (*buffer_alloc)(U64 size, GPU_BufferFlags flags, void* data);
  GPU_Buffer* (*buffer_alloc_pooled)(String8 name, U64 size, GPU_BufferFlags flags, void* data);
  GPU_Buffer* (*buffer_import_host_readonly)(void* host_ptr, U64 size);
  GPU_Buffer* (*buffer_import_host_readonly_pooled)(String8 name, void* host_ptr, U64 size);
  void        (*buffer_release)(GPU_Buffer* buffer);
  void        (*buffer_write)(GPU_Buffer* buffer, void* data, U64 size);
  void        (*buffer_read)(GPU_Buffer* buffer, void* data, U64 size);

  // kernels
  GPU_Kernel* (*kernel_alloc)(String8 name);
  void        (*kernel_release)(GPU_Kernel* kernel);
  void        (*kernel_execute)(GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
  void        (*kernel_set_arg_buffer)(GPU_Kernel* kernel, U32 index, GPU_Buffer* buffer);
  void        (*kernel_set_arg_u64)(GPU_Kernel* kernel, U32 index, U64 value);

  // batching
  GPU_Batch* (*batch_begin)(U64 upload_bytes_needed, U64 download_bytes_needed);
  void       (*batch_buffer_write)(GPU_Batch* batch, GPU_Buffer* buffer, void* data, U64 size);
  void       (*batch_buffer_zero)(GPU_Batch* batch, GPU_Buffer* buffer, U64 size);
  void       (*batch_buffer_fill)(GPU_Batch* batch, GPU_Buffer* buffer, U64 size, U32 value);
  void       (*batch_kernel_execute)(GPU_Batch* batch, GPU_Kernel* kernel, U32 global_work_size, U32 local_work_size);
  void       (*batch_buffer_read)(GPU_Batch* batch, GPU_Buffer* buffer, void* out_data, U64 size);
  B32        (*batch_end)(GPU_Batch* batch);

  // introspection (new — needed to fix the g_vulkan_state->arena leak, see below)
  Arena*     (*scratch_arena)(void);
  String8    (*name)(void);
};
```

Put a `U32 struct_size;` first member if you ever expect to add fields later
and want older/newer backend binaries to interoperate gracefully (matters
more once phase 2 exists — a DLL built against an older header shouldn't
corrupt memory when the host adds a field). For phase 1 (static link, always
rebuilt together) this is optional but cheap to add now.

### Fixing the `g_vulkan_state->arena` leak

`query_exec.c` uses the Vulkan-global arena purely to scratch-allocate
`push_str8f` name keys for pooled buffers (e.g. `agg_group_col_data:%u`).
Add `scratch_arena()` to the vtable (backed by whatever long-lived arena each
backend keeps — Vulkan already has `g_vulkan_state->arena`), and replace all
7 call sites' `g_vulkan_state->arena` with `gpu_scratch_arena()`. This is the
one piece of real-code cleanup phase 1 requires beyond the mechanical
refactor, and it's also just a correctness improvement independent of the
rest of the plan — right now `query_exec.c` cannot compile against any
backend other than Vulkan.

### Compile-time backend selection

In `gpu_inc.h`:

```c
#if !defined(GPU_BACKEND_VULKAN) && !defined(GPU_BACKEND_HIP) && \
    !defined(GPU_BACKEND_CUDA)  && !defined(GPU_BACKEND_DX12) && \
    !defined(GPU_BACKEND_METAL)
#define GPU_BACKEND_VULKAN 1   // default: today's behavior, zero config needed
#endif

#include "gpu.h"
#include "gpu_backend.h"
#if GPU_BACKEND_VULKAN
#include "vulkan/gpu_vulkan.h"
#endif
#if GPU_BACKEND_HIP
#include "hip/gpu_hip.h"
#endif
// ... etc, one guarded include per backend
```

and mirrored in `gpu_inc.c` for the `.c` includes. `build.bat` gains one
`set GPU_BACKEND_VULKAN=1` / `set GPU_BACKEND_HIP=1` style flag per backend,
translated to `/DGPU_BACKEND_HIP=1` (or `-D` for clang) on the compile line,
and — critically — the link line (`cl_link`) and the shader-compile loop only
pull in `vulkan-1.lib` / ROCm libs / shader compilers for the backends that
are actually enabled. A dev who never sets `GPU_BACKEND_HIP` never needs
ROCm installed at all.

### Runtime selection among compiled-in backends

Each backend's `.c` calls `gpu_backend_register(...)` for itself from a
static-initializer-style pattern consistent with the rest of this codebase
(simplest: `gpu_inc.c` calls one `gpu_backend_register_all()` that is itself
built from the same `#if GPU_BACKEND_*` guards, calling
`gpu_backend_register(GPU_BackendKind_Vulkan, gpu_vulkan_get_backend(), str8_lit("vulkan"))`
per enabled backend — no ctor tricks needed).

`gpu_init()` (in `gpu_dispatch.c`) then:
1. Reads `--gpu=<name>` off the command line (falls through to an env var
   `GDB_GPU_BACKEND`, then to the first registered backend) to pick one.
2. Sets `g_active_backend = ` the matching registration.
3. Calls `g_active_backend->init()`.
4. Logs the chosen backend name and available alternatives — useful when
   only one is compiled in, and essential once several are.

For actually swapping backends *mid-process* (not just choosing one at
startup), add:

```c
internal B32 gpu_backend_switch(String8 name);
```

which calls the current backend's `release()`, waits, updates
`g_active_backend`, and calls the new backend's `init()`. Every
`GPU_Buffer*`/`GPU_Kernel*` held by callers becomes invalid at that point
(they're backend-owned opaque pointers) — `query_exec.c`'s pooled-buffer
cache would need a `gpu_pool_invalidate_all()` style hook called on switch,
since pooled buffers currently live for the process lifetime. This is a real
design decision to make explicit in code/docs: switching backends is a
"drain and rebuild," not a hot-swap of live GPU state.

## Phase 2 — backends as loadable DLLs

Once phase 1 is solid, split each backend out of the unity build into its
own standalone project:

- `src/gpu/vulkan/` → builds to `gpu_vulkan.dll`
- `src/gpu/hip/` → builds to `gpu_hip.dll`
- `src/gpu/cuda/` → builds to `gpu_cuda.dll`
- `src/gpu/dx12/`, `src/gpu/metal/` similarly (Metal only relevant if/when
  this ever targets macOS — no-op on Windows/Linux builds)

Each exports exactly one C function:

```c
__declspec(dllexport) GPU_Backend* gpu_backend_get(void);
```

`main.c` no longer `#include`s any backend `.c`/`.h` at all when built for
phase 2 — it only depends on `gpu.h` + `gpu_backend.h`. At `gpu_init()` time
it does the equivalent of, for each candidate name in priority order:
`LoadLibraryA("gpu_hip.dll")` → `GetProcAddress(..., "gpu_backend_get")` →
call it → got a `GPU_Backend*` or the DLL wasn't found/failed to load (e.g.
no ROCm runtime present on this machine) → try the next one. This is real
runtime backend switching with **zero build-time dependency** on backends
you don't ship — exactly "developer doesn't have to build for all of those."

Things this phase needs that phase 1 doesn't:

- **ABI discipline at the ownership boundary.** `GPU_Buffer`/`GPU_Kernel`/
  `GPU_Batch` must stay fully opaque to the host (already true after phase
  1's `g_vulkan_state->arena` fix) — the host only ever holds and passes
  back pointers the backend itself allocated and will itself free. Never let
  the host `push_array` a `GPU_Kernel` from its own arena and hand it to the
  backend, or vice versa.
- **No cross-DLL heap/arena sharing.** Each backend DLL should own its own
  arena internally (as Vulkan's `GPU_State.arena` already does) rather than
  receiving one from the host, since mixing allocators across a DLL boundary
  on Windows is a classic source of heap-corruption bugs if the CRTs ever
  diverge (e.g. static vs dynamic CRT, debug vs release mixed builds).
- **`struct_size`/version field on `GPU_Backend`** becomes load-bearing here
  (not just nice-to-have): a `gpu_vulkan.dll` built against an older header
  than the host must not have the host read past the vtable it actually
  filled in. Check `struct_size` after loading and refuse backends whose
  vtable doesn't cover every function the host needs.
- **A tiny loader shim**, probably `src/gpu/gpu_loader.c`, wrapping
  `LoadLibrary`/`dlopen` + `GetProcAddress`/`dlsym` behind one function:
  `internal GPU_Backend* gpu_load_backend_dll(String8 backend_name);` — this
  is the only new OS-specific code phase 2 needs (the project already has an
  `os/` layer with win32 implementations to extend, e.g. alongside
  `os/core/win32`).
- **Per-backend build scripts.** `build.bat` stops knowing about individual
  backends; instead something like `build_gpu_vulkan.bat`,
  `build_gpu_hip.bat` each compile+link just that one DLL (only that
  backend's SDK needs to be installed to run that script), dropping output
  into `build/gpu_backends/`. `build.bat` itself just builds whichever DLLs
  are present/requested, or the main exe can ship with none prebuilt and
  developers build only what they need.

## New-backend kernel porting checklist

Whatever backend gets added, the actual GPU-side work is bounded to these 7
kernels (today GLSL compute shaders under `src/gpu/vulkan/shaders/`):

| Kernel | Purpose |
|---|---|
| `scan_filter` | bytecode-interpreter row filter (generic VM, not per-query codegen) |
| `bitonic_sort` | GPU sort |
| `aggregate_assign` | group-by slot assignment |
| `aggregate_reduce` | group-by aggregate reduction ([aggregate_reduce.comp](src/gpu/vulkan/shaders/aggregate_reduce.comp) — currently mid-edit on this branch) |
| `csr_scatter` | CSR-style scatter, shared by group-by and hash join |
| `hash_join_build_count` | hash join build-side bucket counting |
| `hash_join_probe` | hash join probe |

For HIP specifically: port each `.comp` to HIP C++ (`.hip` sources under
`src/gpu/hip/shaders/`), compile with `hipcc --genco` (or offline
`hipcc -c --cuda-device-only` depending on ROCm version) to a loadable code
object analogous to today's precompiled `.spv`, and map the host-side API:

| Vulkan concept | HIP equivalent |
|---|---|
| `vkAllocateMemory`/`vkCreateBuffer` | `hipMalloc` / `hipHostMalloc` (pinned) |
| `vkCmdCopyBuffer`, staging buffers | `hipMemcpy`/`hipMemcpyAsync` |
| `VkShaderModule` + SPIR-V | `hipModuleLoad` on the compiled code object |
| pipeline dispatch | `hipModuleGetFunction` + `hipModuleLaunchKernel` |
| `VkQueryPool` timestamps | `hipEventRecord`/`hipEventElapsedTime` |
| `vkGetPhysicalDeviceMemoryProperties` | `hipMemGetInfo`, `hipGetDeviceProperties` |
| `VK_EXT_external_memory_host` (zero-copy host import) | `hipHostRegister` |
| ReBAR (device-local + host-visible memory type) | HIP's unified/managed memory or `hipHostMalloc` with device mapping, needs verifying on target hardware |

CUDA follows the same shape with `cuModuleLoad`/`cuLaunchKernel`/`nvcc --ptx`
instead. DX12 compute would use DXC-compiled DXIL compute shaders (HLSL
source, closer to today's GLSL than HIP/CUDA are) and
`ID3D12GraphicsCommandList::Dispatch`. Metal uses `.metal` source compiled to
a `.metallib`.

## Suggested sequencing

1. Land the vtable refactor (phase 1) for Vulkan only — pure mechanical
   change, should be behavior-neutral. Fix the `g_vulkan_state->arena` leak
   as part of this, since it's required for any second backend to compile
   against `query_exec.c` at all.
2. Add HIP as the second backend under phase 1 (proves the abstraction with
   a real second implementation and a real second kernel language — CUDA
   would prove less since it's structurally almost identical to HIP).
3. Decide, with two real backends in hand, whether phase 2 (DLL split) is
   worth the added build/ABI complexity for your actual deployment story —
   e.g. if this only ever ships as "one exe built for the machine it runs
   on," phase 1 alone may be enough forever and phase 2 is only worth doing
   if you want one exe that auto-detects vendor at runtime.
4. Add DX12/CUDA/Metal incrementally, each bounded by the same 7-kernel
   surface.

## Open questions to resolve before starting

- Target OSes: is HIP/ROCm expected on Windows (HIP SDK for Windows exists
  but is behind CUDA/Linux ROCm in maturity) or Linux-only for the AMD path?
  Affects whether `gpu_hip.c` needs a win32-specific code path at all.
- Is mid-process backend switching (`gpu_backend_switch`) an actual product
  requirement, or is "pick a backend at process start via flag/env/config"
  sufficient? The pooled-buffer invalidation story is meaningfully simpler
  if switching only ever happens before any query has run.
- For phase 2: are backend DLLs expected to be redistributed/discovered at
  a fixed relative path (`./gpu_backends/*.dll` next to the exe), or found
  via an install-time registry/env var? Affects the loader shim's search
  logic.
