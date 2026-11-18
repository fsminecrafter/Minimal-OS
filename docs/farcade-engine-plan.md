# Farcade Engine (working name): a high-performance game engine for Minimal-OS

> **How to read this plan.** *Overview* says what is being built and the four decisions. *Where things stand* is the current state. *Architecture* is the layer diagram. *Roadmap* has the milestones, a short list of small fixes to do first, and every milestone cut into small steps. Parts A, B and C give the detail for the OS track, the SDK/toolchain track and the engine. *Risks*, *Verification* and *After approval* close the plan. Step IDs (H1, OS-6.1, S1.2, E0.3 ...) are used in the roadmap. A step is meant to be about one pull request.

## Overview: goals and decisions

### Goals

Farcade Engine:
- It compiles and exports games to Minimal-OS and includes a Minimal-OS port of the SDK.
- Its editor is easy to use but capable.
- Gameplay code can be Lua and C++, mixed in one project.
- It has modern graphics: CLOD virtual geometry with voxels as the far representation (no impostors), a screen-space + voxel hybrid GI (no ray tracing), and far-away proxies that carry no instance data.
- It is fast.

### Decisions
1. **The editor and toolchain run on Minimal-OS only.** Game makers never need a PC. The engine developer (you) still builds the engine and editor on the PC (WSL/Docker) and tests them in QEMU.
2. **All rendering goes through FarcadeGL.** It already falls back between the kernel's CPU backend and the NVIDIA Turing backend, so speed work goes into FGPU itself.
3. **Scripting is Lua 5.4.**
4. **Kernel and SDK changes run as a parallel OS track.**

---

## Where things stand

### Minimal-OS today (the facts that shape this plan)

Updated after OS-0 to OS-5 (see the status list under Part A). Everything in the "Today" column has only run in QEMU, not on real hardware.

| Area | Today | What it means for the engine |
| --- | --- | --- |
| 3D | FGPU (`SYS_GPU` 40) runs in the kernel, with one context per program and one render lock. The CPU backend has a tile-binning rasterizer, an x86 shader JIT (SSE4.1 and AVX2) and asynchronous submits with fences. Read-only shader storage buffers (4 slots, vec4 loads) were added on 2026-11-16. **Scissor, 2D mips with trilinear filtering and 16 texture units** were added on 2026-11-17 (ABI 4; the NVIDIA backend does the scissor, mips and units above 4 still run on the CPU backend). It still has **no MRT, compute, writable SSBOs, instancing or integer formats** (OS-6 and OS-7). | The CPU backend *is* the renderer on QEMU, Intel and AMD. The engine can use scissor and mips now; MRT waits for OS-6. |
| NVIDIA | Turing GSP-RM, the copy engine and 3D clears work on an RTX 2060. The first draw failed with Xid 69 (a pitch-linear color target next to a depth buffer) and NVDEC with RM error 0x1F (no engine context); both are fixed in code (block-linear color copies; falcon context promote, 2026-11-17) but not yet rerun on the card. | A bonus path, not a dependency. |
| CPU | **AVX and AVX2 work** (XSAVE state saving, OS-2; AVX-512 untested). **A program's threads run on all cores** (OS-3), with affinity and priority calls. One intermittent kernel crash in the thread tests is still open. | The job system can use every core. AVX2 code paths are picked by hand at startup (no ifunc). |
| Memory | Programs live above 4 GiB with `SYS_VM` (map, unmap, protect, decommit; demand-zero pages, NX, guard pages). ELF files up to 256 MiB are streamed in, the main stack is 8 MiB (configurable), and the page pool is sized from RAM, including RAM above 4 GiB. The SDK's own `malloc` is still one syscall per call. | The engine's allocator sits on `SYS_VM`. GCC on the OS has the address space it needs. |
| Runtime | TLS, futex, clocks, spawn/wait and the environment exist (OS-0). The SDK's C library is still tiny: no libm, STL or `operator new`. Static PIE with RELATIVE relocations only (so no ifunc). | Until mlibc is ported, the engine carries a small C/C++ runtime of its own (`runtime/minimalos` in the engine repo). |
| On-OS tools | Nothing compiles or interprets on the OS except the FGSL compiler and Mxplorer's JS. | The native toolchain is on the critical path. |
| Display/input/audio | Full-screen present is one copy straight to the framebuffer, scaled 1x to 3x and paced by a timer (no vsync). Programs and the window manager can share window frames (OS-5), but mosWM's use of them is untested. Only the USB HID boot keyboard and mouse work: no hubs or gamepads. 8 PCM16 kernel-mixed streams. | The editor runs full screen and as a window under MOSdesk (copy path for now). Gamepads are on the OS track (OS-8). |
| Files | MinimaFS: 32-bit offsets, no modification times. `stat`, `rename`, `chdir` and `/` paths exist (OS-0). File reads are synchronous. | Content-hash builds; pak parts of at most 2 GiB; streaming reads on their own thread. |
| Licence | OS, SDK, MOSdesk and the engine repository are GPLv3. `farcadegl.h`, `fglsl.h` and `crt0.c` get compiled into every game. | **Decide before release:** relicense the runtime files (MIT, or GPL plus a linking exception). |

### Repository status (checked 2026-11-17)
- **Minimal-OS:** `main` has OS-0 to OS-5 (the work branch was merged as PR #13). Recent changes: read-only shader storage buffers (see OS-7), a Turing shader local-memory fix, and a Makefile change that syncs the pinned SDK before MOSdesk builds. This plan is `docs/farcade-engine-plan.md` there.
- **Minimal-OS-SDK:** `main` is at 4eb00ba. The SSBO header changes are only on the branch `farcade-engine/shader-storage-buffers-584f118` (584f118), which both Minimal-OS and Farcade-Engine pin. Merge it into `main`, or SDK `main` and the kernel ABI will not match.
- **MOSdesk:** `main` has the mosWM shared-frame compositing (untested). Minimal-OS still pins the older commit 19ee8b0.
- **Farcade-Engine:** builds with a `Makefile` and `mk/*.mk` fragments. It has the editor, the ECS, the Lua runtime, physics, the audio mixer, `.mof` models and the FarcadeGL renderer. Its `external/fgpu` (a copy of the kernel's FGPU CPU backend) is pinned to Minimal-OS a00577e, which is behind the kernel.
- **Toolchain (Part B):** not started. There is no mlibc, the GCC target is still the freestanding `x86_64-elf`, and `msdk` is a 5-line wrapper around `build.sh`.

### Status at a glance (2026-11-17)
| Item | State |
| --- | --- |
| OS-0 to OS-4 | Done (QEMU only). |
| OS-5 present and windows | Mostly done; mosWM compositing from shared frames is untested. |
| OS-6 FGPU v3 graphics + FGSL v2 | Started: OS-6.1 scissor done, OS-6.5 2D mips + trilinear done, OS-6.2 16 texture units and the limits block done (2026-11-17). The rest not started. |
| OS-7 compute + NVIDIA | Read-only SSBOs done; NVIDIA 3D/NVDEC fixes and NVIDIA scissor written, waiting for a run on the card; the rest not started. |
| OS-8 input and storage | Not started. |
| OS-9 audio | The queue query exists; the rest not started. |
| Dev loop | Done (H5): `make run` picks `-cpu host` with KVM or `-cpu max`; `make run-engine`. |
| S1 to S4 (mlibc, GCC target, `msdk` in C, native toolchain) | Not started. |
| S5 FarcadeGL v3 + FGSL v2 SDK | Follows OS-6/OS-7. |
| E0 core | Mostly open (only math exists). |
| E1 FL0 Lua player | Mostly done: feplayer, .fepak packages + VFS, feature levels from caps (2026-11-17). |
| E3 editor on the OS | Mostly done: project settings, model import, export of Lua games (.run + .fepak, .mpkg) on the OS (2026-11-17). |
| E2, E4 to E9 | Not started. |

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

## Roadmap: milestones and small steps

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

**Status (2026-11-17):**
- **E0:** mostly open. `engine/core` has only math; the allocator, jobs, profiler, reflection and VFS/pak are not there.
- **E1:** mostly done: renderer (feature levels, post stack, cascades, probe GI, graphics settings as named choices), flecs, Lua 5.4, the audio mixer, the `.fepak` reader/VFS and the `feplayer` runtime exist; a cooker CLI beyond `mofconv`/export and asynchronous loading do not.
- **E2:** not started (see Repository status).
- **E3:** mostly done: the editor runs on the OS (full screen and as a window, F11 fixed), with scenes, undo/redo, Lua scripts, project settings, model import (glTF/OBJ to .mof) and File > Export game (`.run` + `.fepak` + optional `.mpkg`, graphics settings exported); export runs on the OS in QEMU. Templates and prefabs are open. The UI was redesigned on 2026-11-18: floating windows with border and shadow, borderless docked panels, a sidebar in Settings, and 102 vector icons drawn from the `.img` format's new vector mode (SDK `mosmedia.h`, `mosmedia vimg`).
- **E4 to E9:** not started.

**OS track order:** OS-0, then OS-1, then OS-4 alongside OS-2 (the SSE4.1 JIT doesn't wait for AVX), then OS-3, OS-5, OS-6, OS-7, OS-8/OS-9.
**Minimum lovable product:** E0–E6 (FL0, Lua and C++, the on-OS editor, CLOD + voxels + proxies, CPU GI). FL2, NVIDIA, virtual shadow maps and gamepads are stretch goals.

### Do first: small fixes
These are small and unblock other work. Each is one task.
- [ ] **H1** Merge the SDK branch `farcade-engine/shader-storage-buffers-584f118` into Minimal-OS-SDK `main`, then re-pin Minimal-OS and Farcade-Engine to the merged commit.
- [ ] **H2** Refresh `external/fgpu` in Farcade-Engine from the current kernel (after H1) and rerun the host and QEMU golden checks.
- [ ] **H3** Find the OS-3 crash:
  - (a) repeat os0test's `smp_priority` join check in a loop at `-smp 1` and `-smp 2` until it reproduces;
  - (b) capture the state at the jump to address 3 (empty kernel stack);
  - (c) fix it;
  - (d) rerun the loop many times with no crash.
- [ ] **H4** Run mosWM compositing from shared frames on a desktop in QEMU, then move the MOSdesk pin in Minimal-OS from 19ee8b0 to a tested commit.
- [x] **H5** Dev loop: add `-cpu max` (TCG) or `-cpu host` (KVM) and a `run-engine` target (`-smp 8 -m 8G`) to the Makefile. Done 2026-11-17: `make run` uses `-accel kvm -cpu host` when `/dev/kvm` is usable, else `-cpu max`; `QEMU_CPU=`, `QEMU_ACCEL=`, `QEMU_SMP=` and `QEMU_MEM=` override; `make run-engine` is `QEMU_SMP=8 QEMU_MEM=8G run`.
- [ ] **H6** Decide the licence for the runtime files (`farcadegl.h`, `fglsl.h`, `crt0.c`): MIT, or GPL plus a linking exception. Needed before release.
- [ ] **H7** Settle C4.7: re-baseline the targets against the OS-4 measurements, or label the FL1/FL2 targets as real-GPU goals.
- [ ] **H8** Run the OS on real hardware (an 8-core AVX2 desktop): real SMP, TSC and APIC timing, the 1.5 ms present budget, and AVX-512 if the CPU has it.

### Work breakdown by milestone
Legend: `[x]` done, `[~]` partly done, `[ ]` open. "Needs" lists what must come first.

#### E0 Engine core + headless CI
- [ ] **E0.1** Allocator: size-class pools carved from 64 MiB `SYS_HEAP` chunks, per-frame linear allocators, per-thread caches (later backed by `SYS_VM` reserve/commit).
- [ ] **E0.2** Allocator hooks for Lua (`lua_Alloc`) and flecs (Jolt later).
- [ ] **E0.3** Jobs, single-thread fallback first: the job-graph API running on one thread.
- [ ] **E0.4** Jobs on all cores: work stealing, one worker per core (OS-3 is done).
- [ ] **E0.5** SSE4.1 math wrappers (extending `engine/core/math`) and the by-hand AVX2 dispatch at startup.
- [ ] **E0.6** Containers, strings and log.
- [ ] **E0.7** Reflection: `FE_REFLECT` X-macros, starting from the existing field tables in `engine/scene/modules.cpp`.
- [ ] **E0.8** Serialization driven by reflection (text scenes, cooked binary). Needs E0.7.
- [~] **E0.9** VFS and `.fepak` reader (zstd, parts of at most 2 GiB, 64-bit index). Done without zstd (2026-11-17, engine/core/pak, vfs; docs/pak-format.md).
- [ ] **E0.10** Profiler: an rdtsc ring buffer per thread.
- [~] **E0.11** Unit tests in headless CI: host tests (`make test`) exist; doctest is not added yet.
- [ ] **E0.12** Exit: "hello pak" runs in QEMU.

#### E1 FL0 Lua player
- [x] Done so far: flecs behind `fe::World`, Lua 5.4.7 running scripts in Play, the FL0-style forward renderer and GPU tier, an audio mixer.
- [ ] **E1.1** `gfx` resource pools: sub-allocation from FGPU buffers of at most 16 MiB, within handle budgets.
- [ ] **E1.2** Upload rings (2 to 3 buffers) and an ordered pass list.
- [x] **E1.3** Pick the feature level from `fglGetDeviceInfo` caps (engine/gfx/feature_level).
- [ ] **E1.4** Packed 20-byte vertices for FL0.
- [ ] **E1.5** FGSL compiled at cook time and cached in paks. Needs E0.9.
- [~] **E1.6** Lua bindings generated from reflection (bindings exist; generation needs E0.7).
- [~] **E1.7** Audio mixer features from C6: buses, 3D pan/attenuation/doppler, low-pass, reverb (a mixer exists in `engine/audio/mixer.cpp`).
- [~] **E1.8** Cooker CLI: `tools/mofconv` converts models; add textures, audio, Lua bytecode, FGSL and scenes, writing paks.
- [~] **E1.9** Pak reader and asynchronous loader on an I/O thread. Needs E0.9. The pak reader is done; loading is synchronous.
- [ ] **E1.10** Exit: a Lua-scripted level runs on today's kernel.

#### E2 Native toolchain (critical path)
- [ ] **E2.1** S1 mlibc port (steps S1.1 to S1.6 in Part B).
- [ ] **E2.2** S2 GCC target `x86_64-minimalos` (S2.1 to S2.6).
- [ ] **E2.3** S3 `msdk` in C (S3.1 to S3.6); this can run alongside E2.1 and E2.2.
- [ ] **E2.4** S4 Canadian cross (S4.1 to S4.7). Needs E2.1 and E2.2.
- [ ] **E2.5** Exit: a 10-file C++ sample builds into a `.run` on the OS, each translation unit in under 3 s under KVM.

#### E3 Editor v0 on the OS, Lua only
- [x] Done so far: shell, panels, gizmos, Play, `.fscene` save/open, undo/redo, the unsaved-changes prompt, scripts on objects, running full screen and as a window.
- [ ] **E3.1** Draw ImGui through FarcadeGL with scissor and a 6-float vertex layout in ring buffers. Needs OS-6.1.
- [ ] **E3.2** Project format: `project.fe`, templates (empty, first-person, third-person, top-down).
- [ ] **E3.3** Asset import: "Import from URL" (`SYS_NET` + minimaSSL), Deliver packages, `.mpkg` modules.
- [ ] **E3.4** Prefabs on flecs prefabs.
- [ ] **E3.5** Autosave.
- [x] **E3.6** Export of Lua-only games with the prebuilt `feplayer`, then packaging as a `.run` + paks (C8). Done 2026-11-17: File > Export game and `farcade-editor --export`, `.fepak` next to the `.run`, optional `.mpkg`; graphics settings go with the game and scripts change them with `fe.graphics`.
- [ ] **E3.7** Exit: a Lua game is created, imported, edited and exported entirely on the OS.

#### E4 C++ gameplay
- [ ] **E4.1** The light `fe.h`/`fe.hpp` header (under 3k lines).
- [ ] **E4.2** Module ABI: `fe_game_entry(const fe_api_v1*)` and the function table.
- [ ] **E4.3** The engine's own ELF loader (about 1k lines: RELATIVE/64/GLOB_DAT/JUMP_SLOT). Needs OS-1 `PROT_EXEC` (done).
- [ ] **E4.4** Build a module with the native `msdk` and load it. Needs E2.
- [ ] **E4.5** `FE_COMPONENT`/`FE_SYSTEM`/`FE_FUNCTION` and Lua calls both ways, on one event bus.
- [ ] **E4.6** Hot reload with a reflection-driven ECS snapshot and restore.
- [ ] **E4.7** Crash handler with stack traces, asserts sent to the editor, serial log.
- [ ] **E4.8** Standalone launch: spawn the exported game as its own process.
- [ ] **E4.9** Lua side: hot reload (swap function tables, keep state), `lua_sethook` debugger, time limits for runaway scripts.
- [ ] **E4.10** Exit: from a C++ edit to a reloaded game in under 5 s, with state kept.

#### E5 CLOD + voxel far LOD + cell proxies on FL0
Algorithms are built first in host tests. Needs OS-4 (done) and OS-3 (done) for the parallel cut and cook.
- [ ] **E5.1** Offline DAG build with meshoptimizer (clusters, groups, monotonic errors, locked borders), with the invariant tests.
- [ ] **E5.2** Page format: 64 KiB pages, quantized positions, octahedral normals, delta/byte-plane transform plus zstd.
- [ ] **E5.3** Runtime cut as parallel jobs (`err_px` test, τ = 2 px at FL0).
- [ ] **E5.4** 8-wide BVH, frustum and normal-cone tests, then two-pass Masked Occlusion Culling.
- [ ] **E5.5** CLOD page streaming with the fixed pool budget, priority by projected error and an I/O thread.
- [ ] **E5.6** Voxel switch rule and 8³ bricks, with the parent downsampling.
- [ ] **E5.7** FL0 far LOD: greedy-meshed quads with the 2.5 px switch and hysteresis.
- [ ] **E5.8** World cells (32 m) with the cell states and the always-loaded layer.
- [ ] **E5.9** Proxy baker (steps 1 to 5 of C4.4) for ring L1, then L2 to L4.
- [ ] **E5.10** FL0 texture streaming in whole steps (64, 256, 1024).
- [ ] **E5.11** Streaming overlay and the "keep loaded" flag in the editor.
- [ ] **E5.12** Exit: a 2 km view from at least 50 M source triangles at 60 fps (desktop) or 30 fps (2-core VM) (see C4.7 for the open target decision).

#### E6 FL0 hybrid GI (CPU voxels + probes)
Needs OS-3 (done); OS-2 recommended (done).
- [ ] **E6.1** Camera-centred voxel clipmap built from the cooked bricks, with toroidal scrolling and 10 Hz re-splatting of dynamic objects.
- [ ] **E6.2** Light injection: sun (CSM, then voxel shadow ray), local lights and emissives, last frame's irradiance.
- [ ] **E6.3** Probe update: 3 levels of L1-SH probes, 32 rays, 1/16 per frame, hysteresis 0.93, with the back-face and move-out rules.
- [ ] **E6.4** Probe atlas upload (ARGB8) and the 4-tap sampling.
- [ ] **E6.5** Specular: probe SH plus the octahedral sky atlas with 5 roughness bands.
- [ ] **E6.6** Exit: dynamic time of day with multi-bounce lighting within 8 userland core-ms.

#### E7 FL1
Needs OS-6 and OS-5.
- [ ] **E7.1** Visibility buffer (R32UI + depth).
- [ ] **E7.2** Material resolve (material-ID depth, one quad per material with depth EQUAL).
- [ ] **E7.3** TAA at internal resolution with scaled present, dynamic resolution.
- [ ] **E7.4** Screen probes and the voxel clipmap on the GPU (4×64³ RGBA16F).
- [ ] **E7.5** Screen-probe ray march (depth mips, then clipmap) and denoising into L1 SH.
- [ ] **E7.6** SSR and GTAO.
- [ ] **E7.7** Ray-marched voxel bricks with the dithered cross-fade.
- [ ] **E7.8** Per-mip texture streaming.
- [ ] **E7.9** Exit: the FL1 desktop budget in C4.7 is met on the CPU backend (see C4.7).

#### E8 FL2 GPU-driven pipeline (stretch)
Needs OS-7 and NVIDIA proven on a card.
- [ ] **E8.1** GPU-driven culling and LOD.
- [ ] **E8.2** Compute micro-triangle raster and voxel splatting into a 64-bit visibility buffer.
- [ ] **E8.3** Compute GI and cached virtual shadow maps.
- [ ] **E8.4** Sparse virtual textures.
- [ ] **E8.5** Exit: the same scenes run natively on an RTX 2060.

#### E9 Shipping polish
Needs OS-8 and OS-9.
- [ ] **E9.1** Gamepad support in the engine's input.
- [ ] **E9.2** Deliver packaging (`.mpkg` / `dlr`) with a MOSdesk icon.
- [ ] **E9.3** Templates and docs.
- [ ] **E9.4** Performance CI on the fixed scenes.
- [ ] **E9.5** Exit: a template game installs with `dlr install` on a second machine.

---

## Part A: OS track (Minimal-OS)

**Status (merged into `main` through PR #13; it was branch `claude/os0-process-basics-bq26wb`):**
- **OS-0 done.** TLS, futex, clocks, stat/rename/chdir, spawn/wait, environment.
- **OS-1 done.** Program memory above 4 GiB with demand-zero pages (`SYS_VM`), NX, guard pages, a streaming ELF loader, 8 MiB stacks, a page pool sized from RAM, and RAM above 4 GiB used for program pages.
- **OS-2 done, AVX-512 untested.** XSAVE-based state saving, AVX/AVX2 usable, `noavx` flag, CPU features in SYSINFO. QEMU's emulation has no AVX-512, so that path has never run.
- **OS-3 done, QEMU only.** Threads of one program run on different CPUs by default (`threads1cpu` pins them as before; `smpthreads` forces it on). It includes:
  - TLB shootdown IPIs, with a per-space mask of the CPUs that have the space loaded;
  - a reschedule IPI so idle cores start work at once;
  - thread placement and migration like processes;
  - affinity and priority in `SYS_THREAD` (`thread.h`: `mos_thread_set_affinity/priority`, `mos_cpu_count`);
  - a group exit/crash that stops threads running on other CPUs;
  - an audit of the shared state (`docs/boot-and-kernel.md`, Processes And Threads).

  Thread entry points outside the image were already allowed in OS-1. Tested at `-smp 1/2/4/8` in TCG only; real SMP hardware (including TSC and APIC timing) has not been tried.
  - **Known bug (open, not diagnosed):** an intermittent kernel crash (a jump to address 3 with an empty kernel stack, then the CPU idles) right after os0test's `smp_priority` checks, around joining a low-priority thread. Seen twice in about ten runs (once on 1 CPU, once on 2, the second before the OS-4/OS-5 changes); reruns of the same build pass.
- **OS-4 done, QEMU only.**
  - Rasterizer: tile binning, hierarchical Z and a small-triangle path (default; `fgpuold` selects the previous one).
  - Shader JIT (`fgpu_jit_x64.c`): SSE4.1 and AVX2 code, checked against the interpreter by a fuzzer (`nojit` turns it off).
  - Per-program GPU contexts: objects shared by a program's threads, grown tables with O(1) handles and quotas of 1024/4096/1024, and a per-program lock plus one render lock instead of `g_busy`.
  - Asynchronous submit with fences (ABI 3: `FGPU_SUBMIT_ASYNC`, `SYS_GPU_WAIT`), run by the `fgpu-worker` kernel process.
  - Preemptible `smp-par` helper processes instead of IPI handlers (`smpparipi` brings the IPI helpers back).
  - Measured on one 2.1 GHz core: about 8 M small triangles/s (C4.7 target 30 M/s) and 7 G lane-instructions/s at best (target 16 G). The engine targets a real GPU, so these targets are no longer pursued. Fused rasterizer/shader kernels and SIMD texture sampling were dropped from OS-4 and remain possible later work.
- **OS-5 mostly done, QEMU only.**
  - Scaled present (1x/2x/3x, nearest) straight into the framebuffer with non-temporal stores: one copy instead of two (`FGPU_PRESENT_SCALE2/3`).
  - Frame pacing per program on the monotonic clock (`FGPU_PRESENT_PACED`, `SYS_GPU_SET_PACE`; the adapters here have no vblank).
  - Window frames shared between a program and the window manager (reference-counted shared memory in user VM, `SYS_WINDOW_MAP_FRAME`, `PRESENT_SHARED` with damage rectangles, `TAKE_DAMAGE`). FGPU presents into a window's shared back buffer.
  - **Left:** mosWM compositing from the mapped frames is on MOSdesk `main` (commit 7b3bf89, marked untested) but has not been run on a desktop, so the Minimal-OS submodule still pins the older copy-path mosWM (19ee8b0). The 1.5 ms present budget is unmeasured on real hardware.
- **System-call pointer checks done** (not in the original plan): every user pointer is checked before use; what is left is listed in `docs/syscall-pointers.md`.
- Everything was tested in QEMU without KVM (`make ci-os0`); nothing has run on real hardware yet.
- The SDK half is merged into Minimal-OS-SDK `main` (4eb00ba). The Minimal-OS submodule now pins 584f118, which is ahead of `main` (see Repository status).

The items are listed in dependency order. File paths were rechecked on 2026-11-17.

| ID | Work | Main files |
| --- | --- | --- |
| **OS-0** process and libc basics | Per-thread FS base (TLS) saved on context switch. `SYS_FUTEX` wait/wake with timeout (keyed by memory owner + virtual address). A nanosecond monotonic clock (TSC calibrated against the PIT; `rdtsc` already works in user mode). `stat` and `rename`, aliases between `/` and `0:/` paths. `SYS_SPAWN` (ELF + argv + envp + cwd + stdio files) and `SYS_WAIT` (exit code). Environment variables. | `src/impl/proc/context_switch.asm` (MSR 0xC0000100), `src/intf/x86_64/proc.h` (append fields only; the asm hard-codes 0x90/0xD8/0xE0), `src/impl/kernel/syscall.c`, `src/intf/x86_64/syscall.h` + `Minimal-OS-SDK/include/syscall.h`, `src/impl/time.c`, `src/impl/kernel/lapic_timer.c`, `src/impl/handlers/prochandler.c`, `minimafshandler.c` |
| **OS-1** real user virtual memory | Anonymous `mmap`/`munmap`/`mprotect` with reserve/commit. **NX on by default, `PROT_EXEC` added in the same change** (the module loader depends on today's accidental RWX heap). Guard pages. ELF up to 256 MiB loaded by streaming PT_LOAD segments (cc1plus is 30–40 MB). Configurable main stack (8 MiB+). A frame allocator replaces the 64 MiB pool and returns errors instead of panicking. Later: RAM above 4 GiB for user pages. | `src/impl/proc/proc.c:80-206`, `src/impl/pmm.c:19`, `src/impl/kernel/memlayout.c`, `src/intf/mmu_tables.h`, `src/impl/kernel/allocator.c`, `src/impl/kernel/syscall.c:769-829` (SYS_HEAP), `src/impl/loader/elfloader.c`, `src/impl/commands/runcommand.c` |
| **OS-2** AVX/AVX2 | `CR4.OSXSAVE` + `XSETBV` (x87, SSE, AVX; AVX-512 when present). Replace `fxsave64` with `xsaveopt` using the CPUID.0xD size and 64-byte alignment. Report CPU features in SYSINFO. Add a `noavx` boot flag and a YMM torture test. | `src/impl/boot/main.asm:156`, `ap_trampoline.asm:41`, `src/impl/syscall_isr.asm:29`, `idt_isr.asm:72`, `idt_.asm:46`, `src/impl/kernel/smp_par.c` |
| **OS-3** multicore user threads | Threads of one process run on any CPU: affinity + priority syscalls, join through futex, TLB shootdown IPIs (today `invlpg` is local only), thread entry points allowed outside the image. Opt-in at first, with stress tests at `-smp 8`. | `src/impl/proc/proc.c:700-748`, `scheduler.c` (~294 skips thread groups), `thread.c`, `src/impl/kernel/lapic.c`, `spinlock.h` |
| **OS-4** FGPU CPU backend v2 (the renderer's speed) | (1) **Binning:** one parallel phase per render-target pass instead of a barrier per draw. Geometry (vertex shading, clipping, culling, 16.8 fixed-point setup) bins into 64×64 tiles; tiles are rasterized by work stealing, with tile-local color+depth (32 KB) and hierarchical Z (min/max per 8×8). (2) A small-triangle path (bounding box ≤2×2 px, one triangle per SIMD lane). (3) **A JIT** from the branch-free SSA IR in `fgpu_ir.c` to x86 code with fused vertex/fragment kernels: **SSE4.1 first** (doesn't wait on OS-2), AVX2 later. The interpreter stays as reference, with differential fuzzing. (4) Preemptible kernel worker threads instead of interrupt-driven `smp_par`. Per-process contexts and handle quotas instead of `g_busy` and the shared 64/128/64 object limits. Asynchronous submit with fences (`SYS_GPU_WAIT`). | `src/modules/GPU/fgpu.c:29-31,167-183,723,771`, `fgpu_raster.c:633-645`, `fgpu_shader.c`, `fgpu_ir.c`, new `fgpu_jit_x64.c`, `src/impl/kernel/smp_par.c`, `tests/host/fgpu_*_tests.c` |
| **OS-5** present and windows | Scaled present (2×/3× with non-temporal stores into the write-combining framebuffer). Window frames shared between the process and mosWM (no copies), damage rectangles, frame pacing (timer-based where there's no vblank). | `src/impl/handlers/graphics.c`, `gfx_blit.c:148`, `src/impl/kernel/window_manager.c`, `mmio.c:222`, `MOSdesk/mosWM/main.c` |
| **OS-6** FGPU v3 graphics + FGSL v2 (FL1) | Scissor (also needed early, for FL0), MRT ×4, formats (R32UI, RG32F, RGBA16F, R11G11B10F, RGBA32F), mips + trilinear, depth textures + compare samplers, `gl_FragDepth`, flat/int varyings, `gl_VertexID/InstanceID/PrimitiveID`, `texelFetch`, 3D textures + 2D arrays, instancing, base vertex, (multi-)indirect draw, 16 texture units and 16 varyings, larger uniform blocks, buffers of 256 MiB+, configurable depth/blend/write masks, depth bias, timestamp queries, and a v3 limits block in `syscall_gpu_info_t`. FGSL v2: real ints/uints/bit ops, structs, mat3, functions, bounded `while`/`do`, `#include`. | `src/intf/x86_64/fgpu_abi.h` + `Minimal-OS-SDK/include/fgpu_abi.h` (keep identical), `fgpu.c`, `fgpu_raster.c`, `fgpu_ir.c`, `farcadegl.h`, `fglsl.h` |
| **OS-7** FGPU v3 compute + NVIDIA (FL2, stretch) | **Done (2026-11-16):** read-only SSBOs (4 slots, `FGPU_OP_BIND_STORAGE_BUFFER`, `FGPU_SOP_BUFFER_LOAD`, out-of-range reads return zero). **Left:** a compute stage on the CPU backend: workgroups spread across cores, 8 lanes per batch, barriers handled by splitting the branch-free IR, atomics via `lock cmpxchg`. Also writable SSBOs, image load/store, shared memory, 32/64-bit atomics and indirect dispatch. On NVIDIA: prove 3D on the card, batch one job per frame, compute class, real branches, proper SASS scheduling. | `fgpu_backend_nv.c`, `nv_3d.c`, `nv_sass.c`, `nv_rm_chan.c` |
| **OS-8** input and storage | USB hubs, a HID report-descriptor parser (generic and Xbox/PlayStation USB gamepads), PS/2 mouse, input timestamps. Expose `fat32.c` through the storage syscalls and VFS; USB mass storage later (asset import). | `src/modules/USB/usb/*.c`, `src/intf/usb/usb_stack.h`, `src/impl/handlers/input.c`, `src/impl/storage/fat32.c`, `storage_syscall.c` |
| **OS-9** audio | **Already there:** the query for how much of a stream is queued (`mos_audio_stream_query`: queued, capacity of 16384 frames, mix_rate, underruns). **Left:** smaller buffers for lower latency, float32/48 kHz input. | `src/modules/Audio/*` |
| Dev loop | `make run` uses `qemu64` (no SSE4.1). Add `-cpu host` (KVM) or `-cpu max` (TCG), and a `run-engine` target with `-smp 8 -m 8G`. On Windows, run QEMU with KVM inside WSL2. **Status:** done 2026-11-17 (H5): `make run` passes `-cpu host` with KVM, else `-cpu max`; `make run-engine` is `-smp 8 -m 8G`. | `Makefile:351`, `config.mk` |

### Remaining OS work in small steps
Keep `fgpu_abi.h` identical in the kernel and the SDK in every step.

**OS-6 FGPU v3 graphics + FGSL v2 (FL1)**
- [x] **OS-6.1** Scissor. Done 2026-11-17 (ABI 4): `FGPU_OP_SCISSOR`, top-left origin like the viewport, applied to draws (every path: clipped tile range and edge-setup bounds, no per-pixel cost) and clears (`fgpu_pass_clear_rect`); `FGPU_CAP_SCISSOR`; SDK `fglEnable(FGL_SCISSOR_TEST)` + `fglScissor`. The NVIDIA backend sets the hardware scissor for draws and clears (not yet run on a card).
- [ ] **OS-6.2** Limits: ~~16 texture units~~ (done 2026-11-17: `FGPU_TEXTURE_UNITS` = 16 on the CPU backend, `FGPU_MAX_TEXTURE_UNITS` stays 4 for the NVIDIA backend; the limits block with `max_texture_units`, `max_mip_levels`, ... is at the end of `syscall_gpu_info_t`, `SYS_GPU_INFO` also accepts the old size), 16 varyings, larger uniform blocks, buffers of 256 MiB+ still to do.
- [ ] **OS-6.3** Targets: MRT ×4, the formats (R32UI, RG32F, RGBA16F, R11G11B10F, RGBA32F), configurable depth/blend/write masks, depth bias.
- [ ] **OS-6.4** Depth: depth textures, compare samplers, `gl_FragDepth`.
- [ ] **OS-6.5** Textures: ~~mips with trilinear filtering~~ (2D done 2026-11-17: `FGPU_SURFACE_MIPMAPS`, per-level `WRITE_SURFACE`, `FGPU_OP_GENERATE_MIPMAPS` box filter, `FGPU_TEXTURE_MIP_NEAREST/LINEAR`, lod bias and min/max clamps, level of detail from 2x2 pixel quads in the interpreter, the JIT and both rasterizer paths; no explicit-LOD `textureLod`, no `dFdx` in FGSL yet), `texelFetch`, 3D textures and 2D arrays still to do.
- [ ] **OS-6.6** Draws: instancing, base vertex, (multi-)indirect draw, `gl_VertexID/InstanceID/PrimitiveID`, flat/int varyings.
- [ ] **OS-6.7** Timestamp queries.
- [ ] **OS-6.8** FGSL v2: ints/uints/bit ops, structs, mat3, functions, bounded `while`/`do`, `#include`.
- [ ] **OS-6.9** SDK side (S5): `farcadegl.h`, `fglsl.h` and docs.

**OS-7 FGPU v3 compute + NVIDIA (FL2, stretch)**
- [x] **OS-7.1** Read-only SSBOs (4 slots).
- [ ] **OS-7.2** Writable SSBOs and image load/store.
- [ ] **OS-7.3** A compute stage on the CPU backend: workgroups across cores, 8 lanes per batch.
- [ ] **OS-7.4** Barriers by splitting the branch-free IR, shared memory, 32/64-bit atomics via `lock cmpxchg`, indirect dispatch.
- [ ] **OS-7.5** NVIDIA: prove 3D on the card.
- [ ] **OS-7.6** NVIDIA: batch one job per frame, the compute class, real branches, proper SASS scheduling.
- [~] **OS-7.7** NVIDIA backend catches up with FGPU ABI 4 (the `TODO(nv)` guards below). Scissor done; mipmaps and 16 texture units open.

**NVIDIA backend: `TODO(nv)` guards and integration notes (from the OS-6 work, 2026-11-17)**

The OS-6 work (FGPU ABI 4: scissor, 2D mipmaps, 16 texture units) kept such work off the NVIDIA backend with `TODO(nv)` guards. Status as of the merge into `main` (Minimal-OS 6c70d5c / ef03f05):

*TODO(nv) guards*
- `src/modules/GPU/fgpu_backend_nv.c`, in `nv_draw` (now `nv_draw_3d`), right after `const fgpu_pipeline_t* pipe = d->pipe;`:
  ```c
  // TODO(nv): ... scissor, mipmapped textures (mip_count > 1), texture units >= FGPU_MAX_TEXTURE_UNITS ...
  if (pipe->scissor) return false;
  for (uint32_t u = 0; u < FGPU_TEXTURE_UNITS; u++)
      if (pipe->textures[u].pixels && (pipe->textures[u].mip_count > 1 || u >= FGPU_MAX_TEXTURE_UNITS)) return false;
  ```
  **Status:** the `pipe->scissor` line is gone (6c70d5c: the backend sets the hardware scissor); the mip-chain and unit checks stay.
- There was also a one-comment TODO above `nv_clear`. **Status:** removed with the scissored clears (6c70d5c).
- `nv_clear` had no pipeline or scissor argument, so the scissored-clear guard was in `fgpu.c`, in the `FGPU_OP_CLEAR` case: `if (g_hw && !st->pipe.scissor)`. **Status:** removed; `fgpu_hw_target_t` now carries the scissor rectangle and `nv_clear` clears just that rectangle (3D engine `SET_CLEAR_SURFACE_CONTROL.USE_SCISSOR0`, or a copy-engine fill of the rectangle).
- `fgpu.c` also skips offering a draw to the hardware backend when scissor is on, any bound texture has a chain, or a bound texture is on unit >= 4. This is a `cpu_only` check marked `TODO(nv)`. **Status:** the scissor part is gone; the mip-chain and unit parts stay.
- Shaders that sample a unit above 3 already fail `nv_sass.c`'s own range check, so they run on the CPU.
- `FGPU_MAX_TEXTURE_UNITS` carries a `TODO(nv)` comment in `fgpu_abi.h`.
- On the d12ab89 merge: `nv_sass_tests.c` took only the `fgpu_texture_t` initialiser change, and `fgpu_backend_nv.c` took the comment above `nv_clear` plus the `nv_draw` guard. Nothing in `nv_3d.c` was touched by the OS-6 work.

*What the integrator had to do*
- [x] Bump the SDK pin in Minimal-OS to the SDK commit `61e38ca` (branch `wt/os-fgpu`). Done in ef03f05.
- [x] Copy the FGPU sources into Farcade-Engine's `external/fgpu`. The set that changed: `fgpu_abi.h`, `fgpu_raster.h`, `fgpu_shader.h`, `fgpu_raster.c`, `fgpu_shader.c`, `fgpu_jit_x64.c`. Done in Farcade-Engine ff81b3a (with the host device `engine/gfx/host/fgpu_host.c` taught ABI 4). The engine does not copy the kernel's host tests, so `tests/host/fgpu_mip_tests.c` and its Makefile rule (which must come after `JIT_SRC` is defined) stay in Minimal-OS.
- [x] Merge d12ab89 with `wt/os-fgpu` and rerun `make test-host -j2`. Done in 6c70d5c; all host tests pass, `ci-os0` passes (1085 checks).
- NV backend work for later:
  - [x] Scissor: add a rectangle parameter to the backend `clear` callback and set hardware scissor in `nv_3d.c`, then drop both the `nv_draw` guard and the `fgpu.c` check. Done in 6c70d5c (the rectangle travels in `fgpu_hw_target_t`); not yet run on a card.
  - [ ] Mips: sample mip chains by writing a multi-level texture header, and place such surfaces in VRAM.
  - [ ] 16 units: widen the constant-buffer texture-handle table and the TIC/TSC pools, then raise `FGPU_MAX_TEXTURE_UNITS` to `FGPU_TEXTURE_UNITS`.
- Not done, and not needed for the first step: explicit-LOD `textureLod` and `dFdx` in FGSL, `texelFetch`, and 3D/array textures.
- One known limit: the legacy immediate path (`fgpuold`, float edge functions) can miss pixels that lie exactly on a shared diagonal. The previous rasterizer already did that (1 pixel in a 64x64 quad). The binned default path is exact, and the mip tests allow about 1% there for the immediate path only.

**OS-8 input and storage**
- [ ] **OS-8.1** USB hubs.
- [ ] **OS-8.2** HID report-descriptor parser.
- [ ] **OS-8.3** Generic and Xbox/PlayStation USB gamepads.
- [ ] **OS-8.4** PS/2 mouse and input timestamps.
- [ ] **OS-8.5** Expose `fat32.c` through the storage syscalls and VFS.
- [ ] **OS-8.6** USB mass storage (later, for asset import).

**OS-9 audio**
- [x] **OS-9.1** The queued-amount query.
- [ ] **OS-9.2** Smaller buffers for lower latency.
- [ ] **OS-9.3** float32 and 48 kHz input.

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

### Steps
**S1 mlibc port**
- [ ] **S1.1** Build mlibc with a Minimal-OS sysdeps skeleton.
- [ ] **S1.2** Memory, threads and time sysdeps: `tcb_set`, `futex_wait/wake`, `anon_allocate/free`, `vm_map`, `clock_get`.
- [ ] **S1.3** File sysdeps: open/read/write/seek/close/stat, with the `/` ↔ `0:/` translation.
- [ ] **S1.4** Process sysdeps: spawn/wait and exit.
- [ ] **S1.5** Produce `libc.a`, `libm.a` and pthreads.
- [ ] **S1.6** Test with a hello program and a threads program.

**S2 GCC target `x86_64-minimalos`**
- [ ] **S2.1** `config.gcc` entry.
- [ ] **S2.2** `gcc/config/minimalos.h`: LINK_SPEC, the `__minimalos__` define, gthr-posix through mlibc.
- [ ] **S2.3** libgcc t-files and the binutils alias to `elf_x86_64`.
- [ ] **S2.4** libstdc++ with `--disable-shared`.
- [ ] **S2.5** CMake toolchain file for the PC build.
- [ ] **S2.6** Cross-build a hello C++ program with it.

**S3 `msdk` rewritten in C**
- [ ] **S3.1** Same name and CLI as today's `MOSdesk/msdk` and `build.sh`.
- [ ] **S3.2** Compile driver for C/C++ with `-MD` dependency files.
- [ ] **S3.3** Incremental builds by content hash (xxh3 over the dependency list plus flags).
- [ ] **S3.4** Outputs: MINIRUN1 `.run`, `.slib`, `.mpkg` and Deliver packages (replacing `mkrun.sh`/`runbundle.py`).
- [ ] **S3.5** `msdk --freestanding` for old programs; hosted mode uses mlibc.
- [ ] **S3.6** Runs on the PC and on the OS.

**S4 Canadian cross**
- [ ] **S4.1** binutils hosted on Minimal-OS.
- [ ] **S4.2** GMP, MPFR and MPC built static.
- [ ] **S4.3** GCC with `cc1plus` using the configure flags above.
- [ ] **S4.4** libiberty `pex` through `posix_spawn`, no `-pipe`.
- [ ] **S4.5** libstdc++ hosted on the OS.
- [ ] **S4.6** Package as `msdk-native`, installed to `0:/sdk/`, built in CI.
- [ ] **S4.7** Test in a VM with 4 to 8 GiB of RAM.

**S5 FarcadeGL v3 + FGSL v2**
- [ ] Same work as OS-6.9; follows OS-6/OS-7.

---

## Part C: The engine (new repository)

### C1. Repository layout
**Current state:** the repo builds with a `Makefile` and one `mk/*.mk` fragment per part (no CMake yet). It has `engine/{core,ecs,gfx,script,physics,scene,audio,mof}`, `editor/`, `runtime/minimalos/`, `tools/`, `tests/` and `external/` (imgui, imguizmo, stb, cgltf, fast_obj, flecs, lua, dr_libs, the FGPU copy and the SDK submodule). `engine/mof` is a `.mof` model format with glTF and OBJ importers, which the layout below does not list. `engine/core` has only the math code so far.

**Target layout:**
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
- **Started (farcade-engine `engine/ecs`):**
  - flecs v4.1.6 behind `fe::World`, with its own single-threaded `os_api`; it runs in QEMU.
  - Components: modules (Rigidbody 3D, Collider 3D, listener, audio stream and sampler, light) and scene-object settings (camera, sky, sky light, post processing), with flecs meta filled from the same field tables as the editor.
  - The transform hierarchy runs as a system, and the "one current camera/sky/listener" rule is enforced in the world.
  - The editor's Play runs the scene as a world.
- Scenes are text for editing (diff-friendly) and cooked to binary. Prefabs use flecs prefabs.
- A world-partition grid of 32 m streaming cells feeds the proxy hierarchy (C4.6).

### C4. Rendering (FarcadeGL only)

#### C4.1 The `gfx` layer and feature levels
**Started (farcade-engine `engine/gfx`):**
- An FL0-style forward renderer on `farcadegl.h`: up to 8 lights, hemisphere ambient, a gradient sky with a sun, and post processing folded into the shaders. It renders offscreen and reads the frames back for the editor viewport.
- On the PC it runs on a host copy of the FGPU CPU backend (`external/fgpu`). In QEMU its smoke test gives the same checksum as the host.
- A GPU tier (Settings > Graphics) adds a sun shadow map with PCF, point and spot light shadows from a shadow atlas, GGX PBR, SSAO, bloom, FXAA and fog. It is on by default only when FGPU runs on a graphics card (FGPU_CAP_HW_3D) and can be forced on the CPU backend. Its QEMU smoke test also matches the host checksum.
- On the CPU backend the editor redraws only what changed. While the view moves it renders the viewport at a lower resolution with draft shaders, then refines in bands. In QEMU TCG a FarcadeGL orbit went from 3.4 s to about 0.2 s per frame.
- Mesh objects have a material in the editor's Inspector: roughness, metal, unlit and shadows. Roughness, metal and shadows show in the GPU tier only.
- Not started: CLOD, voxels, GI and the rest of C4.
- The host FGPU copy is pinned to Minimal-OS a00577e, so it lacks later kernel changes (read-only SSBOs). Update it when the kernel changes.

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
> **Status (open decision):** OS-4 measured about 8 M small triangles/s and 7 G lane-instructions/s on one 2.1 GHz core, and the CPU-backend targets are no longer pursued (the engine targets a real GPU). The tables below stay as the reference for what the CPU path would need. The FL1/FL2 frame budgets and the E5/E7 exit criteria assume that speed, so either re-baseline them against measured numbers or treat them as real-GPU goals (NVIDIA path, OS-7).

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

#### C4.8 Streaming: maps, objects and textures
One streaming system serves all three. It reads only cooked paks (never `.mof` or source files), on its own I/O thread with decode jobs, and nothing in it blocks a frame. Each pool has a fixed memory budget; what is loaded is decided by priority, and the lowest priority is evicted first.

- **Maps (the world):**
  - The world is cut into 32 m cells (C3). A cell holds its entity list (instances, lights, script references), collision and a navigation tile.
  - Cells load in a ring around the camera, widened in the direction of travel, with hysteresis so a player on a border does not make cells flap. States: unloaded, loading, active, asleep.
  - Things that must always exist (the player, game rules, the sky) live in an always-loaded layer.
  - Beyond the loaded ring the cell proxies of C4.4 are drawn, so an unloaded cell never disappears from view.
- **Objects:**
  - Every asset has a small record that is always in memory (bounds, material ids, its coarsest LOD or voxel root) and a payload that streams: CLOD pages (C4.2), voxel bricks (C4.3), collision meshes, animation clips, audio.
  - Pages are requested by projected error, so detail arrives where it is visible. The top levels stay resident, so there is always something to draw.
- **Textures:**
  - The cooker writes each texture as a mip chain: a small resident tail (64x64 and below, stored with the material) plus one chunk per larger mip.
  - What to load comes from the texel density on screen: computed on the CPU per cluster or instance at FL0, read from a feedback pass at FL1 and up.
  - **FL0 (today's GPU interface: no mips):** a texture streams in whole steps. Its surface is rebuilt at the next size (64, 256, 1024) and swapped when the upload is done.
  - **FL1 (OS-6: mips and trilinear):** mips stream one at a time, with the lowest loaded mip clamped and faded in.
  - **FL2 (stretch):** virtual textures with sparse pages.
- **Budgets (starting values):** geometry 256 MB on a desktop and 64 MB in a VM (C4.2); textures 512 MB and 96 MB; cell data 64 MB and 16 MB.
- **Pak layout for streaming:** chunks aligned to 64 KiB, each compressed on its own (zstd, LZ4 for the hot path), with neighbours in the world stored next to each other so one read brings several.
- **In the editor:** a streaming overlay (what is loaded, queued and evicted, per pool), and a "keep loaded" flag per asset.
- **What it needs from the OS:** threads on all cores (done), file reads that do not hold up other threads (a dedicated I/O thread is enough today), mips (OS-6), and later faster storage paths (OS-8).
- **When:** the pak reader and the asynchronous loader come with E1. Map and object streaming come with E5, FL0 texture steps with E5, and per-mip streaming with E7.

### C5. Scripting: Lua 5.4 and C++ in one project
- **How mixing works:** both languages register into the same flecs world, through the same reflection.
  - C++ components: `struct Health { float hp, max; }; FE_REFLECT(Health, (hp,f32), (max,f32));`. They appear automatically in Lua (`e:get(Health).hp`) and in the inspector.
  - Lua components: `fe.component("Velocity", { x="f32", y="f32", z="f32" })`. C++ reads them by name through a dynamic view, or through a generated typed header.
  - Systems in either language: Lua `fe.system("Move", {Position, Velocity}, fn)`, C++ `FE_SYSTEM(Move, Position&, const Velocity&)`, ordered in one pipeline.
  - Calls across: Lua calls C++ functions exported with `FE_FUNCTION`, C++ calls Lua with `fe::lua::call("player.on_hit", ...)`, and both share one event bus.
  - Easy mode: a `Script` component with `on_start/on_update/on_event` callbacks (Unity/Godot style). Capable mode: plain ECS systems.
  - **Scripts on objects (editor side done in farcade-engine, see its `docs/scripting.md`):**
    - An object holds several scripts, run in Inspector order.
    - Each script declares inputs in `-- @prop` comments, which the editor reads without Lua. Types: number (optionally with a range), text, toggle, list, object link.
    - Scripts on different objects reach each other through object links: `self.door:script("door")`, `obj:send(event, ...)`, `fe.broadcast`.
  - **Started (farcade-engine `engine/script`):**
    - Lua 5.4.7 runs scripts in Play: one sandboxed instance per script, inputs in `self`, object handles, module fields through the ECS (including `camera.current`), events and physics callbacks.
    - Errors are logged with `file:line` and a traceback.
    - Generational GC.
    - It runs in QEMU, with its own `setjmp`/`longjmp` for the OS runtime.
    - Not yet: the C++ game module, hot reload, the debugger, and time limits for runaway scripts.
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
**Started:** a built-in physics step (`engine/physics`) stands in for Jolt until the engine can meet Jolt's C++ standard library needs. It covers gravity, box, sphere, capsule, cylinder and convex-hull mesh colliders (GJK/EPA, contact manifolds of up to 4 points, sequential impulses with warm starting), rotation from contacts, friction, bounciness, layers, triggers and an upright-capsule character controller. Concave triangle meshes are not done.

**Vendored so far** (`external/VERSIONS.md`): Dear ImGui (docking), ImGuizmo, stb, cgltf, fast_obj, flecs 4.1.6, Lua 5.4.7, dr_libs and the FGPU copy. The other libraries in the table are not added yet.

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
**Started (farcade-engine `editor/`):**
- The shell, panels, gizmos and Play exist and are drawn by a software rasterizer (`SwRenderer`, the fallback described below), not yet through FarcadeGL with scissor. On Minimal-OS the editor runs full screen, and as a resizable "Farcade Editor" window under MOSdesk (changed regions go through `mos_window_present`; F11 toggles borderless fullscreen).
- Scenes save and open as text `.fscene` files. Module fields are written by name from the reflection tables, so older files load. The commands are File > Save scene (Ctrl+S), Save scene as, and double-click in Files.
- In QEMU, `farcade-editor --scene-test` saves, overwrites and reopens the demo scene. This needed a fix to the editor's MinimaFS file writer: create a `.part` file, write it with `SYS_FWRITE`, then rename it over the target.
- Undo and redo (Ctrl+Z, Ctrl+Y) keep whole-scene snapshots, taken when an edit settles; a drag counts as one step.
- New scene, Open and Quit ask before unsaved changes are lost.
- Scripts read and write the material through `obj.material`.
- Prefabs, the material graph and autosave are not started.

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

**The runtime.** There is one engine runtime, `libfe` (renderer, ECS, physics, audio, Lua, streaming). It is not installed separately on the player's machine: every exported game carries it inside its own `.run`. A Lua-only game is the prebuilt player (`feplayer`, which is `libfe` plus a `main`) next to the game's paks; a game with C++ code is linked with `libfe.a` into its own binary. The editor links the same library, which is what "Play" runs.

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
9. **The open intermittent kernel crash in the thread tests (OS-3).** E5 and E6 depend on multicore threads. Mitigation: diagnose it before building on OS-3, and keep `threads1cpu` as the fallback.
10. **Version drift between repos.** The SDK branch pinned by Minimal-OS and Farcade-Engine is ahead of SDK `main`, and the engine's FGPU copy is behind the kernel. Mitigation: merge the SSBO branch, and refresh `external/fgpu` whenever the kernel's FGPU changes.

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
The plan is saved as `docs/farcade-engine-plan.md` in Minimal-OS. The engine repository (fsminecrafter/Farcade-Engine) now exists and points to it; the plan can move there when you choose. OS-0 to OS-5 are done and E1 and E3 have started. Still open: the rest of E0 (allocator, jobs, profiler, reflection, VFS/pak), the toolchain (E2), the OS-3 crash and the SDK branch merge.
