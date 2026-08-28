# Testing

SparkEngine includes a comprehensive test suite using a lightweight internal test framework with CTest integration. For current test file and test case counts, see the auto-generated inventory section on this page.

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

### CI Test Evidence

The primary Windows, Linux, and macOS jobs retain the runner's granular JUnit
XML plus a JSON summary containing executed, passed, failed, errored, skipped,
duration, and slowest-test fields. The summary parser fails closed when the
report is missing, empty, malformed, contains a failure, or records fewer than
the expected registration floor. This prevents an executable launch failure
from being reported as a zero-failure test run. Repository badges count source
test definitions separately because platform and feature gates affect the
runtime set.

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

### Run Windows Tests Under Wine (Cross-Compilation)

Cross-compile with MinGW and run the exact same Windows D3D11 code paths under Wine on Linux:

```bash
# Build Windows .exe
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --parallel $(nproc)

# Run the full SparkTests suite under Wine
tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe

# Or run the full automated test suite (unit tests + live engine + stress + break tests)
python3 tools/test-windows-wine.py --build-dir build/linux-mingw-release
```

Results vary by branch and platform image. See [Cross-Compilation: Wine Testing](../platform/Cross-Compilation-Wine-Testing.md) for full setup and troubleshooting.

### Engine/Editor Test Mode Flags

Both the engine and editor support `--test-frames N` for automated testing:

```bash
# Engine: run 60 frames then exit
./SparkEngine -test-frames 60                              # Linux
wine64 SparkEngine.exe -test-frames 60                     # Wine

# Editor: skip project browser and run 120 frames
./SparkEditor --test-mode --test-frames 120                # Linux
wine64 SparkEditor.exe --test-mode --test-frames 120       # Wine
```

## Test Categories and Coverage

The test files cover all major engine subsystems:

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
| `TestNetworkEncryption` | 17 | Standalone mirror of legacy XOR/FNV prototype behavior; does not execute production networking and is not security evidence |
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

2. Add the source file to the explicit test-source list in `Tests/CMakeLists.txt`. CI runs `Tools/check-test-registration.sh` and fails when a test source is present but not registered. See [Build System and CMake Modules](Build-System-and-CMake-Modules.md) for build configuration.

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
| `build-linux-asan` | ubuntu-24.04 | GCC | Debug | ASan + UBSan + LSan |
| `build-linux-tsan` | ubuntu-24.04 | GCC | Debug | TSan (thread races) |
| `build-linux-msan` | ubuntu-24.04 | Clang + libc++ | Debug | MSan (`continue-on-error`) |
| `build-windows-vs2022` | windows-latest | MSVC v143 | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-windows-vs2026` | windows-latest | MSVC v145 | Debug, Release | `continue-on-error` |
| `build-linux-mingw-wine` | ubuntu-24.04 | MinGW-w64 + Wine | Release | `continue-on-error` |
| `build-macos` | macos-latest | Apple Clang | Debug, Release | `continue-on-error` |
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

- [Build System and CMake Modules](Build-System-and-CMake-Modules.md) -- BUILD_TESTS flag and CI details
- [Getting Started](../getting-started/Getting-Started.md) -- Building the project
- [Contributing](Contributing.md) -- Contribution workflow, pre-commit checks, and adding tests

## Test File Inventory

<!-- AUTO:test_inventory -->
*574 test files, 6892 source-level test definitions*

| Test File | Test Definitions |
|-----------|------------------|
| `TestSubsystemIntegrationScenarios` | 4 |
| `TestAIBehaviorTree` | 16 |
| `TestAIBudgetLimiter` | 6 |
| `TestAIDebugRenderer` | 7 |
| `TestAIDebugRendererPhaseDD` | 9 |
| `TestAIDebugRendererReal` | 8 |
| `TestAIDirector` | 11 |
| `TestAIDirectorPhaseII` | 8 |
| `TestAIIntegratedSystem` | 11 |
| `TestAIStress` | 18 |
| `TestAbilitySystem` | 27 |
| `TestAbilitySystemReal` | 9 |
| `TestAccessibility` | 15 |
| `TestAchievementSystem` | 14 |
| `TestAchievementSystemReal` | 10 |
| `TestAdvancedAssetPipeline` | 4 |
| `TestAdversarialEngine` | 96 |
| `TestAlignedHeapArray` | 6 |
| `TestAlignedHeapArrayReal` | 6 |
| `TestAngelScriptEngine` | 14 |
| `TestAngleUtils` | 10 |
| `TestAngleUtilsReal` | 7 |
| `TestAnimNotify` | 10 |
| `TestAnimationCompression` | 6 |
| `TestAnimationCompressionReal` | 5 |
| `TestAnimationPhysicsIntegration` | 8 |
| `TestAnimationRetargeting` | 9 |
| `TestAnimationStress` | 12 |
| `TestAnimationSystem` | 17 |
| `TestAreaAssetLoader` | 12 |
| `TestAreaSimulationHook` | 6 |
| `TestAssertSuppression` | 9 |
| `TestAssertSuppressionReal` | 8 |
| `TestAssetDatabase` | 4 |
| `TestAssetDependencyGraph` | 19 |
| `TestAssetMigration` | 21 |
| `TestAssetMigrationPhaseEE` | 10 |
| `TestAssetPipelineCache` | 22 |
| `TestAssetPipelineIntegration` | 16 |
| `TestAssetPipelineReal` | 17 |
| `TestAssetServiceClient` | 13 |
| `TestAssetStallDetector` | 9 |
| `TestAssetValidator` | 7 |
| `TestAsyncComputeScheduler` | 9 |
| `TestAsyncComputeSchedulerPhaseCC` | 12 |
| `TestAsyncDatabase` | 23 |
| `TestAsyncDatabaseRegressions` | 3 |
| `TestAtomicSharedPtr` | 3 |
| `TestAtomicSharedPtrReal` | 7 |
| `TestAudioBackendFactory` | 5 |
| `TestAudioEngine` | 18 |
| `TestAudioMixerBus` | 7 |
| `TestAutoLODPerformance` | 2 |
| `TestBVHAccelerator` | 10 |
| `TestBehaviorTreeNodes` | 22 |
| `TestBenchmarkFramework` | 16 |
| `TestBitFlags` | 14 |
| `TestBitFlagsReal` | 4 |
| `TestBitUtils` | 10 |
| `TestBitUtilsReal` | 8 |
| `TestBlendSpace` | 6 |
| `TestBlendSpaceReal` | 6 |
| `TestCSGEditorPanel` | 10 |
| `TestCSGSystem` | 12 |
| `TestCacheDebuggerPhaseFF` | 9 |
| `TestCachedShadowAtlas` | 13 |
| `TestCameraInterpolation` | 9 |
| `TestCameraTransforms` | 26 |
| `TestChromeTracing` | 5 |
| `TestClientPrediction` | 12 |
| `TestClothSimulation` | 7 |
| `TestClusteredLightGPU` | 7 |
| `TestCollaborativeEditing` | 23 |
| `TestCollisionAvoidance` | 8 |
| `TestCollisionLayers` | 10 |
| `TestCollisionSystem` | 22 |
| `TestColorUtils` | 18 |
| `TestColorUtilsReal` | 4 |
| `TestCommandHistory` | 10 |
| `TestCompressionUtils` | 4 |
| `TestCompressionUtilsReal` | 7 |
| `TestConditionSystem` | 12 |
| `TestConfigParser` | 16 |
| `TestConfigParserReal` | 9 |
| `TestConnectionScope` | 8 |
| `TestConnectionScopeFilter` | 5 |
| `TestConnectionScopeWiring` | 6 |
| `TestConnectionTimeout` | 9 |
| `TestConsoleRBAC` | 21 |
| `TestConsoleVariables` | 31 |
| `TestConstantBufferDiff` | 8 |
| `TestConstantBufferDiffReal` | 9 |
| `TestConstantBufferRing` | 9 |
| `TestContainerUtils` | 10 |
| `TestContainerUtilsReal` | 10 |
| `TestContracts` | 6 |
| `TestCooldown` | 14 |
| `TestCooldownReal` | 9 |
| `TestCoreAndBuildSystems` | 39 |
| `TestCoroutineScheduler` | 10 |
| `TestCoverSystem` | 4 |
| `TestCoverSystemReal` | 5 |
| `TestCoverageAI` | 9 |
| `TestCoverageCamera` | 4 |
| `TestCoverageScripting` | 5 |
| `TestCpuDebuggerPhaseGG` | 8 |
| `TestCpuNeuralInference` | 14 |
| `TestCpuNeuralTraining` | 13 |
| `TestCrashReportUploader` | 8 |
| `TestCrossSystemIntegration` | 4 |
| `TestDXRSupport` | 13 |
| `TestDaemonCodexFixes` | 4 |
| `TestDaemonConcurrent` | 6 |
| `TestDaemonDiagnostics` | 11 |
| `TestDaemonFoundation` | 5 |
| `TestDaemonLRU` | 15 |
| `TestDaemonLifecycle` | 11 |
| `TestDaemonProtocol` | 10 |
| `TestDataTableSystem` | 11 |
| `TestDatablockRegistry` | 10 |
| `TestDatablockRegistryPhaseHH` | 8 |
| `TestDayNightCycle` | 10 |
| `TestDeadlockDetector` | 8 |
| `TestDebugHookManager` | 28 |
| `TestDebugTools` | 37 |
| `TestDebugUtilities` | 28 |
| `TestDecalSystem` | 7 |
| `TestDedicatedServer` | 27 |
| `TestDedicatedServerProcessController` | 3 |
| `TestDedicatedServerRuntime` | 7 |
| `TestDeferredDeletion` | 6 |
| `TestDeferredDeletionReal` | 6 |
| `TestDeferredQueue` | 6 |
| `TestDelegate` | 9 |
| `TestDelegateReal` | 8 |
| `TestDeltaSmoother` | 10 |
| `TestDeltaSmootherReal` | 8 |
| `TestDenoiserInterface` | 14 |
| `TestDescriptorCache` | 4 |
| `TestDestructionSystem` | 5 |
| `TestDialogueStress` | 10 |
| `TestDialogueSystem` | 8 |
| `TestDirectStorageLoader` | 11 |
| `TestDirectionalStreaming` | 3 |
| `TestDirtyRectTracker` | 9 |
| `TestDirtyRectTrackerReal` | 12 |
| `TestDirtyRegionGridPhaseDD` | 10 |
| `TestDrawIndirect` | 6 |
| `TestDynamicQualityScalerPhaseBB` | 11 |
| `TestDynamicResponseSystem` | 6 |
| `TestECSIntegration` | 9 |
| `TestECSStress` | 10 |
| `TestECSWorld` | 11 |
| `TestECSystemOrdering` | 15 |
| `TestECSystemSpecialized` | 27 |
| `TestECSystemsReal` | 12 |
| `TestEcsCameraConsole` | 1 |
| `TestEditorAutomation` | 9 |
| `TestEditorCommands` | 8 |
| `TestEditorDocumentTransition` | 7 |
| `TestEditorLayoutManager` | 13 |
| `TestEditorSubsystems` | 128 |
| `TestEditorWindowManager` | 14 |
| `TestEngineBootPlatforms` | 41 |
| `TestEngineContext` | 18 |
| `TestEngineDiagnostics` | 4 |
| `TestEngineInterfaceProtocol` | 4 |
| `TestEngineLifecycle` | 22 |
| `TestEngineLoadTest` | 21 |
| `TestEngineMonitor` | 10 |
| `TestEngineSettingsEdgeCases` | 45 |
| `TestEngineSettingsParser` | 27 |
| `TestEngineSettingsReal` | 13 |
| `TestEntityArchetype` | 5 |
| `TestEntityEventBus` | 11 |
| `TestEntityEventBusReal` | 6 |
| `TestEntityPresetManager` | 10 |
| `TestEntityPresetManagerPhaseEE` | 7 |
| `TestEnvironmentQuery` | 12 |
| `TestEventBus` | 15 |
| `TestEventBusReal` | 7 |
| `TestEventResponseSystem` | 15 |
| `TestEventResponseSystemPhaseEE` | 8 |
| `TestEventSystem` | 10 |
| `TestExtendedSystems` | 38 |
| `TestFBXImportValidation` | 3 |
| `TestFBXImporter` | 17 |
| `TestFPSComponents` | 23 |
| `TestFPSGameplayIntegration` | 17 |
| `TestFPSMultiplayer` | 11 |
| `TestFastNoise2SIMD` | 29 |
| `TestFaultIsolation` | 14 |
| `TestFaultIsolationReal` | 8 |
| `TestFileUtils` | 17 |
| `TestFileUtilsReal` | 7 |
| `TestFileWatcher` | 11 |
| `TestFixtures` | 4 |
| `TestFogSystem` | 17 |
| `TestFoliageImpostorBaker` | 21 |
| `TestFoliageRenderer` | 31 |
| `TestFoliageSystem` | 10 |
| `TestFontSystem` | 13 |
| `TestFormationSystem` | 9 |
| `TestFrameAllocator` | 8 |
| `TestFreezeDetector` | 10 |
| `TestFreezeSystem` | 5 |
| `TestFrustumCulling` | 11 |
| `TestFullEngineDiagnostics` | 7 |
| `TestGLSLPipelineIntegration` | 19 |
| `TestGLTFStaticMeshLoader` | 8 |
| `TestGPUClusterCulling` | 11 |
| `TestGPUDrivenRenderer` | 14 |
| `TestGPUDrivenRendererD3D11` | 2 |
| `TestGPUParticleSystem` | 11 |
| `TestGPUPerfCounters` | 9 |
| `TestGPUProfiler` | 8 |
| `TestGPUResourceLeakDetector` | 10 |
| `TestGPUSkinning` | 9 |
| `TestGPUStallProfiler` | 6 |
| `TestGPUStallProfilerPhaseCC` | 10 |
| `TestGTAOEffect` | 8 |
| `TestGameMode` | 5 |
| `TestGameModeReal` | 11 |
| `TestGameModuleMMO` | 36 |
| `TestGameModulePlatformerARPG` | 37 |
| `TestGameModuleRPG` | 35 |
| `TestGameModuleRTS` | 39 |
| `TestGameModuleRacing` | 28 |
| `TestGameObjectTransforms` | 24 |
| `TestGamePackager` | 10 |
| `TestGameViewPanel` | 3 |
| `TestGamepadInputProcessing` | 23 |
| `TestGameplayDebugger` | 11 |
| `TestGameplayExtensionRegistry` | 7 |
| `TestGameplayStress` | 15 |
| `TestGameplaySystemExtension` | 6 |
| `TestGameplayTags` | 14 |
| `TestGameplayTagsReal` | 7 |
| `TestGatewayAreaControl` | 10 |
| `TestGatewaySecurity` | 11 |
| `TestGizmoMath` | 3 |
| `TestGoldenImageTest` | 14 |
| `TestGraphicsEngine` | 14 |
| `TestGraphicsInitFallback` | 5 |
| `TestGraphicsIntegration` | 33 |
| `TestGraphicsStress` | 15 |
| `TestGraphicsSubsystems` | 45 |
| `TestGroupAI` | 5 |
| `TestHLODBuilderPhaseII` | 8 |
| `TestHLODSystem` | 9 |
| `TestHRTFProcessor` | 8 |
| `TestHResultPlatform` | 24 |
| `TestHash` | 18 |
| `TestHashReal` | 9 |
| `TestHitchDetector` | 10 |
| `TestHybridRT` | 20 |
| `TestInGameConsole` | 12 |
| `TestInputActionSystem` | 12 |
| `TestInputBindings` | 5 |
| `TestInputManagerState` | 21 |
| `TestInputSystem` | 11 |
| `TestInstanceManager` | 14 |
| `TestInventorySystem` | 11 |
| `TestInventorySystemReal` | 11 |
| `TestJobSystem` | 11 |
| `TestJsonStrict` | 20 |
| `TestJsonUtils` | 23 |
| `TestLODGenerator` | 7 |
| `TestLODGeneratorPhaseGG` | 9 |
| `TestLagCompensation` | 12 |
| `TestLagCompensationIntegration` | 4 |
| `TestLauncherPaths` | 4 |
| `TestLauncherProcess` | 4 |
| `TestLevelStreamingSystemPhaseAA` | 11 |
| `TestLightManager` | 13 |
| `TestLightmapBaker` | 9 |
| `TestLoadingScreen` | 11 |
| `TestLoadingScreenReal` | 6 |
| `TestLocalFileCache` | 15 |
| `TestLocalizationSystem` | 6 |
| `TestLockFreeRingAllocator` | 8 |
| `TestLockFreeRingAllocatorReal` | 10 |
| `TestLogger` | 18 |
| `TestLootAndCrafting` | 11 |
| `TestMMOCredentialSecurity` | 3 |
| `TestMacOSPlatform` | 6 |
| `TestMaterialDefinition` | 10 |
| `TestMaterialEffects` | 5 |
| `TestMaterialSystemEdgeCases` | 10 |
| `TestMaterialSystemIntegration` | 14 |
| `TestMaterialSystemReal` | 13 |
| `TestMaterialSystemValidation` | 31 |
| `TestMathUtils` | 11 |
| `TestMathUtilsExtendedPhaseGG` | 5 |
| `TestMemoryDebugger` | 16 |
| `TestMemoryIntegrity` | 16 |
| `TestMemoryMonitor` | 11 |
| `TestMeshLOD` | 8 |
| `TestMeshOptimizer` | 15 |
| `TestMeshShaderPipeline` | 9 |
| `TestMetalRayTracing` | 16 |
| `TestMetalRayTracingLive` | 10 |
| `TestModSystem` | 9 |
| `TestModuleABI` | 23 |
| `TestModuleDependency` | 5 |
| `TestModuleDiscovery` | 6 |
| `TestModuleHotReload` | 12 |
| `TestMovementSystem` | 18 |
| `TestMovieRenderPipeline` | 11 |
| `TestMultiISADispatch` | 7 |
| `TestMusicManager` | 9 |
| `TestNavMesh` | 11 |
| `TestNavMeshLink` | 5 |
| `TestNavMeshObstacles` | 7 |
| `TestNetBuffer` | 29 |
| `TestNetQuantize` | 12 |
| `TestNetworkDebugPanel` | 11 |
| `TestNetworkEncryption` | 17 |
| `TestNetworkHealthMonitor` | 10 |
| `TestNetworkIntegration` | 32 |
| `TestNetworkInterpolation` | 12 |
| `TestNetworkMMOIntegration` | 11 |
| `TestNetworkManagerEdgeCases` | 30 |
| `TestNetworkManagerIntegration` | 33 |
| `TestNetworkManagerOrchestration` | 27 |
| `TestNetworkManagerReal` | 23 |
| `TestNetworkReplicationIntegration` | 13 |
| `TestNetworkSecurity` | 12 |
| `TestNetworkSecurityPhaseHH` | 8 |
| `TestNetworkStack` | 2 |
| `TestNetworkStress` | 21 |
| `TestNeuralInference` | 17 |
| `TestNeuralPostProcessing` | 9 |
| `TestNeuralRadianceCache` | 7 |
| `TestNeuralTextureCompressor` | 10 |
| `TestNoiseGenerator` | 7 |
| `TestNullRHIDevice` | 7 |
| `TestNullRHIDevicePhaseY` | 22 |
| `TestObjectPool` | 6 |
| `TestObjectPoolReal` | 7 |
| `TestOcclusionCulling` | 6 |
| `TestOnlineServices` | 10 |
| `TestOpaqueHandle` | 7 |
| `TestOpenWorldModule` | 61 |
| `TestPacketValidator` | 10 |
| `TestPacketValidatorReal` | 3 |
| `TestParallelCulling` | 5 |
| `TestPasswordHash` | 3 |
| `TestPathCache` | 6 |
| `TestPerceptionSystemMath` | 25 |
| `TestPerformanceStats` | 10 |
| `TestPerformanceStatsReal` | 4 |
| `TestPersistentMaterialCB` | 19 |
| `TestPhysicsComponents` | 22 |
| `TestPhysicsECSIntegration` | 10 |
| `TestPhysicsInterpolation` | 8 |
| `TestPhysicsStress` | 16 |
| `TestPhysicsSystem` | 29 |
| `TestPhysicsTeardownGuard` | 5 |
| `TestPlatformInput` | 11 |
| `TestPlayModeManager` | 34 |
| `TestPluginABI` | 17 |
| `TestPortalCulling` | 14 |
| `TestPoseModifier` | 8 |
| `TestPostProcessingPipeline` | 16 |
| `TestPostProcessingPipelineD3D11` | 1 |
| `TestPostProcessingPipelinePhaseJ` | 20 |
| `TestPostProcessingPipelinePhaseK` | 11 |
| `TestPostProcessingPipelinePhaseN` | 7 |
| `TestPrefabManager` | 4 |
| `TestProceduralGenerator` | 13 |
| `TestProcess` | 20 |
| `TestProcessDrawListLinux` | 9 |
| `TestProfiler` | 19 |
| `TestProximityTriggerSystem` | 4 |
| `TestQuestSystem` | 11 |
| `TestRHIBridgeIntegration` | 19 |
| `TestRHICapabilityParity` | 4 |
| `TestRHIHandlePool` | 10 |
| `TestRHIHandlePoolPhaseX` | 15 |
| `TestRTHandleSystem` | 15 |
| `TestRandomEngine` | 11 |
| `TestRecastIntegration` | 6 |
| `TestReflectedScene` | 7 |
| `TestReflectedSceneEmissiveHierarchy` | 6 |
| `TestReflection` | 18 |
| `TestReflectionProbeCache` | 16 |
| `TestReflectionReal` | 22 |
| `TestRegionMapDataSource` | 7 |
| `TestReliableChannel` | 22 |
| `TestRemoteDebugSystem` | 11 |
| `TestRenderCommandRing` | 8 |
| `TestRenderECSIntegration` | 8 |
| `TestRenderGraph` | 36 |
| `TestReplaySystem` | 8 |
| `TestReplicationFields` | 15 |
| `TestResult` | 8 |
| `TestRingBuffer` | 14 |
| `TestRingBufferReal` | 7 |
| `TestRuntimePackage` | 2 |
| `TestRuntimePrefab` | 19 |
| `TestSHLighting` | 7 |
| `TestSSAOTemporalFilter` | 8 |
| `TestSafetyCoreUtils` | 17 |
| `TestSaveSystem` | 7 |
| `TestSceneConfigDatabase` | 3 |
| `TestSceneConfigDatabaseReal` | 9 |
| `TestSceneGraph2D` | 14 |
| `TestSceneManager` | 19 |
| `TestSceneRoundtrip` | 8 |
| `TestSceneSerializer` | 13 |
| `TestSceneSerializerReal` | 18 |
| `TestSceneSnapshotSerializer` | 20 |
| `TestScheduledCallback` | 8 |
| `TestScopeGuard` | 13 |
| `TestScopedTimer` | 7 |
| `TestScreenCapture` | 9 |
| `TestScreenSpaceEffects` | 16 |
| `TestScriptHookManager` | 15 |
| `TestScriptHookManagerPhaseBB` | 14 |
| `TestScriptHotReload` | 16 |
| `TestScriptSandbox` | 7 |
| `TestSeamlessAreaManager` | 14 |
| `TestSecureRandom` | 3 |
| `TestSelectionManager` | 23 |
| `TestSelfRecovery` | 16 |
| `TestSequencer` | 10 |
| `TestSequencerAudioWiring` | 3 |
| `TestSequencerReal` | 9 |
| `TestSerializer` | 17 |
| `TestServerLiveMockClient` | 10 |
| `TestServerMockClient` | 31 |
| `TestServiceTopologyController` | 10 |
| `TestShaderCrossCompilerPhaseW` | 19 |
| `TestShaderDiskCache` | 6 |
| `TestShaderDiskCacheDaemon` | 8 |
| `TestShaderDiskCachePhaseV` | 16 |
| `TestShaderGraphCompiler` | 8 |
| `TestShaderHotReload` | 8 |
| `TestShaderHotReloadCompilation` | 9 |
| `TestShaderHotReloadPhaseU` | 9 |
| `TestShaderServiceClient` | 11 |
| `TestShaderVariantSystem` | 21 |
| `TestShadowAtlas` | 7 |
| `TestSkyAtmosphere` | 5 |
| `TestSoftwareRendering` | 5 |
| `TestSparkBuildConfig` | 4 |
| `TestSparkConsoleConcurrency` | 3 |
| `TestSparkEngineCameraOwnership` | 1 |
| `TestSparkError` | 6 |
| `TestSparkGameARPG` | 5 |
| `TestSparkGamePlatformer` | 5 |
| `TestSparkGameRPG` | 5 |
| `TestSparkGameRTS` | 5 |
| `TestSparkGameRacing` | 5 |
| `TestSparkGatewayCoordinator` | 7 |
| `TestSparkPak` | 15 |
| `TestSparkServerApplication` | 8 |
| `TestSpatialGrid` | 16 |
| `TestSpatialGridReal` | 7 |
| `TestSplineMath` | 24 |
| `TestSplineMathReal` | 7 |
| `TestSpringArm` | 6 |
| `TestSpringArmReal` | 8 |
| `TestSprite2DComponents` | 35 |
| `TestStackTrace` | 16 |
| `TestStartupSplash` | 7 |
| `TestStateMachine` | 16 |
| `TestStateMachineReal` | 7 |
| `TestSteeringBehaviors` | 15 |
| `TestSteeringBehaviorsReal` | 5 |
| `TestStringPool` | 8 |
| `TestStringUtils` | 19 |
| `TestStringUtilsReal` | 9 |
| `TestSubTickInput` | 5 |
| `TestSubsystemConsoleCommands` | 14 |
| `TestSystemManagerIntegration` | 11 |
| `TestTFAbilityWire` | 6 |
| `TestTFCaptureMath` | 7 |
| `TestTFChatRules` | 11 |
| `TestTFDamageModel` | 18 |
| `TestTFDataTables` | 23 |
| `TestTFDeathRecapWire` | 5 |
| `TestTFFixedStep` | 1 |
| `TestTFNetProtocolLayout` | 9 |
| `TestTFOnboarding` | 37 |
| `TestTFOutfitStore` | 16 |
| `TestTFRedeployRules` | 7 |
| `TestTFRegionLattice` | 11 |
| `TestTFSecondaryMotion` | 7 |
| `TestTFServerValidation` | 15 |
| `TestTFSocialStore` | 7 |
| `TestTacticalPointSystem` | 4 |
| `TestTelemetry` | 15 |
| `TestTelemetryPhaseFF` | 7 |
| `TestTemplatesCompile` | 43 |
| `TestTemporalEffects` | 11 |
| `TestTerrainRenderer` | 5 |
| `TestTextureCompressor` | 9 |
| `TestTextureCompressorPhaseGG` | 6 |
| `TestTextureZombiePool` | 6 |
| `TestThirdPartyIntegration` | 22 |
| `TestThreadDebugger` | 22 |
| `TestThreadSafeQueue` | 10 |
| `TestTimeOfDaySystem` | 18 |
| `TestTimerManager` | 11 |
| `TestTimerManagerReal` | 8 |
| `TestTimerReal` | 6 |
| `TestTransientBufferAllocator` | 10 |
| `TestTransientBufferAllocatorPhaseX` | 14 |
| `TestTutorialSystem` | 22 |
| `TestTween` | 14 |
| `TestTweenReal` | 8 |
| `TestTypeTraits` | 11 |
| `TestUICompositor` | 13 |
| `TestUILayoutExtensions` | 29 |
| `TestUISystem` | 6 |
| `TestUISystemPhaseR` | 7 |
| `TestUUID` | 12 |
| `TestUndoRedoManager` | 7 |
| `TestUndoRedoManagerProduction` | 2 |
| `TestUpscalingSystem` | 10 |
| `TestUtilsStress` | 13 |
| `TestVRSystem` | 12 |
| `TestVersionControlSystemPhaseAA` | 11 |
| `TestVersionedHandle` | 9 |
| `TestVideoPlayer` | 12 |
| `TestVisualScriptCompiler` | 25 |
| `TestVolumeManager` | 11 |
| `TestVolumetricClouds` | 14 |
| `TestVoxelConeTracing` | 24 |
| `TestVulkanLavapipe` | 8 |
| `TestWARPRendering` | 4 |
| `TestWaterRenderer` | 6 |
| `TestWeaponMechanics` | 29 |
| `TestWeaponSystem` | 18 |
| `TestWeatherSystem` | 8 |
| `TestWindowsCommandLine` | 5 |
| `TestWorkSema` | 7 |
| `TestWorldBasicRender` | 8 |
| `TestWorldOriginSystem` | 12 |
| `TestWorldServerConcurrency` | 3 |
| `TestWorldServerRouting` | 22 |
| `Test_ai-anim_animation` | 3 |
| `Test_ai-anim_navmesh` | 2 |
| `Test_core_hardening` | 5 |
| `Test_ecs_ai_pathfollow` | 4 |
| `Test_ecs_audio_doppler` | 4 |
| `Test_editor_collab_lock_protocol` | 3 |
| `Test_engine-misc_Coroutine` | 2 |
| `Test_engine-misc_EventQueueEviction` | 3 |
| `Test_engine-misc_MobileGestures` | 3 |
| `Test_engine-misc_SnapshotCount` | 2 |
| `Test_gamemodules_mmochat_di` | 1 |
| `Test_gameplay_achievement` | 2 |
| `Test_gameplay_dialogue` | 1 |
| `Test_gameplay_instance` | 2 |
| `Test_gameplay_inventory` | 2 |
| `Test_gameplay_response` | 3 |
| `Test_graphics_rhi` | 6 |
| `Test_lifecycle_ecs_phase_wiring` | 4 |
| `Test_net-world_migration` | 2 |
| `Test_persistence_AsyncDatabaseParams` | 2 |
| `Test_persistence_AsyncDatabasePool` | 3 |
| `Test_persistence_ModSystem` | 2 |
| `Test_persistence_ReplaySystem` | 3 |
| `Test_persistence_SaveSystem` | 10 |
| `Test_scripting_hardening` | 8 |
| `Test_tests_ecsystemordering_real` | 5 |
| `Test_tests_enginecontext_real` | 3 |
| `Test_tests_inversekinematics` | 10 |
| `Test_tooling_CommandParser` | 9 |
| `Test_ui-2d_tween` | 7 |
| `Test_ui-2d_ui` | 3 |
<!-- /AUTO:test_inventory -->
