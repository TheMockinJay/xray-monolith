# X-Ray Monolith — Core Architecture, Memory, Timer & Zero-Copy Texture Overhaul

**Lead Architect & Integrator:** Shahryar (`TheMockinJay` / `iFixit`)  
**Base Branch:** `MarkAlexey/xray-monolith-maka-pbr-test` (`pbr-test-s3ds-mt-sss-lass-perf-dlss`)  
**License:** MIT

---

## Overview of the 6 Core Engineering Modules Added in This Branch

This branch integrates our `xrCore`, `xrEngine`, and `xrRenderDX10/R4` systems overhaul directly onto Maka's PBR + SSLR + VSLR + LASS + DLSS 0.6.3 + Multithreaded Shadows branch.

### 1. oneTBB Task Arena Isolation (`src/xrCore/_thread_types.h`, `src/xrCore/xrCore.cpp`)
* Wraps `~xr_task_group()`, `wait()`, `xr_parallel_for` (both blocked-range and integral overloads), `xr_parallel_foreach`, and `xr_parallel_sort` in `tbb::this_task_arena::isolate`.
* **Why:** Prevents TBB worker threads that hold non-recursive Win32 `SRWLOCK` (`xrSRWLock`) guards from stealing outer tasks during nested `wait()` or `parallel_for` calls and deadlocking on the same lock.

### 2. Clean `CLocatorAPI` Synchronization (`src/xrCore/LocatorAPI.h/.cpp`, `LocatorAPI_auth.cpp`)
* Replaces the experimental 500-line `scan_dedupe` / `scan_prefetch` / `thread_local` debug wrapper with clean `xrSRWLock` reader-writer synchronization around `m_files` and a safe iterator erase loop in `unload_archive()`.

### 3. Direct `tbbmalloc`, Zero-Slack Invariant, `1 ns` Atomic Usage Counter & Release `mem_compact()` (`src/xrCore/xrMemory_subst_msvc.cpp`, `xrMemory.cpp`, `memory_usage.cpp`)
* **Direct `tbbmalloc` Wiring:** Calls `scalable_aligned_malloc`, `scalable_aligned_realloc`, and `scalable_aligned_free` directly and zeroes all usable slack bytes (`scalable_msize`) on allocation, growth, and shrink.
* **`1 ns` `xrMemory::mem_usage()`:** Replaces the `2.76-second` `_heapwalk()` freeze with an atomic counter (`g_xr_allocated_bytes`).
* **Release-Mode `xrMemory::mem_compact()`:** Previously compiled out (`#ifdef DEBUG_MEMORY_MANAGER`) in Release builds. Now executes `g_pSharedMemoryContainer->clean()`, `scalable_allocation_command(TBBMALLOC_CLEAN_ALL_BUFFERS, nullptr)`, `HeapCompact(GetProcessHeap(), 0)`, and `_heapmin()`.
* **Itemized Virtual Memory & Heap Ledger (`log_vminfo()`):** Breaks down `MEM_COMMIT` into `MEM_PRIVATE` (by page protection: `RW`, `GPU_WriteCombine`, `JIT_Exec`, `Guard/NoAcc` and by block size: `<1MB`, `1-16MB`, `16-64MB`, `>=64MB`), `MEM_MAPPED`, `MEM_IMAGE`, `tbbmalloc_live`, and `Win32 Heaps` (`GetProcessHeaps` + `HeapSummary`).

### 4. `5.16 ns` 0-HAL Invariant RDTSC Timer & CPUID Fixes (`src/xrCore/cpuid.h/.cpp`, `_math.h/.cpp`, `FTimer.h`)
* Fixes the inverted `strncmp(pinfo->v_name, "AuthenticAMD", 12) == 0` vendor check and UB casts in `cpuid.cpp`, and detects `AVX`, `AVX2`, `Invariant TSC` (Leaf `0x80000007`), `RDTSCP`, and `WAITPKG`.
* Calibrates nominal TSC frequency using Intel CPUID Leaf `0x15` crystal-clock ratios when available and computes a 64-bit fixed-point multiplier (`qpc_mul`) so `CPU::QPC()` runs in `~5.16 ns/call` via `_umul128` in user mode with zero kernel traps and zero `_mm_lfence()` stalls.
* Removes x87 `_control87()` FPU pipeline flushes on x64 (`_M_AMD64`).

### 5. Progressive SMT `xrSpinWait` (`src/xrCore/xrSyncronize.h`, `SH_Texture.h/.cpp`, `R_Backend.h`, `r__occlusion.cpp`, `device.cpp`, `x_ray.cpp`)
* Upgrades `xrSpinWait` to progressive `_mm_pause()` relaxation (`1x` for spins 0–15, `4x` for spins 16–63, then `_mm_pause()` + `std::this_thread::yield()`) across texture-loading waits and occlusion queries, and disables the legacy `slowdownthread`.

### 6. Zero-Copy Native DDS Loader, Active-Only Skyboxes, Multi-Map Unload Purge & 64-Bit DXGI VRAM Ledger (`src/Layers/xrRenderDX10/dx10Texture.cpp`, `dx10SH_Texture.cpp`, `dx10ResourceManager_Resources.cpp`, `ResourceManager.cpp`, `r4_loader.cpp`)
* **Zero-Copy Native DDS Loader (`NativeCreateTextureFromDDS`):** Parses DDS and DX10 headers directly on the stack from `CVirtualFileReader` memory-mapped views (`BC1`–`BC7`, `RGBA`/`BGRA`, `float`, Cubemaps, 3D volumes, 4x4 block padding for non-multiple-of-4 DXT textures, and inline conversion for legacy `L8`/`A8L8`/`R8G8B8`), achieving **100% native loading (`Fallback[0]`)** across GAMMA without touching `d3dx11_43.dll`'s temporary heap buffers.
* **Eliminated `5.5+ GB` of Dormant Texture & Multi-Map Accumulation Bloat:**
  * Disabled `StoreNecessaryTextures()` (which previously pinned `3.44 GB` of startup textures with an artificial `+1` reference count forever).
  * Skipped preloading `3.44 GB` of world/particle textures at the Main Menu (`g_pGameLevel == nullptr`).
  * Skipped preloading `~2.48 GB` of inactive hourly/weather skyboxes in `DeferredUpload()`, loading only the active weather's skyboxes and `#small` irradiance cubemaps on demand inside `CTexture::surface_get()`.
  * Enabled automatic model and texture purging in `CRender::level_Unload()` (resetting `dwLastUsedFrame = 0` and calling `IDXGIDevice3::Trim()` + `Memory.mem_compact()`) so transitioning across multiple maps does not accumulate previous levels' textures or models.
* **64-Bit Texture Byte Counters & DXGI 1.4 VRAM Ledger:**
  * Upgraded `ResourcesGetMemoryUsage` and `_GetMemoryUsage` from 32-bit `u32` (which overflowed every `4.00 GB`) to 64-bit `u64` across 8 engine files, and added `IDXGIAdapter3::QueryVideoMemoryInfo` (`Dedicated_VRAM_Used` vs `Shared_System_GPU_Used`) + a 6-category texture memory breakdown (`World/NPC/Wpn`, `Trees/Terrain`, `Particles/FX`, `Sky`, `UI`, `Lightmaps`).

---

## Building the 3 Executables (`AnomalyDX11.exe`, `AnomalyDX11AVX.exe`, `AnomalyDX12.exe`)

Open Developer PowerShell for Visual Studio 2022 in the repository root and run:

1. **Non-AVX DirectX 11 (`AnomalyDX11.exe`):**
   `msbuild src\engine-vs2022.sln /t:xrEngine /p:Configuration="DX11" /p:Platform=x64 /m /nr:false`
2. **AVX DirectX 11 (`AnomalyDX11AVX.exe`):**
   `msbuild src\engine-vs2022.sln /t:xrEngine /p:Configuration="DX11-AVX" /p:Platform=x64 /m /nr:false`
3. **DirectX 12 Present Bridge (`AnomalyDX12.exe`):**
   `msbuild src\engine-vs2022.sln /t:xrEngine /p:Configuration="DX12" /p:Platform=x64 /m /nr:false`

Compiled binaries are output to `_build\_game\bin_dbg\`, and require `tbb12.dll` and `tbbmalloc.dll` from `sdk\binaries\`.