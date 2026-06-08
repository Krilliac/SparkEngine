# Live Editor Testing on Linux (Software Rendering)

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** Linux host, SDL2 + OpenGL (llvmpipe) primary; Vulkan (Lavapipe) fallback path documented

## Overview

How to build, run, and test SparkEditor and the SparkEngine runtime with **live graphics on Linux** using Xvfb + Mesa software rendering — full GUI testing in headless/CI environments without a GPU.

The editor (SparkEditor) uses SDL2 + OpenGL 3.3 + ImGui on Linux. The engine runtime (SparkEngine) uses SDL2 for windowing and the OpenGL RHI backend. Both run on software rendering via Mesa's llvmpipe driver. A Vulkan path via Lavapipe also exists but falls back to OpenGL on most headless hosts (see below).

## Prerequisites (system packages)

- `xvfb` — virtual framebuffer X server
- `libgl-dev` — OpenGL development headers (**must be installed before CMake configure**)
- `xdotool` — X11 automation (mouse/keyboard simulation)
- `python3-pillow` — screenshot capture via Python

SDL2 is bundled as a git submodule at `ThirdParty/SDL2` (tracks the upstream `SDL2` branch via `https://github.com/libsdl-org/SDL.git`) and built automatically by CMake.

**Critical:** `libgl-dev` must be installed *before* running `cmake -B build`, otherwise SDL2 compiles without OpenGL/GLX support (`SDL not configured with OpenGL/GLX support`). If that happens, install `libgl-dev`, delete the build directory, and reconfigure.

**Fallback when `libgl-dev` is unavailable** (no network / sandbox): the repo bundles GL headers under `ThirdParty/OpenGL/GL/`. Copy them to the system include path and create the dev symlinks manually:

```bash
sudo cp ThirdParty/OpenGL/GL/*.h /usr/include/GL/
sudo ln -sf /usr/lib/x86_64-linux-gnu/libGL.so.1  /usr/lib/x86_64-linux-gnu/libGL.so
sudo ln -sf /usr/lib/x86_64-linux-gnu/libGLX.so.0 /usr/lib/x86_64-linux-gnu/libGLX.so
```

This works as long as the Mesa runtime libraries (`libgl1`, `libglx-mesa0`) are already installed (they usually are).

## Build

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

## Run

```bash
# Start Xvfb
Xvfb :99 -screen 0 1920x1080x24 -ac &

# Environment
export DISPLAY=:99
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_GL_VERSION_OVERRIDE=3.3
export GALLIUM_DRIVER=llvmpipe

# Editor (test mode skips the project browser)
./build/bin/SparkEditor --test-mode --debug-console

# Engine runtime
./build/bin/SparkEngine

# Automated live editor test suite
python3 tools/test-editor-live.py build/bin/SparkEditor
```

### Test-mode flags

- `--test-mode` — skips the project-browser modal, enables debug console
- `--test-frames N` — exit after N frames (automated testing)
- `--debug-console` — print diagnostic output to stdout

`tools/test-editor-live.py` includes X11 screenshot capture via ctypes (no ImageMagick required).

## Key Fixes Discovered (OpenGL path)

1. **SDL2 must have OpenGL/GLX support** — if built before `libgl-dev`, SDL2 compiles without GL. Rebuild SDL2 after installing the GL headers.
2. **Engine GLX bootstrap fails on llvmpipe** — the OpenGL device's `CreateBootstrapContext` used raw GLX, which fails on some software renderers. Fixed by detecting an existing SDL2 GL context (`glXGetCurrentContext() != nullptr`) and skipping the GLX bootstrap.
3. **Engine needs SDL GL attributes** — added `SDL_GL_SetAttribute` calls (GL 3.3 Core, depth 24, stencil 8) before `SDL_CreateWindow` in `RunSDL2Windowed()`, matching the editor.
4. **Engine needs an SDL GL context** — added `SDL_GL_CreateContext` + `SDL_GL_MakeCurrent` before `GraphicsEngine::Initialize`, so the RHI can detect and reuse the existing context.
5. **GLXBadContext on shutdown** — when SDL2 owns the GL context, GLDevice must not call `glXDestroyContext()`. Fixed with an `m_ownsGLXContext` flag — only destroy the context if the engine created it (bootstrap path).
6. **Process pipe fd aliasing breaks stderr capture** — `pipe()` can allocate fds overlapping 0–2 when prior tests closed those fds, corrupting `dup2` redirection. Fixed with `pipe2(O_CLOEXEC)` and an alias-safe `redirectFd` helper.

### Files modified (OpenGL path)

- `SparkEngine/Source/Core/SparkEngine.cpp` — SDL GL attributes + context in `RunSDL2Windowed()`
- `SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp` / `.h` — detect existing GL context, skip GLX bootstrap, ownership-aware shutdown (`m_ownsGLXContext`)
- `SparkEngine/Source/Utils/ProcessLinux.cpp` — `pipe2(O_CLOEXEC)` + safe fd redirect
- `SparkEditor/Source/Core/EditorApplication.{h,cpp}` — `testMode` / `testFrameLimit` in `EditorConfig`
- `SparkEditor/Source/Core/EditorUI.cpp` — skip project browser in test mode
- `SparkEditor/Source/main.cpp` — `--test-mode` / `--test-frames` CLI args

### File added

- `tools/test-editor-live.py` — automated live editor test suite

## Vulkan SIGSEGV → Clean Fallback (April 2026)

On a headless gVisor host with Mesa Lavapipe, `VulkanDevice::Initialize` historically **SIGSEGV'd (RC=139)** shortly after selecting the llvmpipe software device. The root cause was traced under `gdb` to `VulkanSwapChain::CreateSwapChain`: `VulkanDevice::CreateSwapChain` only had a `#ifdef _WIN32` surface-creation branch, so on Linux `m_surface` stayed `VK_NULL_HANDLE` and `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` dereferenced a null surface. The Lavapipe ICD was blameless.

### The real fix (three parts)

1. **`VulkanDevice.h`** — enable XCB + Xlib + Wayland surface macros on Linux so the Vulkan header pulls in all three surface-extension name sets. Also `#undef` Xlib's unqualified macros (`None`, `Status`, `Success`, `Bool`, `True`, `False`, `Always`) that otherwise poison the engine (e.g. `RHICullMode::None`).
2. **`VulkanDevice.cpp`** — enable the matching instance extensions when the ICD advertises them; add an `#elif defined(SPARK_SDL2_AVAILABLE)` branch calling `SDL_Vulkan_CreateSurface`; and bail early with `nullptr` when `desc.windowHandle` is null so `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` is never called with `VK_NULL_HANDLE`.
3. **`RHIBridge.cpp`** — fold swap-chain creation into the backend fallback loop. Previously a `CreateSwapChain()` returning nullptr aborted `RHIBridge::Initialize` with no retry. Now a swap-chain failure logs `Backend 'X' failed to create swap chain — trying next`, tears the device down, and loops to the next candidate — which is what lets OpenGL actually get tried when Vulkan can't make a surface.

### Observed behavior now (default, no env vars)

The Vulkan path fails *fast and cleanly* (`SDL_Vulkan_CreateSurface failed: The specified window isn't a Vulkan window` — because `RunSDL2Windowed` creates the window with `SDL_WINDOW_OPENGL`, not `SDL_WINDOW_VULKAN`), RHIBridge falls back to OpenGL, and the engine boots via llvmpipe and runs 120 frames cleanly (RC=0). No more SIGSEGV.

To actually render via Vulkan on Linux, `RunSDL2Windowed()` would need to pick Vulkan as preferred at startup, create the SDL window with `SDL_WINDOW_VULKAN`, and `SDL_Vulkan_LoadLibrary` before instance creation. That is a separate, larger change; OpenGL/llvmpipe is the working headless-CI path today.

## `SPARK_DISABLE_*` Env-Var Escape Hatches

`RHIBridge.cpp` (`GetAvailableBackends()` / `GetRecommendedBackend()`) honors three env vars that drop a backend from the candidate list *before* the fallback loop runs — verified present in `SparkEngine/Source/Graphics/RHI/RHIBridge.cpp`:

| Env var | Effect |
|---------|--------|
| `SPARK_DISABLE_VULKAN=1` | Vulkan dropped from the backend list |
| `SPARK_DISABLE_OPENGL=1` | OpenGL dropped |
| `SPARK_DISABLE_D3D11=1` | D3D11 dropped (Windows only) |

When a backend is dropped, RHIBridge logs `SPARK_DISABLE_<NAME>=1 — <Name> backend skipped`. With the swap-chain fallback fix above, `SPARK_DISABLE_VULKAN=1` is no longer *required* for a crash-free boot — but it remains a clean, self-documenting, slightly faster way to force OpenGL. Passing `backend=GraphicsBackend::None` with a valid window handle still routes to `NullRHIDevice`; the env-var filter only affects GPU backends.

### Recipe for gVisor / headless Linux runs

```bash
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
  MESA_GL_VERSION_OVERRIDE=3.3 SPARK_DISABLE_VULKAN=1 SPARK_MAX_WORKER_THREADS=1 \
  ./SparkEngine -test-frames 120 -threads 1 -no-subprocess -window-size 1280x720
```

## Notes

- Mesa llvmpipe reports OpenGL 3.3 Core Profile with GLSL 4.50.
- The editor renders at a usable framerate (~30+ FPS) on software rendering.
- `xdotool` works for basic menu interaction, but pixel-hunting ImGui buttons is unreliable — prefer `--test-mode` for automation.
- The engine runtime shows a black window without a loaded scene — this is expected.

## Source & Freshness

- **Original entry date:** 2026-04-15 (`.claude/knowledge/live-editor-testing.md`, type: Pattern), with two same-day updates (env-var escape hatch + Vulkan SIGSEGV root cause/fix)
- **Verified against codebase 2026-06-08.**
- **VERIFIED present:** `tools/test-editor-live.py` exists; `SPARK_DISABLE_VULKAN` / `SPARK_DISABLE_OPENGL` / `SPARK_DISABLE_D3D11` are all handled in `SparkEngine/Source/Graphics/RHI/RHIBridge.cpp` (and `SPARK_DISABLE_VULKAN` is also referenced in `SparkEngineLinux.cpp`). SDL2 submodule confirmed at `ThirdParty/SDL2` tracking the upstream `SDL2` branch.
- **UPDATED:** Reorganized the two 2026-04-15 session-update blocks (originally at the top as raw session logs) into a clean Vulkan-fallback section and an env-var section; stripped session-diary framing.
- **FLAGGED — STALE counts:** the original test-coverage figures (5660/5661 suite, "21 tests" in the live editor script) are 2026-04-15 snapshots; the unit-test suite has since grown past 6,000 tests, so those numbers are treated as historical and omitted from the body.
- **UNVERIFIED:** the SDL2 pin `release-2.30.0` in the original entry — the submodule now tracks the `SDL2` branch generically; the exact checked-out tag was not pinned-down here.

## Related Pages

- [MinGW-Wine-Cross-Compilation.md](MinGW-Wine-Cross-Compilation.md) — the Wine/D3D11 counterpart for exercising Windows code on Linux
- [AI-Bloat-Pattern.md](AI-Bloat-Pattern.md) — the wire-it-in-or-delete discipline behind the RHIBridge fallback work
- [Clang-Format.md](Clang-Format.md) — formatting gate for the files modified here
