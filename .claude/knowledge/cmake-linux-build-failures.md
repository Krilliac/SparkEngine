# CMake Linux Build Failures

**Last updated:** 2026-03-14
**Type:** Issue
**Status:** Resolved

## Issue

The Linux build is the primary CI target. CMake configure and compile failures on Linux fall into recurring patterns: cache conflicts from prior builds, missing apt packages, DirectX headers leaking outside platform guards, submodule initialization, and preset vs. manual flag mismatches.

## Context

Occurs when running `cmake --preset linux-gcc-release` locally or when reproducing `build-linux-gcc` / `build-linux-clang` / `build-linux-asan` CI job failures.

## Methods Tried

1. **`cmake --preset linux-gcc-release` when `build/` already exists with a different config** → FAILS
   Reason: CMake cache conflicts. Must delete `build/` first.

2. **`cmake --build build` without `--parallel`** → WORKS but extremely slow
   Fix: Always pass `--parallel $(nproc)`.

3. **Including `<d3d11.h>` or `<xaudio2.h>` in cross-platform headers without guards** → FAILS on Linux
   Reason: These headers don't exist on Linux. Must wrap in `#ifdef SPARK_PLATFORM_WINDOWS`.

4. **Forgetting to initialize submodules before configure** → FAILS
   CMake reports missing directories under `ThirdParty/`. Fix: `git submodule update --init --recursive`.

5. **Correct clean configure + build sequence** → WORKS (see below)

## Solution (Reliable Method)

```bash
# Ensure apt packages present (mirrors CI ubuntu-24.04)
sudo apt-get update
sudo apt-get install -y build-essential cmake libgl-dev libvulkan-dev libsdl2-dev libglew-dev

# Ensure submodules initialized
git submodule update --init --recursive

# Option A: preset (recommended)
cmake --preset linux-gcc-release
cmake --build build --config Release --parallel $(nproc)

# Option B: manual configure (when a custom flag is needed)
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)

# Run tests
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..

# If configure fails with a cache conflict: clean and retry
rm -rf build && cmake --preset linux-gcc-release
```

**Fixing the DirectX header guard pattern:**

```cpp
// WRONG — causes Linux build failure:
#include <d3d11.h>

// CORRECT — platform-guarded:
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <xaudio2.h>
#endif
```

## Notes

- Platform stub types (DirectXMath equivalents on Linux) live in `SparkEngine/Source/Core/Platform.h`. Add missing stubs there rather than adding Windows-only dependencies.
- `ENABLE_NETWORKING=OFF` is the default — don't add it explicitly unless specifically testing a networking build.
- For the ASan/UBSan build (`build-linux-asan` job), see CLAUDE.md "Matching CI build configurations locally" for the exact CMake flags.
- The `linux-gcc-release` preset requires CMake 3.16+. Check version with `cmake --version`.
