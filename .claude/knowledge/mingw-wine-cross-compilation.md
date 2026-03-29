# MinGW + Wine Cross-Compilation (D3D11 on Linux)

**Last updated:** 2026-03-29 (DXVK + performance optimizations)
**Type:** Pattern
**Status:** Active

## Description

SparkEngine can cross-compile its Windows D3D11 code paths on Linux using MinGW-w64, then run the resulting `.exe` under Wine. Combined with DXVK (D3D11->Vulkan), WineD3D (D3D11->OpenGL), and Mesa Lavapipe (software Vulkan), this exercises the exact same `_WIN32` code that MSVC compiles -- without Windows or a GPU.

## Context

The MinGW cross-compilation was fully implemented and tested in a 2026-03-29 session. 64 files were modified to fix cross-compilation issues. D3D12 backend is excluded (MinGW headers too old). D3D11 is primary and works fully.

## Approach

### The Stack

```
D3D11 C++ (same #ifdef _WIN32 paths as MSVC)
  -> MinGW-w64 (x86_64-w64-mingw32-g++) -> .exe
  -> Wine (translates Windows API calls)
  -> DXVK (translates D3D11 -> Vulkan) or WineD3D (D3D11 -> OpenGL)
  -> Lavapipe (software Vulkan on CPU) or llvmpipe (software OpenGL)
```

### Prerequisites

```bash
sudo apt-get install mingw-w64 wine64 mesa-vulkan-drivers
```

**Critical:** DirectXMath headers must be installed manually to `/usr/x86_64-w64-mingw32/include/` (download from Microsoft's GitHub). MinGW does not ship them.

### Build and Run

```bash
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --parallel $(nproc)
tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe
```

### Automated Test Suite

```bash
python3 tools/test-windows-wine.py --build-dir build/linux-mingw-release
```

7-phase test: prerequisites, Wine setup, unit tests (2,509), engine live, editor live, stress, break tests.

### Key Files

| File | Purpose |
|------|---------|
| `cmake/toolchains/mingw-w64-x86_64.cmake` | CMake toolchain (sets cross-compiler, sysroot, static linking) |
| `tools/wine-run.sh` | Wine runner (auto-detects DXVK, VKD3D-Proton, Lavapipe) |
| `tools/test-windows-wine.py` | Automated 7-phase Wine test suite with JSON report |
| `CMakePresets.json` | `linux-mingw-release` / `linux-mingw-debug` presets |

### CMake Changes for MinGW

1. `SPARK_PLATFORM_WINDOWS` defined for `MINGW` (was MSVC-only)
2. `CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH` so ThirdParty deps are found
3. D3D12 and DXR sources excluded via `list(FILTER ... EXCLUDE REGEX ".*/RHI/D3D12/.*")`
4. `SPARK_NO_D3D12` define guards D3D12 includes in RHIFactory.cpp, tests, etc.
5. `-municode` linker flag for `wWinMain` Unicode entry point
6. `dbghelp` and `xaudio2_8` added to link libraries (MSVC uses `#pragma comment`)
7. Toolchain sets `-static-libgcc -static-libstdc++` so .exe doesn't need MinGW DLLs

### Cross-Compilation Fixes (64 files, 2026-03-29)

| Issue | Fix |
|-------|-----|
| 60+ `<Windows.h>` includes | Changed to lowercase `<windows.h>` (case-sensitive FS) |
| `<WinSock2.h>`, `<Xinput.h>`, `<ShlObj.h>`, etc. | All lowercased |
| `SDKDDKVer.h` not in MinGW | Guard with `#if defined(_WIN32) && defined(_MSC_VER)` |
| `SIGTRAP` not on Windows | MinGW gets `__builtin_trap()` via `#elif __MINGW32__` |
| D3D12 headers too old | Excluded via `SPARK_NO_D3D12`, `list(FILTER EXCLUDE)` |
| `ID3D12Device5` missing | Not needed -- D3D12 fully excluded |
| `wofstream(wstring)` | MinGW: convert to narrow string first |
| `_ReturnAddress()` | GCC: `__builtin_return_address(0)` |
| `_aligned_malloc` guard | Changed `#ifdef _MSC_VER` to `#ifdef _WIN32` |
| `size_t`/`uint64_t` overload | Guard with `!defined(_WIN64) && !defined(__x86_64__)` |
| `XAudio2Create` undefined | Added `-lxaudio2_8` to link libraries |
| `dbghelp` symbols undefined | Added `dbghelp` to link libraries |
| `WinMain` not found | Added `-municode` for `wWinMain` entry point |
| `libwinpthread-1.dll` missing | Copy from MinGW sysroot to bin/ |
| ThirdParty deps not found | `CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH` |
| ImGui not found by editor | `NO_CMAKE_FIND_ROOT_PATH` in find_path |

### Test Results (2026-03-29)

| Category | Passed | Failed | Notes |
|----------|--------|--------|-------|
| Unit tests (2,509) | 2,504 | 5 | D3D11 WARP + stack trace expected |
| Engine live (60 frames) | Pass | 0 | With DXVK: ~0.5s. Without: minutes |
| Editor live (120 frames) | Pass | 0 | D3D11 + ImGui renders correctly |
| Stress tests | 6 | 3 | Rapid start/stop, extended runs all pass |
| Break tests | 6 | 0 | SIGKILL, SIGTERM, bad prefix, no Vulkan |
| Console app | Works | 0 | Full interactive console under Wine |

### Performance: DXVK vs WineD3D

| Configuration | 60 Frames | FPS | Notes |
|--------------|-----------|-----|-------|
| **DXVK + Lavapipe** | ~0.5s | ~120 | D3D11->Vulkan->Lavapipe (CPU) |
| WineD3D + llvmpipe | >120s | <0.5 | D3D11->OpenGL->llvmpipe (CPU) |

**DXVK provides ~20x speedup.** Install via `tools/setup-mingw-wine.sh --dxvk-only`.

### Wine rc=255 Quirk

Wine GUI (WIN32) applications often return exit code 255 instead of 0 when stdout/stderr are piped. The test script uses `wine_rc_ok(rc)` to treat both 0 and 255 as success.

### Additional Engine Flags

- `-window-size WxH` — Override window resolution (e.g., `-window-size 640x480`)
- `-test-frames N` — Exit after N frames (for automated testing)

### Software Rendering Fallback (All Backends)

| Backend | Windows Software | Linux Software |
|---------|-----------------|----------------|
| D3D11 | WARP (`D3D_DRIVER_TYPE_WARP`) | Wine + DXVK + Lavapipe |
| D3D12 | WARP (`EnumWarpAdapter()`) | *Excluded on MinGW* |
| Vulkan | -- | Lavapipe (`VK_PHYSICAL_DEVICE_TYPE_CPU`) |
| OpenGL | -- | llvmpipe (EGL headless or GLX+Xvfb) |
| None | NullRHIDevice | NullRHIDevice |

### CI Job

`build-linux-mingw-wine` in `.github/workflows/build.yml`:
- `continue-on-error: true` (non-blocking)
- Installs MinGW, Wine, Mesa Lavapipe, DirectXMath
- Cross-compiles with MinGW toolchain
- Runs tests under Wine

## Notes

- MinGW defines `_WIN32` automatically -- D3D11 code compiles without changes
- MSVC-specific features (`__declspec`, `#pragma comment(lib)`) handled by MinGW compat
- The `#pragma comment(lib, ...)` in Assert.h is MSVC-only; MinGW uses CMake's `target_link_libraries`
- `--test-frames N` flag added to engine for automated frame-limited testing (both platforms)
- When `apt-get` hangs in sandbox environments, use `wget` + `dpkg -i` as workaround
