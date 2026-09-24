cmake_minimum_required(VERSION 3.25)

# SparkEngine Test Suite
# Uses a lightweight test framework without external dependencies

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(TARGET SparkCollabServer AND TARGET SparkEngineLib)
    add_executable(SparkCollabProcessSmoke
        "${CMAKE_SOURCE_DIR}/SparkDaemon/tests/CollabProcessSmoke.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/StandaloneCollaborationClient.cpp"
    )
    target_include_directories(SparkCollabProcessSmoke PRIVATE
        "${CMAKE_SOURCE_DIR}/SparkDaemon/src"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source"
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
    )
    target_link_libraries(SparkCollabProcessSmoke PRIVATE SparkEngineLib)
    if(MSVC)
        target_compile_options(SparkCollabProcessSmoke PRIVATE /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(SparkCollabProcessSmoke PRIVATE -Wall -Wextra -Werror)
    endif()
    add_dependencies(SparkCollabProcessSmoke SparkCollabServer)
    add_test(NAME SparkCollabProcessSmoke
        COMMAND SparkCollabProcessSmoke "$<TARGET_FILE:SparkCollabServer>")
    set_tests_properties(SparkCollabProcessSmoke PROPERTIES
        LABELS "collaboration;integration;process"
        TIMEOUT 20)
endif()

# The RemoteDebug hostile boundary probe deliberately emulates an external
# translation unit (it defines the legacy test macro and only calls the public
# API), so it cannot live inside SparkTests. It used to be reachable only by
# hand through Tests/Tools/run-remote-debug-security-probes.ps1, which meant the
# static_asserts guarding "the legacy macro must not mint a principal" were
# never evaluated by any build or CI lane.
# Tools/RemoteDebugSecurityBoundaryProbe.cpp is a standalone harness that carries
# its own Logger/SimpleConsole mirrors, so it cannot link against SparkEngineLib
# (LNK2005 duplicate symbols). The protections it probes are enforced in-suite by
# Tests/TestRemoteDebugSystem.cpp; it stays a manual tool, not a CTest target.

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
    TestCoverageAI.cpp
    TestCoverageCamera.cpp
    TestCoverageScripting.cpp
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
    TestModuleABIDiagnostics.cpp
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
    TestConnectionTimeout.cpp
    TestRecastIntegration.cpp
    TestAudioBackendFactory.cpp
    TestAudioEngine.cpp
    TestSceneManager.cpp
    TestPhysicsSystem.cpp
    TestPhysicsTeardownGuard.cpp
    TestGraphicsEngine.cpp
    TestGraphicsBenchmarkStats.cpp
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
    TestRHI240OpenGLReal.cpp
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
    TestNET100TransportReal.cpp
    TestLogger.cpp
    TestProfiler.cpp
    TestStackTrace.cpp
    TestJobSystem.cpp
    TestOpaqueHandle.cpp
    TestSparkError.cpp
    TestThreadDebugger.cpp
    TestCollisionSystem.cpp
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
    TestRHI230VulkanValidationReal.cpp
    TestMSanCanary.cpp
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
    TestMMOAssetImport.cpp
    TestENG220ObjImportReal.cpp
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
    TestENG200ScriptFaultsReal.cpp
    TestScriptHotReload.cpp
    TestVRSystem.cpp
    TestSeamlessAreaManager.cpp
    TestAreaAssetLoader.cpp
    TestEventResponseSystem.cpp
    TestVisualScriptCompiler.cpp
    TestEntityPresetManager.cpp
    TestGamePackager.cpp
    TestASSET220GamePackagerReal.cpp
    TestAssetValidator.cpp
    TestAssetMigration.cpp
    TestAccessibility.cpp
    TestPlatformInput.cpp
    TestTelemetry.cpp
    TestTelemetrySpool.cpp
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
    TestNeuralWeightsValidation.cpp
    TestCpuNeuralInference.cpp
    TestCpuNeuralTraining.cpp
    TestNeuralTextureCompressor.cpp
    TestNeuralRadianceCache.cpp
    TestNeuralPostProcessing.cpp
    TestGPUProfiler.cpp
    TestVRAMTelemetry.cpp
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
    TestEditorLayoutManager.cpp
    TestGizmoMath.cpp
    # Game module tests
    TestSparkGameRPG.cpp
    TestSparkGameARPG.cpp
    TestSparkGameRTS.cpp
    TestSparkGameRacing.cpp
    TestSparkGamePlatformer.cpp
    # Security
    TestMemoryIntegrity.cpp
    TestMemoryIntegrityLifecycle.cpp
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
    # PLT-210 Linux defects in the POSIX Process implementation
    TestPLT210ProcessPosixReal.cpp
    TestPLT210AngelScriptVector3Real.cpp
    TestPLT210RuntimeProcessesReal.cpp
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
    TestEditorAssetReference.cpp
    TestDedicatedServerProcessController.cpp
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/DedicatedServerProcessController.cpp"
    TestServiceTopologyController.cpp
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/ServiceTopologyController.cpp"
    TestLauncherProcess.cpp
    "${CMAKE_SOURCE_DIR}/SparkLauncher/src/LauncherProcess.cpp"
    TestLauncherPaths.cpp
    "${CMAKE_SOURCE_DIR}/SparkLauncher/src/LauncherPaths.cpp"
    TestCoreAndBuildSystems.cpp
    TestOpenWorldModule.cpp
    TestGameModuleRTS.cpp
    TestMOD370SkirmishDeterminismReal.cpp
    TestGameModuleMMO.cpp
    TestGameModuleRPG.cpp
    TestGameModuleRacing.cpp
    TestMOD380RacingCompleteRaceReal.cpp
    TestGameModulePlatformerARPG.cpp
    TestMOD340PlatformerCompletionReal.cpp
    TestMOD330ARPGDungeonReal.cpp
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
    # DATA-120 atomic multi-row character commits + schema versioning (TFDatabase above)
    TestDATA120PersistenceReal.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFPlayerMeta.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFAccountSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFCharacterSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFCrypto.cpp"
    # DATA-120 / OD-22: no plaintext password, session token, or DB credential
    # at rest (TF account store above + MMO KV store + shipped config scan).
    TestDATA120SecretsAtRest.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Persistence/MMOPersistenceSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Reputation/MMOReputationSystem.cpp"
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
    # FPS game module sources needed by TestSparkGameFPSLoopReal / TestFPSComponentsReal /
    # TestSparkGameFPSMirrorCompanionsReal. None of the four pulls in Player or D3D11 symbols,
    # so they link into the headless test binary.
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/GameMechanicsRespawn.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FPSLocalProfile.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FPSAssetPaths.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/ProgressionSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Core/EngineWeatherAdapter.cpp"
    # Editor core sources (no ImGui dependency)
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorLogger.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorCrashHandler.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorRecovery.cpp"
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
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Gizmos/SceneEditTools.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Gizmos/SceneEditToolsDuplicate.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/SceneFile.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/SceneComponentCodec.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/SceneSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/BinarySceneSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/SceneSystem/JSONSceneSerializer.cpp"
    # Editor sources needed by tests
    TestEngineInterfaceProtocol.cpp
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/EngineInterface.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeEditSession.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeEditSessionBroker.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/StandaloneCollaborationClient.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/LiveEditBridge.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Workflow/EditorWorkflow.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prototyping/PrototypingSystem.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/UIDesigner/UIDesignerSystem.cpp"
)

# This is an in-tree compatibility check, not an installed-SDK consumer. It
# intentionally includes the legacy engine enum and verifies that it remains
# aligned with the canonical public SDK type at compile time.
add_library(SparkWeaponEnumCompatibility OBJECT
    "${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/FPSProgression/weapon_enum_compatibility.cpp"
)
target_compile_features(SparkWeaponEnumCompatibility PRIVATE cxx_std_23)
target_include_directories(SparkWeaponEnumCompatibility PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
    "${CMAKE_SOURCE_DIR}/SparkSDK/Include"
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

# The stable-v1 Windows shipping profile deliberately disables networking.
# Keep this public-header include contract independently buildable so a
# networking-only type cannot leak into that profile through an unguarded
# header dependency.
if(NOT ENABLE_NETWORKING)
    add_executable(SparkNetworkingDisabledCompileContract
        Fixtures/NetworkingDisabledCompileContract.cpp)
    target_compile_features(SparkNetworkingDisabledCompileContract PRIVATE cxx_std_23)
    target_include_directories(SparkNetworkingDisabledCompileContract PRIVATE
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source")
endif()

if(TARGET SparkGatewayCore)
    add_executable(SparkGatewayTopologyProbe
        Fixtures/GatewayTopologyProbe.cpp)
    target_compile_features(SparkGatewayTopologyProbe PRIVATE cxx_std_23)
    target_link_libraries(SparkGatewayTopologyProbe PRIVATE SparkGatewayCore)
    set_target_properties(SparkGatewayTopologyProbe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# Real shared-library fixtures for the pre-instantiation module ABI gate.
# They intentionally do not link SparkEngineLib: the boundary under test must
# be satisfied by SDK headers and the exported C ABI alone.
add_library(SparkMismatchedModuleFixture SHARED
    Fixtures/ModuleABI/MismatchedModule.cpp)
add_library(SparkCompatibleModuleFixture SHARED
    Fixtures/ModuleABI/CompatibleModule.cpp)
add_library(SparkRegistryLifecycleModuleFixture SHARED
    Fixtures/ModuleABI/RegistryLifecycleModule.cpp)
add_library(SparkLegacyModuleFixture SHARED
    Fixtures/ModuleABI/LegacyModule.cpp)
add_library(SparkModuleSiblingDependency SHARED
    Fixtures/ModuleABI/SiblingDependency.cpp)
add_library(SparkSiblingDependentModuleFixture SHARED
    Fixtures/ModuleABI/SiblingDependentModule.cpp)

foreach(_module_fixture IN ITEMS SparkMismatchedModuleFixture SparkCompatibleModuleFixture SparkLegacyModuleFixture)
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

target_compile_features(SparkRegistryLifecycleModuleFixture PRIVATE cxx_std_23)
target_compile_definitions(SparkRegistryLifecycleModuleFixture PRIVATE SPARK_MODULE_DLL)
target_include_directories(SparkRegistryLifecycleModuleFixture PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
    "${CMAKE_SOURCE_DIR}/SparkSDK/Include"
    "${SPARK_GENERATED_INCLUDE_DIR}")
if(WIN32)
    target_link_libraries(SparkRegistryLifecycleModuleFixture PRIVATE SparkEngineLib)
elseif(TARGET SparkEngineInterface)
    target_link_libraries(SparkRegistryLifecycleModuleFixture PRIVATE SparkEngineInterface)
endif()
if(MSVC)
    set_property(TARGET SparkRegistryLifecycleModuleFixture PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

target_include_directories(SparkLegacyModuleFixture PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source")

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
spark_configure_module_abi(SparkRegistryLifecycleModuleFixture)
spark_configure_module_abi(SparkLegacyModuleFixture)
spark_configure_module_abi(SparkSiblingDependentModuleFixture)

add_dependencies(SparkTests SparkMismatchedModuleFixture SparkCompatibleModuleFixture
    SparkRegistryLifecycleModuleFixture SparkLegacyModuleFixture
    SparkSiblingDependentModuleFixture)
target_compile_definitions(SparkTests PRIVATE
    SPARK_TEST_MISMATCHED_MODULE_PATH="$<TARGET_FILE:SparkMismatchedModuleFixture>"
    SPARK_TEST_COMPATIBLE_MODULE_PATH="$<TARGET_FILE:SparkCompatibleModuleFixture>"
    SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH="$<TARGET_FILE:SparkRegistryLifecycleModuleFixture>"
    SPARK_TEST_LEGACY_MODULE_PATH="$<TARGET_FILE:SparkLegacyModuleFixture>"
    SPARK_TEST_SIBLING_DEPENDENT_MODULE_PATH="$<TARGET_FILE:SparkSiblingDependentModuleFixture>")

if(TARGET SparkGame AND NOT WIN32)
    add_dependencies(SparkTests SparkGame)
    target_compile_definitions(SparkTests PRIVATE
        SPARK_TEST_SPARK_GAME_MODULE_PATH="$<TARGET_FILE:SparkGame>")
endif()

if(TARGET SparkDaemon)
    add_dependencies(SparkTests SparkDaemon)
    target_compile_definitions(SparkTests PRIVATE
        SPARK_TEST_DAEMON_PATH="$<TARGET_FILE:SparkDaemon>")
endif()

# Real native images for the stable plugin C ABI. Metadata is produced by the
# same public helper shipped to SDK consumers, so successful loading also
# proves that generated ABI fields match Spark/PluginABI.h.
spark_add_plugin(SparkValidPluginFixture
    ID "org.sparkengine.test.native-plugin"
    VERSION "1.0.0"
    TYPE "editor-extension"
    SOURCES Fixtures/PluginABI/ValidPlugin.cpp)
spark_add_plugin(SparkForwardMinorPluginFixture
    ID "org.sparkengine.test.forward-minor-plugin"
    VERSION "1.0.0"
    TYPE "editor-extension"
    SOURCES Fixtures/PluginABI/ForwardMinorPlugin.cpp)
spark_add_plugin(SparkRollbackRefusingPluginFixture
    ID "org.sparkengine.test.rollback-refusing-plugin"
    VERSION "1.0.0"
    TYPE "editor-extension"
    SOURCES Fixtures/PluginABI/RollbackRefusingPlugin.cpp)

add_dependencies(SparkTests SparkValidPluginFixture SparkForwardMinorPluginFixture SparkRollbackRefusingPluginFixture)
target_compile_definitions(SparkTests PRIVATE
    SPARK_TEST_VALID_PLUGIN_PATH="$<TARGET_FILE:SparkValidPluginFixture>"
    SPARK_TEST_FORWARD_MINOR_PLUGIN_PATH="$<TARGET_FILE:SparkForwardMinorPluginFixture>"
    SPARK_TEST_ROLLBACK_REFUSING_PLUGIN_PATH="$<TARGET_FILE:SparkRollbackRefusingPluginFixture>")

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
    # Tests that compile the real FPS GameMode request module headers
    # explicitly; SparkEngineLib no longer exports a game-module include path.
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source"
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
    EDITOR_VERSION_MAJOR=${SparkEngine_VERSION_MAJOR}
    EDITOR_VERSION_MINOR=${SparkEngine_VERSION_MINOR}
    EDITOR_VERSION_PATCH=${SparkEngine_VERSION_PATCH}
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
    if(APPLE)
        target_link_options(SparkTests PRIVATE "LINKER:-export_dynamic")
    endif()
endif()

if(WIN32)
    target_sources(SparkTests PRIVATE TestWorldBasicRender.cpp)
    target_sources(SparkTests PRIVATE TestLegacyGameObjectMaterial.cpp)
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
# Release-readiness sweep — production-source regression tests
# =============================================================================
target_sources(SparkTests PRIVATE
    TestAudioECSBindingReal.cpp
    TestAudioEngineReal.cpp
    TestBuildInfraReal.cpp
    TestConsoleProcessPipeReal.cpp
    TestCrashHandlerGatingReal.cpp
    TestEditorCrashHandlerFilterReal.cpp
    TestEditorRecovery.cpp
    TestEngineWiringReal.cpp
    TestD3D11DeviceContractsReal.cpp
    TestRHI210D3D11GoldenReal.cpp
    TestEditorGizmoTransformReal.cpp
    TestEditorProjectMaterializationReal.cpp
    TestEditorUndoHierarchyReal.cpp
    TestEDT210InspectorEditCommitReal.cpp
    TestFPSComponentsReal.cpp
    TestLoggerSinksReal.cpp
    TestModuleLifecycleReal.cpp
    TestRunnerSemanticsReal.cpp
    TestSEC100ChatAuditLogReal.cpp
    TestSaveSystemRoundTripReal.cpp
    TestSAVE230NewerFormatSlotReal.cpp
    TestSecurityParsersReal.cpp
    TestTextureStexBoundsReal.cpp
    TestShaderCompilerReal.cpp
    TestShadowPassReal.cpp
    TestSparkGameFPSLoopReal.cpp
    TestSparkGameFPSMirrorCompanionsReal.cpp
    TestMOD310FPSSceneReloadRespawnReal.cpp
    TestFPSWeatherPort.cpp
    TestTemplateRuntimeReal.cpp
    TestUserDataPathsReal.cpp
    TestHEAD220NullRHILifetimeReal.cpp
    TestSceneManagerUnicodeReal.cpp
    TestSceneSaveConfinedReal.cpp
    TestWeaponMechanicsReal.cpp
)

# TestBuildInfraReal.cpp asserts the generated SDK version header still carries
# the version CMake is building, so the number has to reach the test as data.
target_compile_definitions(SparkTests PRIVATE
    SPARK_TESTS_EXPECTED_VERSION="${PROJECT_VERSION}")

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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/FogOfWar/RTSFogOfWarSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Match/RTSMatchSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Simulation/RTSSkirmishSimulation.cpp"
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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Player/PlatformerPlayerCollision.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Level/PlatformerLevelSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Level/PlatformerPlatformSimulation.cpp"
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
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorNotificationManager.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/AssetBrowserPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/GameViewPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/InspectorVectorControl.cpp"
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
        # Release-readiness sweep: editor tests that drive real panel and
        # subsystem classes. Every one of these production .cpp files calls
        # ImGui, so neither the tests nor the sources can live in the
        # ungated list above.
        "${CMAKE_SOURCE_DIR}/Tests/TestEditorPanelsRealBackends.cpp"
        "${CMAKE_SOURCE_DIR}/Tests/TestEditorSubsystemsReal.cpp"
        "${CMAKE_SOURCE_DIR}/Tests/TestVersionControlSystemGitReal.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/TimeOfDayPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/ModdingPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/ReplayPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/SaveSystemPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/WeatherFogPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/SceneStatisticsPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/SearchPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/SearchPanelSearch.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/CSGEditorPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/WeaponEditorPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/ObjectPlacementPanel.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Terrain/TerrainEditor.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Terrain/TerrainEditorTools.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Terrain/TerrainEditorUI.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Terrain/TerrainEditorProperties.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Terrain/TerrainData.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prefabs/PrefabManager.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prefabs/PrefabManagerInstances.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prefabs/PrefabAsset.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Profiler/PerformanceProfiler.cpp"
    )

    message(STATUS "[Tests] ImGui available — gated editor + 6 game module tests enabled")
else()
    message(STATUS "[Tests] ImGui not available — gated editor & OpenWorld tests disabled")
endif()

# Propagate engine feature defines so tests can gate on them
if(ENABLE_NETWORKING)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_NETWORKING)
    target_sources(SparkTests PRIVATE
        TestNetworkStack.cpp
        TestNetworkSecurity.cpp
        TestNetworkSecurityPhaseHH.cpp
        harden/Test_net-world_migration.cpp)
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
    if(NOT WIN32)
        # SparkServerCore carries SparkEngineLib as WHOLE_ARCHIVE while other
        # dependencies link it plainly; see SparkServer/CMakeLists.txt.
        set_property(TARGET SparkTests PROPERTY
            LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,SparkEngineLib")
    endif()
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

# ----------------------------------------------------------------------------
# Derived test-count floor
# ----------------------------------------------------------------------------
# CI carries hand-written --min-tests literals. A literal cannot notice that a
# whole test family stopped being compiled in, so the registered case count is
# also emitted here, derived from the source list this build actually compiles.
# testMacrosUnconditional in the report is a sound floor (those macros are
# compiled in every configuration); testMacrosTotal additionally counts
# #if-gated macros and is an upper bound, so it must not be used as a floor.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    get_target_property(_spark_test_sources SparkTests SOURCES)
    string(REPLACE ";" "\n" _spark_test_source_lines "${_spark_test_sources}")
    file(WRITE "${CMAKE_BINARY_DIR}/test-registration-sources.txt" "${_spark_test_source_lines}\n")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/Tools/count_registered_tests.py"
            --source-list "${CMAKE_BINARY_DIR}/test-registration-sources.txt"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
            --output "${CMAKE_BINARY_DIR}/test-registration-count.json"
        RESULT_VARIABLE _spark_test_count_result
        OUTPUT_VARIABLE _spark_test_count_output
        ERROR_VARIABLE _spark_test_count_error)
    if(NOT _spark_test_count_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to derive the registered SparkTests case count: "
            "${_spark_test_count_output}${_spark_test_count_error}")
    endif()
    message(STATUS "[Tests] ${_spark_test_count_output}")
    unset(_spark_test_sources)
    unset(_spark_test_source_lines)
else()
    message(FATAL_ERROR "Python3 is required to derive the registered SparkTests case count")
endif()

# Add to CTest. SparkTests also emits its granular JUnit report from CTest so
# CI can distinguish a real zero-failure run from an executable launch failure.
enable_testing()

# ------------------------------------------------------------------
# SparkTests wall-clock budgets (single source of truth)
# ------------------------------------------------------------------
# The wrapper's execute_process TIMEOUT always fires before ctest's own TIMEOUT,
# so the two numbers must be derived from one place or the outer one is dead
# code. Debug (/Od /RTC1 /JMC) runs several times slower than Release, which is
# why one constant could not serve both.
set(SPARK_TEST_TIMEOUT_DEBUG 900 CACHE STRING
    "Wall-clock budget in seconds for the Debug SparkEngineTests run")
set(SPARK_TEST_TIMEOUT_OPTIMIZED 240 CACHE STRING
    "Wall-clock budget in seconds for optimized-configuration SparkEngineTests runs")
set(SPARK_TEST_LOAD_TIMEOUT_DEBUG 900 CACHE STRING
    "Wall-clock budget in seconds for the Debug SparkEngineLoadTests run")
set(SPARK_TEST_LOAD_TIMEOUT_OPTIMIZED 300 CACHE STRING
    "Wall-clock budget in seconds for optimized-configuration SparkEngineLoadTests runs")
# Manual promotion knob, deliberately not enabled by any CI lane. It is the
# executable form of "prove nothing still needs its waiver": flip it ON locally
# (or in a nightly) and every Tests/TestWarnings.h entry becomes a hard failure,
# so entries that no longer earn their keep can be deleted with evidence.
# It stays OFF in CI because the registry documents real non-determinism -- e.g.
# Integration_NetworkingECS_ReplicationLatencyJitterPredictionReconciliation is
# recorded as failing ~60% of runs on pristine main -- and turning that into a
# hard failure would make a required lane red on schedule rather than on a
# regression. Enable it in CI only once the registry is empty.
option(SPARK_TESTS_WARN_IS_ERROR
    "Run the ctest SparkTests lanes with --warn-is-error (known-flaky waivers become failures)" OFF)

# ctest must outlive the wrapper in every configuration, so its budget is
# derived from the largest inner budget rather than written down separately.
foreach(_spark_budget IN ITEMS SPARK_TEST_TIMEOUT_DEBUG SPARK_TEST_TIMEOUT_OPTIMIZED
                              SPARK_TEST_LOAD_TIMEOUT_DEBUG SPARK_TEST_LOAD_TIMEOUT_OPTIMIZED)
    if(NOT "${${_spark_budget}}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "${_spark_budget} must be a positive integer, got '${${_spark_budget}}'")
    endif()
endforeach()
set(SPARK_TEST_CTEST_MARGIN_SECONDS 60)
set(_spark_ctest_inner_max ${SPARK_TEST_TIMEOUT_DEBUG})
foreach(_spark_budget IN ITEMS ${SPARK_TEST_TIMEOUT_OPTIMIZED}
                              ${SPARK_TEST_LOAD_TIMEOUT_DEBUG} ${SPARK_TEST_LOAD_TIMEOUT_OPTIMIZED})
    if(_spark_budget GREATER _spark_ctest_inner_max)
        set(_spark_ctest_inner_max ${_spark_budget})
    endif()
endforeach()
math(EXPR SPARK_TEST_CTEST_TIMEOUT "${_spark_ctest_inner_max} + ${SPARK_TEST_CTEST_MARGIN_SECONDS}")

# The load/stress family runs 3000 engine frames, a 100000-entity flood and a
# 100000-event storm; giving it its own lane keeps one heavy family from
# consuming the unit suite's budget. The exclusion is a substring match over
# test names, so it names only the two heavy prefixes: the handful of cheap
# EdgeCase_ tests that share TestEngineLoadTest.cpp run in both lanes, which
# costs milliseconds and cannot drop a test the way a broader pattern could.
set(SPARK_TEST_LOAD_EXCLUDE "LoadTest_,DeepStress_")

add_test(
    NAME SparkEngineTests
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEST_EXECUTABLE=$<TARGET_FILE:SparkTests>
        -DSPARK_JUNIT_REPORT=${CMAKE_BINARY_DIR}/SparkTests-junit.xml
        -DSPARK_TEST_LOG=${CMAKE_BINARY_DIR}/SparkTests.log
        -DSPARK_TEST_OUTPUT=${CMAKE_BINARY_DIR}/SparkTests-output.log
        -DSPARK_TEST_WARN_IS_ERROR=${SPARK_TESTS_WARN_IS_ERROR}
        "-DSPARK_TEST_TIMEOUT_SECONDS=$<IF:$<CONFIG:Debug>,${SPARK_TEST_TIMEOUT_DEBUG},${SPARK_TEST_TIMEOUT_OPTIMIZED}>"
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkTests.cmake
)
# The main suite holds both unit and Integration_* families, so the documented
# `ctest -L unit` and `ctest -L integration` selectors must both reach it.
set_tests_properties(SparkEngineTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_EXCLUDE=${SPARK_TEST_LOAD_EXCLUDE}"
    LABELS "sparktests;unit;integration"
    TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})

add_test(
    NAME SparkEngineLoadTests
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEST_EXECUTABLE=$<TARGET_FILE:SparkTests>
        -DSPARK_JUNIT_REPORT=${CMAKE_BINARY_DIR}/SparkTests-load-junit.xml
        -DSPARK_TEST_LOG=${CMAKE_BINARY_DIR}/SparkTests-load.log
        -DSPARK_TEST_OUTPUT=${CMAKE_BINARY_DIR}/SparkTests-load-output.log
        -DSPARK_TEST_WARN_IS_ERROR=${SPARK_TESTS_WARN_IS_ERROR}
        "-DSPARK_TEST_TIMEOUT_SECONDS=$<IF:$<CONFIG:Debug>,${SPARK_TEST_LOAD_TIMEOUT_DEBUG},${SPARK_TEST_LOAD_TIMEOUT_OPTIMIZED}>"
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkTests.cmake
)
# SPARK_TEST_EXPECT_COUNT fails the lane if the family silently stops being
# compiled in, so the split cannot turn into a lane that runs nothing.
set_tests_properties(SparkEngineLoadTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestEngineLoadTest.cpp;SPARK_TEST_EXPECT_COUNT=22"
    LABELS "load;slow"
    RUN_SERIAL TRUE
    TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})

# RHI-240: real OpenGL backend under KHR_debug (llvmpipe on Linux hosts). The
# lane requires a GL context (SPARK_REQUIRE_OPENGL=1) so it cannot pass by
# skipping; llvmpipe evidence is software-rasterizer only, not driver certification.
# The dedicated real-GL lane needs Mesa (EGL/llvmpipe or GLX) and a 4.5 core context: Linux only.
# macOS caps at GL 4.1 core without KHR_debug/DSA and has no headless context bootstrap.
if(SPARK_OPENGL_AVAILABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(
        NAME SparkOpenGLTests
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_TEST_EXECUTABLE=$<TARGET_FILE:SparkTests>
            -DSPARK_JUNIT_REPORT=${CMAKE_BINARY_DIR}/SparkTests-opengl-junit.xml
            -DSPARK_TEST_LOG=${CMAKE_BINARY_DIR}/SparkTests-opengl.log
            -DSPARK_TEST_OUTPUT=${CMAKE_BINARY_DIR}/SparkTests-opengl-output.log
            -DSPARK_TEST_WARN_IS_ERROR=${SPARK_TESTS_WARN_IS_ERROR}
            "-DSPARK_TEST_TIMEOUT_SECONDS=$<IF:$<CONFIG:Debug>,${SPARK_TEST_TIMEOUT_DEBUG},${SPARK_TEST_TIMEOUT_OPTIMIZED}>"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkTests.cmake
    )
    set_tests_properties(SparkOpenGLTests PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestRHI240OpenGLReal.cpp;SPARK_TEST_EXPECT_COUNT=11;SPARK_REQUIRE_OPENGL=1"
        LABELS "opengl;llvmpipe"
        TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})

    # RHI-240: the production SDL2 host must honor SPARK_RHI_BACKEND=opengl
    # (initialize OpenGL, never NullRHI) and must exit non-zero when that
    # explicit request cannot be met. Uses DISPLAY or wraps itself in xvfb-run;
    # having neither fails the lane rather than skipping it.
    if(SPARK_PACKAGE_HAS_SDL2 AND TARGET SparkEngine)
        find_program(SPARK_XVFB_RUN_EXECUTABLE xvfb-run)
        add_dependencies(SparkTests SparkEngine)
        add_test(
            NAME SparkEngineExplicitOpenGLStartup
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
                "-DSPARK_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/opengl-explicit-startup"
                "-DSPARK_XVFB_RUN=${SPARK_XVFB_RUN_EXECUTABLE}"
                -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkOpenGLExplicitStartup.cmake
        )
        set_tests_properties(SparkEngineExplicitOpenGLStartup PROPERTIES
            LABELS "opengl;llvmpipe;linux;sdl2;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 240)
    endif()
endif()
add_test(NAME SparkSaveCompatibilityTests COMMAND $<TARGET_FILE:SparkTests> --quiet)
set_tests_properties(SparkSaveCompatibilityTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=SaveMigration_"
    LABELS "compatibility;save;unit"
    RUN_SERIAL TRUE
    TIMEOUT 30)

add_test(NAME TelemetrySpool COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(TelemetrySpool PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=Telemetry_SpoolRecovery;SPARK_TEST_EXPECT_COUNT=8"
    LABELS "telemetry-integration"
    RUN_SERIAL TRUE
    TIMEOUT 30)

# OPS-100: retired crash-transport credentials are dropped on settings load and never saved.
add_test(NAME CrashSettingsCredentials COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(CrashSettingsCredentials PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=CrashSettings_;SPARK_TEST_EXPECT_COUNT=1"
    LABELS "crash-security;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 30)

# DATA-120 (OD-22): no plaintext password, session token, or database credential
# reaches a persistence store or a shipped config file.
add_test(NAME PersistenceSecretsAtRest COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(PersistenceSecretsAtRest PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestDATA120SecretsAtRest.cpp;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "persistence;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 60)

add_test(NAME LifecyclePartialInit COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(LifecyclePartialInit PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=PartialInit_EngineContext;SPARK_TEST_EXPECT_COUNT=4"
    LABELS "lifecycle;unit"
    TIMEOUT 30)

# RHI-230: Vulkan backend under VK_LAYER_KHRONOS_validation (Lavapipe on CI).
# Lavapipe proves API-usage correctness only, not hardware certification.
if(SPARK_VULKAN_AVAILABLE)
    add_test(NAME VulkanValidation COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(VulkanValidation PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestRHI230VulkanValidationReal.cpp;SPARK_TEST_EXPECT_COUNT=16"
        LABELS "vulkan;rendering;unit"
        RUN_SERIAL TRUE
        TIMEOUT 120)
endif()

foreach(_remote_admin_selector IN ITEMS
        RemoteAdmin_AnonymousDenied
        RemoteAdmin_MalformedTimestampDenied
        RemoteAdmin_RoleMatrix
        RemoteAdmin_ReplayDenied
        RemoteAdmin_RateLimited
        RemoteAdmin_ExpiredDenied
        RemoteAdmin_ReservedCommandsCannotBeRebound)
    add_test(NAME ${_remote_admin_selector} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(${_remote_admin_selector} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=${_remote_admin_selector};SPARK_TEST_EXPECT_COUNT=1"
        LABELS "remote-admin;security;unit"
        RUN_SERIAL TRUE
        TIMEOUT 30)
endforeach()

foreach(_remote_debug_epoch_selector IN ITEMS
        RemoteDebug_ResponseEpochStartListening
        RemoteDebug_ResponseEpochStopListening)
    add_test(NAME ${_remote_debug_epoch_selector} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(${_remote_debug_epoch_selector} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=${_remote_debug_epoch_selector};SPARK_TEST_EXPECT_COUNT=1"
        LABELS "remote-admin;security;unit"
        RUN_SERIAL TRUE
        TIMEOUT 30)
endforeach()

# SEC-100: gateway area-control fail-closed and audit cases, one selector each
# so a hostile-frame regression names the exact rejection that stopped holding.
if(TARGET SparkGatewayCore)
    foreach(_gateway_area_control_selector IN ITEMS
            GatewayAreaControl_RejectsWrongKeyMac
            GatewayAreaControl_RejectsStaleAndFutureTimestamp
            GatewayAreaControl_RejectsReplayedNonce
            GatewayAreaControl_RejectsNewNoncesWhenReplayLedgerIsFull
            GatewayAreaControl_AuditRecordOmitsSecrets)
        add_test(NAME ${_gateway_area_control_selector} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(${_gateway_area_control_selector} PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME=${_gateway_area_control_selector};SPARK_TEST_EXPECT_COUNT=1"
            LABELS "gateway;security;network"
            RUN_SERIAL TRUE
            TIMEOUT 30)
    endforeach()
endif()

# RHI-210: D3D11 golden-frame and render-target resource contracts. Exact
# counts so a filter that silently selects nothing (or a skip) cannot pass.
if(WIN32)
    add_test(NAME D3D11_Golden COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(D3D11_Golden PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=D3D11_Golden_;SPARK_TEST_EXPECT_COUNT=2"
        LABELS "d3d11-golden;d3d11;stable-v1"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    add_test(NAME D3D11_Resource COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(D3D11_Resource PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=D3D11_Resource_;SPARK_TEST_EXPECT_COUNT=2"
        LABELS "d3d11-stress;d3d11;stable-v1"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()

add_test(
    NAME ModuleProfileLifecycleParserContract
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_LIFECYCLE_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkModuleProfileLifecycle.cmake
)

if(TARGET SparkNetworkingDisabledCompileContract)
    add_test(NAME SparkNetworkingDisabledCompileContract
        COMMAND $<TARGET_FILE:SparkNetworkingDisabledCompileContract>)
    set_tests_properties(SparkNetworkingDisabledCompileContract PROPERTIES
        LABELS "networking;build-contract"
        TIMEOUT 10)
endif()
set_tests_properties(ModuleProfileLifecycleParserContract PROPERTIES
    LABELS "module-profile;stable-v1;d3d11;unit"
    TIMEOUT 10)

add_test(
    NAME FPSPackage_SaveReloadParserContract
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        -DSPARK_FPS_SAVE_RELOAD_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledFPSSaveReload.cmake)
set_tests_properties(FPSPackage_SaveReloadParserContract PROPERTIES
    LABELS "module;stable-v1;fps;package;unit"
    TIMEOUT 10)

if(UNIX AND NOT APPLE AND SPARK_PACKAGE_HAS_SDL2 AND BUILD_GAME_MODULES AND
   TARGET SparkEngine AND TARGET SparkGame)
    add_dependencies(SparkTests SparkEngine SparkGame)
    add_test(
        NAME LinuxSDLConsoleBeforeModules
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGame>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkLinuxSDLConsoleStartup.cmake
    )
    set_tests_properties(LinuxSDLConsoleBeforeModules PROPERTIES
        LABELS "lifecycle;module;linux;sdl2;integration;process"
        RUN_SERIAL TRUE
        TIMEOUT 45)
endif()

add_test(
    NAME NullRHIWindowsLifecycleParserContract
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_HEADLESS_NULLRHI_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkHeadlessNullRHILifecycle.cmake
)
set_tests_properties(NullRHIWindowsLifecycleParserContract PROPERTIES
    LABELS "nullrhi-headless;stable-v1;unit"
    TIMEOUT 10)

# HEAD-220: NullRHIDevice live-resource accounting follows resource lifetime
# (leak/soak signal for packaged headless runs).
add_test(NAME NullRHIResourceLifetime COMMAND $<TARGET_FILE:SparkTests> --empty-is-error)
set_tests_properties(NullRHIResourceLifetime PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=NullRHI_Lifetime_;SPARK_TEST_EXPECT_COUNT=6"
    LABELS "nullrhi-headless;stable-v1;unit"
    TIMEOUT 30)

if(WIN32 AND NOT CMAKE_CROSSCOMPILING AND BUILD_GAME_MODULES)
    if(NOT TARGET SparkEngine OR NOT TARGET SparkGameFPS)
        message(FATAL_ERROR
            "BUILD_GAME_MODULES=ON requires SparkEngine and SparkGameFPS "
            "for the stable-v1 module-profile lifecycle gate")
    endif()

    # Keep nested MSVC consumer try-compiles below the Windows path-length
    # pressure that makes FileTracker fail before the consumer can configure.
    # The build-directory hash keeps separate worktrees/configurations isolated.
    string(MD5 _spark_mod310_build_id "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${_spark_mod310_build_id}" 0 8 _spark_mod310_build_id)
    set(_spark_mod310_test_root "$ENV{TEMP}/m310-${_spark_mod310_build_id}")

    # The nested package project also runs CMake try-compiles. Prefer the
    # bundled Ninja binary over a nested Visual Studio project so those probes
    # do not inherit FileTracker's long-path failure mode.
    set(_spark_mod310_consumer_generator "${CMAKE_GENERATOR}")
    set(_spark_mod310_consumer_platform "${CMAKE_GENERATOR_PLATFORM}")
    set(_spark_mod310_consumer_toolset "${CMAKE_GENERATOR_TOOLSET}")
    set(_spark_mod310_consumer_make_program "${CMAKE_MAKE_PROGRAM}")
    set(_spark_mod310_ninja "")
    if(CMAKE_GENERATOR MATCHES "Ninja" AND
       DEFINED CMAKE_MAKE_PROGRAM AND
       EXISTS "${CMAKE_MAKE_PROGRAM}" AND
       NOT IS_DIRECTORY "${CMAKE_MAKE_PROGRAM}")
        # Preserve the outer generator's known-good make program. A nested
        # CMake configure must not replace it with a stale PATH/cache result.
        set(_spark_mod310_ninja "${CMAKE_MAKE_PROGRAM}")
    else()
        get_filename_component(_spark_mod310_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
        get_filename_component(_spark_mod310_cmake_dist "${_spark_mod310_cmake_bin}" DIRECTORY)
        get_filename_component(_spark_mod310_cmake_bundle "${_spark_mod310_cmake_dist}" DIRECTORY)
        set(_spark_mod310_bundled_ninja "${_spark_mod310_cmake_bundle}/Ninja/ninja.exe")
        if(EXISTS "${_spark_mod310_bundled_ninja}" AND
           NOT IS_DIRECTORY "${_spark_mod310_bundled_ninja}")
            set(_spark_mod310_ninja "${_spark_mod310_bundled_ninja}")
        else()
            find_program(_spark_mod310_ninja NAMES ninja ninja.exe NO_CACHE)
            if(_spark_mod310_ninja AND
               (NOT EXISTS "${_spark_mod310_ninja}" OR
                IS_DIRECTORY "${_spark_mod310_ninja}"))
                set(_spark_mod310_ninja "")
            endif()
        endif()
    endif()
    if(_spark_mod310_ninja)
        set(_spark_mod310_consumer_generator "Ninja Multi-Config")
        set(_spark_mod310_consumer_platform "")
        set(_spark_mod310_consumer_toolset "")
        set(_spark_mod310_consumer_make_program "${_spark_mod310_ninja}")
    elseif(CMAKE_GENERATOR MATCHES "Ninja")
        message(FATAL_ERROR
            "The profile package consumer requires a runnable Ninja make program; "
            "the outer Ninja generator did not provide one")
    endif()

    # Exercise the stable-v1 first-party slice through the production host and
    # real module DLL. Generator expressions keep every path correct for the
    # selected Visual Studio/Ninja Multi-Config configuration.
    add_dependencies(SparkTests SparkEngine SparkGameFPS)
    add_test(
        NAME ModuleProfileLifecycle_SparkGameFPS_D3D11
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGameFPS>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            -DSPARK_RHI_BACKEND=d3d11
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkModuleProfileLifecycle.cmake
    )
    set_tests_properties(ModuleProfileLifecycle_SparkGameFPS_D3D11 PROPERTIES
        LABELS "module-profile;stable-v1;d3d11;integration"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    # Source-tree precursor for HEAD-220. It proves the production Windows
    # host, actual NullRHIDevice, and real SparkGameFPS DLL complete a bounded
    # no-render lifecycle. Packaged clean-layout and soak certification remain
    # separate required work and are not claimed by this test.
    add_test(
        NAME NullRHI_Windows_FPSLifecycle
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGameFPS>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            -DSPARK_RHI_BACKEND=null
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkHeadlessNullRHILifecycle.cmake
    )
    set_tests_properties(NullRHI_Windows_FPSLifecycle PROPERTIES
        LABELS "nullrhi-headless;stable-v1;integration;source-tree"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    # Copy the built FPS runtime to a fresh package-like directory, edit only
    # its copied scene, and inspect post-update game/wave state plus rendered
    # frames. Fresh-process and same-process material edits also catch stale
    # FPS material cache entries without trusting a synthetic scene preview.
    add_test(
        NAME FPSAuthoredScene_PackageLayoutEdit
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_PACKAGE_BIN=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/fps-authored/$<CONFIG>"
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/VerifyFPSAuthoredScene.cmake)
    set_tests_properties(FPSAuthoredScene_PackageLayoutEdit PROPERTIES
        LABELS "module;fps;d3d11;scene;integration"
        RUN_SERIAL TRUE
        TIMEOUT 300)

    # MOD-310 selector contracts. Each entry invokes a real production-source
    # or installed-package test; these are bounded evidence slices, not a
    # declaration that the full FPS module is public-SDK compliant or released.
    add_test(
        NAME FPSSinglePlayerSlice_RespawnProductionSource
        COMMAND $<TARGET_FILE:SparkTests> --quiet --warn-is-error)
    set_tests_properties(FPSSinglePlayerSlice_RespawnProductionSource PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=FPSRespawn_;SPARK_TEST_EXPECT_COUNT=10"
        LABELS "module;stable-v1;fps;production-source"
        RUN_SERIAL TRUE
        TIMEOUT 30)

    if(SPARK_GAME_MODULES STREQUAL "SparkGameFPS")
        add_test(
            NAME FPSPackage_InstalledRuntime
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
                "-DSPARK_CONFIG=$<CONFIG>"
                "-DSPARK_TEST_ROOT=${_spark_mod310_test_root}/p/$<CONFIG>"
                -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledFPSPackage.cmake)
        set_tests_properties(FPSPackage_InstalledRuntime PROPERTIES
            LABELS "module;stable-v1;fps;package;integration;save;d3d11"
            RUN_SERIAL TRUE
            TIMEOUT 600)

        add_test(
            NAME FPSPublicSDK_InstalledConsumers
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
                "-DSPARK_CONFIG=$<CONFIG>"
                "-DSPARK_TEST_ROOT=${_spark_mod310_test_root}/s/$<CONFIG>"
                "-DSPARK_CONSUMER_GENERATOR=${_spark_mod310_consumer_generator}"
                "-DSPARK_CONSUMER_PLATFORM=${_spark_mod310_consumer_platform}"
                "-DSPARK_CONSUMER_TOOLSET=${_spark_mod310_consumer_toolset}"
                "-DSPARK_CONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DSPARK_CONSUMER_MAKE_PROGRAM=${_spark_mod310_consumer_make_program}"
                "-DSPARK_CONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
                "-DSPARK_CONSUMER_CTEST=${CMAKE_CTEST_COMMAND}"
                -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledFPSPublicSDK.cmake)
        set_tests_properties(FPSPublicSDK_InstalledConsumers PROPERTIES
            LABELS "module;stable-v1;fps;sdk;package;integration"
            RUN_SERIAL TRUE
            TIMEOUT 600)
    endif()
endif()

add_test(
    NAME SparkTemplateAssets
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEMPLATE_ROOT=${CMAKE_SOURCE_DIR}/Templates
        -P ${CMAKE_SOURCE_DIR}/cmake/VerifyTemplateAssets.cmake
)
set_tests_properties(SparkTemplateAssets PROPERTIES
    LABELS "template;asset;unit"
    TIMEOUT 60)
add_test(
    NAME SparkVersionSingleSource
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEMPLATE_ROOT=${CMAKE_SOURCE_DIR}/Templates
        -DSPARK_EXPECTED_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/VerifyVersionSingleSource.cmake
)
set_tests_properties(SparkVersionSingleSource PROPERTIES
    LABELS "release;version;unit"
    TIMEOUT 30)
add_test(
    NAME SparkTemplateServiceConfigs
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEMPLATE_ROOT=${CMAKE_SOURCE_DIR}/Templates
        -P ${CMAKE_SOURCE_DIR}/cmake/VerifyTemplateServiceConfigs.cmake
)
set_tests_properties(SparkTemplateServiceConfigs PROPERTIES
    LABELS "template;config;unit"
    TIMEOUT 60)
add_test(
    NAME SparkTemplateServiceConfigValidatorTests
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_TEMPLATE_SOURCE=${CMAKE_SOURCE_DIR}/Templates
        -DSPARK_VALIDATOR=${CMAKE_SOURCE_DIR}/cmake/VerifyTemplateServiceConfigs.cmake
        -DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/template-service-validator-tests
        -DSPARK_BINARY_ROOT=${CMAKE_BINARY_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/TestTemplateServiceConfigValidator.cmake
)
set_tests_properties(SparkTemplateServiceConfigValidatorTests PROPERTIES
    LABELS "template;config;unit"
    TIMEOUT 120)
add_test(
    NAME SparkPackageModuleSidecarValidatorTests
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_VALIDATOR=${CMAKE_SOURCE_DIR}/cmake/ValidateStagedPackageExecutables.cmake
        -DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/module-sidecar-validator-ctest
        -DSPARK_BINARY_ROOT=${CMAKE_BINARY_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/TestValidateStagedPackageModuleSidecars.cmake
)
set_tests_properties(SparkPackageModuleSidecarValidatorTests PROPERTIES
    LABELS "package;unit"
    # The contract launches a separate CMake validator for each malformed
    # fixture. On a busy Windows host, 30 seconds can expire after only the
    # first twenty cases; keep the full adversarial battery bounded at 90.
    TIMEOUT 90)

add_test(
    NAME SparkSDKRejectsAmbiguousVersionHeader
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_GAME_MODULE_CMAKE=${CMAKE_SOURCE_DIR}/cmake/SparkGameModule.cmake"
        "-DFIXTURE=${CMAKE_BINARY_DIR}/sdk-version-header-ambiguity/$<CONFIG>"
        "-DCONSUMER_GENERATOR=${CMAKE_GENERATOR}"
        "-DCONSUMER_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCONSUMER_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCONSUMER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/FPSProgression/TestSparkGameModuleVersion.cmake)
set_tests_properties(SparkSDKRejectsAmbiguousVersionHeader PROPERTIES
    LABELS "sdk;module;unit"
    TIMEOUT 30)

add_test(
    NAME SparkInstalledPackageRejectsMissingVersionDiagnostics
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        "-DFIXTURE=${CMAKE_BINARY_DIR}/incomplete-sdk-package/$<CONFIG>"
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/TestSparkEnginePackageCompleteness.cmake)
set_tests_properties(SparkInstalledPackageRejectsMissingVersionDiagnostics PROPERTIES
    LABELS "sdk;package;unit"
    TIMEOUT 30)

add_test(
    NAME SparkInstalledPackageRejectsMissingPublicSDKHeader
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        "-DFIXTURE=${CMAKE_BINARY_DIR}/incomplete-sdk-public-header/$<CONFIG>"
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/TestSparkEnginePackageRequiresPublicHeader.cmake)
set_tests_properties(SparkInstalledPackageRejectsMissingPublicSDKHeader PROPERTIES
    LABELS "sdk;package;unit"
    TIMEOUT 30)

add_test(
    NAME SparkSDKRequiresInstalledVersionHeader
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_GAME_MODULE_CMAKE=${CMAKE_SOURCE_DIR}/cmake/SparkGameModule.cmake"
        "-DFIXTURE=${CMAKE_BINARY_DIR}/sdk-version-header-missing/$<CONFIG>"
        "-DCONSUMER_GENERATOR=${CMAKE_GENERATOR}"
        "-DCONSUMER_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCONSUMER_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCONSUMER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/TestInstalledPackageRequiresVersionHeader.cmake)
set_tests_properties(SparkSDKRequiresInstalledVersionHeader PROPERTIES
    LABELS "sdk;package;unit"
    TIMEOUT 30)

add_test(
    NAME SparkSDKComponentCompleteness
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DSPARK_CONFIG=$<CONFIG>"
        "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/sdk-component-completeness/$<CONFIG>"
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/TestSparkSDKComponentCompleteness.cmake)
set_tests_properties(SparkSDKComponentCompleteness PROPERTIES
    LABELS "sdk;package;unit"
    TIMEOUT 660)

# ----------------------------------------------------------------------------
# Installed-SDK template gate (opt-in)
# ----------------------------------------------------------------------------
# Configuring and building the nine shipped templates against a *staged install*
# is the only thing that proves the exported SparkEngine package is consumable.
# It stages a full install and runs nine CMake configures, so it is far too slow
# for the default lane: it is off by default and carries the 'installed-sdk'
# label so a nightly (or a developer) can select it explicitly with
#   cmake -B build -DSPARK_ENABLE_INSTALLED_SDK_TESTS=ON
#   ctest --test-dir build -L installed-sdk --no-tests=error
# Both halves matter. Without the -D the label selects nothing, and without
# --no-tests=error ctest reports success for having found no tests at all -- a
# green "installed SDK verified" that never opened the SDK.
# The underlying script is not opt-in: .github/workflows/release.yml runs
# cmake -P cmake/VerifyInstalledTemplates.cmake on every published Windows and
# Linux package, so this option only controls the local CTest registration.
option(SPARK_ENABLE_INSTALLED_SDK_TESTS
    "Register the slow installed-SDK template verification as a CTest" OFF)
if(SPARK_ENABLE_INSTALLED_SDK_TESTS)
    add_test(NAME SparkInstalledTemplates
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_TEMPLATE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/installed-sdk-templates/prefix"
            "-DSPARK_TEMPLATE_BUILD_ROOT=${CMAKE_BINARY_DIR}/installed-sdk-templates/build"
            "-DSPARK_TEMPLATE_SOURCE_ROOT=${CMAKE_SOURCE_DIR}/Templates"
            "-DSPARK_TEMPLATE_CONFIG=$<CONFIG>"
            "-DSPARK_TEMPLATE_LIVE_SMOKE_NAME=EmptyProject"
            # Every live-smoke block in VerifyInstalledTemplates.cmake is guarded
            # by SPARK_ENGINE_EXECUTABLE, including the count reconciliation that
            # proves the requested smoke actually ran. Omitting it did not skip
            # one step: it silently dropped the entire live-load gate and still
            # printed "verification passed". The staging install above puts the
            # engine at <prefix>/bin, which is the same path release.yml passes.
            "-DSPARK_ENGINE_EXECUTABLE=${CMAKE_BINARY_DIR}/installed-sdk-templates/prefix/bin/SparkEngine${CMAKE_EXECUTABLE_SUFFIX}"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyInstalledTemplates.cmake")
    set_tests_properties(SparkInstalledTemplates PROPERTIES
        LABELS "installed-sdk"
        TIMEOUT 3600)
endif()


find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME FPSSceneMaterialContract
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_fps_scene_material_contract.py)
set_tests_properties(FPSSceneMaterialContract PROPERTIES
    LABELS "module;stable-v1;fps;asset;unit"
    TIMEOUT 30)

# RDY-020 / OD-09: the stable-v1 package derives its asset manifest without any
# NOASSERTION entry, installs only that set, and the package check rejects a
# manifest that ships, relabels, or adds an unreviewed asset.
add_test(
    NAME PackageAssets_StableV1ExcludesNoAssertion
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_asset_package_profile.py)
set_tests_properties(PackageAssets_StableV1ExcludesNoAssertion PROPERTIES
    LABELS "stable-v1;asset;packaging;unit;python"
    TIMEOUT 300)

# GOV-400 LicenseInventory_*: the staged-package notice-coverage gate
# (cmake/ValidateStagedPackageNotices.cmake) against fixture packages, and the
# notices generator's shared rule set, parity with the CMake gate, and the real
# packaged notice file (Tests/Tools/test_third_party_notices.py).
add_test(
    NAME LicenseInventory_PackageNoticeCoverage
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_VALIDATOR=${CMAKE_SOURCE_DIR}/cmake/ValidateStagedPackageNotices.cmake
        -DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/package-notice-validator-ctest
        -DSPARK_BINARY_ROOT=${CMAKE_BINARY_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/TestValidateStagedPackageNotices.cmake)
set_tests_properties(LicenseInventory_PackageNoticeCoverage PROPERTIES
    LABELS "package;governance;unit"
    TIMEOUT 120)
add_test(
    NAME LicenseInventory_ThirdPartyNotices
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_third_party_notices.py)
set_tests_properties(LicenseInventory_ThirdPartyNotices PROPERTIES
    LABELS "governance;unit;python"
    TIMEOUT 300)

# RDY-010: ModuleDiscovery_* drive the production ModuleManager and the real ABI
# fixture DLL. The exact count fails closed if the family stops being compiled
# in; the contract test keeps the file from regressing to a test-local mirror.
add_test(NAME ModuleDiscoveryProductionSource COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(ModuleDiscoveryProductionSource PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=ModuleDiscovery_;SPARK_TEST_EXPECT_COUNT=7"
    LABELS "module;module-profile;stable-v1;unit"
    RUN_SERIAL TRUE
    TIMEOUT 60)
add_test(
    NAME ModuleDiscoveryProductionSourceContract
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_module_evidence_discovery_real.py)
set_tests_properties(ModuleDiscoveryProductionSourceContract PROPERTIES
    LABELS "module;module-profile;stable-v1;unit"
    TIMEOUT 30)

# RDY-010: no stable-v1/module-profile CTest that runs SparkTests may select a
# mirror-file or tautological TEST. Checks every platform branch of this file
# statically and this configured tree's own CTest inventory, plus the mutations.
add_test(
    NAME TestSourceCensus_ProfileSelectors
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_source_census_profile.py)
set_tests_properties(TestSourceCensus_ProfileSelectors PROPERTIES
    ENVIRONMENT "SPARK_CENSUS_CTEST=${CMAKE_CTEST_COMMAND};SPARK_CENSUS_BUILD_DIR=${CMAKE_BINARY_DIR};SPARK_CENSUS_CONFIG=$<CONFIG>"
    LABELS "module;readiness;unit;python"
    TIMEOUT 180)

# MOD-290: every discovered module's GameModules/<Name>/module.json must name
# existing source, assets, registered tests with live TEST prefixes, a README,
# and the parity N/A cells it declares; the mutations must each fail by name.
add_test(
    NAME ModuleManifest_Contract
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_module_manifest.py)
set_tests_properties(ModuleManifest_Contract PROPERTIES
    LABELS "module;module-kit;readiness;unit;python"
    TIMEOUT 120)

if(WIN32 AND NOT CMAKE_CROSSCOMPILING AND TARGET SparkCrashReporter)
    add_dependencies(SparkTests SparkCrashReporter)
    add_test(
        NAME CrashCapturePackageSecurity
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Tests/Tools/run_crash_capture_security.py
            --producer $<TARGET_FILE:SparkTests>
            --reporter $<TARGET_FILE:SparkCrashReporter>)
    set_tests_properties(CrashCapturePackageSecurity PROPERTIES
        LABELS "crash-security;security;integration"
        RUN_SERIAL TRUE
        TIMEOUT 180)
endif()

# CI-110: exercise the shipped SparkCooker binary against real nested assets.
# This registration is independent of Python because the smoke is a CMake
# script invoking the executable directly.
if(TARGET SparkCooker)
    if(TARGET SparkTests)
        add_dependencies(SparkTests SparkCooker)
    endif()
    add_test(
        NAME SparkCookerExecutableSmoke
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_COOKER=$<TARGET_FILE:SparkCooker>"
            "-DSPARK_COOKER_WORK_DIR=${CMAKE_BINARY_DIR}/cooker-executable-smoke/$<CONFIG>"
            -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkCookerSmoke.cmake)
    set_tests_properties(SparkCookerExecutableSmoke PROPERTIES
        LABELS "release;cooker;process"
        TIMEOUT 30)
endif()

# CI-110: execute the shipped editor against an isolated project.  This is a
# Windows-only gate because the stable-v1 editor backend is Win32/D3D11; Linux
# editor builds require an X11/GL display and are covered by the live-editor
# lane instead of reporting a misleading headless pass.
if(WIN32 AND TARGET SparkEditor)
    add_dependencies(SparkTests SparkEditor)
    add_test(
        NAME SparkEditorExecutableSmoke
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_EDITOR=$<TARGET_FILE:SparkEditor>"
            "-DSPARK_EDITOR_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_EDITOR_WORK_DIR=${CMAKE_BINARY_DIR}/editor-executable-smoke/$<CONFIG>"
            -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkEditorSmoke.cmake)
    set_tests_properties(SparkEditorExecutableSmoke PROPERTIES
        LABELS "release;editor;process"
        TIMEOUT 45)
endif()

if(Python3_Interpreter_FOUND)
    add_test(
        NAME SparkEditorPlayTruthContract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_editor_play_truth.py)
    set_tests_properties(SparkEditorPlayTruthContract PROPERTIES
        LABELS "release;editor;static;python"
        TIMEOUT 30)

    # CI-110: exercise the actual shipped Windows shader compiler binary. The
    # test is intentionally Windows-only because D3DCompiler is the only
    # integrated HLSL backend; non-Windows builds must not report a fake pass.
    if(WIN32 AND TARGET SparkShaderCompiler)
        if(TARGET SparkTests)
            add_dependencies(SparkTests SparkShaderCompiler)
        endif()
        add_test(
            NAME SparkShaderCompilerExecutableSmoke
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_SHADER_COMPILER=$<TARGET_FILE:SparkShaderCompiler>"
                "-DSPARK_SHADER_FIXTURE=${CMAKE_SOURCE_DIR}/Tests/Fixtures/ShaderCompiler/BasicVS.hlsl"
                "-DSPARK_SHADER_OUTPUT_DIR=${CMAKE_BINARY_DIR}/shader-compiler-smoke"
                -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkShaderCompilerSmoke.cmake)
        set_tests_properties(SparkShaderCompilerExecutableSmoke PROPERTIES
            LABELS "release;shader;process"
            TIMEOUT 30)
    endif()

    # CI-110: prove the shipped automation host can launch the production
    # engine and FPS module through the bounded Windows NullRHI lifecycle. The
    # direct NullRHI lifecycle test remains separate; this test specifically
    # covers SparkAutomation's process, timeout, and report contract.
    if(WIN32 AND TARGET SparkAutomation AND TARGET SparkEngine AND TARGET SparkGameFPS)
        add_dependencies(SparkTests SparkAutomation SparkEngine SparkGameFPS)
        add_test(
            NAME SparkAutomationNullRHIExecutableSmoke
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_AUTOMATION=$<TARGET_FILE:SparkAutomation>"
                "-DSPARK_ENGINE=$<TARGET_FILE:SparkEngine>"
                "-DSPARK_GAME=$<TARGET_FILE:SparkGameFPS>"
                "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
                "-DSPARK_AUTOMATION_SMOKE_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DSPARK_AUTOMATION_SMOKE_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/spark-automation-nullrhi-smoke"
                -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkAutomationNullRHISmoke.cmake)
        set_tests_properties(SparkAutomationNullRHIExecutableSmoke PROPERTIES
            LABELS "release;automation;nullrhi;process"
            RUN_SERIAL TRUE
            TIMEOUT 135)
    endif()

    add_test(
        NAME AsyncDatabaseDurabilityContract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_async_database_durability.py)
    set_tests_properties(AsyncDatabaseDurabilityContract PROPERTIES
        LABELS "persistence;durability;static;python"
        TIMEOUT 30)

    add_test(
        NAME SparkNetworkSecurityCsprngContract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_network_security_csprng.py)
    set_tests_properties(SparkNetworkSecurityCsprngContract PROPERTIES
        LABELS "network-security;security;static;python"
        TIMEOUT 30)

    # REL-100: one executable-level --version contract for every non-test
    # stable-v1 product. The script also fails if the stable-v1 build-product
    # contract gains an executable its table does not cover.
    set(_spark_stable_version_products
        SparkEngine SparkEditor SparkConsole SparkShaderCompiler SparkCrashReporter
        SparkCooker SparkAutomation SparkLauncher SparkBuild SparkInstaller)
    set(_spark_stable_version_products_built TRUE)
    foreach(_spark_product IN LISTS _spark_stable_version_products)
        if(NOT TARGET ${_spark_product})
            set(_spark_stable_version_products_built FALSE)
        endif()
    endforeach()
    if(_spark_stable_version_products_built)
        add_test(
            NAME VersionConsistency_StableProducts
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_rel100_product_versions.py
                --bin-dir $<TARGET_FILE_DIR:SparkEngine>)
        set_tests_properties(VersionConsistency_StableProducts PROPERTIES
            LABELS "release;version;integration"
            TIMEOUT 120)
    endif()
    unset(_spark_product)
    unset(_spark_stable_version_products_built)
    unset(_spark_stable_version_products)

    add_test(
        NAME BlenderMMOAssetSourceContract
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/blender/validate_mmo_props.py
            --root ${CMAKE_SOURCE_DIR})
    set_tests_properties(BlenderMMOAssetSourceContract PROPERTIES
        LABELS "asset;cpu;static;python"
        TIMEOUT 30)

    add_test(
        NAME BlenderMMOAssetGeneratorRegression
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_blender_mmo_assets.py)
    set_tests_properties(BlenderMMOAssetGeneratorRegression PROPERTIES
        LABELS "asset;cpu;unit;python"
        TIMEOUT 30)

    add_test(
        NAME SparkRunSparkTestsHarness
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test-run-spark-tests.py)
    set_tests_properties(SparkRunSparkTestsHarness PROPERTIES
        LABELS "ci;test-evidence;unit"
        TIMEOUT 30)

    # REL-110: release SBOM vulnerability gate (owned, expiring exceptions)
    # and its fail-closed wiring in release.yml.
    add_test(
        NAME ArtifactIntegrity_VulnerabilityGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_verify_vulnerability_findings.py)
    set_tests_properties(ArtifactIntegrity_VulnerabilityGate PROPERTIES
        LABELS "release;security;supply-chain;unit;python"
        TIMEOUT 60)

    # REL-110: protected stable-release approval event recorder, its consumer
    # digest binding, and the release.yml wiring.
    add_test(
        NAME ReleaseApproval_Record
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_record_release_approval.py)
    set_tests_properties(ReleaseApproval_Record PROPERTIES
        LABELS "release;security;unit;python"
        TIMEOUT 60)

    add_test(
        NAME SparkNetworkBoundaryStatic
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/check_network_boundary.py)
    set_tests_properties(SparkNetworkBoundaryStatic PROPERTIES
        LABELS "network-security;network;security;static;python"
        TIMEOUT 30)
    add_test(
        NAME SparkNetworkBoundaryMutation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_check_network_boundary.py)
    # The tracked-inventory cases spin up throwaway git checkouts, so the
    # mutation lane needs more headroom than the pure in-process cases did.
    set_tests_properties(SparkNetworkBoundaryMutation PROPERTIES
        LABELS "network-security;network;security;static;python"
        TIMEOUT 120)

    add_test(
        NAME SparkGeneratedArtifactHygiene
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_generated_artifact_hygiene.py)
    set_tests_properties(SparkGeneratedArtifactHygiene PROPERTIES
        LABELS "release;hygiene;python"
        TIMEOUT 30)

    add_test(
        NAME SparkOrchestrationProcessHarness
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/SparkDaemon/tests/test_orchestration_process_harness.py)
    set_tests_properties(SparkOrchestrationProcessHarness PROPERTIES
        LABELS "orchestration;unit;python"
        TIMEOUT 30)

    if(TARGET SparkDaemon AND TARGET SparkOrchestrator AND TARGET SparkCollabServer AND TARGET SparkAutomation)
        add_dependencies(SparkTests SparkDaemon SparkOrchestrator SparkCollabServer SparkAutomation)
        # When an engine host is available, the smoke also supervises SparkServer
        # so the supervisor's graceful-stop signal is proven against a real
        # module-owning process, not only against the collaboration broker.
        set(SPARK_ORCHESTRATION_SMOKE_HOST_ARGS)
        if(TARGET SparkServer AND TARGET SparkGame)
            add_dependencies(SparkTests SparkServer SparkGame)
            set(SPARK_ORCHESTRATION_SMOKE_HOST_ARGS
                --server $<TARGET_FILE:SparkServer>
                --game-module $<TARGET_FILE:SparkGame>
                --working-dir ${CMAKE_SOURCE_DIR})
        endif()
        add_test(
            NAME SparkOrchestrationProcessSmoke
            COMMAND $<TARGET_FILE:SparkAutomation>
                --name orchestration-process-smoke
                --executable ${Python3_EXECUTABLE}
                --working-dir ${CMAKE_SOURCE_DIR}
                --timeout-ms 180000
                --expected-exit 0
                --captured-log ${CMAKE_BINARY_DIR}/orchestration-process-smoke.log
                --json ${CMAKE_BINARY_DIR}/orchestration-process-smoke.json
                --junit ${CMAKE_BINARY_DIR}/orchestration-process-smoke.xml
                --
                ${CMAKE_SOURCE_DIR}/SparkDaemon/tests/test_orchestration_process.py
                --daemon $<TARGET_FILE:SparkDaemon>
                --orchestrator $<TARGET_FILE:SparkOrchestrator>
                --child $<TARGET_FILE:SparkCollabServer>
                ${SPARK_ORCHESTRATION_SMOKE_HOST_ARGS}
        )
        set_tests_properties(SparkOrchestrationProcessSmoke PROPERTIES
            LABELS "orchestration;integration;process"
            TIMEOUT 195)
    endif()
    if(TARGET SparkCooker AND TARGET SparkWorker AND TARGET SparkAutomation)
        add_dependencies(SparkTests SparkCooker SparkWorker SparkAutomation)
        add_test(
            NAME SparkCookerWorkerProcessSmoke
            COMMAND $<TARGET_FILE:SparkAutomation>
                --name cooker-worker-process-smoke
                --executable ${Python3_EXECUTABLE}
                --working-dir ${CMAKE_SOURCE_DIR}
                --timeout-ms 60000
                --expected-exit 0
                --captured-log ${CMAKE_BINARY_DIR}/cooker-worker-process-smoke.log
                --json ${CMAKE_BINARY_DIR}/cooker-worker-process-smoke.json
                --junit ${CMAKE_BINARY_DIR}/cooker-worker-process-smoke.xml
                --
                ${CMAKE_SOURCE_DIR}/SparkCooker/tests/test_cooker_worker_process.py
                --cooker $<TARGET_FILE:SparkCooker>
                --worker $<TARGET_FILE:SparkWorker>)
        set_tests_properties(SparkCookerWorkerProcessSmoke PROPERTIES
            LABELS "cooker;worker;integration;process"
            TIMEOUT 75)
    endif()
    if(TARGET SparkServer AND TARGET SparkGateway AND TARGET SparkGame AND
       TARGET SparkGatewayTopologyProbe AND TARGET SparkAutomation)
        if(NOT WIN32)
            get_target_property(_spark_server_exports SparkServer ENABLE_EXPORTS)
            if(NOT _spark_server_exports)
                message(FATAL_ERROR
                    "SparkServerGatewayProcessSmoke requires SparkServer ENABLE_EXPORTS on POSIX so game modules can resolve engine symbols")
            endif()
            get_target_property(_spark_engine_links SparkEngine LINK_LIBRARIES)
            if(NOT "$<LINK_LIBRARY:WHOLE_ARCHIVE,SparkEngineLib>" IN_LIST _spark_engine_links)
                message(FATAL_ERROR
                    "SparkEngine must whole-archive SparkEngineLib on POSIX so loaded game modules can resolve the complete engine surface")
            endif()
            if(APPLE)
                get_target_property(_spark_server_link_options SparkServer LINK_OPTIONS)
                if(NOT "LINKER:-export_dynamic" IN_LIST _spark_server_link_options)
                    message(FATAL_ERROR
                        "SparkServerGatewayProcessSmoke requires -export_dynamic on macOS so dynamic_lookup game modules can bind engine symbols")
                endif()
            endif()
        endif()
        add_dependencies(SparkTests SparkServer SparkGateway SparkGame SparkGatewayTopologyProbe SparkAutomation)
        add_test(
            NAME SparkServerGatewayProcessSmoke
            COMMAND $<TARGET_FILE:SparkAutomation>
                --name server-gateway-process-smoke
                --executable ${Python3_EXECUTABLE}
                --working-dir ${CMAKE_SOURCE_DIR}
                --timeout-ms 90000
                --expected-exit 0
                --captured-log ${CMAKE_BINARY_DIR}/server-gateway-process-smoke.log
                --json ${CMAKE_BINARY_DIR}/server-gateway-process-smoke.json
                --junit ${CMAKE_BINARY_DIR}/server-gateway-process-smoke.xml
                --
                ${CMAKE_SOURCE_DIR}/SparkGateway/tests/test_server_gateway_process.py
                --server $<TARGET_FILE:SparkServer>
                --gateway $<TARGET_FILE:SparkGateway>
                --module $<TARGET_FILE:SparkGame>
                --probe $<TARGET_FILE:SparkGatewayTopologyProbe>
                --working-dir ${CMAKE_SOURCE_DIR})
        set_tests_properties(SparkServerGatewayProcessSmoke PROPERTIES
            LABELS "server;gateway;integration;network;process"
            TIMEOUT 105)
    endif()
    add_test(
        NAME SparkTemplateRuntimeSheets
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Tools/validate_template_runtime_sheets.py
            ${CMAKE_SOURCE_DIR}/Templates
    )
    set_tests_properties(SparkTemplateRuntimeSheets PROPERTIES
        LABELS "template;static;python"
        TIMEOUT 60)

    # CI-110: CTest applies no timeout to a test without TIMEOUT (this project
    # never includes CTest.cmake), so one hang would stall the run instead of
    # failing. This gate keeps every registration above bounded and labelled.
    add_test(
        NAME SparkCTestTimeoutPolicy
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_validate_ctest_policy.py)
    set_tests_properties(SparkCTestTimeoutPolicy PROPERTIES
        LABELS "ci;test-policy;unit;python"
        TIMEOUT 60)

    # RDY-000: the whole fail-closed site-data/readiness contract suite (about one
    # minute locally). Its CI home is the Linux site-data workflow; it drives git
    # and POSIX path handling that no Windows lane has qualified, so it is not
    # registered there.
    if(NOT WIN32)
        add_test(
            NAME site-data-contract
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_contract.py)
        set_tests_properties(site-data-contract PROPERTIES
            LABELS "readiness;site-data;unit;python"
            TIMEOUT 300)

        # RDY-000: the fast cross-reference subset -- the checked-in contract under
        # the strict validator, dependency closure, promotion evidence, selector and
        # future-path resolution, prose ID references, and the handoff being current.
        add_test(
            NAME readiness-cross-references
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_contract.py
                LiveContractTests
                TransitiveDependencyTests
                ReadyPromotionTests
                ReadyAndPassingEvidenceTests
                SelectorResolutionTests
                FutureAcceptancePathTests
                ProseCrossReferenceTests
                GenerationAndCiTests)
        set_tests_properties(readiness-cross-references PROPERTIES
            LABELS "readiness;site-data;unit;python"
            TIMEOUT 180)
    endif()

    add_test(
        NAME SparkCliContract
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/Tools/spark-cli/tests
            -p test_*.py
            -v
    )
    set_tests_properties(SparkCliContract PROPERTIES LABELS "cli;integration" TIMEOUT 180)

    add_test(
        NAME PlatformCertification
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_platform_certification.py
            -v
    )
    set_tests_properties(PlatformCertification PROPERTIES LABELS "certification;integration" TIMEOUT 120)
    add_test(
        NAME PlatformCertMatrixOnly
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Tools/platform-cert/validate_certification.py
            --matrix-only
            --matrix ${CMAKE_SOURCE_DIR}/docs/certification/support-matrix.json
    )
    set_tests_properties(PlatformCertMatrixOnly PROPERTIES LABELS "certification" TIMEOUT 30)

    set(_spark_cli_companion_targets
        SparkServer SparkGateway SparkDaemon SparkOrchestrator
        SparkCollabServer SparkCooker SparkWorker SparkAutomation)
    set(_spark_cli_companions_available TRUE)
    foreach(_target IN LISTS _spark_cli_companion_targets)
        if(NOT TARGET ${_target})
            set(_spark_cli_companions_available FALSE)
        endif()
    endforeach()
    if(_spark_cli_companions_available)
        add_dependencies(SparkTests ${_spark_cli_companion_targets})
        add_test(
            NAME SparkCliToolInventory
            # Preset builds place companion tools under the active binary tree
            # (for example build/windows-release/bin/Release), while spark-cli's
            # source-tree fallback intentionally searches only legacy layouts.
            # Bind this inventory check to the exact directory CMake produced so
            # it validates the freshly built targets rather than stale siblings.
            COMMAND ${CMAKE_COMMAND} -E env
                "SPARKENGINE_TOOL_DIR=$<TARGET_FILE_DIR:SparkServer>"
                ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/spark-cli/spark_cli.py
                tools --config $<CONFIG> --format json)
        set_tests_properties(SparkCliToolInventory PROPERTIES LABELS "cli;integration;process" TIMEOUT 30)

        function(add_spark_cli_route_test suffix command target)
            add_test(
                NAME SparkCli${suffix}Route
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/spark-cli/spark_cli.py
                    ${command} --config $<CONFIG> --executable $<TARGET_FILE:${target}> -- --help)
            set_tests_properties(SparkCli${suffix}Route PROPERTIES LABELS "cli;integration;process" TIMEOUT 30)
        endfunction()

        add_spark_cli_route_test(Server server SparkServer)
        add_spark_cli_route_test(Gateway gateway SparkGateway)
        add_spark_cli_route_test(Daemon daemon SparkDaemon)
        add_spark_cli_route_test(Orchestrator orchestrator SparkOrchestrator)
        add_spark_cli_route_test(Collaboration collab SparkCollabServer)
        add_spark_cli_route_test(Cooker cooker SparkCooker)
        add_spark_cli_route_test(Worker worker SparkWorker)
        add_spark_cli_route_test(Automation automation SparkAutomation)
    endif()
else()
    message(WARNING "Python 3 not found; template runtime-sheet and Spark CLI validation are disabled")
endif()

# Actual FPS mutation adapters and engine event publication, without module rendering dependencies.
add_subdirectory(FPSGameplayEvents)

if(SPARK_ENABLE_FUZZ_TARGETS)
    add_subdirectory(Fuzz)
endif()
