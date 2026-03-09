# Build System & Testing

Context: `#prompt:copilot-instructions` for project overview.

## CMake Build System

Root `CMakeLists.txt` (CMake 3.16+) with 30+ feature toggles.

### Quick Start

```bash
# Windows (Visual Studio 2022)
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Or use provided scripts:
./generate.bat        # Windows: generate VS solution
./build.ps1           # Windows: build
./generate.sh         # Linux: generate Makefiles
./build.sh            # Linux: build
```

### CMake Presets

`CMakePresets.json` provides pre-configured build presets. Use `cmake --preset <name>`.

### Key Feature Toggles

| Toggle | Default | Controls |
|--------|---------|----------|
| `ENABLE_EDITOR` | ON | ImGui editor (SparkEditor target) |
| `ENABLE_GRAPHICS` | ON | DirectX 11 rendering |
| `ENABLE_PHYSX` | ON | Bullet Physics 3 integration |
| `ENABLE_AI` | ON | AI system (behavior trees, NavMesh) |
| `ENABLE_ANIMATION` | ON | Skeletal animation system |
| `ENABLE_NETWORKING` | OFF | UDP multiplayer (requires CURL) |
| `ENABLE_LUA` | ON | Lua scripting support |
| `ENABLE_VULKAN` | OFF | Vulkan backend (experimental) |
| `ENABLE_OPENGL` | OFF | OpenGL backend (experimental) |
| `ENABLE_DXR` | OFF | DirectX Raytracing (requires D3D12) |
| `BUILD_TESTS` | ON | Unit tests with CTest |

### Build Targets

| Target | Type | Description |
|--------|------|-------------|
| `SparkEngine` | Executable | Runtime host |
| `SparkEditor` | Executable | ImGui editor |
| `SparkGame` | Shared Library | Example game module (DLL/SO) |
| `SparkConsole` | Executable | External debug console |
| `SparkShaderCompiler` | Executable | Offline shader compilation |

### Dependencies (ThirdParty/)

Managed as git submodules:

| Library | Purpose |
|---------|---------|
| EnTT | Entity Component System |
| Bullet3 | Physics simulation |
| Dear ImGui | Editor UI (docking branch) |
| Assimp | 3D model import (FBX, glTF, OBJ) |
| DirectXTK | DirectX 11 toolkit |
| spdlog | Fast logging |
| RapidJSON | JSON serialization |
| miniz | Compression (save files) |
| stb | Image loading (stb_image) |
| GLM | Math (secondary to DirectXMath) |

### Platform Support

| Platform | Compiler | Status |
|----------|----------|--------|
| Windows 10+ | MSVC v143 (VS 2022), v144 (VS 2026) | Primary |
| Linux | GCC 11+, Clang 14+ | Experimental |
| macOS | Apple Clang (C++20) | Experimental |

### Quality Policy

- **Zero warnings**: `/W4` (MSVC), `-Wall -Wextra` (GCC/Clang)
- **Zero memory leaks**: Validate with debug builds
- All warnings treated as errors in CI

---

## CI/CD (GitHub Actions)

### Workflow Matrix

```yaml
# Builds on every push/PR:
- Windows MSVC (Debug + Release)
- Linux GCC (Debug + Release)
- Linux Clang (Debug + Release)
```

- Pre-built binaries published on every commit to `master`
- Dependabot configured for weekly dependency updates

---

## Testing

### Test Framework

35 unit tests in `Tests/` directory using internal test framework + CTest integration.

```bash
# Run all tests
cd build && ctest --output-on-failure

# Run specific test
ctest -R TestPhysics --output-on-failure
```

### Test Coverage Areas

- ECS: Component creation, system execution, entity lifecycle
- Physics: Collision detection, raycasting, body creation
- Audio: Sound loading, 3D positioning, volume control
- Graphics: Shader compilation, render target management
- Serialization: Save/load round-trip, compression
- Math: Vector/matrix operations, AABB/OBB tests

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

### Console Commands (Testing)

| Command | Description |
|---------|-------------|
| `test_run_all` | Execute all test suites |
| `test_run <name>` | Run specific test |
| `benchmark_start` | Start performance benchmark |
| `stress_test <system>` | Stress-test a subsystem |
| `test_memory_leaks` | Check for memory leaks |
