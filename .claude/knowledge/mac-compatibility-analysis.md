# Mac Compatibility Analysis

**Last updated:** 2026-03-28
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

### Key files
- `SparkEngine/Source/Graphics/RHI/Metal/MetalDevice.h` — Metal interface (header only)
- `SparkEngine/Source/Graphics/RHI/RHIFactory.cpp` — Backend selection (Metal on `__APPLE__`)
- `SparkEngine/Source/Core/Platform.h` — Platform/compiler detection
- `SparkEngine/Source/Audio/OpenALAudioEngine.h` — Cross-platform audio
- `SparkEngine/Source/Input/InputManager.cpp` — Win32-only (needs SDL2 variant)

## Notes

- Metal files are excluded from clang-format CI checks (`-not -path '*/Metal/*'`)
- macOS CI job uses `continue-on-error: true` until support stabilizes
- MoltenVK path avoids ~2,500 lines of Objective-C++ Metal implementation
