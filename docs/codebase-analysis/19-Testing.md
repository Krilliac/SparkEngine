# 19 — Testing

**Location:** `Tests/`

Custom lightweight test framework with 146 test files, 745+ test cases, and 11 assertion macros. No external test framework dependency.

---

## Test Framework

**File:** `Tests/TestFramework.h`

### Test Registration

```cpp
TEST(MyTestName) {
    // Test body
    EXPECT_TRUE(condition);
    EXPECT_EQ(actual, expected);
}
```

Macro expands to a function + static registrar that appends to `GetTestRegistry()`.

### Assertion Macros

| Macro | Description |
|-------|-------------|
| `EXPECT_TRUE(expr)` | Boolean assertion |
| `EXPECT_FALSE(expr)` | Negated boolean |
| `EXPECT_EQ(a, b)` | Equality |
| `EXPECT_NE(a, b)` | Inequality |
| `EXPECT_NEAR(a, b, tol)` | Float tolerance |
| `EXPECT_GT(a, b)` | Greater than |
| `EXPECT_LT(a, b)` | Less than |
| `EXPECT_GE(a, b)` | Greater or equal |
| `EXPECT_LE(a, b)` | Less or equal |
| `EXPECT_THROW(expr, type)` | Exception expected |
| `EXPECT_NO_THROW(expr)` | No exception expected |

### Test Output

```
=== SparkEngine Test Suite ===
Running 146 tests...

[ RUN    ] TestMathUtils
[   OK   ] TestMathUtils
[ RUN    ] TestPhysicsSystem
  FAIL: Expected 10.0 == 9.99 (TestPhysicsSystem.cpp:42)
[ FAILED ] TestPhysicsSystem

=== Results ===
Tests:      145 passed, 1 failed, 146 total
Assertions: 3421 passed, 2 failed
```

### Running Tests

```bash
# Via CTest
cd build && ctest --output-on-failure

# Direct execution
./build/bin/SparkTests
```

---

## Test Coverage by Subsystem

### Core & ECS (11 tests)

| Test File | What It Tests |
|-----------|--------------|
| `TestECSWorld.cpp` | Entity creation, component add/remove, queries |
| `TestECSIntegration.cpp` | End-to-end ECS pipeline with multiple systems |
| `TestEngineContext.cpp` | Service locator registration and retrieval |
| `TestEntityArchetype.cpp` | Entity archetype patterns |
| `TestEntityEventBus.cpp` | Entity-specific event distribution |
| `TestEventSystem.cpp` | Global event system publish/subscribe |
| `TestEventBus.cpp` | Event bus thread safety and dispatch |
| `TestGameMode.cpp` | Game mode state management |
| `TestExtendedSystems.cpp` | 38 individual system tests |

### Graphics & Rendering (18 tests)

| Test File | What It Tests |
|-----------|--------------|
| `TestGraphicsEngine.cpp` | Graphics init, state management |
| `TestGraphicsIntegration.cpp` | Multi-subsystem integration |
| `TestRenderGraph.cpp` | DAG compilation and execution |
| `TestLightManager.cpp` | Light culling and tile binning |
| `TestShadowAtlas.cpp` | Shadow tile allocation and eviction |
| `TestSkyAtmosphere.cpp` | Preetham sky model evaluation |
| `TestWaterRenderer.cpp` | Gerstner wave computation |
| `TestTerrainRenderer.cpp` | Terrain LOD and mesh generation |
| `TestHybridRT.cpp` | Software + hardware ray tracing |
| `TestPostProcessingPipeline.cpp` | Post-process effect chain |
| `TestScreenSpaceEffects.cpp` | SSAO, SSR |
| `TestTemporalEffects.cpp` | TAA, reprojection |
| `TestParallelCulling.cpp` | Parallel frustum culling |
| `TestDrawIndirect.cpp` | GPU-driven indirect rendering |
| `TestNullRHIDevice.cpp` | Null RHI for headless mode |
| `TestMaterialEffects.cpp` | Material shader effects |

### Physics (8 tests)

| Test File | What It Tests |
|-----------|--------------|
| `TestPhysicsSystem.cpp` | Jolt world, bodies, constraints |
| `TestPhysicsComponents.cpp` | ECS physics components |
| `TestPhysicsInterpolation.cpp` | State interpolation between ticks |
| `TestCollisionAvoidance.cpp` | AI avoidance steering |
| `TestCollisionLayers.cpp` | Collision layer masks |
| `TestClothSimulation.cpp` | Cloth physics |
| `TestOcclusionCulling.cpp` | Occlusion queries |

### Animation (6 tests)

`TestAnimationSystem.cpp`, `TestAnimationRetargeting.cpp`, `TestAnimationCompression.cpp`, `TestBlendSpace.cpp`, `TestPoseModifier.cpp`

### AI & Pathfinding (6 tests)

`TestAIBehaviorTree.cpp`, `TestNavMesh.cpp`, `TestSteeringBehaviors.cpp`, `TestEnvironmentQuery.cpp`, `TestTacticalPointSystem.cpp`, `TestCoverSystem.cpp`

### Networking (9 tests)

`TestNetworkIntegration.cpp`, `TestNetworkInterpolation.cpp`, `TestNetBuffer.cpp`, `TestNetworkEncryption.cpp`, `TestReliableChannel.cpp`, `TestClientPrediction.cpp`, `TestConnectionTimeout.cpp`, `TestConnectionScope.cpp`, `TestDedicatedServer.cpp`

### Gameplay Systems (15 tests)

`TestInventorySystem.cpp`, `TestQuestSystem.cpp`, `TestWeaponSystem.cpp`, `TestAbilitySystem.cpp`, `TestConditionSystem.cpp`, `TestWeatherSystem.cpp`, `TestDayNightCycle.cpp`, `TestFogSystem.cpp`, `TestTimeOfDaySystem.cpp`, `TestFPSComponents.cpp`, `TestMovementSystem.cpp`, `TestPlayModeManager.cpp`, `TestGroupAI.cpp`, `TestFormationSystem.cpp`, `TestProximityTriggerSystem.cpp`

### Math & Utilities (28 tests)

`TestMathUtils.cpp`, `TestStringUtils.cpp`, `TestFileUtils.cpp`, `TestColorUtils.cpp`, `TestHash.cpp`, `TestUUID.cpp`, `TestResult.cpp`, `TestBitFlags.cpp`, `TestTypeTraits.cpp`, `TestScopeGuard.cpp`, `TestScopedTimer.cpp`, `TestCooldown.cpp`, `TestDeltaSmoother.cpp`, `TestFrameAllocator.cpp`, `TestTransientBufferAllocator.cpp`, `TestLockFreeRingAllocator.cpp`, `TestObjectPool.cpp`, `TestRingBuffer.cpp`, `TestThreadSafeQueue.cpp`, `TestCommandHistory.cpp`, `TestTween.cpp`, `TestCoroutineScheduler.cpp`, `TestSplineMath.cpp`, `TestRandomEngine.cpp`, `TestNoiseGenerator.cpp`, `TestCameraInterpolation.cpp`, `TestConfigParser.cpp`, `TestDebugTools.cpp`

### Graphics Infrastructure (13 tests)

`TestRHIHandlePool.cpp`, `TestConstantBufferDiff.cpp`, `TestGPUPerfCounters.cpp`, `TestInstanceManager.cpp`, `TestMeshLOD.cpp`, `TestFrustumCulling.cpp`, `TestShaderGraphCompiler.cpp`, `TestMaterialDefinition.cpp`, `TestReflection.cpp`, `TestClusteredLightGPU.cpp`, `TestSHLighting.cpp`, `TestPathCache.cpp`, `TestUpscalingSystem.cpp`

### Persistence & Serialization (6 tests)

`TestSaveSystem.cpp`, `TestSceneSnapshotSerializer.cpp`, `TestDatablockRegistry.cpp`, `TestSerializer.cpp`, `TestReplicationFields.cpp`, `TestPerformanceStats.cpp`

### UI, Dialogue & Localization (3 tests)

`TestUISystem.cpp`, `TestDialogueSystem.cpp`, `TestLocalizationSystem.cpp`

### Other (14 tests)

`TestDestructionSystem.cpp`, `TestSequencer.cpp`, `TestChromeTracing.cpp`, `TestFreezeSystem.cpp`, `TestConsoleRBAC.cpp`, `TestDynamicResponseSystem.cpp`, `TestMultiISADispatch.cpp`, `TestAlignedHeapArray.cpp`, `TestInputSystem.cpp`, `TestInputBindings.cpp`, `TestSceneManager.cpp`, `TestLoadingScreen.cpp`, `TestLocalFileCache.cpp`, `TestAsyncDatabase.cpp`

---

## Test Statistics

| Metric | Value |
|--------|-------|
| Test files | 146 |
| Test cases (TEST macros) | 745+ |
| Lines of test code | ~38,114 |
| Assertion types | 11 |
| Subsystems covered | 20+ |

---

## Code Coverage

Generated via GCC `--coverage` + lcov in CI:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="--coverage" -DCMAKE_C_FLAGS="--coverage"
cmake --build build --parallel
cd build && ./bin/SparkTests
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/ThirdParty/*' '*/Tests/*' --output-file coverage.info
```

Coverage artifacts uploaded to CI (14-day retention).
