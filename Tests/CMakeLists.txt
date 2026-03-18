cmake_minimum_required(VERSION 3.25)

# SparkEngine Test Suite
# Uses a lightweight test framework without external dependencies

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Test executable
add_executable(SparkTests
    TestMain.cpp
    TestMathUtils.cpp
    TestObjectPool.cpp
    TestECSWorld.cpp
    TestGameMode.cpp
    TestNoiseGenerator.cpp
    TestSaveSystem.cpp
    TestAIBehaviorTree.cpp
    TestSequencer.cpp
    TestMeshLOD.cpp
    TestInputSystem.cpp
    TestAnimationSystem.cpp
    TestNavMesh.cpp
    TestSteeringBehaviors.cpp
    TestPhysicsComponents.cpp
    TestEventSystem.cpp
    TestWeatherSystem.cpp
    TestInventorySystem.cpp
    TestQuestSystem.cpp
    TestDayNightCycle.cpp
    TestPerformanceStats.cpp
    TestLightManager.cpp
    TestFogSystem.cpp
    TestScreenSpaceEffects.cpp
    TestTemporalEffects.cpp
    TestPostProcessingPipeline.cpp
    TestStringUtils.cpp
    TestRingBuffer.cpp
    TestUUID.cpp
    TestFileUtils.cpp
    TestConfigParser.cpp
    TestFrustumCulling.cpp
    TestCoroutineScheduler.cpp
    TestTween.cpp
    TestCommandHistory.cpp
    TestDebugTools.cpp
    TestResult.cpp
    TestFrameAllocator.cpp
    TestChromeTracing.cpp
    TestLocalFileCache.cpp
    TestEngineContext.cpp
    TestNetBuffer.cpp
    TestSprite2DComponents.cpp
    TestRandomEngine.cpp
    TestCooldown.cpp
    TestColorUtils.cpp
    TestScopedTimer.cpp
    TestBitFlags.cpp
    TestThreadSafeQueue.cpp
    TestDeltaSmoother.cpp
    TestFPSComponents.cpp
    TestWeaponSystem.cpp
    TestNetworkEncryption.cpp
    TestECSIntegration.cpp
    TestUpscalingSystem.cpp
    TestPlayModeManager.cpp
    TestSceneSnapshotSerializer.cpp
    TestEnvironmentQuery.cpp
    TestAnimationRetargeting.cpp
    TestDedicatedServer.cpp
    TestSplineMath.cpp
    TestPhysicsInterpolation.cpp
    TestCameraInterpolation.cpp
    TestNetworkInterpolation.cpp
    TestTypeTraits.cpp
    TestHash.cpp
    TestScopeGuard.cpp
    TestSerializer.cpp
    TestEventBus.cpp
    TestStateMachine.cpp
    TestClientPrediction.cpp
    TestClothSimulation.cpp
    TestDialogueSystem.cpp
    TestInputBindings.cpp
    TestLoadingScreen.cpp
    TestUISystem.cpp
    TestLocalizationSystem.cpp
    TestDestructionSystem.cpp
    TestShadowAtlas.cpp
    TestHybridRT.cpp
    TestReliableChannel.cpp
    TestModuleDependency.cpp
    TestReflection.cpp
    TestPathCache.cpp
    TestShaderGraphCompiler.cpp
    TestAbilitySystem.cpp
    TestConditionSystem.cpp
    TestInstanceManager.cpp
    TestMovementSystem.cpp
    TestSpatialGrid.cpp
    TestReplicationFields.cpp
    TestAsyncDatabase.cpp
    TestScriptHookManager.cpp
    TestModuleHotReload.cpp
)

target_include_directories(SparkTests PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
    "${CMAKE_SOURCE_DIR}/ThirdParty"
)

# Link engine static library so tests can call .cpp-backed implementations
target_link_libraries(SparkTests PRIVATE SparkEngineLib)

# Link with EnTT (header-only)
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt")
    target_include_directories(SparkTests PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include")
endif()

# Cross-platform libraries
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(SparkTests PRIVATE Threads::Threads)
    target_compile_definitions(SparkTests PRIVATE SPARK_PLATFORM_LINUX)
endif()

# Add to CTest
enable_testing()
add_test(NAME SparkEngineTests COMMAND SparkTests)
