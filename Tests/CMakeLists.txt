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
    TestAIStress.cpp
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
    TestDebugHookManager.cpp
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
    TestDedicatedServerRuntime.cpp
    TestServerMockClient.cpp
    TestServerLiveMockClient.cpp
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
    TestLagCompensationIntegration.cpp
    TestModuleDependency.cpp
    TestModuleDiscovery.cpp
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
    TestConsoleRBAC.cpp
    TestNetworkIntegration.cpp
    TestConnectionTimeout.cpp
    TestRecastIntegration.cpp
    TestAudioBackendFactory.cpp
    TestAudioEngine.cpp
    TestSceneManager.cpp
    TestPhysicsSystem.cpp
    TestGraphicsEngine.cpp
    TestRenderGraph.cpp
    TestTerrainRenderer.cpp
    TestMaterialEffects.cpp
    TestTacticalPointSystem.cpp
    TestCoverSystem.cpp
    TestProximityTriggerSystem.cpp
    TestFormationSystem.cpp
    TestGroupAI.cpp
    TestCollisionAvoidance.cpp
    TestDynamicResponseSystem.cpp
    TestEntityArchetype.cpp
    TestSkyAtmosphere.cpp
    TestWaterRenderer.cpp
    TestOcclusionCulling.cpp
    TestPortalCulling.cpp
    TestPoseModifier.cpp
    TestAnimationCompression.cpp
    TestBlendSpace.cpp
    TestExtendedSystems.cpp
    TestFreezeSystem.cpp
    TestNullRHIDevice.cpp
    TestGraphicsInitFallback.cpp
    TestSoftwareRendering.cpp
    TestRenderCommandRing.cpp
    TestMultiISADispatch.cpp
    TestConstantBufferDiff.cpp
    TestDirtyRectTracker.cpp
    TestLockFreeRingAllocator.cpp
    TestSceneConfigDatabase.cpp
    TestGPUPerfCounters.cpp
    TestWorkSema.cpp
    TestAlignedHeapArray.cpp
    TestDeferredDeletion.cpp
    TestCollisionLayers.cpp
    TestTransientBufferAllocator.cpp
    TestDrawIndirect.cpp
    TestParallelCulling.cpp
    TestConnectionScope.cpp
    TestConnectionScopeWiring.cpp
    TestDatablockRegistry.cpp
    TestEntityEventBus.cpp
    TestSHLighting.cpp
    TestClusteredLightGPU.cpp
    TestRHIHandlePool.cpp
    TestMaterialDefinition.cpp
    TestCollaborativeEditing.cpp
    TestGraphicsIntegration.cpp
    TestTimeOfDaySystem.cpp
    TestContainerUtils.cpp
    TestBitUtils.cpp
    TestAngleUtils.cpp
    TestDeferredQueue.cpp
    TestVersionedHandle.cpp
    TestStringPool.cpp
    TestDelegate.cpp
    TestScheduledCallback.cpp
    TestFixtures.cpp
    TestJsonUtils.cpp
    TestThirdPartyIntegration.cpp
    TestNetworkStress.cpp
    TestNetworkMMOIntegration.cpp
    TestAnimationStress.cpp
    TestECSStress.cpp
    TestPhysicsStress.cpp
    TestAIStress.cpp
    TestGraphicsStress.cpp
    TestGameplayStress.cpp
    TestUtilsStress.cpp
    TestDialogueStress.cpp
    TestAdversarialEngine.cpp
    TestEngineDiagnostics.cpp
    TestEngineMonitor.cpp
    TestFullEngineDiagnostics.cpp
    TestEngineLoadTest.cpp
    TestDebugUtilities.cpp
    TestMemoryDebugger.cpp
    TestMemoryMonitor.cpp
    TestFreezeDetector.cpp
    TestDeadlockDetector.cpp
    TestScriptSandbox.cpp
    TestPacketValidator.cpp
    TestLogger.cpp
    TestProfiler.cpp
    TestStackTrace.cpp
    TestJobSystem.cpp
    TestOpaqueHandle.cpp
    TestSparkError.cpp
    TestThreadDebugger.cpp
    TestCollisionSystem.cpp
    TestNetworkSecurity.cpp
    TestSubTickInput.cpp
    TestBehaviorTreeNodes.cpp
    TestAIBudgetLimiter.cpp
    TestAIDirector.cpp
    TestWorldOriginSystem.cpp
    TestModSystem.cpp
    TestReplaySystem.cpp
    TestSceneGraph2D.cpp
    TestDecalSystem.cpp
    TestFoliageSystem.cpp
    TestFoliageRenderer.cpp
    TestFoliageImpostorBaker.cpp
    TestGTAOEffect.cpp
    TestSSAOTemporalFilter.cpp
    TestPostProcessingPipelinePhaseJ.cpp
    TestVolumeManager.cpp
    TestPostProcessingPipelinePhaseK.cpp
    TestMeshOptimizer.cpp
    TestBVHAccelerator.cpp
    TestReflectionProbeCache.cpp
    TestCachedShadowAtlas.cpp
    TestRTHandleSystem.cpp
    TestConstantBufferRing.cpp
    TestPostProcessingPipelinePhaseN.cpp
    TestShaderVariantSystem.cpp
    TestPersistentMaterialCB.cpp
    TestDenoiserInterface.cpp
    TestUICompositor.cpp
    TestUISystemPhaseR.cpp
    TestFastNoise2SIMD.cpp
    TestVoxelConeTracing.cpp
    TestDXRSupport.cpp
    TestCSGEditorPanel.cpp
    TestNavMeshLink.cpp
    TestGameplaySystemExtension.cpp
    TestSpringArm.cpp
    TestConnectionScopeFilter.cpp
    TestNavMeshObstacles.cpp
    TestFaultIsolation.cpp
    TestSelfRecovery.cpp
    TestVulkanLavapipe.cpp
    TestWARPRendering.cpp
    # Hardware acceleration tests
    TestGPUParticleSystem.cpp
    TestGPUSkinning.cpp
    TestAsyncComputeScheduler.cpp
    TestGPUDrivenRenderer.cpp
    TestGPUClusterCulling.cpp
    TestDirectStorageLoader.cpp
    TestMeshShaderPipeline.cpp
    TestSparkPak.cpp
    TestNetworkManagerOrchestration.cpp
    TestNetworkManagerEdgeCases.cpp
    TestECSystemOrdering.cpp
    TestECSystemSpecialized.cpp
    TestMaterialSystemValidation.cpp
    TestMaterialSystemEdgeCases.cpp
    TestAssetPipelineCache.cpp
    TestEngineSettingsParser.cpp
    TestEngineSettingsEdgeCases.cpp
    TestPerceptionSystemMath.cpp
    TestInputManagerState.cpp
    TestGamepadInputProcessing.cpp
    TestCameraTransforms.cpp
    TestWorldServerRouting.cpp
    TestAudioMixerBus.cpp
    TestConsoleVariables.cpp
    TestWeaponMechanics.cpp
    TestGameObjectTransforms.cpp
    TestSceneSerializer.cpp
    TestMusicManager.cpp
    TestAngelScriptEngine.cpp
    TestScriptHotReload.cpp
    TestVRSystem.cpp
    TestSeamlessAreaManager.cpp
    TestAreaAssetLoader.cpp
    TestEventResponseSystem.cpp
    TestVisualScriptCompiler.cpp
    TestEntityPresetManager.cpp
    TestGamePackager.cpp
    TestAssetValidator.cpp
    TestAssetMigration.cpp
    TestAccessibility.cpp
    TestPlatformInput.cpp
    TestTelemetry.cpp
    TestBenchmarkFramework.cpp
    TestUILayoutExtensions.cpp
    TestAchievementSystem.cpp
    TestShaderHotReload.cpp
    TestRuntimePrefab.cpp
    TestTutorialSystem.cpp
    TestGoldenImageTest.cpp
    TestAnimNotify.cpp
    TestGameplayTags.cpp
    TestGameplayDebugger.cpp
    TestScreenCapture.cpp
    TestProceduralGenerator.cpp
    TestVideoPlayer.cpp
    TestLightmapBaker.cpp
    # Cross-system integration tests
    TestPhysicsECSIntegration.cpp
    TestSceneRoundtrip.cpp
    TestAnimationPhysicsIntegration.cpp
    TestNetworkReplicationIntegration.cpp
    TestRenderECSIntegration.cpp
    Integration/TestSubsystemIntegrationScenarios.cpp
    TestFPSMultiplayer.cpp
    # Asset pipeline and GPU profiling
    TestFBXImporter.cpp
    TestFBXImportValidation.cpp
    TestTextureCompressor.cpp
    TestNeuralInference.cpp
    TestCpuNeuralInference.cpp
    TestNeuralTextureCompressor.cpp
    TestNeuralRadianceCache.cpp
    TestNeuralPostProcessing.cpp
    TestGPUProfiler.cpp
    TestShaderHotReloadCompilation.cpp
    TestShaderHotReloadPhaseU.cpp
    TestShaderDiskCachePhaseV.cpp
    TestShaderCrossCompilerPhaseW.cpp
    TestRHIHandlePoolPhaseX.cpp
    TestTransientBufferAllocatorPhaseX.cpp
    TestNullRHIDevicePhaseY.cpp
    TestScriptHookManagerPhaseBB.cpp
    TestDynamicQualityScalerPhaseBB.cpp
    TestGPUStallProfilerPhaseCC.cpp
    TestAsyncComputeSchedulerPhaseCC.cpp
    TestAIDebugRendererPhaseDD.cpp
    TestDirtyRegionGridPhaseDD.cpp
    TestEventResponseSystemPhaseEE.cpp
    TestEntityPresetManagerPhaseEE.cpp
    TestAssetMigrationPhaseEE.cpp
    # Editor, gameplay, LOD, AI debug
    TestEditorCommands.cpp
    TestEditorWindowManager.cpp
    TestGameplayExtensionRegistry.cpp
    TestLODGenerator.cpp
    TestAIDebugRenderer.cpp
    TestAssertSuppression.cpp
    TestCrossSystemIntegration.cpp
    TestUndoRedoManager.cpp
    TestPrefabManager.cpp
    TestEditorLayoutManager.cpp
    TestGizmoMath.cpp
    # Asset database and game module tests
    TestAssetDatabase.cpp
    TestSparkGameRPG.cpp
    TestSparkGameARPG.cpp
    TestSparkGameRTS.cpp
    TestSparkGameRacing.cpp
    TestSparkGamePlatformer.cpp
    # Crash reporting
    TestCrashReportUploader.cpp
    # Security
    TestMemoryIntegrity.cpp
    # Online Services, DataTable, MovieRender, Selection, RemoteDebug, HLOD, Crafting
    TestOnlineServices.cpp
    TestDataTableSystem.cpp
    TestMovieRenderPipeline.cpp
    TestSelectionManager.cpp
    TestRemoteDebugSystem.cpp
    TestHLODSystem.cpp
    TestLootAndCrafting.cpp
    TestAssetDependencyGraph.cpp
    TestEditorAutomation.cpp
    TestFileWatcher.cpp
    TestTimerManager.cpp
    TestInGameConsole.cpp
    TestNetworkDebugPanel.cpp
    TestContracts.cpp
    TestSafetyCoreUtils.cpp
    # Subprocess spawning and piping
    TestProcess.cpp
    # Comprehensive subsystem coverage
    TestGraphicsSubsystems.cpp
    TestEditorSubsystems.cpp
    TestCoreAndBuildSystems.cpp
    TestOpenWorldModule.cpp
    TestGameModuleRTS.cpp
    TestGameModuleMMO.cpp
    TestGameModuleRPG.cpp
    TestGameModuleRacing.cpp
    TestGameModulePlatformerARPG.cpp
    # Editor core sources (no ImGui dependency)
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorLogger.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorCrashHandler.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorLayoutManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/ProjectManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorPluginManager.cpp"
    # Editor sources needed by tests
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeEditSession.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/LiveEditBridge.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Workflow/EditorWorkflow.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prototyping/PrototypingSystem.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/UIDesigner/UIDesignerSystem.cpp"
)

target_include_directories(SparkTests PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source"
    "${CMAKE_SOURCE_DIR}/ThirdParty"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source"
)

# Link engine static library so tests can call .cpp-backed implementations
target_link_libraries(SparkTests PRIVATE SparkEngineLib)

# Editor version defines needed by ProjectManager.cpp
target_compile_definitions(SparkTests PRIVATE
    EDITOR_VERSION_MAJOR=1
    EDITOR_VERSION_MINOR=0
    EDITOR_VERSION_PATCH=0
)

# Link with EnTT (header-only)
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt")
    target_include_directories(SparkTests PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include")
endif()

# Cross-platform libraries
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(SparkTests PRIVATE Threads::Threads)
    target_compile_definitions(SparkTests PRIVATE SPARK_PLATFORM_LINUX)

    # Export symbols so backtrace_symbols() can resolve function names in stack traces
    set_target_properties(SparkTests PROPERTIES ENABLE_EXPORTS ON)
endif()

# =============================================================================
# Feature-gated test support
# When optional dependencies (ImGui, SDL2, Vulkan, etc.) are available, extra
# tests activate automatically. Tests use #ifdef SPARK_TEST_HAS_* guards.
# =============================================================================

# ImGui — enables full editor tests, OpenWorld module tests, CommandPalette, etc.
if(TARGET imgui)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_IMGUI)
    target_link_libraries(SparkTests PRIVATE imgui)

    # OpenWorld module sources (all use ImGui for RenderDebugUI)
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Player/OWPlayerSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Exploration/OWExplorationSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Gathering/OWGatheringSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Wildlife/OWWildlifeSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Events/OWDynamicEventSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Settlement/OWSettlementSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/World/OWWorldSetup.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Core/OWEngineSystems.cpp"
    )

    # RTS module sources (all use ImGui for RenderDebugUI)
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Core/RTSEngineSystems.cpp"
    )

    # MMO module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Guild/MMOGuildSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Core/MMOEngineSystems.cpp"
    )

    # RPG module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Character/RPGCharacterSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Combat/RPGCombatSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Quest/RPGQuestSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Core/RPGEngineSystems.cpp"
    )

    # Racing module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Vehicle/RacingVehicleSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Track/RacingTrackSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Race/RacingRaceManager.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Core/RacingEngineSystems.cpp"
    )

    # Platformer module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Player/PlatformerPlayerController.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Checkpoint/PlatformerCheckpointSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Core/PlatformerEngineSystems.cpp"
    )

    # ARPG module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Hero/ARPGHeroSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Combat/ARPGCombatSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Loot/ARPGLootSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Dungeon/ARPGDungeonSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Core/ARPGEngineSystems.cpp"
    )

    # Editor sources that require ImGui
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorTheme.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Search/CommandPalette.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/LevelStreaming/LevelStreamingSystem.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/VersionControl/VersionControlSystem.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/VersionControl/VersionControlConflicts.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/VersionControl/VersionControlGitOps.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/VersionControl/VersionControlHelpers.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/VersionControl/VersionControlRender.cpp"
        # Phase AA Theme 3C: tests for LevelStreamingSystem and
        # VersionControlSystem. Gated behind the ImGui-available block
        # because both classes are EditorPanel subclasses whose .cpp
        # files require ImGui to link.
        "${CMAKE_SOURCE_DIR}/Tests/TestLevelStreamingSystemPhaseAA.cpp"
        "${CMAKE_SOURCE_DIR}/Tests/TestVersionControlSystemPhaseAA.cpp"
    )

    message(STATUS "[Tests] ImGui available — gated editor + 6 game module tests enabled")
else()
    message(STATUS "[Tests] ImGui not available — gated editor & OpenWorld tests disabled")
endif()

# Propagate engine feature defines so tests can gate on them
if(ENABLE_NETWORKING)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_NETWORKING)
endif()
if(SPARK_JOLT_PHYSICS_AVAILABLE OR TARGET Jolt)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_PHYSICS)
endif()
if(SPARK_VULKAN_SUPPORT OR ENABLE_VULKAN)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_VULKAN)
endif()
if(SPARK_OPENGL_SUPPORT OR ENABLE_OPENGL)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_OPENGL)
endif()
if(ENABLE_MOBILE)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_MOBILE)
endif()

# Add to CTest
enable_testing()
add_test(NAME SparkEngineTests COMMAND SparkTests)
