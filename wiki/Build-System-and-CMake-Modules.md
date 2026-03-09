# Build System and CMake Modules

SparkEngine uses CMake 3.16+ as its build system with 30+ toggleable feature modules, cross-platform presets, and CI/CD integration.

**Source:** `CMakeLists.txt`, `cmake/`, `CMakePresets.json`

## CMake Configuration

### Minimum Requirements

- CMake 3.16+
- C++20 standard (enforced, no extensions)
- CMP0091 policy for consistent MSVC runtime

### Quick Configuration

```bash
# Windows (Visual Studio 2022)
.\generate.bat -g "Visual Studio 17 2022" release

# Linux (Ninja)
./generate.sh release -g Ninja

# Direct CMake
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
```

## Feature Flags

All flags can be set during CMake configuration with `-D<FLAG>=ON|OFF`:

### Graphics

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_GRAPHICS` | ON | Graphics rendering engine |
| `ENABLE_VULKAN` | ON | Vulkan backend |
| `ENABLE_OPENGL` | ON | OpenGL backend |
| `ENABLE_DXR` | OFF | DirectX Raytracing (requires D3D12) |
| `ENABLE_POST_PROCESSING` | ON | Post-processing effects |
| `ENABLE_LIGHTING_SYSTEM` | ON | Advanced lighting |
| `ENABLE_MESH_LOD` | ON | Mesh level-of-detail |
| `ENABLE_DECALS` | ON | Decal system |
| `ENABLE_FOG_SYSTEM` | ON | Fog rendering |
| `ENABLE_SCREEN_SPACE` | ON | SSAO, SSR effects |

### Gameplay

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_PHYSX` | ON | Physics engine (Bullet) |
| `ENABLE_AI` | ON | AI and navigation |
| `ENABLE_ANIMATION` | ON | Skeletal animation |
| `ENABLE_TERRAIN_SYSTEM` | ON | Heightmap terrain |
| `ENABLE_WEATHER` | ON | Weather system |
| `ENABLE_INVENTORY` | ON | Inventory system |
| `ENABLE_QUEST_SYSTEM` | ON | Quest/objective tracking |
| `ENABLE_DAY_NIGHT` | ON | Day/night cycle |
| `ENABLE_SAVE_SYSTEM` | ON | Save/load system |
| `ENABLE_PROCEDURAL` | ON | Procedural generation |
| `ENABLE_CINEMATIC` | ON | Cinematic sequencer |
| `ENABLE_EVENT_SYSTEM` | ON | Event bus |

### Features

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_EDITOR` | ON (Windows) | ImGui editor |
| `ENABLE_PROFILING` | ON | Performance profiler |
| `ENABLE_HOT_RELOAD` | ON | Script hot-reload |
| `ENABLE_ASSET_STREAMING` | ON | Runtime asset streaming |
| `ENABLE_ADVANCED_INPUT` | ON | Advanced input features |
| `ENABLE_PERF_STATS` | ON | Performance statistics overlay |
| `ENABLE_LUA` | ON | Lua scripting support |
| `ENABLE_COLLABORATIVE` | ON | Collaborative features |

### External / Disabled by Default

| Flag | Default | Description |
|------|---------|-------------|
| `ENABLE_NETWORKING` | OFF | Networking (CURL dependency issues) |
| `ENABLE_SDL2` | OFF | SDL2 cross-platform input |
| `BUILD_TESTS` | ON | Unit test suite (see [Testing](Testing)) |

### MSVC Toolset

```bash
cmake -DSPARK_MSVC_TOOLSET=v144 ...  # VS 2026
cmake -DSPARK_MSVC_TOOLSET=v143 ...  # VS 2022 (default)
```

## CMake Presets

`CMakePresets.json` provides ready-made configurations:

| Preset | Description |
|--------|-------------|
| `windows-debug` | Windows, VS 2022, Debug |
| `windows-release` | Windows, VS 2022, Release |
| `linux-gcc-debug` | Linux, GCC, Debug |
| `linux-gcc-release` | Linux, GCC, Release |
| `linux-clang-debug` | Linux, Clang, Debug |
| `linux-clang-release` | Linux, Clang, Release |
| `ci-linux-asan` | AddressSanitizer build |
| `ci-linux-tsan` | ThreadSanitizer build |
| `minimal` | Core-only, no advanced features |

```bash
cmake --preset windows-release
cmake --build --preset windows-release
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

## Build Targets

| Target | Type | Description |
|--------|------|-------------|
| `SparkEngineLib` | Static library | Core engine systems |
| `SparkEngine` | Executable | Runtime host |
| `SparkGame` | Shared library | Default game module |
| [SparkEditor](SparkEditor) | Executable | Visual editor (Windows) |
| [SparkConsole](SparkConsole) | Executable | Debug console (Windows) |
| `SparkShaderCompiler` | Executable | Shader compilation tool |

## Build Output

```
build/
├── bin/       # Executables and DLLs
├── lib/       # Static libraries
├── Shaders/   # Compiled shaders
└── Assets/    # Game assets (copied)
```

## CMake Helper Modules

### SparkGameModule.cmake

Provides `spark_add_game_module()` for creating game module DLLs:

```cmake
include(cmake/SparkGameModule.cmake)
spark_add_game_module(MyGame ${GAME_SOURCES})
```

This:
- Creates a shared library with module API exports
- Links against SparkEngineLib
- Handles platform-specific compilation flags

### SparkEnginePreflight.cmake

Validates that a SparkEngine SDK installation is complete before `find_package` runs. Include it in standalone projects to get clear error messages when `SparkEngineTargets.cmake` or `SparkGameModule.cmake` is missing (e.g. due to an interrupted install or pointing at a build tree):

```cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(SparkEnginePreflight)

find_package(SparkEngine REQUIRED)
```

If the installation is incomplete, the preflight check emits a `FATAL_ERROR` with the exact missing file and remediation steps.

### SparkEngineConfig.cmake

Enables `find_package(SparkEngine)` for standalone game projects.

## Build Scripts

| Script | Platform | Description |
|--------|----------|-------------|
| `generate.bat` | Windows | CMake configuration |
| `generate.sh` | Linux/macOS | CMake configuration |
| `build.ps1` | Windows | PowerShell build script |
| `build.sh` | Linux/macOS | Bash build script |

## CI/CD (GitHub Actions)

### build.yml

Runs on every push to `main`, `develop`, and `feature/*` branches:
- **Platforms:** Windows (VS 2022), Linux (GCC, Clang)
- **Configurations:** Debug and Release matrix
- **Artifacts:** Retained for 7 days

### release.yml

Runs on every push to `master`/`main`:
- Creates rolling `latest` GitHub Release
- Packages: Windows and Linux binaries (Debug + Release)
- Formats: `.zip` (Windows), `.tar.gz` (Linux)
- Includes exact commit hash and timestamp

### update-sparkbuild.yml

Runs weekly (every Monday at 06:00 UTC) or on manual dispatch:
- Downloads the latest [SparkBuild](https://github.com/Krilliac/SparkBuild) release binary
- Compares SHA-256 checksums to detect changes
- Opens a PR to update `tools/SparkBuild.exe` when a new version is available

## Compiler Support

| Compiler | Version | Platform |
|----------|---------|----------|
| MSVC | v143 (VS 2022) | Windows |
| MSVC | v144 (VS 2026) | Windows (experimental) |
| GCC | 11+ | Linux |
| Clang | 14+ | Linux |
| Apple Clang | C++20 capable | macOS |

---

## See Also

- [Getting Started](Getting-Started) — Build quickstart
- [Testing](Testing) — Running unit tests
- [Architecture Overview](Architecture-Overview) — Engine design and subsystems
- [Contributing](Contributing) — Contribution workflow and code style
