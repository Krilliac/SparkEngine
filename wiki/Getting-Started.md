# Getting Started

This guide covers everything you need to clone, build, and run SparkEngine from source. It includes platform-specific setup instructions, troubleshooting for common build issues, and verification steps.

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **C++ Compiler** | MSVC v143 (Visual Studio 2022), GCC 11+, or Clang 14+ with C++20 support |
| **CMake** | 3.16 or newer |
| **Graphics** | DirectX 11 capable GPU (Windows). Vulkan SDK optional. OpenGL 4.5 optional. |
| **Platform** | Windows 10+ (primary), Linux x64 (experimental) |
| **Git** | For cloning with submodules |
| **Linux packages** | `build-essential`, `ninja-build`, `cmake` |

### Windows Prerequisites

1. **Visual Studio 2022** (Community edition or higher) with the following workloads:
   - "Desktop development with C++" workload
   - Windows 10/11 SDK (any recent version)
   - MSVC v143 build tools
2. **CMake 3.16+** (bundled with Visual Studio, or install separately from cmake.org)
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

SparkEngine depends on 15 third-party libraries managed as Git submodules. After cloning, verify they are present:

```bash
# Check that ThirdParty directories are populated
ls ThirdParty/
```

You should see directories for: entt, bullet3, imgui, angelscript, assimp, glm, rapidjson, spdlog, stb, miniaudio, DirectXTK, ImGuizmo, imnodes, miniz, tinyobjloader.

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
| `minimal` | Any | Core-only build without advanced features |

```bash
cmake --preset windows-release
cmake --build --preset windows-release
```

### Minimal Build (Core Only)

If you want the fastest possible build with only essential features:

```bash
cmake --preset minimal
cmake --build build --config Release
```

This disables the editor, advanced graphics, AI, animation, networking, and other optional subsystems. Useful for CI or for working on core engine code.

## Build

### Windows (PowerShell)

```powershell
.\build.ps1 -config Release -editor -angelscript
```

Options:
- `-config Debug|Release` -- Build configuration
- `-editor` -- Include the [SparkEditor](SparkEditor)
- `-console` -- Include [SparkConsole](SparkConsole)
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
│   ├── SparkEngine.exe      # Main engine executable
│   ├── SparkConsole.exe     # Debug console (Windows) — see [SparkConsole](SparkConsole)
│   ├── SparkEditor.exe      # Visual editor (Windows) — see [SparkEditor](SparkEditor)
│   ├── SparkShaderCompiler.exe  # Shader compilation tool
│   ├── SparkTests           # Unit test runner (when BUILD_TESTS=ON)
│   └── SparkGame.dll        # Default game module
├── lib/                     # Static/shared libraries
├── Shaders/                 # Compiled shaders
└── Assets/                  # Game assets
```

## Run the Engine

### Windows

```batch
cd build\bin
SparkEngine.exe
```

You should see:
1. A DirectX 11 window with a blue background
2. [SparkConsole](SparkConsole) opens automatically
3. Console shows initialization messages

### Linux

```bash
cd build/bin
./SparkEngine
```

On Linux, the engine uses OpenGL stubs by default. Full rendering requires the Vulkan backend (experimental).

### Command-Line Options

| Option | Description |
|--------|-------------|
| `-headless` | Run without graphics or audio (for dedicated servers) |
| `-game <path>` | Load a specific game module DLL |
| `-scene <path>` | Load a specific scene on startup |
| `-width <N>` | Set window width in pixels |
| `-height <N>` | Set window height in pixels |
| `-fullscreen` | Start in fullscreen mode |
| `-console` | Force enable the debug console |
| `-noconsole` | Disable the debug console |

Example:

```bash
./SparkEngine -game MyGame.dll -scene Assets/Scenes/Level01.scene -width 1920 -height 1080
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

Once the engine is running, open [SparkConsole](SparkConsole) and try:

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
ctest --test-dir build --output-on-failure
```

All 71 test files (864+ test cases) should pass. See [Testing](Testing) for details.

## SparkBuild Tool

SparkEngine ships with [SparkBuild](https://github.com/Krilliac/SparkBuild), a standalone C++ build tool located at `tools/SparkBuild.exe`. The binary is pre-built and ready to use -- no compilation needed.

The tool is kept current automatically via a weekly GitHub Action, but you can also update it manually:

```powershell
# Windows
.\tools\update-sparkbuild.ps1
```

```bash
# Linux / macOS
./tools/update-sparkbuild.sh
```

See [Build System and CMake Modules -- SparkBuild](Build-System-and-CMake-Modules#sparkbuild) for more details.

## Build Options

SparkEngine has 30+ toggleable CMake modules. Pass them during configuration:

```bash
cmake -B build -DENABLE_AI=OFF -DENABLE_NETWORKING=ON ...
```

### Commonly Used Build Flags

| Flag | Default | Description |
|------|---------|-------------|
| `BUILD_TESTS` | ON | Build the unit test suite |
| `ENABLE_EDITOR` | ON (Windows) | Include the ImGui editor |
| `ENABLE_GRAPHICS` | ON | Enable graphics rendering |
| `ENABLE_PHYSX` | ON | Enable Bullet Physics integration |
| `ENABLE_AI` | ON | Enable AI and navigation |
| `ENABLE_ANIMATION` | ON | Enable skeletal animation |
| `ENABLE_NETWORKING` | OFF | Enable networking (requires CURL) |
| `ENABLE_PROFILING` | ON | Enable performance profiler |
| `ENABLE_HOT_RELOAD` | ON | Enable script hot-reload |

See [Build System and CMake Modules](Build-System-and-CMake-Modules) for the full list of options.

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

**Fix:** Update to CMake 3.16 or newer:

```bash
# Ubuntu (may need to add Kitware PPA for latest version)
sudo apt remove cmake
sudo snap install cmake --classic

# macOS
brew upgrade cmake
```

### MSVC C++20 Errors

**Symptom:** Compiler errors about missing C++20 features, `std::format`, or `constexpr` issues.

**Fix:** Ensure you are using MSVC v143 (Visual Studio 2022) or newer. Open the Visual Studio Installer and update to the latest version. The engine requires full C++20 support.

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
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src SparkGame/Source \
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
cd build && ctest --output-on-failure
```

## Project Structure Quick Reference

```
SparkEngine/
├── SparkEngine/          # Core engine library + executable
│   └── Source/
│       ├── Core/         # Platform, EngineContext, entry point
│       ├── Graphics/     # DX11 renderer, post-processing
│       ├── Engine/       # ECS, AI, Animation, Events, Networking
│       ├── Physics/      # Bullet Physics integration
│       ├── Input/        # Keyboard, mouse, gamepad
│       ├── Audio/        # XAudio2 audio engine
│       └── Utils/        # Logger, Profiler, Console
├── SparkEditor/          # ImGui editor (Windows)
├── SparkGame/            # Default game module (DLL)
├── SparkConsole/         # Standalone debug console
├── SparkShaderCompiler/  # Shader compilation tool
├── SparkSDK/             # Public SDK headers
├── Templates/            # Game module templates
├── ThirdParty/           # Git submodules (15 libraries)
├── Tests/                # Unit tests (71 files, 864+ cases)
├── Shaders/              # HLSL, GLSL shaders
├── Assets/               # Models, Scenes, Scripts
├── cmake/                # CMake helper modules
└── CMakeLists.txt        # Root build configuration
```

## Next Steps

- [Architecture Overview](Architecture-Overview) -- Understand the engine's design
- [Creating a Game Module](Creating-a-Game-Module) -- Build your first game
- [Entity Component System](Entity-Component-System) -- Work with entities and components
- [Build System and CMake Modules](Build-System-and-CMake-Modules) -- Full build configuration reference
- [Testing](Testing) -- Running and writing tests
- [Troubleshooting](Troubleshooting) -- If you run into issues

---

## See Also

- [SparkConsole](SparkConsole) -- Debug console for engine interaction
- [SparkEditor](SparkEditor) -- Visual editor for scenes and materials
- [Architecture Overview](Architecture-Overview) -- Understand the engine's design
- [Creating a Game Module](Creating-a-Game-Module) -- Build your first game module
- [Input System](Input-System) -- Configuring input bindings
- [Scene Management](Scene-Management) -- Loading and editing scenes
