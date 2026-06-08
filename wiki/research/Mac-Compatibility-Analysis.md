# Mac Compatibility Analysis

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** macOS (Metal / MoltenVK / OpenGL), with cross-platform contrast

## Overview

Deep analysis of macOS support across every subsystem — graphics, audio, input,
networking, build infrastructure, and the Metal RHI backend including hardware ray
tracing. macOS started as an experimental, partially-stubbed path and is now buildable
end-to-end with a real Metal backend; CI still runs the macOS job as
`continue-on-error` until the platform fully stabilizes.

This page consolidates a long series of incremental sessions into the current state and
the remaining work.

## Subsystem Readiness

### Ready (no work needed)

- **Platform abstraction** — `Platform.h` detects `__APPLE__`, defines
  `SPARK_PLATFORM_MACOS`, identifies Apple Clang.
- **Audio** — `OpenALAudioEngine.h` compiles for macOS (guard `!SPARK_PLATFORM_WINDOWS`);
  CMake probes Homebrew `openal-soft` and falls back to system `OpenAL.framework`,
  defining `SPARK_OPENAL_AVAILABLE=1` so the engine pulls in real `AL/al.h`.
- **Networking** — POSIX sockets with `fcntl()` fallback in `UDPTransport.h`.
- **Physics** — Jolt is cross-platform; Metal compute optional via `xcrun`.
- **Module loading** — `dlopen`/`dlsym` fallback in `ModuleManager.cpp` for `.dylib`.
- **Crash handling** — POSIX signals + `backtrace()` with macOS guards
  (`sysctl`/`mach` system info).
- **Console process manager** — POSIX `fork`/`exec` with macOS guards.
- **Input** — `InputManager.cpp` now has Windows + Linux/macOS branches (SDL2 for
  capture). This was a critical gap in the original analysis; resolved.
- **Third-party deps** — all support macOS (Jolt, EnTT, ImGui, AngelScript, SDL2,
  GLAD, OpenAL, Recast).

### Remaining gaps / constraints

- **OpenGL on macOS** caps at GL 4.1, while the engine targets GL 4.6 — viable as a
  fallback only, not long-term. `GL_SILENCE_DEPRECATION=1` is defined on Apple to keep
  build logs clean.
- **Hardware ray tracing** requires macOS 12+ and a GPU reporting
  `supportsRaytracing`; otherwise the engine falls back to SDFGI.
- The Metal RT scene path is wired but several deeper items remain (see Metal RT below).

## Graphics: Metal Backend

The Metal backend is **implemented**, not header-only. `MetalDevice.mm` is a full
implementation (~70 KB on disk) alongside the `MetalDevice.h` interface. Supporting
Metal translation units exist:

- `MetalDevice.h` / `MetalDevice.mm` — device, swapchain, pipeline state.
- `MetalRayTracing.h` / `MetalRayTracing.mm` — hardware ray-tracing system.
- `MetalTextureReadback.h` / `MetalTextureReadback.mm` — CPU readback of MTLTextures.
- `MetalGoldenImageCapture.h` / `MetalGoldenImageCapture.mm` — golden-image capture
  (an `IGoldenImageCapture` subclass over the readback helper).

What the Metal device covers end-to-end:

- `MetalSwapChain` owns an `MTLCommandQueue`; `Present(vsync)` submits a command buffer
  that calls `presentDrawable:` — drawables actually display.
- `ConfigureMetalLayer` accepts an `NSView` (raw Cocoa or `SDL_Metal_CreateView`) or a
  `CAMetalLayer`, reusing an existing layer on the SDL path.
- `SetRenderTargets` binds up to 8 color attachments plus depth/stencil (MRT).
- `ClearDepthStencil` uses a clear-load render pass.
- Indirect draw/dispatch via Metal's indirect-buffer API.
- `CreatePipelineState` wires blend, depth/stencil, input layout (vertex descriptor),
  stencil format for combined D24S8/D32S8, and pipeline debug labels.
- `SetPipelineState` applies rasterizer state (cull, winding, fill, depth bias).
- Capabilities report `RayTracingBackend::HardwareMetalRT` with correct RT flags when
  the Apple GPU supports it.

### Window integration

macOS window/Metal wiring lives in a dedicated, cross-platform-safe pair:

- `Core/SparkEngineMacOS.h` — declares `Spark::MacOS::` helpers
  (`ShouldPreferMetal`, `GetMetalWindowFlag`, `CreateMetalView`, `DestroyMetalView`,
  `GetExecutableDirectory`). Safe to call on any platform — non-macOS links no-op stubs.
- `Core/SparkEngineMacOS.cpp` — real implementations under `SPARK_PLATFORM_MACOS`
  (`<SDL_metal.h>`, `<mach-o/dyld.h>`, `RHIBridge::GetRecommendedBackend()`); no-op
  stubs elsewhere.
- The POSIX entry point (`SparkEngineLinux.cpp`, "Linux + macOS") has **no** `#ifdef
  __APPLE__` branches — it calls the helpers.
- `SparkEngineMacOS.cpp` lives in `SparkEngineLib` so both the engine exe and the test
  runner see the symbols.

## Graphics: Metal Hardware Ray Tracing

`MetalRayTracingSystem` provides a working hardware RT path on capable Apple GPUs,
gated by `[device supportsRaytracing]` and `@available(macOS 12.0, *)`. It compiles a
4-kernel Metal library at runtime (`MTLLanguageVersion2_4`) for shadows, reflections,
ambient occlusion, and global illumination.

Capabilities:

- **BLAS** — real `CreateBLAS` / `UpdateBLAS` (refit) / `DestroyBLAS` using primitive
  acceleration-structure descriptors and one-shot command buffers.
- **TLAS** — real `BuildTLAS` from instance descriptors (4×3 transforms, mask,
  hit-group offset, BLAS index).
- **Kernels** — shadows reconstruct world position and trace visibility; reflections
  reflect off GBuffer normal with sky-gradient miss; AO uses cosine-weighted hemisphere
  sampling; GI does single-bounce diffuse. Shared MSL helpers `Hash12` (PRNG) and
  `CosineHemisphere`.
- **Materials** — `MaterialParams` (albedo, emissive, roughness/metallic; 48 bytes,
  16-byte aligned) uploaded as an MTLBuffer at slot 2; `ShadeHit` does Lambert +
  emissive. A consistent 7-slot uniform binding layout across all four kernels.
- **Frame I/O** — `FrameParams` (invViewProj, cameraPos, lightDir, resolution),
  `SetFrameParams` / `SetInputTextures` / `SetOutputTextures`, dispatched at 8×8
  threadgroups by `EncodeTracePass`.

### HybridRT integration and scene feeder

- `HybridRTManager` (macOS only) constructs a `MetalRayTracingSystem` when the detected
  backend is `HardwareMetalRT`; `Execute`'s `HardwareMetalRT` case rebuilds a dirty
  TLAS, fills `FrameParams`, dispatches the four kernels, then falls through to SDFGI so
  the compositor always receives data.
- Mesh-push API: `PushTriangleMesh(TriangleMeshDesc)` and a `MeshAsset` overload route
  to `CreateBLAS`; `ClearTriangleMeshes()` tears everything down. Off-macOS these are
  guard-only no-ops.
- `Graphics/HybridRT/RTSceneFeeder.{h,cpp}` walks the `Transform + MeshRenderer` ECS
  view (same order as `RenderSystem::Update`), resolves meshes via `AssetPipeline`, and
  pushes geometry; a companion populates per-instance materials in identical traversal
  order so `MaterialParams[]` index equals TLAS `instance_id`.
- `RTMaterialAdapter.h` converts `PBRProperties` to `MaterialParams`.

## Build & CI

- **CMake presets** — `macos-debug` / `macos-release` default to `ENABLE_METAL=ON`,
  `ENABLE_VULKAN=OFF`, `ENABLE_DXR=OFF`, `ENABLE_SDL2=ON`, `ENABLE_OPENGL=ON`.
  `macos-metal` adds Metal explicitly; `macos-moltenvk` opts into Vulkan via Homebrew
  MoltenVK. (Current preset set verified 2026-06-08: `macos-debug`, `macos-release`,
  `macos-metal`, `macos-moltenvk`.)
- **`CMAKE_OSX_DEPLOYMENT_TARGET=11.0`** (Big Sur) pinned when unset — first Apple
  Silicon release and the Metal 3 floor for M-series.
- **OBJCXX** — `enable_language(OBJCXX)` runs only behind `SPARK_METAL_AVAILABLE`; `.mm`
  globbing is gated the same way so an OpenGL-only macOS build configures cleanly.
- **Frameworks** — Cocoa, IOKit, CoreVideo, AudioToolbox, CoreAudio, CoreFoundation,
  plus Metal/Foundation for RT TUs.
- **X11** — guarded with `NOT APPLE` (Cocoa via SDL2; no X11 on macOS).
- **CI** — `build-macos` uses an `include:` matrix:
  `(Debug, OpenGL, metal=OFF)`, `(Release, OpenGL, metal=OFF)`,
  `(Release, Metal, metal=ON)`. The Metal row compiles `MetalDevice.mm` +
  `MetalRayTracing.mm` end-to-end on every PR. `brew install` adds
  `sdl2 openal-soft ccache` and best-effort `molten-vk`. Job is `continue-on-error`.
- Metal files are excluded from clang-format CI (`-not -path '*/Metal/*'`).

## Cross-platform render-target plumbing

To bring Linux/macOS toward Windows parity in the deferred path:

- `RHIBridge` has a render-target **registry**: `RenderTargetSlot` enum (4 GBuffer + depth
  + HDR), `RegisterRenderTarget` / `GetRenderTarget`, non-owning fixed-size array.
- `HybridRTBindings` uses raw pointers aliasing into an `owned` vector (Windows holds
  per-frame `WrapNativeTexture` wrappers; Linux/macOS read from the registry).
- Linux/macOS GBuffer + HDR targets are created and registered in
  `GraphicsEngineLinux.cpp` (`CreatePlatformRenderTargets` / `ReleasePlatformRenderTargets`),
  hooked into Initialize/Shutdown/Resize.
- `RenderDeferred` on Linux/macOS calls the shared `DispatchHybridRTPass`.

## Non-Windows draw path

`ProcessDrawList` is fully ported to non-Windows via the RHI bridge (no more
drain-without-render stub):

- `MeshAsset` carries `IRHIBuffer` vertex/index buffers, built by
  `AssetPipeline::BuildRHIBuffersForMesh` (Linux) post-load.
- `TextureAsset::Load` on Linux decodes via stb_image and uploads through the RHI
  bridge.
- `BindMesh` / `BindMaterial` / `DrawBoundMesh` issue real
  `SetVertexBuffer` / `SetIndexBuffer` / `SetShaderResource` / `DrawIndexed`.
- `ProcessDrawList` drains, sorts by (material, mesh), binds, and draws with profiler
  events.

## Test coverage

- `Tests/TestMacOSPlatform.cpp` — smoke tests for `Spark::MacOS::*` (run on all
  platforms; stub contract off-macOS, real behavior on macOS) plus HybridRT
  push/clear-without-init safety.
- `Tests/TestMetalRayTracing.cpp` — CPU-only RT tests (no MTLDevice): pre-init state,
  `MaterialParamsFromPBR`, struct layout/size, `TracePass` bitmask.
- `Tests/TestMetalRayTracingLive.mm` — the first `.mm` test file; live-`MTLDevice`
  tests via `MetalDevice::Initialize` (headless `MTLCreateSystemDefaultDevice`), gated
  by `if(APPLE AND SPARK_METAL_AVAILABLE)`. Includes BLAS/TLAS build, material
  round-trip, golden-capture readback. A `SKIP_IF_NO_RT` macro neutralizes runners
  lacking hardware RT.
- `Tests/TestProcessDrawListLinux.cpp` — RHI buffer/texture upload + draw-list drain on
  NullRHI.

## Key Files

- `SparkEngine/Source/Graphics/RHI/Metal/MetalDevice.{h,mm}` — Metal device/swapchain.
- `SparkEngine/Source/Graphics/RHI/Metal/MetalRayTracing.{h,mm}` — hardware RT.
- `SparkEngine/Source/Graphics/RHI/Metal/MetalTextureReadback.{h,mm}` — CPU readback.
- `SparkEngine/Source/Graphics/RHI/Metal/MetalGoldenImageCapture.{h,mm}` — golden capture.
- `SparkEngine/Source/Graphics/RHI/RHIFactory.cpp` — backend selection (Metal on `__APPLE__`).
- `SparkEngine/Source/Core/Platform.h` — platform/compiler detection.
- `SparkEngine/Source/Core/SparkEngineMacOS.{h,cpp}` — macOS window/Metal helpers.
- `SparkEngine/Source/Audio/OpenALAudioEngine.h` — cross-platform audio.
- `SparkEngine/Source/Input/InputManager.cpp` — Windows + Linux/macOS branches.
- `SparkEngine/Source/Graphics/HybridRT/RTSceneFeeder.{h,cpp}` — ECS RT feeder.

## Platform Requirements Summary

| Platform | Minimum | Recommended |
|----------|---------|-------------|
| **Windows** | Win10 x64, MSVC 19.36+, D3D11 FL 10.0 | Win11, MSVC v143/v144, D3D11 FL 11.1, D3D12+DXR |
| **Linux** | glibc 2.35+ x64, GCC 13+/Clang 17+, OpenGL 4.6 *or* Vulkan 1.3 | Ubuntu 24.04, GCC 14, Vulkan 1.3 |
| **macOS** | macOS 11 Big Sur, x64/ARM64, Metal 2.3 *or* OpenGL 4.1 | macOS 12+, Apple Silicon M2+, Metal 3 with `supportsRaytracing` |

Pinned by: `CMAKE_OSX_DEPLOYMENT_TARGET=11.0`, `cmake_minimum_required(3.25)`, the
`MetalRayTracing.mm` `@available(macOS 12.0, *)` gate for hardware RT. A public-facing
version of this matrix also lives at `wiki/platform/System-Requirements.md`.

## Remaining Work

- Reflection/AO/GI kernels could use deeper trace bodies and per-pass quality tuning.
- Per-draw constant-buffer upload (world/view/proj) on the RHI path — needs a
  shader-constant layout refactor with per-backend slot coordination.
- GPU particle system, FSR upscaling, and denoiser GPU paths on non-Windows — require
  per-backend compute shader ports.
- A CI test harness that actively routes RT output through `MetalGoldenImageCapture`
  needs a stable reference-image policy.

## Source & Freshness

- **Original entry date:** 2026-04-18 (consolidated from 11 incremental sessions in
  `.claude/knowledge/mac-compatibility-analysis.md`, dated 2026-04-17 through 2026-04-18).
- **Verified against codebase 2026-06-08.**

Updates / status changes since the original:

- **Metal backend** — confirmed implemented: `MetalDevice.mm` present (~70 KB). The
  original's earliest "critical gap" entry called it header-only; later sessions
  delivered it. Status: **Resolved.**
- **Metal companion TUs** — `MetalRayTracing`, `MetalTextureReadback`, and
  `MetalGoldenImageCapture` (.h + .mm) all present. Status: **Implemented.**
- **Input** — `InputManager.cpp` now has macOS/SDL branches (originally Win32-only).
  Status: **Resolved.**
- **CMake presets** — current set is `macos-debug`, `macos-release`, `macos-metal`,
  `macos-moltenvk`, matching the documented end-state.
- **OpenGL 4.1 cap** — still an open constraint (fallback only).
- **Hardware-RT remaining items** (deeper kernels, per-draw constants, GPU compute
  ports on non-Windows, golden-image CI harness) — still **Open**.
- The narrative was consolidated from 11 dated session logs into the current-state form;
  no factual claims were invented.

## Related Pages

- [Engine Viability Evaluation](Engine-Viability-Evaluation.md)
- [Third-Party Library Evaluation](Third-Party-Library-Evaluation.md)
- [System Requirements](../platform/System-Requirements.md)
- [Engine Feature Recommendations](Engine-Feature-Recommendations.md)
- [Project Recommendations](Project-Recommendations.md)
