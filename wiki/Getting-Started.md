# Getting Started

This guide covers everything you need to clone, build, and run SparkEngine from source.

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **C++ Compiler** | MSVC v143 (Visual Studio 2022), GCC 11+, or Clang 14+ with C++20 support |
| **CMake** | 3.16 or newer |
| **Graphics** | DirectX 11 capable GPU (Windows). Vulkan SDK optional. OpenGL 4.5 optional. |
| **Platform** | Windows 10+ (primary), Linux x64 (experimental) |
| **Git** | For cloning with submodules |
| **Linux packages** | `build-essential`, `ninja-build`, `cmake` |

**Linux (Debian/Ubuntu):**

```bash
sudo apt install build-essential ninja-build cmake
```

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

## Build

### Windows (PowerShell)

```powershell
.\build.ps1 -config Release -editor -angelscript
```

Options:
- `-config Debug|Release` — Build configuration
- `-editor` — Include the [SparkEditor](SparkEditor)
- `-console` — Include [SparkConsole](SparkConsole)
- `-angelscript` — Include AngelScript scripting

### Linux / macOS

```bash
./build.sh release
```

Options:
- `-g Ninja` — Use Ninja generator (faster)
- `-E` — Disable editor
- `-C` — Disable console

### Using CMake Directly

```bash
cmake --build build --config Release
```

## Build Output

After a successful build:

```
build/
├── bin/
│   ├── SparkEngine.exe      # Main engine executable
│   ├── SparkConsole.exe     # Debug console (Windows) — see [SparkConsole](SparkConsole)
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

## Default Controls

| Input | Action |
|-------|--------|
| W / A / S / D | Move |
| Mouse | Look |
| Space | Jump |
| Ctrl | Crouch |
| Left Click | Fire / Capture Mouse |
| Esc | Release Mouse / Menu |
| ` (Backtick) | Toggle Debug Console |

## Verify Installation

Once the engine is running, open [SparkConsole](SparkConsole) and try:

```
help            # List all commands
engine_status   # Check system initialization
fps             # Show framerate
graphics_info   # Display GPU information
diag            # Run diagnostics
```

## SparkBuild Tool

SparkEngine ships with [SparkBuild](https://github.com/Krilliac/SparkBuild), a standalone C++ build tool located at `tools/SparkBuild.exe`. The binary is pre-built and ready to use — no compilation needed.

The tool is kept current automatically via a weekly GitHub Action, but you can also update it manually:

```powershell
# Windows
.\tools\update-sparkbuild.ps1
```

```bash
# Linux / macOS
./tools/update-sparkbuild.sh
```

See [Build System and CMake Modules — SparkBuild](Build-System-and-CMake-Modules#sparkbuild) for more details.

## Build Options

SparkEngine has 30+ toggleable CMake modules. Pass them during configuration:

```bash
cmake -B build -DENABLE_AI=OFF -DENABLE_NETWORKING=ON ...
```

See [Build System and CMake Modules](Build-System-and-CMake-Modules) for the full list of options.

## Next Steps

- [Architecture Overview](Architecture-Overview) — Understand the engine's design
- [Creating a Game Module](Creating-a-Game-Module) — Build your first game
- [Troubleshooting](Troubleshooting) — If you run into issues

---

## See Also

- [SparkConsole](SparkConsole) — Debug console for engine interaction
- [SparkEditor](SparkEditor) — Visual editor for scenes and materials
- [Architecture Overview](Architecture-Overview) — Understand the engine's design
- [Creating a Game Module](Creating-a-Game-Module) — Build your first game module
