# Getting Started

> **Release boundary:** The only declared profile is the blocked and uncertified
> `stable-v1` Windows 11 x64/MSVC v143 + D3D11/Windows NullRHI + C++ module slice.
> Windows 10, Linux, macOS, and other toolchain/backend paths below are development
> instructions outside that profile.

This guide covers everything you need to clone, build, and run SparkEngine from source. It includes platform-specific setup instructions, troubleshooting for common build issues, and verification steps.

For end-user hardware targets (CPU/RAM/VRAM minimums and recommended specs),
see [System Requirements](../platform/System-Requirements.md).

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **C++ Compiler** | MSVC v143 (Visual Studio 2022 17.6+), GCC 13+, or Clang 17+ with C++23 support |
| **CMake** | 3.25 or newer |
| **Graphics** | DirectX 11 capable GPU (Windows). Vulkan SDK optional. OpenGL 4.5 optional. Metal 2.3+ on macOS. |
| **Release candidate host** | Windows 11 x64 is the blocked and uncertified `stable-v1` target |
| **Development hosts** | Windows 10 x64, Linux x64, and macOS are outside the release profile |
| **Git** | For cloning with submodules |
| **Linux packages** | `build-essential`, `ninja-build`, `cmake` |
| **macOS packages** | `brew install cmake sdl2 openal-soft` (plus `molten-vk` for Vulkan) |

### Windows Prerequisites

1. **Visual Studio 2022** (Community edition or higher) with the following workloads:
   - "Desktop development with C++" workload
   - Windows 10/11 SDK (any recent version)
   - MSVC v143 build tools
2. **CMake 3.25+** (bundled with current Visual Studio installations, or install separately from cmake.org)
3. **Git** (install from git-scm.com or via `winget install Git.Git`)

To verify your Windows setup:

```powershell
# Check Visual Studio compiler
cl.exe 2>&1 | Select-String "Version"

# Check CMake
cmake --version

# Check Git
git --version
```

### Linux Prerequisites (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install build-essential ninja-build cmake git
```

For Clang builds (optional but recommended for matching CI):

```bash
sudo apt install clang clang-format
```

To verify your Linux setup:

```bash
# Check GCC version (must be 11+)
g++ --version

# Check CMake version (must be 3.16+)
cmake --version

# Check Ninja (optional but faster)
ninja --version
```

### Linux Prerequisites (Fedora/RHEL)

```bash
sudo dnf groupinstall "Development Tools"
sudo dnf install cmake ninja-build git clang clang-tools-extra
```

### Linux Prerequisites (Arch Linux)

```bash
sudo pacman -S base-devel cmake ninja git clang
```

### macOS Prerequisites (Experimental)

```bash
# Install Xcode command-line tools
xcode-select --install

# Install CMake and Ninja via Homebrew
brew install cmake ninja
```

Note: macOS is experimental. OpenGL stubs are provided but DirectX 11 features are not available.

## Clone the Repository

```bash
git clone --recurse-submodules https://github.com/Krilliac/SparkEngine.git
cd SparkEngine
```

If you already cloned without submodules:

```bash
git submodule sync
git submodule init
git submodule update --recursive
```

### Verifying Submodules

SparkEngine currently declares six Git submodules. After cloning, verify they
are present; additional audited dependencies are vendored snapshots recorded in
`ThirdParty/dependencies.lock` rather than submodules:

```bash
# Check that ThirdParty directories are populated
ls ThirdParty/
```

The submodule paths are `ThirdParty/Utils/miniz`, `ThirdParty/UI/imgui`,
`ThirdParty/ECS/entt`, `ThirdParty/Scripting/angelscript-mirror`,
`ThirdParty/AI/recastnavigation`, and `ThirdParty/SDL2`. Do not infer a
dependency from an old documentation name; the manifest and CMake target graph
are authoritative.

If any are empty, re-run:

```bash
git submodule update --init --recursive
```

## Configure

### Windows (Visual Studio 2022)

```batch
.\generate.bat -g "Visual Studio 17 2022" release
```

### Linux / macOS

```bash
chmod +x generate.sh
./generate.sh release -g Ninja
```

### Using CMake Directly

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
```

### CMake Presets

SparkEngine includes `CMakePresets.json` with ready-made configurations:

| Preset | Platform | Description |
|--------|----------|-------------|
| `windows-debug` | Windows | VS 2022, Debug |
| `windows-release` | Windows | VS 2022, Release |
| `linux-gcc-debug` | Linux | GCC, Debug |
| `linux-gcc-release` | Linux | GCC, Release |
| `linux-clang-debug` | Linux | Clang, Debug |
| `linux-clang-release` | Linux | Clang, Release |
| `ci-linux-asan` | Linux | AddressSanitizer build |
| `ci-linux-tsan` | Linux | ThreadSanitizer build |
| `minimal` | Any | Reduced development preset: networking and DXR are effectively disabled; several other preset variables are inert |

```bash
cmake --preset windows-release
cmake --build --preset windows-release
```

### Reduced Development Preset (Not Core Only)

For a release-configured development build with networking and DXR disabled:

```bash
cmake --preset minimal
cmake --build build --config Release
```

This preset effectively disables networking and DXR. Its AI, animation, save,
procedural, cinematic, decal, and mesh-LOD cache variables do not correspond to
root options and therefore do not strip those sources; the editor also remains
enabled. Disable declared targets explicitly when needed. `HEAD-220` tracks a
true stripped/headless configuration.

## Build

### Windows (PowerShell)

```powershell
.\build.ps1 -config Release -editor -angelscript
```

Options:
- `-config Debug|Release` -- Build configuration
- `-editor` -- Include the [SparkEditor](../gameplay-tools/SparkEditor.md)
- `-console` -- Include [SparkConsole](../gameplay-tools/SparkConsole.md)
- `-angelscript` -- Include AngelScript scripting

### Linux / macOS

```bash
./build.sh release
```

Options:
- `-g Ninja` -- Use Ninja generator (faster)
- `-E` -- Disable editor
- `-C` -- Disable console

### Using CMake Directly

```bash
cmake --build build --config Release
```

### Parallel Builds

Speed up the build by specifying the number of parallel jobs:

```bash
# Linux/macOS
cmake --build build --config Release -- -j$(nproc)

# Windows (PowerShell)
cmake --build build --config Release -- /maxcpucount
```

A clean Release build typically takes 2-5 minutes on a modern machine with 8+ cores.

## Build Output

After a successful build:

```
build/
├── bin/
│   └── Release/             # VS/Ninja Multi-Config output; use the built configuration
│       ├── SparkBuild.exe   # In-tree SparkBuild target
│       ├── SparkEngine.exe  # Main engine executable
│       ├── SparkConsole.exe # Optional debug-console subprocess (Windows)
│       ├── SparkEditor.exe  # Visual editor (Windows)
│       ├── SparkShaderCompiler.exe
│       ├── SparkTests.exe   # Unit test runner (when BUILD_TESTS=ON)
│       └── SparkGameFPS.dll # A built game module; choose it explicitly at launch
├── lib/                     # Static/shared libraries
├── Shaders/                 # Compiled shaders
└── Assets/                  # Game assets
```

Single-config generators retain the flat `build/bin/` layout. With a CMake
preset, substitute that preset's binary directory for `build/`.

## Run the Engine

### Windows

```batch
cd build\bin\Release
SparkEngine.exe -game .\SparkGameFPS.dll
```

For a VS/Ninja Multi-Config build, replace `Release` with the configuration you
built. The game module is selected explicitly; a bare launch only discovers
candidates and prompts for a selection.

You should see:
1. A DirectX 11 window with a blue background
2. Unless `-no-subprocess` is supplied, the engine attempts to launch the optional [SparkConsole](../gameplay-tools/SparkConsole.md) subprocess
3. The in-process console emits initialization messages

`SparkEditor` is a separate executable. When `ENABLE_EDITOR=ON`, launch the
same configuration directly:

```batch
.\build\bin\Release\SparkEditor.exe
```

### Linux

```bash
cd build/bin
./SparkEngine
```

On Linux, select a compiled Vulkan or OpenGL development backend. For explicit
CPU-rendered OpenGL, configure Mesa llvmpipe and the required virtual display:

```bash
# Software rendering (no GPU required)
sudo apt-get install -y xvfb libgl1-mesa-dri
Xvfb :99 -screen 0 1024x768x24 &
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./SparkEngine
```

`RHIFactory::CreateDevice` alone does not retry a failed backend. At runtime,
`RHIBridge::Initialize` retries its available GPU backends after device or
swap-chain setup fails, then creates the no-render `NullRHIDevice` only if all
of them fail. That resilience path is not a certified Linux compatibility claim.

### Command-Line Options

| Option | Description |
|--------|-------------|
| `-headless` | Select the host headless entry path. Current host wiring initializes no RHI; wiring the separate `NullRHIDevice` path remains `HEAD-220`. |
| `-game <path>` | Load a specific game module DLL |
| `-scene <path>` | Load a specific scene on startup |
| `-window-size <W>x<H>` | Override the initial window size, for example `-window-size 1920x1080` |
| `-no-subprocess` | Skip the optional standalone `SparkConsole` subprocess; the in-process console remains available |

Example:

```bash
./SparkEngine -game MyGame.dll -scene Assets/Scenes/Level01.scene -window-size 1920x1080
```

## Default Controls

| Input | Action |
|-------|--------|
| W / A / S / D | Move |
| Mouse | Look |
| Space | Jump |
| Ctrl | Crouch |
| Shift | Sprint |
| Left Click | Fire / Capture Mouse |
| Right Click | Aim Down Sights |
| R | Reload |
| E | Interact |
| 1 / 2 / 3 / 4 | Weapon slots |
| Esc | Release Mouse / Menu |
| ` (Backtick) | Toggle Debug Console |
| F1 | Toggle Editor (if enabled) |
| F3 | Toggle Performance Stats |

## Verify Installation

Once the engine is running, you should see the editor interface:

![SparkEditor interface](../../docs/screenshots/editor-overview.png)

*SparkEditor with docked panels — Material Editor, Physics tools, Scene View, Inspector, and Asset Browser.*

Open [SparkConsole](../gameplay-tools/SparkConsole.md) and try:

```
help            # List all commands
engine_status   # Check system initialization
fps             # Show framerate
graphics_info   # Display GPU information
diag            # Run diagnostics
scene_info      # Show current scene info
physics_info    # Show physics system state
audio_info      # Show audio device information
```

### Running the Test Suite

To verify that the build is correct, run the test suite:

```bash
# Build with tests enabled (default)
cmake -B build -DBUILD_TESTS=ON
cmake --build build --config Release

# Run tests
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

The generated source inventory currently records 6,952 GoogleTest definitions across 575 test-bearing implementation files; that inventory is not a CTest verdict. Use the command's exit status and final CTest summary to determine the actual result. See [Testing](../advanced/Testing.md) for details.

## SparkBuild Tool

The repository includes **SparkBuild**, a cross-platform terminal-UI wrapper around CMake. Its source lives in-tree under `SparkBuild/`; when `ENABLE_SPARKBUILD=ON` (the root default), normal CMake configuration adds its target to the build. Opt out with `-DENABLE_SPARKBUILD=OFF`.

```powershell
cmake --build build --config Release --target SparkBuild
.\build\bin\Release\SparkBuild.exe
```

Single-config superproject builds use `build/bin/SparkBuild`; a standalone
SparkBuild-only project also retains its historical flat `bin` output.

See [Build System and CMake Modules -- SparkBuild](../advanced/Build-System-and-CMake-Modules.md#sparkbuild) for more details.

## Build Options

SparkEngine exposes documented root CMake options. Pass supported options during configuration:

```bash
cmake -B build -DENABLE_EDITOR=OFF -DENABLE_NETWORKING=ON ...
```

### Commonly Used Build Flags

| Flag | Default | Description |
|------|---------|-------------|
| `BUILD_TESTS` | ON | Build the unit test suite |
| `ENABLE_EDITOR` | ON (Windows) | Include the ImGui editor |
| `ENABLE_GRAPHICS` | ON | Declared but currently inert; OFF does not remove graphics/RHI (`HEAD-220`) |
| `ENABLE_NETWORKING` | ON | Controls networking definitions/libraries; server-process targets have a separate option |
| `ENABLE_PROFILING` | ON | Controls the `PROFILING_ENABLED` compile definition |
| `ENABLE_VULKAN` | ON | Controls Vulkan discovery and its support definition; source-glob breadth still needs verification |
| `ENABLE_OPENGL` | ON | Controls OpenGL discovery/enablement; host context setup remains separate |
| `BUILD_GAME_MODULES` | ON | Include in-tree game-module targets |

See [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) for the full list of options.

## Troubleshooting

### Submodule Errors

**Symptom:** CMake fails with "Could not find EnTT" or similar missing dependency errors.

**Fix:** Ensure submodules are fully initialized:

```bash
git submodule update --init --recursive
```

If submodules are stuck in a detached HEAD state:

```bash
git submodule sync --recursive
git submodule update --init --recursive --force
```

### CMake Version Too Old

**Symptom:** CMake errors about unsupported features or policy warnings.

**Fix:** Update to CMake 3.25 or newer:

```bash
# Ubuntu (may need to add Kitware PPA for latest version)
sudo apt remove cmake
sudo snap install cmake --classic

# macOS
brew upgrade cmake
```

### MSVC C++23 Errors

**Symptom:** Compiler errors about missing C++23 features, `std::expected`, `std::print`, or deducing `this`.

**Fix:** Ensure you are using MSVC v143 (Visual Studio 2022 version 17.6+) or newer. Open the Visual Studio Installer and update to the latest version. The engine requires full C++23 support.

### Linux: Missing X11/OpenGL Headers

**Symptom:** Linker errors about `X11`, `GL`, or `Xrandr` symbols.

**Fix:** Install the development packages:

```bash
# Debian/Ubuntu
sudo apt install libx11-dev libxrandr-dev libgl1-mesa-dev libglu1-mesa-dev

# Fedora
sudo dnf install libX11-devel mesa-libGL-devel
```

### Clang-Format Failures

**Symptom:** CI rejects your PR with clang-format violations.

**Fix:** Format your code before committing:

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules/SparkGame/Source \
  -name '*.h' -o -name '*.cpp' | xargs clang-format -i
```

### Build Succeeds but Engine Crashes on Startup

**Symptom:** The engine executable starts but immediately crashes or shows a black window.

**Fix:** Check the following:
1. Ensure your GPU supports DirectX 11 (Windows) or OpenGL 4.5 (Linux)
2. Update your GPU drivers to the latest version
3. Check the `spark.log` file in the working directory for error messages
4. Try running in Debug mode for better error messages:
   ```bash
   cmake --build build --config Debug
   ```

### Windows: Missing DLLs at Runtime

**Symptom:** "The program can't start because XXXXX.dll was not found."

**Fix:** Ensure all DLLs are in the same directory as the executable. The build system should copy them automatically, but if not:

```powershell
# Copy required DLLs to the bin directory
cmake --build build --config Release --target install
```

### AddressSanitizer Build Failures

**Symptom:** ASan build reports link errors or missing runtime libraries.

**Fix:** Ensure you are using GCC 11+ or Clang 14+ with ASan support:

```bash
cmake --preset ci-linux-asan
cmake --build build
cd build && ctest --output-on-failure --no-tests=error
```

## Project Structure Quick Reference

```
SparkEngine/
├── SparkEngine/          # Core engine library + executable
│   └── Source/
│       ├── Core/         # Platform, EngineContext, entry point
│       ├── Graphics/     # DX11 renderer, post-processing
│       ├── Engine/       # ECS, AI, Animation, Events, Networking
│       ├── Physics/      # Jolt Physics integration
│       ├── Input/        # Keyboard, mouse, gamepad
│       ├── Audio/        # XAudio2 audio engine
│       └── Utils/        # Logger, Profiler, Console
├── SparkEditor/          # ImGui editor (Windows)
├── GameModules/          # Game modules
│   ├── SparkGame/        # Default game module (DLL)
│   └── SparkGameMMO/     # MMO game module (DLL)
├── SparkConsole/         # Standalone debug console
├── SparkShaderCompiler/  # Shader compilation tool
├── SparkSDK/             # Public SDK headers
├── Templates/            # Game module templates
├── ThirdParty/           # Audited dependencies: six submodules plus vendored snapshots
├── Tests/                # Unit-test sources (generated inventory above; CTest is the verdict)
├── Shaders/              # HLSL, GLSL shaders
├── Assets/               # Models, Scenes, Scripts
├── cmake/                # CMake helper modules
└── CMakeLists.txt        # Root build configuration
```

## Next Steps

- [Architecture Overview](Architecture-Overview.md) -- Understand the engine's design
- [Creating a Game Module](Creating-a-Game-Module.md) -- Build your first game
- [Entity Component System](../subsystems/Entity-Component-System.md) -- Work with entities and components
- [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) -- Full build configuration reference
- [Testing](../advanced/Testing.md) -- Running and writing tests
- [Troubleshooting](../advanced/Troubleshooting.md) -- If you run into issues

---

## See Also

- [SparkConsole](../gameplay-tools/SparkConsole.md) -- Debug console for engine interaction
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Visual editor for scenes and materials
- [Architecture Overview](Architecture-Overview.md) -- Understand the engine's design
- [Creating a Game Module](Creating-a-Game-Module.md) -- Build your first game module
- [Input System](../subsystems/Input-System.md) -- Configuring input bindings
- [Scene Management](../subsystems/Scene-Management.md) -- Loading and editing scenes
