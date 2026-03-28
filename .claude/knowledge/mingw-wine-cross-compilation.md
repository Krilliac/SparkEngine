# MinGW + Wine Cross-Compilation (D3D11/D3D12 on Linux)

**Last updated:** 2026-03-28
**Type:** Pattern
**Status:** Active

## Description

SparkEngine can cross-compile its Windows D3D11/D3D12 code paths on Linux using MinGW-w64, then run the resulting `.exe` under Wine. Combined with DXVK (D3D11→Vulkan), VKD3D-Proton (D3D12→Vulkan), and Mesa Lavapipe (software Vulkan), this exercises the exact same `_WIN32` code that MSVC compiles — without Windows or a GPU.

## Approach

### The Stack

```
D3D11/D3D12 C++ (same #ifdef _WIN32 paths as MSVC)
  → MinGW-w64 (x86_64-w64-mingw32-g++) → .exe
  → Wine (translates Windows API calls)
  → DXVK (translates D3D11 → Vulkan)
  → VKD3D-Proton (translates D3D12 → Vulkan)
  → Lavapipe (software Vulkan on CPU)
```

### Prerequisites

```bash
sudo tools/setup-mingw-wine.sh          # Install everything
tools/setup-mingw-wine.sh --check       # Verify installation
```

Or manually:
```bash
sudo apt-get install mingw-w64 wine64 mesa-vulkan-drivers
# Optional: sudo apt-get install dxvk
```

### Build and Run

```bash
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release
tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe
```

### Key Files

| File | Purpose |
|------|---------|
| `cmake/toolchains/mingw-w64-x86_64.cmake` | CMake toolchain (sets cross-compiler, sysroot, static linking) |
| `tools/wine-run.sh` | Wine runner (auto-detects DXVK, VKD3D-Proton, Lavapipe) |
| `tools/setup-mingw-wine.sh` | Install + verify all prerequisites |
| `CMakePresets.json` | `linux-mingw-release` / `linux-mingw-debug` presets |

### CMake Changes for MinGW

1. `SPARK_PLATFORM_WINDOWS` is now defined for `MINGW` (was MSVC-only) — needed for D3D11 includes
2. `d3d12` added to Windows link libraries (was missing)
3. MinGW detected as GCC, so GCC flags apply (not `/W3` etc.)
4. Toolchain sets `-static-libgcc -static-libstdc++` so .exe doesn't need MinGW DLLs

### Software Rendering Fallback (All Backends)

| Backend | Windows Software | Linux Software |
|---------|-----------------|----------------|
| D3D11 | WARP (`D3D_DRIVER_TYPE_WARP`) | Wine + DXVK + Lavapipe |
| D3D12 | WARP (`EnumWarpAdapter()`) | Wine + VKD3D-Proton + Lavapipe |
| Vulkan | — | Lavapipe (`VK_PHYSICAL_DEVICE_TYPE_CPU`) |
| OpenGL | — | llvmpipe (EGL headless or GLX+Xvfb) |
| None | NullRHIDevice | NullRHIDevice |

All backends track `isSoftwareDevice` in `RHIDeviceCapabilities`.

### CI Job

`build-linux-mingw-wine` in `.github/workflows/build.yml`:
- `continue-on-error: true` (non-blocking)
- Installs MinGW, Wine, Mesa Lavapipe
- Cross-compiles with MinGW toolchain
- Runs tests under Wine

### Troubleshooting

- **MinGW `d3d11.h` not found**: Ensure `mingw-w64` package is installed (includes DirectX headers)
- **Wine crashes**: Check `WINEDEBUG=warn+all` for details. `wineboot --init` resets the prefix.
- **Lavapipe not detected**: Set `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`
- **DXVK not available**: Wine's built-in WineD3D translates D3D→OpenGL (slower but works)
- **`apt-get` timeouts**: Use `--fix-broken` and retry. Sandbox environments may block package downloads.

## Notes

- MinGW defines `_WIN32` automatically — D3D12 code compiles without changes
- D3D11 code needs `SPARK_PLATFORM_WINDOWS` (now set for MinGW in CMakeLists.txt)
- MSVC-specific features (`__declspec`, `#pragma comment(lib)`) are handled by MinGW compatibility
- The `#pragma comment(lib, ...)` in D3D11Device.cpp is MSVC-only; MinGW uses CMake's `target_link_libraries`
