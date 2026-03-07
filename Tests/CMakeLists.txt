cmake_minimum_required(VERSION 3.16)

# SparkEngine Test Suite
# Uses a lightweight test framework without external dependencies

set(CMAKE_CXX_STANDARD 20)
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
)

target_include_directories(SparkTests PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
    "${CMAKE_SOURCE_DIR}/ThirdParty"
)

# Link with EnTT (header-only)
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/entt")
    target_include_directories(SparkTests PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/entt/single_include")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt")
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
