# Build System & Testing

Context: `#prompt:copilot-instructions` for project overview. Console commands: see `console-scripting` prompt.

## CMake Build System

### Quick Start

| Action | Windows | Linux |
|--------|---------|-------|
| Generate | `generate.bat` or `cmake -B build -G "Visual Studio 17 2022" -A x64` | `generate.sh` |
| Build | `build.ps1` or `cmake --build build --config Release` | `build.sh` |

Presets: `cmake --preset <name>` (see `CMakePresets.json`).

### Key Feature Toggles

| Toggle | Default | Controls |
|--------|---------|----------|
| `ENABLE_EDITOR` | ON | ImGui editor |
| `ENABLE_GRAPHICS` | ON | DirectX 11 |
| `ENABLE_PHYSX` | ON | Jolt Physics |
| `ENABLE_AI` | ON | Behavior trees, NavMesh |
| `ENABLE_ANIMATION` | ON | Skeletal animation |
| `ENABLE_NETWORKING` | ON | UDP multiplayer (raw sockets) |
| `ENABLE_VULKAN` | ON | Vulkan (experimental) |
| `ENABLE_OPENGL` | OFF | OpenGL (experimental) |
| `ENABLE_DXR` | ON | DXR (Windows/D3D12; SDFGI fallback elsewhere) |
| `BUILD_TESTS` | ON | CTest unit tests |

### Build Targets

SparkEngine (exe), SparkEditor (exe), SparkGame (DLL/SO), SparkConsole (exe), SparkShaderCompiler (exe).

### Dependencies (ThirdParty/ — git submodules)

| Library | Purpose |
|---------|---------|
| EnTT | ECS |
| Jolt Physics | Physics |
| Dear ImGui | Editor UI (docking branch) |
| Assimp | 3D model import |
| DirectXTK | DX11 toolkit |
| spdlog | Logging |
| RapidJSON | JSON serialization |
| miniz | Compression |
| stb | Image loading |
| GLM | Math (secondary to DirectXMath) |

### Platform Support

| Platform | Compiler | Status |
|----------|----------|--------|
| Windows 10+ | MSVC v143/v144 | Primary |
| Linux | GCC 13+, Clang 17+ | Experimental |
| macOS | Apple Clang (C++23) | Experimental |

---

## CI/CD (GitHub Actions)

Builds on every push/PR: Windows MSVC + Linux GCC + Linux Clang (Debug + Release). Pre-built binaries on `master`. Dependabot for weekly updates.

---

## Testing

3563 unit tests across 284 files in `Tests/` with internal framework + CTest.

```bash
cd build && ctest --output-on-failure          # all tests
ctest -R TestPhysics --output-on-failure       # specific test
```

### Coverage Areas

ECS, Physics, Audio, Graphics (shader compilation), Serialization (round-trip), Math (vector/matrix, AABB/OBB).

### Adding a Test

```cpp
// Tests/TestMyFeature.cpp
#include "TestFramework.h"
TEST(MyFeature, BasicFunctionality) {
    MyClass obj;
    obj.Initialize();
    EXPECT_TRUE(obj.IsValid());
    EXPECT_EQ(obj.GetValue(), 42);
}
```
