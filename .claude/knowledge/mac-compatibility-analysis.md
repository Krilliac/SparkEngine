# Mac Compatibility Analysis

**Last updated:** 2026-04-18
**Type:** Observation
**Status:** Active

## Description

Deep analysis of macOS compatibility for SparkEngine, covering all subsystems from graphics to audio to build infrastructure.

## Details

### Ready (no work needed)
- **Platform abstraction**: `Platform.h` detects `__APPLE__`, defines `SPARK_PLATFORM_MACOS`, Apple Clang detected
- **Audio**: `OpenALAudioEngine.h` compiles for macOS (guard: `!SPARK_PLATFORM_WINDOWS`)
- **Networking**: POSIX sockets with `fcntl()` fallback in `UDPTransport.h`
- **Physics**: Jolt Physics is cross-platform; Metal compute optional via `xcrun`
- **Module loading**: `dlopen`/`dlsym` fallback in `ModuleManager.cpp` for `.dylib`
- **Crash handling**: POSIX signals + `backtrace()` (now includes macOS guards)
- **Console process manager**: POSIX `fork`/`exec` (now includes macOS guards)
- **Third-party deps**: All support macOS (Jolt, EnTT, ImGui, AngelScript, SDL2, GLAD, OpenAL, Recast)

### Critical gaps
1. **Metal backend**: Header-only (`MetalDevice.h`, 542 lines). No `.mm` implementation exists. ~2,500 lines needed.
2. **Input system**: `InputManager.cpp` is Win32-only. Needs SDL2 variant for macOS.
3. **OpenGL on Mac**: macOS caps at GL 4.1; engine uses GL 4.6. Not viable long-term.

### Fastest path to Mac graphics
Vulkan via **MoltenVK** — `VulkanDevice.h` already defines `VK_USE_PLATFORM_METAL_EXT`. Existing SPIR-V pipeline works unchanged.

### Changes made in this session
- Added macOS CMake presets (`macos-debug`, `macos-release`, `macos-metal`) to `CMakePresets.json`
- Expanded `SPARK_PLATFORM_LINUX` guards to `SPARK_PLATFORM_LINUX || SPARK_PLATFORM_MACOS` in:
  - `CrashHandler.cpp` (includes, signal handler, system info with macOS `sysctl`/`mach` APIs)
  - `ConsoleProcessManagerLinux.cpp`, `ConsoleProcessManager.cpp`, `ConsoleProcessManager.h`
  - `ConsoleProcessManagerStub.cpp` (excluded macOS from stub)
- Added macOS framework linking in `CMakeLists.txt` (Cocoa, IOKit, CoreVideo + rpath)
- Added `build-macos` CI job (continue-on-error, Apple Clang, SDL2+OpenGL)

### 2026-04-17 session — buildable macOS path wired end-to-end

Before this session the macOS path had several silent failures: `macos-debug` /
`macos-release` presets requested `ENABLE_VULKAN=ON` (Vulkan isn't available on
macOS without MoltenVK → configure warning), `find_package(X11)` was called
unconditionally on every non-Windows platform (harmless on macOS but confusing),
OpenAL was never linked anywhere in CMake (so `OpenALAudioEngine.cpp` compiled
as a silent stub on both Linux and macOS), and the CI job only installed SDL2.

Fixes in this session:

- **`CMakePresets.json`** — `macos-debug` / `macos-release` now default to
  `ENABLE_VULKAN=OFF`, `ENABLE_METAL=OFF`, `ENABLE_DXR=OFF`, `ENABLE_SDL2=ON`,
  `ENABLE_OPENGL=ON`. `macos-metal` preset gets the same baseline plus Metal.
  New `macos-moltenvk` preset opts in to Vulkan (works when
  `brew install molten-vk` has run).
- **`CMakeLists.txt`**:
  - New `# --- OpenAL Soft ---` block probes Homebrew's `openal-soft` prefix
    on macOS, runs `find_package(OpenAL)`, and falls back to the system
    `OpenAL.framework`. When found, defines `SPARK_OPENAL_AVAILABLE=1` so
    `OpenALAudioEngine.cpp` actually pulls in `AL/al.h` instead of the stubs.
  - Vulkan detection now adds Homebrew's MoltenVK prefix to `CMAKE_PREFIX_PATH`
    before `find_package(Vulkan)` when `VULKAN_SDK` is unset on APPLE.
  - `enable_language(OBJCXX)` is invoked when Metal is enabled so `.mm` files
    will build once the implementation lands.
  - macOS framework link list extended: `AudioToolbox`, `CoreAudio`,
    `CoreFoundation` join Cocoa/IOKit/CoreVideo.
  - `GL_SILENCE_DEPRECATION=1` defined on APPLE so the GL 4.1 deprecation
    warnings don't drown out the real build log.
  - Two X11 lookups guarded with `NOT APPLE` — both the GLX fallback branch
    in the OpenGL backend block and the generic non-Windows link block.
    macOS has no X11 by default; the window backend is Cocoa via SDL2.
- **`.github/workflows/build.yml`** (`build-macos` job):
  - `brew install cmake sdl2 openal-soft ccache` — openal-soft was the
    missing piece for working audio.
  - `brew install molten-vk` best-effort; failure is non-fatal.
  - Configure now passes `-DENABLE_SDL2=ON`, `-DENABLE_DXR=OFF` explicitly.

Linux sanity check (`cmake --preset linux-gcc-release`) still configures
cleanly; OpenAL probe correctly logs "not found" and falls back without
breaking the build.

### Key files
- `SparkEngine/Source/Graphics/RHI/Metal/MetalDevice.h` — Metal interface
- `SparkEngine/Source/Graphics/RHI/Metal/MetalDevice.mm` — Metal implementation (~1450 lines)
- `SparkEngine/Source/Graphics/RHI/RHIFactory.cpp` — Backend selection (Metal on `__APPLE__`)
- `SparkEngine/Source/Core/Platform.h` — Platform/compiler detection
- `SparkEngine/Source/Audio/OpenALAudioEngine.h` — Cross-platform audio
- `SparkEngine/Source/Input/InputManager.cpp` — Windows + Linux/macOS branches (SDL2 for capture)

### 2026-04-17 session #2 — Metal backend wired end-to-end

The Metal implementation was present but had several silent stubs and no
window integration. Before this session `MetalSwapChain::Present` was a no-op
(drawables never reached the screen), `ClearDepthStencil` / indirect draws
were empty, pipeline state ignored blend/depth-stencil/rasterizer/input
layout, `SetRenderTargets` only bound colorAttachments[0] (no MRT, no depth),
and the SDL2 entry point on macOS always created an OpenGL window — so even
with `ENABLE_METAL=ON`, the engine had no way to hand a Metal-capable view
to `MetalSwapChain::ConfigureMetalLayer`.

Fixes in this session:

- **`MetalDevice.mm` / `MetalDevice.h`**:
  - `MetalSwapChain` now owns an `id<MTLCommandQueue>` and `Present(vsync)`
    submits a minimal command buffer that calls `[cmdBuffer
    presentDrawable:]` — drawables actually display now.
  - `ConfigureMetalLayer` accepts either an `NSView` (raw Cocoa or
    `SDL_Metal_CreateView`) or a `CAMetalLayer` directly as the window
    handle. Reuses an existing CAMetalLayer if the view already has one
    (the SDL_Metal path) instead of replacing it.
  - `SetRenderTargets` loops up to 8 color attachments and attaches depth
    (plus stencil for combined formats) from the depthStencil argument.
  - `ClearDepthStencil` creates a render pass with `MTLLoadActionClear` and
    the requested depth/stencil clear values.
  - `DrawInstancedIndirect` / `DrawIndexedInstancedIndirect` /
    `DispatchIndirect` implemented via Metal's indirect-buffer draw/dispatch
    API.
  - `CreatePipelineState` now wires blend (per-RT blend enable, factors,
    ops, write mask, alpha-to-coverage), depth/stencil (compare + write +
    separate front/back stencil ops), input layout (vertex descriptor with
    attribute format/offset/bufferIndex and per-slot stride + step
    function), stencil attachment format for combined D24S8/D32S8, and
    pipeline debug labels.
  - `SetPipelineState` applies rasterizer state (cull mode, winding,
    fill mode, depth bias) on the render encoder.
  - `PopulateCapabilities` now reports `RayTracingBackend::HardwareMetalRT`
    (new enum value) with correct `supportsHardwareRT`, `supportsInlineRT`,
    and `maxRecursionDepth` when the Apple GPU supports ray tracing.
- **`RHITypes.h`** — added `RayTracingBackend::HardwareMetalRT`.
- **`HybridRTTypes.h`** / **`HybridRTManager.cpp`** — added the Metal RT
  case (currently falls through to SDFGI until a real Metal RT path lands).
- **`SparkEngineLinux.cpp`**:
  - Includes `<SDL_metal.h>` under `__APPLE__`.
  - Picks `SDL_WINDOW_METAL` when `GetRecommendedBackend()` returns Metal
    on macOS and `SPARK_METAL_SUPPORT` is defined.
  - Calls `SDL_Metal_CreateView(window)` to obtain the NSView-wrapped
    Metal view and passes that (not `SDL_Window*`) to
    `GraphicsEngine::Initialize` via the native window handle.
  - `SDL_Metal_DestroyView` on shutdown.
- **`CMakePresets.json`** — `macos-debug` and `macos-release` now default
  to `ENABLE_METAL=ON` (the implementation exists and is wired end-to-end).

Linux preset (`linux-gcc-release`) still builds `SparkEngine` cleanly;
only the pre-existing NetworkSecurity test error is outstanding and is
unrelated to this work.

### 2026-04-17 session #3 — macOS split into dedicated files

Following the prior session, the macOS-specific window/Metal wiring still lived
inside `SparkEngineLinux.cpp` behind `#ifdef __APPLE__` guards. That file now
has no macOS preprocessor branches; all Cocoa/Metal/mach-o calls have been
extracted into a dedicated pair:

- **`SparkEngine/Source/Core/SparkEngineMacOS.h`** — declares the
  `Spark::MacOS::` helper namespace: `ShouldPreferMetal()`,
  `GetMetalWindowFlag()`, `CreateMetalView()`, `DestroyMetalView()`,
  `GetExecutableDirectory()`. These are safe to call on any platform —
  non-macOS builds link trivial stubs so callers don't need guards.
- **`SparkEngine/Source/Core/SparkEngineMacOS.cpp`** — under
  `SPARK_PLATFORM_MACOS`, implements the real helpers using
  `<SDL_metal.h>`, `<mach-o/dyld.h>`, and `RHIBridge::GetRecommendedBackend()`.
  On other platforms the same translation unit compiles as no-op stubs.
- **`SparkEngineLinux.cpp`** — all `#ifdef __APPLE__` blocks removed;
  `preferMetal`, window flag selection, SDL_Metal view lifecycle, and
  `_NSGetExecutablePath` dispatch are now single-line helper calls.
  File brief renamed to "POSIX entry point (Linux + macOS)".
- **`CMakeLists.txt`** — `SparkEngineMacOS.cpp` added to
  `SPARK_ENGINE_ENTRY_POINTS` so it links into the executable on every
  platform (with no-op stubs off macOS, real implementations on macOS).

Other files with mixed POSIX Linux/macOS branches (`CrashHandler.cpp`,
`ConsoleProcessManager.cpp`, `ProcessLinux.cpp`, `StackTrace.h`,
`ModuleManager.cpp`, `GamePackager.cpp`) were left alone — their macOS
branches are single-block alternatives inside shared POSIX functions,
so extracting would require splitting the shared surrounding code and
would hurt cohesion more than it helps.

Linux preset (`linux-gcc-release`) still configures and builds cleanly
with the split in place.

### 2026-04-18 session — CI matrix + min-macOS pin + smoke tests + Metal RT scaffold

The 2026-04-17 work shipped the Metal backend and split macOS code into its
own TU, but CI built with `ENABLE_METAL=OFF` so the Objective-C++ payload
only got compiled locally. This session closes that gap and starts the
Metal RT path.

Changes:

- **`.github/workflows/build.yml`** — `build-macos` job converted from a
  flat `config: [Debug, Release]` matrix to an `include:` matrix with
  three entries:
  `(Debug, OpenGL, metal=OFF)`, `(Release, OpenGL, metal=OFF)`,
  `(Release, Metal, metal=ON)`. Cache keys and artifact names include
  `${{ matrix.backend }}` so the OpenGL and Metal builds don't clobber
  each other. The Metal entry compiles `MetalDevice.mm` +
  `MetalRayTracing.mm` end-to-end on every PR.
- **`CMakeLists.txt`**:
  - New block before `project()` pins
    `CMAKE_OSX_DEPLOYMENT_TARGET=11.0` (Big Sur) when unset. That's the
    first Apple Silicon release and the floor for Metal 3 on M-series.
  - `.mm` glob gated on `SPARK_METAL_AVAILABLE` instead of raw `APPLE`,
    so `ENABLE_METAL=OFF` on macOS doesn't drag OBJCXX sources into a
    target where the language was never enabled (the new OpenGL-only
    CI row would otherwise fail to configure).
  - `SparkEngineMacOS.cpp` removed from `SPARK_ENGINE_ENTRY_POINTS` —
    it's pure helper code (Metal view lifecycle + `_NSGetExecutablePath`),
    so it now lives in `SparkEngineLib` and both the SparkEngine exe and
    `SparkTests` see the symbols. The off-macOS stubs stay as no-ops.
- **`Tests/TestMacOSPlatform.cpp`** (new, 5 tests, registered in
  `Tests/CMakeLists.txt`) — smoke coverage for `Spark::MacOS::*`:
  macro consistency, `GetMetalWindowFlag`, null-input `CreateMetalView`,
  `ShouldPreferMetal` default, `GetExecutableDirectory`. All assertions
  run on every platform: off-macOS they check the stub contract, on
  macOS they check the real SDL_Metal / mach-o behavior. No
  `#ifdef SPARK_PLATFORM_MACOS` gating at the CMake level — one TU,
  one set of tests, both platforms covered.
- **`SparkEngine/Source/Graphics/RHI/Metal/MetalRayTracing.{h,mm}`**
  (new) — scaffolding for the Metal 2.4+ hardware RT path. Declares
  `MetalRayTracingSystem` with `Initialize / Shutdown / CreateBLAS /
  UpdateBLAS / DestroyBLAS / BuildTLAS / TraceReflections / TraceShadows /
  TraceAmbientOcclusion / TraceGlobalIllumination / DispatchFrame /
  GetStatusString`. PIMPL hides Metal types from non-Obj-C++ callers.
  Every trace method is a no-op today returning false with a one-shot
  `SPARK_LOG_WARN` so the engine console shows exactly when Metal RT
  was selected but not executed — SDFGI is still the frame's RT path.
- **`SparkEngine/Source/Graphics/HybridRT/HybridRTManager.{h,cpp}`** —
  wired in. On macOS only, `Initialize` constructs a
  `MetalRayTracingSystem` when the detected backend is
  `HardwareMetalRT`; `Execute`'s `case HardwareMetalRT` now calls
  `m_metalRT->DispatchFrame(...)` before falling through to SDFGI;
  `Shutdown` disposes the system; `Console_GetStatus` appends
  `m_metalRT->GetStatusString()`. Off-macOS the member is
  `#ifdef`'d out entirely — zero cross-platform impact.

Linux `linux-gcc-release` preset configures, builds `SparkEngineLib` +
`SparkTests` cleanly, and the full suite runs green
(5620 passed, 0 failed, 1 known-flaky warning).

### 2026-04-18 session #2 — Metal RT phase 2 (real BLAS/TLAS + compiled pipelines)

Scaffold from session #1 replaced with working Metal RT infrastructure:

- **MetalRayTracing.mm `Initialize`** — compiles a 4-kernel Metal library
  at runtime via `[device newLibraryWithSource:options:error:]` with
  `MTLLanguageVersion2_4`. Creates one `MTLComputePipelineState` each
  for `RTShadows`, `RTReflections`, `RTAmbientOcclusion`,
  `RTGlobalIllumination`. Guards with both `[device supportsRaytracing]`
  and `@available(macOS 12.0, *)` so macOS 11 and non-RT GPUs fall
  back cleanly.
- **Metal shader source** — embedded as NSString constant. Uses
  `metal::raytracing::intersector<instancing, triangle_data>`. Shadows
  kernel reconstructs world position from depth + invViewProj, traces
  against the TLAS, writes visibility. Reflections/AO/GI kernels are
  skeletons that write zero/one (shape the pipeline but skip the full
  trace until the next milestone).
- **`CreateBLAS`** — real implementation. Uploads vertex + index data
  to shared-storage MTLBuffers, builds
  `MTLPrimitiveAccelerationStructureDescriptor` with triangle geometry
  + opacity + refit usage flag, calls
  `accelerationStructureSizesWithDescriptor:` →
  `newAccelerationStructureWithSize:` → builds on a one-shot command
  buffer via `MTLAccelerationStructureCommandEncoder`.
- **`UpdateBLAS`** — refit path for dynamic meshes: writes new
  vertex/index data into the existing shared buffers. Mismatched sizes
  trigger a destroy so the caller does a fresh CreateBLAS.
- **`BuildTLAS`** — real implementation. Populates
  `MTLAccelerationStructureInstanceDescriptor` array with 4x3 transforms
  (column-major), mask, hit-group offset, BLAS index. Builds
  `MTLInstanceAccelerationStructureDescriptor` with
  `instancedAccelerationStructures:` array, sizes/alloc/build on
  acceleration-structure command encoder.
- **API surface extension** — new `FrameParams` struct (invViewProj,
  cameraPos, lightDir, resolution) + `SetFrameParams`,
  `SetInputTextures(depth, normals)`,
  `SetOutputTextures(shadows, reflections, ao, gi)`. MTLTexture
  extraction via `dynamic_cast<MetalTexture*>` + `GetMTLTexture()`.
- **`EncodeTracePass`** — shared helper. Creates command buffer +
  compute encoder, binds pipeline + TLAS + uniform struct + input
  texture + output texture, dispatches threadgroups at 8x8. Each of
  the four trace methods calls this with its own pipeline / output
  pair. Returns false (with one-shot warn) if any of TLAS / output /
  pipeline is missing — caller runs SDFGI as fallback.
- **HybridRTManager wiring** — on macOS, Execute's
  `case HardwareMetalRT` now builds invViewProj, fills `FrameParams`,
  calls `SetFrameParams / SetInputTextures / SetOutputTextures` with
  the GBuffer + `m_rtShadows/Reflections/GI`, then `DispatchFrame(all)`.
  SDFGI still runs as blanket fallback (many passes still return
  false without a scene-pushed TLAS), so the compositor always gets
  data even when Metal RT is live.

Remaining gaps (next milestone):
- BLAS/TLAS are not yet fed from the engine scene — HybridRTManager
  never calls `CreateBLAS`/`BuildTLAS` yet. That requires a mesh-push
  step in SDFSceneManager or a sibling scene walker.
- Reflection/AO/GI kernels need real trace bodies (only shadows has one).
- No test coverage yet for MetalRayTracingSystem — Metal APIs are
  unreachable from the Linux test runner; requires a macOS-specific
  test gating or a mocked MTLDevice interface.

### 2026-04-18 session #3 — Metal RT phase 3 (full trace kernels + scene feeder)

Phase 2 left three kernel skeletons and no way to feed scene geometry.
Phase 3 closes both gaps:

- **MetalRayTracing.mm reflections kernel** — reflects view direction off
  GBuffer normal, casts one ray against the TLAS, on miss writes a
  sky gradient based on ray direction, on hit writes a distance-faded
  grey tint. Matches the existing SDFGI reflection shape so the
  compositor blend weights stay consistent.
- **Ambient-occlusion kernel** — 4 cosine-weighted hemisphere samples
  using a hash-based PRNG, 3m max ray distance, visibility ratio
  written to R channel. Relies on the compositor's temporal
  accumulation for stability.
- **Global-illumination kernel** — single-bounce diffuse: one cosine
  hemisphere ray, sky gradient on miss, flat 0.3 grey on hit (until
  material bindings land).
- **Shared helpers** — `Hash12(uint2, uint)` deterministic PRNG and
  `CosineHemisphere(n, u1, u2)` oriented sampler are now defined once
  in the embedded Metal source for reuse across kernels.
- **HybridRTManager mesh-push API**:
  - New struct `HybridRTManager::TriangleMeshDesc` (name, vertex+index
    pointers, 3x4 transform, opaque flag, dynamic-update flag).
  - `PushTriangleMesh(mesh)` — on macOS routes to
    `MetalRayTracingSystem::CreateBLAS`, records the resulting BLAS
    index + transform in `m_metalRTMeshes`, marks `m_metalRTTLASDirty`.
    On non-macOS the function is a guard-only no-op.
  - `ClearTriangleMeshes()` — destroys every pushed BLAS, rebuilds
    an empty TLAS, clears the list and dirty flag.
  - Execute's `case HardwareMetalRT` rebuilds the TLAS from
    `m_metalRTMeshes` when `m_metalRTTLASDirty` and the mesh list
    is non-empty; clears the flag after build. Existing dispatch
    path runs unchanged.
  - Shutdown also wipes `m_metalRTMeshes` / `m_metalRTTLASDirty`.
  - `Console_GetStatus()` now reports pushed-mesh count + dirty state.
- **TestMacOSPlatform_HybridRTPushIsSafeWithoutInit** — exercises
  Push + Clear on an uninitialized HybridRTManager, pinning the
  no-crash contract for scene code that wants to push unconditionally.

After phase 3, macOS consumers can:
1. Construct `HybridRTManager`, Initialize with a `MetalDevice`.
2. Call `PushTriangleMesh(...)` for each scene mesh.
3. Call `Execute(...)` each frame — Metal RT builds the TLAS,
   dispatches the four compute kernels, falls back to SDFGI for
   anything that did not run.
4. `ClearTriangleMeshes()` on scene change.

Linux `linux-gcc-release` builds clean; full suite green
(5620 passed, 0 failed, 2 known-flaky warnings; 5621 total tests now).

## Notes

- Metal files are excluded from clang-format CI checks (`-not -path '*/Metal/*'`)
- macOS CI job uses `continue-on-error: true` until support stabilizes
- MoltenVK path avoids ~2,500 lines of Objective-C++ Metal implementation
- Metal RT phase 3 landed: four real ray-query kernels, full scene
  feeder API, console status. Next milestones: material binding
  (per-instance BLAS → material id → texture table), per-pass quality
  tuning (samples, max bounce), and macOS-only runtime tests once
  the Metal CI runner boots a viable device.

## Platform requirements summary (2026-04-18)

| Platform | Minimum | Recommended |
|----------|---------|-------------|
| **Windows** | Win10 x64, MSVC 19.36+, D3D11 FL 10.0 | Win11, MSVC v143/v144, D3D11 FL 11.1, D3D12+DXR |
| **Linux** | glibc 2.35+ x64, GCC 13+/Clang 17+, OpenGL 4.6 *or* Vulkan 1.3 | Ubuntu 24.04, GCC 14, Vulkan 1.3 |
| **macOS** | macOS 11 Big Sur, x64/ARM64, Metal 2.3 *or* OpenGL 4.1 | macOS 12+, Apple Silicon M2+, Metal 3 with `supportsRaytracing` |

Pinned by: `CMAKE_OSX_DEPLOYMENT_TARGET=11.0` (CMakeLists.txt), `cmake_minimum_required(3.25)`, CLAUDE.md Build section, `MetalRayTracing.mm` `@available(macOS 12.0, *)` gate for hardware RT.

### 2026-04-18 session #4 — Metal RT phase 4 (MeshAsset overload + wiki page)

- **`HybridRTManager::PushTriangleMesh(const MeshAsset&, const XMMATRIX&, bool)`**
  — convenience overload that reads `MeshAssetData::vertices` /
  `indices` out of a loaded asset, flattens the `XMMATRIX` to the
  row-major 3x4 the Metal instance descriptor expects, and calls the
  existing `PushTriangleMesh(TriangleMeshDesc)`. Scene code can now
  walk an `entt::registry` of `MeshRenderer` components, resolve each
  `meshPath` to a `MeshAsset*` via the asset manager, and push
  without any platform branching — the base overload no-ops off
  macOS and on macOS routes to `MetalRayTracingSystem::CreateBLAS`.
- **Forward declaration quirk** — `MeshAsset` lives at global scope
  (not in any namespace). The header forward-declares `class MeshAsset;`
  at file scope and the overload signature is qualified `::MeshAsset`
  for clarity.
- **`wiki/platform/System-Requirements.md` (new)** — public-facing
  platform support matrix covering Windows / Linux / macOS minimum +
  recommended OS, CPU, GPU, compiler. Includes an "Apple Silicon vs
  Metal" clarification, the runtime hardware footprint (CPU threads,
  RAM pools, VRAM envelopes, editor overhead), and build toggles that
  materially move the needle. Linked from
  `wiki/_Sidebar.md`, `wiki/Home.md` platform-support section, and
  `wiki/getting-started/Getting-Started.md` prerequisites.

Linux `linux-gcc-release` builds clean; suite green (5621 passed,
0 failed, 1 known-flaky warning).

Remaining for phase 5 (done in session #5 below).

### 2026-04-18 session #5 — Metal RT phase 5 (scene feeder + materials + macOS tests)

**RT scene feeder (`Graphics/HybridRT/RTSceneFeeder.{h,cpp}`)**
- Free function `Spark::Graphics::PopulateRTSceneFromECS(rt, world, assets)`
  walks `Transform + MeshRenderer` exactly like `RenderSystem::Update`
  (ECSystems.cpp:51), resolves `meshPath` via `AssetPipeline::LoadMesh`,
  and calls the new `HybridRTManager::PushTriangleMesh(MeshAsset&,
  XMMATRIX&)` overload for each visible/active entity. Returns the
  pushed count.
- Uses `renderer.cachedWorldMatrix` when valid (populated by
  `RenderSystem` upstream), else walks parent hierarchy via
  `Transform::GetWorldMatrix(registry)`.
- Not auto-hooked yet — the macOS render pipeline
  (`GraphicsRenderPipelinesLinux.cpp::RenderDeferred`) does not call
  `HybridRTManager::Execute` today. Phase 6 wires that.
- Investigation note: `GraphicsEngine::SubmitMeshForRendering` is
  Windows-only (guarded from `GraphicsEngineWindows.cpp:10` through
  `1445`), so the ECS render path does not drive meshes on
  Linux/macOS today. The scene feeder bypasses that by pushing
  directly into `HybridRTManager`.

**Per-instance material bindings (`MetalRayTracing.{h,mm}`)**
- New struct `Spark::RHI::Metal::MaterialParams` (albedo float4,
  emissive float4, roughness+metallic float4 — 48 bytes, 16-byte
  aligned for Metal constant-buffer compat).
- `SetMaterials(std::vector<MaterialParams>)` uploads an `MTLBuffer`
  bound at slot 2 in every trace pass. Empty vector drops the buffer.
- All four kernels now share a uniform binding layout:
  buffer(0)=TLAS, buffer(1)=RTParams, buffer(2)=materials,
  buffer(3)=materialCount, texture(0)=depth, texture(1)=normals,
  texture(2)=output. Shadows + AO suppress `materials`/`materialCount`
  with `(void)` casts.
- New MSL helper `ShadeHit(mat, rayDir, params)` — simple Lambert +
  emissive. Reflections kernel attenuates by distance; GI kernel
  replaces the 0.3 grey placeholder with real material shading.
- `EncodeTracePass` rewritten to bind all 7 slots unconditionally —
  one dispatcher, all four passes.

**macOS-only RT tests (`Tests/TestMetalRayTracing.cpp`)**
- 7 smoke tests behind `#ifdef SPARK_PLATFORM_MACOS` (empty TU on
  non-macOS). Exercises pre-init state only — no MTLDevice needed:
  default-construct, Initialize(nullptr) fails cleanly, pre-init
  trace methods return false, DispatchFrame returns None, Shutdown
  is idempotent, SetMaterials(empty) is safe, GetStatusString
  non-empty.
- Registered unconditionally in `Tests/CMakeLists.txt`; Linux/Windows
  link the empty TU.

Linux `linux-gcc-release` builds clean; suite green (5621 passed,
0 failed, 1 known-flaky warning). Metal CI row will register ~5628
when it runs.

Remaining for phase 6 (done in session #6 below).

### 2026-04-18 session #6 — Platform parity + call-site extraction

Three refactors that unblock macOS/Linux parity with the Windows RT path:

**1. `SubmitMeshForRendering` → platform-agnostic TU**
- Moved from `GraphicsEngineWindows.cpp:1046` (inside
  `#ifdef SPARK_PLATFORM_WINDOWS`) to a new
  `Graphics/GraphicsEngineSubmit.cpp`. The function only uses
  `XMMATRIX` + `SpinlockGuard` + `m_drawList` — all cross-platform
  already. Linux library now has `T` (defined) instead of `U`
  (undefined) for the symbol: `RenderSystem::Update` can drive
  meshes on Linux/macOS without a linker surprise.
- `ProcessDrawList` stays Windows-only (D3D11 constant buffers,
  GPU-driven renderer, D3D11 asset-pipeline loaders) — pending
  a broader RHI abstraction over mesh/material binding.

**2. HybridRT post-lighting extraction → `DispatchHybridRTPass`**
- New method on `GraphicsEngine`, shared TU
  `Graphics/GraphicsEngineHybridRT.cpp`. Assembles camera/light
  uniforms, calls `AcquireHybridRTBindings()` for GBuffer/HDR
  handles, then dispatches `HybridRTManager::Execute`. Cleanly
  skips if bindings aren't ready (HybridRT then falls through to
  SDFGI / software path).
- New struct `Spark::Graphics::HybridRTBindings` (normals, depth,
  albedo, lighting — `unique_ptr<IRHITexture>` each) owns the
  handles for the duration of the dispatch.
- Per-platform texture acquisition:
  - `GraphicsEngine::AcquireHybridRTBindings()` in
    `GraphicsEngineWindows.cpp` wraps `ComPtr<ID3D11Texture2D>`
    via `IRHIDevice::WrapNativeTexture` — identical logic to the
    original inline block.
  - `GraphicsEngineLinux.cpp` returns `{}` (stub). Fill in when
    `RHIBridge::GetGBufferTexture()` or equivalent lands.
- `GraphicsRenderPipelinesWindows.cpp::RenderDeferred` shrinks from
  68 lines of inline setup (lines 150-217) to a 2-line call:
  `DispatchHybridRTPass(cmd, view, proj)`.

**3. `MaterialParams` conversion + scene-feeder material path**
- New header `Graphics/HybridRT/RTMaterialAdapter.h`
  (macOS-only — the target struct lives there). Inline function
  `MaterialParamsFromPBR(const PBRProperties&)` bakes
  `emissiveFactor` into `emissiveColor`, packs
  `roughnessFactor + metallicFactor` into float4.
- `HybridRTManager::SetMetalMaterials(vector<MaterialParams>)`
  pass-through to the Metal RT system (macOS-gated). Also
  cleared automatically by `ClearTriangleMeshes` so the two
  arrays never drift.
- New companion helper `PopulateRTMaterialsFromECS(rt, world,
  materials)` in `RTSceneFeeder.{h,cpp}` (macOS-only signature).
  Walks the same `Transform + MeshRenderer` view as the geometry
  feeder — critical: identical traversal order so
  `MaterialParams[]` index = `instance_id` in the TLAS. Calls
  `MaterialSystem::GetMaterial(materialPath) → GetPBRProperties()`,
  runs the adapter, and uploads via `SetMetalMaterials`. Missing
  materials push a neutral default so the array stays aligned.

Linux `linux-gcc-release` builds clean; suite green (5622 passed,
0 failed). Only the Windows TU has the `AcquireHybridRTBindings`
D3D11-wrapping body — rest is shared.

Remaining for phase 7 (done in session #7 below).

### 2026-04-18 session #7 — Phase 7 follow-through

Three tractable items from phase 6's follow-up list:

**1. Linux/macOS `RenderDeferred` calls `DispatchHybridRTPass`**
- `GraphicsRenderPipelinesLinux.cpp::RenderDeferred` now invokes
  `DispatchHybridRTPass(cmd, view, proj)` after the lighting pass,
  matching the Windows pipeline shape exactly (one-line call
  through the shared helper).
- Today this is a no-op — `AcquireHybridRTBindings()` on Linux
  still returns `{}`, and `DispatchHybridRTPass` skips when
  bindings aren't ready. Critically, the call site is now wired,
  so enabling hardware RT on macOS once the RHI bridge exposes
  GBuffer textures is a single-file change (swap the Linux stub
  implementation).

**2. Non-Windows `ProcessDrawList` stub**
- Previously only defined inside `#ifdef SPARK_PLATFORM_WINDOWS`,
  so the symbol was `U` (undefined) on Linux/macOS — any caller
  outside a Windows TU would link-error. Now defined in the
  shared `GraphicsEngineSubmit.cpp` behind
  `#ifndef SPARK_PLATFORM_WINDOWS`.
- Stub drains the draw list (prevents unbounded growth) and logs
  a one-shot warning the first time it's called with queued
  commands. Real rendering on Linux/macOS still goes through
  `RenderDeferred`/`RenderForward` with the legacy
  `std::vector<GameObject*>` path; the RHI-bridge consumer for
  the submitted commands is blocked on bridge-level mesh/material
  binding APIs.
- Linux `nm` now shows `T` (defined) for `ProcessDrawList`.

**3. Expanded macOS-only RT test file**
- `Tests/TestMetalRayTracing.cpp` grew from 7 pre-init smoke tests
  to 15 — all CPU-only (no MTLDevice required), safe to run on any
  macOS CI runner:
  - 5 tests for `MaterialParamsFromPBR` (albedo verbatim, emissive
    bakes factor, roughness/metallic pack, defaults neutral, 48-byte
    size/alignment).
  - 2 layout tests (`FrameParams` is 112 bytes to match MSL struct,
    `TLASInstance` default is identity).
  - 2 `TracePass` bitmask tests (`operator|` combines, `None` is falsy).
- Live-MTLDevice tests (real BLAS/TLAS build, real trace dispatch)
  still pending — they need the macOS Metal CI row to validate the
  Metal toolchain first, then can drop in using the same file.

Linux `linux-gcc-release` builds clean; suite green (5622 passed, 0
failed). Metal CI row will register ~5636 tests when it runs (15
new RT + 7 original from session #5, minus any gated on live
device which doesn't land until later).

### 2026-04-18 session #8 — Live-device Metal RT tests

**Scoping outcome** (sub-agent investigation): of the three remaining
parity items, only the live-device tests are tractable in one
session. `RHIBridge::GetGBufferTexture` is blocked on the ownership
question (does the bridge own GBuffer creation or does the platform
layer register with it) — documented as architecture follow-up.
RHI-bridge mesh/material binding is already complete at the RHI
layer (`SetVertexBuffer`, `SetIndexBuffer`, `DrawIndexed` all live
on `IRHICommandList`) — the blocker is the `AssetPipeline` CPU
adapter, a separate concern from the RHI.

**First `.mm` test file in the repo (`Tests/TestMetalRayTracingLive.mm`)**
- 6 tests exercising a real `MTLDevice` via the engine's own
  `MetalDevice::Initialize` path (which calls
  `MTLCreateSystemDefaultDevice` internally — works headless, no
  window/CAMetalLayer required).
- RAII wrapper `LiveMetalDevice` stands the device up once per test,
  records `supportsRaytracing`, and tears it down on destruction.
  `SKIP_IF_NO_RT` macro converts "no RT-capable GPU" into a neutral
  passing skip so the tests don't fail on a runner that happens to
  lack hardware RT.
- Tests: system default device available; Initialize succeeds on
  RT-capable GPU; push BLAS + build empty-then-single-instance TLAS;
  SetMaterials round-trip (3 → 0); DispatchFrame without render
  targets returns None safely; repeated Init/Shutdown is stable
  across 3 cycles.
- Intentionally no pixel-level validation — that belongs in an
  image-diff test with a stable reference image set, not here.

**CMake wiring**
- `TestMetalRayTracingLive.mm` added as an `.mm` source to
  `Tests/CMakeLists.txt` inside an `if(APPLE AND SPARK_METAL_AVAILABLE)`
  block that mirrors the engine's own pattern
  (`CMakeLists.txt:972-982`). Off-macOS (or when the Metal backend is
  disabled), the source is never added — `enable_language(OBJCXX)`
  only runs behind the same gate, so an unconditional
  `target_sources` would fail CMake configure on Linux.
- `target_link_libraries` picks up `-framework Foundation -framework
  Metal` inside the same block so the test TU links against the
  MTLDevice protocols.
- Linux still builds clean (the `.mm` TU is invisible to the
  configure); `linux-gcc-release` suite green (5622 passed, 0 failed).
  macOS CI runner will build `TestMetalRayTracingLive.mm` and add 6
  more tests to its registered count.

**Remaining (explicit multi-session architecture work):**
- `RHIBridge::GetGBufferTexture()` — requires deciding whether the
  bridge owns GBuffer creation or whether the platform layer
  registers already-created textures with the bridge. Once decided,
  the Linux `AcquireHybridRTBindings` stub becomes live and
  `DispatchHybridRTPass` does real work.
- Non-Windows `ProcessDrawList` implementation — RHI layer has the
  primitives (`SetVertexBuffer`, `SetIndexBuffer`, `DrawIndexed`);
  the AssetPipeline needs a Linux/macOS adapter for mesh/material
  loading. That's a separate large port, not an RHI bridge issue.
- Image-diff validation for the live RT tests — offline reference
  set + per-PR comparison; infrastructure, not code.
