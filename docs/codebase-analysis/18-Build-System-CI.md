# 18 — Build System & CI

---

## CMake Build System

**Root:** `CMakeLists.txt` (~750+ lines)
**Required:** CMake 3.25+, C++23

### Build Targets

| Target | Type | Purpose |
|--------|------|---------|
| `SparkEngineLib` | STATIC | All engine systems (reusable library) |
| `SparkEngine` | EXECUTABLE | Host launcher (loads game modules) |
| `SparkGame` | SHARED | Example FPS game module |
| `SparkGameMMO` | SHARED | MMO game module |
| `SparkEditor` | EXECUTABLE | ImGui visual editor |
| `SparkConsole` | EXECUTABLE | External debug console |
| `SparkShaderCompiler` | EXECUTABLE | Offline shader compilation tool |
| `SparkTests` | EXECUTABLE | Test suite (146 tests) |

### Feature Toggles

```cmake
# Core features (ON by default)
ENABLE_GRAPHICS=ON          # Rendering engine
ENABLE_PROFILING=ON         # Frame profiler, Chrome tracing
ENABLE_NETWORKING=ON        # UDP networking
ENABLE_EDITOR=ON            # ImGui editor
ENABLE_RECAST=ON            # Recast/Detour navmesh
ENABLE_HYBRID_RT=ON         # Hybrid ray tracing

# Graphics backends
ENABLE_VULKAN=ON            # Vulkan RHI (experimental)
ENABLE_OPENGL=ON            # OpenGL RHI (experimental)
ENABLE_METAL=OFF            # Metal (macOS only, experimental)
ENABLE_DXR=OFF              # DirectX Raytracing (experimental)

# Platform
ENABLE_SDL2=ON (Linux)      # SDL2 windowing/input

# Build
BUILD_TESTS=ON              # Build test suite
SPARK_SUPPRESS_THIRDPARTY_WARNINGS=ON
SPARK_DOUBLE_PRECISION_PHYSICS=OFF
```

### Quick Build

```bash
# Windows (Visual Studio 2022)
cmake --preset windows-release
cmake --build build --config Release

# Linux (GCC)
cmake --preset linux-gcc-release
cmake --build build --config Release

# Run tests
cd build && ctest --output-on-failure
```

### Third-Party Libraries

| Library | Type | Purpose |
|---------|------|---------|
| EnTT | Header-only | ECS framework |
| Jolt Physics | Source | Physics engine |
| ImGui | Source | Editor UI |
| AngelScript | Source | Scripting language |
| Recast/Detour | Source | NavMesh pathfinding |
| miniz | Source | Compression |
| TinyObjLoader | Header | OBJ file loading |
| DirectX 11 | SDK | Primary graphics API |
| Vulkan | SDK | Graphics API |
| OpenGL + GLAD | System | Graphics API |
| SDL2 | System | Cross-platform windowing |

### Compiler Flags

**MSVC:**
```
/W3 /MP /bigobj /Zc:__cplusplus /permissive- /Zc:preprocessor
/wd4005 /wd4996 /wd4244 /wd4267 /wd26495
```

**GCC/Clang:**
```
-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-reorder
```

### Output Directories

```
build/bin/    — Executables and DLLs
build/lib/    — Static libraries and import libs
```

---

## CMake Presets

**File:** `CMakePresets.json`

| Preset | Compiler | Config |
|--------|----------|--------|
| `windows-debug` | MSVC v143 | Debug |
| `windows-release` | MSVC v143 | Release |
| `linux-gcc-debug` | GCC | Debug |
| `linux-gcc-release` | GCC | Release |
| `linux-clang-debug` | Clang | Debug |
| `linux-clang-release` | Clang | Release |

All presets enable `BUILD_TESTS=ON` by default.

---

## CI/CD Pipeline

**File:** `.github/workflows/build.yml` (~372 lines)

### Trigger Events

- Push to: `main`, `develop`, `Working`, `feature/**`, `claude/**`
- Pull requests against: `main`, `develop`, `Working`

### CI Jobs (11 total)

| Job | Runner | Purpose | Required |
|-----|--------|---------|----------|
| `check-format` | ubuntu-24.04 | clang-format style enforcement | Yes |
| `validate-prompts` | ubuntu-24.04 | Prompt system validation | Yes |
| `build-linux-gcc` | ubuntu-24.04 | GCC Debug+Release + tests | Yes |
| `build-linux-clang` | ubuntu-24.04 | Clang Debug+Release + tests | Informational |
| `build-linux-asan` | ubuntu-24.04 | ASan+UBSan memory checks | Informational |
| `build-windows-vs2022` | windows-latest | MSVC v143 Debug+Release + tests | Yes |
| `build-windows-vs2026` | windows-latest | MSVC v144 (experimental) | Warning only |
| `coverage` | ubuntu-24.04 | Code coverage (lcov) | Informational |
| `clang-tidy` | ubuntu-24.04 | Static analysis | Warning only |
| `todo-count` | ubuntu-24.04 | TODO/FIXME threshold (max 20) | Warning |
| `loc-counter` | ubuntu-24.04 | Lines of code stats | Informational |

### Reproducing CI Locally

**Format check:**
```bash
find SparkEngine/Source GameModules/SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror
```

**Linux GCC build:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests
```

**Linux ASan build:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build && ./bin/SparkTests
```

**Windows MSVC:**
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DSPARK_MSVC_TOOLSET=v143 -DBUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### Build Artifacts

| Artifact | Retention |
|----------|-----------|
| `SparkEngine-Windows-VS2022-Release.zip` | 14 days |
| `SparkEngine-Linux-GCC-Release.tar.gz` | 14 days |
| `SparkEngine-Linux-Clang-Release.tar.gz` | 14 days |
| `coverage-report` (lcov) | 14 days |

---

## SparkShaderCompiler

**File:** `SparkShaderCompiler/src/main.cpp`

Standalone CLI tool for offline shader compilation:

```bash
# Single file
SparkShaderCompiler input.hlsl -o output.cso -stage vertex -backend D3D11

# Batch compilation
SparkShaderCompiler -batch Shaders/HLSL -backend vulkan

# Validate only
SparkShaderCompiler input.hlsl -validate

# Reflection data
SparkShaderCompiler input.hlsl -reflect
```

### Options

| Flag | Description |
|------|-------------|
| `-o <path>` | Output file |
| `-stage <stage>` | Vertex, Pixel, Geometry, Hull, Domain, Compute, RT stages |
| `-backend <api>` | D3D11, D3D12, Vulkan, OpenGL, Auto |
| `-entry <name>` | Entry point (default: "main") |
| `-D<DEFINE>` | Preprocessor define |
| `-I<path>` | Include search path |
| `-O` / `-Od` | Enable/disable optimization |
| `-Zi` | Debug info |
| `-validate` | Validate only |
| `-reflect` | Print reflection data |
| `-batch <dir>` | Batch compile directory |
| `-v` | Verbose output |

---

## Documentation Scripts

### API Documentation Generator

```bash
docs/generate-api-docs.sh generate   # Full generation (~250 headers → ~240 pages)
docs/generate-api-docs.sh check      # Incremental (checksum-based)
docs/generate-api-docs.sh status     # Show stats
```

### Wiki Synchronization

```bash
docs/sync-wiki.sh sync    # Update auto-generated sections
docs/sync-wiki.sh check   # Dry-run (exit 1 if stale)
docs/sync-wiki.sh status  # Show codebase + wiki stats
```

### Doxygen (Legacy)

```bash
cd docs && ./generate-docs.sh   # Full HTML output (requires doxygen + graphviz)
```
