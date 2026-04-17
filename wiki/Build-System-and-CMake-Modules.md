# Build System and CMake Modules

SparkEngine uses CMake 3.16+ as its build system with 30+ toggleable feature modules, cross-platform presets, and CI/CD integration.

**Source:** `CMakeLists.txt`, `cmake/`, `CMakePresets.json`

## CMake Configuration

### Minimum Requirements

- CMake 3.25+
- C++23 standard (enforced via `cxx_std_23`, no extensions)
- CMP0091 policy for consistent MSVC runtime library selection

### Quick Configuration

```bash
# Windows (Visual Studio 2022)
.\generate.bat -g "Visual Studio 17 2022" release

# Linux (Ninja)
./generate.sh release -g Ninja

# Direct CMake
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

# Using presets (recommended)
cmake --preset windows-release
cmake --build --preset windows-release
```

### Configuration Flow

```
cmake -B build [options]
    │
    ├── Check CMake version (>= 3.25)
    ├── Set C++23 standard (no extensions)
    ├── Apply CMP0091 policy (MSVC runtime)
    │
    ├── Evaluate feature flags (-DENABLE_*)
    │   ├── ON  → include subsystem sources, define compile macros
    │   └── OFF → skip subsystem, set stub/no-op implementations
    │
    ├── Detect platform and compiler
    │   ├── MSVC → /W4, MSVC runtime selection
    │   ├── GCC  → -Wall -Wextra
    │   └── Clang → -Wall -Wextra
    │
    ├── Configure targets (SparkEngineLib, SparkEngine, SparkGame, ...)
    ├── Include component libraries (cmake/SparkComponentLibraries.cmake)
    ├── Configure tests (if BUILD_TESTS=ON)
    └── Generate build files
```

## Feature Flags

All flags can be set during CMake configuration with `-D<FLAG>=ON|OFF`.

### Graphics Flags

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_GRAPHICS` | ON | Graphics rendering engine (DX11, Vulkan, GL) |
| `ENABLE_VULKAN` | ON | Vulkan backend |
| `ENABLE_OPENGL` | ON | OpenGL backend (supports CPU software rendering via Mesa llvmpipe) |
| `ENABLE_DXR` | ON | DirectX Raytracing (Windows/D3D12; SDFGI fallback on other platforms) |
| `SPARK_HEADLESS_SUPPORT` | ON | Headless/dedicated server mode support |
| `ENABLE_POST_PROCESSING` | ON | Post-processing effects (bloom, SSAO, etc.) |
| `ENABLE_LIGHTING_SYSTEM` | ON | Advanced lighting (PBR, IBL) |
| `ENABLE_MESH_LOD` | ON | Mesh level-of-detail |
| `ENABLE_DECALS` | ON | Projected decal system |
| `ENABLE_FOG_SYSTEM` | ON | Fog rendering (distance, height) |
| `ENABLE_SCREEN_SPACE` | ON | Screen-space effects (SSAO, SSR) |

### Gameplay Flags

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_PHYSX` | ON | Physics engine (Jolt Physics) |
| `ENABLE_AI` | ON | AI and navigation (behavior trees, NavMesh) |
| `ENABLE_ANIMATION` | ON | Skeletal animation (blending, IK, state machines) |
| `ENABLE_TERRAIN_SYSTEM` | ON | Heightmap terrain rendering and generation |
| `ENABLE_WEATHER` | ON | Dynamic weather system |
| `ENABLE_INVENTORY` | ON | Item inventory system |
| `ENABLE_QUEST_SYSTEM` | ON | Quest/objective tracking |
| `ENABLE_DAY_NIGHT` | ON | Day/night cycle with time-of-day lighting |
| `ENABLE_SAVE_SYSTEM` | ON | Game state save/load |
| `ENABLE_PROCEDURAL` | ON | Procedural generation (noise, erosion, WFC) |
| `ENABLE_CINEMATIC` | ON | Cinematic sequencer |
| `ENABLE_EVENT_SYSTEM` | ON | Publish/subscribe event bus |

### Feature Flags

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_EDITOR` | ON (Windows) | ImGui-based visual editor |
| `ENABLE_PROFILING` | ON | Performance profiler integration |
| `ENABLE_HOT_RELOAD` | ON | AngelScript hot-reload during development |
| `ENABLE_ASSET_STREAMING` | ON | Runtime asset streaming |
| `ENABLE_ADVANCED_INPUT` | ON | Advanced input features (rebinding, combos) |
| `ENABLE_PERF_STATS` | ON | Performance statistics overlay |
| `ENABLE_COLLABORATIVE` | ON | Collaborative editing features |

### External / Disabled by Default

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_NETWORKING` | ON | Networking subsystem (UDP sockets, no external dependencies) |
| `ENABLE_SDL2` | OFF | SDL2 cross-platform input (alternative to native) |
| `BUILD_TESTS` | ON | Unit test suite (see [Testing](Testing)) |

### Minimal Build Example

Disable most features for a core-only build:

```bash
cmake -B build \
    -DENABLE_GRAPHICS=OFF \
    -DENABLE_AI=OFF \
    -DENABLE_ANIMATION=OFF \
    -DENABLE_EDITOR=OFF \
    -DENABLE_WEATHER=OFF \
    -DENABLE_TERRAIN_SYSTEM=OFF \
    -DENABLE_CINEMATIC=OFF
```

Or use the `minimal` preset:

```bash
cmake --preset minimal
```

### Headless / Software Rendering Build (Linux)

Build with OpenGL enabled for CPU-based software rendering via Mesa llvmpipe:

```bash
# Install dependencies
sudo apt-get install -y libgl-dev libx11-dev

# Configure with OpenGL backend
cmake -B build -DENABLE_OPENGL=ON -DBUILD_TESTS=ON

# Build
cmake --build build --config Release

# Run with software rendering (no GPU required)
Xvfb :99 -screen 0 1024x768x24 &
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./build/bin/SparkEngine
```

The GLAD OpenGL loader is bundled in `ThirdParty/glad/` and detected automatically. When no GPU backend is available at runtime, the engine automatically falls back to `NullRHIDevice` (headless no-op mode).

### Third-Party Manifest + Configure-Time Audit

SparkEngine now tracks third-party dependency metadata in `ThirdParty/dependencies.lock` (single source of truth).

Each manifest row declares:
- dependency name + upstream source URL/repo,
- exact pinned version/tag/commit,
- license,
- local path,
- fallback/shim behavior (`SPARK_HAS_*` macro + stub/fallback path).

`cmake/SparkThirdPartyAudit.cmake` is invoked from root `CMakeLists.txt` early in configure and:
- validates required files exist for each declared dependency,
- prints source/version/license summary during configure,
- warns on manifest mismatches (and can be made fatal with `-DSPARK_STRICT_DEPS=ON`).

CI enforces manifest hygiene via `tools/check-thirdparty-manifest-sync.sh`: dependency path/URL/version wiring changes must include a matching `ThirdParty/dependencies.lock` update.

### MSVC Toolset Selection

```bash
cmake -DSPARK_MSVC_TOOLSET=v143 ...  # VS 2022 (default)
cmake -DSPARK_MSVC_TOOLSET=v144 ...  # VS 2026
```

## CMake Presets

`CMakePresets.json` provides ready-made configurations:

| Preset | Generator | Compiler | Config | Notes |
|--------|-----------|----------|--------|-------|
| `windows-debug` | VS 2022 | MSVC | Debug | Full features |
| `windows-release` | VS 2022 | MSVC | Release | Full features |
| `linux-gcc-debug` | Ninja | GCC | Debug | Full features |
| `linux-gcc-release` | Ninja | GCC | Release | Full features |
| `linux-clang-debug` | Ninja | Clang | Debug | Full features |
| `linux-clang-release` | Ninja | Clang | Release | Full features |
| `ci-linux-asan` | Ninja | GCC | Debug | AddressSanitizer + UBSan |
| `ci-linux-tsan` | Ninja | GCC | Debug | ThreadSanitizer |
| `linux-mingw-release` | Makefiles | MinGW-w64 | Release | Cross-compile Windows D3D11/D3D12, run under Wine |
| `linux-mingw-debug` | Makefiles | MinGW-w64 | Debug | Cross-compile Windows D3D11/D3D12, run under Wine |
| `minimal` | Default | Default | Release | Core-only, no advanced features |

```bash
# List available presets
cmake --list-presets

# Configure and build with a preset
cmake --preset windows-release
cmake --build --preset windows-release
```

## Cross-Compilation: Windows on Linux (MinGW + Wine)

SparkEngine supports cross-compiling the Windows D3D11 code paths on a Linux host using MinGW-w64. The resulting `.exe` files run under Wine, with DXVK translating D3D11 calls to Vulkan (or WineD3D as fallback). Combined with Mesa Lavapipe, this enables full D3D11 testing without a GPU.

> **Full guide:** See [Cross-Compilation: Wine Testing](Cross-Compilation-Wine-Testing) for complete setup, troubleshooting, and the automated test suite.

### Quick Start

```bash
# 1. Install prerequisites
sudo apt-get install mingw-w64 wine64 mesa-vulkan-drivers

# 2. Install DirectXMath headers (MinGW doesn't ship them)
# See the full guide for download instructions
sudo cp DirectX*.h DirectX*.inl /usr/x86_64-w64-mingw32/include/

# 3. Build
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --parallel $(nproc)

# 4. Run tests under Wine
tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe

# 5. Run automated test suite (7 phases)
python3 tools/test-windows-wine.py --build-dir build/linux-mingw-release
```

### What Gets Built

5 Windows executables + game module DLLs: `SparkEngine.exe`, `SparkEditor.exe`, `SparkTests.exe`, `SparkConsole.exe`, `SparkShaderCompiler.exe`

### Key Differences from MSVC

- **D3D12 excluded** (MinGW headers too old for ID3D12Device5/DXR)
- **D3D11 fully supported** (primary backend, works under Wine + DXVK)
- **DirectXMath** must be installed manually to the MinGW sysroot
- **`-municode`** required for `wWinMain` Unicode entry point

### Key Files

| File | Purpose |
|------|---------|
| `cmake/toolchains/mingw-w64-x86_64.cmake` | CMake toolchain for MinGW cross-compilation |
| `tools/wine-run.sh` | Wine runner with DXVK/VKD3D-Proton auto-setup |
| `tools/test-windows-wine.py` | Automated 7-phase Wine test suite |
| `CMakePresets.json` | `linux-mingw-release` / `linux-mingw-debug` presets |

### Software Rendering Fallback (All Backends)

Every RHI backend gracefully falls back to software rendering when no GPU is available:

| Backend | Platform | Software Fallback |
|---------|----------|-------------------|
| D3D11 | Windows | WARP (`D3D_DRIVER_TYPE_WARP`, built into Windows 10+) |
| D3D12 | Windows | WARP (`EnumWarpAdapter()`, built into Windows 10+) |
| Vulkan | Cross-platform | Mesa Lavapipe (`VK_PHYSICAL_DEVICE_TYPE_CPU`) |
| OpenGL | Linux | Mesa llvmpipe (EGL headless or GLX + Xvfb) |
| None | All | NullRHIDevice (no-op headless mode) |

All backends expose `isSoftwareDevice` in `RHIDeviceCapabilities` and report it in `GetDeviceInfo()`.

## Build Targets

| Target | Type | Description |
|--------|------|-------------|
| `SparkEngineLib` | Static library | Core engine systems (all subsystems linked here) |
| `SparkEngine` | Executable | Runtime host (loads game modules) |
| `SparkGame` | Shared library | Default game module DLL |
| [SparkEditor](SparkEditor) | Executable | ImGui visual editor (Windows only) |
| [SparkConsole](SparkConsole) | Executable | Standalone debug console (Windows only) |
| `SparkShaderCompiler` | Executable | Offline HLSL/GLSL shader compilation tool |
| `SparkTests` | Executable | Unit test runner (when `BUILD_TESTS=ON`) |

## Build Output

```
build/
├── bin/           # Executables and DLLs
│   ├── SparkEngine.exe
│   ├── SparkGame.dll
│   ├── SparkEditor.exe
│   ├── SparkConsole.exe
│   └── SparkTests.exe
├── lib/           # Static libraries (.lib / .a)
│   └── SparkEngineLib.lib
├── Shaders/       # Compiled shader bytecode
└── Assets/        # Game assets (copied from source tree)
```

## SparkBuild

[SparkBuild](https://github.com/Krilliac/SparkBuild) is the standalone C++ build tool for SparkEngine. A pre-built binary ships in `tools/SparkBuild.exe` so you can use it immediately without compiling it yourself.

**Repository:** [Krilliac/SparkBuild](https://github.com/Krilliac/SparkBuild)
**Current release:** v1.0.0

### Updating SparkBuild

The binary is kept up to date automatically via the [`update-sparkbuild`](../.github/workflows/update-sparkbuild.yml) GitHub Action, which checks for new releases every Monday and opens a PR when a newer binary is available.

You can also update manually:

```powershell
# Windows (PowerShell)
.\tools\update-sparkbuild.ps1            # latest release
.\tools\update-sparkbuild.ps1 v1.0.0     # specific version
```

```bash
# Linux / macOS
./tools/update-sparkbuild.sh             # latest release
./tools/update-sparkbuild.sh v1.0.0      # specific version
```

## CMake Helper Modules

### SparkGameModule.cmake

**Source:** `cmake/SparkGameModule.cmake`

Provides `spark_add_game_module()` for creating game module DLLs:

```cmake
include(cmake/SparkGameModule.cmake)
spark_add_game_module(MyGame ${GAME_SOURCES})
```

#### What it does

| Step | Action |
|------|--------|
| 1 | Creates a `SHARED` library from provided sources |
| 2 | Defines `SPARK_MODULE_DLL` and `SPARK_GAME_DLL` compile definitions |
| 3 | Links against `Spark::SparkEngineLib` (private) |
| 4 | Sets SDK include directories (`SPARK_ENGINE_INCLUDE_DIR`, `Spark/`, `SparkEngine/`) |
| 5 | Enforces C++23 via `target_compile_features(cxx_std_23)` |
| 6 | On MSVC: sets runtime library to `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` |

See [Creating a Game Module](Creating-a-Game-Module) for a complete usage guide.

### SparkEnginePreflight.cmake

**Source:** `cmake/SparkEnginePreflight.cmake`

Validates that a SparkEngine SDK installation is complete before `find_package` runs. Include it in standalone projects to get clear error messages when required files are missing.

```cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(SparkEnginePreflight)

find_package(SparkEngine REQUIRED)
```

#### Validation checks

The `spark_preflight_check()` function searches candidate directories and verifies:

| Required File | Purpose |
|---------------|---------|
| `SparkEngineConfig.cmake` | CMake package configuration |
| `SparkEngineTargets.cmake` | Imported target definitions for `Spark::SparkEngineLib` |
| `SparkGameModule.cmake` | Game module helper function |

If any file is missing, it emits a `FATAL_ERROR` with the exact missing path and remediation steps (re-run `cmake --install`).

### SparkComponentLibraries.cmake

**Source:** `cmake/SparkComponentLibraries.cmake`

Defines per-subsystem OBJECT library targets that partition the monolithic `SparkEngineLib` into fine-grained component libraries.

```cmake
include(cmake/SparkComponentLibraries.cmake)
spark_define_component_libraries()
```

#### Component Library Targets

| Target | Sources | Dependencies | Description |
|--------|---------|-------------|-------------|
| `SparkCore` | `Core/*.cpp/.h` | -- | Platform, EngineContext, Assert, Logger |
| `SparkUtils` | `Utils/*.cpp/.h` | SparkCore | Console, Profiler, JobSystem, Octree |
| `SparkECS` | `Engine/ECS/**/*.cpp/.h` | SparkCore | Components, Systems, World, ReactiveSystem |
| `SparkGraphics` | `Graphics/**/*.cpp/.h` | SparkCore, SparkUtils | GraphicsEngine, RHI, Shaders, PostProcessing |
| `SparkPhysics` | `Engine/Physics/**/*.cpp/.h` | SparkCore, SparkECS | PhysicsSystem, Jolt Physics integration |
| `SparkAudio` | `Engine/Audio/**/*.cpp/.h` | SparkCore, SparkECS | AudioEngine, XAudio2 |
| `SparkAI` | `Engine/AI/**/*.cpp/.h` | SparkCore, SparkECS, SparkUtils | AISystem, BehaviorTree, NavMesh |
| `SparkAnimation` | `Engine/Animation/**/*.cpp/.h` | SparkCore, SparkECS | Skeleton, Clips, Blending, IK |
| `SparkNetworking` | `Engine/Networking/**/*.cpp/.h` | SparkCore | NetworkManager, Transport |

#### Migration from Monolithic to Component Libraries

```cmake
# Before (monolithic)
target_link_libraries(MyTarget PRIVATE SparkEngineLib)

# After (component-based — link only what you need)
target_link_libraries(MyTarget PRIVATE SparkECS SparkGraphics SparkPhysics)
```

`SparkEngineLib` remains as an umbrella target that links all components for backward compatibility.

### SparkEngineConfig.cmake

Enables `find_package(SparkEngine)` for standalone game projects. Defines the `Spark::SparkEngineLib` imported target.

## Build Scripts

| Script | Platform | Description |
|--------|----------|-------------|
| `generate.bat` | Windows | CMake configuration wrapper (accepts generator and config) |
| `generate.sh` | Linux/macOS | CMake configuration wrapper |
| `build.ps1` | Windows | PowerShell build script |
| `build.sh` | Linux/macOS | Bash build script |

## Documentation Generation

```bash
# Generate API docs from headers (no Doxygen required)
docs/generate-api-docs.sh generate    # Full generation
docs/generate-api-docs.sh check       # Only if headers changed (checksum-based)
docs/generate-api-docs.sh status      # Show generation status

# Sync wiki pages with codebase inventory
docs/sync-wiki.sh sync               # Update auto-generated sections
docs/sync-wiki.sh check              # Dry-run: report what's stale

# Legacy Doxygen (optional, requires doxygen + graphviz)
cd docs && ./generate-docs.sh
```

See the [Documentation Index](../docs/README.md) and the [Doc Tooling page](../docs/tooling/README.md) for full documentation tooling details.

## CI/CD (GitHub Actions)

### build.yml

Runs on every push to `main`, `develop`, and `feature/*` branches, and on all pull requests.

#### CI Job Matrix

| Job | Runner | Compiler | Configs | Key Flags |
|-----|--------|----------|---------|-----------|
| `check-format` | ubuntu-24.04 | clang-format | -- | `--dry-run --Werror` |
| `validate-prompts` | ubuntu-24.04 | -- | -- | `--ci` |
| `build-linux-gcc` | ubuntu-24.04 | GCC | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-clang` | ubuntu-24.04 | Clang | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-asan` | ubuntu-24.04 | GCC | Debug | ASan + UBSan |
| `build-windows-vs2022` | windows-latest | MSVC v143 | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-windows-vs2026` | windows-latest | MSVC v144 | Debug, Release | `continue-on-error` |
| `coverage` | ubuntu-24.04 | GCC | Debug | `--coverage` + lcov |
| `clang-tidy` | ubuntu-24.04 | Clang | Debug | `continue-on-error` |
| `todo-count` | ubuntu-24.04 | -- | -- | threshold: 20 |

#### Reproducing CI Builds Locally

**Linux GCC:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**Linux Clang:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**AddressSanitizer (ASan + UBSan):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**Windows MSVC (VS 2022):**
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 \
    -DSPARK_MSVC_TOOLSET=v143 -DBUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### release.yml

Runs on every push to `master`/`main`:
- Creates rolling `latest` GitHub Release
- Packages: Windows and Linux binaries (Debug + Release)
- Formats: `.zip` (Windows), `.tar.gz` (Linux)
- Includes exact commit hash and timestamp

### update-sparkbuild.yml

Runs weekly (every Monday at 06:00 UTC) or on manual dispatch:
- Downloads the latest SparkBuild release binary
- Compares SHA-256 checksums to detect changes
- Opens a PR to update `tools/SparkBuild.exe` when a new version is available

## Compiler Support

| Compiler | Version | Platform | Status |
|----------|---------|----------|--------|
| MSVC | v143 (VS 2022) | Windows | Fully supported |
| MSVC | v144 (VS 2026) | Windows | Experimental (`continue-on-error`) |
| GCC | 13+ | Linux | Fully supported |
| Clang | 17+ | Linux | Fully supported |
| Apple Clang | C++23 capable | macOS | Experimental |

### Compiler Flags

| Compiler | Warning Flags | Additional |
|----------|--------------|------------|
| MSVC | `/W4` | Zero warnings enforced |
| GCC | `-Wall -Wextra` | Zero warnings enforced |
| Clang | `-Wall -Wextra` | Zero warnings enforced |

## Code Formatting

SparkEngine enforces consistent code style via `.clang-format` (Microsoft-based, Allman braces, 120-col, 4-space indent).

```bash
# Check formatting (dry run)
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules/SparkGame/Source \
  -name '*.h' -o -name '*.cpp' | head -50 | xargs clang-format --dry-run --Werror 2>&1

# Fix formatting automatically
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules/SparkGame/Source \
  -name '*.h' -o -name '*.cpp' | xargs clang-format -i
```

CI rejects PRs with formatting violations.

---

## See Also

- [Getting Started](Getting-Started) -- Build quickstart
- [Testing](Testing) -- Running unit tests
- [Architecture Overview](Architecture-Overview) -- Engine design and subsystems
- [Contributing](Contributing) -- Contribution workflow and code style
- [Creating a Game Module](Creating-a-Game-Module) -- Building game modules
