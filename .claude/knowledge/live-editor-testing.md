# Live Editor Testing on Linux (Software Rendering)

**Last updated:** 2026-04-15
**Type:** Pattern
**Status:** Active

## 2026-04-15 update (part 2) — Vulkan SIGSEGV root cause + real fix

Follow-up on the `SPARK_DISABLE_VULKAN` escape hatch committed earlier
today. Running the engine under `gdb` gave a concrete backtrace:

```
Thread 1 (crashed):
#0 libvulkan.so.1 (??)
#1 VulkanSwapChain::CreateSwapChain    (VulkanCommandList.cpp:53)
#2 VulkanDevice::CreateSwapChain       (VulkanDevice.cpp:971)
#3 RHIBridge::Initialize               (RHIBridge.cpp:311)
#4 GraphicsEngine::Initialize          (GraphicsEngineLinux.cpp:91)
#5 main                                (SparkEngineLinux.cpp:683)
```

The crash is **not** inside `VulkanDevice::Initialize` as I originally
thought. `VulkanDevice::Initialize` completes successfully — instance,
physical device, logical device, queues, command pool, etc. all come
up fine. The SIGSEGV is on the very first line of
`VulkanSwapChain::CreateSwapChain`:

```cpp
VkSurfaceKHR m_surface = VK_NULL_HANDLE; // never set on Linux!
...
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, m_surface, &capabilities);
```

…and the reason is simple: `VulkanDevice::CreateSwapChain` only had a
`#ifdef _WIN32` surface-creation branch. On Linux there was no surface
creation path at all, so `m_surface` stayed `VK_NULL_HANDLE`, and
`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` promptly dereferenced a
null surface and died. The Mesa Lavapipe ICD is blameless.

### The real fix (three parts)

1. **`VulkanDevice.h`** — enable XCB + Xlib + Wayland surface macros on
   Linux so the Vulkan header pulls in all three sets of surface
   extension names. Also `#undef` Xlib's unqualified macros (`None`,
   `Status`, `Success`, `Bool`, `True`, `False`, `Always`) that
   otherwise poison the rest of the engine (`RHICullMode::None` and
   friends stop compiling).

2. **`VulkanDevice.cpp`** — enable the matching instance extensions
   (`VK_KHR_xcb_surface`, `VK_KHR_xlib_surface`, `VK_KHR_wayland_surface`)
   when the ICD advertises them, and add an `#elif defined(SPARK_SDL2_AVAILABLE)`
   branch to `CreateSwapChain` that calls `SDL_Vulkan_CreateSurface(sdlWindow,
   m_instance, &surface)`. We also bail early with a `nullptr` return when
   `desc.windowHandle` is null so the downstream
   `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` can never be called with
   `VK_NULL_HANDLE`.

3. **`RHIBridge.cpp`** — fold swap-chain creation into the backend
   fallback loop. Previously, if `Initialize()` succeeded but
   `CreateSwapChain()` returned nullptr, the whole `RHIBridge::Initialize`
   bailed with `return false` — no retry with a different backend.
   Now a swap-chain failure logs `Backend 'X' failed to create swap
   chain — trying next`, tears the device down, and loops to the next
   candidate. This is what makes OpenGL actually get tried when Vulkan
   can't make a surface.

### Observed behavior now (default, no env vars)

```
RHIBridge::Initialize 1280x720
VulkanDevice::Initialize starting
Vulkan: selected software device 'llvmpipe (LLVM 20.1.2, 256 bits)' (Lavapipe/CPU)
VulkanDevice::CreateSwapChain: SDL_Vulkan_CreateSurface failed: The specified window isn't a Vulkan window
Backend 'Vulkan' failed to create swap chain — trying next
VulkanDevice::Shutdown
GLDevice::Initialize starting
OpenGL swap chain: windowed mode (1280x720)
Preferred backend 'Vulkan' unavailable — fell back to 'OpenGL'
Initialized on Linux via RHI (OpenGL)
...120 frames run cleanly, RC=0
```

No more SIGSEGV. The Vulkan path fails fast with a clear error
(`SDL_Vulkan_CreateSurface failed: The specified window isn't a Vulkan
window` — because `SparkEngineLinux::RunSDL2Windowed` creates the SDL
window with `SDL_WINDOW_OPENGL` and not `SDL_WINDOW_VULKAN`), RHIBridge's
fallback loop correctly drops to OpenGL, and the engine boots via
llvmpipe as it should.

`SPARK_DISABLE_VULKAN=1` from the earlier commit **still works** as a
user-level escape hatch — it short-circuits the loop entirely by
dropping Vulkan from `GetAvailableBackends()` before the loop even
runs, which is slightly faster and more explicit in logs. But it is no
longer required: the default path is crash-free.

### Follow-up — enabling real Vulkan rendering on Linux

The above fix makes Vulkan **fail cleanly**. To actually render via
Vulkan on Linux, `RunSDL2Windowed()` in `SparkEngineLinux.cpp` would
need to:

1. Decide at startup whether Vulkan or OpenGL is the preferred backend
   (currently hardcoded to `SDL_WINDOW_OPENGL`).
2. Create the SDL window with `SDL_WINDOW_VULKAN` instead when Vulkan
   is preferred.
3. Load libvulkan via `SDL_Vulkan_LoadLibrary` before creating the
   instance.

That is a separate, larger change and is not blocking anything today
— OpenGL/llvmpipe is the working path for headless Linux CI.

### Test coverage
- Full `SparkTests` suite: 5660 passed / 0 failed / 1 pre-existing
  flaky-list tolerated warning (5661 total).
- Live engine boot, default env: RC=0 via Vulkan→OpenGL fallback.
- Live engine boot, `SPARK_DISABLE_VULKAN=1`: RC=0 via direct OpenGL.


## 2026-04-15 update — SPARK_DISABLE_VULKAN env-var escape hatch

On a headless gVisor host with Mesa 25.2.8 Lavapipe, SparkEngine's
`VulkanDevice::Initialize` **SIGSEGVs (RC=139)** a few milliseconds after
logging `Vulkan: selected software device 'llvmpipe' (Lavapipe/CPU)`:

```
[INFO ] RHIBridge::Initialize 1280x720
[INFO ] VulkanDevice::Initialize starting
[INFO ] Vulkan: selected software device 'llvmpipe (LLVM 20.1.2, 256 bits)' (Lavapipe/CPU)
*** Segmentation fault (core dumped), RC=139 ***
```

`RHIBridge`'s fallback loop is designed to drop to OpenGL when a backend's
`Initialize()` returns false, but Vulkan **crashes** rather than returning
a clean failure, so the fallback never runs and the whole engine dies.

**Fix (`SparkEngine/Source/Graphics/RHI/RHIBridge.cpp`):** `GetAvailableBackends()`
and `GetRecommendedBackend()` now honor three env-var escape hatches:

| Env var | Effect |
|---|---|
| `SPARK_DISABLE_VULKAN=1` | Vulkan dropped from the backend list before the fallback loop runs |
| `SPARK_DISABLE_OPENGL=1` | OpenGL dropped |
| `SPARK_DISABLE_D3D11=1` | D3D11 dropped (Windows only) |

When a backend is dropped, RHIBridge logs `SPARK_DISABLE_<NAME>=1 — <Name>
backend skipped` at init time, and the fallback loop picks up the next
available backend. Setting `SPARK_DISABLE_VULKAN=1` on gVisor produces a
clean boot through OpenGL/llvmpipe:

```
[INFO ] SPARK_DISABLE_VULKAN=1 — Vulkan backend skipped
[INFO ] GLDevice::Initialize starting
[INFO ] Existing EGL context detected (SDL2/host-owned) — skipping EGL bootstrap
[INFO ] OpenGL 3.3 (Core Profile) Mesa 25.2.8 — Renderer: llvmpipe (LLVM 20.1.2, 256 bits)
[INFO ] Initialized on Linux via RHI (OpenGL)
...120 frames run, clean shutdown, RC=0
```

**Recipe for gVisor/headless Linux runs:**

```bash
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
  MESA_GL_VERSION_OVERRIDE=3.3 SPARK_DISABLE_VULKAN=1 SPARK_MAX_WORKER_THREADS=1 \
  ./SparkEngine -test-frames 120 -threads 1 -no-subprocess -window-size 1280x720
```

This is the workaround until `VulkanDevice::Initialize` is hardened to
return `false` instead of SIGSEGVing on broken Lavapipe ICDs.

The previous workaround (`VK_ICD_FILENAMES=/tmp/nonexistent-vk.json`) still
works as a libvulkan-level escape hatch, but `SPARK_DISABLE_VULKAN=1` is
cleaner because it's project-level and self-documenting.

Also preserved: explicitly passing `backend=GraphicsBackend::None` with a
valid window handle still routes to `NullRHIDevice` (the RHIBridge test
suite depends on this). The env-var filter only affects GPU backends.


## Description

How to build, run, and test the SparkEditor and SparkEngine runtime with live graphics on Linux using Xvfb + Mesa llvmpipe software rendering. This enables full GUI testing in headless/CI environments without a GPU.

## Context

The editor (SparkEditor) uses SDL2 + OpenGL 3.3 + ImGui on Linux. The engine runtime (SparkEngine) uses SDL2 for windowing and the OpenGL RHI backend. Both can run on software rendering via Mesa's llvmpipe driver.

## Approach

### Prerequisites (system packages)
- `xvfb` — Virtual framebuffer X server
- `libgl-dev` — OpenGL development headers (**must be installed before CMake configure**)
- `xdotool` — X11 automation (mouse/keyboard simulation)
- `python3-pillow` — Screenshot capture via Python

SDL2 is bundled as a git submodule at `ThirdParty/SDL2` (release-2.30.0) and built automatically by CMake. **Critical:** `libgl-dev` must be installed *before* running `cmake -B build`, otherwise SDL2 compiles without OpenGL/GLX support (`SDL not configured with OpenGL/GLX support` error). If this happens, install `libgl-dev`, delete the build directory, and reconfigure.

**Fallback when `libgl-dev` is unavailable** (no network, sandboxed environment):
The repo bundles GL headers in `ThirdParty/OpenGL/GL/`. Copy them to the system include path and create the dev symlink manually:
```bash
sudo cp ThirdParty/OpenGL/GL/*.h /usr/include/GL/
sudo ln -sf /usr/lib/x86_64-linux-gnu/libGL.so.1 /usr/lib/x86_64-linux-gnu/libGL.so
sudo ln -sf /usr/lib/x86_64-linux-gnu/libGLX.so.0 /usr/lib/x86_64-linux-gnu/libGLX.so
```
This works as long as the Mesa runtime libraries (`libgl1`, `libglx-mesa0`) are already installed (they usually are).

### Build
```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_OPENGL=ON \
  -DENABLE_SDL2=ON \
  -DENABLE_EDITOR=ON \
  -DSPARK_HEADLESS_SUPPORT=ON \
  -DBUILD_TESTS=ON
cmake --build build --parallel $(nproc)
```

### Run
```bash
# Start Xvfb
Xvfb :99 -screen 0 1920x1080x24 -ac &

# Environment
export DISPLAY=:99
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_GL_VERSION_OVERRIDE=3.3
export GALLIUM_DRIVER=llvmpipe

# Editor (with test mode to skip project browser)
./build/bin/SparkEditor --test-mode --debug-console

# Engine runtime
./build/bin/SparkEngine

# Automated test suite
python3 tools/test-editor-live.py build/bin/SparkEditor
```

### Test mode flags
- `--test-mode` — Skips the project browser modal, enables debug console
- `--test-frames N` — Exit after N frames (for automated testing)
- `--debug-console` — Print diagnostic output to stdout

### Screenshot capture
The `tools/test-editor-live.py` script includes X11 screenshot capture via ctypes (no ImageMagick required). Can also be used standalone via `/tmp/screenshot.py`.

## Key Fixes Discovered

1. **SDL2 must have OpenGL/GLX support** — If built before `libgl-dev` is installed, SDL2 compiles without GL support. Rebuild SDL2 after installing GL headers.

2. **Engine runtime GLX bootstrap fails on llvmpipe** — The OpenGLDevice's `CreateBootstrapContext` uses raw GLX which fails on some software renderers. Fixed by detecting an existing SDL2 GL context (`glXGetCurrentContext() != nullptr`) and skipping the GLX bootstrap.

3. **Engine runtime needs SDL GL attributes** — Added `SDL_GL_SetAttribute` calls (GL 3.3 Core, depth 24, stencil 8) before `SDL_CreateWindow` in `RunSDL2Windowed()`, matching what the editor already does.

4. **Engine runtime needs SDL GL context** — Added `SDL_GL_CreateContext` + `SDL_GL_MakeCurrent` in `RunSDL2Windowed()` before calling `GraphicsEngine::Initialize`, so the RHI can detect and reuse the existing context.

5. **GLXBadContext on engine shutdown** — When SDL2 creates the GL context, GLDevice stored the context handle but called `glXDestroyContext()` during shutdown, destroying SDL2's context before SDL2 could clean it up. Fixed by adding `m_ownsGLXContext` flag — only destroy the GLX context if the engine created it (bootstrap path).

6. **Process pipe fd aliasing breaks stderr capture** — `pipe()` can allocate fds overlapping with stdin/stdout/stderr (0-2) when previous tests close those fds. When `stderrPipe[0]` was fd 2, `dup2(stderrPipe[1], STDERR_FILENO)` would destroy the pipe read-end, then the child's `close(stderrPipe[0])` would close the newly-redirected stderr. Fixed by using `pipe2(O_CLOEXEC)` and a `redirectFd` helper that handles fd aliasing safely.

## Files Modified
- `SparkEngine/Source/Core/SparkEngine.cpp` — SDL GL attributes + context creation in `RunSDL2Windowed()`
- `SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp` — Detect existing GL context, skip GLX bootstrap, ownership-aware shutdown
- `SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.h` — `m_ownsGLXContext` flag
- `SparkEngine/Source/Utils/ProcessLinux.cpp` — `pipe2(O_CLOEXEC)` + safe fd redirect in child
- `SparkEditor/Source/Core/EditorApplication.h` — `testMode` and `testFrameLimit` in `EditorConfig`
- `SparkEditor/Source/Core/EditorApplication.cpp` — Test mode frame limit in `Run()`
- `SparkEditor/Source/Core/EditorUI.cpp` — Skip project browser in test mode
- `SparkEditor/Source/main.cpp` — `--test-mode` and `--test-frames` CLI args

## Files Added
- `tools/test-editor-live.py` — Automated live editor test suite (21 tests)

## Notes
- Mesa llvmpipe reports OpenGL 3.3 Core Profile with GLSL 4.50
- The editor renders at usable FPS (~30+) on software rendering
- xdotool works for basic menu interaction but pixel-hunting ImGui buttons is unreliable — prefer the `--test-mode` flag for automated testing
- The engine runtime shows a black window without a loaded scene — this is expected
