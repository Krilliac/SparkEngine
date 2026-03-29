# Cross-Compilation: Windows Testing on Linux (MinGW + Wine)

SparkEngine can cross-compile its Windows D3D11 code paths on Linux using MinGW-w64, then run the resulting `.exe` binaries under Wine. Combined with DXVK (D3D11->Vulkan) or Wine's built-in WineD3D (D3D11->OpenGL) and Mesa Lavapipe (software Vulkan/OpenGL), this exercises the **exact same `_WIN32` code** that MSVC compiles -- without Windows or a GPU.

**Key files:**
- `cmake/toolchains/mingw-w64-x86_64.cmake` -- CMake toolchain
- `tools/wine-run.sh` -- Wine runner (auto-detects DXVK, Lavapipe)
- `tools/test-windows-wine.py` -- Automated 7-phase Wine test suite
- `CMakePresets.json` -- `linux-mingw-release` / `linux-mingw-debug` presets

---

## The Stack

```
D3D11 C++ (same #ifdef _WIN32 code paths as MSVC)
  -> MinGW-w64 (x86_64-w64-mingw32-g++) -> .exe
  -> Wine (translates Windows API calls to Linux syscalls)
  -> DXVK (translates D3D11 -> Vulkan)       [if installed]
  -> WineD3D (translates D3D11 -> OpenGL)     [fallback]
  -> Lavapipe (software Vulkan on CPU)        [no GPU needed]
  -> llvmpipe (software OpenGL on CPU)        [WineD3D fallback]
```

## Prerequisites

### Required Packages

```bash
sudo apt-get install mingw-w64 wine64 mesa-vulkan-drivers
```

| Package | Purpose | Size |
|---------|---------|------|
| `mingw-w64` | Cross-compiler (x86_64-w64-mingw32-g++), Windows headers (d3d11.h, xinput.h, etc.), import libraries | ~80 MB |
| `wine64` | Windows API translation layer, WineD3D (D3D11->OpenGL) | ~300 MB |
| `mesa-vulkan-drivers` | Lavapipe software Vulkan (CPU rendering) | ~30 MB |

### Optional Packages

| Package | Purpose |
|---------|---------|
| `dxvk` | D3D11->Vulkan translation (much faster than WineD3D for rendering) |
| `xvfb` | Virtual X11 framebuffer (needed for windowed mode without a display) |
| `libgl-dev` | OpenGL headers (needed if building OpenGL backend too) |

### DirectXMath Headers

MinGW does **not** ship Microsoft's DirectXMath library. You must install them to the MinGW sysroot:

```bash
# Download from Microsoft's official GitHub repository
mkdir -p /tmp/dxmath && cd /tmp/dxmath
for f in DirectXMath.h DirectXMathConvert.inl DirectXMathMatrix.inl \
         DirectXMathMisc.inl DirectXMathVector.inl DirectXCollision.h \
         DirectXCollision.inl DirectXColors.h DirectXPackedVector.h \
         DirectXPackedVector.inl; do
    wget -q "https://raw.githubusercontent.com/microsoft/DirectXMath/main/Inc/$f"
done

# Install to MinGW sysroot
sudo cp DirectX*.h DirectX*.inl /usr/x86_64-w64-mingw32/include/
```

### Verify Installation

```bash
x86_64-w64-mingw32-g++ --version     # Should show GCC 13+
wine64 --version                       # Should show wine-9.0+
ls /usr/share/vulkan/icd.d/lvp_icd*   # Lavapipe ICD file
ls /usr/x86_64-w64-mingw32/include/DirectXMath.h  # DirectXMath
```

## Build

```bash
# Configure using the preset
cmake --preset linux-mingw-release

# Build all targets (SparkEngine.exe, SparkEditor.exe, SparkTests.exe, etc.)
cmake --build build/linux-mingw-release --parallel $(nproc)
```

### What Gets Built

| Target | File | Description |
|--------|------|-------------|
| `SparkEngine.exe` | `bin/SparkEngine.exe` | Engine runtime with D3D11 graphics |
| `SparkEditor.exe` | `bin/SparkEditor.exe` | Editor with D3D11 + ImGui |
| `SparkTests.exe` | `bin/SparkTests.exe` | Full unit test suite (2,500+ tests) |
| `SparkConsole.exe` | `bin/SparkConsole.exe` | Standalone debug console |
| `SparkShaderCompiler.exe` | `bin/SparkShaderCompiler.exe` | Offline shader compiler |
| `libSparkGameFPS.dll` | `bin/libSparkGameFPS.dll` | FPS game module |
| Other game modules | `bin/lib*.dll` | RPG, MMO, RTS, Racing, etc. |

### MinGW Build Differences from MSVC

| Feature | MSVC Build | MinGW Build |
|---------|-----------|-------------|
| D3D11 | Full support | Full support |
| D3D12 | Full support | **Excluded** (MinGW headers too old for ID3D12Device5) |
| DXR Raytracing | Optional | **Excluded** (requires D3D12) |
| DirectXMath | Built-in | Requires manual header install (see above) |
| XAudio2 | Built-in | Linked via `-lxaudio2_8` |
| dbghelp | Via `#pragma comment(lib)` | Linked explicitly in CMake |
| Entry point | `wWinMain` | `wWinMain` with `-municode` linker flag |
| Static linking | N/A | `-static-libgcc -static-libstdc++` |

## Running Under Wine

### Unit Tests

```bash
# Quick run (no graphics needed)
tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe

# Or manually:
export WINEPREFIX=$(pwd)/build/.wineprefix
export WINEDEBUG=-all
export LIBGL_ALWAYS_SOFTWARE=1
wine64 build/linux-mingw-release/bin/SparkTests.exe
```

Expected result: **~2,504/2,509 tests pass** (99.8%). The 5 expected failures are:
- `WARP_D3D11DeviceInit` -- needs real D3D11 runtime (DXVK or native Windows)
- `WARP_D3D11BufferCreation` -- same
- `WARP_D3D11FactoryCreate` -- same
- `StackTrace_FramesHaveAddresses` -- stack walking differs under Wine
- `LoadTest_FullEngine_3000Frames` -- needs graphics context

### Missing DLLs

If Wine reports a missing DLL (e.g., `libwinpthread-1.dll`), copy it from the MinGW sysroot:

```bash
cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll build/linux-mingw-release/bin/
```

### Console Application

```bash
wine64 build/linux-mingw-release/bin/SparkConsole.exe
# Output: "Spark Engine Console v1.0.0" -- interactive console works
```

### Engine with Graphics (Windowed)

The engine creates a Win32 window and initializes D3D11. Under Wine, this requires:
1. An X11 display (Xvfb for headless environments)
2. WineD3D (built into Wine) or DXVK for D3D11 translation

```bash
# Start virtual display
Xvfb :99 -screen 0 1920x1080x24 -ac &
export DISPLAY=:99

# Run engine with test frame limit
export WINEPREFIX=$(pwd)/build/.wineprefix
export WINEDEBUG=-all
export LIBGL_ALWAYS_SOFTWARE=1
wine64 build/linux-mingw-release/bin/SparkEngine.exe -test-frames 60
```

**Performance:**

| Configuration | 60 Frames | FPS | Translation Path |
|--------------|-----------|-----|-----------------|
| **DXVK + Lavapipe** | ~0.5s | ~120 | D3D11 -> Vulkan -> Lavapipe (CPU) |
| WineD3D + llvmpipe | >120s | <0.5 | D3D11 -> OpenGL -> llvmpipe (CPU) |

**DXVK provides ~20x speedup.** Install DXVK with:
```bash
tools/setup-mingw-wine.sh --dxvk-only   # Downloads from GitHub, no sudo needed
```

You can also use `-window-size 640x480` for faster software rendering:
```bash
wine64 SparkEngine.exe -test-frames 60 -window-size 640x480
```

### Engine Headless Mode

```bash
wine64 build/linux-mingw-release/bin/SparkEngine.exe -headless
```

### Automated Test Suite

The `test-windows-wine.py` script runs a comprehensive 7-phase test:

```bash
python3 tools/test-windows-wine.py --build-dir build/linux-mingw-release
```

**Phases:**

| Phase | Name | What It Tests |
|-------|------|---------------|
| 0 | Prerequisites | Wine, MinGW, Lavapipe, build artifacts exist |
| 1 | Wine Setup | Wine prefix initialization, DXVK/VKD3D detection |
| 2 | Unit Tests | Full test suite under Wine (2,509 tests) |
| 3 | Engine Live | D3D11 initialization, frame rendering, headless mode |
| 4 | Editor Live | D3D11 + ImGui initialization, test-mode rendering |
| 5 | Stress Tests | Rapid start/stop, concurrent instances, bad args |
| 6 | Break Tests | SIGKILL, SIGTERM, corrupt prefix, missing Vulkan |

Output: JSON report at `/tmp/spark-windows-wine-test/report.json`

## The `--test-frames` Flag

Both the engine and editor support a `--test-frames N` / `-test-frames N` command-line flag for automated testing:

```bash
# Engine (Windows): exit after 60 frames
wine64 SparkEngine.exe -test-frames 60

# Engine (Linux): exit after 60 frames
./SparkEngine -test-frames 60

# Editor: exit after 120 frames (with --test-mode to skip project browser)
wine64 SparkEditor.exe --test-mode --test-frames 120
```

The flag works on both platforms:
- **Windows (`wWinMain`):** Parsed from the wide command line, enforced in `RunWindowedMainLoop`
- **Linux (`main`):** Parsed from `argv`, enforced in `RunSDL2MainLoop`

## wine-run.sh

The `tools/wine-run.sh` script automates Wine environment setup:

```bash
tools/wine-run.sh <executable.exe> [args...]
tools/wine-run.sh --setup-only          # Initialize Wine prefix only
tools/wine-run.sh --info                # Print environment info
```

**What it does:**
1. Initializes a Wine prefix at `build/.wineprefix`
2. Auto-detects DXVK (D3D11->Vulkan) at standard paths
3. Auto-detects VKD3D-Proton (D3D12->Vulkan)
4. Finds Lavapipe ICD for software Vulkan
5. Sets `LIBGL_ALWAYS_SOFTWARE=1` for Mesa software rendering
6. Runs the `.exe` under `wine64`

## Troubleshooting

### Package Installation Issues

If `apt-get install` hangs on network, download packages directly:

```bash
# Get download URLs
apt-get download --print-uris mingw-w64-x86-64-dev g++-mingw-w64-x86-64-posix \
  gcc-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix-runtime \
  gcc-mingw-w64-base binutils-mingw-w64-x86-64 mingw-w64-common \
  libz-mingw-w64 2>&1 | grep "^'" | sed "s/' .*//" | sed "s/^'//"

# Download each URL with wget, then install:
sudo dpkg -i *.deb
```

### `DirectXMath.h: No such file or directory`

MinGW doesn't ship DirectXMath. Install it manually (see Prerequisites above).

### `SDKDDKVer.h: No such file or directory`

This is MSVC-only. The engine's `targetver.h` handles this automatically for MinGW by defining `_WIN32_WINNT` and `WINVER` directly.

### `SIGTRAP was not declared`

MinGW targets Windows where `SIGTRAP` doesn't exist. The engine's `Assert.h` uses `__builtin_trap()` on MinGW instead.

### Case-sensitive header includes

MinGW on Linux uses a case-sensitive filesystem. Windows headers must use lowercase:
- `#include <windows.h>` (not `<Windows.h>`)
- `#include <winsock2.h>` (not `<WinSock2.h>`)
- `#include <xinput.h>` (not `<Xinput.h>`)
- `#include <shlobj.h>` (not `<ShlObj.h>`)

### `wofstream` with `wstring` path

MinGW's libstdc++ doesn't support `std::wstring` paths in `fstream`. Convert to narrow string first:
```cpp
#if defined(_MSC_VER)
    std::wofstream ofs(wideFilename, std::ios::out);
#else
    std::string narrow(wideFilename.begin(), wideFilename.end());
    std::wofstream ofs(narrow.c_str(), std::ios::out);
#endif
```

### `_ReturnAddress()` not available

Use `__builtin_return_address(0)` on GCC/MinGW.

### `size_t` / `uint64_t` overload conflict

On 64-bit Windows (both MSVC and MinGW), `size_t` is `uint64_t`. Guard size_t overloads:
```cpp
#if !defined(_WIN64) && !defined(__LP64__) && !defined(__x86_64__)
    void Value(size_t v) { /* ... */ }
#endif
```

### `WinMain` entry point not found

MinGW needs `-municode` for `wWinMain` (Unicode entry point). This is set in CMakeLists.txt.

### WineD3D performance (D3D11 very slow)

WineD3D translates D3D11->OpenGL, which combined with llvmpipe is extremely slow (~minutes per frame for shader compilation). **Install DXVK** for D3D11->Vulkan translation:

```bash
sudo apt-get install dxvk
# Or download from: https://github.com/doitsujin/dxvk/releases
```

### `libwinpthread-1.dll` not found

Copy from MinGW sysroot:
```bash
cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll build/linux-mingw-release/bin/
```

## CI Integration

The `build-linux-mingw-wine` CI job in `.github/workflows/build.yml` runs this automatically:
- Installs MinGW, Wine, Mesa Lavapipe
- Cross-compiles with the MinGW toolchain
- Runs tests under Wine
- `continue-on-error: true` (non-blocking, advisory)

## Performance Optimization

### Implemented (2026-03-29)

| Optimization | Speedup | Status |
|-------------|---------|--------|
| **DXVK** (D3D11->Vulkan) | ~20x | Done — `setup-mingw-wine.sh --dxvk-only` |
| **Low resolution** (`-window-size 640x480`) | ~4x | Done — engine flag |
| **`-test-frames N`** flag | N/A | Done — automated frame-limited exits |

### Also Implemented

| Optimization | How | Status |
|-------------|-----|--------|
| **DXVK state cache** | `DXVK_STATE_CACHE_PATH` persists compiled pipelines | Done in wine-run.sh |
| **GPU auto-detection** | `detect_gpu()` in wine-run.sh skips Lavapipe if real GPU found | Done |

### Remaining Opportunities

1. **GPU passthrough in CI** — GitHub Actions runners with GPUs could run real D3D11 hardware tests
2. **Shader pre-warm** — the engine already lazy-loads shaders (only BasicVertex + BasicPixel at startup), complex scenes will benefit from DXVK pipeline cache

### Wine Return Code Quirk

Wine GUI (WIN32) applications often return exit code 255 instead of 0 when stdout/stderr are piped or redirected. The test script uses `wine_rc_ok(rc)` to treat both 0 and 255 as success. This is a known Wine behavior with `wWinMain` applications.
