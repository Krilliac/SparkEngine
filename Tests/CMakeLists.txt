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
    TestReflectedScene.cpp
    TestReflectedSceneEmissiveHierarchy.cpp
    TestGameMode.cpp
    TestNoiseGenerator.cpp
    TestSaveSystem.cpp
    TestTFFixedStep.cpp
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
    TestWindowsCommandLine.cpp
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
    TestNetQuantize.cpp
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
    TestPasswordHash.cpp
    TestMMOCredentialSecurity.cpp
    TestSecureRandom.cpp
    TestSparkConsoleConcurrency.cpp
    TestScopeGuard.cpp
    TestSerializer.cpp
    TestEventBus.cpp
    TestStateMachine.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.cpp"
    TestClientPrediction.cpp
    TestClothSimulation.cpp
    TestDialogueSystem.cpp
    TestInputBindings.cpp
    TestLoadingScreen.cpp
    TestStartupSplash.cpp
    TestUISystem.cpp
    TestLocalizationSystem.cpp
    TestDestructionSystem.cpp
    TestShadowAtlas.cpp
    TestHybridRT.cpp
    TestReliableChannel.cpp
    TestLagCompensationIntegration.cpp
    TestLagCompensation.cpp
    TestAreaSimulationHook.cpp
    TestModuleDependency.cpp
    TestModuleDiscovery.cpp
    TestModuleABI.cpp
    TestPluginABI.cpp
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
    TestAsyncDatabaseRegressions.cpp
    TestScriptHookManager.cpp
    TestModuleHotReload.cpp
    TestConsoleRBAC.cpp
    TestNetworkIntegration.cpp
    TestNetworkStack.cpp
    TestConnectionTimeout.cpp
    TestRecastIntegration.cpp
    TestAudioBackendFactory.cpp
    TestAudioEngine.cpp
    TestSceneManager.cpp
    TestPhysicsSystem.cpp
    TestPhysicsTeardownGuard.cpp
    TestGraphicsEngine.cpp
    TestProcessDrawListLinux.cpp
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
    TestVolumetricClouds.cpp
    TestHRTFProcessor.cpp
    TestDirectionalStreaming.cpp
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
    TestJsonStrict.cpp
    TestThirdPartyIntegration.cpp
    TestGLTFStaticMeshLoader.cpp
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
    TestPacketValidatorReal.cpp
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
    TestAIIntegratedSystem.cpp
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
    TestEngineSettingsReal.cpp
    TestECSystemsReal.cpp
    TestNetworkManagerReal.cpp
    TestMaterialSystemReal.cpp
    TestAssetPipelineReal.cpp
    TestAbilitySystemReal.cpp
    TestAchievementSystemReal.cpp
    TestConstantBufferDiffReal.cpp
    TestContainerUtilsReal.cpp
    TestDirtyRectTrackerReal.cpp
    TestSceneConfigDatabaseReal.cpp
    TestSpringArmReal.cpp
    TestPerceptionSystemMath.cpp
    TestInputManagerState.cpp
    TestGamepadInputProcessing.cpp
    TestCameraTransforms.cpp
    TestSparkEngineCameraOwnership.cpp
    TestWorldServerRouting.cpp
    TestWorldServerConcurrency.cpp
    TestAudioMixerBus.cpp
    TestConsoleVariables.cpp
    TestWeaponMechanics.cpp
    TestGameObjectTransforms.cpp
    TestSceneSerializer.cpp
    TestSceneSerializerReal.cpp
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
    TestCpuNeuralTraining.cpp
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
    TestTelemetryPhaseFF.cpp
    TestCacheDebuggerPhaseFF.cpp
    TestMathUtilsExtendedPhaseGG.cpp
    TestCpuDebuggerPhaseGG.cpp
    TestLODGeneratorPhaseGG.cpp
    TestTextureCompressorPhaseGG.cpp
    TestDatablockRegistryPhaseHH.cpp
    TestNetworkSecurityPhaseHH.cpp
    TestAIDirectorPhaseII.cpp
    TestHLODBuilderPhaseII.cpp
    TestAngleUtilsReal.cpp
    TestBitUtilsReal.cpp
    TestBitFlagsReal.cpp
    TestTweenReal.cpp
    TestSplineMathReal.cpp
    TestSpatialGridReal.cpp
    TestSteeringBehaviorsReal.cpp
    TestCoverSystemReal.cpp
    TestAlignedHeapArrayReal.cpp
    TestAtomicSharedPtrReal.cpp
    TestDeferredDeletionReal.cpp
    TestPerformanceStatsReal.cpp
    TestBlendSpaceReal.cpp
    TestSequencerReal.cpp
    TestSequencerAudioWiring.cpp
    TestFaultIsolationReal.cpp
    TestColorUtilsReal.cpp
    TestStateMachineReal.cpp
    TestEventBusReal.cpp
    TestCompressionUtilsReal.cpp
    TestEntityEventBusReal.cpp
    TestConfigParserReal.cpp
    TestReflectionReal.cpp
    TestGameplayTagsReal.cpp
    TestInventorySystemReal.cpp
    TestLoadingScreenReal.cpp
    TestDelegateReal.cpp
    TestRingBufferReal.cpp
    TestAnimationCompressionReal.cpp
    TestStringUtilsReal.cpp
    TestTimerManagerReal.cpp
    TestObjectPoolReal.cpp
    TestHashReal.cpp
    TestCooldownReal.cpp
    TestDeltaSmootherReal.cpp
    TestTimerReal.cpp
    TestFileUtilsReal.cpp
    TestAIDebugRendererReal.cpp
    TestAssertSuppressionReal.cpp
    TestLockFreeRingAllocatorReal.cpp
    TestGameModeReal.cpp
    # Editor, gameplay, LOD, AI debug
    TestEditorCommands.cpp
    TestEditorDocumentTransition.cpp
    TestEditorWindowManager.cpp
    TestGameplayExtensionRegistry.cpp
    TestLODGenerator.cpp
    TestAIDebugRenderer.cpp
    TestAssertSuppression.cpp
    TestCrossSystemIntegration.cpp
    TestUndoRedoManager.cpp
    TestUndoRedoManagerProduction.cpp
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
    # Critical-path integration tests
    TestRHIBridgeIntegration.cpp
    TestNetworkManagerIntegration.cpp
    TestSystemManagerIntegration.cpp
    TestAssetPipelineIntegration.cpp
    TestMaterialSystemIntegration.cpp
    TestEngineLifecycle.cpp
    TestFPSGameplayIntegration.cpp
    TestGLSLPipelineIntegration.cpp
    TestHResultPlatform.cpp
    TestEngineBootPlatforms.cpp
    TestMacOSPlatform.cpp
    # Subprocess spawning and piping
    TestProcess.cpp
    # SparkDaemon foundation (Phase 1)
    TestDaemonProtocol.cpp
    TestDaemonFoundation.cpp
    "${CMAKE_SOURCE_DIR}/SparkDaemon/src/DaemonServer.cpp"
    "${CMAKE_SOURCE_DIR}/SparkDaemon/src/ControlService.cpp"
    # SparkDaemon Shader service (Phase 2a)
    TestShaderServiceClient.cpp
    "${CMAKE_SOURCE_DIR}/SparkDaemon/src/ShaderService.cpp"
    # SparkDaemon Asset service (Phase 3a)
    TestAssetServiceClient.cpp
    "${CMAKE_SOURCE_DIR}/SparkDaemon/src/AssetService.cpp"
    # Engine-side daemon wiring (Phase 3b)
    TestShaderDiskCacheDaemon.cpp
    # Engine-level daemon lifecycle (Phase 3c)
    TestDaemonLifecycle.cpp
    # Concurrent clients + StatsRequest (Phase 4)
    TestDaemonConcurrent.cpp
    # LRU cache eviction (Phase 5)
    TestDaemonLRU.cpp
    # Codex PR #478 review fixes (thread reap, fatal error, strict filename parsing)
    TestDaemonCodexFixes.cpp
    # Stats formatter used by the daemon.stats console command (Phase 6)
    TestDaemonDiagnostics.cpp
    # Comprehensive subsystem coverage
    TestGraphicsSubsystems.cpp
    TestEditorSubsystems.cpp
    TestDedicatedServerProcessController.cpp
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/DedicatedServerProcessController.cpp"
    TestServiceTopologyController.cpp
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/ServiceTopologyController.cpp"
    TestCoreAndBuildSystems.cpp
    TestOpenWorldModule.cpp
    TestGameModuleRTS.cpp
    TestGameModuleMMO.cpp
    TestGameModuleRPG.cpp
    TestGameModuleRacing.cpp
    TestGameModulePlatformerARPG.cpp
    # TERRAFRONT (SparkGameMMOFPS) — header-only wire protocol, standalone
    # rule math, and validation of the real Assets/MMOFPS/Data tables.
    # Module .cpp sources are NOT compiled into SparkTests by design.
    TestTFNetProtocolLayout.cpp
    TestTFDataTables.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Data/TFDataTablesParseWorld.cpp"
    TestTFDamageModel.cpp
    TestTFRegionLattice.cpp
    TestRegionMapDataSource.cpp
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/RegionMapDataSource.cpp"
    TestTFChatRules.cpp
    # W7/W8 rule math + wire freezes (header-only includes / standalone mirrors)
    TestTFRedeployRules.cpp
    TestTFCaptureMath.cpp
    # W9 class-abilities wire freeze + shared jet-thrust math (header-only)
    TestTFAbilityWire.cpp
    # W11 death-recap wire freeze + damage-log ring semantics (header-only)
    TestTFDeathRecapWire.cpp
    # W5 onboarding (TFDatabase Task 1) — module .cpp added explicitly below
    # since SparkTests does not compile module sources by default.
    TestTFOnboarding.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFPlayerMeta.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFAccountSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFCharacterSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFCrypto.cpp"
    # W7/W8 outfit persistence + secondary-motion chain math — both module
    # .cpps are deliberately minimal-dependency and link standalone into
    # SparkTests (the TFDatabase.cpp pattern above).
    TestTFOutfitStore.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFOutfitStore.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFOutfitStoreDisk.cpp"
    TestTFSocialStore.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Game/TFSocialSystemStore.cpp"
    TestTFSecondaryMotion.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Game/TFSecondaryMotion.cpp"
    # W13 anti-cheat lane (TFServerValidation) -- movement-clamp, fire-origin,
    # and input-rate token-bucket math; pure logic (Core/TFTypes.h +
    # Utils/LogMacros.h only), same minimal-dependency pattern as the two
    # module .cpps immediately above.
    TestTFServerValidation.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Game/TFServerValidation.cpp"
    # Compile-time validation that shipped project templates are real C++
    TestTemplatesCompile.cpp
    # Packaged runtimes must resolve content when launched from a foreign CWD.
    TestRuntimePackage.cpp
    # GraphicsEngine device-attach init + headless WorldBasicRenderer smoke is
    # Windows-only and is added below.
    # FPS game module source needed by TestGameModeReal
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/GameMode.cpp"
    # Editor core sources (no ImGui dependency)
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorLogger.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorCrashHandler.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorLayoutManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorWindowManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/ProjectManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/BuildPipeline.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/BuildPipelineProcess.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Utils/EditorProcessLaunch.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Utils/EditorProcessLaunchWin32.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Utils/EditorProcessLaunchPosix.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorPluginManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/UndoRedo/UndoRedoManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/SceneFile.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/SceneComponentCodec.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/SceneSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/BinarySceneSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/JSONSceneSerializer.cpp"
    # Editor sources needed by tests
    TestEngineInterfaceProtocol.cpp
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/EngineInterface.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeEditSession.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/LiveEditBridge.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Workflow/EditorWorkflow.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prototyping/PrototypingSystem.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/UIDesigner/UIDesignerSystem.cpp"
)

# Standalone process used to exercise CRT destruction order without the test
# runner's top-level crash handler masking a teardown access violation.
add_executable(SparkDebugHookTeardownProbe
    Teardown/DebugHookTeardownProbe.cpp)
target_compile_features(SparkDebugHookTeardownProbe PRIVATE cxx_std_23)
target_link_libraries(SparkDebugHookTeardownProbe PRIVATE SparkEngineLib)
add_dependencies(SparkTests SparkDebugHookTeardownProbe)
target_compile_definitions(SparkTests PRIVATE
    SPARK_TEST_DEBUG_HOOK_TEARDOWN_PROBE_PATH="$<TARGET_FILE:SparkDebugHookTeardownProbe>")

# Real shared-library fixtures for the pre-instantiation module ABI gate.
# They intentionally do not link SparkEngineLib: the boundary under test must
# be satisfied by SDK headers and the exported C ABI alone.
add_library(SparkMismatchedModuleFixture SHARED
    Fixtures/ModuleABI/MismatchedModule.cpp)
add_library(SparkCompatibleModuleFixture SHARED
    Fixtures/ModuleABI/CompatibleModule.cpp)
add_library(SparkModuleSiblingDependency SHARED
    Fixtures/ModuleABI/SiblingDependency.cpp)
add_library(SparkSiblingDependentModuleFixture SHARED
    Fixtures/ModuleABI/SiblingDependentModule.cpp)

foreach(_module_fixture IN ITEMS SparkMismatchedModuleFixture SparkCompatibleModuleFixture)
    target_compile_features(${_module_fixture} PRIVATE cxx_std_23)
    target_compile_definitions(${_module_fixture} PRIVATE SPARK_MODULE_DLL)
    target_include_directories(${_module_fixture} PRIVATE
        "${CMAKE_SOURCE_DIR}/SparkSDK/Include"
        "${SPARK_GENERATED_INCLUDE_DIR}")
    if(MSVC)
        set_property(TARGET ${_module_fixture} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endforeach()

target_compile_features(SparkModuleSiblingDependency PRIVATE cxx_std_23)
target_compile_features(SparkSiblingDependentModuleFixture PRIVATE cxx_std_23)
target_compile_definitions(SparkSiblingDependentModuleFixture PRIVATE SPARK_MODULE_DLL)
target_include_directories(SparkSiblingDependentModuleFixture PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkSDK/Include"
    "${SPARK_GENERATED_INCLUDE_DIR}")
target_link_libraries(SparkSiblingDependentModuleFixture PRIVATE SparkModuleSiblingDependency)
if(MSVC)
    set_property(TARGET SparkSiblingDependentModuleFixture PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(APPLE)
    set(_spark_fixture_origin "@loader_path")
elseif(UNIX)
    set(_spark_fixture_origin "$ORIGIN")
endif()
if(DEFINED _spark_fixture_origin)
    set_target_properties(SparkSiblingDependentModuleFixture PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "${_spark_fixture_origin}")
endif()

get_property(SPARK_TEST_CURRENT_SDK_VERSION GLOBAL PROPERTY SPARK_MODULE_CURRENT_SDK_VERSION)
if(NOT SPARK_TEST_CURRENT_SDK_VERSION)
    message(FATAL_ERROR "SparkTests: module SDK version global property is not initialized")
endif()
math(EXPR SPARK_TEST_MISMATCHED_SDK_VERSION
    "${SPARK_TEST_CURRENT_SDK_VERSION} + 1")
spark_configure_module_abi(SparkMismatchedModuleFixture
    SDK_VERSION "${SPARK_TEST_MISMATCHED_SDK_VERSION}")
spark_configure_module_abi(SparkCompatibleModuleFixture)
spark_configure_module_abi(SparkSiblingDependentModuleFixture)

add_dependencies(SparkTests SparkMismatchedModuleFixture SparkCompatibleModuleFixture
    SparkSiblingDependentModuleFixture)
target_compile_definitions(SparkTests PRIVATE
    SPARK_TEST_MISMATCHED_MODULE_PATH="$<TARGET_FILE:SparkMismatchedModuleFixture>"
    SPARK_TEST_COMPATIBLE_MODULE_PATH="$<TARGET_FILE:SparkCompatibleModuleFixture>"
    SPARK_TEST_SIBLING_DEPENDENT_MODULE_PATH="$<TARGET_FILE:SparkSiblingDependentModuleFixture>")

# Objective-C++ test sources — only compile when the Metal backend is
# actually available. SPARK_METAL_AVAILABLE is set by the root CMakeLists
# when ENABLE_METAL AND APPLE is true and enable_language(OBJCXX) has run;
# adding a .mm source unconditionally would fail CMake configure on the
# Linux CI row that doesn't register OBJCXX.
if(APPLE AND SPARK_METAL_AVAILABLE)
    target_sources(SparkTests PRIVATE
        TestMetalRayTracing.cpp
        TestMetalRayTracingLive.mm)
    # Match the engine's Metal linker flags — Foundation + Metal frameworks
    # are needed for MTLCreateSystemDefaultDevice and MTLDevice protocols.
    target_link_libraries(SparkTests PRIVATE "-framework Foundation" "-framework Metal")
endif()

target_include_directories(SparkTests PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source"
    "${CMAKE_SOURCE_DIR}/SparkDaemon/src"
    "${CMAKE_SOURCE_DIR}/ThirdParty"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source"
    # TERRAFRONT wire-protocol headers (TestTFNetProtocolLayout includes
    # "Net/TFNetProtocol.h" / "Net/TFRepProtocol.h"; header-only PODs)
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source"
)

# Link engine static library so tests can call .cpp-backed implementations
target_link_libraries(SparkTests PRIVATE SparkEngineLib)
if(TARGET miniz)
    target_link_libraries(SparkTests PRIVATE miniz)
endif()

# SparkBuild command generation is part of the supported first-party build
# surface. Keep these tests conditional so minimal engine configurations can
# still disable the tool entirely.
if(TARGET SparkBuildCore)
    target_sources(SparkTests PRIVATE TestSparkBuildConfig.cpp)
    target_link_libraries(SparkTests PRIVATE SparkBuildCore)
endif()

# Editor version defines needed by ProjectManager.cpp
target_compile_definitions(SparkTests PRIVATE
    SPARK_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    EDITOR_VERSION_MAJOR=1
    EDITOR_VERSION_MINOR=0
    EDITOR_VERSION_PATCH=0
    SPARK_SOCIAL_STORE_TESTS
)

# Link with EnTT (header-only)
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt")
    target_include_directories(SparkTests PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include")
endif()

# Cross-platform libraries
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(SparkTests PRIVATE Threads::Threads)
    if(APPLE)
        target_compile_definitions(SparkTests PRIVATE SPARK_PLATFORM_MACOS)
    else()
        target_compile_definitions(SparkTests PRIVATE SPARK_PLATFORM_LINUX)
    endif()

    # Export symbols so backtrace_symbols() can resolve function names in stack traces
    set_target_properties(SparkTests PROPERTIES ENABLE_EXPORTS ON)
endif()

if(WIN32)
    target_sources(SparkTests PRIVATE TestWorldBasicRender.cpp)
    target_sources(SparkTests PRIVATE TestPostProcessingPipelineD3D11.cpp)
    target_sources(SparkTests PRIVATE TestGPUDrivenRendererD3D11.cpp)
    target_compile_definitions(SparkTests PRIVATE
        SPARK_TEST_SOURCE_DIR_WIDE=L"${CMAKE_SOURCE_DIR}")
endif()

# =============================================================================
# Previously-authored but unregistered tests (found by the 2026-07-13
# test-health sweep): substantial, non-stub test files that existed in Tests/
# but were never added to this target, so they silently never ran in CI.
# Several used a nonexistent SPARK_TEST/SPARK_TEST_ASSERT macro convention or
# a Catch2-style two-arg TEST("name","[tag]") signature that don't match this
# project's TestFramework.h -- fixed to TEST(name)/EXPECT_TRUE before adding.
#
# TestAutoLODPerformance.cpp is included again now that LODGenerator::Simplify
# no longer rescans every triangle on each edge collapse (it was accidentally
# O(collapses * triCount) and never finished on the 540k-vert scene this test
# builds -- which is almost certainly why the file was left unregistered).
# =============================================================================
target_sources(SparkTests PRIVATE
    TestAssetStallDetector.cpp
    TestAtomicSharedPtr.cpp
    TestAutoLODPerformance.cpp
    TestCSGSystem.cpp
    TestCompressionUtils.cpp
    TestDescriptorCache.cpp
    TestFontSystem.cpp
    TestGPUResourceLeakDetector.cpp
    TestGPUStallProfiler.cpp
    TestHitchDetector.cpp
    TestInputActionSystem.cpp
    TestNetworkHealthMonitor.cpp
    TestRHICapabilityParity.cpp
    TestShaderDiskCache.cpp
    TestSubsystemConsoleCommands.cpp
    TestEcsCameraConsole.cpp
    TestTextureZombiePool.cpp
)

# =============================================================================
# Hardening regression tests (claude/harden-fleet) — shared TestMain runner
# =============================================================================
target_sources(SparkTests PRIVATE
    harden/Test_core_hardening.cpp
    harden/Test_tests_inversekinematics.cpp
    harden/Test_tests_ecsystemordering_real.cpp
    harden/Test_tests_enginecontext_real.cpp
    harden/Test_ui-2d_tween.cpp
    harden/Test_ui-2d_ui.cpp
    harden/Test_gameplay_inventory.cpp
    harden/Test_gameplay_achievement.cpp
    harden/Test_gameplay_instance.cpp
    harden/Test_gameplay_dialogue.cpp
    harden/Test_gameplay_response.cpp
    harden/Test_scripting_hardening.cpp
    harden/Test_tooling_CommandParser.cpp
    harden/Test_ai-anim_navmesh.cpp
    harden/Test_ai-anim_animation.cpp
    harden/Test_graphics_rhi.cpp
    harden/Test_persistence_AsyncDatabasePool.cpp
    harden/Test_persistence_AsyncDatabaseParams.cpp
    harden/Test_persistence_ReplaySystem.cpp
    harden/Test_persistence_SaveSystem.cpp
    harden/Test_persistence_ModSystem.cpp
    harden/Test_engine-misc_SnapshotCount.cpp
    harden/Test_engine-misc_Coroutine.cpp
    harden/Test_engine-misc_MobileGestures.cpp
    harden/Test_engine-misc_EventQueueEviction.cpp
    harden/Test_ecs_audio_doppler.cpp
    harden/Test_ecs_ai_pathfollow.cpp
    harden/Test_lifecycle_ecs_phase_wiring.cpp
    "${CMAKE_SOURCE_DIR}/SparkConsole/src/CommandParser.cpp"
)
# harden/ tests live one directory deeper; ensure they resolve the shared TestFramework.h
target_include_directories(SparkTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Core/RTSEngineSystems.cpp"
    )

    # MMO module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Guild/MMOGuildSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Inventory/MMOInventorySystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Crafting/MMOCraftingSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Trading/MMOTradingSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/WorldBoss/MMOWorldBossSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/UI/MMOLoginUI.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Core/MMOEngineSystems.cpp"
    )

    # Hardening tests requiring ImGui/editor sources (claude/harden-fleet)
    target_sources(SparkTests PRIVATE
        harden/Test_gamemodules_mmochat_di.cpp
        harden/Test_editor_collab_lock_protocol.cpp
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeEditSession.cpp"
    )

    # RPG module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Character/RPGCharacterSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Combat/RPGCombatSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Inventory/RPGInventorySystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/NPC/RPGNPCSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Quest/RPGQuestSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/World/RPGWorldSetup.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Gameplay/RPGDemoSession.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Core/RPGEngineSystems.cpp"
    )

    # Racing module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Vehicle/RacingVehicleSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Track/RacingTrackSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Race/RacingRaceManager.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/AI/RacingAIDriver.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Camera/RacingCameraSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/HUD/RacingHUDSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Core/RacingEngineSystems.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Core/RacingRaceFlow.cpp"
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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Skill/ARPGSkillSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Monster/ARPGMonsterSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Demo/ARPGDemoEncounter.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Core/ARPGEngineSystems.cpp"
    )

    # Editor sources that require ImGui
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/Tests/TestAdvancedAssetPipeline.cpp"
        "${CMAKE_SOURCE_DIR}/Tests/TestGameViewPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/AssetPipeline/AdvancedAssetPipeline.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/AssetPipeline/AdvancedAssetPipelineUI.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/AssetPipeline/AssetProcessors.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorTheme.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/AssetBrowserPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/GameViewPanel.cpp"
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
    target_sources(SparkTests PRIVATE harden/Test_net-world_migration.cpp)
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

# Disable LTO for the test executable. ThinLTO + --gc-sections can discard
# Logger/LogMacros symbols that tests reference only through macro-expanded
# inline code. CI disables LTO project-wide for Clang (-DENABLE_LTO=OFF);
# this property is a safety net for local builds.
set_target_properties(SparkTests PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION OFF
    INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
)

# Standalone process cores remain independently testable while their thin
# executable entry points are covered by --help CTests in each subtree.
if(TARGET SparkServerCore)
    target_sources(SparkTests PRIVATE TestSparkServerApplication.cpp)
    target_link_libraries(SparkTests PRIVATE SparkServerCore)
endif()
if(TARGET SparkGatewayCore)
    target_sources(SparkTests PRIVATE
        TestSparkGatewayCoordinator.cpp
        TestGatewaySecurity.cpp
        TestGatewayAreaControl.cpp)
    target_link_libraries(SparkTests PRIVATE SparkGatewayCore SparkGatewaySecurity)
endif()
if(TARGET SparkCooker)
    add_dependencies(SparkTests SparkCooker)
    add_custom_command(TARGET SparkTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkCooker>
            $<TARGET_FILE_DIR:SparkTests>/$<TARGET_FILE_NAME:SparkCooker>
    )
endif()
if(TARGET SparkAutomation)
    add_dependencies(SparkTests SparkAutomation)
    add_custom_command(TARGET SparkTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkAutomation>
            $<TARGET_FILE_DIR:SparkTests>/$<TARGET_FILE_NAME:SparkAutomation>
    )
endif()

# Add to CTest. SparkTests also emits its granular JUnit report from CTest so
# CI can distinguish a real zero-failure run from an executable launch failure.
enable_testing()
add_test(
    NAME SparkEngineTests
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEST_EXECUTABLE=$<TARGET_FILE:SparkTests>
        -DSPARK_JUNIT_REPORT=${CMAKE_BINARY_DIR}/SparkTests-junit.xml
        -DSPARK_TEST_LOG=${CMAKE_BINARY_DIR}/SparkTests.log
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkTests.cmake
)
set_tests_properties(SparkEngineTests PROPERTIES TIMEOUT 210)
add_test(
    NAME SparkTemplateAssets
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEMPLATE_ROOT=${CMAKE_SOURCE_DIR}/Templates
        -P ${CMAKE_SOURCE_DIR}/cmake/VerifyTemplateAssets.cmake
)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME SparkTemplateRuntimeSheets
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Tools/validate_template_runtime_sheets.py
            ${CMAKE_SOURCE_DIR}/Templates
    )
    add_test(
        NAME SparkCliContract
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/Tools/spark-cli/tests
            -p test_*.py
            -v
    )
    set_tests_properties(SparkCliContract PROPERTIES LABELS "cli;integration" TIMEOUT 180)
else()
    message(WARNING "Python 3 not found; template runtime-sheet and Spark CLI validation are disabled")
endif()
