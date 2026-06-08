# MinGW + Wine Cross-Compilation (D3D11 on Linux)

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** Linux host → Windows target (`_WIN32` / D3D11 code paths), run under Wine + DXVK/WineD3D + Mesa software rasterizers

## Overview

SparkEngine can cross-compile its Windows D3D11 code paths on Linux using MinGW-w64, then run the resulting `.exe` under Wine. Combined with DXVK (D3D11 → Vulkan), WineD3D (D3D11 → OpenGL), and Mesa Lavapipe/llvmpipe (software rasterization), this exercises the exact same `#ifdef _WIN32` code that MSVC compiles — without Windows and without a GPU.

This was fully implemented and tested in a 2026-03-29 session. 64 files were touched to fix cross-compilation issues. The D3D12 backend is **excluded** (the MinGW headers are too old); D3D11 is primary and works fully.

## The Stack

```
D3D11 C++ (same #ifdef _WIN32 paths as MSVC)
  -> MinGW-w64 (x86_64-w64-mingw32-g++) -> .exe
  -> Wine (translates Windows API calls)
  -> DXVK (D3D11 -> Vulkan)  or  WineD3D (D3D11 -> OpenGL)
  -> Lavapipe (software Vulkan, CPU)  or  llvmpipe (software OpenGL, CPU)
```

## Prerequisites

```bash
sudo apt-get install mingw-w64 wine64 wine mesa-vulkan-drivers
```

**Critical:** DirectXMath headers must be installed manually into `/usr/x86_64-w64-mingw32/include/` (download from Microsoft's GitHub). MinGW does not ship them.

When `apt-get` hangs in sandboxed environments, fall back to `wget` + `dpkg -i`.

## Build and Run

Presets `linux-mingw-release` and `linux-mingw-debug` exist in `CMakePresets.json`. Both set `CMAKE_TOOLCHAIN_FILE` to the MinGW toolchain and disable Vulkan, OpenGL, and SDL2 (the cross-build targets the D3D11 + Wine path, not the Linux-native RHI backends):

```bash
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --parallel $(nproc)
tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe
```

> Note: the preset's `binaryDir` is `build/linux-mingw-release` (per-preset build dir). Earlier notes sometimes used a plain `build/` directory — use the preset's directory to match `--preset`.

### Automated Test Suite

```bash
python3 tools/test-windows-wine.py --build-dir build/linux-mingw-release
```

A 7-phase test: prerequisites, Wine setup, unit tests, engine-live, editor-live, stress, and break tests.

### Key Files

| File | Purpose |
|------|---------|
| `cmake/toolchains/mingw-w64-x86_64.cmake` | CMake toolchain (cross-compiler, sysroot, static linking) |
| `tools/wine-run.sh` | Wine runner (auto-detects DXVK / VKD3D-Proton / Lavapipe; `--setup-only`, `--dxvk-only`) |
| `tools/test-windows-wine.py` | Automated 7-phase Wine test suite with JSON report |
| `tools/setup-mingw-wine.sh` | One-shot environment setup (supports `--dxvk-only`) |
| `CMakePresets.json` | `linux-mingw-release` / `linux-mingw-debug` presets |

All four files verified present as of 2026-06-08.

## CMake Changes for MinGW

1. `SPARK_PLATFORM_WINDOWS` defined for `MINGW` (was MSVC-only).
2. `CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH` so ThirdParty deps are found.
3. D3D12 and DXR sources excluded via `list(FILTER ... EXCLUDE REGEX ".*/RHI/D3D12/.*")`.
4. `SPARK_NO_D3D12` define guards D3D12 includes in `RHIFactory.cpp`, tests, etc.
5. `-municode` linker flag for the `wWinMain` Unicode entry point.
6. `dbghelp` and `xaudio2_8` added to link libraries (MSVC uses `#pragma comment`).
7. Toolchain sets `-static-libgcc -static-libstdc++` so the `.exe` does not need MinGW DLLs.

## Cross-Compilation Fixes (64 files, 2026-03-29)

| Issue | Fix |
|-------|-----|
| 60+ `<Windows.h>` includes | Lowercased to `<windows.h>` (case-sensitive FS) |
| `<WinSock2.h>`, `<Xinput.h>`, `<ShlObj.h>`, etc. | All lowercased |
| `SDKDDKVer.h` not in MinGW | Guarded with `#if defined(_WIN32) && defined(_MSC_VER)` |
| `SIGTRAP` not on Windows | MinGW gets `__builtin_trap()` via `#elif __MINGW32__` |
| D3D12 headers too old | Excluded via `SPARK_NO_D3D12`, `list(FILTER EXCLUDE)` |
| `ID3D12Device5` missing | Not needed — D3D12 fully excluded |
| `wofstream(wstring)` | MinGW: convert to narrow string first |
| `_ReturnAddress()` | GCC: `__builtin_return_address(0)` |
| `_aligned_malloc` guard | Changed `#ifdef _MSC_VER` to `#ifdef _WIN32` |
| `size_t`/`uint64_t` overload | Guarded with `!defined(_WIN64) && !defined(__x86_64__)` |
| `XAudio2Create` undefined | Added `-lxaudio2_8` |
| `dbghelp` symbols undefined | Added `dbghelp` to link libraries |
| `WinMain` not found | Added `-municode` for `wWinMain` |
| `libwinpthread-1.dll` missing | Copy from MinGW sysroot to `bin/` |
| ThirdParty deps not found | `CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH` |
| ImGui not found by editor | `NO_CMAKE_FIND_ROOT_PATH` in `find_path` |

`#pragma comment(lib, ...)` in `Assert.h` is MSVC-only; under MinGW the same libs come in via CMake's `target_link_libraries`.

## Test Results (2026-03-29 snapshot)

| Category | Passed | Failed | Notes |
|----------|--------|--------|-------|
| Unit tests | 2,504 | 5 | D3D11 WARP + stack-trace expected failures |
| Engine live (60 frames) | Pass | 0 | DXVK: ~0.5s. Without DXVK: minutes |
| Editor live (120 frames) | Pass | 0 | D3D11 + ImGui renders correctly |
| Stress tests | 6 | 3 | Rapid start/stop, extended runs pass |
| Break tests | 6 | 0 | SIGKILL, SIGTERM, bad prefix, no Vulkan |
| Console app | Works | 0 | Full interactive console under Wine |

> The 2,509 unit-test count is a 2026-03-29 snapshot. The suite has since grown well past 6,000 tests; treat the table as historical for that run.

## Performance: DXVK vs WineD3D

| Configuration | 60 frames | FPS | Path |
|---------------|-----------|-----|------|
| **DXVK + Lavapipe** | ~0.5s | ~120 | D3D11 → Vulkan → Lavapipe (CPU) |
| WineD3D + llvmpipe | >120s | <0.5 | D3D11 → OpenGL → llvmpipe (CPU) |

**DXVK provides roughly a 20x speedup.** Install via `tools/setup-mingw-wine.sh --dxvk-only`.

## Wine rc=255 Quirk

Wine GUI (`WIN32`) applications often return exit code 255 instead of 0 when stdout/stderr are piped. The test script's `wine_rc_ok(rc)` treats both 0 and 255 as success.

## Useful Engine Flags

- `-window-size WxH` — override window resolution (e.g. `-window-size 640x480`)
- `-test-frames N` — exit after N frames (automated testing)

## Software Rendering Fallback (All Backends)

| Backend | Windows software | Linux software |
|---------|------------------|----------------|
| D3D11 | WARP (`D3D_DRIVER_TYPE_WARP`) | Wine + DXVK + Lavapipe |
| D3D12 | WARP (`EnumWarpAdapter()`) | *Excluded on MinGW* |
| Vulkan | — | Lavapipe (`VK_PHYSICAL_DEVICE_TYPE_CPU`) |
| OpenGL | — | llvmpipe (EGL headless or GLX + Xvfb) |
| None | NullRHIDevice | NullRHIDevice |

## CI Job

`build-linux-mingw-wine` in `.github/workflows/build.yml`:

- **`if: github.event_name == 'workflow_dispatch'`** — the job runs **only on manual dispatch**, not on every push/PR. (This changed from the original entry, which described it as running on PRs.)
- `continue-on-error: true` (non-blocking).
- Installs MinGW, Wine, Mesa Lavapipe; cross-compiles with the MinGW toolchain.
- Configures with `-DENABLE_VULKAN=OFF -DENABLE_OPENGL=OFF -DENABLE_SDL2=OFF` (matching the preset).
- Sets up the Wine prefix via `tools/wine-run.sh --setup-only`, then runs `SparkTests.exe` under Wine.

## Notes

- MinGW defines `_WIN32` automatically — D3D11 code compiles without changes.
- MSVC-specific features (`__declspec`, `#pragma comment(lib)`) are handled by MinGW compat.
- The `-test-frames N` flag works on both platforms for frame-limited automated testing.

## Source & Freshness

- **Original entry date:** 2026-03-29 (`.claude/knowledge/mingw-wine-cross-compilation.md`, type: Pattern)
- **Verified against codebase 2026-06-08.**
- **VERIFIED present:** `cmake/toolchains/mingw-w64-x86_64.cmake`, `tools/wine-run.sh`, `tools/test-windows-wine.py`, `tools/setup-mingw-wine.sh`. Presets `linux-mingw-release` / `linux-mingw-debug` exist in `CMakePresets.json` and disable Vulkan/OpenGL/SDL2 as documented.
- **UPDATED — CI trigger:** `build-linux-mingw-wine` now runs **only on `workflow_dispatch`** (manual), not on every PR. The original entry implied it ran on PRs. Confirmed it still uses `continue-on-error: true` and `-DENABLE_VULKAN=OFF -DENABLE_OPENGL=OFF -DENABLE_SDL2=OFF`.
- **UPDATED — build directory:** corrected build/run commands to use the preset's per-preset `build/linux-mingw-release` directory.
- **FLAGGED — STALE counts:** the 2,509 unit-test figure is a 2026-03-29 snapshot; the suite now exceeds 6,000 tests. Test-results table marked historical.

## Related Pages

- [Live-Editor-Testing.md](Live-Editor-Testing.md) — the Linux-native (SDL2 + llvmpipe) counterpart to this Wine path
- [Clang-Format.md](Clang-Format.md) — CI formatting gate that runs on the same Linux runners
- [Code-Quality-Violations.md](Code-Quality-Violations.md) — quality audit covering the cross-platform code touched here
