# Build System & Testing

Context: `#prompt:copilot-instructions` for project overview. Console commands: see `console-scripting` prompt.

## CMake Build System

### Quick Start

| Action | Windows | Linux |
|--------|---------|-------|
| Generate | `generate.bat` or `cmake -B build -G "Visual Studio 17 2022" -A x64` | `generate.sh` |
| Build | `build.ps1` or `cmake --build build --config Release` | `build.sh` |

Presets: `cmake --preset <name>` (see `CMakePresets.json`).

### Selected CMake Options

| Toggle | Default | Controls |
|--------|---------|----------|
| `ENABLE_EDITOR` | ON | ImGui editor |
| `ENABLE_NETWORKING` | ON | UDP multiplayer (raw sockets) |
| `ENABLE_VULKAN` | ON | Experimental Vulkan backend sources when prerequisites are available |
| `ENABLE_OPENGL` | ON | Experimental OpenGL backend sources when prerequisites are available |
| `ENABLE_DXR` | ON | Experimental D3D12/DXR source path on eligible Windows builds |
| `ENABLE_ANGELSCRIPT` | ON | AngelScript gameplay runtime |
| `ENABLE_RECAST` | ON | Recast/Detour navigation backend |
| `BUILD_GAME_MODULES` | ON | In-tree game-module targets |
| `BUILD_TESTS` | ON | CTest unit tests |

`ENABLE_GRAPHICS` is currently a compatibility cache variable, not a D3D11
source-selection switch. `ENABLE_PHYSX`, `ENABLE_AI`, and `ENABLE_ANIMATION` are
not root CMake options.

### Build Targets

SparkEngine (exe), SparkEditor (exe), SparkGame (DLL/SO), SparkConsole (exe), SparkShaderCompiler (exe).

### Selected Tracked Dependencies

`ThirdParty/dependencies.lock` is the authoritative audited manifest. This list
summarizes linked or compiled surfaces; presence is not support certification.

| Library | Purpose |
|---------|---------|
| EnTT | ECS |
| Jolt Physics | Physics |
| Dear ImGui | Editor UI (docking branch) |
| AngelScript | Gameplay scripting |
| SDL2 | Experimental non-Windows window/input path |
| Recast/Detour | Navigation backend |
| miniz and zstd | Compression paths |
| stb_image, cgltf, tinyobjloader, tinyexr | Image/model import paths |
| nlohmann/json | JSON parsing when available |
| VulkanMemoryAllocator and glad | Experimental Vulkan/OpenGL backend support |
| miniaudio | Linked implementation surface; not the active audio-factory fallback |

### Platform Support

| Platform | Compiler | Status |
|----------|----------|--------|
| Windows 11 x64 | MSVC v143 (VS 2022) | `stable-v1` target; blocked and uncertified |
| Windows 10 x64 | MSVC v143 (VS 2022) | Development-compatible path; outside `stable-v1` and not certified |
| Linux | GCC 13+, Clang 17+ | Experimental |
| macOS | Apple Clang (C++23) | Experimental |

---

## CI/CD (GitHub Actions)

Workflow triggers and required/advisory matrix rows are defined in `.github/workflows/` and the readiness contract. Rolling `nightly` artifacts, when published, are unversioned development snapshots rather than stable releases.

---

## Testing

6,963 test definitions across 576 files in `Tests/` with internal framework + CTest.

```powershell
# Registered CTest cases; an empty selection is an error.
ctest --test-dir build -C Debug --output-on-failure --no-tests=error

# Filter cases inside the aggregate SparkTests executable by source file.
$env:SPARK_TEST_FILE = "TestPhysics.cpp"
.\build\bin\Debug\SparkTests.exe
Remove-Item Env:SPARK_TEST_FILE
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
