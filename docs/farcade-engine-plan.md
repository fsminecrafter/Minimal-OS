# Farcade Engine (working name): a high-performance game engine for Minimal-OS

## Context

You want a real game engine for Minimal-OS, in its own repository (to be created later):
- It compiles and exports games to Minimal-OS and includes a Minimal-OS port of the SDK.
- Its editor is easy to use but capable.
- Gameplay code can be Lua and C++, mixed in one project.
- It has modern graphics: CLOD virtual geometry with voxels as the far representation (no impostors), a screen-space + voxel hybrid GI (no ray tracing), and far-away proxies that carry no instance data.
- It is fast.

Your decisions:
1. **The editor and toolchain run on Minimal-OS only.** Game makers never need a PC. The engine developer (you) still builds the engine and editor on the PC (WSL/Docker) and tests them in QEMU.
2. **All rendering goes through FarcadeGL.** It already falls back between the kernel's CPU backend and the NVIDIA Turing backend, so speed work goes into FGPU itself.
3. **Scripting is Lua 5.4.**
4. **Kernel and SDK changes run as a parallel OS track.**

### Where Minimal-OS stands today (the facts that shape this plan)

| Area | Today | What it means for the engine |
| --- | --- | --- |
| 3D | FGPU (`SYS_GPU` 40) runs **in the kernel under one global lock**. The CPU backend is a scalar raster with a parallel barrier on every draw and a bytecode interpreter (8 lanes, SSE). It has no MRT, mips, compute, SSBOs, scissor, instancing or integer formats. | The CPU backend *is* the renderer on QEMU, Intel and AMD, so it needs a JIT, binning and FGPU v3. |
| NVIDIA | Turing GSP-RM and copy engine work on an RTX 2060. The 3D engine and SASS compiler aren't proven on a card yet. | A bonus path, not a dependency. |
| CPU | SSE2 only (no `CR4.OSXSAVE`, so AVX faults). **All of a program's threads are pinned to one core** (`proc_create_thread`, `src/impl/proc/proc.c:700`). | Kernel work is needed for AVX and multicore. SSE4.1 already works on `-cpu host`/`max`. |
| Memory | `malloc` is a syscall per call into the kernel heap below 4 GiB (identity-mapped, no NX). There is no mmap. ELF ≤16 MiB, image ≤64 MiB, a 64 KiB stack with no guard page, and a 64 MiB page pool that panics when empty. | The engine needs its own allocator now; GCC on the OS needs virtual-memory work. |
| Runtime | No libc, libm, STL or `operator new`. Static PIE with RELATIVE relocations only (so no ifunc). | Port mlibc, add a GCC `x86_64-minimalos` target, and dispatch SIMD by hand. |
| On-OS tools | Nothing compiles or interprets on the OS except the FGSL compiler and Mxplorer's JS. | The native toolchain is on the critical path. |
| Display/input/audio | Copy-based present (about 4 copies per frame through mosWM), no vsync. USB HID boot keyboard and mouse only, no hubs or gamepads. 8 PCM16 kernel-mixed streams. | Shared surfaces, scaled present and gamepads go on the OS track. |
| Files | MinimaFS: 32-bit offsets, no mtime/rename/chdir, `0:/` paths. | Content-hash builds; pak parts of at most 2 GiB. |
| Licence | OS, SDK and MOSdesk are GPLv3. `farcadegl.h`, `fglsl.h` and `crt0.c` get compiled into every game. | **Decide before release:** relicense the SDK runtime files (MIT, or GPL plus a linking exception). |

---

## Architecture

```
 Game project (Lua 5.4 scripts + optional C++ module + assets)
 ┌────────────────────────────────┐   ┌──────────────────────────────┐
 │ Editor (runs on MOSdesk)       │   │ Player runtime (.run)        │
 │ Dear ImGui on FarcadeGL        │   │ static: libfe.a + game code  │
 └───────────────┬────────────────┘   └───────────────┬──────────────┘
 Engine runtime libfe.a: core · jobs · flecs ECS · assets/paks · gfx (feature levels)
                 · vgeo/voxel/proxy · GI · Jolt · ozz · audio mixer · Lua · RmlUi
 ─────────────────────────────────────────────────────────────────────────────
 Minimal-OS SDK: mlibc + libstdc++ · FarcadeGL v3 + FGSL v2 · msdk (native)
 ─────────────────────────────────────────────────────────────────────────────
 Kernel: FGPU (CPU backend v2: binning + x86 JIT | NVIDIA Turing backend)
         threads/futex/TLS · mmap/NX · XSAVE/AVX · spawn/wait · shared surfaces
```

The work runs in three tracks:
- **OS track:** the Minimal-OS repo.
- **SDK track:** the Minimal-OS-SDK repo, plus the native toolchain.
- **Engine track:** the new repo.

---

## Part A: OS track (Minimal-OS)

**Status (branch `claude/os0-process-basics-bq26wb`):**
- **OS-0 done.** TLS, futex, clocks, stat/rename/chdir, spawn/wait, environment.
- **OS-1 done.** Program memory above 4 GiB with demand-zero pages (`SYS_VM`), NX, guard pages, a streaming ELF loader, 8 MiB stacks, a page pool sized from RAM, and RAM above 4 GiB used for program pages.
- **OS-2 done, AVX-512 untested.** XSAVE-based state saving, AVX/AVX2 usable, `noavx` flag, CPU features in SYSINFO. QEMU's emulation has no AVX-512, so that path has never run.
- **OS-4 in progress.** The tile-binning rasterizer (binning, hierarchical Z, small-triangle path) is in and is the default; `fgpuold` selects the previous one. Measured on one 2.1 GHz core it is well short of the C4.7 targets for small triangles (about 8 M/s against 30 M/s). The shader JIT, kernel worker threads, per-process contexts and async submit are still to do.
- **System-call pointer checks done** (not in the original plan): every user pointer is checked before use; what is left is listed in `docs/syscall-pointers.md`.
- Everything was tested in QEMU without KVM (`make ci-os0`); nothing has run on real hardware yet.
- The SDK half waits in `patches/` until it is pushed to Minimal-OS-SDK.

The items are listed in dependency order. File paths are verified.

| ID | Work | Main files |
| --- | --- | --- |
| **OS-0** process and libc basics | Per-thread FS base (TLS) saved on context switch. `SYS_FUTEX` wait/wake with timeout (keyed by memory owner + virtual address). A nanosecond monotonic clock (TSC calibrated against the PIT; `rdtsc` already works in user mode). `stat` and `rename`, aliases between `/` and `0:/` paths. `SYS_SPAWN` (ELF + argv + envp + cwd + stdio files) and `SYS_WAIT` (exit code). Environment variables. | `src/impl/proc/context_switch.asm` (MSR 0xC0000100), `src/intf/x86_64/proc.h` (append fields only; the asm hard-codes 0x90/0xD8/0xE0), `src/impl/kernel/syscall.c`, `src/intf/x86_64/syscall.h` + `Minimal-OS-SDK/include/syscall.h`, `src/impl/time.c`, `src/impl/kernel/lapic_timer.c`, `src/impl/handlers/prochandler.c`, `minimafshandler.c` |
| **OS-1** real user virtual memory | Anonymous `mmap`/`munmap`/`mprotect` with reserve/commit. **NX on by default, `PROT_EXEC` added in the same change** (the module loader depends on today's accidental RWX heap). Guard pages. ELF up to 256 MiB loaded by streaming PT_LOAD segments (cc1plus is 30–40 MB). Configurable main stack (8 MiB+). A frame allocator replaces the 64 MiB pool and returns errors instead of panicking. Later: RAM above 4 GiB for user pages. | `src/impl/proc/proc.c:80-206`, `src/impl/pmm.c:19`, `src/impl/kernel/memlayout.c`, `src/intf/mmu_tables.h`, `src/impl/kernel/allocator.c`, `src/impl/kernel/syscall.c:769-829` (SYS_HEAP), `src/impl/loader/elfloader.c`, `src/impl/commands/runcommand.c` |
| **OS-2** AVX/AVX2 | `CR4.OSXSAVE` + `XSETBV` (x87, SSE, AVX; AVX-512 when present). Replace `fxsave64` with `xsaveopt` using the CPUID.0xD size and 64-byte alignment. Report CPU features in SYSINFO. Add a `noavx` boot flag and a YMM torture test. | `src/impl/boot/main.asm:156`, `ap_trampoline.asm:41`, `src/impl/syscall_isr.asm:29`, `idt_isr.asm:72`, `idt_.asm:46`, `src/impl/kernel/smp_par.c` |
| **OS-3** multicore user threads | Threads of one process run on any CPU: affinity + priority syscalls, join through futex, TLB shootdown IPIs (today `invlpg` is local only), thread entry points allowed outside the image. Opt-in at first, with stress tests at `-smp 8`. | `src/impl/proc/proc.c:700-748`, `scheduler.c` (~294 skips thread groups), `thread.c`, `src/impl/kernel/lapic.c`, `spinlock.h` |
| **OS-4** FGPU CPU backend v2 (the renderer's speed) | (1) **Binning:** one parallel phase per render-target pass instead of a barrier per draw. Geometry (vertex shading, clipping, culling, 16.8 fixed-point setup) bins into 64×64 tiles; tiles are rasterized by work stealing, with tile-local color+depth (32 KB) and hierarchical Z (min/max per 8×8). (2) A small-triangle path (bounding box ≤2×2 px, one triangle per SIMD lane). (3) **A JIT** from the branch-free SSA IR in `fgpu_ir.c` to x86 code with fused vertex/fragment kernels: **SSE4.1 first** (doesn't wait on OS-2), AVX2 later. The interpreter stays as reference, with differential fuzzing. (4) Preemptible kernel worker threads instead of interrupt-driven `smp_par`. Per-process contexts and handle quotas instead of `g_busy` and the shared 64/128/64 object limits. Asynchronous submit with fences (`SYS_GPU_WAIT`). | `src/modules/GPU/fgpu.c:29-31,167-183,723,771`, `fgpu_raster.c:633-645`, `fgpu_shader.c`, `fgpu_ir.c`, new `fgpu_jit_x64.c`, `src/impl/kernel/smp_par.c`, `tests/host/fgpu_*_tests.c` |
| **OS-5** present and windows | Scaled present (2×/3× with non-temporal stores into the write-combining framebuffer). Window frames shared between the process and mosWM (no copies), damage rectangles, frame pacing (timer-based where there's no vblank). | `src/impl/handlers/graphics.c`, `gfx_blit.c:148`, `src/impl/kernel/window_manager.c`, `mmio.c:222`, `MOSdesk/mosWM/main.c` |
| **OS-6** FGPU v3 graphics + FGSL v2 (FL1) | Scissor (also needed early, for FL0), MRT ×4, formats (R32UI, RG32F, RGBA16F, R11G11B10F, RGBA32F), mips + trilinear, depth textures + compare samplers, `gl_FragDepth`, flat/int varyings, `gl_VertexID/InstanceID/PrimitiveID`, `texelFetch`, 3D textures + 2D arrays, instancing, base vertex, (multi-)indirect draw, 16 texture units and 16 varyings, larger uniform blocks, buffers of 256 MiB+, configurable depth/blend/write masks, depth bias, timestamp queries, and a v3 limits block in `syscall_gpu_info_t`. FGSL v2: real ints/uints/bit ops, structs, mat3, functions, bounded `while`/`do`, `#include`. | `src/intf/x86_64/fgpu_abi.h` + `Minimal-OS-SDK/include/fgpu_abi.h` (keep identical), `fgpu.c`, `fgpu_raster.c`, `fgpu_ir.c`, `farcadegl.h`, `fglsl.h` |
| **OS-7** FGPU v3 compute + NVIDIA (FL2, stretch) | A compute stage on the CPU backend: workgroups spread across cores, 8 lanes per batch, barriers handled by splitting the branch-free IR, atomics via `lock cmpxchg`. Also SSBOs, image load/store, shared memory, 32/64-bit atomics and indirect dispatch. On NVIDIA: prove 3D on the card, batch one job per frame, compute class, real branches, proper SASS scheduling. | `fgpu_backend_nv.c`, `nv_3d.c`, `nv_sass.c`, `nv_rm_chan.c` |
| **OS-8** input and storage | USB hubs, a HID report-descriptor parser (generic and Xbox/PlayStation USB gamepads), PS/2 mouse, input timestamps. Expose `fat32.c` through the storage syscalls and VFS; USB mass storage later (asset import). | `src/modules/USB/usb/*.c`, `src/intf/usb/usb_stack.h`, `src/impl/handlers/input.c`, `src/impl/storage/fat32.c`, `storage_syscall.c` |
| **OS-9** audio | A query for how much of the stream buffer is queued, smaller buffers for lower latency, float32/48 kHz input. | `src/modules/Audio/*` |
| Dev loop | `make run` uses `qemu64` (no SSE4.1). Add `-cpu host` (KVM) or `-cpu max` (TCG), and a `run-engine` target with `-smp 8 -m 8G`. On Windows, run QEMU with KVM inside WSL2. | `Makefile:332`, `config.mk` |

---

## Part B: SDK track and the native toolchain (the "Minimal-OS port of Minimal-OS-SDK")

1. **S1 mlibc port** (MIT; built for hobby OSes). Its sysdeps sit on OS-0/OS-1: `tcb_set`, `futex_wait/wake`, `anon_allocate/free`, `vm_map`, `clock_get`, open/read/write/seek/close/stat, spawn/wait and exit, with `/` ↔ `0:/` translation. It produces `libc.a`, `libm.a` and pthreads. mlibc beats newlib or the 846-line Doom shim because GCC on the OS needs POSIX spawn, stat and threads.
2. **S2 GCC target `x86_64-minimalos`** (GCC 15.1 + binutils 2.45, the versions `build_gccbinutils.sh` already uses):
   - an entry in `config.gcc`;
   - `gcc/config/minimalos.h`: LINK_SPEC `-static-pie -z max-page-size=0x1000`, a built-in `__minimalos__` define, and gthr-posix through mlibc;
   - libgcc t-files, plus a binutils alias to `elf_x86_64`;
   - libstdc++ with `--disable-shared`;
   - a CMake toolchain file for the engine developer's PC build.

   GCC is the only compiler, on both the PC and the OS.
3. **S3 `msdk` rewritten in C** (keeping today's name and CLI from `MOSdesk/msdk` and `build.sh`). It builds C/C++, incrementally by **content hash** (xxh3 over the `-MD` dependency list plus flags, since MinimaFS has no mtime), and makes MINIRUN1 `.run`, `.slib`, `.mpkg` and Deliver packages (replacing `mkrun.sh`/`runbundle.py`). It runs on the PC and on the OS. Old freestanding programs keep building with `msdk --freestanding`; hosted mode uses mlibc.
4. **S4 Canadian cross:** binutils, GCC, cc1plus and libstdc++ *hosted on* Minimal-OS:
   - Configure flags: `--disable-lto --disable-plugin --without-isl --disable-nls --disable-multilib --enable-languages=c,c++ --enable-threads=posix`, with GMP, MPFR and MPC static. libiberty's `pex` goes through `posix_spawn`, with no `-pipe`.
   - It ships as an `msdk-native` package installed to `0:/sdk/`, built in CI.
   - Needs OS-0 and OS-1 (big ELFs, stacks, spawn/wait), and a VM with 4–8 GiB of RAM.
5. **S5 FarcadeGL v3 + FGSL v2** in the SDK, with docs (follows OS-6/OS-7).

---

## Part C: The engine (new repository)

### C1. Repository layout
```
farcade-engine/
  cmake/toolchains/minimalos-x86_64.cmake   # developer's PC build; headless-host.cmake for CI
  external/      lua-5.4, flecs, meshoptimizer, MaskedOcclusionCulling, JoltPhysics, ozz-animation,
                 recastnavigation, imgui(docking), ImGuizmo, imnodes, implot, textedit(fork), RmlUi,
                 cgltf, fast_obj, ufbx, stb, dr_libs, zstd, lz4, xxhash, msdfgen, doctest
  engine/core/       memory, jobs, simd math, containers, strings, log, profiler, reflection, serialize, vfs/pak
  engine/platform/   minimalos/ (window, input, audio, time, threads, fs, net), headless/ (CI)
  engine/ecs/        flecs integration, scene, prefabs, transform hierarchy, world cells
  engine/gfx/        FarcadeGL layer, feature levels, pass list, resource pools, upload rings
  engine/render/     vgeo/ voxel/ proxy/ gi/ shadows/ post/ materials/ debug/
  engine/assets/     importers, cooker, derived-data cache (DDC), paks
  engine/script/     Lua runtime, bindings, scheduler, debugger
  engine/{physics,anim,audio,ui,nav,module}/
  editor/            app, panels, gizmos, material graph, code editor, build/export, Lua debugger
  player/            feplayer runtime (also runs Lua-only games without a compiler)
  shaders/           FGSL sources per feature level
  templates/         empty, first-person, third-person, top-down
  tests/             unit, golden image diffs, perf scenes
```

### C2. Language and core
- **C++20 (GCC 15) subset:** `-fno-exceptions -fno-rtti`, no template-heavy libraries in public headers. **Game code includes only a light `fe.h`/`fe.hpp`** (target under 3k lines; no STL-heavy, flecs, Jolt or RmlUi headers), so C++ compiles quickly *on* the OS. The engine ships prebuilt as `libfe.a`.
- **SIMD:** in-house wrapper types. The baseline is SSE4.1 (x86-64-v2); AVX2 paths are dispatched by hand at startup (no ifunc, since the loader only handles RELATIVE relocations).
- **Memory:**
  - Today: carve 64 MiB `SYS_HEAP` chunks into size-class pools, per-frame linear allocators and per-thread caches.
  - After OS-1: back it with mmap reserve/commit.
  - Every library gets allocator hooks: Lua's `lua_Alloc`, Jolt and flecs.
- **Jobs:** a work-stealing job graph with one worker per core (falls back to a single thread until OS-3). Frames are pipelined: simulation of frame N+1 overlaps rendering of frame N.
- **Reflection:** X-macro descriptors `FE_REFLECT(T, (field, type)...)`, so no libclang is needed and it works with GCC on the OS. It drives the inspector, serialization, the Lua bindings, editor autocomplete and hot-reload snapshots.
- **Profiler:** its own rdtsc ring buffer per thread with an editor timeline (Tracy is dropped: it needs threads and networking first).
- **Large worlds:** 64-bit world positions (cell + float offset), rendered relative to the camera.

### C3. ECS, scene, world
- **flecs v4** (MIT, C99), used through its C API inside the engine. Its meta addon is filled from engine reflection, and its `os_api` hooks run on engine jobs. Components from Lua and C++ live in one world.
- Scenes are text for editing (diff-friendly) and cooked to binary. Prefabs use flecs prefabs.
- A world-partition grid of 32 m streaming cells feeds the proxy hierarchy (C4.6).

### C4. Rendering (FarcadeGL only)

#### C4.1 The `gfx` layer and feature levels
`gfx` is a thin layer over FarcadeGL:
- Resources are pooled and sub-allocated from FGPU buffers of at most 16 MiB until OS-6 lifts the limit, within handle budgets.
- Uploads go through rings of 2–3 buffers, and passes are an ordered list.
- FGSL is compiled at cook time and cached in paks.
- The feature level is picked from `fglGetDeviceInfo` caps, and the same game runs on all three:

| Level | Needs | Pipeline |
| --- | --- | --- |
| **FL0** | Today's FGPU v2 + scissor + more texture units + additive blend | Forward shading. DAG cut and **Masked Occlusion Culling** (Intel, SSE4.1/AVX2) run on the CPU and build a compacted index buffer each frame. Packed 20-byte vertices. 2 cached shadow cascades. Voxel far LOD as greedy-meshed quads. CPU GI into a probe atlas. |
| **FL1** | OS-6 (FGPU v3 graphics + FGSL v2) | **Visibility buffer** (R32UI + depth), Nanite-style material resolve (material-ID depth, one full-screen quad per material with depth EQUAL), TAA at internal resolution with scaled present, full hybrid GI, SSR, GTAO, ray-marched voxel bricks. |
| **FL2** | OS-7 (compute, SSBOs, 64-bit atomics) | GPU-driven culling and LOD, compute micro-triangle raster and voxel splatting into a 64-bit visibility buffer, compute GI, cached virtual shadow maps. Stretch goal. |

**Rule for the CPU path:** every pass runs at *internal* resolution (for example 960×540 for 1080p output) and is upscaled at present.

#### C4.2 Virtual geometry (CLOD)
- **Offline DAG build** with meshoptimizer ≥0.23 (MIT): `meshopt_buildMeshlets`, `meshopt_partitionClusters`, and `meshopt_simplifyWithAttributes` with `vertex_lock` on group borders and absolute errors. Its cluster-LOD demo is the starting reference.
- **Clusters and groups:** clusters hold up to 128 triangles and 64 vertices (7-bit local indices). Groups target 8 clusters (allowed 4–16) and are simplified to 50%, then split again. Errors are monotonic: a parent's error is ≥ its children's, and its LOD sphere encloses theirs.
- **Runtime cut:** `err_px = err · k / max(d_nearest, near)` with `k = 0.5·h / tan(fov/2)`. A cluster is drawn when its parent's projected error is above τ and its own is not. Each cluster decides independently, so the cut runs as parallel jobs. τ = 2 px (FL0), 1–1.25 px (FL1 with TAA), 1 px (FL2).
- **Culling:** an 8-wide BVH over groups (culled by maximum parent error), then frustum and normal-cone tests, then **two-pass occlusion**. Masked Occlusion Culling runs at 640×360, using last frame's cut at 4τ plus proxies as occluders.
- **Streaming:**
  - Page format: 64 KiB pages (about 30–40 clusters). Positions are quantized on a mesh-wide power-of-two grid so border vertices match and cracks can't appear.
  - Normals are octahedral and UVs quantized per cluster.
  - On disk: a delta/byte-plane transform plus zstd.
  - The top 2–3 levels and the voxel roots always stay resident.
  - Pools are 256 MB on the desktop and 64 MB in the VM, prioritized by projected error, with I/O on a worker thread.
  - At FL0/FL1, pages decode to float vertex pools; FL2 decodes in the shader from SSBOs.

#### C4.3 Voxels as the far representation (no impostors)
- **Switch rule, at build time:** DAG level L and everything coarser become voxels when any of these holds:
  - triangle reduction stalls below 1.4×;
  - voxels reach error ≤ e_tri(L) in fewer bytes than triangles;
  - the geometry is thin or aggregate (foliage, fences): a high area/volume ratio or many connected components.

  The first voxel edge is set to ≥ e_tri(L), so the error stays monotonic.
- **Bricks:** 8³ with a 512-bit occupancy mask, plus 8 bytes per occupied voxel: albedo RGB8 + coverage, octahedral normal 8:8, roughness and metal 4:4, and opacity per axis. Parents are built by 2×2×2 coverage-weighted downsampling, with normal variance folded into roughness (Toksvig).
- **Rendering per level:**
  - FL0: greedy-meshed quads, switched at a projected voxel edge of ≤2.5 px with 10% hysteresis.
  - FL1: a ray march through each brick's bounding box (3D texture plus `gl_FragDepth`) at ≤1.25 px, with a 15% dithered cross-fade that TAA resolves.
  - FL2: compute splatting with stochastic coverage at ≤1 px.
- **The same per-mesh bricks feed the GI voxel scene** (C4.5), so they are built once.

#### C4.4 Far proxies without instance data
For distant rings, each world cell has a **baked proxy**: one merged voxel/cluster CLOD of all its static instances. No per-instance transforms, bounds or culling data are loaded or processed. Ring k starts at `d_k = 2·cell_k`, with finest error `e_k = d_k·τ/k`:

| Ring | Cell | Starts at | Finest error (τ = 1.5 px, h = 540) |
| --- | --- | --- | --- |
| L0 | 32 m streaming cells with real instances | 0 | per asset |
| L1 | 128 m proxy | 256 m | ≈1.0 m |
| L2 | 512 m | 1 km | ≈3.9 m |
| L3 | 2 km | 4 km | ≈16 m |
| L4 | 8 km | 16 km | ≈62 m |

- **Bake pipeline** (incremental by a hash of each cell's contents, run as editor background jobs):
  1. Assign static instances to cells; instances larger than half a cell move up a level.
  2. Flatten each instance's cut at e/2 into world space.
  3. Voxelize conservatively with 4³ supersampled coverage.
  4. Keep triangles only where they win (large flat surfaces).
  5. Build L(k+1) by merging 4×4 cells of Lk and downsampling 2×2×2.
- **Drawing:** one draw per cell, which suits FL0. The working set scales with pixels, not with world area (about 1.5 M voxels, around 12 MB; pool 48–64 MB).
- **Dynamic objects** never go into proxies.

#### C4.5 Hybrid GI: screen space + voxels (no ray tracing)
- **The voxel scene** is a camera-centred clipmap built by instancing cooked per-mesh bricks.
  - Size: 4 levels of 64³ on the desktop, 3 in the VM; base voxel 0.5 m; coverage 32/64/128/256 m.
  - Scrolling is toroidal in 8-voxel slabs, and dynamic objects are re-splatted at 10 Hz.
  - Storage: a sparse table of surface voxels at 10 B each (albedo, normal, flags, R11G11B10 radiance) plus 64-bit occupancy masks per 4³ brick.
- **Light injection** (amortized over 1/4 of surface voxels per frame; 1/8 in the VM):
  - sun: CSM in range, beyond it a hierarchical voxel shadow ray of at most 64 steps;
  - up to 8 local lights and emissives;
  - albedo × last frame's irradiance, which gives multi-bounce lighting one frame late.
- **FL0** (CPU jobs, about 8 core-ms):
  - Probes: 3 levels of 24×12×24 L1-SH probes at 1/4/16 m spacing, 32 rays per update, 1/16 of probes updated per frame, hysteresis 0.93.
  - Probes are moved out of geometry and invalidated on back-face hits, which limits leaks.
  - The probe atlas is uploaded as ARGB8 textures and sampled with a manual slice lerp (4 taps).
  - Specular comes from probe SH plus an octahedral sky atlas with 5 roughness bands.
- **FL1** (about 8 core-ms in the kernel + 4 in userland):
  - The clipmap goes to the GPU as 4×64³ RGBA16F 3D textures (8 MiB), with at most 2 MB of dirty-brick uploads per frame.
  - **Screen probes** every 32 px on the CPU backend (16 px on NVIDIA), 32 rays each in 8×4 octahedral texels. Each ray marches in screen space over depth mips first (at most 16 steps), then continues in the voxel clipmap (at most 32 steps, level chosen by cone width), then falls back to sky or probes.
  - Denoising: temporal α = 0.1 with reprojection and depth/normal rejection, a 3×3 probe filter weighted by hit distance, then integration into L1 SH. Interpolation runs at half resolution with a joint-bilateral upsample.
  - Specular: SSR (half resolution, roughness < 0.45), then a voxel cone, then probes and sky.
  - GTAO at half resolution (2 slices × 6 steps) for short-range occlusion only.
- **Shadows:** 3 cached CSM cascades (1024²) plus voxel sun shadow beyond 128 m. Virtual shadow maps come at FL2.
- **Post:** TAA + temporal upscaling, dynamic resolution to hold the frame target, bloom, auto exposure, AgX tonemapping, and FXAA at FL0 (which has no TAA).

#### C4.6 Materials
- A node graph in the editor (imnodes) plus hand-written FGSL. Both generate per-level FGSL: a forward variant (FL0), a material-resolve variant (FL1) and a compute variant (FL2).
- The model is PBR metallic-roughness with layered inputs.
- The kernel compiles FGSL bytecode to x86 (OS-4 JIT) or to SASS, so there is one material path for CPU and GPU.

#### C4.7 Performance contract
**What the kernel CPU backend must reach (OS-4)**, per core, AVX2 at about 4 GHz (the SSE4.1 JIT reaches about 0.6× of this):

| Stage | Target |
| --- | --- |
| Per-draw overhead | ≤1.5 µs (one parallel phase per pass) |
| Vertex shading (25 instructions) | ≥40 M vertices/s |
| Clip, cull, set up and bin | ≥25 M triangles/s |
| Depth-tested pixels (large triangles, hierarchical Z) | ≥1.5 G px/s |
| Small triangles (≤2×2 px) | ≥30 M triangles/s |
| Fragment shader arithmetic | **≥16 G lane-instructions/s** (today about 0.1–0.2; `fglcube --shader` takes 42 ms) |
| ARGB8 bilinear sampling | ≥300 M samples/s (stochastic sampling ≥1 G/s) |
| Present at 1080p including 2× upscale | ≤1.5 ms |

**Frame budgets** (the core-ms are a starting point, to be checked with the benchmarks):
- **8-core AVX2 desktop, FL1, 960×540 → 1080p at 60 fps** (133 core-ms available):
  - kernel about 63: geometry 15, visibility buffer 6, resolve + light 18, GI 8, SSR/GTAO 4, TAA/post 9, UI/present 2.5;
  - userland about 39: simulation, physics, animation and Lua 20; cut + occlusion 5; GI upkeep 8; commands, streaming and audio 6;
  - **≈77% use**.
- **2-core QEMU+KVM, FL0/FL1-low, 640×360 → 720p at 30 fps:** ≈53 of 66 core-ms.
- **FL0 on today's kernel, before OS-4:** demo quality only (about 10–20 fps at 640×480 for small scenes).

### C5. Scripting: Lua 5.4 and C++ in one project
- **How mixing works:** both languages register into the same flecs world, through the same reflection.
  - C++ components: `struct Health { float hp, max; }; FE_REFLECT(Health, (hp,f32), (max,f32));`. They appear automatically in Lua (`e:get(Health).hp`) and in the inspector.
  - Lua components: `fe.component("Velocity", { x="f32", y="f32", z="f32" })`. C++ reads them by name through a dynamic view, or through a generated typed header.
  - Systems in either language: Lua `fe.system("Move", {Position, Velocity}, fn)`, C++ `FE_SYSTEM(Move, Position&, const Velocity&)`, ordered in one pipeline.
  - Calls across: Lua calls C++ functions exported with `FE_FUNCTION`, C++ calls Lua with `fe::lua::call("player.on_hit", ...)`, and both share one event bus.
  - Easy mode: a `Script` component with `on_start/on_update/on_event` callbacks (Unity/Godot style). Capable mode: plain ECS systems.
- **Fast Lua 5.4:**
  - Generational GC stepped within about 0.5 ms per frame, and an engine allocator through `lua_Alloc`.
  - Entities as light userdata (the 64-bit flecs id, generation checked on use, no allocation). Multi-return accessors (`x,y,z = fe.pos(e)`).
  - Batch column APIs (struct-of-arrays tables, refilled and reused).
  - Stripped precompiled bytecode in paks, tagged with the engine build hash. API functions cached in locals.
  - Pooled coroutines for sequences and AI (`wait(t)`, `wait_event`).
  - One VM on the game thread, a sandboxed `_ENV` per script, pcall with tracebacks into the editor console.
- **Bindings:** the stack API, generated from the reflection data (no sol2). Component get/set is data-driven, and function trampolines use shallow templates.
- **Lua hot reload:** swap the function tables and keep the state tables. The debugger uses `lua_sethook` (only while breakpoints are set): breakpoints, stepping, locals and upvalues, and a REPL. A count-hook sampling profiler covers scripts.
- **C++ game module:**
  - Built as ET_DYN with `-fPIC -shared -nostdlib`, with one entry point `fe_game_entry(const fe_api_v1*)` and every engine call going through that function table.
  - The engine's own ELF loader (about 1k lines) handles RELATIVE/64/GLOB_DAT/JUMP_SLOT relocations. Modules use no TLS, and `new`/`malloc` are routed through the API.
  - Hot reload keeps state with a reflection-driven ECS snapshot and restore.
  - Exported games link statically into one `.run`.
- **C++ debugging on the OS:** crash handler with stack traces (frame pointers + ELF symbols), asserts sent to the editor, serial log. The engine developer gets QEMU's gdbstub. A gdbserver-lite over TCP is a later stretch goal.

### C6. Other systems and libraries
Every library here must build with GCC against mlibc, and the heavy ones live only inside `libfe.a`.

| Area | Choice | Licence |
| --- | --- | --- |
| Physics | Jolt Physics 5 (custom JobSystem + allocator) | MIT |
| Animation | ozz-animation (runtime SIMD; gltf2ozz in the cooker) | MIT |
| Navigation | Recast/Detour | zlib |
| Occlusion | Intel Masked Occlusion Culling | Apache-2.0 |
| Editor UI | Dear ImGui (docking), ImGuizmo, imnodes, ImPlot | MIT |
| Code editor | ImGuiColorTextEdit fork (hand-written Lua/C++ tokenizers, no `std::regex`) | MIT |
| Game UI | RmlUi 6 (HTML/CSS, Lua plugin; renders through FarcadeGL; font engine on stb_truetype/msdf instead of FreeType) | MIT |
| Fonts | stb_truetype, msdfgen, Inter (already in MOSdesk) | PD / MIT / OFL |
| Import | cgltf, fast_obj, ufbx; stb_image + MOSdesk `image_decode.c` (PNG/JPEG/GIF) | MIT / PD |
| Audio | Own SIMD mixer (buses, 3D pan/attenuation/doppler, low-pass, reverb) over `SYS_AUDIO_STREAM`; dr_wav/flac/mp3, stb_vorbis, plus Minimal-OS `.adi` | PD / MIT-0 |
| Compression/hash | zstd, LZ4, xxHash (xxh3) | BSD |
| Tests | doctest | MIT |
| Runtime | mlibc; GCC 15.1/binutils 2.45 (runtime exception, so proprietary games are fine on the GCC side) | MIT / GPL+RLE |

### C7. Editor (runs on Minimal-OS)
- **Shell:**
  - Dear ImGui docking in a mosWM window (or full screen). It is drawn through FarcadeGL with scissor and a 6-float vertex layout in ring buffers.
  - Fallback until scissor lands: a userland 2D rasterizer (fast quads and glyphs) blitted with `SYS_WINDOW`.
  - **It redraws only on input or animation**, which is critical on a CPU-rendered OS.
  - The viewport is an FGPU surface shown as an ImGui texture.
  - The theme matches MOSdesk (Modern/Aero) with Inter text.
- **Panels:**
  - Scene hierarchy; inspector generated from reflection; viewports with gizmos, snapping and a grid.
  - Asset browser with thumbnails rendered by the engine; material graph.
  - Code editor (Lua/C++ with error markers from parsed GCC `file:line:col` output and `luaL_loadbuffer` checks while typing).
  - Build and console panels; profiler timeline; Lua debugger; SDK docs viewer (the existing markdown docs); project settings; export dialog.
- **Ease of use:** project templates, one-click **Play** (instant for Lua; C++ does an incremental build + hot reload), undo/redo, prefabs, drag and drop, a Ctrl+P command palette, autocomplete from the reflection database, and automatic LOD/voxel/proxy builds with no manual bake steps.
- **Getting assets onto the OS:**
  - "Import from URL" over HTTP/HTTPS (`SYS_NET` + minimaSSL), or Deliver (`dlr`) packages.
  - `.mpkg` boot modules.
  - `tools/minimafs-explorer/mfs-explorer.py` (writes files into a disk image from the PC).
  - A FAT32 disk, and later USB mass storage (OS-8).
- **Background work:** cooking into a derived-data cache `0:/projects/<name>/.cache/`, keyed by content hash. Autosave.

### C8. Project format, build and export
```
MyGame/
  project.fe      name, version, minimum feature level, entry scene, modules
  assets/         meshes, textures, audio, *.fmat materials, *.fscene scenes, prefabs
  scripts/*.lua
  src/*.cpp *.h   (optional C++ module)
  .cache/  build/
```
**Export** (in the editor, or `msdk export`):
1. Cook assets: CLOD pages, voxel bricks, cell proxies, FGSL bytecode, Lua bytecode, textures, audio.
2. Write `.fepak` files (zstd, parts of at most 2 GiB, 64-bit index).
3. Choose the binary:
   - **Lua-only:** use the prebuilt `feplayer` runtime. No compiler needed.
   - **C++:** the native `msdk` compiles the module and statically links `libfe.a` + `liblua.a` into `main.elf`, then wraps it in a MINIRUN1 `.run`.
4. Package the `.run` + paks as an `.mpkg` / Deliver package. It installs to `0:/programs/<Game>.run` + `0:/games/<Game>/`, with a MOSdesk icon (from `Resources/icon.bmp`).
5. "Run standalone" spawns the exported game as its own process.

---

## Milestones

Dependencies are in brackets. OS and engine lanes run in parallel.

| # | Milestone | Depends on | Exit criteria |
| --- | --- | --- | --- |
| E0 | Engine core + headless CI: allocator, jobs (single-thread fallback), SSE4.1 math, VFS/pak, profiler, reflection | none | Unit tests pass in CI; "hello pak" runs in QEMU |
| E1 | FL0 Lua player: `gfx`, forward renderer, flecs, Lua 5.4 bindings, mixer, cooker CLI | none (scissor helps) | A Lua-scripted level runs on today's kernel |
| E2 | **Native toolchain (critical path):** mlibc, cross GCC, Canadian cross, native `msdk` | OS-0, OS-1 | A 10-file C++ sample builds into a `.run` on the OS, each translation unit in under 3 s under KVM |
| E3 | Editor v0 on the OS, Lua only | scissor (OS-6 subset); runs alongside E2 | A Lua game is created, imported, edited and exported entirely on the OS |
| E4 | C++ gameplay: module loader, hot reload, standalone launch | E2, OS-1 PROT_EXEC | From a C++ edit to a reloaded game in under 5 s, with state kept |
| E5 | CLOD + voxel far LOD + cell proxies on FL0 (algorithms built earlier in host tests) | OS-4; OS-3 for a parallel cut and cook | A 2 km view from ≥50 M source triangles at 60 fps (desktop) or 30 fps (2-core VM) |
| E6 | FL0 hybrid GI (CPU voxels + probes) | OS-3; OS-2 recommended | Dynamic time of day with multi-bounce lighting within 8 userland core-ms |
| E7 | FL1: visibility buffer, TAA/upscale, screen probes + voxel GI, SSR, GTAO, ray-marched bricks | OS-6, OS-5 | The FL1 desktop budget in C4.7 met on the CPU backend |
| E8 | FL2 GPU-driven pipeline (stretch) | OS-7, NVIDIA proven on a card | Same scenes running natively on an RTX 2060 |
| E9 | Shipping polish: gamepads, Deliver packaging, templates, docs, performance CI | OS-8, OS-9 | A template game installs with `dlr install` on a second machine |

**OS track order:** OS-0, then OS-1, then OS-4 alongside OS-2 (the SSE4.1 JIT doesn't wait for AVX), then OS-3, OS-5, OS-6, OS-7, OS-8/OS-9.
**Minimum lovable product:** E0–E6 (FL0, Lua and C++, the on-OS editor, CLOD + voxels + proxies, CPU GI). FL2, NVIDIA, virtual shadow maps and gamepads are stretch goals.

---

## Main risks and mitigations
1. **The kernel CPU backend is the renderer's single point of failure.** Mitigation: the contract in C4.7, performance tests built on `tests/host/fgpu_raster_tests.c`, and keeping the backend free of kernel dependencies so it could also run in-process.
2. **The JIT runs in ring 0.** Mitigation: generate code only from checked IR, allow memory access only through helpers that clamp bounds, differential fuzzing against the interpreter, and a boot flag that falls back to the interpreter.
3. **The native toolchain is on the critical path** (cc1plus size, stack, RAM, spawn). Mitigation: ship Lua-only games first, keep OS-0/OS-1 small, pin GCC/binutils versions, build the Canadian cross in CI.
4. **Multicore races and XSAVE corruption in the kernel.** Mitigation: multicore threads opt-in at first, stress tests at `-smp 8`, YMM torture tests, a `noavx` flag.
5. **System-wide FGPU handle limits and the 16 MiB buffer cap.** Mitigation: pooled sub-allocation now, per-process quotas in v3.
6. **Editor latency (4 copies per frame through mosWM).** Mitigation: redraw only on change, damage rectangles, OS-5 shared surfaces.
7. **GPLv3 SDK headers compiled into every game.** Mitigation: relicense the SDK runtime files before release.
8. **Scope.** Mitigation: strict tiers, FL0-first, stretch goals labelled.

## Verification
- **Headless host CI (Linux):**
  - doctest unit tests for core, math, ECS and the Lua bindings.
  - DAG invariants: monotonic errors, crack-free borders.
  - Voxelizer and proxy baker checked against reference output and for determinism.
  - FGPU JIT checked against the interpreter by differential fuzzing; the existing `make test-host` pattern extends to new IR/raster tests.
- **Golden images:** fixed camera paths per feature level, rendered in QEMU and read back with `fglReadPixels`, then compared with tolerances. CPU and NVIDIA backends must match the same goldens.
- **Performance CI:** the Sponza, Bistro-style and foliage-field scenes on fixed paths, with frame times logged per pass against the C4.7 budgets (desktop under KVM with `-cpu host -smp 8`, plus a 2-core profile).
- **Self-hosting test on the OS:** the editor creates a template project, builds the C++ module with native GCC, hot-reloads it, exports a `.run`, and runs it as a separate process.
- **Real hardware:** an 8-core AVX2 desktop for the CPU path; an RTX 2060 for the NVIDIA path (`gpu nvidia` self-tests).

## After approval
No code changes yet. This plan gets saved as `docs/farcade-engine-plan.md` in Minimal-OS, and it moves to the engine repo once you create it. The suggested first implementation step, when you say go, is **OS-0** in Minimal-OS (TLS, futex, ns clock, spawn/wait), alongside **E0** in the new engine repo.
