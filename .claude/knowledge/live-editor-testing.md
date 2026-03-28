# Live Editor Testing on Linux (Software Rendering)

**Last updated:** 2026-03-28
**Type:** Pattern
**Status:** Active

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

## Files Modified
- `SparkEngine/Source/Core/SparkEngine.cpp` — SDL GL attributes + context creation in `RunSDL2Windowed()`
- `SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp` — Detect existing GL context, skip GLX bootstrap
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
