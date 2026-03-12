# Testing

SparkEngine includes 35 unit tests using a lightweight internal test framework with CTest integration.

**Source:** `Tests/TestFramework.h`, `Tests/`

## Test Framework

The engine uses its own lightweight test framework (no external test library dependencies):

### Test Macros

```cpp
#include "TestFramework.h"

TEST(MyTestName) {
    EXPECT_EQ(value, expected);        // Equality check
    EXPECT_TRUE(condition);            // Boolean check
    EXPECT_FALSE(condition);           // Negative boolean check
    EXPECT_NEAR(a, b, tolerance);      // Floating-point comparison
    EXPECT_NE(value, unexpected);      // Not-equal check
}
```

## Running Tests

### Build with Tests Enabled

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
```

### Run All Tests

```bash
ctest --test-dir build --output-on-failure
```

### Run Specific Tests

```bash
ctest --test-dir build -R "TestPhysics"           # Run tests matching pattern
ctest --test-dir build -R "TestECS|TestAnimation"  # Run multiple patterns
```

## Test Coverage

The 35 unit tests cover all major subsystems:

| Category | Tests |
|----------|-------|
| **Math** | Math utilities, vector operations |
| **Core** | Object pool, ring buffer |
| **ECS** | Entity creation, component operations, views |
| **Physics** | Body creation, raycasting, collision |
| **AI** | Behavior trees, NavMesh pathfinding |
| **Animation** | State machines, blending, IK |
| **Audio** | Audio source management |
| **Input** | Input state tracking |
| **Scripting** | Script compilation, lifecycle |
| **Save System** | Serialization, compression |
| **Events** | Event bus subscribe/publish |
| **Weather** | Weather transitions |
| **Inventory** | Item management |
| **Quests** | Quest tracking |
| **Day/Night** | Time progression |
| **Lighting** | Light calculations |
| **Performance** | Profiler, frame stats |
| **Fog** | Fog calculations |
| **Screen-Space** | SSAO, SSR parameters |
| **Post-Processing** | Effect pipeline |
| **Sequencer** | Cinematic timeline |
| **Mesh LOD** | LOD switching |
| **Console** | Command parsing |
| **Coroutines** | Coroutine scheduling |
| **Tweening** | Animation tweens |
| **Temporal Effects** | TAA settings |
| **Noise** | Procedural noise generation |
| **Game Modes** | Game mode management |

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
```

2. The test is automatically discovered by CMake (tests are globbed in `Tests/CMakeLists.txt`). See [Build System and CMake Modules](Build-System-and-CMake-Modules) for build configuration.

3. Build and run:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

## CI Integration

Tests run automatically on every push via GitHub Actions:
- Windows (VS 2022, VS 2026) — Debug and Release
- Linux (GCC, Clang) — Debug and Release
- AddressSanitizer + UBSanitizer builds for memory safety
- clang-format check (rejects PRs with formatting violations)
- CodeQL security scanning

### Running Sanitizer Builds Locally

```bash
cmake --preset ci-linux-asan
cmake --build build
cd build && ctest --output-on-failure
```

---

## See Also

- [Build System and CMake Modules](Build-System-and-CMake-Modules) — BUILD_TESTS flag and CI details
- [Getting Started](Getting-Started) — Building the project
- [Contributing](Contributing) — Contribution workflow, pre-commit checks, and adding tests

## Test File Inventory

<!-- AUTO:test_inventory -->
*70 test files, 837+ test cases*

| Test File | Test Cases |
|-----------|------------|
| `TestAIBehaviorTree` | 16 |
| `TestAchievementSystem` | 5 |
| `TestAnimationRetargeting` | 9 |
| `TestAnimationSystem` | 17 |
| `TestBitFlags` | 14 |
| `TestChromeTracing` | 5 |
| `TestClientPrediction` | 5 |
| `TestClothSimulation` | 4 |
| `TestColorUtils` | 18 |
| `TestCommandHistory` | 10 |
| `TestConfigParser` | 16 |
| `TestCooldown` | 14 |
| `TestCoroutineScheduler` | 10 |
| `TestDayNightCycle` | 10 |
| `TestDebugTools` | 31 |
| `TestDeltaSmoother` | 10 |
| `TestDestructionSystem` | 5 |
| `TestDialogueSystem` | 4 |
| `TestECSIntegration` | 9 |
| `TestECSWorld` | 11 |
| `TestEngineContext` | 18 |
| `TestEnvironmentQuery` | 12 |
| `TestEventSystem` | 10 |
| `TestFPSComponents` | 23 |
| `TestFileUtils` | 15 |
| `TestFogSystem` | 17 |
| `TestFrameAllocator` | 8 |
| `TestFrustumCulling` | 11 |
| `TestGameMode` | 5 |
| `TestInputBindings` | 5 |
| `TestInputSystem` | 11 |
| `TestInventorySystem` | 11 |
| `TestLightManager` | 13 |
| `TestLoadingScreen` | 4 |
| `TestLocalFileCache` | 15 |
| `TestLocalizationSystem` | 6 |
| `TestMathUtils` | 11 |
| `TestMeshLOD` | 8 |
| `TestNavMesh` | 11 |
| `TestNetBuffer` | 29 |
| `TestNetworkEncryption` | 17 |
| `TestNoiseGenerator` | 7 |
| `TestObjectPool` | 6 |
| `TestPerformanceStats` | 10 |
| `TestPhysicsComponents` | 22 |
| `TestPlayModeManager` | 33 |
| `TestPostProcessingPipeline` | 11 |
| `TestQuestSystem` | 10 |
| `TestRandomEngine` | 11 |
| `TestReplaySystem` | 4 |
| `TestResult` | 8 |
| `TestRingBuffer` | 14 |
| `TestSaveSystem` | 7 |
| `TestSceneSnapshotSerializer` | 19 |
| `TestScopedTimer` | 3 |
| `TestScreenSpaceEffects` | 16 |
| `TestSequencer` | 10 |
| `TestSplatmapSystem` | 10 |
| `TestSprite2DComponents` | 35 |
| `TestSteeringBehaviors` | 15 |
| `TestStringUtils` | 19 |
| `TestTemporalEffects` | 11 |
| `TestThreadSafeQueue` | 10 |
| `TestTween` | 14 |
| `TestUISystem` | 6 |
| `TestUUID` | 12 |
| `TestUpscalingSystem` | 5 |
| `TestVisualScriptSystem` | 0 |
| `TestWeaponSystem` | 18 |
| `TestWeatherSystem` | 8 |
<!-- /AUTO:test_inventory -->
