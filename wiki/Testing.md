# Testing

SparkEngine includes a comprehensive test suite with 71 test files and 864+ test cases using a lightweight internal test framework with CTest integration.

**Source:** `Tests/TestFramework.h`, `Tests/`

## Test Framework

The engine uses its own lightweight test framework (no external test library dependencies). The framework is defined entirely in `Tests/TestFramework.h` and uses a static registry pattern for automatic test discovery.

### Architecture

```
TestFramework.h     — Macros, test registration, assertion tracking
TestMain.cpp        — main() entry point, runs all registered tests
Test*.cpp           — Individual test files (auto-registered)
```

The framework uses three global counters to track test execution:

| Global Variable | Purpose |
|-----------------|---------|
| `g_assertionsPassed` | Total assertions that succeeded |
| `g_assertionsFailed` | Total assertions that failed |
| `g_currentTest` | Name of the currently executing test |

### Test Registration

Tests register themselves at static initialization time via the `TestRegistrar` struct. Each test case is stored in a global `vector<TestCase>` and executed by the main runner.

```cpp
struct TestCase
{
    std::string name;               // Human-readable test name
    std::string file;               // Source file path
    int line;                       // Line number of the TEST() macro
    std::function<void()> func;     // Test body function
};
```

### Test Macros

```cpp
#include "TestFramework.h"

TEST(MyTestName) {
    EXPECT_EQ(value, expected);        // Equality check
    EXPECT_NE(value, unexpected);      // Not-equal check
    EXPECT_TRUE(condition);            // Boolean check
    EXPECT_FALSE(condition);           // Negative boolean check
    EXPECT_NEAR(a, b, tolerance);      // Floating-point comparison
    EXPECT_GT(a, b);                   // Greater-than check
    EXPECT_LT(a, b);                   // Less-than check
    EXPECT_GE(a, b);                   // Greater-or-equal check
    EXPECT_LE(a, b);                   // Less-or-equal check
    EXPECT_THROW(expr, ExceptionType); // Expects a specific exception
    EXPECT_NO_THROW(expr);             // Expects no exception
}
```

### Assertion Macro Reference

| Macro | Condition | Failure Output |
|-------|-----------|----------------|
| `EXPECT_TRUE(expr)` | `expr` is true | "FAIL: expr was false" |
| `EXPECT_FALSE(expr)` | `expr` is false | "FAIL: expr was true" |
| `EXPECT_EQ(a, b)` | `a == b` | "FAIL: a == b (actual != expected)" |
| `EXPECT_NE(a, b)` | `a != b` | "FAIL: a != b (both value)" |
| `EXPECT_NEAR(a, b, t)` | `|a - b| <= t` | "FAIL: |a - b| <= t (diff)" |
| `EXPECT_GT(a, b)` | `a > b` | "FAIL: a > b (actual <= expected)" |
| `EXPECT_LT(a, b)` | `a < b` | "FAIL: a < b (actual >= expected)" |
| `EXPECT_GE(a, b)` | `a >= b` | "FAIL: a >= b (actual < expected)" |
| `EXPECT_LE(a, b)` | `a <= b` | "FAIL: a <= b (actual > expected)" |
| `EXPECT_THROW(expr, T)` | `expr` throws `T` | "FAIL: Expected T from expr" |
| `EXPECT_NO_THROW(expr)` | `expr` throws nothing | "FAIL: Unexpected exception from expr" |

All macros use `do { ... } while(0)` for safe use in if/else blocks. Failed assertions print the file, line, and values to `stderr` but do **not** abort the test -- all assertions in a test body are evaluated.

## Running Tests

### Build with Tests Enabled

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
```

### Run All Tests

```bash
# Via CTest (recommended)
ctest --test-dir build --output-on-failure

# Via direct binary execution
./build/bin/SparkTests
```

### Run Specific Tests

```bash
# CTest pattern matching
ctest --test-dir build -R "TestPhysics"           # Run tests matching pattern
ctest --test-dir build -R "TestECS|TestAnimation"  # Run multiple patterns
ctest --test-dir build -E "TestNetworking"         # Exclude a pattern

# Verbose output
ctest --test-dir build -V

# List all available tests without running them
ctest --test-dir build -N
```

### Run Tests in Parallel

```bash
ctest --test-dir build --output-on-failure -j$(nproc)
```

## Test Categories and Coverage

The 71 test files cover all major engine subsystems:

### Core & Utilities

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestMathUtils` | 11 | Math utility functions, vector operations |
| `TestObjectPool` | 6 | Generic object pool allocation/deallocation |
| `TestRingBuffer` | 14 | Circular buffer operations, wrap-around |
| `TestResult` | 8 | Result/Error type handling |
| `TestStringUtils` | 19 | String manipulation, parsing, formatting |
| `TestColorUtils` | 18 | Color conversion (RGB, HSL, hex) |
| `TestFileUtils` | 15 | File path operations, extension parsing |
| `TestUUID` | 12 | UUID generation and comparison |
| `TestRandomEngine` | 11 | Random number generation, seeding |
| `TestBitFlags` | 14 | Bitwise flag operations |
| `TestFrameAllocator` | 8 | Per-frame linear allocator |
| `TestScopedTimer` | 3 | High-resolution timer scoping |
| `TestThreadSafeQueue` | 10 | Thread-safe queue operations |
| `TestLocalFileCache` | 15 | File caching system |
| `TestConfigParser` | 16 | INI/config file parsing |
| `TestDeltaSmoother` | 10 | Frame delta time smoothing |

### ECS (Entity Component System)

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestECSWorld` | 11 | Entity creation, component add/remove, queries |
| `TestECSIntegration` | 9 | System integration with World |
| `TestFPSComponents` | 23 | Decal, Projectile, Interaction components |
| `TestSprite2DComponents` | 35 | 2D sprite rendering and animation |
| `TestPhysicsComponents` | 22 | RigidBody, Collider component validation |

### Physics

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestPhysicsComponents` | 22 | Physics component creation and validation |
| `TestFrustumCulling` | 11 | View frustum culling accuracy |

### AI & Navigation

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestAIBehaviorTree` | 16 | Behavior tree node execution, composites |
| `TestNavMesh` | 11 | NavMesh pathfinding, A* search |
| `TestSteeringBehaviors` | 15 | Steering: seek, flee, arrive, wander |
| `TestEnvironmentQuery` | 12 | EQS spatial queries |

### Animation

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestAnimationSystem` | 17 | State machines, blending, IK, evaluation |
| `TestAnimationRetargeting` | 9 | Skeleton retargeting between different rigs |
| `TestClothSimulation` | 4 | Cloth physics simulation |

### Networking

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestNetBuffer` | 29 | Network buffer serialization/deserialization |
| `TestNetworkEncryption` | 17 | AES encryption, key exchange |
| `TestClientPrediction` | 5 | Client-side prediction and reconciliation |
| `TestDedicatedServer` | 27 | Server lifecycle, RCON, map rotation |

### Gameplay Systems

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestWeaponSystem` | 18 | Fire modes, reload, recoil, ADS |
| `TestInventorySystem` | 11 | Item add/remove, stacking, weight |
| `TestQuestSystem` | 10 | Quest stages, objectives, completion |
| `TestGameMode` | 5 | Game mode switching and score tracking |
| `TestAchievementSystem` | 5 | Achievement tracking and unlocking |
| `TestDestructionSystem` | 5 | Object destruction and debris |
| `TestCooldown` | 14 | Cooldown timer management |

### Events & Systems

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestEventSystem` | 10 | Pub/sub, subscribe, unsubscribe, publish |
| `TestCoroutineScheduler` | 10 | Coroutine scheduling, yield, resume |
| `TestTween` | 14 | Easing functions, value interpolation |

### Engine Context & Infrastructure

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestEngineContext` | 18 | Service locator, subsystem registration |
| `TestCommandHistory` | 10 | Console command history and recall |
| `TestDebugTools` | 31 | Debug visualization, imgui panels |
| `TestPlayModeManager` | 33 | Play/pause/stop mode transitions |
| `TestInputSystem` | 11 | Input state tracking and mapping |
| `TestInputBindings` | 5 | Configurable key bindings |
| `TestChromeTracing` | 5 | Chrome trace output format |
| `TestPerformanceStats` | 10 | FPS, frame time, memory tracking |

### Graphics & Post-Processing

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestFogSystem` | 17 | Fog calculation (linear, exponential) |
| `TestScreenSpaceEffects` | 16 | SSAO, SSR parameter validation |
| `TestPostProcessingPipeline` | 11 | Effect chain ordering and configuration |
| `TestTemporalEffects` | 11 | TAA settings and jitter patterns |
| `TestMeshLOD` | 8 | LOD distance switching |
| `TestLightManager` | 13 | Light creation, shadow setup |
| `TestUpscalingSystem` | 5 | Resolution upscaling parameters |
| `TestNoiseGenerator` | 7 | Perlin/simplex noise output ranges |
| `TestSplatmapSystem` | 10 | Terrain texture splatmaps |

### Scene & Save

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestSceneSnapshotSerializer` | 19 | Scene snapshot save/load round-trip |
| `TestSaveSystem` | 7 | Serialization, compression, file I/O |
| `TestLoadingScreen` | 4 | Loading screen state management |

### World Systems

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestWeatherSystem` | 8 | Weather transitions, intensity |
| `TestDayNightCycle` | 10 | Time progression, sunrise/sunset |
| `TestSequencer` | 10 | Cinematic timeline tracks and playback |

### Other

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestDialogueSystem` | 4 | Dialogue tree navigation |
| `TestLocalizationSystem` | 6 | String localization and language switching |
| `TestReplaySystem` | 4 | Game replay recording/playback |
| `TestUISystem` | 6 | UI layout and event handling |
| `TestVisualScriptSystem` | 0 | Placeholder for visual scripting tests |

## Adding a New Test

1. Create a test file in `Tests/`:

```cpp
// Tests/TestMyFeature.cpp
#include "TestFramework.h"
#include "MyFeature.h"

TEST(MyFeature_BasicTest) {
    MyFeature feature;
    feature.Initialize();
    EXPECT_TRUE(feature.IsReady());
}

TEST(MyFeature_EdgeCase) {
    MyFeature feature;
    EXPECT_EQ(feature.Compute(0), 0);
    EXPECT_NEAR(feature.Compute(1.0f), 1.0f, 0.001f);
}

TEST(MyFeature_ExceptionHandling) {
    MyFeature feature;
    EXPECT_THROW(feature.InvalidOp(), std::runtime_error);
    EXPECT_NO_THROW(feature.SafeOp());
}

TEST(MyFeature_Comparisons) {
    MyFeature feature;
    EXPECT_GT(feature.GetSize(), 0);
    EXPECT_LE(feature.GetLoad(), 1.0f);
}
```

2. The test is automatically discovered by CMake (tests are globbed in `Tests/CMakeLists.txt`). See [Build System and CMake Modules](Build-System-and-CMake-Modules) for build configuration.

3. Build and run:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

### Test Naming Conventions

Follow these conventions for consistency:

- Test file: `TestFeatureName.cpp` (e.g., `TestPhysicsComponents.cpp`)
- Test case: `FeatureName_DescriptiveAction` (e.g., `PhysicsComponents_CreateDynamicBody`)
- Use underscores to separate the feature from the behavior being tested

### Testing Best Practices

1. **Keep tests independent** -- Each `TEST()` should set up its own state and not depend on other tests
2. **Test edge cases** -- Zero values, empty collections, maximum values
3. **Use `EXPECT_NEAR` for floating-point** -- Never use `EXPECT_EQ` for float comparisons
4. **No external dependencies** -- Tests should run without network, filesystem, or GPU access
5. **Fast execution** -- Individual tests should complete in milliseconds

## CI Integration

Tests run automatically on every push via GitHub Actions. The CI matrix covers multiple platforms, compilers, and configurations.

### CI Build Matrix

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

### Code Coverage

The `coverage` CI job produces lcov reports showing line and branch coverage. To generate coverage locally:

```bash
# Build with coverage flags
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="--coverage" \
  -DCMAKE_C_FLAGS="--coverage"
cmake --build build --parallel $(nproc)

# Run tests to generate coverage data
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..

# Generate coverage report (requires lcov)
lcov --capture --directory build --output-file coverage.info
lcov --remove coverage.info '/usr/*' 'ThirdParty/*' 'Tests/*' --output-file coverage.filtered.info
genhtml coverage.filtered.info --output-directory coverage-report
```

Open `coverage-report/index.html` in a browser to view the report.

### Running Sanitizer Builds Locally

#### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

Or use the preset:

```bash
cmake --preset ci-linux-asan
cmake --build build
cd build && ctest --output-on-failure
```

#### ThreadSanitizer

```bash
cmake --preset ci-linux-tsan
cmake --build build
cd build && ctest --output-on-failure
```

### Matching CI Locally

To reproduce a specific CI failure, match the exact compiler and flags:

```bash
# Linux GCC (matches build-linux-gcc)
cmake -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTS=ON
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..

# Linux Clang (matches build-linux-clang)
cmake -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTS=ON
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

### Clang-Format Check

The CI enforces formatting on every PR. To check locally:

```bash
find SparkEngine/Source GameModules/SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1
```

To auto-fix:

```bash
find SparkEngine/Source GameModules/SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format -i
```

---

## Live Editor Testing (Software Rendering)

The SparkEditor can be tested with full graphics on headless Linux using Xvfb and Mesa llvmpipe software rendering. An automated test script exercises the editor UI, menus, panels, and keyboard shortcuts.

### Quick Start

```bash
# Start virtual framebuffer
Xvfb :99 -screen 0 1920x1080x24 -ac &

# Set environment
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3

# Run automated test suite (21 tests)
python3 tools/test-editor-live.py build/bin/SparkEditor
```

### Editor Test Mode Flags

| Flag | Description |
|------|-------------|
| `--test-mode` | Skip project browser, enable debug console |
| `--test-frames N` | Exit after N rendered frames |
| `--debug-console` | Print diagnostics to stdout |

### What Gets Tested

- Editor launch and clean shutdown
- OpenGL 3.3 rendering via Mesa llvmpipe
- ImGui frame rendering (non-blank, sufficient UI complexity)
- Menu bar interaction (File, Edit, Window menus)
- Panel toggle via Window menu (Scene View, Asset Browser, Profiler, Material Editor)
- Keyboard shortcuts (Ctrl+Z, Ctrl+S, Ctrl+N)
- Hierarchy panel click and right-click context menu
- 5-second stress test (no crashes)

### Prerequisites

System packages: `xvfb`, `libgl-dev`, `xdotool`, Python 3 with Pillow.
SDL2 must be built with OpenGL/GLX support (install `libgl-dev` *before* building SDL2).

---

## See Also

- [Build System and CMake Modules](Build-System-and-CMake-Modules) -- BUILD_TESTS flag and CI details
- [Getting Started](Getting-Started) -- Building the project
- [Contributing](Contributing) -- Contribution workflow, pre-commit checks, and adding tests

## Test File Inventory

<!-- AUTO:test_inventory -->
*179 test files, 2168+ test cases*

| Test File | Test Cases |
|-----------|------------|
| `TestAIBehaviorTree` | 16 |
| `TestAIStress` | 18 |
| `TestAbilitySystem` | 15 |
| `TestAdversarialEngine` | 89 |
| `TestAlignedHeapArray` | 6 |
| `TestAngleUtils` | 10 |
| `TestAnimationCompression` | 6 |
| `TestAnimationRetargeting` | 9 |
| `TestAnimationStress` | 12 |
| `TestAnimationSystem` | 17 |
| `TestAsyncDatabase` | 23 |
| `TestAudioEngine` | 18 |
| `TestBitFlags` | 14 |
| `TestBitUtils` | 10 |
| `TestBlendSpace` | 6 |
| `TestCameraInterpolation` | 9 |
| `TestChromeTracing` | 5 |
| `TestClientPrediction` | 5 |
| `TestClothSimulation` | 4 |
| `TestClusteredLightGPU` | 7 |
| `TestCollaborativeEditing` | 20 |
| `TestCollisionAvoidance` | 4 |
| `TestCollisionLayers` | 10 |
| `TestColorUtils` | 18 |
| `TestCommandHistory` | 10 |
| `TestConditionSystem` | 12 |
| `TestConfigParser` | 16 |
| `TestConnectionScope` | 8 |
| `TestConnectionTimeout` | 9 |
| `TestConsoleRBAC` | 21 |
| `TestConstantBufferDiff` | 2 |
| `TestContainerUtils` | 10 |
| `TestCooldown` | 14 |
| `TestCoroutineScheduler` | 10 |
| `TestCoverSystem` | 4 |
| `TestDatablockRegistry` | 10 |
| `TestDayNightCycle` | 10 |
| `TestDebugHookManager` | 27 |
| `TestDebugTools` | 36 |
| `TestDebugUtilities` | 28 |
| `TestDedicatedServer` | 27 |
| `TestDeferredDeletion` | 6 |
| `TestDeferredQueue` | 6 |
| `TestDelegate` | 9 |
| `TestDeltaSmoother` | 10 |
| `TestDestructionSystem` | 5 |
| `TestDialogueStress` | 10 |
| `TestDialogueSystem` | 4 |
| `TestDirtyRectTracker` | 4 |
| `TestDrawIndirect` | 6 |
| `TestDynamicResponseSystem` | 6 |
| `TestECSIntegration` | 9 |
| `TestECSStress` | 10 |
| `TestECSWorld` | 11 |
| `TestEngineContext` | 18 |
| `TestEngineDiagnostics` | 4 |
| `TestEngineLoadTest` | 9 |
| `TestEngineMonitor` | 10 |
| `TestEntityArchetype` | 6 |
| `TestEntityEventBus` | 11 |
| `TestEnvironmentQuery` | 12 |
| `TestEventBus` | 15 |
| `TestEventSystem` | 10 |
| `TestExtendedSystems` | 38 |
| `TestFPSComponents` | 23 |
| `TestFaultIsolation` | 14 |
| `TestFileUtils` | 15 |
| `TestFixtures` | 0 |
| `TestFogSystem` | 17 |
| `TestFormationSystem` | 4 |
| `TestFrameAllocator` | 8 |
| `TestFreezeSystem` | 5 |
| `TestFrustumCulling` | 11 |
| `TestFullEngineDiagnostics` | 7 |
| `TestGPUPerfCounters` | 2 |
| `TestGameMode` | 5 |
| `TestGameplayStress` | 15 |
| `TestGraphicsEngine` | 10 |
| `TestGraphicsInitFallback` | 5 |
| `TestGraphicsIntegration` | 27 |
| `TestGraphicsStress` | 15 |
| `TestGroupAI` | 5 |
| `TestHash` | 18 |
| `TestHybridRT` | 20 |
| `TestInputBindings` | 5 |
| `TestInputSystem` | 11 |
| `TestInstanceManager` | 14 |
| `TestInventorySystem` | 11 |
| `TestJsonUtils` | 23 |
| `TestLightManager` | 13 |
| `TestLoadingScreen` | 4 |
| `TestLocalFileCache` | 15 |
| `TestLocalizationSystem` | 6 |
| `TestLockFreeRingAllocator` | 3 |
| `TestMaterialDefinition` | 10 |
| `TestMaterialEffects` | 5 |
| `TestMathUtils` | 11 |
| `TestMeshLOD` | 8 |
| `TestModuleDependency` | 5 |
| `TestModuleDiscovery` | 6 |
| `TestModuleHotReload` | 15 |
| `TestMovementSystem` | 12 |
| `TestMultiISADispatch` | 2 |
| `TestNavMesh` | 11 |
| `TestNetBuffer` | 29 |
| `TestNetworkEncryption` | 17 |
| `TestNetworkIntegration` | 31 |
| `TestNetworkInterpolation` | 12 |
| `TestNetworkMMOIntegration` | 11 |
| `TestNetworkStress` | 21 |
| `TestNoiseGenerator` | 7 |
| `TestNullRHIDevice` | 3 |
| `TestObjectPool` | 6 |
| `TestOcclusionCulling` | 6 |
| `TestParallelCulling` | 5 |
| `TestPathCache` | 6 |
| `TestPerformanceStats` | 10 |
| `TestPhysicsComponents` | 22 |
| `TestPhysicsInterpolation` | 8 |
| `TestPhysicsStress` | 16 |
| `TestPhysicsSystem` | 29 |
| `TestPlayModeManager` | 33 |
| `TestPoseModifier` | 4 |
| `TestPostProcessingPipeline` | 16 |
| `TestProximityTriggerSystem` | 4 |
| `TestQuestSystem` | 10 |
| `TestRHIHandlePool` | 10 |
| `TestRandomEngine` | 11 |
| `TestRecastIntegration` | 6 |
| `TestReflection` | 7 |
| `TestReliableChannel` | 9 |
| `TestRenderCommandRing` | 4 |
| `TestRenderGraph` | 24 |
| `TestReplicationFields` | 15 |
| `TestResult` | 8 |
| `TestRingBuffer` | 14 |
| `TestSHLighting` | 7 |
| `TestSaveSystem` | 7 |
| `TestSceneConfigDatabase` | 3 |
| `TestSceneManager` | 19 |
| `TestSceneSnapshotSerializer` | 19 |
| `TestScheduledCallback` | 8 |
| `TestScopeGuard` | 13 |
| `TestScopedTimer` | 3 |
| `TestScreenSpaceEffects` | 16 |
| `TestScriptHookManager` | 15 |
| `TestSequencer` | 10 |
| `TestSerializer` | 17 |
| `TestServerLiveMockClient` | 10 |
| `TestServerMockClient` | 31 |
| `TestShaderGraphCompiler` | 5 |
| `TestShadowAtlas` | 7 |
| `TestSkyAtmosphere` | 5 |
| `TestSoftwareRendering` | 5 |
| `TestSpatialGrid` | 16 |
| `TestSplineMath` | 24 |
| `TestSprite2DComponents` | 35 |
| `TestStateMachine` | 16 |
| `TestSteeringBehaviors` | 15 |
| `TestStringPool` | 8 |
| `TestStringUtils` | 19 |
| `TestTacticalPointSystem` | 4 |
| `TestTemporalEffects` | 11 |
| `TestTerrainRenderer` | 5 |
| `TestThreadSafeQueue` | 10 |
| `TestTimeOfDaySystem` | 18 |
| `TestTransientBufferAllocator` | 10 |
| `TestTween` | 14 |
| `TestTypeTraits` | 11 |
| `TestUISystem` | 6 |
| `TestUUID` | 12 |
| `TestUpscalingSystem` | 10 |
| `TestUtilsStress` | 13 |
| `TestVersionedHandle` | 9 |
| `TestVulkanLavapipe` | 4 |
| `TestWaterRenderer` | 6 |
| `TestWeaponSystem` | 18 |
| `TestWeatherSystem` | 8 |
| `TestWorkSema` | 2 |
<!-- /AUTO:test_inventory -->
