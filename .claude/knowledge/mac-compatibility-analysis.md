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

## Notes

- Metal files are excluded from clang-format CI checks (`-not -path '*/Metal/*'`)
- macOS CI job uses `continue-on-error: true` until support stabilizes
- MoltenVK path avoids ~2,500 lines of Objective-C++ Metal implementation
- Metal RT is scaffold-only today — the trace pipelines, shaders, and
  acceleration-structure build/refit code are the next milestone.
  Capability detection, wiring, console surfacing, and HybridRT
  fallback are already in place.
