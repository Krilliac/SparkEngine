# SparkBuild

A cross-platform terminal UI build tool for configuring and compiling [SparkEngine](https://github.com/Krilliac/SparkEngine). SparkBuild provides an interactive menu-driven interface to manage CMake options, select build configurations, and compile the engine — without needing to remember complex command-line invocations.

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-green)
![License](https://img.shields.io/badge/license-MIT-brightgreen)

## Features

- **Interactive TUI** — Color-coded menus with categorized build options, input validation, and live process output
- **Cross-platform** — Native support for Windows, Linux, and macOS with platform-specific defaults
- **35+ toggleable modules** — Enable or disable engine systems individually (graphics, physics, scripting, rendering effects, gameplay systems, etc.)
- **Preset system** — Quickly apply predefined configurations: All On, All Off, Defaults, Minimal, Linux-Friendly, Shipping, or Development
- **CMakePresets.json support** — Auto-detects and lists available presets from the engine directory
- **Environment management** — Checks for Git, CMake, and compilers; can clone SparkEngine and download CMake automatically
- **Configuration persistence** — Saves and restores your settings via an INI file between sessions

## Requirements

- **CMake** 3.16 or newer
- **C++17 compiler:**
  - Windows: Visual Studio 2022 (MSVC v143+)
  - Linux: GCC 9+ or Clang 10+
  - macOS: Xcode Command Line Tools or Clang
- **Git** (for cloning SparkEngine and managing submodules)
- **Ninja** (recommended on Linux/macOS; optional on Windows)

## Building SparkBuild

### Windows

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

The binary will be at `build/bin/SparkBuild.exe`.

### Linux

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install cmake ninja-build curl

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The binary will be at `build/bin/SparkBuild`.

### macOS

```bash
brew install cmake ninja

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The binary will be at `build/bin/SparkBuild`.

## Quick Start

1. **Build SparkBuild** using the instructions above
2. **Run** `SparkBuild` (or `SparkBuild.exe` on Windows)
3. **Environment** — Use option `1` to check dependencies and set the path to your SparkEngine clone (or clone it directly from the menu)
4. **Configure** — Use option `2` to select your generator, build type, and toggle engine modules
5. **Build** — Use option `3` to generate the project and compile

## Main Menu

```
╔══════════════════════════════════════╗
║           SparkBuild v2.1           ║
╚══════════════════════════════════════╝
  1. Environment
  2. Configure Build
  3. Build
  4. Show Config Summary
  5. Exit
```

### 1. Environment

Check and set up the tools SparkBuild needs:

| Option | Description |
|--------|-------------|
| Check All Dependencies | Verifies Git, CMake, compilers, submodules, and the engine repo |
| Set Engine Path | Point to an existing SparkEngine checkout |
| Clone SparkEngine | Clone the repo from GitHub with recursive submodule init |
| Download CMake | Download CMake 3.31.5 binaries for your platform |
| Init Submodules | Run `git submodule update --init --recursive` |

### 2. Configure Build

Customize how SparkEngine is built:

| Option | Description |
|--------|-------------|
| Select Generator | Visual Studio 2022, Ninja, Makefiles, Xcode, etc. |
| Select Build Type | Debug, Release, RelWithDebInfo, MinSizeRel |
| Set Paths | Engine source path and build output path |
| Toggle Build Options | Enable/disable 35+ engine modules by category |
| Apply Preset | All On, All Off, Defaults, Minimal, Linux-Friendly, Shipping, Development |
| CMake Presets | Detect and select from `CMakePresets.json` |
| MSVC Toolset | Override the MSVC toolset version (Windows only) |
| Parallel Jobs | Set the number of parallel compilation jobs |

### 3. Build

Execute the actual build operations:

| Option | Description |
|--------|-------------|
| Generate Project | Run `cmake -S ... -B ...` with your configured options |
| Build Project | Run `cmake --build ...` to compile |
| Generate & Build | Run both steps sequentially |
| Clean Build Dir | Delete the build directory (with confirmation) |
| Open Build Folder | Open the build directory in your file manager |
| Run Engine | Launch the compiled engine executable |

## Build Options

SparkBuild exposes 35+ CMake options organized into categories, matching the engine's CMakeLists.txt exactly:

| Category | Options |
|----------|---------|
| **Core Systems** | Graphics, Physics (Bullet 3), AI/NavMesh, Animation, Save/Load, Advanced Input, Asset Streaming |
| **Graphics Backends** | Vulkan, OpenGL, DirectX Raytracing (DXR) |
| **Rendering & Effects** | Post-Processing, Advanced Lighting, Decals, Mesh LOD, Fog System, Screen-Space Effects (SSAO/SSR) |
| **Editor & Tools** | ImGui Editor, Profiling, Performance Stats, Unit Tests |
| **Scripting** | Lua (Sol2), Hot Reload |
| **Gameplay** | Terrain, Procedural Generation, Cinematics, Weather, Inventory, Quest System, Event System, Day/Night Cycle |
| **Shipping & Deployment** | Headless Mode, Console in Shipping, Dev Commands in Shipping, Strip Debug Symbols |
| **Experimental** | Networking, SDL2, Collaborative Editing |

### Presets

| Preset | Description |
|--------|-------------|
| All On | Enable every module |
| All Off | Disable everything |
| Defaults | Recommended defaults for the current platform |
| Minimal | Defaults with AI, Animation, Networking, Save, Procedural, Cinematic, Decals, Mesh LOD, and DXR disabled |
| Linux-Friendly | SDL2 + OpenGL for best Linux compatibility |
| Shipping | Release build with editor, profiling, hot-reload, and tests disabled; strips debug symbols |
| Development | Debug build with all editor/profiling/testing tools enabled |

## Configuration File

SparkBuild persists your settings to an INI file:

| Platform | Location |
|----------|----------|
| Windows | `sparkbuild.ini` (next to the executable) |
| Linux/macOS | `~/.config/sparkbuild/sparkbuild.ini` |

Example contents:

```ini
[Paths]
EnginePath=/home/user/SparkEngine
BuildPath=/home/user/SparkEngine/build

[Build]
Generator=Ninja
BuildType=Release
ParallelJobs=8

[Options]
ENABLE_GRAPHICS=ON
ENABLE_PHYSX=ON
ENABLE_LUA=ON
ENABLE_NETWORKING=OFF
```

## Project Structure

```
SparkBuild/
├── src/
│   ├── main.cpp            # Entry point
│   ├── SparkBuild.h/cpp    # Main TUI application and menu logic
│   ├── Config.h/cpp        # Configuration management and CMake command generation
│   ├── ProcessRunner.h/cpp # Cross-platform async process execution
│   ├── Terminal.h/cpp      # Terminal colors, input handling, layout utilities
│   ├── Downloader.h/cpp    # HTTP downloads and ZIP extraction
│   └── Platform.h          # Platform detection macros
├── resources/
│   ├── SparkBuild.rc       # Windows version info resource
│   └── resource.h
├── .github/workflows/
│   └── build.yml           # CI/CD for Windows, Linux, and macOS
├── CMakeLists.txt
└── .gitignore
```

## CI/CD

GitHub Actions automatically builds SparkBuild on every push to `main`:

- **Windows** — Visual Studio 2022 (x64)
- **Linux** — GCC and Clang with Ninja
- **macOS** — Clang with Ninja

Tagged commits (`v*`) create versioned GitHub Releases. Every push to `main` updates a rolling `latest` pre-release with fresh binaries for all platforms.

## Related

- [SparkEngine](https://github.com/Krilliac/SparkEngine) — The 3D game engine that SparkBuild configures and compiles

## License

This project is licensed under the MIT License.
