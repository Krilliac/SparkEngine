# Build System and CMake Modules

SparkEngine uses CMake 3.25+ as its build system with documented options, cross-platform presets, and CI/CD integration.

**Source:** `CMakeLists.txt`, `cmake/`, `CMakePresets.json`, `Tools/buildmatrix/`

> **Release boundary:** Presets and compiler paths document configuration
> coverage, not certification. Only Windows 11 x64 with MSVC v143 is declared in
> `stable-v1`, and that profile remains blocked and uncertified. Linux, macOS,
> MinGW/Wine, other compiler lines, and non-D3D11 graphics paths stay outside it.

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
    ├── Evaluate declared CMake options
    │   ├── consumed options alter definitions, dependencies, or selected targets
    │   └── inert cache variables do not remove a subsystem (tracked by HEAD-220)
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

## Selected Root Build Options

Only options declared and consumed by the current root `CMakeLists.txt` have a
proven build effect. Setting an arbitrary `ENABLE_*` cache variable does not remove
a subsystem. The table below intentionally avoids undocumented pseudo-options.

| Option | Root default | Source-backed effect when disabled |
|--------|--------------|------------------------------------|
| `ENABLE_GRAPHICS` | ON | Currently inert; OFF does not remove graphics/RHI (`HEAD-220`) |
| `ENABLE_VULKAN` | ON | Disables Vulkan discovery and omits `SPARK_VULKAN_SUPPORT`; root source glob remains |
| `ENABLE_OPENGL` | ON | Disables OpenGL discovery/enablement; verify host context separately |
| `ENABLE_METAL` | ON on Apple, OFF elsewhere | Omits Metal enablement on Apple development builds |
| `ENABLE_DXR` | ON | Skips DXR shader compilation and hardware-RT definition setup |
| `ENABLE_HYBRID_RT` | ON | Omits `SPARK_HYBRID_RT` and its hardware-RT definition setup |
| `ENABLE_NETWORKING` | ON | Omits networking definition/libraries from `SparkEngineLib`; service targets use `ENABLE_SERVER_PROCESSES` |
| `SPARK_HEADLESS_SUPPORT` | ON | Omits the compile definition; host entry sources are still listed |
| `ENABLE_EDITOR` | ON | Omits the SparkEditor target |
| `BUILD_TESTS` | ON | Omits test targets |
| `BUILD_GAME_MODULES` | ON | Omits in-tree game-module targets |

### Reduced Development Preset

The `minimal` preset currently disables networking and DXR. Several additional
preset cache variables have no matching root option and are inert, so this is not
a core-only build contract. Disable declared targets such as the editor explicitly
when needed; `HEAD-220` tracks a true stripped/headless configuration.

```bash
cmake --preset minimal
cmake -B build -DENABLE_EDITOR=OFF
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

The GLAD OpenGL loader is bundled in `ThirdParty/glad/`. A bare
`RHIFactory::CreateDevice` call does not retry a failed backend, but runtime
`RHIBridge::Initialize` retries its available GPU candidates after device or
swap-chain setup fails and creates `NullRHIDevice` only after all candidates
fail. That no-render resilience path is in `stable-v1` only on Windows 11 x64;
other hosts are uncertified.

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
cmake -G "Visual Studio 17 2022" -A x64 -T v143 ...  # VS 2022
cmake -G "Visual Studio 18 2026" -A x64 ...          # VS 2026 (v145 default)
```

Toolset and platform are generator inputs; select them with `-T`/`-A` or the preset `toolset`/`architecture` fields, not project cache variables.

## CMake Presets

`CMakePresets.json` provides ready-made configurations:

Preset availability is configuration coverage, not release certification. The
Windows 11 x64/v143 line is the only toolchain family in `stable-v1`, which remains
blocked; Linux and MinGW/Wine rows are development paths outside the profile.

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
| `linux-mingw-release` | Makefiles | MinGW-w64 | Release | Cross-compile the Windows D3D11 path; D3D12/DXR are excluded; run under Wine |
| `linux-mingw-debug` | Makefiles | MinGW-w64 | Debug | Cross-compile the Windows D3D11 path; D3D12/DXR are excluded; run under Wine |
| `minimal` | Default | Default | Release | Core runtime with optional tools, modules, scripting, networking, and experimental rendering breadth disabled |

```bash
# List available presets
cmake --list-presets

# Configure and build with a preset
cmake --preset windows-release
cmake --build --preset windows-release
```

### CI-120 configured-target evidence

CI-120 uses CMake File API replies to prove which targets a concrete build profile actually configured. The capture command creates a unique stateful File API query, invokes the canonical configure itself, and records only the matching reply transaction before inventory generation:

```bash
python3 Tools/buildmatrix/capture_provenance.py \
  --profile windows-shipping \
  --build-dir build/windows-shipping
python3 Tools/buildmatrix/inventory.py \
  --codemodel windows-shipping=build/windows-shipping \
  --output docs/readiness/ci120-build-matrix-inventory.json
python3 Tools/buildmatrix/check_parity.py \
  --inventory docs/readiness/ci120-build-matrix-inventory.json \
  --baseline docs/readiness/ci120-parity-findings.json
```

The capture step requires an exactly clean repository, derives the commit itself, creates the query before configuring, and binds the profile, source/build directories, CMake executable and version, generator, cache values, and exact index/codemodel/cache/target reply digests. Inventory rejects missing, malformed, oversized, linked, out-of-directory, changed, unbound, or post-hoc reply data. Caller-supplied commit text is not provenance, and missing material fields or expected cache values remain blocking findings.

The local record detects reply substitution but cannot authorize itself. `CI-120 Trusted Verifier` runs later from the exact current `Working` default-branch workflow, binds one source run/job/artifact through the Actions API, downloads by immutable artifact ID with digest mismatch failure, reparses the File API replies as bounded data, rehashes every declared product, reconstructs the inventory and parity report with trusted code, and attests only the resulting verified receipt. During the initial rollout the producer remains deliberately red at its external-authority enforcement step; CI-120 is not promoted until a remote verifier run proves this path end to end.

## Cross-Compilation: Windows on Linux (MinGW + Wine)

The repository can cross-compile Windows D3D11 code paths on a Linux host using MinGW-w64 for development evaluation. The resulting `.exe` files run under Wine, with DXVK translating D3D11 calls to Vulkan (or WineD3D as fallback). Combined with Mesa Lavapipe, this exercises D3D11 paths without a GPU; it does not certify D3D11 or a Linux host for `stable-v1`.

> **Full guide:** See [Cross-Compilation: Wine Testing](../platform/Cross-Compilation-Wine-Testing.md) for complete setup, troubleshooting, and the automated test suite.

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
- **D3D11 paths exercised** under Wine + DXVK; this is outside `stable-v1` and not certification
- **DirectXMath** must be installed manually to the MinGW sysroot
- **`-municode`** required for `wWinMain` Unicode entry point

### Key Files

| File | Purpose |
|------|---------|
| `cmake/toolchains/mingw-w64-x86_64.cmake` | CMake toolchain for MinGW cross-compilation |
| `tools/wine-run.sh` | Wine runner with DXVK/VKD3D-Proton auto-setup |
| `tools/test-windows-wine.py` | Automated 7-phase Wine test suite |
| `CMakePresets.json` | `linux-mingw-release` / `linux-mingw-debug` presets |

### Development Software/No-Render Routes and Bridge Fallback

The routes below are configuration/development paths, not universal host
certification. `RHIBridge::Initialize` tries the available GPU candidates in
order after device or swap-chain failure, then creates `NullRHIDevice` if none
succeeds; the bare `RHIFactory::CreateDevice` operation has no retry loop. The
bridge does not probe every conceptual backend row regardless of what was
compiled or made available.

| Backend | Platform | Software Fallback |
|---------|----------|-------------------|
| D3D11 | Windows | WARP route where explicitly selected by the backend |
| D3D12 | Windows | WARP adapter route where explicitly selected by the backend |
| Vulkan | Host/runtime dependent | A CPU ICD such as Lavapipe, only when installed and selected |
| OpenGL | Linux development | Mesa llvmpipe with the required EGL/GLX/display setup |
| Metal | macOS | No software fallback declared |
| None | Host-independent implementation | Explicit `NullRHIDevice`; no pixels are rendered |

`RHIDeviceCapabilities` contains an `isSoftwareDevice` field, but that field is
not evidence of automatic fallback or uniform runtime probing.

## Build Targets

| Target | Type | Description |
|--------|------|-------------|
| `SparkEngineLib` | Static library | Core engine systems (all subsystems linked here) |
| `SparkEngine` | Executable | Runtime host (loads game modules) |
| `SparkGame` | Shared library | Default game module DLL |
| [SparkEditor](../gameplay-tools/SparkEditor.md) | Executable | ImGui visual editor with Windows and experimental non-Windows build paths |
| [SparkConsole](../gameplay-tools/SparkConsole.md) | Executable | Debug console with named-pipe transport on Windows and bidirectional inherited stdio transport elsewhere |
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

**SparkBuild** is an in-tree C++17 terminal-UI wrapper around CMake. The source lives at `SparkBuild/` (vendored from the now-archived `Krilliac/SparkBuild`; see `SparkBuild/UPSTREAM.md` for the pinned commit). It is built as part of the normal engine build under the `ENABLE_SPARKBUILD` option (ON by default).

**Output:** `build/bin/SparkBuild` (or `SparkBuild.exe` on Windows).

```bash
cmake --preset linux-gcc-release
cmake --build build --target SparkBuild
./build/bin/SparkBuild
```

Because SparkBuild only shells out to `cmake`, it has no dependency on any SparkEngine header or library — so in-tree hosting introduces no circular build dependency. To skip it:

```bash
cmake -B build -DENABLE_SPARKBUILD=OFF ...
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

See [Creating a Game Module](../getting-started/Creating-a-Game-Module.md) for a complete usage guide.

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

See the [Documentation Index](../../docs/README.md) and the [Doc Tooling page](../../docs/tooling/README.md) for full documentation tooling details.

## CI/CD (GitHub Actions)

### build.yml

Runs on pushes to `main`, `develop`, `Working`, `feature/**`, `claude/**`, and `release/**`; pull requests targeting `main`, `develop`, or `Working`; and manual dispatch.

#### CI Job Matrix

| Job | Runner | Compiler | Configs | Key Flags |
|-----|--------|----------|---------|-----------|
| `check-format` | ubuntu-24.04 | clang-format | -- | `--dry-run --Werror` |
| `validate-prompts` | ubuntu-24.04 | -- | -- | `--ci` |
| `build-linux-gcc` | ubuntu-24.04 | GCC | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-clang` | ubuntu-24.04 | Clang | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-asan` | ubuntu-24.04 | GCC | Debug | ASan + UBSan |
| `build-windows-vs2022` | windows-2022 | MSVC v143 | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-windows-vs2026` | windows-latest | MSVC v145 | Debug, Release | native VS 18 generator; advisory until runner availability is guaranteed |
| `coverage` | ubuntu-24.04 | GCC | Debug | `--coverage` + lcov |
| `clang-tidy` | ubuntu-24.04 | Clang | Debug | `continue-on-error` |
| `todo-count` | ubuntu-24.04 | -- | -- | threshold: 20 |

#### Reproducing CI Builds Locally

**Linux GCC:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure --no-tests=error && ./bin/SparkTests && cd ..
```

**Linux Clang:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure --no-tests=error && ./bin/SparkTests && cd ..
```

**AddressSanitizer (ASan + UBSan):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure --no-tests=error && ./bin/SparkTests && cd ..
```

**Windows MSVC (VS 2022):**
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 \
    -T v143 -DBUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

### release.yml (rolling pre-release artifacts)

Runs for `release/**` pushes, `v*` tags, the nightly schedule, and manual dispatch:
- Can publish rolling `nightly` assets with `make_latest: false` for development evaluation; these are not versioned releases and do not supply `stable-v1` signing, install, or attestation certification
- Builds Windows, Linux, and macOS Debug/Release packages plus configured Windows installer artifacts
- Formats include `.zip` (Windows) and `.tar.gz` (Linux/macOS)
- Includes exact commit hash and timestamp

## Compiler Support

| Compiler | Version | Platform | Status |
|----------|---------|----------|--------|
| MSVC | v143 (VS 2022) | Windows | Declared `stable-v1` toolset line; profile blocked and uncertified |
| MSVC | v145 (VS 2026) | Windows | Experimental (`continue-on-error`) |
| GCC | 13+ | Linux | Development build floor; outside `stable-v1` |
| Clang | 17+ | Linux | Development build floor; outside `stable-v1` |
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
find SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
     SparkBuild/src SparkInstaller/src SparkDaemon/src SparkServer/src SparkGateway/src \
     SparkCooker/src SparkWorker/src SparkAutomation/src SparkLauncher/src Tests \
  -not -path '*/Metal/*' \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) \
  | xargs clang-format --dry-run --Werror 2>&1

# Fix formatting automatically
find SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
     SparkBuild/src SparkInstaller/src SparkDaemon/src SparkServer/src SparkGateway/src \
     SparkCooker/src SparkWorker/src SparkAutomation/src SparkLauncher/src Tests \
  -not -path '*/Metal/*' \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) \
  | xargs clang-format -i
```

CI rejects PRs with formatting violations.

---

## See Also

- [Getting Started](../getting-started/Getting-Started.md) -- Build quickstart
- [Testing](Testing.md) -- Running unit tests
- [Architecture Overview](../getting-started/Architecture-Overview.md) -- Engine design and subsystems
- [Contributing](Contributing.md) -- Contribution workflow and code style
- [Creating a Game Module](../getting-started/Creating-a-Game-Module.md) -- Building game modules
