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
    TestSDKGameTypes.cpp
    TestMathUtils.cpp
    TestObjectPool.cpp
    TestECSWorld.cpp
    TestReflectedScene.cpp
    TestReflectedSceneEmissiveHierarchy.cpp
    TestReflectedSceneCompatibility.cpp
    # SEC2: scene/JSON document trust-boundary hardening (depth, size, field, int, escape).
    TestSerializationHardening.cpp
    TestNoiseGenerator.cpp
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
    TestMMOAuthHardening.cpp
    TestMMOAuthNetwork.cpp
    TestSessionGateProtocol.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Session/MMOSessionGateProtocol.cpp"
    TestSecureRandom.cpp
    TestSparkConsoleConcurrency.cpp
    TestExecScript.cpp
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
    TestModuleVersion.cpp
    TestSparkGameShowcase.cpp
    TestLIFE200ModuleReloadReal.cpp
    TestSDK240ModuleDiagnostics.cpp
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
    TestGraphicsEngineLinuxPassTruthReal.cpp
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
    TestRHI240OpenGLGoldenReal.cpp
    TestRHI240LinuxForwardPassReal.cpp
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
    TestGLTFSkinnedMeshLoader.cpp
    TestGLTFAnimationImport.cpp
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
    TestNET100Libsodium.cpp
    TestNET100Handshake.cpp
    TestNET100TrustStore.cpp
    TestSecureTransportWired.cpp
    Fixtures/NetworkTestSecurity.cpp
    Fixtures/SecureTestPeer.cpp
    TestSessionCompatibilityReal.cpp
    TestReliableOrderedSequenceReal.cpp
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
    TestRHI230VulkanGoldenReal.cpp
    TestMSanCanary.cpp
    TestWARPRendering.cpp
    # Hardware acceleration tests
    TestGPUParticleSystem.cpp
    TestAsyncComputeScheduler.cpp
    TestGPUDrivenRenderer.cpp
    TestGPUClusterCulling.cpp
    TestDirectStorageLoader.cpp
    TestSEC3GameplayHardening.cpp
    TestSparkTerrainFormatReal.cpp
    TestMeshShaderPipeline.cpp
    TestSparkPak.cpp
    TestAssetParserHardening.cpp
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
    TestUnwiredSystemRemoval.cpp
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
    TestGamepadInputProcessing.cpp
    TestCameraTransforms.cpp
    TestSparkEngineCameraOwnership.cpp
    TestWorldServerRouting.cpp
    TestWorldServerConcurrency.cpp
    TestAudioMixerBus.cpp
    TestConsoleVariables.cpp
    TestGameObjectTransforms.cpp
    TestSceneSerializer.cpp
    TestSceneSerializerReal.cpp
    TestMusicManager.cpp
    TestAngelScriptEngine.cpp
    TestAngelScriptStackAlignmentReal.cpp
    TestENG200ScriptFaultsReal.cpp
    TestENG200ScriptBindingsReal.cpp
    TestENG200ScriptAudioAnimationReal.cpp
    TestScriptHotReload.cpp
    TestENG200ScriptHotReloadReal.cpp
    TestVRSystem.cpp
    TestSeamlessAreaManager.cpp
    TestAreaAssetLoader.cpp
    TestEventResponseSystem.cpp
    TestVisualScriptCompiler.cpp
    # SEC3: visual-script emitter depth budgets, editor script-name paths, the
    # script Transform@ handle, stale entity ids, hot-reload commit and sandbox policy lock.
    TestSEC3ScriptingHardening.cpp
    # MOD-390: the SparkGameVisualScript demo world builder (the module's real
    # OnLoad/vs_restart load, spawn and rollback path) driven against a live
    # World and AngelScriptEngine.
    TestMOD390VisualScriptDiagnosticsReal.cpp
    # MOD-390: the shipped scripts played headless to the five-pickup win, with
    # scripted key input through a real InputManager in the injected EngineContext.
    TestMOD390VisualScriptGameplayReal.cpp
    # MOD-390: engine-owned .vscript graph I/O, and the module's checked-in graphs
    # regenerate its shipped scripts byte for byte.
    TestMOD390VisualScriptGraphsReal.cpp
    # MOD-390: vs_reload hot-reloads the running demo's scripts, keeping entity state.
    TestMOD390VisualScriptHotReloadReal.cpp
    # ENG-200: the module's graphs compiled, packaged by GamePackager and played to
    # the win through RuntimePackage root resolution (shares VisualScriptGameplayHarness.h).
    TestENG200VisualScriptRuntimeReal.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoWorld.cpp"
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
    TestPrefabPersistence.cpp
    TestPrefabCompatibility.cpp
    TestEditorStateCompatibility.cpp
    TestEditorUntrustedProject.cpp
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
    TestNetTransportSecurity.cpp
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
    # Security hardening of the daemon cache services (cache-file validation, persistence)
    TestSecDaemonCacheHardening.cpp
    # Security: daemon endpoint identity (pipe naming, first-instance, client SQOS + server SID)
    TestSecDaemonPipeIdentity.cpp
    # Stats formatter used by the daemon.stats console command (Phase 6)
    TestDaemonDiagnostics.cpp
    # Comprehensive subsystem coverage
    TestGraphicsSubsystems.cpp
    TestEditorSubsystems.cpp
    TestEditorAssetReference.cpp
    TestEditorAssetDrag.cpp
    TestEditorAssetDropWorldReal.cpp
    TestEditorCookPackageReal.cpp
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
    TestMOD370RTSSaveReal.cpp
    TestPrototypeModuleKitReal.cpp
    TestGameModuleMMO.cpp
    TestGameModuleRPG.cpp
    TestGameModuleRacing.cpp
    TestMOD380RacingCompleteRaceReal.cpp
    TestMOD380VehiclePhysicsReal.cpp
    TestGameModulePlatformerARPG.cpp
    TestMOD340PlatformerCompletionReal.cpp
    TestMOD340PlatformerProgressReal.cpp
    TestMOD330ARPGDungeonReal.cpp
    TestMOD330ARPGWorldActors.cpp
    TestMOD350RPGQuestSliceReal.cpp
    TestMOD350RPGNPCNavigationReal.cpp
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
    # TF-110 forged TF_LoadoutChange decode (header-only)
    TestTFLoadoutWire.cpp
    # TF-110 tf_observe text form + tf_aim_at angles (header-only)
    TestTFObservation.cpp
    # Security review: TERRAFRONT server trust-boundary regressions (header-only
    # rules + the TFAccountSystem/TFDatabase sources listed below)
    TestTFServerSecurity.cpp
    # W5 onboarding (TFDatabase Task 1) — module .cpp added explicitly below
    # since SparkTests does not compile module sources by default.
    TestTFOnboarding.cpp
    # DATA-120 atomic multi-row character commits + schema versioning (TFDatabase above)
    TestDATA120PersistenceReal.cpp
    # DATA-120 TFDatabase backup/restore + forced-failure recovery drill
    TestDATA120BackupRestore.cpp
    # TF-120 transaction-scoped locking: shared TF_SAVE_ROOT across authorities
    TestTF120SharedSaveRoot.cpp
    # TF-120 character residency fence (TFDatabaseResidency.cpp below)
    TestTF120Residency.cpp
    TestTF120HandoffReservation.cpp
    TestPersistenceConcurrency.cpp
    # DATA-120 snapshot retries and handoff operation fences (production persistence sources below)
    TestDATA120Idempotency.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabaseResidency.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabaseBackup.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Persistence/TFPlayerMeta.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFAccountSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFCharacterSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Account/TFCrypto.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Net/TFScramWire.cpp"
    # DATA-120 / OD-22: no plaintext password, session token, or DB credential
    # at rest (TF account store above + MMO KV store + shipped config scan).
    TestDATA120SecretsAtRest.cpp
    # NET-100: SCRAM-SHA-256 login over the TF account store above
    TestTFScramAuth.cpp
    # MOD-320: MMO character IDs and per-character state across a cold restart
    # (MMOPersistenceSystem below; the character-system case needs the ImGui block).
    TestMOD320MMOPersistenceReal.cpp
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
    # SEC-120: LAN discovery beacon decode + server-list cap (pure codec, no sockets)
    TestTFLanBeaconCodec.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Game/TFLanBeaconCodec.cpp"
    # Compile-time validation that shipped project templates are real C++
    TestTemplatesCompile.cpp
    # Packaged runtimes must resolve content when launched from a foreign CWD.
    TestRuntimePackage.cpp
    TestRuntimePackageContentRoots.cpp
    # GraphicsEngine device-attach init + headless WorldBasicRenderer smoke is
    # Windows-only and is added below.
    # FPS game module source needed by TestGameModeReal
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/GameMode.cpp"
    # FPS game module sources needed by TestSparkGameFPSLoopReal / TestFPSComponentsReal /
    # TestSparkGameFPSMirrorCompanionsReal. None of the four pulls in Player or D3D11 symbols,
    # so they link into the headless test binary.
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/GameMechanicsRespawn.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FPSLocalProfile.cpp"
    # SEC2: the transactional quick-load both FPS hosts share (SEC2Persist_ tests).
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FPSQuickLoad.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FPSAssetPaths.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/ProgressionSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Core/EngineWeatherAdapter.cpp"
    # MOD-315: FPSMultiplayerProduction_ tests (TestFPSMultiplayer.cpp) drive the real
    # FPSMultiplayerSystem; it depends only on NetworkManager, ClientPrediction and SimpleConsole.
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerSystem.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerClient.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerCombat.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerNetFlow.cpp"
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FpsSnapshotCodec.cpp"
    # Editor core sources (no ImGui dependency)
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorLogger.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorCrashHandler.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorRecovery.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorDocument.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorLayoutManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/EditorWindowManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/ProjectManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/AssetPipeline/EditorAssetDrag.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/InspectorWorldAssetDrop.cpp"
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
    # SAVE-230: prefab I/O needs only SceneFile and SceneComponentCodec (above), no ImGui.
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prefabs/PrefabAsset.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prefabs/PrefabTextFormat.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prefabs/PrefabManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prefabs/PrefabManagerInstances.cpp"
    # Editor sources needed by tests
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeEditSession.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeWireCodec.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/CollaborativeEditSessionBroker.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/StandaloneCollaborationClient.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Communication/LiveEditBridge.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Workflow/EditorWorkflow.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Prototyping/PrototypingSystem.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/UIDesigner/UIDesignerSystem.cpp"
)

# The MMO account verifier seam exists only in SparkTests, which compiles both
# the account source and its structural PBKDF2-path test directly.
target_compile_definitions(SparkTests PRIVATE SPARK_TEST_MMO_AUTH_VERIFIER)

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
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
        "${CMAKE_SOURCE_DIR}/SparkSDK/Include")
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
# SDK-240 / OD-02: a real module image built against the previous SDK version.
add_library(SparkPreviousSdkModuleFixture SHARED
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

foreach(_module_fixture IN ITEMS SparkMismatchedModuleFixture SparkPreviousSdkModuleFixture
        SparkCompatibleModuleFixture SparkLegacyModuleFixture)
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
math(EXPR SPARK_TEST_PREVIOUS_SDK_VERSION
    "${SPARK_TEST_CURRENT_SDK_VERSION} - 1")
target_compile_definitions(SparkMismatchedModuleFixture PRIVATE SPARK_FIXTURE_SDK_VERSION_OFFSET=1)
target_compile_definitions(SparkPreviousSdkModuleFixture PRIVATE SPARK_FIXTURE_SDK_VERSION_OFFSET=-1)
spark_configure_module_abi(SparkMismatchedModuleFixture
    SDK_VERSION "${SPARK_TEST_MISMATCHED_SDK_VERSION}")
spark_configure_module_abi(SparkPreviousSdkModuleFixture
    SDK_VERSION "${SPARK_TEST_PREVIOUS_SDK_VERSION}")
spark_configure_module_abi(SparkCompatibleModuleFixture)
spark_configure_module_abi(SparkRegistryLifecycleModuleFixture)
spark_configure_module_abi(SparkLegacyModuleFixture)
spark_configure_module_abi(SparkSiblingDependentModuleFixture)

add_dependencies(SparkTests SparkMismatchedModuleFixture SparkPreviousSdkModuleFixture SparkCompatibleModuleFixture
    SparkRegistryLifecycleModuleFixture SparkLegacyModuleFixture
    SparkSiblingDependentModuleFixture)
target_compile_definitions(SparkTests PRIVATE
    SPARK_TEST_MISMATCHED_MODULE_PATH="$<TARGET_FILE:SparkMismatchedModuleFixture>"
    SPARK_TEST_PREVIOUS_SDK_MODULE_PATH="$<TARGET_FILE:SparkPreviousSdkModuleFixture>"
    SPARK_TEST_COMPATIBLE_MODULE_PATH="$<TARGET_FILE:SparkCompatibleModuleFixture>"
    SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH="$<TARGET_FILE:SparkRegistryLifecycleModuleFixture>"
    SPARK_TEST_LEGACY_MODULE_PATH="$<TARGET_FILE:SparkLegacyModuleFixture>"
    SPARK_TEST_SIBLING_DEPENDENT_MODULE_PATH="$<TARGET_FILE:SparkSiblingDependentModuleFixture>"
    SPARK_TEST_SIBLING_DEPENDENCY_PATH="$<TARGET_FILE:SparkModuleSiblingDependency>")

if(TARGET SparkGame)
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
# NET-100 tests call libsodium directly (RFC known-answer vectors, handshake peers).
target_link_libraries(SparkTests PRIVATE spark_sodium)

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
    target_sources(SparkTests PRIVATE TestENG220GPUSkinningD3D11Real.cpp)
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

# TF-110: the real net_* console commands drive the InstabilitySimulator.
target_sources(SparkTests PRIVATE TestNetImpairmentWiring.cpp)

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
    TestCrashArtifactRetention.cpp
    TestCrashHandlerGatingReal.cpp
    # Shared directory-link helper (NTFS junction on Windows) for the link-refusal tests.
    TestFilesystemLinks.cpp
    TestCrashSymbolication.cpp
    TestEditorCrashHandlerFilterReal.cpp
    TestEditorRecovery.cpp
    TestEngineWiringReal.cpp
    TestLifecycleCompositionRootFailure.cpp
    TestLIFE200LifecycleLoopReal.cpp
    TestLIFE200ModuleReloadLoopReal.cpp
    TestD3D11DeviceContractsReal.cpp
    TestRHI210D3D11GoldenReal.cpp
    TestRHI210D3D11PassGoldenReal.cpp
    TestRHI210D3D11ValidationReal.cpp
    TestRHI210D3D11DeviceLossReal.cpp
    TestSEC2RenderingHardeningReal.cpp
    TestRHI225D3D12ValidationReal.cpp
    TestRHI225D3D12FallbackReal.cpp
    TestRHI225D3D12ParityReal.cpp
    TestSEC4SoundEffectWav.cpp
    TestEditorGizmoTransformReal.cpp
    TestEditorProjectMaterializationReal.cpp
    TestEditorUndoHierarchyReal.cpp
    TestEditorDocumentReal.cpp
    TestEditorUndoWorldMatrixReal.cpp
    TestEDT210InspectorEditCommitReal.cpp
    TestFPSComponentsReal.cpp
    TestLoggerSinksReal.cpp
    TestModuleLifecycleReal.cpp
    TestRunnerSemanticsReal.cpp
    TestSEC100ChatAuditLogReal.cpp
    TestSEC100RemoteAdminUnavailableReal.cpp
    TestSaveSystemRoundTripReal.cpp
    TestSAVE230NewerFormatSlotReal.cpp
    TestSaveInterruptionReal.cpp
    TestDocumentInterruptionReal.cpp
    TestDocumentDurableWrite.cpp
    TestSEC2PersistenceHardening.cpp
    TestMOD360OpenWorldPersistenceReal.cpp
    TestMOD360OpenWorldTraversalReal.cpp
    # MOD-300: SparkGame showcase string tables (ShowcaseLocalization links without the module image).
    TestMOD300ShowcaseLocalizationReal.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGame/Source/Core/ShowcaseLocalization.cpp"
    TestSecurityParsersReal.cpp
    TestTextureStexBoundsReal.cpp
    TestShaderCompilerReal.cpp
    TestShadowPassReal.cpp
    TestSparkGameFPSLoopReal.cpp
    # SEC2 game-module security fixes (SEC2GM_ family, pinned below)
    TestSEC2GameModules.cpp
    TestSparkGameFPSMirrorCompanionsReal.cpp
    TestMOD310FPSSceneReloadRespawnReal.cpp
    # MOD-310: arena autopilot phases/steering (links GameMode, RespawnSystem, camera and input only).
    TestMOD310FPSArenaAutopilotReal.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FPSArenaAutopilot.cpp"
    TestAssetManifestReal.cpp
    TestFPSWeatherPort.cpp
    TestTemplateRuntimeReal.cpp
    TestUserDataPathsReal.cpp
    TestHEAD220NullRHILifetimeReal.cpp
    TestHeadlessTickStats.cpp
    TestInputFrameEdgesReal.cpp
    TestQuaternionStubsReal.cpp
    TestSceneManagerUnicodeReal.cpp
    TestSceneSaveConfinedReal.cpp
    TestWeaponMechanicsReal.cpp
    TestSEC2ConsoleIpcReal.cpp
    TestSEC4NarrowPathsReal.cpp
)

# SEC2 console-IPC security regressions (TestSEC2ConsoleIpcReal.cpp). Seven tests
# run on every platform; Windows adds two handle-inheritance tests, and POSIX
# adds two that drive the real SparkConsole binary. The pinned count fails the
# lane if a platform branch silently compiles its tests out.
set(_spark_sec2_console_count 7)
if(WIN32)
    math(EXPR _spark_sec2_console_count "${_spark_sec2_console_count} + 2")
elseif(TARGET SparkConsole)
    add_dependencies(SparkTests SparkConsole)
    target_compile_definitions(SparkTests PRIVATE
        SPARK_TEST_SPARK_CONSOLE_PATH="$<TARGET_FILE:SparkConsole>")
    math(EXPR _spark_sec2_console_count "${_spark_sec2_console_count} + 2")
endif()
add_test(NAME SEC2ConsoleIpcReal COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC2ConsoleIpcReal PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC2Console_;SPARK_TEST_EXPECT_COUNT=${_spark_sec2_console_count}"
    LABELS "security;console;process;integration"
    TIMEOUT 120)
unset(_spark_sec2_console_count)

# SEC4 reflection binary codec hardening (TestReflectionReal.cpp): the decoder
# checks type tags, never raw-copies String/Custom fields, normalizes Bool and
# reports truncation. The pinned count keeps all five regressions selected.
add_test(NAME SEC4ReflectionBinaryCodec COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC4ReflectionBinaryCodec PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC4Reflection_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "security;reflection;serialization"
    TIMEOUT 120)

# SEC4 narrow-path regressions (TestSEC4NarrowPathsReal.cpp): directory scans must
# survive a file name the Windows ANSI code page cannot spell, and the legacy packager's
# editor filter must fail closed on a name with no UTF-8 spelling. Thirteen tests are
# selected on every platform (three SKIP off Windows); the pinned count fails the lane
# if one silently drops out.
add_test(NAME SEC4NarrowPathsReal COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC4NarrowPathsReal PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC4NarrowPath_;SPARK_TEST_EXPECT_COUNT=13"
    LABELS "security;filesystem;unicode"
    TIMEOUT 120)

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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameOpenWorld/Source/Player/OWPlayerController.cpp"
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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Core/RTSPersistence.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Core/RTSPersistenceCodec.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/FogOfWar/RTSFogOfWarSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Match/RTSMatchSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Simulation/RTSScriptedCommander.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Simulation/RTSSkirmishSimulation.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRTS/Source/Navigation/RTSGridPathfinder.cpp"
    )

    # MMO module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Guild/MMOGuildSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Chat/MMOChatWire.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Inventory/MMOInventorySystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Crafting/MMOCraftingSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Trading/MMOTradingSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Player/MMOPlayerSession.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Player/MMOEntityEventCodec.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Dungeon/MMODungeonSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/World/MMOClientStateCodec.cpp"
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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Gameplay/RPGGameplayBridge.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Gameplay/RPGQuestAutopilot.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRPG/Source/Core/RPGEngineSystems.cpp"
    )

    # Racing module sources
    target_sources(SparkTests PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Vehicle/RacingVehicleSystem.cpp" "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Vehicle/RacingVehicleChassis.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Track/RacingTrackSystem.cpp" "${CMAKE_SOURCE_DIR}/GameModules/SparkGameRacing/Source/Track/RacingTrackColliders.cpp"
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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Player/PlatformerRouteRunner.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Level/PlatformerLevelSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Level/PlatformerPlatformSimulation.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Checkpoint/PlatformerCheckpointSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Collectible/PlatformerCollectibleSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Hazard/PlatformerHazardSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Core/PlatformerEngineSystems.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Core/PlatformerLevelFlow.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGamePlatformer/Source/Core/PlatformerProgress.cpp"
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
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/Demo/ARPGActorPresentation.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameARPG/Source/UI/ARPGHud.cpp"
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
        "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Profiler/PerformanceProfiler.cpp"
    )

    message(STATUS "[Tests] ImGui available — gated editor + 6 game module tests enabled")
else()
    message(STATUS "[Tests] ImGui not available — gated editor & OpenWorld tests disabled")
endif()

# Propagate engine feature defines so tests can gate on them
if(ENABLE_NETWORKING)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_NETWORKING)
    # MOD-320: use the same session gate and player/character services as the MMO module.
    # The peer is only a process host; server authority is implemented in production sources.
    set(_spark_mmo_session_sources
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Session/MMOSessionGate.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Session/MMOSessionGateWorld.cpp")
    add_executable(SparkMMOSessionGatePeer
        Fixtures/SessionGatePeer.cpp
        ${_spark_mmo_session_sources}
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Session/MMOSessionGateProtocol.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Player/MMOPlayerSession.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Player/MMOEntityEventCodec.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Persistence/MMOPersistenceSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source/Reputation/MMOReputationSystem.cpp")
    target_compile_features(SparkMMOSessionGatePeer PRIVATE cxx_std_23)
    target_include_directories(SparkMMOSessionGatePeer PRIVATE
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMO/Source")
    target_link_libraries(SparkMMOSessionGatePeer PRIVATE SparkEngineLib)
    add_dependencies(SparkTests SparkMMOSessionGatePeer)
    if(TARGET imgui)
        # MMOPlayerSystem's client input route calls the gate in the module test sources too.
        target_sources(SparkTests PRIVATE ${_spark_mmo_session_sources})
    endif()
    target_sources(SparkTests PRIVATE
        TestNetworkStack.cpp
        TestNetworkSecurity.cpp
        TestNetworkSecurityPhaseHH.cpp
        harden/Test_net-world_migration.cpp)

    # MOD-315: three-process FPSLAN loopback harness. Each peer process runs the production
    # FPSMultiplayerSystem against its own NetworkManager; TestFPSLANLoopback.cpp launches one
    # server and two clients (and, in one test, a hostile intruder) and requires both clients
    # to converge on the server's spawn-move-kill-respawn-score round.
    add_executable(SparkFPSLANLoopbackPeer
        Fixtures/FPSLANLoopbackPeer.cpp Fixtures/FPSLANIntruderTransport.cpp
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerSystem.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerClient.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerCombat.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/MultiplayerNetFlow.cpp"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Game/FpsSnapshotCodec.cpp")
    target_compile_features(SparkFPSLANLoopbackPeer PRIVATE cxx_std_23)
    target_include_directories(SparkFPSLANLoopbackPeer PRIVATE
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source")
    target_link_libraries(SparkFPSLANLoopbackPeer PRIVATE SparkEngineLib)
    add_dependencies(SparkTests SparkFPSLANLoopbackPeer)
    target_sources(SparkTests PRIVATE TestFPSLANLoopback.cpp)
    target_compile_definitions(SparkTests PRIVATE
        SPARK_TEST_FPSLAN_PEER_PATH="$<TARGET_FILE:SparkFPSLANLoopbackPeer>")
    add_test(NAME FPSLANTwoClientConvergence COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(FPSLANTwoClientConvergence PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=FPSLAN_;SPARK_TEST_EXPECT_COUNT=2"
        LABELS "fps;multiplayer;network;integration;process"
        TIMEOUT 360)
endif()
if(SPARK_JOLT_PHYSICS_AVAILABLE OR TARGET Jolt)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_PHYSICS)
    # ENG-200: script physics/event bindings against the real AngelScript VM and
    # Jolt world (TestENG200ScriptBindingsReal.cpp) plus the audio/animation
    # bindings (TestENG200ScriptAudioAnimationReal.cpp); the count pins all ten.
    if(SPARK_ANGELSCRIPT_AVAILABLE)
        add_test(NAME ScriptBindingsReal COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(ScriptBindingsReal PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=ScriptBindings_;SPARK_TEST_EXPECT_COUNT=10"
            LABELS "scripting;scripting-integration;physics;integration"
            TIMEOUT 120)
        # ENG-200: engine-owned collision/trigger dispatch from the real Jolt world
        # and EventBus to script OnCollision/OnTriggerEnter/OnTriggerExit (same file).
        add_test(NAME ScriptContactDispatchReal COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(ScriptContactDispatchReal PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=ScriptLifecycle_ENG200_Contact;SPARK_TEST_EXPECT_COUNT=5"
            LABELS "scripting;scripting-integration;physics;integration"
            TIMEOUT 120)
    endif()
endif()
# ENG-200: script playSound()/playAnimation() through the real AudioUpdateSystem and
# AnimationUpdateSystem (TestENG200ScriptAudioAnimationReal.cpp); needs no physics.
# AngelScript stack alignment: value objects (script string locals, return slots,
# a 12-byte value type) reach native code 8-byte aligned through odd dword offsets,
# stack-block growth, nested contexts, suspend/resume, exceptions and bytecode
# save/load (TestAngelScriptStackAlignmentReal.cpp; raw API on the patched core).
if(SPARK_ANGELSCRIPT_AVAILABLE)
    add_test(NAME AngelScriptStackAlignment COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(AngelScriptStackAlignment PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=AngelScriptStackAlignment_;SPARK_TEST_EXPECT_COUNT=11"
        LABELS "scripting;unit"
        TIMEOUT 120)
endif()
if(SPARK_ANGELSCRIPT_AVAILABLE)
    add_test(NAME ScriptMediaBindingsReal COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(ScriptMediaBindingsReal PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=ScriptBindings_ENG200_Media;SPARK_TEST_EXPECT_COUNT=5"
        LABELS "scripting;scripting-integration;audio;animation;integration"
        TIMEOUT 120)
    # ENG-200: AngelScriptEngine::HotReloadModule() state rules R1-R8 against real
    # .as files, and reload after a working-directory change (TestENG200ScriptHotReloadReal.cpp).
    # SEC3: script-API trust-boundary regressions against the real VM
    # (TestSEC3ScriptingHardening.cpp): a retained Transform@ never reaches another
    # entity, stale entity ids are safe, hot reload commits the staged module, and
    # the sandbox function policy is fixed once the API is registered.
    add_test(NAME SEC3ScriptHardening COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SEC3ScriptHardening PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC3Script_;SPARK_TEST_EXPECT_COUNT=5"
        LABELS "scripting;scripting-integration;security;integration"
        TIMEOUT 120)
    add_test(NAME ScriptHotReloadReal COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(ScriptHotReloadReal PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=ScriptHotReload_ENG200_;SPARK_TEST_EXPECT_COUNT=10"
        LABELS "scripting;scripting-integration;integration"
        TIMEOUT 120)
    # ENG-200: visual-script graphs -> compiled scripts -> GamePackager layout ->
    # RuntimePackage anchoring from a foreign working directory -> gameplay to the win
    # (TestENG200VisualScriptRuntimeReal.cpp).
    add_test(NAME VisualScriptRuntimePackagedLoop COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(VisualScriptRuntimePackagedLoop PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=VisualScriptRuntime_;SPARK_TEST_EXPECT_COUNT=3"
        LABELS "scripting;scripting-integration;visual-script;integration"
        TIMEOUT 180)
endif()
# SEC3: the visual-script emitter fails deeply nested control flow and long pure
# data chains with a compile error instead of overflowing the stack, bounds the
# generated source across all event bodies and function sub-graphs of one compile,
# and editor script names cannot become paths (TestSEC3ScriptingHardening.cpp; no VM needed).
add_test(NAME SEC3VisualScriptHardening COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC3VisualScriptHardening PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC3VisualScript_;SPARK_TEST_EXPECT_COUNT=6"
    LABELS "scripting;visual-script;security;unit"
    TIMEOUT 120)
# SEC4: SparkLauncher passes every child-process argument as UTF-8 (non-ASCII
# project folders no longer crash the Editor/Server/Services launch), and bad
# argument text fails closed as an error instead of an uncaught exception
# (TestLauncherProcess.cpp). The launcher's recent-project Refresh and the
# RecentProjects.json load decode stored UTF-8 paths and drop unreadable entries
# instead of throwing (TestEditorSubsystems.cpp).
add_test(NAME SEC4LauncherPaths COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC4LauncherPaths PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC4Launcher_;SPARK_TEST_EXPECT_COUNT=4"
    LABELS "launcher;security;unit"
    TIMEOUT 60)
if(SPARK_VULKAN_SUPPORT OR ENABLE_VULKAN)
    target_compile_definitions(SparkTests PRIVATE SPARK_TEST_HAS_VULKAN)
endif()
# RHI-230: the SPIR-V built from Shaders/GLSL (root CMakeLists.txt section 9.4).
# TestRHI230VulkanValidationReal.cpp loads every module from here.
if(TARGET SparkSpirvShaders)
    add_dependencies(SparkTests SparkSpirvShaders)
    target_compile_definitions(SparkTests PRIVATE
        SPARK_TEST_SPIRV_DIR="$<TARGET_FILE_DIR:SparkEngine>/Shaders/SPIRV")
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
    target_sources(SparkTests PRIVATE TestSparkServerApplication.cpp TestSparkServerHealth.cpp)
    target_link_libraries(SparkTests PRIVATE SparkServerCore)
    if(NOT WIN32)
        # SparkServerCore carries SparkEngineLib as WHOLE_ARCHIVE while other
        # dependencies link it plainly; see SparkServer/CMakeLists.txt.
        set_property(TARGET SparkTests PROPERTY
            LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,SparkEngineLib")
    endif()
endif()
if(ENABLE_NETWORKING)
    # TF-120: production participants and host dispatcher; no mirrored handoff state machine.
    target_sources(SparkTests PRIVATE
        TestHandoffParticipant.cpp
        TestTF120Migration.cpp
        "${CMAKE_SOURCE_DIR}/GameModules/SparkGameMMOFPS/Source/Net/TFHandoffParticipant.cpp")
endif()
if(TARGET SparkGatewayCore)
    target_sources(SparkTests PRIVATE
        TestSparkGatewayCoordinator.cpp
        TestGatewaySecurity.cpp
        TestGatewayAreaControl.cpp
        TestOnlineServicesLocalStack.cpp)
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
    if(TARGET SparkMMOSessionGatePeer)
        add_test(NAME MMOIntegratedWorld_TwoClientSessionGate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/TestMOD320SessionGateProcess.py
                --peer $<TARGET_FILE:SparkMMOSessionGatePeer>)
        set_tests_properties(MMOIntegratedWorld_TwoClientSessionGate PROPERTIES
            LABELS "mmo;network;security;integration;process"
            TIMEOUT 180
            # Three processes race a 2 s authentication budget around a 600000-round
            # PBKDF2; run alone so the timing does not depend on a concurrent ctest job.
            RUN_SERIAL TRUE)
    endif()
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

# Takes one test family out of the main suite because another CTest entry in this
# file runs it. Callers must sit under the same condition as that entry, so the
# family is never dropped from a build that does not register its own lane; the
# heavy families moved this way keep the main suite inside its wall-clock budget
# instead of running twice. set_tests_properties replaces ENVIRONMENT, so this
# extends the SPARK_TEST_EXCLUDE list already present rather than restating it.
function(spark_exclude_from_main_suite family_prefix)
    get_test_property(SparkEngineTests ENVIRONMENT _environment)
    if(NOT _environment MATCHES "(^|;)SPARK_TEST_EXCLUDE=")
        message(FATAL_ERROR "SparkEngineTests has no SPARK_TEST_EXCLUDE to extend with ${family_prefix}")
    endif()
    string(REGEX REPLACE "(^|;)SPARK_TEST_EXCLUDE=([^;]*)" "\\1SPARK_TEST_EXCLUDE=\\2,${family_prefix}"
        _environment "${_environment}")
    set_tests_properties(SparkEngineTests PROPERTIES ENVIRONMENT "${_environment}")
    set_property(DIRECTORY APPEND PROPERTY SPARK_MAIN_SUITE_EXCLUDED_FAMILIES "${family_prefix}")
endfunction()

# TestMain matches SPARK_TEST_EXCLUDE anywhere in a name, while the dedicated
# lanes select by name prefix. A test that contains an excluded family prefix
# without starting with it would therefore run in neither lane, so configure
# fails on one. Deferred to the end of this file so every SparkTests source added
# after this point is scanned too.
function(_spark_check_main_suite_exclusions)
    get_property(_families DIRECTORY PROPERTY SPARK_MAIN_SUITE_EXCLUDED_FAMILIES)
    if(NOT _families)
        return()
    endif()
    get_target_property(_sources SparkTests SOURCES)
    foreach(_source IN LISTS _sources)
        if(_source MATCHES "\\$<" OR NOT _source MATCHES "\\.(cpp|mm)$")
            continue()
        endif()
        cmake_path(ABSOLUTE_PATH _source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE
            OUTPUT_VARIABLE _path)
        file(STRINGS "${_path}" _lines REGEX "^[ \t]*TEST(_F)?[ \t]*\\(")
        foreach(_line IN LISTS _lines)
            # TEST(Name) registers "Name"; TEST_F(Fixture, Name) registers "Fixture.Name".
            if(NOT _line MATCHES "^[ \t]*TEST(_F)?[ \t]*\\([ \t]*([A-Za-z0-9_]+)([ \t]*,[ \t]*([A-Za-z0-9_]+))?")
                continue()
            endif()
            set(_name "${CMAKE_MATCH_2}")
            if(CMAKE_MATCH_4)
                set(_name "${CMAKE_MATCH_2}.${CMAKE_MATCH_4}")
            endif()
            foreach(_family IN LISTS _families)
                string(FIND "${_name}" "${_family}" _at)
                if(_at GREATER 0)
                    message(FATAL_ERROR
                        "${_path}: TEST ${_name} contains the main-suite exclusion '${_family}' without starting "
                        "with it, so neither SparkEngineTests nor the ${_family} prefix lane would run it.")
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()
cmake_language(DEFER CALL _spark_check_main_suite_exclusions)

# MOD-315: FPSLAN_ launches three processes and converges a full round over
# loopback (the slowest single SparkTests case); FPSLANTwoClientConvergence runs
# it under exactly the condition that creates the peer target.
if(TARGET SparkFPSLANLoopbackPeer)
    spark_exclude_from_main_suite(FPSLAN_)
endif()

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
# The llvmpipe lanes pin Mesa's software rasterizer (LIBGL_ALWAYS_SOFTWARE=1,
# GALLIUM_DRIVER=llvmpipe) so a GPU host cannot quietly run them on hardware;
# OpenGL_RHI240_DeviceRowMatchesLane fails when the context disagrees with
# SPARK_GL_EXPECT_ROW. SPARK_GL_HARDWARE_ROW (default OFF: hosted runners have no
# GPU) adds SparkOpenGLHardwareTests, the same file on a GPU-backed context.
option(SPARK_GL_HARDWARE_ROW "Register the OpenGL hardware-row lane (needs a GPU-backed GL 4.5 context)" OFF)
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
        ENVIRONMENT "SPARK_TEST_FILE=TestRHI240OpenGLReal.cpp;SPARK_TEST_EXPECT_COUNT=13;SPARK_REQUIRE_OPENGL=1;SPARK_GL_EXPECT_ROW=software;LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe"
        LABELS "opengl;llvmpipe"
        TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})

    # RHI-240 hardware row: the same 13 tests on a GPU-backed context, labelled apart
    # from llvmpipe. The row test fails if the context is a software rasterizer, so the
    # lane cannot pass on llvmpipe (for example with GALLIUM_DRIVER=llvmpipe forced).
    if(SPARK_GL_HARDWARE_ROW)
        add_test(
            NAME SparkOpenGLHardwareTests
            COMMAND ${CMAKE_COMMAND}
                -DSPARK_TEST_EXECUTABLE=$<TARGET_FILE:SparkTests>
                -DSPARK_JUNIT_REPORT=${CMAKE_BINARY_DIR}/SparkTests-opengl-hardware-junit.xml
                -DSPARK_TEST_LOG=${CMAKE_BINARY_DIR}/SparkTests-opengl-hardware.log
                -DSPARK_TEST_OUTPUT=${CMAKE_BINARY_DIR}/SparkTests-opengl-hardware-output.log
                -DSPARK_TEST_WARN_IS_ERROR=${SPARK_TESTS_WARN_IS_ERROR}
                "-DSPARK_TEST_TIMEOUT_SECONDS=$<IF:$<CONFIG:Debug>,${SPARK_TEST_TIMEOUT_DEBUG},${SPARK_TEST_TIMEOUT_OPTIMIZED}>"
                -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkTests.cmake
        )
        set_tests_properties(SparkOpenGLHardwareTests PROPERTIES
            ENVIRONMENT "SPARK_TEST_FILE=TestRHI240OpenGLReal.cpp;SPARK_TEST_EXPECT_COUNT=13;SPARK_REQUIRE_OPENGL=1;SPARK_GL_EXPECT_ROW=hardware"
            LABELS "opengl;opengl-hardware"
            RUN_SERIAL TRUE
            TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})
    endif()

    # RHI-240: shipped-shader goldens on the opengl-llvmpipe row. Renders the
    # production Shaders/GLSL programs through GLDevice and compares each frame
    # with the committed baseline under the reviewed thresholds and SHA-256 in
    # Tests/GoldenImages/manifest.json. Software-row shader evidence only, not
    # engine-pass goldens. Requires an llvmpipe context (run under xvfb-run);
    # the exact count fails the lane if a scene test stops being compiled in.
    add_test(
        NAME SparkOpenGLGoldenTests
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_TEST_EXECUTABLE=$<TARGET_FILE:SparkTests>
            -DSPARK_JUNIT_REPORT=${CMAKE_BINARY_DIR}/SparkTests-opengl-golden-junit.xml
            -DSPARK_TEST_LOG=${CMAKE_BINARY_DIR}/SparkTests-opengl-golden.log
            -DSPARK_TEST_OUTPUT=${CMAKE_BINARY_DIR}/SparkTests-opengl-golden-output.log
            -DSPARK_TEST_WARN_IS_ERROR=${SPARK_TESTS_WARN_IS_ERROR}
            "-DSPARK_TEST_TIMEOUT_SECONDS=$<IF:$<CONFIG:Debug>,${SPARK_TEST_TIMEOUT_DEBUG},${SPARK_TEST_TIMEOUT_OPTIMIZED}>"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkTests.cmake
    )
    set_tests_properties(SparkOpenGLGoldenTests PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestRHI240OpenGLGoldenReal.cpp;SPARK_TEST_EXPECT_COUNT=8;SPARK_REQUIRE_OPENGL=1;SPARK_GOLDEN_OUTPUT_DIR=${CMAKE_BINARY_DIR}/Tests/Output;LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe"
        LABELS "opengl;llvmpipe;opengl-golden"
        TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})
    # The baselines belong to one reviewed Mesa build, so exactly one lane
    # compares them: the main suite would otherwise repeat the comparison in
    # every Linux build, sanitizer and coverage run. The tests are compiled only
    # with SPARK_OPENGL_SUPPORT, so the registration floor does not count them.
    spark_exclude_from_main_suite(OpenGLGolden_)

    # RHI-240: the production SDL2 host must honor SPARK_RHI_BACKEND=opengl
    # (initialize OpenGL, never NullRHI) and must exit non-zero when that
    # explicit request cannot be met. PLT-210: it also runs SparkGameFPS on the
    # OpenGL window for 30 frames, and a windowed run whose window cannot be
    # created must exit non-zero rather than continue on NullRHI. Uses DISPLAY
    # or wraps itself in xvfb-run; having neither fails the lane rather than
    # skipping it.
    if(SPARK_PACKAGE_HAS_SDL2 AND TARGET SparkEngine)
        find_program(SPARK_XVFB_RUN_EXECUTABLE xvfb-run)
        add_dependencies(SparkTests SparkEngine)
        set(_spark_opengl_startup_game_arg)
        if(TARGET SparkGameFPS)
            add_dependencies(SparkTests SparkGameFPS)
            set(_spark_opengl_startup_game_arg "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGameFPS>")
        endif()
        add_test(
            NAME SparkEngineExplicitOpenGLStartup
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
                "-DSPARK_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/opengl-explicit-startup"
                "-DSPARK_XVFB_RUN=${SPARK_XVFB_RUN_EXECUTABLE}"
                ${_spark_opengl_startup_game_arg}
                -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkOpenGLExplicitStartup.cmake
        )
        set_tests_properties(SparkEngineExplicitOpenGLStartup PROPERTIES
            LABELS "opengl;llvmpipe;linux;sdl2;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 240)
    endif()

    # RHI-240: the Linux forward draw-list pass (GraphicsEngine::ProcessDrawList) renders a
    # submitted cube through its bound pipeline on a real SDL2 OpenGL window on llvmpipe, read
    # back from the back buffer. The window needs an X server, so the lane always runs under
    # xvfb-run when it exists; without one SDL_Init fails and SPARK_REQUIRE_OPENGL=1 fails the
    # lane instead of skipping it. Software-rasterizer forward-pass evidence only.
    if(SPARK_PACKAGE_HAS_SDL2)
        find_program(SPARK_XVFB_RUN_EXECUTABLE xvfb-run)
        set(_spark_forward_pass_display)
        if(SPARK_XVFB_RUN_EXECUTABLE)
            set(_spark_forward_pass_display "${SPARK_XVFB_RUN_EXECUTABLE}" -a -s "-screen 0 1280x720x24")
        endif()
        add_test(NAME LinuxForwardPassOpenGL
            COMMAND ${_spark_forward_pass_display} $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(LinuxForwardPassOpenGL PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=LinuxForwardPass_OpenGL;SPARK_TEST_EXPECT_COUNT=1;SPARK_REQUIRE_OPENGL=1;LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe;SDL_AUDIODRIVER=dummy"
            LABELS "opengl;llvmpipe"
            RUN_SERIAL TRUE
            TIMEOUT 120)
        spark_exclude_from_main_suite(LinuxForwardPass_OpenGL)
    endif()
endif()
add_test(NAME SparkSaveCompatibilityTests COMMAND $<TARGET_FILE:SparkTests> --quiet)
set_tests_properties(SparkSaveCompatibilityTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=SaveMigration_"
    LABELS "compatibility;save;unit"
    RUN_SERIAL TRUE
    TIMEOUT 30)

# SAVE-230: AtomicWrite_ SIGKILLs an exec'd SparkTests writer child at seeded, randomized
# offsets while it saves successive generations through the real SaveSystem, then requires
# the slot to load the last completed (or in-flight) generation with a valid CRC, an intact
# .bak and no promoted .tmp; two deterministic AtomicWrite_ tests pin the staged .bak refresh.
# POSIX only; Windows needs a TerminateProcess variant.
if(NOT WIN32)
    add_test(NAME SparkSaveInterruptionTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SparkSaveInterruptionTests PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=AtomicWrite_;SPARK_TEST_EXPECT_COUNT=3"
        LABELS "compatibility;save;process"
        TIMEOUT 300)
endif()

# SEC-assets: untrusted asset-parser regressions (SparkPak TOC binding and bounds,
# the repo-authored stb_image stub BMP/TGA decoders, OBJ face-index validation,
# the OBJ static-mesh size cap and mtllib confinement, the Windows TGA texture
# loader and the non-Windows EXR and undecodable-file texture paths). Tests that
# only apply to one platform still register everywhere, so the count is fixed.
add_test(NAME SparkAssetParserHardeningTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkAssetParserHardeningTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=AssetSec_;SPARK_TEST_EXPECT_COUNT=15"
    LABELS "security;assets;unit"
    TIMEOUT 120)

# SAVE-230: the whole-document writer behind prefab and project saves keeps the
# destination and its retained .bak byte-identical on failure and refreshes the
# .bak by staged rename. Not AtomicWrite_: that prefix is pinned to 3 above.
add_test(NAME SparkDocumentWriteTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkDocumentWriteTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=DocumentWrite_;SPARK_TEST_EXPECT_COUNT=4"
    LABELS "compatibility;save;unit"
    TIMEOUT 30)

# SAVE-230: DocumentInterruption_ kills an exec'd SparkTests writer (SIGKILL on POSIX,
# TerminateProcess on Windows) at seeded, randomized offsets while it saves successive
# generations of a scene (SaveWorld), a prefab (PrefabAsset::Save) and a project document
# (ProjectManager), then requires the production loader to read exactly the last completed
# or in-flight generation, an intact .bak and no promoted .tmp. The family spawns ~300
# child processes, so it runs only in this lane, not in the SparkEngineTests budget.
add_test(NAME SparkDocumentInterruptionTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkDocumentInterruptionTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=DocumentInterruption_;SPARK_TEST_EXPECT_COUNT=3"
    LABELS "compatibility;save;process"
    RUN_SERIAL TRUE
    TIMEOUT 600)
get_test_property(SparkEngineTests ENVIRONMENT _spark_main_suite_environment)
string(REGEX REPLACE "(^|;)SPARK_TEST_EXCLUDE=([^;]*)" "\\1SPARK_TEST_EXCLUDE=\\2,DocumentInterruption_"
    _spark_main_suite_environment "${_spark_main_suite_environment}")
set_tests_properties(SparkEngineTests PROPERTIES ENVIRONMENT "${_spark_main_suite_environment}")
unset(_spark_main_suite_environment)

# Security: JobSystem worker counts never underflow on a 0-thread host, operator-supplied
# counts (-threads, SPARK_MAX_WORKER_THREADS) are parsed without atoi UB and capped, and a
# partial worker start joins every started thread instead of leaving std::terminate behind.
add_test(NAME SparkJobSystemHardeningTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkJobSystemHardeningTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=JobSystemHardening_;SPARK_TEST_EXPECT_COUNT=3"
    LABELS "security;core;unit"
    TIMEOUT 30)

# Security: each ECS phase system runs under its own fault-isolator key, so one throwing
# system is disabled alone and the later phases keep running instead of the whole
# "ECS_Phases" pipeline being skipped and then disabled.
add_test(NAME SparkPhaseFaultIsolationTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkPhaseFaultIsolationTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=PhaseFaultIsolation_;SPARK_TEST_EXPECT_COUNT=1"
    LABELS "security;ecs;unit"
    TIMEOUT 30)

# Security: FileUtils / LocalFileCache treat engine path strings as UTF-8 on Windows
# instead of decoding them in the active ANSI code page (missed files, writes to a
# mojibake-named sibling); legacy code-page strings keep their native meaning.
add_test(NAME SparkUtf8PathTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkUtf8PathTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=Utf8Path_;SPARK_TEST_EXPECT_COUNT=3"
    LABELS "security;filesystem;unit"
    TIMEOUT 30)

# SEC2: persistence hardening. Staging files are created exclusively and never through a
# planted link (SaveFileDurability, SaveSystem, AsyncDatabase); a rename is never reported
# uncommitted; the KV store rolls back unpublished writes, never publishes a revision over the budget its loader enforces, refuses damaged files and admits
# one authority; SaveSystem keeps every RigidBody/Light field and reports partial deletes;
# the FPS quick-load validates the profile before replacing the world. The two POSIX
# directory-sync cases SKIP on Windows.
add_test(NAME SparkSEC2PersistenceTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkSEC2PersistenceTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC2Persist_;SPARK_TEST_EXPECT_COUNT=13"
    LABELS "persistence;save;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 60)

# SAVE-230: editor prefabs fail closed on a newer SPARKPREFAB version, name the
# component and line of damage, recover from the retained .bak, and a failed save
# leaves the previous prefab byte-identical. PrefabManager saves to and reloads
# from <project>/Prefabs and never writes into the working directory. Project
# prefabs are untrusted: file size, symlinks, file count and total bytes are bounded.
add_test(NAME SparkPrefabPersistenceTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkPrefabPersistenceTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=PrefabPersistence_;SPARK_TEST_EXPECT_COUNT=12"
    LABELS "compatibility;prefab;unit"
    TIMEOUT 60)

# SAVE-230: editor prefabs read SPARKPREFAB 1 (N-1) and 2 (N) and write 2 only. The
# committed v1 fixture loads in place with its declared values and is never rewritten,
# re-saves as v2 bit-exactly, and the v3 fixture fails closed
# (Tests/Fixtures/Compatibility/Prefab).
add_test(NAME SparkPrefabCompatibilityTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkPrefabCompatibilityTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=PrefabMigration_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "compatibility;prefab;unit"
    TIMEOUT 30)

# SAVE-230: the .sparkproject document reads and writes projectFileVersion 1, fails
# closed on a newer version, keeps the previous document as .bak on save and
# recovers a damaged document from it (Tests/Fixtures/Compatibility/EditorState).
# Editor layouts read version 1 (and the unversioned legacy dialect) in place and
# fail closed on version 2 without applying any panel (EditorState/Layouts).
add_test(NAME SparkEditorStateCompatibilityTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkEditorStateCompatibilityTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=EditorStateMigration_;SPARK_TEST_EXPECT_COUNT=8"
    LABELS "compatibility;editor;unit"
    TIMEOUT 60)

# SAVE-230 / OD-03: SceneFile reads v2 (N) and v1 (N-1) from committed fixtures,
# writes v2 only, and rejects older/newer versions with a versioned error. The
# reflected-World dialect (TestReflectedSceneCompatibility.cpp) loads its committed
# version 1 and legacy editor sceneVersion 1 fixtures in place, resaves version 1
# only, and fails closed on a newer-version fixture.
add_test(NAME SparkSceneCompatibilityTests COMMAND $<TARGET_FILE:SparkTests> --empty-is-error)
set_tests_properties(SparkSceneCompatibilityTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=SceneMigration_;SPARK_TEST_EXPECT_COUNT=7"
    LABELS "compatibility;scene;unit"
    TIMEOUT 30)

# SEC2 scene-serialization lane: the nlohmann stub bounds nesting depth, reflected
# scene loads cap file size and reject damaged current-version fields (recovering
# the .bak), Json AsInt/TryAsInt never convert an out-of-range double, mod
# loadOrder is validated, and ParseStrict rejects undefined escapes/control bytes.
# The stub also bounds value count (breadth), and SaveWorld refuses a world the
# reader would reject (NaN/Inf field, over the byte cap) instead of writing it.
add_test(NAME SparkSerializationHardeningTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(SparkSerializationHardeningTests PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SerializationHardening_;SPARK_TEST_EXPECT_COUNT=9"
    LABELS "security;scene;unit"
    TIMEOUT 60)

add_test(NAME TelemetrySpool COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(TelemetrySpool PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=Telemetry_SpoolRecovery;SPARK_TEST_EXPECT_COUNT=10"
    LABELS "telemetry-integration"
    RUN_SERIAL TRUE
    TIMEOUT 30)

# SEC3 gameplay-systems hardening: bounded DirectStorage fallback worker pool, navmesh and
# replay counts checked against the bytes left in the file before any allocation, replay
# versions, durations, frame order and entity/event values validated before commit, navmesh
# triangle indices and adjacency links checked against the mesh they index, a
# linear size-capped localization parser, script-bearing mods refused instead of reported
# Active, symlinked or oversized mod manifests refused with a bounded read, junctioned or
# linked mod directories refused, and mod folder names outside the ANSI code page scanned.
add_test(NAME SparkSEC3GameplayHardening COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkSEC3GameplayHardening PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC3Gameplay_;SPARK_TEST_EXPECT_COUNT=18"
    LABELS "security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 120)

# SEC-120 runtime .sparkterrain decoder: non-square heightmaps (which made the clipmap index
# resolution^2 samples from a width*height map) are rejected, and the editor layout round-trips.
add_test(NAME SparkTerrainFormat COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SparkTerrainFormat PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SparkTerrainFormat_;SPARK_TEST_EXPECT_COUNT=2"
    LABELS "security;unit"
    TIMEOUT 60)

# OPS-100: retired crash-transport credentials are dropped on settings load and never saved.
add_test(NAME CrashSettingsCredentials COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(CrashSettingsCredentials PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=CrashSettings_;SPARK_TEST_EXPECT_COUNT=1"
    LABELS "crash-security;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 30)

# SEC2 finding 15: stale spark_crash_* artifact directories are pruned by an
# owner-verified age/count/byte retention policy before a new one is created.
add_test(NAME CrashArtifactRetention COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(CrashArtifactRetention PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=CrashRetention_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "crash-security;security;unit"
    TIMEOUT 30)

# SEC4: the shared directory-link helper creates a real NTFS junction on Windows
# (no privilege needed) and a symlink elsewhere, so the link-refusal tests that use
# it run on every host instead of skipping where create_symlink needs Developer Mode.
add_test(NAME FilesystemLinkHelper COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(FilesystemLinkHelper PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=FsLinkHelper_;SPARK_TEST_EXPECT_COUNT=2"
    LABELS "security;unit"
    TIMEOUT 30)

# OPS-100: signal-path build-id records (bounded ELF note parsing, fixed-buffer
# SYMBOLIC FRAMES section) that tools/ops/symbolicate_crash.py consumes.
add_test(NAME CrashSymbolicationRecords COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(CrashSymbolicationRecords PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=CrashSymbolication_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "crash-canary;unit"
    TIMEOUT 30)

# DATA-120 (OD-22): no plaintext password, session token, or database credential
# reaches a persistence store or a shipped config file.
add_test(NAME PersistenceSecretsAtRest COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(PersistenceSecretsAtRest PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestDATA120SecretsAtRest.cpp;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "persistence;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 60)

# NET-100: TERRAFRONT login proves the password with SCRAM-SHA-256 (RFC 7677 vector, replay,
# truncation, legacy PBKDF2 row migration, unknown-user salts, verifier policy floor and ceiling at
# intake and when a stored row is loaded, challenges bound to their connection, expiry and the global
# pending-challenge bound).
add_test(NAME TFScramAuth COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(TFScramAuth PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=TFScram_;SPARK_TEST_EXPECT_COUNT=11"
    LABELS "network-security;persistence;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 120)
# Each TFScram_ case runs PBKDF2 at the production iteration floor (about 10 s for
# the family in Release), so it runs only in the lane above, not twice.
spark_exclude_from_main_suite(TFScram_)

# SEC-120: the TERRAFRONT LAN beacon decoder rejects wrong size, magic, version and
# port, bounds unterminated wire names, and caps the scanner list a spoofing peer fills.
add_test(NAME TFLanBeaconCodec COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(TFLanBeaconCodec PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestTFLanBeaconCodec.cpp;SPARK_TEST_NAME_PREFIX=TFLanBeacon_;SPARK_TEST_EXPECT_COUNT=3"
    LABELS "network-security;security;unit"
    TIMEOUT 60)

# Unwired-system removal: MaterialSystem/Material expose no file import, export, reload or
# hot-reload path, and LifecycleSystem latches deaths (no unregistered callback) with
# HealthComponent::SetHealth re-arming the latch on respawn.
add_test(NAME UnwiredSystemRemoval COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(UnwiredSystemRemoval PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=UnwiredSys_;SPARK_TEST_EXPECT_COUNT=3"
    LABELS "rendering;security;unit"
    TIMEOUT 60)

# DATA-120: TFDatabase multi-row transactions, schema migration, the TERRAFRONT
# durable-commit primitive (SavePaths::WriteDurableReplace), concurrent writers (Persistence_Concurrency_),
# and the real AsyncDatabase KV flush run under the persistence label, so `ctest -L persistence`
# executes the C++ evidence and not only the static Python durability contract.
# Counts are exact per family: a dropped or renamed case fails its CTest instead
# of shrinking the evidence. RUN_SERIAL because the fixtures reuse fixed paths
# (Saves/test_data120_*.db, <tmp>/spark_save_roundtrip_kv_store) that SparkEngineTests
# also writes, so a parallel ctest run would let one process delete the other's files.
# Persistence_Durable_DirectorySyncFailureAfterRenameStillReportsCommit is POSIX-only
# (it skips, but still counts, as root), so Windows builds register one case fewer.
if(WIN32)
    set(_spark_data120_durable_count 3)
else()
    set(_spark_data120_durable_count 4)
endif()
set(_spark_data120_persistence_selectors
    "Persistence_Transaction_=3"
    "Persistence_Migration_=11"
    "Persistence_Durable_=${_spark_data120_durable_count}" "Persistence_Concurrency_=6"
    "Persistence_Idempotency_=6"
    "AsyncDatabaseReal_=1")
foreach(_spark_data120_selector IN LISTS _spark_data120_persistence_selectors)
    string(REPLACE "=" ";" _spark_data120_parts "${_spark_data120_selector}")
    list(GET _spark_data120_parts 0 _spark_data120_prefix)
    list(GET _spark_data120_parts 1 _spark_data120_count)
    string(REGEX REPLACE "_$" "" _spark_data120_name "${_spark_data120_prefix}")
    add_test(NAME ${_spark_data120_name} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(${_spark_data120_name} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=${_spark_data120_prefix};SPARK_TEST_EXPECT_COUNT=${_spark_data120_count}"
        LABELS "persistence;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endforeach()

# DATA-120: TFDatabase backup/restore and the forced-failure recovery drill run
# under persistence;recovery-drill, so `ctest -L recovery-drill` rehearses a
# verified backup, a restore, and reopening after a writer dies inside a commit.
# The drill cases exec a fresh SparkTests child that _exit()s mid-commit or is
# killed (SIGKILL / TerminateProcess), on every platform: 4 TFDatabase drills
# plus outfit (2), social (2) and territory (1) store drills.
set(_spark_data120_recovery_selectors "Persistence_BackupRestore_=7" "Persistence_RecoveryDrill_=9")
foreach(_spark_data120_selector IN LISTS _spark_data120_recovery_selectors)
    string(REPLACE "=" ";" _spark_data120_parts "${_spark_data120_selector}")
    list(GET _spark_data120_parts 0 _spark_data120_prefix)
    list(GET _spark_data120_parts 1 _spark_data120_count)
    string(REGEX REPLACE "_$" "" _spark_data120_name "${_spark_data120_prefix}")
    add_test(NAME ${_spark_data120_name} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(${_spark_data120_name} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=${_spark_data120_prefix};SPARK_TEST_EXPECT_COUNT=${_spark_data120_count}"
        LABELS "persistence;recovery-drill;unit"
        RUN_SERIAL TRUE
        TIMEOUT 120)
endforeach()

# TF-120: a character is in world on at most one continent authority
# (TFDatabase residency fence, TFCharacterSystem enter/leave world, parked
# meta keeping residency, disconnect durability, delete/bind refusals). RUN_SERIAL: fixed Saves/ paths.
# The three forced-crash cases SIGKILL a peer SparkTests process and exist on
# POSIX only, so Windows registers three fewer.
if(WIN32)
    set(_spark_tf120_residency_count 16)
else()
    set(_spark_tf120_residency_count 19)
endif()
add_test(NAME TF120_Residency COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(TF120_Residency PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=TF120_Residency_;SPARK_TEST_EXPECT_COUNT=${_spark_tf120_residency_count}"
    LABELS "persistence;unit"
    RUN_SERIAL TRUE
    TIMEOUT 120)

add_test(NAME TerrafrontMigration_Reservation COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(TerrafrontMigration_Reservation PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=TF120_HandoffReservation_;SPARK_TEST_EXPECT_COUNT=3"
    LABELS "persistence;terrafront-migration"
    RUN_SERIAL TRUE
    TIMEOUT 120)
if(ENABLE_NETWORKING)
    add_test(NAME TerrafrontMigration_Participant COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(TerrafrontMigration_Participant PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=TF120_Migration_;SPARK_TEST_EXPECT_COUNT=6"
        LABELS "persistence;terrafront-migration"
        RUN_SERIAL TRUE
        TIMEOUT 240)
    add_test(NAME TerrafrontMigration_Dispatcher COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(TerrafrontMigration_Dispatcher PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestHandoffParticipant.cpp;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "network;terrafront-migration"
        RUN_SERIAL TRUE
        TIMEOUT 30)
endif()
if(TARGET SparkGatewayCore)
    # Authenticated, epoch-fenced area-control frames are the only way into the game-thread participant.
    add_test(NAME TerrafrontMigration_GatewayDispatch COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(TerrafrontMigration_GatewayDispatch PROPERTIES
        ENVIRONMENT
            "SPARK_TEST_NAME=GatewayAreaControl_DispatchesOnlyAuthenticatedFencedPhasesToParticipant;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "gateway;security;terrafront-migration"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()

# DATA-120 / TF-120: authority processes sharing one TF_SAVE_ROOT database. The
# two spawned-peer cases (TF120PeerProcess.h) run on every platform, so the
# count is the same everywhere. A peer that cannot spawn SKIPs its case, which
# the runner does not count as a failure, so the skip fails the CTest here.
# RUN_SERIAL: fixed Saves/ paths, and on Windows the peers inherit a scoped
# copy of this process's environment.
add_test(NAME TF120_SharedRoot COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(TF120_SharedRoot PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=TF120_SharedRoot_;SPARK_TEST_EXPECT_COUNT=8"
    FAIL_REGULAR_EXPRESSION "cannot spawn a peer;[0-9]+ skipped"
    LABELS "persistence;unit"
    RUN_SERIAL TRUE
    TIMEOUT 240)

# Security: SparkDaemon shader/asset cache hardening. The pinned count fails the
# lane if the family silently stops being compiled in.
add_test(NAME SecDaemonCacheHardening COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SecDaemonCacheHardening PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SecDaemonCache_;SPARK_TEST_EXPECT_COUNT=8"
    LABELS "daemon;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 120)

# Security: SparkDaemon endpoint identity. Two tests run on every platform;
# Windows adds three named-pipe tests (per-path pipe names, client SQOS at
# identification level, server-SID check). The pinned count fails the lane if
# a platform branch silently compiles its tests out.
set(_spark_sec_daemon_pipe_count 2)
if(WIN32)
    math(EXPR _spark_sec_daemon_pipe_count "${_spark_sec_daemon_pipe_count} + 3")
endif()
add_test(NAME SecDaemonPipeIdentity COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SecDaemonPipeIdentity PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SecDaemonPipe_;SPARK_TEST_EXPECT_COUNT=${_spark_sec_daemon_pipe_count}"
    LABELS "daemon;security;unit"
    RUN_SERIAL TRUE
    TIMEOUT 120)
unset(_spark_sec_daemon_pipe_count)

# OPS-110: SparkServer operator health (build identity, tick percentiles, RSS, drain ordering).
if(TARGET SparkServerCore)
    add_test(NAME ServerHealth COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(ServerHealth PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=Server_Health_;SPARK_TEST_EXPECT_COUNT=8"
        LABELS "server;observability"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()
# OPS-110: NetworkManager queue depth/peak published through GetStats (the soak's queue-growth signal).
if(ENABLE_NETWORKING)
    add_test(NAME NetQueueMetrics COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(NetQueueMetrics PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=NetQueueMetrics_;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "network;observability;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()

# NET-100: SecureChannel (ChaCha20-Poly1305) transport families run under the
# network-security label, so `ctest -L network-security` executes the C++ suite and
# not only the static Python checks. Counts are exact per family: a dropped or
# renamed case fails its CTest instead of shrinking the evidence. This covers
# SecureChannel in isolation; it is not evidence of production wiring.
set(_spark_net100_transport_selectors
    "Transport_KnownAnswer_=2"
    "Transport_Tamper_=3"
    "Transport_Replay_=4"
    "Transport_Downgrade_=2"
    "Transport_KeyRotation_=3"
    "Transport_FuzzPacket_=1"
    "Transport_KeyGeneration_=1"
    "Transport_Libsodium_=1"
    "Transport_Handshake_=11"
    "Transport_RateLimit_=1"
    "Transport_TrustStore_=6")
foreach(_spark_net100_selector IN LISTS _spark_net100_transport_selectors)
    string(REPLACE "=" ";" _spark_net100_parts "${_spark_net100_selector}")
    list(GET _spark_net100_parts 0 _spark_net100_prefix)
    list(GET _spark_net100_parts 1 _spark_net100_count)
    string(REGEX REPLACE "_$" "" _spark_net100_name "${_spark_net100_prefix}")
    add_test(NAME NetworkSecurity_${_spark_net100_name} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(NetworkSecurity_${_spark_net100_name} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=${_spark_net100_prefix};SPARK_TEST_EXPECT_COUNT=${_spark_net100_count}"
        LABELS "network-security;security;unit"
        TIMEOUT 30)
endforeach()

# NET-100: protocol-version negotiation in Connect/ConnectAccepted over real loopback UDP.
# The server must reject missing/older/newer/malformed handshakes with a typed ConnectRejected
# before allocating a slot, and the client must refuse a mismatched ConnectAccepted echo.
if(ENABLE_NETWORKING)
    add_test(NAME NetworkSessionCompatibility COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(NetworkSessionCompatibility PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestSessionCompatibilityReal.cpp;SPARK_TEST_EXPECT_COUNT=11"
        LABELS "network-security;network;security;unit"
        TIMEOUT 60)
    # Reliable and ReliableOrdered traffic to one peer: the ordered stream must be numbered on its
    # own, so ordered messages after any Reliable message (a client's ClientFinished, MMO chat) are
    # still delivered in order exactly once -- interleaved, lost/reordered/duplicated, and across a
    # reconnect. Production sender and receiver over loopback UDP, replayed through a raw peer.
    if(TARGET imgui)
        set(_spark_reliable_ordered_count 6) # + ReliableOrderedSeq_MMOChatFromClientReachesServer
    else()
        set(_spark_reliable_ordered_count 5)
    endif()
    add_test(NAME NetworkReliableOrderedSequence COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(NetworkReliableOrderedSequence PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestReliableOrderedSequenceReal.cpp;SPARK_TEST_EXPECT_COUNT=${_spark_reliable_ordered_count}"
        LABELS "network;unit"
        TIMEOUT 60)
    # NET-100: the v2 handshake and sealed datagrams through the production NetworkManager over
    # loopback UDP: wrong/tampered/downgraded server identity, truncated or suite-downgraded
    # ClientHello, missing ClientFinished, tamper/replay/reorder/wrong-key/truncation of sealed
    # frames, sensitive-without-channel refusal, send-key rotation, and packet captures that must
    # contain no player name, chat text or credential bytes. Also: a rewritten ConnectAccepted
    # client id is refused (signed prefix), a small-order-key hello flood does no crypto work, the
    # per-source Connect budget drops excess hellos unanswered, and a Securing slot receives nothing
    # but its ConnectAccepted.
    add_test(NAME NetworkSecureTransportWired COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(NetworkSecureTransportWired PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestSecureTransportWired.cpp;SPARK_TEST_EXPECT_COUNT=20"
        LABELS "network-security;network;security"
        TIMEOUT 120)
endif()

# NET-110: IOnlinePlatform conformance (docs/specs/online-services.md 5.1, 6) across
# every adapter in OnlineServices.h (Null, Steam, Epic, Console) plus the Null
# adapter's fixed-transcript determinism check. Exact count: a dropped adapter test fails.
add_test(NAME OnlineServicesContract COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(OnlineServicesContract PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=OnlineServices_Contract_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "online-services;unit"
    TIMEOUT 30)

# NET-110: degraded-dependency semantics in OnlineServiceManager (docs/specs/online-services.md
# 5.1): adapter exceptions become failures, per-capability circuit breaker (5 failures, 30 s,
# one probe), 5 ms per-call budget accounting (optionally tripping the circuit), state reported
# by Console_GetStatus. Exact count: a dropped test fails.
add_test(NAME OnlineServicesDegraded COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(OnlineServicesDegraded PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=OnlineServices_Degraded_;SPARK_TEST_EXPECT_COUNT=11"
    LABELS "online-services;unit"
    TIMEOUT 30)

# TF-110: TERRAFRONT server trust boundary and harness probes. Forged
# loadout/fire state is rejected and audited (TFServerValidation +
# Net/TFLoadoutWire.h); tf_observe output is sorted and quantized
# (Game/TFObservation.h). Exact counts per family.
set(_spark_tf110_selectors
    "TF110_ForgedState_=4"
    "TF110_LoadoutWire_=3"
    "TF110_Observe_=4"
    "TFSec_=9")
foreach(_spark_tf110_selector IN LISTS _spark_tf110_selectors)
    string(REPLACE "=" ";" _spark_tf110_parts "${_spark_tf110_selector}")
    list(GET _spark_tf110_parts 0 _spark_tf110_prefix)
    list(GET _spark_tf110_parts 1 _spark_tf110_count)
    string(REGEX REPLACE "_$" "" _spark_tf110_name "${_spark_tf110_prefix}")
    add_test(NAME ${_spark_tf110_name} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(${_spark_tf110_name} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=${_spark_tf110_prefix};SPARK_TEST_EXPECT_COUNT=${_spark_tf110_count}"
        LABELS "terrafront;unit"
        TIMEOUT 30)
endforeach()

# TF-110: network impairment reaches the wire. NetImpairmentWiring pins the
# net_* console -> EngineSettings -> InstabilitySimulator bridge and seeded
# reproducibility; NetImpairmentTransport pins per-destination delay,
# duplication and reordering of server unicast over real loopback UDP.
add_test(NAME NetImpairmentWiring COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(NetImpairmentWiring PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestNetImpairmentWiring.cpp;SPARK_TEST_EXPECT_COUNT=7"
    LABELS "network;unit"
    RUN_SERIAL TRUE
    TIMEOUT 30)
if(ENABLE_NETWORKING)
    add_test(NAME NetImpairmentTransport COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(NetImpairmentTransport PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestNetworkManagerIntegration.cpp;SPARK_TEST_NAME_PREFIX=NetImpairment_;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "network;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()

# SEC-net-transport: transport hardening regressions over real loopback UDP
# (input retention, client state cleanup, session termination, full-sync
# throttling, reliable sequence wrap, client placeholder cap).
if(ENABLE_NETWORKING)
    add_test(NAME NetTransportSecurity COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(NetTransportSecurity PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestNetTransportSecurity.cpp;SPARK_TEST_NAME_PREFIX=NetTransportSec_;SPARK_TEST_EXPECT_COUNT=11"
        LABELS "network;security;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()
add_test(NAME MMOSessionGateProtocol COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(MMOSessionGateProtocol PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SessionGateProtocol_;SPARK_TEST_EXPECT_COUNT=4"
    LABELS "mmo;network;security;unit"
    TIMEOUT 30)

# MMO auth policy hardening: equal-work login, per-peer admission and registration budgets.
set(_spark_mmo_auth_count 5)
add_test(NAME MMOAuthHardening COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(MMOAuthHardening PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestMMOAuthHardening.cpp;SPARK_TEST_NAME_PREFIX=MMOAuth;SPARK_TEST_EXPECT_COUNT=${_spark_mmo_auth_count}"
    LABELS "mmo;network;security;unit"
    # Registration performs a 600000-round PBKDF2 derivation (seconds in Debug).
    RUN_SERIAL TRUE
    TIMEOUT 180)

# MMO auth network behavior: session-gate peer admission/registration and game-thread chat
# delivery over secured loopback. This source intentionally uses only baseline APIs so it can
# provide behavioral RED evidence when copied to the pre-hardening tree.
if(ENABLE_NETWORKING AND TARGET imgui)
    add_test(NAME MMOAuthNetwork COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(MMOAuthNetwork PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestMMOAuthNetwork.cpp;SPARK_TEST_NAME_PREFIX=MMOAuthNet;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "mmo;network;security;unit"
        RUN_SERIAL TRUE
        TIMEOUT 180)
endif()

# SEC-net-transport finding 35: the MMO server binds client-authored player state
# to the sender's own server entity instead of relaying it (MMO sources need ImGui).
if(TARGET imgui AND ENABLE_NETWORKING)
    add_test(NAME MMOClientStateRequest COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(MMOClientStateRequest PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestGameModuleMMO.cpp;SPARK_TEST_NAME_PREFIX=MMO_StateRequest_;SPARK_TEST_EXPECT_COUNT=2"
        LABELS "network;security;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    # NET-100: the MMO server bridge registers only admitted (Connected) clients with the
    # WorldServer, never a Securing slot, and under the name its ClientFinished carried.
    add_test(NAME MMOServerBridgeAdmission COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(MMOServerBridgeAdmission PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestGameModuleMMO.cpp;SPARK_TEST_NAME_PREFIX=MMO_ServerBridge_;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "network-security;network;security;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    # SEC-120: the network-free MMO decoders (client state request on the server,
    # EntitySpawn / EntityDestroy on the client) that the module fuzz targets drive.
    add_test(NAME MMOClientStateCodec COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(MMOClientStateCodec PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestGameModuleMMO.cpp;SPARK_TEST_NAME_PREFIX=MMOClientState_;SPARK_TEST_EXPECT_COUNT=2"
        LABELS "network-security;network;security;unit"
        TIMEOUT 60)
    add_test(NAME MMOEntityEventCodec COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(MMOEntityEventCodec PROPERTIES
        ENVIRONMENT "SPARK_TEST_FILE=TestGameModuleMMO.cpp;SPARK_TEST_NAME_PREFIX=MMOEntityEvents_;SPARK_TEST_EXPECT_COUNT=3"
        LABELS "network-security;network;security;unit"
        TIMEOUT 60)
endif()

# LIFE-200: the production LifecycleCompositionRoot fails closed on injected
# stage failures (false return, std::exception, unknown throw, throwing shutdown).
add_test(NAME LifecycleCompositionRootFailure COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(LifecycleCompositionRootFailure PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=PartialInit_CompositionRoot;SPARK_TEST_EXPECT_COUNT=10"
    LABELS "lifecycle;unit"
    TIMEOUT 30)

# LIFE-200: every in-process lifecycle selector family runs under the lifecycle
# label, so `ctest -L lifecycle` (and the sanitizer presets' build dirs) cover the
# work item's selectors. Counts are exact per platform: a family that silently
# loses tests fails its CTest instead of shrinking the evidence.
#   ModuleABI_: 38 cross-platform, plus 3 Windows-only (wide-path loader, reload
#   parent-directory cleanup, module-directory dependency search) or 3 POSIX-only
#   (verified shadow, read-only install, private stage), plus 1 POSIX test that
#   needs the SparkGame module.
#   EngineLifecycle_: the whole family is compiled only on non-Windows hosts.
#   LIFE200_: 3 in-process, plus 2 that reload the real SparkGame module.
set(_spark_life_life200_count 3)
if(TARGET SparkGame)
    math(EXPR _spark_life_life200_count "${_spark_life_life200_count} + 2")
endif()
set(_spark_life_module_abi_count 38)
if(WIN32)
    math(EXPR _spark_life_module_abi_count "${_spark_life_module_abi_count} + 3")
else()
    math(EXPR _spark_life_module_abi_count "${_spark_life_module_abi_count} + 3")
    if(TARGET SparkGame)
        math(EXPR _spark_life_module_abi_count "${_spark_life_module_abi_count} + 1")
    endif()
endif()
set(_spark_life_selectors
    "ModuleLifecycle_=6"
    "ModuleABI_=${_spark_life_module_abi_count}"
    "EngineWiring_=12"
    "ModuleHotReload_=15"
    "LIFE200_=${_spark_life_life200_count}"
    "LifecycleLoop_=7"
    "ModuleReload_=3")
if(NOT WIN32)
    list(APPEND _spark_life_selectors "EngineLifecycle_=22")
endif()
foreach(_spark_life_selector IN LISTS _spark_life_selectors)
    string(REPLACE "=" ";" _spark_life_parts "${_spark_life_selector}")
    list(GET _spark_life_parts 0 _spark_life_prefix)
    list(GET _spark_life_parts 1 _spark_life_count)
    string(REGEX REPLACE "_$" "" _spark_life_name "${_spark_life_prefix}")
    add_test(NAME Lifecycle_${_spark_life_name} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(Lifecycle_${_spark_life_name} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=${_spark_life_prefix};SPARK_TEST_EXPECT_COUNT=${_spark_life_count}"
        LABELS "lifecycle;module;unit"
        RUN_SERIAL TRUE
        TIMEOUT 120)
endforeach()
# LifecycleLoop_ boots the production lifecycle about 120 times and ModuleReload_ cycles a
# module image 90 times; ASan/TSan builds need more than the family default. Each
# cycle is separately bounded by an in-process deadlock watchdog.
set_tests_properties(Lifecycle_LifecycleLoop Lifecycle_ModuleReload PROPERTIES TIMEOUT 600)

# SEC2 game-module security fixes. The family size depends on which module
# sources (ImGui-gated) and networking paths are compiled into SparkTests, so
# the pinned count is derived from the same conditions the test file uses.
# Unconditional: 2 FPS progression, 2 wave composition, 2 module network-teardown
# source contracts. Networking: 3 NetworkManager handler-ownership seams.
set(_spark_sec2gm_count 6)
if(ENABLE_NETWORKING)
    math(EXPR _spark_sec2gm_count "${_spark_sec2gm_count} + 3")
endif()
if(TARGET imgui)
    math(EXPR _spark_sec2gm_count "${_spark_sec2gm_count} + 3")
    if(ENABLE_NETWORKING)
        math(EXPR _spark_sec2gm_count "${_spark_sec2gm_count} + 3")
    endif()
endif()
add_test(NAME SEC2GameModules COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC2GameModules PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC2GM_;SPARK_TEST_EXPECT_COUNT=${_spark_sec2gm_count}"
    LABELS "security;module;unit"
    TIMEOUT 120)
if(TARGET imgui)
    # OpenWorld save/load through the real SaveSystem, including the SEC2 split-brain
    # regression (stale sidecar) and legacy-sidecar migration.
    add_test(NAME OpenWorldPersistence COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(OpenWorldPersistence PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=OpenWorldPersistence_;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "module;persistence;integration"
        RUN_SERIAL TRUE
        TIMEOUT 120)
endif()

# SDK-240 / OD-02: real N-1 and N+1 SDK module images are rejected with a
# diagnostic naming both versions before any module code runs; the current-SDK
# image loads. Exact count so a dropped N-1/N+1 case fails the lane.
add_test(NAME ModuleVersionCompatibility COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(ModuleVersionCompatibility PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=ModuleVersion_;SPARK_TEST_EXPECT_COUNT=6"
    LABELS "module;sdk;unit"
    RUN_SERIAL TRUE
    TIMEOUT 120)

# MOD-300: the real SparkGame image runs its spawn -> damage -> heal coroutine on the
# host CoroutineScheduler, stops and destroys it before unload, cancels on a lost target,
# and reports a missing scheduler. Exact count so a dropped case fails the lane.
if(TARGET SparkGame AND NOT WIN32)
    add_test(NAME SparkGameShowcase_CoroutineSequence COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SparkGameShowcase_CoroutineSequence PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SparkGameShowcase_Coroutine;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "module;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    # Quickload restores the exact showcase entities (name, Transform, Health, tags), re-tracks
    # them for status/spawn/shutdown, and cancels the in-flight coroutine.
    add_test(NAME SparkGameShowcase_QuickLoadRestoresExactState COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SparkGameShowcase_QuickLoadRestoresExactState PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=SparkGameShowcase_QuickLoadRestoresExactState;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "module;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    # The module loads its en/fr string tables into the host LocalizationSystem and
    # showcase_language relabels showcase_status.
    add_test(NAME SparkGameShowcase_StatusIsLocalized COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SparkGameShowcase_StatusIsLocalized PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=SparkGameShowcase_StatusIsLocalized;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "module;unit"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    # showcase_outcome reports the same record (coroutine, events, weather, time of day, exhibit meshes
    # resolved on disk, spawn count) for two fresh loads at 1/64 s and one at 1/60 s.
    add_test(NAME SparkGameShowcase_OutcomeIsDeterministic COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SparkGameShowcase_OutcomeIsDeterministic PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=SparkGameShowcase_OutcomeIsDeterministic;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "module;unit"
        RUN_SERIAL TRUE
        TIMEOUT 120)
endif()

# RHI-230: Vulkan backend under VK_LAYER_KHRONOS_validation (Lavapipe on CI).
# Lavapipe proves API-usage correctness only, not hardware certification.
# SPARK_REQUIRE_VULKAN_VALIDATION turns a missing ICD or validation layer into a
# failure: SPARK_TEST_EXPECT_COUNT counts skipped tests, so without it the lane
# would pass with all 18 tests skipped. Linux only, like SparkOpenGLTests: the
# Linux CI jobs install mesa-vulkan-drivers (Lavapipe), vulkan-validationlayers
# and glslang-tools (configure requires glslangValidator with Vulkan enabled).
if(SPARK_VULKAN_AVAILABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME VulkanValidation COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(VulkanValidation PROPERTIES
        ENVIRONMENT
            "SPARK_TEST_FILE=TestRHI230VulkanValidationReal.cpp;SPARK_TEST_EXPECT_COUNT=18;SPARK_REQUIRE_VULKAN_VALIDATION=1"
        LABELS "vulkan;vulkan-lavapipe;rendering;unit"
        RUN_SERIAL TRUE
        TIMEOUT 120)
endif()

# RHI-230: the Windows Vulkan shader toolchain. A Windows build with the Vulkan backend
# requires glslangValidator and builds the same SPIR-V (root CMakeLists.txt section 9.4),
# so the toolchain family runs there too: malformed SPIR-V rejected, valid SPIR-V accepted,
# every shipped module creates its pipeline, and the shader cache loads the staged SPIR-V.
# Needs a Vulkan ICD and VK_LAYER_KHRONOS_validation (the LunarG SDK); hosted windows
# runners have no Vulkan SDK, so SPARK_VULKAN_AVAILABLE is FALSE there and this lane is
# not registered. Exact count: equal to the Linux VulkanShaderToolchain_ family.
if(WIN32 AND SPARK_VULKAN_AVAILABLE AND TARGET SparkSpirvShaders)
    add_test(NAME VulkanShaderToolchainWindows COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(VulkanShaderToolchainWindows PROPERTIES
        ENVIRONMENT
            "SPARK_TEST_NAME_PREFIX=VulkanShaderToolchain_;SPARK_TEST_EXPECT_COUNT=4;SPARK_REQUIRE_VULKAN_VALIDATION=1"
        LABELS "vulkan;vulkan-hardware"
        RUN_SERIAL TRUE
        TIMEOUT 120)
endif()

# RHI-230: shipped-shader goldens on the vulkan-lavapipe row. Renders the
# FullscreenQuad post-process programs from the SPIR-V the build compiles
# (SparkSpirvShaders) through VulkanDevice under the validation layer, and
# compares each frame with the committed baseline under the proposed thresholds
# (owner review pending) and SHA-256 in Tests/GoldenImages/manifest.json.
# Software-row shader evidence only, not engine-pass goldens or hardware
# certification; the device must be Lavapipe. The exact count fails the lane if
# a scene test stops being compiled.
if(SPARK_VULKAN_AVAILABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET SparkSpirvShaders)
    add_test(NAME VulkanGoldenTests COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(VulkanGoldenTests PROPERTIES
        ENVIRONMENT
            "SPARK_TEST_FILE=TestRHI230VulkanGoldenReal.cpp;SPARK_TEST_EXPECT_COUNT=4;SPARK_REQUIRE_VULKAN_VALIDATION=1;SPARK_GOLDEN_OUTPUT_DIR=${CMAKE_BINARY_DIR}/Tests/Output"
        LABELS "vulkan;vulkan-lavapipe;vulkan-golden;rendering"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    # The baselines belong to one Mesa build, so exactly one lane compares them;
    # the main suite would otherwise repeat the comparison in every Linux build,
    # sanitizer and coverage lane that has Lavapipe. The tests are compiled only
    # with SPARK_VULKAN_SUPPORT and the SPIR-V build, so the registration floor
    # does not count them.
    spark_exclude_from_main_suite(VulkanGolden_RHI230_)

    # RHI-230: the Linux forward draw-list pass on a real SDL2 Vulkan window on Lavapipe, with
    # VK_LAYER_KHRONOS_validation forced on by the test and zero validation errors required.
    # Same display rule as LinuxForwardPassOpenGL (xvfb-run when present, else fail under
    # SPARK_REQUIRE_VULKAN_VALIDATION=1).
    if(SPARK_PACKAGE_HAS_SDL2)
        find_program(SPARK_XVFB_RUN_EXECUTABLE xvfb-run)
        set(_spark_forward_pass_display)
        if(SPARK_XVFB_RUN_EXECUTABLE)
            set(_spark_forward_pass_display "${SPARK_XVFB_RUN_EXECUTABLE}" -a -s "-screen 0 1280x720x24")
        endif()
        add_test(NAME LinuxForwardPassVulkan
            COMMAND ${_spark_forward_pass_display} $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(LinuxForwardPassVulkan PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=LinuxForwardPass_Vulkan;SPARK_TEST_EXPECT_COUNT=1;SPARK_REQUIRE_VULKAN_VALIDATION=1;SDL_AUDIODRIVER=dummy"
            LABELS "vulkan;vulkan-validation"
            RUN_SERIAL TRUE
            TIMEOUT 120)
        spark_exclude_from_main_suite(LinuxForwardPass_Vulkan)
    endif()
endif()

foreach(_remote_admin_selector IN ITEMS
        RemoteAdmin_AnonymousDenied
        RemoteAdmin_MalformedTimestampDenied
        RemoteAdmin_RoleMatrix
        RemoteAdmin_ReplayDenied
        RemoteAdmin_RateLimited
        RemoteAdmin_ExpiredDenied
        RemoteAdmin_ReservedCommandsCannotBeRebound
        # OD-05: remote administration is permanently unavailable in stable-v1.
        RemoteAdmin_UnavailableRawTransportDeniedAndAudited
        RemoteAdmin_UnavailableLocalGrantCannotAdminister)
    add_test(NAME ${_remote_admin_selector} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(${_remote_admin_selector} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=${_remote_admin_selector};SPARK_TEST_EXPECT_COUNT=1"
        LABELS "remote-admin;security;unit"
        RUN_SERIAL TRUE
        TIMEOUT 30)
endforeach()

# OD-05 through SparkServer's operator surface: no command-line switch or config
# key enables remote administration. The option parser exists only where the
# server core is built.
if(TARGET SparkServerCore)
    foreach(_remote_admin_server_selector IN ITEMS
            RemoteAdmin_UnavailableServerCliSwitchesRejected
            RemoteAdmin_UnavailableServerConfigKeysRejected)
        add_test(NAME ${_remote_admin_server_selector} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(${_remote_admin_server_selector} PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME=${_remote_admin_server_selector};SPARK_TEST_EXPECT_COUNT=1"
            LABELS "remote-admin;security;unit"
            RUN_SERIAL TRUE
            TIMEOUT 30)
    endforeach()
endif()

# OD-05 static ratchet: no argv comparison or config-key read in the engine,
# server, gateway or editor sources names a remote-administration switch.
add_test(
    NAME RemoteAdmin_UnavailableSwitchScan
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_remote_admin_switch_scan -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(RemoteAdmin_UnavailableSwitchScan PROPERTIES
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    LABELS "remote-admin;security;unit;python"
    TIMEOUT 120)

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
            GatewayAreaControl_AuditRecordOmitsSecrets
            GatewayAreaControl_RejectsOversizeFrame
            GatewayAreaControl_RejectsWrongServiceId
            GatewayAreaControl_RejectsMalformedFrames
            GatewayAreaControl_RejectsPhaseMismatch
            GatewayAreaControl_RejectsOutOfRangeTimestamp
            GatewayAreaControl_RejectsTruncatedAndOverlongHeaderFrames)
        add_test(NAME ${_gateway_area_control_selector} COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(${_gateway_area_control_selector} PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME=${_gateway_area_control_selector};SPARK_TEST_EXPECT_COUNT=1"
            LABELS "gateway;security;network"
            RUN_SERIAL TRUE
            TIMEOUT 30)
    endforeach()

    # NET-110: the GuardedGatewayAuthenticator GatewayCoordinator installs around every
    # authenticator (docs/specs/online-services.md 5.2): exceptions contained without the
    # credential, 2 s call budget, consecutive-fault circuit, counters and open circuit in the
    # gateway health JSON. Exact count: a dropped test fails.
    add_test(NAME GatewayGuardedAuthenticator COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(GatewayGuardedAuthenticator PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SparkGateway_GuardedAuthenticator_;SPARK_TEST_EXPECT_COUNT=7"
        LABELS "online-services;gateway;unit"
        TIMEOUT 30)

    # NET-110: the engine runs locally with deterministic adapters (docs/specs/online-services.md
    # section 6): GatewayCoordinator + WorldServer + NullOnlinePlatform with LocalFixtureAuthenticator
    # and LocalDeterministicPlacement give a byte-identical admission transcript across fresh runs.
    # Exact count: a dropped test fails.
    add_test(NAME OnlineServicesLocalStack COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(OnlineServicesLocalStack PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=OnlineServices_LocalStack_;SPARK_TEST_EXPECT_COUNT=6"
        LABELS "online-services;gateway;unit"
        TIMEOUT 30)
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
    # RHI-210: the production PostProcessingPipeline passes (ACES tonemap, bloom,
    # FXAA, GTAO) on WARP, compared with the d3d11-warp baselines and reviewed
    # thresholds in Tests/GoldenImages/manifest.json (owner review pending). The
    # device is always WARP, so exactly this lane compares them; the main suite
    # excludes the family.
    add_test(NAME D3D11PassGolden COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(D3D11PassGolden PROPERTIES
        ENVIRONMENT
            "SPARK_TEST_FILE=TestRHI210D3D11PassGoldenReal.cpp;SPARK_TEST_EXPECT_COUNT=5;SPARK_GOLDEN_OUTPUT_DIR=${CMAKE_BINARY_DIR}/Tests/Output"
        LABELS "d3d11-golden;d3d11;stable-v1"
        RUN_SERIAL TRUE
        TIMEOUT 120)
    spark_exclude_from_main_suite(D3D11PassGolden_)
    add_test(NAME D3D11_Resource COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(D3D11_Resource PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=D3D11_Resource_;SPARK_TEST_EXPECT_COUNT=3"
        LABELS "d3d11-stress;d3d11;stable-v1"
        RUN_SERIAL TRUE
        TIMEOUT 180)
    # RHI-210: SPARK_D3D11_DEBUG_LAYER validation of real engine and RHI frames plus the
    # debug-layer create/destroy stress (TestRHI210D3D11ValidationReal.cpp), and full
    # device-loss recovery through gfx_reset_device (TestRHI210D3D11DeviceLossReal.cpp).
    # Every D3D11_Validation_ test needs the Windows Graphics Tools debug layer and fails
    # (never skips) without it, so the family runs only in its own lane, labelled
    # d3d11-debug-layer; build-windows-vs2022 installs the layer before running ctest.
    add_test(NAME D3D11_Validation COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(D3D11_Validation PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=D3D11_Validation_;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "d3d11-validation;d3d11-debug-layer;d3d11;stable-v1"
        RUN_SERIAL TRUE
        TIMEOUT 180)
    add_test(NAME D3D11_DeviceLoss COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(D3D11_DeviceLoss PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=D3D11_DeviceLoss_;SPARK_TEST_EXPECT_COUNT=2"
        LABELS "d3d11-stress;d3d11;stable-v1"
        RUN_SERIAL TRUE
        TIMEOUT 120)
    # CreateShader must compile source with no debugName (D3DCompile rejects an empty source
    # name with the standard include handler), name the source by filePath so relative
    # includes resolve, and log the HRESULT on failure (TestD3D11DeviceContractsReal.cpp).
    add_test(NAME D3D11_ShaderCompile COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(D3D11_ShaderCompile PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=D3D11_ShaderCompile_;SPARK_TEST_EXPECT_COUNT=3"
        LABELS "d3d11;unit"
        TIMEOUT 60)
    spark_exclude_from_main_suite(D3D11_Validation_)
endif()

# SEC4 audio: SoundEffect accepts only PCM / IEEE-float WAV fmt chunks with
# self-consistent fields and a zero cbSize, so a crafted EXTENSIBLE or ADPCM header
# cannot make XAudio2 read past the stored 18-byte WAVEFORMATEX (TestSEC4SoundEffectWav.cpp;
# no audio device needed). Chunk ids and sizes are read as 32-bit fields, so LP64 hosts
# (8-byte DWORD) load WAVs too. Exact count so a filter that selects nothing cannot pass.
add_test(NAME SEC4SoundEffectWav COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC4SoundEffectWav PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC4SoundWav_;SPARK_TEST_EXPECT_COUNT=6"
    LABELS "security;audio;unit"
    TIMEOUT 60)
# SEC-120: OpenALAudioEngine::LoadWAVFile decodes through the same SoundEffect parser
# and maps its format onto OpenAL's core buffer formats (SelectOpenALWavFormat). The
# OpenAL backend only exists off Windows, so this family is registered there only.
if(NOT WIN32)
    add_test(NAME SEC4OpenALWav COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SEC4OpenALWav PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC4OpenALWav_;SPARK_TEST_EXPECT_COUNT=2"
        LABELS "security;audio;unit"
        TIMEOUT 60)
endif()

# SEC2 rendering lane (TestSEC2RenderingHardeningReal.cpp): daemon shader-blob length
# bounds, the shared UpdateBuffer range check, D3D11 UpdateBuffer, D3D12 render-target
# clamping, D3D12 fence-deferred resource/descriptor release and SparkShaderCompiler
# -batch output containment. Exact counts per family so a filter that selects nothing
# cannot pass.
add_test(NAME SEC2Render_Portable COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(SEC2Render_Portable PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC2Render_Portable_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "security;rendering;unit"
    TIMEOUT 60)
if(WIN32)
    add_test(NAME SEC2Render_D3D11 COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(SEC2Render_D3D11 PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC2Render_D3D11_;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "security;rendering;d3d11"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    # D3D12 sources are excluded from MinGW builds (root CMakeLists.txt, SPARK_NO_D3D12).
    if(NOT (MINGW AND NOT MSVC))
        add_test(NAME SEC2Render_D3D12 COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(SEC2Render_D3D12 PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=SEC2Render_D3D12_;SPARK_TEST_EXPECT_COUNT=2"
            LABELS "security;rendering;d3d12"
            RUN_SERIAL TRUE
            TIMEOUT 120)

        # RHI-225: the D3D12 RHI under the debug layer and GPU-based validation
        # (TestRHI225D3D12ValidationReal.cpp): a triangle frame, a constant-buffer draw, a
        # sampled-texture draw and 200 fence-deferred create/destroy cycles with zero error
        # or corruption messages, and a negative control proving the counter sees an
        # injected invalid call. Needs
        # d3d12SDKLayers.dll (Windows "Graphics Tools"); an inactive debug layer fails. The
        # Windows CI jobs that run the whole tree install it first ("Install D3D12 debug layer").
        # Out of the main suite: enabling the debug layer is process-wide.
        add_test(NAME D3D12_Validation COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(D3D12_Validation PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=D3D12_Validation_;SPARK_TEST_EXPECT_COUNT=5"
            LABELS "d3d12;d3d12-validation"
            RUN_SERIAL TRUE
            TIMEOUT 300)
        spark_exclude_from_main_suite(D3D12_Validation_)

        # RHI-225: advanced-feature fallbacks (TestRHI225D3D12FallbackReal.cpp). On the adapter
        # D3D12Device selects (hardware, or WARP on a GPU-less host) every advanced capability
        # flag equals an independent CheckFeatureSupport query, two initializations agree, and
        # the DXR entry point follows its flag (no ID3D12Device5 and the software backend when
        # unsupported). Fails when no D3D12 device can be created.
        add_test(NAME D3D12Fallback COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(D3D12Fallback PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=D3D12Fallback_;SPARK_TEST_EXPECT_COUNT=3"
            LABELS "d3d12"
            RUN_SERIAL TRUE
            TIMEOUT 120)

        # RHI-225: the declared D3D11/D3D12 parity matrix (TestRHI225D3D12ParityReal.cpp,
        # mirrored in wiki/graphics/D3D12-Backend.md). Each scene renders the same HLSL through
        # both RHI devices; frames must agree (<= 0.5% of pixels beyond 2 per channel) and each
        # must match a CPU expectation. Both devices must share an adapter (LUID, or both
        # software); a missing D3D12 feature fails, nothing skips. Out of the main suite: every
        # scene creates two GPU devices.
        add_test(NAME D3D12_Parity COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
        set_tests_properties(D3D12_Parity PROPERTIES
            ENVIRONMENT "SPARK_TEST_NAME_PREFIX=D3D12_Parity_;SPARK_TEST_EXPECT_COUNT=12"
            LABELS "d3d12;d3d12-parity"
            RUN_SERIAL TRUE
            TIMEOUT 300)
        spark_exclude_from_main_suite(D3D12_Parity_)
    endif()
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

# RDY-015: the Linux hosts honor -require-game (exit 2 when no game module
# initialized) and print one post-teardown SPARK_MODULE_LIFECYCLE record named
# after the loaded library. Host evidence only; it promotes no module profile.
add_test(NAME ModuleLifecycleRecordHost COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(ModuleLifecycleRecordHost PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=ModuleLifecycleRecord_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "lifecycle;module;unit"
    RUN_SERIAL TRUE
    TIMEOUT 60)
if(UNIX AND NOT APPLE AND SPARK_PACKAGE_HAS_SDL2 AND BUILD_GAME_MODULES AND
   TARGET SparkEngine AND TARGET SparkGame)
    add_dependencies(SparkTests SparkEngine SparkGame SparkCompatibleModuleFixture)
    if(SPARK_HEADLESS_SUPPORT)
        set(_spark_lifecycle_record_headless ON)
    else()
        set(_spark_lifecycle_record_headless OFF)
    endif()
    add_test(
        NAME LinuxHostModuleLifecycleRecord
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGame>"
            "-DSPARK_FAILING_MODULE=$<TARGET_FILE:SparkCompatibleModuleFixture>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_CHECK_HEADLESS=${_spark_lifecycle_record_headless}"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkLinuxModuleLifecycleRecord.cmake
    )
    set_tests_properties(LinuxHostModuleLifecycleRecord PROPERTIES
        LABELS "lifecycle;module;linux;sdl2;integration;process"
        RUN_SERIAL TRUE
        TIMEOUT 240)
endif()

# EDT-210: the Linux host runs `-scene <path>` engine-only. An editor-saved
# reflected scene must load into the engine ECS world (one SPARK_SCENE_LOADED
# record with the fixture's counts, no game module), and a missing or rejected
# scene must exit 4 instead of running an empty engine. Loading only: the Linux
# basic draw path is a no-op, so this is not rendering evidence.
if(UNIX AND NOT APPLE AND TARGET SparkEngine AND (SPARK_HEADLESS_SUPPORT OR SPARK_PACKAGE_HAS_SDL2))
    add_dependencies(SparkTests SparkEngine)
    set(_spark_scene_preview_headless OFF)
    set(_spark_scene_preview_sdl OFF)
    if(SPARK_HEADLESS_SUPPORT)
        set(_spark_scene_preview_headless ON)
    endif()
    if(SPARK_PACKAGE_HAS_SDL2)
        set(_spark_scene_preview_sdl ON)
    endif()
    add_test(
        NAME LinuxScenePreview
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_SCENE_FIXTURE=${CMAKE_SOURCE_DIR}/Tests/Fixtures/EditorScene/Scenes/EditorSeeded.sparkscene"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_SCENE_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/linux-scene-preview"
            "-DSPARK_CHECK_HEADLESS=${_spark_scene_preview_headless}"
            "-DSPARK_CHECK_SDL=${_spark_scene_preview_sdl}"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkLinuxScenePreview.cmake
    )
    set_tests_properties(LinuxScenePreview PROPERTIES
        LABELS "editor;scene;linux;integration;process"
        RUN_SERIAL TRUE
        TIMEOUT 600)
endif()

# EDT-210: the Windows host runs an editor-authored scene via -scene on the
# headless NullRHI loop, reports its Assets/... references, and fails a scene
# that cannot load with exit 4 instead of running an empty engine.
if(WIN32 AND TARGET SparkEngine AND SPARK_HEADLESS_SUPPORT)
    add_dependencies(SparkTests SparkEngine)
    add_test(
        NAME WindowsScenePreview
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_SCENE_FIXTURE=${CMAKE_SOURCE_DIR}/Tests/Fixtures/EditorScene/Scenes/EditorSeeded.sparkscene"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_SCENE_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/windows-scene-preview"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkWindowsScenePreview.cmake
    )
    set_tests_properties(WindowsScenePreview PROPERTIES
        LABELS "editor;scene;windows;integration;process;editor-integration"
        RUN_SERIAL TRUE
        TIMEOUT 600)
endif()

# EDT-210: author -> cook -> package -> run. A mesh dragged onto a World entity through
# the Asset Browser payload and the Inspector drop core is saved, cooked by the real
# SparkCooker, packaged around the real SparkEngine host by AssembleNativePackage, and
# the packaged ScenePreview host must load the scene and resolve (or, with the cooked
# mesh removed, report missing) its Assets/... reference. Windows until the Linux host's
# packaged-scene asset record is covered; the source compiles its TESTs only under _WIN32,
# so no other host's whole-binary run (sanitizers, coverage) contains the family.
# EditorCookPackage_ needs this lane's SPARK_ENGINE_EXECUTABLE and fails without it, so
# the Windows main suite excludes the family.
if(WIN32)
    get_test_property(SparkEngineTests ENVIRONMENT _spark_main_suite_environment)
    string(REGEX REPLACE "(^|;)SPARK_TEST_EXCLUDE=([^;]*)" "\\1SPARK_TEST_EXCLUDE=\\2,EditorCookPackage_"
        _spark_main_suite_environment "${_spark_main_suite_environment}")
    set_tests_properties(SparkEngineTests PROPERTIES ENVIRONMENT "${_spark_main_suite_environment}")
    unset(_spark_main_suite_environment)
endif()
if(WIN32 AND TARGET SparkEngine AND TARGET SparkCooker AND SPARK_HEADLESS_SUPPORT)
    add_dependencies(SparkTests SparkEngine SparkCooker)
    add_test(NAME EditorCookPackageRoundTrip COMMAND $<TARGET_FILE:SparkTests> --empty-is-error)
    set_tests_properties(EditorCookPackageRoundTrip PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=EditorCookPackage_;SPARK_TEST_EXPECT_COUNT=2;SPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
        LABELS "editor;editor-integration;editor-package-roundtrip;process"
        RUN_SERIAL TRUE
        TIMEOUT 300)
    # EDT-210: the same author -> cook -> package -> run family against an INSTALLED
    # runtime: the runtime component is installed into a fresh prefix outside the source
    # and build trees, and the tests require the packaged host to come from it.
    add_test(
        NAME EditorCookPackageInstalledRuntime
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_CONFIG=$<CONFIG>"
            "-DSPARK_TESTS_EXECUTABLE=$<TARGET_FILE:SparkTests>"
            "-DSPARK_ENGINE_EXECUTABLE_NAME=$<TARGET_FILE_NAME:SparkEngine>"
            -DSPARK_STAGE_TIMEOUT=420
            -P ${CMAKE_SOURCE_DIR}/cmake/RunEditorAuthorInstalledRuntime.cmake)
    set_tests_properties(EditorCookPackageInstalledRuntime PROPERTIES
        LABELS "editor;editor-integration;editor-package-roundtrip;installed-runtime;process"
        RUN_SERIAL TRUE
        TIMEOUT 900)
endif()

# SEC3: an -exec script that cannot be loaded (missing, or a directory) fails the
# headless launch with exit 1 on both hosts instead of running an empty timeline
# to its -test-frames limit and exiting 0; a readable script is the passing control.
if((WIN32 OR (UNIX AND NOT APPLE)) AND TARGET SparkEngine AND SPARK_HEADLESS_SUPPORT)
    add_dependencies(SparkTests SparkEngine)
    add_test(
        NAME ExecScriptLoadFailureFailsLaunch
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_SCENE_FIXTURE=${CMAKE_SOURCE_DIR}/Tests/Fixtures/EditorScene/Scenes/EditorSeeded.sparkscene"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_EXEC_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/exec-script-load-failure"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkExecScriptLoadFailure.cmake
    )
    set_tests_properties(ExecScriptLoadFailureFailsLaunch PROPERTIES
        LABELS "headless;automation;integration;process"
        RUN_SERIAL TRUE
        TIMEOUT 300)
endif()

# EDT-210: the ImGui-free asset drag/drop contract shared by the Asset Browser
# producer and the inspector mesh/material drop targets, including the
# World-backed Inspector's drop (one undo step, save/reload round trip).
add_test(NAME EditorAssetDrag COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(EditorAssetDrag PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=EditorAssetDrag_;SPARK_TEST_EXPECT_COUNT=10"
    LABELS "editor;editor-integration;unit"
    TIMEOUT 60)

# EDT-210: the document create/delete/applied-edit commands EditorUI forwards to
# SparkEditor::EditorDocument. Each records one undo step whose undo and redo
# reproduce the SerializeWorld snapshots byte for byte.
add_test(NAME EditorUndoDocument COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(EditorUndoDocument PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=EditorUndo_Document_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "editor;editor-integration;unit"
    TIMEOUT 60)

# EDT-210: "all world mutations" is the closed list in SparkEditor/world-mutation-inventory.json.
# The inventory test fails on an unlisted, recounted or stale mutation site; the matrix
# drives every command-class entry point through one undo-all/redo-all session.
add_test(
    NAME EditorUndo_WorldMutationInventory
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_editor_world_mutation_inventory.py)
set_tests_properties(EditorUndo_WorldMutationInventory PROPERTIES
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    LABELS "editor;editor-integration;unit;python"
    TIMEOUT 120)
add_test(NAME EditorUndoWorldMatrix COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(EditorUndoWorldMatrix PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=EditorUndo_WorldMatrix_;SPARK_TEST_EXPECT_COUNT=2"
    LABELS "editor;editor-integration"
    TIMEOUT 60)

add_test(
    NAME NullRHIWindowsLifecycleParserContract
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_HEADLESS_NULLRHI_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkHeadlessNullRHILifecycle.cmake
)
set_tests_properties(NullRHIWindowsLifecycleParserContract PROPERTIES
    LABELS "nullrhi-headless;stable-v1;unit"
    TIMEOUT 10)

# HEAD-220: the staged-package repository-isolation scrub that
# FPSHeadlessPackage_NullRHISaveReload applies, fed synthetic output (both
# slash spellings, case, build-dir mentions, a sibling directory, and the
# positive control that the output names the package at all).
add_test(
    NAME FPSHeadlessPackage_RepositoryIsolationDetection
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_REPOSITORY_ISOLATION_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkHeadlessFPSSaveReload.cmake
)
set_tests_properties(FPSHeadlessPackage_RepositoryIsolationDetection PROPERTIES
    LABELS "nullrhi-headless;unit"
    PASS_REGULAR_EXPRESSION "Repository isolation detection self-test passed 7 cases"
    TIMEOUT 10)

# PLT-210: the production Linux headless host runs the real SparkGameFPS module
# on the EngineRuntime-owned NullRHI bridge and must print exactly one
# SPARK_MODULE_READY, SPARK_HEADLESS_RHI and SPARK_HEADLESS_LIFECYCLE record, in
# that order, with rendered=0 and faults=0. It reuses the strict parser of
# NullRHI_Windows_FPSLifecycle so the Linux sanitizer lanes exercise the shared
# FPS/NullRHI lifecycle. Source-tree evidence only; no packaged certification.
if(UNIX AND NOT APPLE AND SPARK_HEADLESS_SUPPORT AND BUILD_GAME_MODULES AND
   TARGET SparkEngine AND TARGET SparkGameFPS)
    add_dependencies(SparkTests SparkEngine SparkGameFPS)
    add_test(
        NAME NullRHI_Linux_FPSLifecycle
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGameFPS>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            -DSPARK_RHI_BACKEND=null
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkHeadlessNullRHILifecycle.cmake
    )
    set_tests_properties(NullRHI_Linux_FPSLifecycle PROPERTIES
        LABELS "nullrhi-headless;linux;integration"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    # HEAD-220: writer/reader quicksave persistence across two fresh headless
    # NullRHI processes (the WIN32 block registers the same test).
    add_test(
        NAME HeadlessSaveReload_FPSNullRHI
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGameFPS>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/headless-save-reload/$<CONFIG>"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkHeadlessFPSSaveReload.cmake
    )
    set_tests_properties(HeadlessSaveReload_FPSNullRHI PROPERTIES
        LABELS "nullrhi-headless;save;integration;process"
        RUN_SERIAL TRUE
        TIMEOUT 300)
endif()

# HEAD-220: graceful (SIGTERM), forced-kill-then-restart and kill-during-boot
# scenarios against the production headless FPS host on NullRHI, each accepted
# by the strict parser of RunSparkHeadlessNullRHILifecycle. Linux only: the
# Windows host is a GUI-subsystem executable with no console when its output is
# redirected, so CTRL_BREAK_EVENT cannot reach it. Source-tree evidence only,
# not packaged Windows certification.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME HeadlessShutdown_HarnessContract
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/run_headless_shutdown_recovery.py --self-test)
    set_tests_properties(HeadlessShutdown_HarnessContract PROPERTIES
        LABELS "nullrhi-headless;unit;python"
        TIMEOUT 30)
    if(UNIX AND NOT APPLE AND NOT CMAKE_CROSSCOMPILING AND SPARK_HEADLESS_SUPPORT AND
       BUILD_GAME_MODULES AND TARGET SparkEngine AND TARGET SparkGameFPS)
        add_dependencies(SparkTests SparkEngine SparkGameFPS)
        foreach(_spark_headless_shutdown_case
                "Graceful:graceful" "ForcedRecovery:forced-recovery" "BootInterrupted:boot-interrupted")
            string(REPLACE ":" ";" _spark_headless_shutdown_pair "${_spark_headless_shutdown_case}")
            list(GET _spark_headless_shutdown_pair 0 _spark_headless_shutdown_name)
            list(GET _spark_headless_shutdown_pair 1 _spark_headless_shutdown_scenario)
            add_test(
                NAME HeadlessShutdown_${_spark_headless_shutdown_name}
                COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/run_headless_shutdown_recovery.py
                    --scenario ${_spark_headless_shutdown_scenario}
                    --engine $<TARGET_FILE:SparkEngine>
                    --module $<TARGET_FILE:SparkGameFPS>
                    --source-root ${CMAKE_SOURCE_DIR}
                    --cmake ${CMAKE_COMMAND})
            set_tests_properties(HeadlessShutdown_${_spark_headless_shutdown_name} PROPERTIES
                LABELS "nullrhi-headless;linux;integration;process"
                RUN_SERIAL TRUE
                TIMEOUT 300)
        endforeach()
    endif()
endif()

# LIFE-200: ten consecutive production headless FPS boots on NullRHI, each with a
# fresh cwd and private user dirs, a 60 s deadlock bound, the strict lifecycle
# parser, the arena record, released NullRHI resources, a teardown within the
# nullrhi.headless.shutdown_time ceiling of perf-budgets/v1/budget.json and no
# sanitizer report, plus an identical arena state across boots. The host exits on -test-frames, so
# no signal channel is needed and the loop runs on the Windows and Linux hosts.
# The lifecycle label puts it in the Linux ASan/TSan presets' ctest runs.
if(Python3_Interpreter_FOUND)
    add_test(
        NAME HeadlessBootLoop_HarnessContract
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/run_headless_boot_loop.py --self-test
            --source-root ${CMAKE_SOURCE_DIR}
            --cmake ${CMAKE_COMMAND})
    set_tests_properties(HeadlessBootLoop_HarnessContract PROPERTIES
        LABELS "lifecycle;nullrhi-headless;unit;python"
        TIMEOUT 60)
    if(NOT CMAKE_CROSSCOMPILING AND BUILD_GAME_MODULES AND TARGET SparkEngine AND TARGET SparkGameFPS AND
       (WIN32 OR (UNIX AND NOT APPLE AND SPARK_HEADLESS_SUPPORT)))
        add_dependencies(SparkTests SparkEngine SparkGameFPS)
        add_test(
            NAME HeadlessBootLoop_FPSNullRHI
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/run_headless_boot_loop.py
                --engine $<TARGET_FILE:SparkEngine>
                --module $<TARGET_FILE:SparkGameFPS>
                --source-root ${CMAKE_SOURCE_DIR}
                --cmake ${CMAKE_COMMAND}
                --iterations 10)
        set_tests_properties(HeadlessBootLoop_FPSNullRHI PROPERTIES
            LABELS "lifecycle;nullrhi-headless;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 900)
    endif()
endif()

# RDY-015: real-source lifecycle evidence for every experimental game module.
# The parser contract is deterministic and runs in every lane; the per-module
# runs launch the production Linux SDL host with -require-game and carry the
# experimental-modules label, which every full-ctest lane excludes (-LE) and
# only the advisory experimental-module-lifecycle job selects. The module list
# is every tools/module-evidence/manifest.json entry tracked under RDY-015, so
# these tests never carry a stable-v1 or module-profile label.
add_test(
    NAME ExperimentalModuleLifecycleParserContract
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_EXPERIMENTAL_LIFECYCLE_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkExperimentalModuleLifecycle.cmake
)
set_tests_properties(ExperimentalModuleLifecycleParserContract PROPERTIES
    LABELS "prototype;lifecycle;unit"
    TIMEOUT 10)
if(UNIX AND NOT APPLE AND SPARK_PACKAGE_HAS_SDL2 AND BUILD_GAME_MODULES AND TARGET SparkEngine)
    set(_spark_module_evidence_manifest "${CMAKE_SOURCE_DIR}/tools/module-evidence/manifest.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_spark_module_evidence_manifest}")
    file(READ "${_spark_module_evidence_manifest}" _spark_module_evidence_json)
    string(JSON _spark_manifest_module_count LENGTH "${_spark_module_evidence_json}" modules)
    math(EXPR _spark_manifest_module_last "${_spark_manifest_module_count} - 1")
    set(_spark_experimental_unregistered "")
    foreach(_spark_manifest_index RANGE ${_spark_manifest_module_last})
        string(JSON _spark_experimental_tracker ERROR_VARIABLE _spark_not_experimental
            GET "${_spark_module_evidence_json}" modules ${_spark_manifest_index} experimentalSeparation trackedUnder)
        if(_spark_not_experimental OR NOT _spark_experimental_tracker STREQUAL "RDY-015")
            continue()
        endif()
        string(JSON _spark_experimental_module GET "${_spark_module_evidence_json}"
            modules ${_spark_manifest_index} cmakeTarget)
        # SPARK_GAME_MODULES may select a subset (e.g. the stable-v1 FPS-only
        # profile); with the full module set every manifest module must build.
        if(NOT TARGET ${_spark_experimental_module})
            list(APPEND _spark_experimental_unregistered "${_spark_experimental_module}")
            continue()
        endif()
        add_test(
            NAME ExperimentalModuleLifecycle_${_spark_experimental_module}
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
                "-DSPARK_GAME_MODULE=$<TARGET_FILE:${_spark_experimental_module}>"
                "-DSPARK_MODULE_TARGET=${_spark_experimental_module}"
                "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
                -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkExperimentalModuleLifecycle.cmake
        )
        set_tests_properties(ExperimentalModuleLifecycle_${_spark_experimental_module} PROPERTIES
            LABELS "experimental-modules;prototype"
            RUN_SERIAL TRUE
            TIMEOUT 60)
    endforeach()
    if(SPARK_GAME_MODULES STREQUAL "" AND _spark_experimental_unregistered)
        message(FATAL_ERROR
            "RDY-015: tools/module-evidence/manifest.json tracks experimental module(s) with no CMake target, "
            "so they would get no ExperimentalModuleLifecycle_ test: ${_spark_experimental_unregistered}")
    endif()
endif()

# SEC2: legacy peer-hosted collaboration — join-code authentication, frame/queue/
# connection bounds and handshake deadlines. Hosting is a hard precondition in these
# tests, so --empty-is-error plus the pinned count keep a hosting regression (or a
# dropped test) from reading as a pass.
add_test(NAME CollaborativeEditingSecurity COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(CollaborativeEditingSecurity PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestCollaborativeEditing.cpp;SPARK_TEST_EXPECT_COUNT=33"
    LABELS "editor;collaboration;network;security"
    RUN_SERIAL TRUE
    TIMEOUT 180)

# SEC2: an opened project is untrusted — cook/package refuse project links and
# link-redirected output, and oversized project documents are refused unread.
add_test(NAME EditorUntrustedProject COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(EditorUntrustedProject PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=UntrustedProject_;SPARK_TEST_EXPECT_COUNT=5"
    LABELS "editor;security;unit"
    TIMEOUT 120)

# HEAD-220: NullRHIDevice live-resource accounting follows resource lifetime
# (leak/soak signal for packaged headless runs).
add_test(NAME NullRHIResourceLifetime COMMAND $<TARGET_FILE:SparkTests> --empty-is-error)
set_tests_properties(NullRHIResourceLifetime PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=NullRHI_Lifetime_;SPARK_TEST_EXPECT_COUNT=8"
    LABELS "nullrhi-headless;stable-v1;unit"
    TIMEOUT 30)

# ENG-220: fail-closed glTF skin import (JOINTS_0/WEIGHTS_0, inverse binds, joint
# tree, 256-joint GPU palette). CPU importer coverage only: no skinned rendering,
# animation clips or Blender-authored skinned asset. Exact count: a dropped test fails.
add_test(NAME GLTFSkinnedMeshImport COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(GLTFSkinnedMeshImport PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=GLTF_Skinning_;SPARK_TEST_EXPECT_COUNT=19"
    LABELS "assets;gltf;unit"
    TIMEOUT 30)

# ENG-220: glTF skeletons and LINEAR/STEP TRS joint animations through AnimationManager
# (LoadSkeleton/LoadAnimations), posed vertices from AnimationEvaluator checked against an
# independent skinning reference, and skin influences kept by MeshAsset on every platform
# (Windows loads the D3D11 MeshAsset on a WARP device). Exact count: a dropped test fails.
add_test(NAME GLTFAnimationImport COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(GLTFAnimationImport PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=GLTF_Animation_;SPARK_TEST_EXPECT_COUNT=27"
    LABELS "assets;gltf;unit"
    TIMEOUT 30)

# ENG-220: production GPUSkinning (SkinningCS.hlsl) on a WARP D3D11 device. The skinned
# fixture GLB goes through the shared MeshAsset glTF import and AnimationManager/Evaluator; the
# GPU output buffer is read back and compared with the independent ReferencePose() at three clip
# times, a mutated bone matrix must fail the comparison, and a 130-vertex four-influence blend
# (with a zero weight on a NaN bone) matches its closed form. Exact count: a dropped test fails.
if(WIN32)
    add_test(NAME GPUSkinningD3D11 COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(GPUSkinningD3D11 PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=GPUSkinningD3D11_;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "gltf-d3d11;d3d11;assets;stable-v1"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()

# ENG-220: basic-material texture diagnostics on a WARP GraphicsEngine. A rejected (rooted,
# absolute or escaping) declared texture fails the material; a missing or undecodable one logs an
# error naming the material, key, path and reason, and the editor-import retry still recovers it.
# Also selects the fresh-render-thread COM load. Exact count: a dropped test fails.
if(WIN32)
    add_test(NAME BasicMaterialDiagnostics COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(BasicMaterialDiagnostics PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME_PREFIX=BasicMaterial_;SPARK_TEST_EXPECT_COUNT=4"
        LABELS "gltf-d3d11;assets;d3d11"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()

# ENG-220: DirectXMath quaternion slerp/multiply, rotation-axis and XMMatrixDecompose semantics
# against closed-form values, plus the animation slerp and BlendTransforms paths built on them.
# Real DirectXMath on Windows, PlatformDirectXMathStubs.h elsewhere. Exact count: a dropped test fails.
add_test(NAME DirectXMathStubParity COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(DirectXMathStubParity PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=DXMathStub_;SPARK_TEST_EXPECT_COUNT=14"
    LABELS "animation;math;unit"
    TIMEOUT 30)

# PERF-100: the bounded tick work-time histogram both headless hosts feed, then
# the producer end to end: the real headless host on NullRHI with the SparkGame
# module, its SPARK_HEADLESS_TICK_STATS record (incl. self-read peak RSS), and a result JSON that
# passes validate_result and stays NON-AUTHORITATIVE on the uncertified row.
add_test(NAME HeadlessTickStats COMMAND $<TARGET_FILE:SparkTests> --empty-is-error)
set_tests_properties(HeadlessTickStats PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=HeadlessTickStats_;SPARK_TEST_EXPECT_COUNT=7"
    LABELS "benchmark;nullrhi-headless;unit"
    TIMEOUT 30)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME HeadlessTickCollector_Parser
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_collect_headless_result.py ParserTests CollectorGuardTests)
    set_tests_properties(HeadlessTickCollector_Parser PROPERTIES
        LABELS "benchmark;perf-budget;unit;python"
        TIMEOUT 60)
    # PERF-100 soak harness: leak fit, result schema, and every failure mode
    # (leak, crash, deadlock, spin, never-ready) against a stand-in host. Not
    # labelled nullrhi-soak: only a real-host soak may satisfy that label.
    add_test(
        NAME NullRHISoak_Harness
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_run_nullrhi_soak.py LeakFitTests ResultTests HarnessTests
            WindowsSamplerTests)
    set_tests_properties(NullRHISoak_Harness PROPERTIES
        LABELS "perf-budget;unit;python"
        TIMEOUT 120)
    # Under ASan, freed blocks sit in a 256 MiB allocator quarantine that fills
    # for minutes and reads as RSS growth. The soaks bound it to 1 MiB so the
    # slope measures engine memory; leak detection stays on. Other builds
    # ignore ASAN_OPTIONS.
    set(_spark_soak_asan_quarantine "ASAN_OPTIONS=string_append::quarantine_size_mb=1")
    if(UNIX AND NOT APPLE AND SPARK_HEADLESS_SUPPORT AND BUILD_GAME_MODULES AND
       TARGET SparkEngine AND TARGET SparkGame)
        add_dependencies(SparkTests SparkEngine SparkGame)
        add_test(
            NAME Benchmark_HeadlessTickLoop
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_collect_headless_result.py HeadlessTickLoopBenchmark)
        set_tests_properties(Benchmark_HeadlessTickLoop PROPERTIES
            ENVIRONMENT "SPARK_HEADLESS_ENGINE=$<TARGET_FILE:SparkEngine>;SPARK_HEADLESS_MODULE=$<TARGET_FILE:SparkGame>"
            LABELS "benchmark;nullrhi-headless;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 180)
        # ~120 s real-host soak; scheduled long soaks run
        # tools/perf-budget/run_nullrhi_soak.py --duration directly.
        add_test(
            NAME Soak_NullRHIHeadlessSmoke
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_run_nullrhi_soak.py SoakNullRHIHeadlessSmoke)
        set_tests_properties(Soak_NullRHIHeadlessSmoke PROPERTIES
            ENVIRONMENT "SPARK_HEADLESS_ENGINE=$<TARGET_FILE:SparkEngine>;SPARK_HEADLESS_MODULE=$<TARGET_FILE:SparkGame>"
            ENVIRONMENT_MODIFICATION "${_spark_soak_asan_quarantine}"
            LABELS "nullrhi-soak;nullrhi-headless;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 480)
    endif()
    # PERF-100: the same ~120 s real-host soak on the Windows headless host,
    # sampled through kernel32 (private commit and main-thread cycle count).
    # It writes a --report only: no Windows soak metric row is defined yet.
    if(WIN32 AND NOT CMAKE_CROSSCOMPILING AND SPARK_HEADLESS_SUPPORT AND BUILD_GAME_MODULES AND
       TARGET SparkEngine AND TARGET SparkGame)
        add_dependencies(SparkTests SparkEngine SparkGame)
        add_test(
            NAME Soak_NullRHIHeadlessSmoke
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_run_nullrhi_soak.py SoakNullRHIHeadlessSmoke)
        set_tests_properties(Soak_NullRHIHeadlessSmoke PROPERTIES
            ENVIRONMENT "SPARK_HEADLESS_ENGINE=$<TARGET_FILE:SparkEngine>;SPARK_HEADLESS_MODULE=$<TARGET_FILE:SparkGame>"
            LABELS "nullrhi-soak;nullrhi-headless;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 480)
    endif()
    # HEAD-220: ten-minute bounded soak of the shared headless FPS/NullRHI path
    # (SparkGameFPS, 36000 ticks): VmRSS sampled from /proc, the provisional
    # leak-slope ceiling, and zero NullRHI resources live at device shutdown.
    # Too slow for the default lanes, so it is opt-in:
    #   cmake -B build -DSPARK_ENABLE_SOAK_TESTS=ON
    #   ctest --test-dir build -L soak --no-tests=error
    # Linux shared-code evidence only; not the Windows package soak.
    option(SPARK_ENABLE_SOAK_TESTS "Register the ten-minute headless FPS/NullRHI soak (label soak)" OFF)
    if(SPARK_ENABLE_SOAK_TESTS AND UNIX AND NOT APPLE AND SPARK_HEADLESS_SUPPORT AND BUILD_GAME_MODULES AND
       TARGET SparkEngine AND TARGET SparkGameFPS)
        add_dependencies(SparkTests SparkEngine SparkGameFPS)
        add_test(
            NAME Soak_FPSHeadlessNullRHI
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_run_nullrhi_soak.py SoakNullRHIHeadlessSmoke)
        set_tests_properties(Soak_FPSHeadlessNullRHI PROPERTIES
            ENVIRONMENT "SPARK_HEADLESS_ENGINE=$<TARGET_FILE:SparkEngine>;SPARK_HEADLESS_MODULE=$<TARGET_FILE:SparkGameFPS>;SPARK_SOAK_DURATION=600"
            ENVIRONMENT_MODIFICATION "${_spark_soak_asan_quarantine}"
            LABELS "soak;linux;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 1500)
    endif()
endif()

# MOD-300/330/340/350/370/380: installed-package objective runs of the game
# modules. Tests/PackageSmoke/RunInstalledModuleObjective.cmake installs the
# runtime and samples components, then drives the installed SparkEngine and
# module image headless on NullRHI through Tests/PackageSmoke/ModuleObjectives/<Module>.cmake.
# The audit parser contract (which also lints every spec) always runs, and so
# does the short SparkGame showcase smoke on Linux. The experimental modules'
# real-time runs are opt-in:
#   cmake -B build -DSPARK_ENABLE_MODULE_PACKAGE_RUNS=ON
#   ctest --test-dir build -L module-package-run --no-tests=error
# They never carry the experimental-modules, stable-v1 or module-profile labels
# (build.yml pins those label sets to other families).
add_test(
    NAME ModulePackageObjectiveParserContract
    COMMAND ${CMAKE_COMMAND} -DSPARK_MODULE_OBJECTIVE_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledModuleObjective.cmake)
set_tests_properties(ModulePackageObjectiveParserContract PROPERTIES
    LABELS "module;package;unit"
    TIMEOUT 10)
function(spark_add_module_objective_test test_name module labels timeout)
    add_test(
        NAME ${test_name}
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -DSPARK_CONFIG=$<CONFIG>
            -DSPARK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/module-package-runs/${test_name}
            -DSPARK_MODULE_TARGET=${module}
            -DSPARK_MODULE_FILE_NAME=$<TARGET_FILE_NAME:${module}>
            -DSPARK_OBJECTIVE_SPEC=${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/ModuleObjectives/${module}.cmake
            -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledModuleObjective.cmake)
    set_tests_properties(${test_name} PROPERTIES
        LABELS "${labels}"
        RUN_SERIAL TRUE
        TIMEOUT ${timeout})
endfunction()
if(BUILD_GAME_MODULES AND NOT CMAKE_CROSSCOMPILING AND TARGET SparkEngine)
    # MOD-300: the installed SparkGame showcase loads its own localization tables and spawns (Linux headless).
    if(UNIX AND NOT APPLE AND SPARK_HEADLESS_SUPPORT AND TARGET SparkGame)
        spark_add_module_objective_test(SparkGameShowcase_PackagedSmoke SparkGame "module;package;integration" 300)
    endif()
    option(SPARK_ENABLE_MODULE_PACKAGE_RUNS
        "Register real-time installed-package objective runs for experimental game modules" OFF)
    if(SPARK_ENABLE_MODULE_PACKAGE_RUNS AND (WIN32 OR (UNIX AND NOT APPLE AND SPARK_HEADLESS_SUPPORT)))
        foreach(_spark_module_run IN ITEMS
                "ARPGDungeonPackage_ClearAndRestart|SparkGameARPG|arpg|600"
                "PlatformerCompletionPackage_Level0ThreeMinutes|SparkGamePlatformer|platformer|420"
                "RPGQuestSlicePackage_WolfHuntRestart|SparkGameRPG|rpg|900"
                "RTSSkirmishPackage_DeterministicVictory|SparkGameRTS|rts|1800"
                "RacingCompleteRacePackage_Circuit|SparkGameRacing|racing|900")
            string(REPLACE "|" ";" _spark_module_run_fields "${_spark_module_run}")
            list(GET _spark_module_run_fields 0 _spark_module_run_name)
            list(GET _spark_module_run_fields 1 _spark_module_run_target)
            list(GET _spark_module_run_fields 2 _spark_module_run_label)
            list(GET _spark_module_run_fields 3 _spark_module_run_timeout)
            if(TARGET ${_spark_module_run_target})
                spark_add_module_objective_test(${_spark_module_run_name} ${_spark_module_run_target}
                    "module;package;module-package-run;${_spark_module_run_label}" ${_spark_module_run_timeout})
            endif()
        endforeach()
    endif()
endif()

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
        # The Visual Studio generator's instance always ships a Ninja next to its CMake.
        set(_spark_mod310_vs_ninja
            "${CMAKE_GENERATOR_INSTANCE}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")
        if(EXISTS "${_spark_mod310_bundled_ninja}" AND
           NOT IS_DIRECTORY "${_spark_mod310_bundled_ninja}")
            set(_spark_mod310_ninja "${_spark_mod310_bundled_ninja}")
        elseif(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_GENERATOR_INSTANCE AND
               EXISTS "${_spark_mod310_vs_ninja}" AND NOT IS_DIRECTORY "${_spark_mod310_vs_ninja}")
            set(_spark_mod310_ninja "${_spark_mod310_vs_ninja}")
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

    # HEAD-220: writer/reader quicksave persistence across two fresh headless
    # NullRHI processes with an isolated LOCALAPPDATA. Source-tree evidence;
    # FPSPackage_InstalledRuntime runs the packaged WARP save/reload.
    add_test(
        NAME HeadlessSaveReload_FPSNullRHI
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGameFPS>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/headless-save-reload/$<CONFIG>"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkHeadlessFPSSaveReload.cmake
    )
    set_tests_properties(HeadlessSaveReload_FPSNullRHI PROPERTIES
        LABELS "nullrhi-headless;save;integration;process"
        RUN_SERIAL TRUE
        TIMEOUT 300)

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

    # SDK-240 / ASSET-220: generate SparkGeneratedGame with the installed spark-cli
    # from the sdk component's EmptyProject example, build it from an sdk+tools
    # install, require its ABI sidecar to match a host-built module's, and fail
    # on any resolved path back into the engine source or build tree.
    add_test(
        NAME SDKConsumer_InstalledEmptyProjectTemplate
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_CONFIG=$<CONFIG>"
            "-DSPARK_TEST_ROOT=${_spark_mod310_test_root}/t/$<CONFIG>"
            "-DSPARK_CONSUMER_GENERATOR=${_spark_mod310_consumer_generator}"
            "-DSPARK_CONSUMER_PLATFORM=${_spark_mod310_consumer_platform}"
            "-DSPARK_CONSUMER_TOOLSET=${_spark_mod310_consumer_toolset}"
            "-DSPARK_CONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DSPARK_CONSUMER_MAKE_PROGRAM=${_spark_mod310_consumer_make_program}"
            "-DSPARK_CONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
            "-DSPARK_REFERENCE_SIDECAR=$<TARGET_FILE:SparkGameFPS>.sparkabi"
            "-DSPARK_PYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
            -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledSDKTemplate.cmake)
    set_tests_properties(SDKConsumer_InstalledEmptyProjectTemplate PROPERTIES
        LABELS "sdk;package;installed-sdk;integration"
        RUN_SERIAL TRUE
        TIMEOUT 1800)

    if(SPARK_GAME_MODULES STREQUAL "SparkGameFPS")
        # ENG-220: a Vulkan-enabled Windows build hard-imports vulkan-1.dll,
        # which only a Vulkan GPU driver provides, so its package fails the PE
        # import-closure step of the two package tests below. That is an open
        # product gap, not a test fault: configure the FPS package tree with
        # ENABLE_VULKAN=OFF, as windows-shipping does.
        if(WIN32 AND SPARK_VULKAN_AVAILABLE)
            message(WARNING
                "FPSPackage_InstalledRuntime and FPSHeadlessPackage_NullRHISaveReload will fail their import-closure "
                "check: this tree links Vulkan, so SparkEngine.exe/SparkServer.exe hard-import vulkan-1.dll. "
                "Configure the FPS package tree with -DENABLE_VULKAN=OFF (wiki/gameplay-tools/Game-Packaging.md).")
        endif()

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

        # HEAD-220: the same staged package's NullRHI writer/reader save/reload
        # (the headless counterpart of the WARP phase above). Local evidence
        # still runs on a host with a GPU and display; the clean no-GPU host is
        # hosted-CI evidence.
        add_test(
            NAME FPSHeadlessPackage_NullRHISaveReload
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
                "-DSPARK_CONFIG=$<CONFIG>"
                "-DSPARK_TEST_ROOT=${_spark_mod310_test_root}/h/$<CONFIG>"
                -DSPARK_FPS_PACKAGE_MODE=headless-save-reload
                -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledFPSPackage.cmake)
        set_tests_properties(FPSHeadlessPackage_NullRHISaveReload PROPERTIES
            LABELS "nullrhi-headless;stable-v1;fps;package;save;integration"
            RUN_SERIAL TRUE
            TIMEOUT 600)

        # MOD-310: a fresh install prefix plays spawn -> move -> kill -> die ->
        # respawn -> score on D3D11/WARP through the fps_autoplay developer
        # command, scored by GameMode and RespawnSystem. Shipping (MinSizeRel)
        # does not register developer commands, so the test is not offered there.
        add_test(
            NAME FPSSinglePlayerSlice_InstalledArenaLoop
            CONFIGURATIONS Debug Release RelWithDebInfo
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
                "-DSPARK_CONFIG=$<CONFIG>"
                "-DSPARK_TEST_ROOT=${_spark_mod310_test_root}/a/$<CONFIG>"
                -DSPARK_FPS_PACKAGE_MODE=arena-loop
                -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledFPSPackage.cmake)
        set_tests_properties(FPSSinglePlayerSlice_InstalledArenaLoop PROPERTIES
            LABELS "module;stable-v1;fps;package;d3d11"
            RUN_SERIAL TRUE
            TIMEOUT 900)

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

# MOD-310: headless arena simulation. The parser contract is deterministic and
# runs everywhere. The Linux run launches the production headless host on
# NullRHI with the real SparkGameFPS module, which must load the authored
# level1 arena, bind RespawnSystem and GameMode to its default spawns and tick
# the match; the runner fails closed unless the SPARK_FPS_HEADLESS_ARENA record
# matches an independent parse of the staged scene. Source-tree evidence only.
add_test(
    NAME FPSHeadlessArenaParserContract
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_FPS_HEADLESS_ARENA_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkFPSHeadlessArena.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FPSHeadlessArenaParserContract PROPERTIES
    LABELS "module;fps;nullrhi-headless;unit"
    TIMEOUT 10)
# MOD-310: verdict contract of the installed arena-loop run (every host).
add_test(
    NAME FPSArenaLoopParserContract
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_FPS_ARENA_LOOP_PARSER_SELF_TEST=ON
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledFPSArenaLoop.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FPSArenaLoopParserContract PROPERTIES
    LABELS "module;fps;package;unit"
    TIMEOUT 30)
if(UNIX AND NOT APPLE AND SPARK_HEADLESS_SUPPORT AND BUILD_GAME_MODULES AND
   TARGET SparkEngine AND TARGET SparkGameFPS)
    add_dependencies(SparkTests SparkEngine SparkGameFPS)
    add_test(
        NAME FPSSinglePlayerSlice_HeadlessArenaLinux
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_EXECUTABLE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_GAME_MODULE=$<TARGET_FILE:SparkGameFPS>"
            "-DSPARK_WORKING_DIRECTORY=$<TARGET_FILE_DIR:SparkEngine>"
            "-DSPARK_ARENA_SCENE=$<TARGET_FILE_DIR:SparkEngine>/Assets/Scenes/level1.scene"
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSparkFPSHeadlessArena.cmake
    )
    set_tests_properties(FPSSinglePlayerSlice_HeadlessArenaLinux PROPERTIES
        LABELS "module;fps;nullrhi-headless;linux;integration"
        RUN_SERIAL TRUE
        TIMEOUT 120)
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

# SDK-240: the .sparkabi sidecar's descriptor format/size/runtime ABI version
# are derived from Spark/ModuleABI.h, not passed as literals.
add_test(
    NAME SparkModuleABISidecarDerivation
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_GAME_MODULE_CMAKE=${CMAKE_SOURCE_DIR}/cmake/SparkGameModule.cmake"
        "-DSPARK_SDK_INCLUDE=${CMAKE_SOURCE_DIR}/SparkSDK/Include"
        "-DFIXTURE=${CMAKE_BINARY_DIR}/sdk240-sidecar-derivation/$<CONFIG>"
        "-DCONSUMER_GENERATOR=${CMAKE_GENERATOR}"
        "-DCONSUMER_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCONSUMER_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCONSUMER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
        -P ${CMAKE_SOURCE_DIR}/Tests/SDK/TestSparkModuleABISidecarDerivation.cmake)
set_tests_properties(SparkModuleABISidecarDerivation PROPERTIES
    LABELS "sdk;module;abi;unit"
    TIMEOUT 300)

# SDK-240: incompatible-module rejection names both sides' values and reaches the Logger.
add_test(NAME SDK240ModuleCompatibilityDiagnostics
    COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(SDK240ModuleCompatibilityDiagnostics PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=SDK240_;SPARK_TEST_EXPECT_COUNT=3"
    LABELS "sdk;module;abi;unit"
    RUN_SERIAL TRUE
    TIMEOUT 60)

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

# SDK-240: the installed-template runner's sidecar and boundary checks must
# reject every broken fixture by name (runs everywhere, no install or build).
add_test(
    NAME SDKConsumer_TemplateRunnerContract
    COMMAND ${CMAKE_COMMAND}
        -DSPARK_SDK_TEMPLATE_SELF_TEST=ON
        "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/sdk-template-contract"
        -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/RunInstalledSDKTemplate.cmake)
set_tests_properties(SDKConsumer_TemplateRunnerContract PROPERTIES
    LABELS "sdk;package;unit"
    TIMEOUT 60)

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

# ----------------------------------------------------------------------------
# ASSET-220 installed-package consumer (Linux, opt-in, local evidence)
# ----------------------------------------------------------------------------
# Installs this build into a build-owned prefix, then configures, builds and
# ctests Tests/PackageSmoke against only that prefix -- the Linux counterpart of
# build.yml's per-PR Windows installed-SDK consumer step -- and fails if the
# consumer resolved any header or library from the source or build tree. The
# engine tree may use any generator; the consumer always uses Unix Makefiles
# (needs `make`), whose depfiles and link.txt the boundary proof reads.
# It needs a complete build and takes minutes, so it is off by default:
#   cmake -B build -DSPARK_ENABLE_PACKAGE_CONSUMER_TESTS=ON
#   ctest --test-dir build -L package-consumer-linux --no-tests=error
# Its result is local, non-hosted evidence. The leak detector it relies on is
# always tested by PackageConsumer_SourceTreeBoundaryDetection below.
option(SPARK_ENABLE_PACKAGE_CONSUMER_TESTS
    "Register the slow Linux installed-package consumer test (PackageConsumer_*)" OFF)
if(SPARK_ENABLE_PACKAGE_CONSUMER_TESTS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "SPARK_ENABLE_PACKAGE_CONSUMER_TESTS registers the native Linux consumer lane only; "
            "the Windows installed-SDK consumer runs in .github/workflows/build.yml")
    endif()
    # A single-config tree configured without CMAKE_BUILD_TYPE has an empty
    # $<CONFIG>; the consumer then builds as Release. Seven stages (install,
    # configure, build, ctest, then generate, configure and build the
    # spark-cli generated game) each get SPARK_STAGE_TIMEOUT; 7 x 480 s plus
    # slack stays inside the 3600 s CTest TIMEOUT (the CI-110 policy ceiling),
    # so a stage limit with its log, not a CTest kill, reports a hang.
    set(_spark_consumer_config "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Release>")
    set(_spark_consumer_stage_timeout 480)
    add_test(NAME PackageConsumer_LinuxInstalledSDK
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_CONFIG=${_spark_consumer_config}"
            "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/package-consumer-linux/${_spark_consumer_config}"
            "-DSPARK_STAGE_TIMEOUT=${_spark_consumer_stage_timeout}"
            "-DSPARK_CONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DSPARK_CONSUMER_CTEST=${CMAKE_CTEST_COMMAND}"
            "-DSPARK_PYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
            -P "${CMAKE_SOURCE_DIR}/cmake/RunInstalledPackageConsumer.cmake")
    set_tests_properties(PackageConsumer_LinuxInstalledSDK PROPERTIES
        LABELS "package-consumer-linux"
        RUN_SERIAL TRUE
        TIMEOUT 3600)
endif()
add_test(NAME PackageConsumer_SourceTreeBoundaryDetection
    COMMAND ${CMAKE_COMMAND}
        "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/package-consumer-boundary-ctest"
        -P "${CMAKE_SOURCE_DIR}/cmake/TestPackageConsumerBoundary.cmake")
set_tests_properties(PackageConsumer_SourceTreeBoundaryDetection PROPERTIES
    LABELS "package;sdk;unit"
    TIMEOUT 60)

# ----------------------------------------------------------------------------
# PLT-210 installed Linux tree runtime closure
# ----------------------------------------------------------------------------
# Installs this build into a build-owned prefix, then fails on any RUNPATH or
# NEEDED that reaches the source/build tree or leaves the prefix for anything
# but a host system library, on any glibc/libstdc++/libgcc_s symbol-version need
# above the Ubuntu 24.04 (OD-10) ceiling, on any module sidecar that no longer hashes its
# installed image, and on an installed SparkEngine -headless + SparkGameFPS run
# (cwd=/, empty environment, fresh HOME/XDG) without clean lifecycle records.
# The same script verifies a linux-shipping install standalone (see its header).
# LinuxInstalledRuntime_ClosureDetection proves each rule on real defective
# copies of the build-tree images.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING AND SPARK_HEADLESS_SUPPORT AND
   TARGET SparkEngine AND TARGET SparkGameFPS)
    set(_spark_linux_runtime_config "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Release>")
    add_test(NAME VerifyLinuxInstalledRuntime
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_CONFIG=$<CONFIG>"
            "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/linux-installed-runtime/${_spark_linux_runtime_config}"
            -P "${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/VerifyLinuxInstalledRuntime.cmake")
    set_tests_properties(VerifyLinuxInstalledRuntime PROPERTIES
        LABELS "package;linux;integration"
        RUN_SERIAL TRUE
        TIMEOUT 600)

    add_test(NAME LinuxInstalledRuntime_ClosureDetection
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_LINUX_RUNTIME_CLOSURE_SELF_TEST=ON
            "-DSPARK_FIXTURE_ENGINE=$<TARGET_FILE:SparkEngine>"
            "-DSPARK_FIXTURE_MODULE=$<TARGET_FILE:SparkGameFPS>"
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_FORBIDDEN_ROOTS=${CMAKE_BINARY_DIR}"
            "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/linux-installed-runtime-detection/${_spark_linux_runtime_config}"
            -P "${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/VerifyLinuxInstalledRuntime.cmake")
    set_tests_properties(LinuxInstalledRuntime_ClosureDetection PROPERTIES
        LABELS "package;linux;unit"
        TIMEOUT 120)

    # PLT-210: the same closure, lifecycle and isolation checks on the extracted
    # CPack TGZ archive (the package a user downloads) instead of an install.
    add_test(NAME VerifyLinuxPackagedRuntime
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_PACKAGE_ARCHIVE_BUILD=ON
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_CONFIG=$<CONFIG>"
            "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/linux-packaged-runtime/${_spark_linux_runtime_config}"
            -P "${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/VerifyLinuxInstalledRuntime.cmake")
    set_tests_properties(VerifyLinuxPackagedRuntime PROPERTIES
        LABELS "package;linux;integration"
        RUN_SERIAL TRUE
        TIMEOUT 1200)

    # Packaged mode must fail, not pass vacuously, when there is nothing to package.
    add_test(NAME LinuxPackagedRuntime_RequiresCPackConfig
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_PACKAGE_ARCHIVE_BUILD=ON
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/linux-packaged-runtime-no-cpack"
            -P "${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/VerifyLinuxInstalledRuntime.cmake")
    set_tests_properties(LinuxPackagedRuntime_RequiresCPackConfig PROPERTIES
        LABELS "package;linux;unit"
        PASS_REGULAR_EXPRESSION "has[ \n]+no[ \n]+CPackConfig\\.cmake"
        TIMEOUT 60)
endif()


find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME FPSSceneMaterialContract
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_fps_scene_material_contract.py)
set_tests_properties(FPSSceneMaterialContract PROPERTIES
    LABELS "module;stable-v1;fps;asset;unit"
    TIMEOUT 30)

# RDY-020: every asset the FPS module source and level1.scene reference is
# declared in Assets/assets.integrity.json with exact case and resolves through
# the production FPSAssets root; case-altered and traversal spellings fail.
add_test(NAME AssetManifest_FPSReferences COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(AssetManifest_FPSReferences PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=AssetManifest_;SPARK_TEST_EXPECT_COUNT=2"
    LABELS "module;stable-v1;fps;asset;unit"
    TIMEOUT 60)
add_test(NAME PathCase_FPSReferences COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(PathCase_FPSReferences PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=PathCase_;SPARK_TEST_EXPECT_COUNT=2"
    LABELS "module;stable-v1;fps;asset;unit"
    TIMEOUT 60)

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

# MOD-360 / RDY-020: tools/check-module-asset-refs.py fails closed on a missing, case-altered,
# unrecorded or drifted asset reference in every module and on a run-time-composed path in an
# enforced module, and all 11 module reference records verify clean (Tests/Tools/test_check_module_asset_refs.py).
add_test(
    NAME OpenWorldAssets_ReferenceCheckFailsClosed
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_check_module_asset_refs.py)
set_tests_properties(OpenWorldAssets_ReferenceCheckFailsClosed PROPERTIES
    LABELS "module;openworld;asset;unit;python"
    TIMEOUT 60)

add_test(NAME PackageAssets_RepositoryIsolationContract
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/asset_repository_isolation_tests.py)
set_tests_properties(PackageAssets_RepositoryIsolationContract PROPERTIES
    LABELS "stable-v1;asset;package;unit;python"
    TIMEOUT 60)

option(SPARK_ENABLE_PACKAGE_REPEATABILITY_TESTS "Register installed CPack tree repeatability qualification" OFF)
if(TARGET SparkEngine AND SPARK_ENABLE_PACKAGE_REPEATABILITY_TESTS)
    add_test(NAME PackageInstall_Repeatability
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DSPARK_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_CONFIG=$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Release>"
            "-DSPARK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-repeatability/$<CONFIG>"
            -P ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/installer_repeatability.cmake)
    set_tests_properties(PackageInstall_Repeatability PROPERTIES
        LABELS "package;installer;integration"
        RUN_SERIAL TRUE
        TIMEOUT 1200)
endif()

add_test(NAME PackageAssets_ProvenanceMetadata
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/asset_provenance_tests.py)
set_tests_properties(PackageAssets_ProvenanceMetadata PROPERTIES
    LABELS "stable-v1;asset;package;unit;python"
    TIMEOUT 60)

# RDY-020: opt-in installed-consumer qualification requires bubblewrap/strace.
# Missing tools or namespace support fail; an empty cwd alone is not isolation.
if(SPARK_ENABLE_PACKAGE_CONSUMER_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   NOT CMAKE_CROSSCOMPILING AND SPARK_HEADLESS_SUPPORT AND TARGET SparkEngine AND TARGET SparkGameFPS)
    add_test(NAME PackageAssets_RepositoryUnreachable
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/PackageSmoke/asset_repository_isolation.py
            --source-root ${CMAKE_SOURCE_DIR}
            --build-dir ${CMAKE_BINARY_DIR}
            --config "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Release>"
            --test-root ${CMAKE_CURRENT_BINARY_DIR}/asset-isolation/$<CONFIG>)
    set_tests_properties(PackageAssets_RepositoryUnreachable PROPERTIES
        LABELS "stable-v1;asset;package-consumer-linux;integration"
        RUN_SERIAL TRUE
        TIMEOUT 600)
endif()

# CI-110: documented ctest commands (work-item commands[] and wiki/advanced/Testing.md)
# must select enabled tests whose executables exist in this configured tree. The
# checker's fixture tests prove a disabled-only, unbuilt or empty selection fails.
add_test(
    NAME DocumentedTestCommands_CheckerFailsClosed
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_check_documented_selectors.py)
set_tests_properties(DocumentedTestCommands_CheckerFailsClosed PROPERTIES
    LABELS "ci;policy;python;unit"
    TIMEOUT 120)
# Registered only in the preset trees the documented commands target (pinned by
# DocumentedCommandInventoryTests), so every registration has commands to check;
# any non-zero exit, including 2 for "nothing applied", fails the test.
set(SPARK_DOCUMENTED_SELECTOR_TREES linux-gcc-release windows-release)
get_filename_component(_spark_documented_tree_parent "${CMAKE_BINARY_DIR}" DIRECTORY)
get_filename_component(_spark_documented_tree_name "${CMAKE_BINARY_DIR}" NAME)
if(_spark_documented_tree_parent STREQUAL "${CMAKE_SOURCE_DIR}/build"
   AND _spark_documented_tree_name IN_LIST SPARK_DOCUMENTED_SELECTOR_TREES)
    add_test(
        NAME DocumentedTestCommands_SelectBuiltTests
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/tools/site-data/check_documented_selectors.py
            --build-dir ${CMAKE_BINARY_DIR} --config $<CONFIG> --ctest ${CMAKE_CTEST_COMMAND})
    set_tests_properties(DocumentedTestCommands_SelectBuiltTests PROPERTIES
        LABELS "ci;policy;python;unit"
        TIMEOUT 300)
endif()
# MOD-360: the real OWEngineSystems music tracks and OWWorldSetup area manifests name existing files.
add_test(NAME OpenWorldAssets_RegisteredAssetsExist COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
set_tests_properties(OpenWorldAssets_RegisteredAssetsExist PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME=OpenWorldAssets_;SPARK_TEST_EXPECT_COUNT=1"
    LABELS "module;openworld;asset;unit"
    TIMEOUT 60)

# MOD-300: tools/check-module-positioning.py fails when a public doc labels SparkGame a
# game or an FPS, or its README loses the showcase disclaimers (Tests/Tools/test_check_module_positioning.py).
add_test(
    NAME ModulePositioning_SparkGameLabelIsShowcase
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_check_module_positioning.py)
set_tests_properties(ModulePositioning_SparkGameLabelIsShowcase PROPERTIES
    LABELS "module;docs;unit;python"
    TIMEOUT 60)

# INST-130: SparkInstaller/README.md flags and exit codes equal what main.cpp,
# --help and the installer return, and a flag compiled out of the published
# build-installer binary is marked source-build only (tools/installer/check_installer_claims.py).
add_test(
    NAME Installer_DocClaims
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_installer_claims.py -v)
set_tests_properties(Installer_DocClaims PROPERTIES
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    LABELS "installer;python"
    TIMEOUT 60)

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
# GOV-400: every governed public claim surface stays inside the declared
# non-OSI license classification. Runs on all hosts (no git/POSIX needed).
add_test(
    NAME PublicWording_LegalSurfaces
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_public_wording.py -v
        LegalPublicWordingLiveTests LegalPublicWordingRuleTests)
set_tests_properties(PublicWording_LegalSurfaces PROPERTIES
    LABELS "governance;unit;python"
    TIMEOUT 60)
# OD-12 (PLT-230/240/250): mobile, OpenXR and console stay unsupported/blocked in
# the readiness contract, and no public surface claims support for them.
add_test(
    NAME PublicWording_DeferredPlatforms
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_public_wording.py -v
        DeferredPlatformWordingTests DeferredPlatformSupportTests)
set_tests_properties(PublicWording_DeferredPlatforms PROPERTIES
    LABELS "governance;platform;unit;python"
    TIMEOUT 60)
# OD-10/OD-11 (PLT-210/PLT-220): the experimental Linux and macOS rows stay
# experimental in the readiness contract and no public surface calls them supported.
add_test(
    NAME PublicWording_ExperimentalPlatforms
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_public_wording.py -v
        ExperimentalPlatformWordingTests ExperimentalPlatformSupportTests)
set_tests_properties(PublicWording_ExperimentalPlatforms PROPERTIES
    LABELS "governance;platform;unit;python"
    TIMEOUT 60)
# PLT-250: no workflow, CMake file, public doc or engine source implies console
# certification (console runners/options/presets, unqualified certification wording).
add_test(
    NAME PublicWording_ConsoleCertification
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_public_wording.py -v
        ConsoleCertificationImplicationTests)
set_tests_properties(PublicWording_ConsoleCertification PROPERTIES
    LABELS "governance;platform;unit;python"
    TIMEOUT 60)

add_test(
    NAME PublicWording_GovernancePolicy
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_governance.py -v)
set_tests_properties(PublicWording_GovernancePolicy PROPERTIES
    LABELS "governance;site-data;unit;python"
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    TIMEOUT 60)
# PLT-220: SPARK_MACOS_MIN_VERSION, every workflow deployment target, the
# documented macOS floor/runner, and the Mach-O minimum-version parser.
add_test(
    NAME MacOSBaseline_MinimumVersionParity
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_macos_min_version.py -v)
set_tests_properties(MacOSBaseline_MinimumVersionParity PROPERTIES
    LABELS "platform;unit;python"
    TIMEOUT 60)
# CI-110 / PERF-100: the committed golden baselines (manifest schema, PNG
# hashes and structure, no orphans, lane kScenes parity) on every host; the
# GPU lanes SparkOpenGLGoldenTests / VulkanGoldenTests compare pixels on Linux.
add_test(
    NAME GoldenImage_ManifestIntegrity
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_golden_manifest.py -v)
set_tests_properties(GoldenImage_ManifestIntegrity PROPERTIES
    LABELS "rendering;golden;unit;python"
    TIMEOUT 60)
# PERF-100: tools/perf-budget/check_golden_review.py, the diff gate that the
# required performance-budget-governance job runs on every PR and push, rejects
# a baseline or threshold change that inherits or downgrades its review record.
add_test(
    NAME GoldenImage_ReviewGate
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_golden_review_gate.py -v)
set_tests_properties(GoldenImage_ReviewGate PROPERTIES
    LABELS "governance;golden;python"
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    TIMEOUT 120)

# CI-100/CI-110: exercise the real SARIF gate and required workflow contracts.
add_test(NAME CI_AnalysisResultsFailClosed
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/.github/scripts/test-check-analysis-results.py)
set_tests_properties(CI_AnalysisResultsFailClosed PROPERTIES
    LABELS "ci;policy;python;unit"
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    TIMEOUT 120)
add_test(NAME CI_PerformanceWorkflowContract
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/.github/scripts/test-ci-perf-contract.py)
set_tests_properties(CI_PerformanceWorkflowContract PROPERTIES
    LABELS "ci;policy;python;unit"
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    TIMEOUT 120)
add_test(NAME PerformanceBudget_Hardening
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_perf_budget_hardening -v)
set_tests_properties(PerformanceBudget_Hardening PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS "ci;policy;performance;python;unit"
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    TIMEOUT 180)

# TF-110: TERRAFRONT multi-client harness (Tools/Terrafront/multiclient.py). The
# unit test covers the audit parser, the convergence comparator and every
# scenario verdict on every host; each TerrafrontMultiClient_<Scenario> runs a
# real dedicated server plus two headless clients (onboard + spawn + walk,
# combat kill/respawn, forged loadouts, reconnect, territory flips, vehicle life
# cycle) and fails unless both clients agree with the server at every checkpoint
# and the scenario's authoritative outcome holds. ContinentMismatchRefused (TF-120)
# runs one client booted on another continent than the server's: it must refuse
# the server's TF_ContinentIdentity and never spawn.
# The process runs are timing-sensitive (three processes on one host) and have
# not yet run on hosted Linux or 2-core runners, so they are opt-in and stay out of the
# required full-ctest lanes until a dedicated job enables it:
#   cmake -B build -DSPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS=ON
#   ctest --test-dir build -L terrafront-multiclient --no-tests=error
add_test(
    NAME TerrafrontMultiClient_Harness
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_terrafront_multiclient.py -v)
set_tests_properties(TerrafrontMultiClient_Harness PROPERTIES
    FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
    LABELS "terrafront;unit;python"
    TIMEOUT 120)
option(SPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS
    "Register the TF-110 dedicated server + two-client run (label terrafront-multiclient)" OFF)
if(SPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS AND ENABLE_NETWORKING AND SPARK_HEADLESS_SUPPORT AND
   TARGET SparkEngine AND TARGET SparkGameMMOFPS)
    add_dependencies(SparkTests SparkEngine SparkGameMMOFPS)
    # <CTest name suffix>:<multiclient.py scenario>
    foreach(_tf_run IN ITEMS
            OnboardSpawnMove:onboard_spawn_move
            CombatKillRespawn:combat_kill_respawn
            ForgedStateRejectedAndAudited:forged_state
            ReconnectRestoresAllowedState:reconnect
            TerritoryReplicates:territory
            VehicleLifecycle:vehicle_lifecycle
            ContinentMismatchRefused:continent_mismatch)
        string(REPLACE ":" ";" _tf_run "${_tf_run}")
        list(GET _tf_run 0 _tf_name)
        list(GET _tf_run 1 _tf_scenario)
        add_test(
            NAME TerrafrontMultiClient_${_tf_name}
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tools/Terrafront/multiclient.py
                --engine $<TARGET_FILE:SparkEngine>
                --module $<TARGET_FILE:SparkGameMMOFPS>
                --scenario ${_tf_scenario}
                --workdir ${CMAKE_BINARY_DIR}/Testing/terrafront-multiclient/${_tf_scenario})
        set_tests_properties(TerrafrontMultiClient_${_tf_name} PROPERTIES
            LABELS "terrafront-multiclient;network;process"
            RUN_SERIAL TRUE
            TIMEOUT 240)
    endforeach()
    set(_tf_harness ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tools/Terrafront/multiclient.py
        --engine $<TARGET_FILE:SparkEngine> --module $<TARGET_FILE:SparkGameMMOFPS>)
    set(_tf_runs ${CMAKE_BINARY_DIR}/Testing/terrafront-multiclient)
    # TF-110: the same checkpoints under 80 ms / 20 ms / 3% loss / 2% dup / 3%
    # reorder with a fixed seed; fails unless impairment was live in every process.
    add_test(
        NAME TerrafrontMultiClient_ImpairedConvergence
        COMMAND ${_tf_harness} --scenario onboard_spawn_move combat_kill_respawn territory
            --impair 80,20,0.03,2,3 --seed 20260927 --timeout 150 --workdir ${_tf_runs}/impaired)
    set_tests_properties(TerrafrontMultiClient_ImpairedConvergence PROPERTIES
        LABELS "terrafront-multiclient;network;process"
        RUN_SERIAL TRUE
        TIMEOUT 360)
    # TF-110: 120 s soak against Tools/Terrafront/soak_budgets.json (provisional
    # guards). The 30-minute run is the same command with --soak-seconds 1800.
    add_test(
        NAME TerrafrontSoak_Short
        COMMAND ${_tf_harness} --soak-seconds 120 --timeout 280 --workdir ${_tf_runs}/soak)
    set_tests_properties(TerrafrontSoak_Short PROPERTIES
        LABELS "terrafront-soak;process"
        RUN_SERIAL TRUE
        TIMEOUT 300)
    # TF-120: a hard-killed server restarts on the same save root with its
    # authoritative state, after an explicit save and after the debounced commit.
    foreach(_tf_run IN ITEMS
            ColdRestartRestoresAuthoritativeState:cold_restart
            UngracefulKillRestoresLastCommit:ungraceful_restart)
        string(REPLACE ":" ";" _tf_run "${_tf_run}")
        list(GET _tf_run 0 _tf_name)
        list(GET _tf_run 1 _tf_scenario)
        add_test(
            NAME TerrafrontRestart_${_tf_name}
            COMMAND ${_tf_harness} --scenario ${_tf_scenario} --timeout 280 --workdir ${_tf_runs}/${_tf_scenario})
        set_tests_properties(TerrafrontRestart_${_tf_name} PROPERTIES
            LABELS "terrafront-multiclient;persistence;process"
            RUN_SERIAL TRUE
            TIMEOUT 300)
    endforeach()
endif()

# SEC-110: lock-derived SPDX 2.3 SBOM generation (commit and dependency-lock
# binding, --check reproducibility) and package-inventory reconciliation
# against forged, unlocked, and missing third-party payload.
add_test(
    NAME SupplyChain_SbomAndPackageReconciliation
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_generate_sbom -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(SupplyChain_SbomAndPackageReconciliation PROPERTIES
    LABELS "governance;security;unit;python"
    TIMEOUT 300)

# SEC-110: release.yml reconciles every package with the lock after install and
# before CPack, and the release job requires a passing report per published
# package plus the reproducible lock-bound SBOM before collecting assets.
add_test(
    NAME SupplyChain_ReleaseReconciliationWiring
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_release_supply_chain_wiring.py)
set_tests_properties(SupplyChain_ReleaseReconciliationWiring PROPERTIES
    LABELS "supply-chain;release;security;python"
    TIMEOUT 120)

# SEC-110: repository secret scan (tools/check-secret-scan.py) against planted
# tokens, PEM keys and credentials, symlinks, oversize files, and exceptions that
# are expired, stale, wildcarded, directory-scoped, count-exceeded or invalid. The
# full-tree scan itself runs in build.yml's required secret-scan job.
add_test(
    NAME SecretScan_RepositoryGate
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_check_secret_scan -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(SecretScan_RepositoryGate PROPERTIES
    LABELS "governance;security;unit;python"
    TIMEOUT 300)

# The MinGW toolchain's DirectXMath fetch re-verifies the pinned SHA-256 on every
# configure: a mismatched download or a tampered cached archive never reaches the
# include path, even on the configure after the failing one.
add_test(
    NAME SupplyChain_MinGWDirectXMathFetch
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_mingw_dxmath_fetch -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(SupplyChain_MinGWDirectXMathFetch PROPERTIES
    ENVIRONMENT "SPARK_CMAKE=${CMAKE_COMMAND}"
    LABELS "supply-chain;security;unit;python"
    TIMEOUT 300)

# The root-run Wine/MinGW setup scripts install packages only through APT's
# signed-index verification and build Wine only inside a private directory. The
# behavioral half drives the real shell functions with stub commands and needs
# POSIX permissions, so it runs on the Linux/macOS lanes.
add_test(
    NAME SupplyChain_WineSetupScriptIntegrity
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_wine_setup_script_integrity -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(SupplyChain_WineSetupScriptIntegrity PROPERTIES
    LABELS "supply-chain;security;unit;python"
    TIMEOUT 300)

# BLD-100 / OD-04: the stable-v1 x86-64 CPU floor is SSE4.2 + POPCNT.
# CpuFloor_IsaBaselineChecker proves tools/check_isa_baseline.py rejects
# AVX/AVX2/AVX-512/FMA/F16C/BMI/LZCNT/MOVBE and the legacy-encoded AES-NI,
# PCLMULQDQ, SHA, GFNI, RDRAND, RDSEED and ADX instructions in real ELF and PE
# fixtures and passes floor-only builds. CpuFloor_IsaBaseline disassembles this
# tree's linked engine, editor, server and game-module images and fails on any
# above-floor instruction that reached them (SparkCpuFloor.cmake only sees flags).
# The image scan is ELF-only for now. tools/check_isa_baseline.py can scan an
# MSVC PE against its matching PDB, exempting only the reviewed CPUID-dispatched
# MSVC runtime procedure ranges (Tests/Tools/test_pe_isa_ranges.py), but real
# MSVC Release images still carry unreviewed above-floor code (libsodium's
# AVX2/AES-NI variants, which the MSVC branch of SparkLibsodium.cmake compiles,
# the UCRT's _Avx2WmemEnabled wmemchr/wmemcmp, <bit>'s LZCNT dispatch and the
# symbol-less vector_algorithms ranges; docs/readiness/BLD-100-PROGRESS.md).
# Registering it for Windows before those are resolved would fail every run.
add_test(
    NAME CpuFloor_IsaBaselineChecker
    COMMAND ${Python3_EXECUTABLE} -B -m unittest
        Tests.Tools.test_check_isa_baseline Tests.Tools.test_pe_isa_ranges -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(CpuFloor_IsaBaselineChecker PROPERTIES
    LABELS "build;shipping;unit;python"
    TIMEOUT 300)

# ENG-220: tools/pe_import_closure.py fails a staged Windows package whose
# EXE/DLL imports or delay-imports a DLL that is neither beside it, an API set,
# nor an allowlisted System32 DLL. Synthetic in-memory PE fixtures, so it runs
# on every host; FPSPackage_InstalledRuntime runs the checker on the real stage.
add_test(
    NAME PEImportClosure_Contract
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_pe_import_closure -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(PEImportClosure_Contract PROPERTIES
    LABELS "package;unit;python"
    TIMEOUT 120)

include("${CMAKE_SOURCE_DIR}/cmake/SparkCpuFloor.cmake")
spark_cpu_floor_enforced(_spark_isa_floor_enforced)
if(_spark_isa_floor_enforced AND CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
    find_program(SPARK_ISA_DISASSEMBLER NAMES objdump llvm-objdump)
    set(_spark_isa_images "")
    foreach(_spark_isa_target IN ITEMS SparkEngine SparkEditor SparkServer ${_SPARK_GAME_MODULE_NAMES})
        if(TARGET ${_spark_isa_target})
            list(APPEND _spark_isa_images "$<TARGET_FILE:${_spark_isa_target}>")
        endif()
    endforeach()
    if(SPARK_ISA_DISASSEMBLER AND _spark_isa_images)
        add_test(
            NAME CpuFloor_IsaBaseline
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/check_isa_baseline.py
                --objdump ${SPARK_ISA_DISASSEMBLER} ${_spark_isa_images})
        set_tests_properties(CpuFloor_IsaBaseline PROPERTIES
            LABELS "build;shipping;integration"
            TIMEOUT 600)
    else()
        message(STATUS "CpuFloor_IsaBaseline not registered: no objdump/llvm-objdump or no shipped image target")
    endif()
endif()

# BLD-100: ShippingManifest_SymbolManifestTool proves tools/shipping_symbol_manifest.py
# on real fixtures: gcc ELF images split by the production link launcher
# (cmake/SparkSplitDebugLink.cmake) and clang/lld-link PE DLLs with PDBs,
# cross-checked against llvm-readobj and llvm-pdbutil, and every rejection
# (missing, mismatched, duplicate or orphaned symbols, unstripped images,
# absolute PDB paths, symbols in the runtime tree). It also keeps the root
# SPARK_SHIPPED_IMAGE_TARGETS list in step with the install(TARGETS) rules.
add_test(
    NAME ShippingManifest_SymbolManifestTool
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_shipping_symbol_manifest -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(ShippingManifest_SymbolManifestTool PROPERTIES
    LABELS "build;shipping;unit;python"
    TIMEOUT 300)

# BLD-100: ReproducibleBuild_CompareTool proves tools/compare_build_outputs.py
# on real gcc ELF, crafted PE and ar fixtures (build-directory DWARF leaks,
# code and timestamp differences, closed manifest schema, failure codes).
# ReproducibleBuild_LinuxToolTargets copies this source tree to two
# differently named and nested directories, configures each with the
# linux-shipping settings (MinSizeRel, split private symbols) plus this tree's
# compiler, ENABLE_LTO and cached C/C++/linker flags, so a lane proves the
# toolchain it ships (the Clang lane: LTO off, libc++), builds SparkCooker
# without any compiler cache, and requires
# equivalent bin/ outputs: the stripped image and its .debug file. lib/ is not
# compared: GCC's LTO IR in static-library members records the build
# directory whatever the prefix maps say, so those members differ between
# trees (they do not reach the linked image). The copies carry no .git, so
# SPARK_STRICT_DEPS stays OFF: its third-party audit resolves submodule
# gitlinks through git. It runs two clean builds, serially.
add_test(
    NAME ReproducibleBuild_CompareTool
    COMMAND ${Python3_EXECUTABLE} -B -m unittest Tests.Tools.test_compare_build_outputs -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(ReproducibleBuild_CompareTool PROPERTIES
    LABELS "build;reproducibility;unit;python"
    TIMEOUT 300)
if(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF" AND CMAKE_OBJCOPY)
    add_test(
        NAME ReproducibleBuild_LinuxToolTargets
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/compare_build_outputs.py two-tree
            --source ${CMAKE_SOURCE_DIR}
            --work ${CMAKE_BINARY_DIR}/reproducible-build-trees
            --exclude ${CMAKE_SOURCE_DIR}/build
            --exclude ${CMAKE_BINARY_DIR}
            --cmake ${CMAKE_COMMAND}
            --jobs 4
            --target SparkCooker
            --scan bin
            --
            -G ${CMAKE_GENERATOR}
            -DCMAKE_BUILD_TYPE=MinSizeRel
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            # two-tree scrubs CFLAGS/CXXFLAGS/LDFLAGS, so this lane's own LTO
            # choice and cached flags (the Clang lane's libc++) travel here.
            -DENABLE_LTO=${ENABLE_LTO}
            "-DCMAKE_C_FLAGS=$CACHE{CMAKE_C_FLAGS}"
            "-DCMAKE_CXX_FLAGS=$CACHE{CMAKE_CXX_FLAGS}"
            "-DCMAKE_EXE_LINKER_FLAGS=$CACHE{CMAKE_EXE_LINKER_FLAGS}"
            "-DCMAKE_SHARED_LINKER_FLAGS=$CACHE{CMAKE_SHARED_LINKER_FLAGS}"
            -DSTRIP_DEBUG_SYMBOLS=ON
            -DSPARK_NATIVE_ARCH=OFF
            -DENABLE_EDITOR=OFF
            -DENABLE_PROFILING=OFF
            -DENABLE_CONSOLE_IN_SHIPPING=OFF
            -DENABLE_DEVCOMMANDS_IN_SHIPPING=OFF
            -DENABLE_ANGELSCRIPT=OFF
            -DBUILD_GAME_MODULES=OFF
            -DBUILD_TESTS=OFF)
    set_tests_properties(ReproducibleBuild_LinuxToolTargets PROPERTIES
        LABELS "build;reproducibility;shipping;integration"
        RUN_SERIAL TRUE
        TIMEOUT 3600)
endif()
# ReproducibleBuild_WindowsToolTargets is the MSVC counterpart: two source
# copies configured with this tree's generator and the windows-shipping cache
# values, SparkCooker built in MinSizeRel (the only configuration with /Brepro
# and /d1trimfile among the ones Shipping uses) without any compiler cache, and
# bin/MinSizeRel and lib/MinSizeRel compared: PE sections, COFF timestamp, RSDS
# identity and static-library members. OD-24 normalizes only the equal-length
# build-root strings inside library members' CodeView .debug$S sections. PDBs
# are not compared (not byte-reproducible; the image's RSDS record names them).
# It runs only under `ctest -C MinSizeRel`, so the Debug/Release CI lanes do not
# pay for two clean MSVC builds; the reproducibility-windows job compares two
# hosted windows-shipping builds.
if(MSVC)
    set(_spark_repro_generator_args -G ${CMAKE_GENERATOR})
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND _spark_repro_generator_args -A ${CMAKE_GENERATOR_PLATFORM})
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND _spark_repro_generator_args -T ${CMAKE_GENERATOR_TOOLSET})
    endif()
    if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
        # Ninja and Makefile generators take the compiler from the cache, not the IDE.
        list(APPEND _spark_repro_generator_args
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
    endif()
    add_test(
        NAME ReproducibleBuild_WindowsToolTargets
        CONFIGURATIONS MinSizeRel
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/compare_build_outputs.py two-tree
            --source ${CMAKE_SOURCE_DIR}
            --work ${CMAKE_BINARY_DIR}/reproducible-build-trees
            --exclude ${CMAKE_SOURCE_DIR}/build
            --exclude ${CMAKE_BINARY_DIR}
            --cmake ${CMAKE_COMMAND}
            --jobs 4
            --target SparkCooker
            --config MinSizeRel
            --scan bin/MinSizeRel
            --scan lib/MinSizeRel
            --normalize-coff-build-root
            --
            ${_spark_repro_generator_args}
            -DCMAKE_CONFIGURATION_TYPES=MinSizeRel
            -DENABLE_LTO=OFF
            -DSTRIP_DEBUG_SYMBOLS=ON
            -DSPARK_NATIVE_ARCH=OFF
            -DENABLE_EDITOR=OFF
            -DENABLE_PROFILING=OFF
            -DENABLE_CONSOLE_IN_SHIPPING=OFF
            -DENABLE_DEVCOMMANDS_IN_SHIPPING=OFF
            -DENABLE_ANGELSCRIPT=OFF
            -DBUILD_GAME_MODULES=OFF
            -DBUILD_TESTS=OFF)
    set_tests_properties(ReproducibleBuild_WindowsToolTargets PROPERTIES
        LABELS "build;reproducibility;shipping;integration"
        RUN_SERIAL TRUE
        TIMEOUT 3600)
endif()

# The packaged components (runtime, tools, samples) and the private "symbols"
# component install to separate roots, as a release stages them
# (cmake/SparkStagePrivateSymbols.cmake). In a STRIP_DEBUG_SYMBOLS ELF tree every
# installed image must be stripped and map by build-id to exactly one installed
# .debug file; on MSVC every installed EXE/DLL must map by its RSDS PDB GUID+age
# to exactly one installed PDB and record a bare PDB name. Only shipped images
# carry private symbols, so the test installs rather than scanning the output
# directories, and it needs every installed target built. MSVC Debug links keep
# the absolute PDB path for local debugging, so it runs in optimized
# configurations only there.
if(SPARK_SPLIT_DEBUG_SYMBOLS OR MSVC)
    if(MSVC)
        set(_spark_private_symbol_configs CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
    else()
        set(_spark_private_symbol_configs "")
    endif()
    add_test(
        NAME ShippingManifest_PrivateSymbols
        ${_spark_private_symbol_configs}
        COMMAND ${CMAKE_COMMAND}
            -DSTAGE=${CMAKE_BINARY_DIR}/shipping-symbol-stage
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DCONFIG=$<CONFIG>
            -DPYTHON=${Python3_EXECUTABLE}
            -DTOOL=${CMAKE_SOURCE_DIR}/tools/shipping_symbol_manifest.py
            -P ${CMAKE_SOURCE_DIR}/cmake/SparkStagePrivateSymbols.cmake)
    set_tests_properties(ShippingManifest_PrivateSymbols PROPERTIES
        LABELS "build;shipping;integration"
        TIMEOUT 600)
endif()

# BLD-100: ShippingConfiguration_Distinct reads this tree's own CMake File API
# codemodel, requested here and answered by this same configure, and requires
# MinSizeRel (Shipping) to compile SparkEngineLib and SparkEngine with
# SPARK_BUILD_SHIPPING and SPARK_SHIPPING=1 while Debug and Release define
# neither, every configuration to own its artifact directory, and the Shipping
# link to carry /DEBUG, /Brepro and a bare-name /PDBALTPATH. It needs an MSVC
# multi-config tree that resolves all three configurations; Ninja Multi-Config
# omits MinSizeRel unless CMAKE_CONFIGURATION_TYPES names it. build.yml runs the
# same checker on the windows-shipping preset tree with --require-preset.
if(MSVC AND SPARK_GENERATOR_IS_MULTI_CONFIG AND "Debug" IN_LIST CMAKE_CONFIGURATION_TYPES
   AND "Release" IN_LIST CMAKE_CONFIGURATION_TYPES AND "MinSizeRel" IN_LIST CMAKE_CONFIGURATION_TYPES)
    # cmake_file_api() needs CMake 3.27; without it the checker fails on the missing reply.
    if(COMMAND cmake_file_api)
        cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
    endif()
    add_test(
        NAME ShippingConfiguration_Distinct
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/check_shipping_configuration.py
            --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(ShippingConfiguration_Distinct PROPERTIES
        LABELS "build;shipping;integration"
        TIMEOUT 120)
else()
    message(STATUS "ShippingConfiguration_Distinct not registered: needs an MSVC multi-config tree with Debug, Release and MinSizeRel")
endif()
# SDK-240: the SDK vtable/struct surface is pinned in SparkSDK/ABI/sdk-abi-surface.json;
# an ABI change without a re-pinned SPARK_SDK_VERSION bump fails here.
add_test(
    NAME SparkSDKABISurface
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/SparkSDK/Tools/sdk_abi_surface.py check)
set_tests_properties(SparkSDKABISurface PROPERTIES
    LABELS "sdk;module;abi;unit;python"
    TIMEOUT 60)
add_test(
    NAME SparkSDKABISurfaceChecker
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_sdk240_abi_surface.py)
set_tests_properties(SparkSDKABISurfaceChecker PROPERTIES
    LABELS "sdk;module;abi;unit;python"
    TIMEOUT 120)

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

# MOD-350: RPGQuestSlice_* drive the real RPGDemoSession with the RPGGameplayBridge
# quest policy installed (Talk -> AcceptQuest -> Travel -> Attack -> loot -> reward).
# The module sources compile into SparkTests only when ImGui is available.
if(TARGET imgui)
    add_test(NAME RPGQuestSlice COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(RPGQuestSlice PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=RPGQuestSlice_;SPARK_TEST_EXPECT_COUNT=3"
        LABELS "module;rpg;unit"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    # MOD-350: RPGPersistence_* save mid-quest through RPGEngineSystems::SaveGame into the real
    # SaveSystem, rebuild every RPG system (and the QuestSystem) as a restart would, and require
    # LoadGame to restore quest, inventory, area, NPC disposition/patrol and world-hour state.
    # RPGDEMO 2 (N-1) migrates to default NPC state; RPGDEMO 1, future, truncated, non-finite and out-of-range
    # snapshots and damaged slots leave the session unchanged.
    add_test(NAME RPGPersistence COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(RPGPersistence PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=RPGPersistence_;SPARK_TEST_EXPECT_COUNT=6"
        LABELS "module;rpg;persistence;unit"
        RUN_SERIAL TRUE
        TIMEOUT 180)

    # MOD-350: RPGNPCNavigation_* bake area NavMeshes through the engine NavMeshBuilder and require
    # schedule changes and patrol legs to walk NPCs along NavMesh routes at walking speed (around
    # blocked ground, resuming after a restore) and to leave them in place when no route exists,
    # never teleporting.
    add_test(NAME RPGNPCNavigation COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(RPGNPCNavigation PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=RPGNPCNavigation_;SPARK_TEST_EXPECT_COUNT=6"
        LABELS "module;rpg;ai;unit"
        TIMEOUT 120)

    # MOD-370: RTSSave_* save a scripted skirmish mid-match (codec and the real SaveSystem
    # through RTSEngineSystems::SaveMatch/LoadMatch), load it into rebuilt systems, and
    # require the uninterrupted run's per-tick state hash; v1 and truncated saves are rejected.
    add_test(NAME RTSSave COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
    set_tests_properties(RTSSave PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=RTSSave_;SPARK_TEST_EXPECT_COUNT=3"
        LABELS "module;rts;unit"
        RUN_SERIAL TRUE
        TIMEOUT 300)
endif()

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

# RDY-010: the census mirror ratchet (--check) runs locally, not only in hosted CI.
add_test(
    NAME TestSourceCensus_MirrorBaseline
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_source_census_baseline.py)
set_tests_properties(TestSourceCensus_MirrorBaseline PROPERTIES
    LABELS "readiness;unit;python"
    TIMEOUT 120)

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

# MOD-295: each prototype module's engine-private headers and copied
# *EngineSystems.cpp files must match its committed module-content inventory
# entry; the shared include resolver and the ratchet mutations must fail by name.
add_test(
    NAME PrototypeModuleKit_PrivateDependencyRatchet
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_module_private_dependencies.py)
set_tests_properties(PrototypeModuleKit_PrivateDependencyRatchet PROPERTIES
    LABELS "module;module-kit;prototype-module-kit;readiness;unit;python"
    TIMEOUT 120)

# MOD-295: IEngineContext::GetLogger() and the Spark::ModuleLog helpers reach the
# engine Logger, IEngineContext::GetConsole() registers into the host SimpleConsole,
# IEngineContext::GetStateValidation() adds rules to the host InvalidStateDetector,
# and SparkGameRTS's engine-system wiring logs through them. The RTS case needs the
# module sources, which SparkTests compiles only with ImGui.
if(TARGET imgui)
    set(_spark_prototype_kit_count 5)
else()
    set(_spark_prototype_kit_count 4)
endif()
add_test(NAME PrototypeModuleKit_Helpers COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(PrototypeModuleKit_Helpers PROPERTIES
    ENVIRONMENT "SPARK_TEST_NAME_PREFIX=PrototypeModuleKit_;SPARK_TEST_EXPECT_COUNT=${_spark_prototype_kit_count}"
    LABELS "module;module-kit;prototype-module-kit;unit"
    TIMEOUT 60)
unset(_spark_prototype_kit_count)

# MOD-310: the same ratchet on the stable-v1 SparkGameFPS module, under a
# reviewed one-way ceiling in test_module_private_dependencies.py.
add_test(
    NAME FPSPublicSDK_PrivateIncludeRatchet
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/Tests/Tools/test_module_private_dependencies.py
        FPSPrivateIncludeRatchetTests FPSBuildCouplingTests FPSBuildCouplingSpellingTests)
set_tests_properties(FPSPublicSDK_PrivateIncludeRatchet PROPERTIES
    LABELS "module;stable-v1;fps;sdk;readiness;unit;python"
    TIMEOUT 120)

add_test(NAME PrototypeModuleKit_PublicConsoleBoundary
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/SDK/test_module_console_boundary.py)
set_tests_properties(PrototypeModuleKit_PublicConsoleBoundary PROPERTIES
    LABELS "module;module-kit;prototype-module-kit;sdk;unit;python"
    TIMEOUT 60)

add_test(NAME FPSPublicSDK_GameTypes COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)
set_tests_properties(FPSPublicSDK_GameTypes PROPERTIES
    ENVIRONMENT "SPARK_TEST_FILE=TestSDKGameTypes.cpp;SPARK_TEST_EXPECT_COUNT=1"
    LABELS "module;module-kit;stable-v1;fps;sdk;unit"
    TIMEOUT 60)

# MOD-290: per-module test registration is generated from each module's
# module.json rather than hand-written. Every declared {prefix, count} selector
# becomes ModuleManifest_<Module>_<Prefix>, which runs SparkTests filtered to
# names that start with that prefix (SPARK_TEST_NAME_PREFIX, so RPG_ does not
# also select ARPG_*) and fails unless exactly `count` tests run, each asserting
# something. A selector that names a missing, renamed or emptied test family
# therefore fails ctest. The module sources these tests exercise compile into
# SparkTests only when ImGui is available (see the feature-gated block above).
#
# Selectors listed here run only in their ModuleManifest_ lane, not also in
# SparkEngineTests: RacingCompleteRace_ drives twelve full Jolt races (about 130 s
# of the main suite on Linux GCC Release). The exclusion is added only once the
# lane is registered, so a build without it still runs the family in the suite.
if(TARGET imgui)
    set(_spark_module_kit_main_suite_moves RacingCompleteRace_)
    file(GLOB _spark_module_kit_cmake_files "${CMAKE_SOURCE_DIR}/GameModules/*/CMakeLists.txt")
    foreach(_spark_module_kit_cmake IN LISTS _spark_module_kit_cmake_files)
        get_filename_component(_spark_module_kit_dir "${_spark_module_kit_cmake}" DIRECTORY)
        get_filename_component(_spark_module_kit_name "${_spark_module_kit_dir}" NAME)
        set(_spark_module_kit_manifest "${_spark_module_kit_dir}/module.json")
        if(NOT EXISTS "${_spark_module_kit_manifest}")
            message(FATAL_ERROR
                "MOD-290: game module ${_spark_module_kit_name} has no module.json; "
                "see GameModules/README.md for the per-module manifest contract.")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_spark_module_kit_manifest}")
        file(READ "${_spark_module_kit_manifest}" _spark_module_kit_json)
        string(JSON _spark_module_kit_selector_count LENGTH "${_spark_module_kit_json}" tests prefixes)
        if(_spark_module_kit_selector_count EQUAL 0)
            message(FATAL_ERROR "MOD-290: ${_spark_module_kit_manifest} declares no tests.prefixes selectors.")
        endif()
        math(EXPR _spark_module_kit_last "${_spark_module_kit_selector_count} - 1")
        foreach(_spark_module_kit_index RANGE ${_spark_module_kit_last})
            string(JSON _spark_module_kit_prefix GET "${_spark_module_kit_json}"
                tests prefixes ${_spark_module_kit_index} prefix)
            # count is one integer, or {windows, other} when a family has
            # Windows-only (or non-Windows-only) TEST( definitions.
            string(JSON _spark_module_kit_count_type TYPE "${_spark_module_kit_json}"
                tests prefixes ${_spark_module_kit_index} count)
            if(_spark_module_kit_count_type STREQUAL "OBJECT")
                if(WIN32)
                    set(_spark_module_kit_platform windows)
                else()
                    set(_spark_module_kit_platform other)
                endif()
                string(JSON _spark_module_kit_count GET "${_spark_module_kit_json}"
                    tests prefixes ${_spark_module_kit_index} count ${_spark_module_kit_platform})
            else()
                string(JSON _spark_module_kit_count GET "${_spark_module_kit_json}"
                    tests prefixes ${_spark_module_kit_index} count)
            endif()
            if(NOT _spark_module_kit_prefix MATCHES "^[A-Za-z][A-Za-z0-9_]*$" OR
               NOT _spark_module_kit_count MATCHES "^[1-9][0-9]*$")
                message(FATAL_ERROR
                    "MOD-290: ${_spark_module_kit_manifest} tests.prefixes[${_spark_module_kit_index}] "
                    "needs a TEST-name prefix and a positive count "
                    "(got '${_spark_module_kit_prefix}', '${_spark_module_kit_count}').")
            endif()
            # Optional requires: build features without which the family is not
            # compiled into SparkTests (count is the feature-on count), so the
            # exact-count CTest is not registered when one of them is off.
            string(JSON _spark_module_kit_requires ERROR_VARIABLE _spark_module_kit_requires_error
                GET "${_spark_module_kit_json}" tests prefixes ${_spark_module_kit_index} requires)
            set(_spark_module_kit_missing_feature "")
            if(NOT _spark_module_kit_requires_error)
                string(JSON _spark_module_kit_requires_count LENGTH "${_spark_module_kit_requires}")
                math(EXPR _spark_module_kit_requires_last "${_spark_module_kit_requires_count} - 1")
                foreach(_spark_module_kit_requires_index RANGE ${_spark_module_kit_requires_last})
                    string(JSON _spark_module_kit_feature GET "${_spark_module_kit_requires}"
                        ${_spark_module_kit_requires_index})
                    if(_spark_module_kit_feature STREQUAL "angelscript")
                        if(NOT SPARK_ANGELSCRIPT_AVAILABLE)
                            set(_spark_module_kit_missing_feature "${_spark_module_kit_feature}")
                        endif()
                    else()
                        message(FATAL_ERROR
                            "MOD-290: ${_spark_module_kit_manifest} tests.prefixes[${_spark_module_kit_index}] "
                            "requires unknown feature '${_spark_module_kit_feature}'.")
                    endif()
                endforeach()
            endif()
            string(REGEX REPLACE "_+$" "" _spark_module_kit_suffix "${_spark_module_kit_prefix}")
            set(_spark_module_kit_test "ModuleManifest_${_spark_module_kit_name}_${_spark_module_kit_suffix}")
            if(_spark_module_kit_missing_feature)
                message(STATUS "${_spark_module_kit_test}: not registered (requires "
                    "${_spark_module_kit_missing_feature}, which this build disables)")
                # Its TESTs are not compiled either, so the main suite has nothing to hand over.
                list(REMOVE_ITEM _spark_module_kit_main_suite_moves ${_spark_module_kit_prefix})
                continue()
            endif()
            add_test(NAME ${_spark_module_kit_test} COMMAND $<TARGET_FILE:SparkTests> --quiet --empty-is-error)
            # RUN_SERIAL: several families open loopback UDP listeners or write
            # save/database files under shared per-user roots.
            set_tests_properties(${_spark_module_kit_test} PROPERTIES
                ENVIRONMENT "SPARK_TEST_NAME_PREFIX=${_spark_module_kit_prefix};SPARK_TEST_EXPECT_COUNT=${_spark_module_kit_count}"
                LABELS "module;module-kit;unit"
                RUN_SERIAL TRUE
                TIMEOUT 300)
            if(_spark_module_kit_prefix IN_LIST _spark_module_kit_main_suite_moves)
                spark_exclude_from_main_suite(${_spark_module_kit_prefix})
                list(REMOVE_ITEM _spark_module_kit_main_suite_moves ${_spark_module_kit_prefix})
            endif()
        endforeach()
    endforeach()
    if(_spark_module_kit_main_suite_moves)
        message(FATAL_ERROR "No module.json registers the selector(s) ${_spark_module_kit_main_suite_moves} "
            "that the main suite hands to their ModuleManifest_ lane; update the list above.")
    endif()
endif()

# OPS-100: the isolated production crash-capture producer runs on every native
# host that builds the reporter (Windows minidump path, POSIX assertion path).
# Without miniz the engine links CrashHandlerStub.cpp, which has no producer.
if(NOT CMAKE_CROSSCOMPILING AND MINIZ_FOUND AND TARGET SparkCrashReporter)
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

# OPS-100: build-id symbolication canary (Linux, local symbol store, no relay).
# The probe crashes through the production signal handler; it is compiled with
# -g (RelWithDebInfo-style) and an explicit GNU build-id so its split debug
# info can be filed in a .build-id store and the fault resolved to file:line.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING AND MINIZ_FOUND
   AND Python3_Interpreter_FOUND AND CMAKE_OBJCOPY AND CMAKE_ADDR2LINE)
    add_executable(SparkCrashSymbolicationProbe Fixtures/CrashSymbolicationProbe.cpp)
    target_compile_features(SparkCrashSymbolicationProbe PRIVATE cxx_std_23)
    # -fno-lto: an LTO-compiled unit reports its DWARF file as <artificial>.
    target_compile_options(SparkCrashSymbolicationProbe PRIVATE -g -O2 -fno-lto)
    target_link_options(SparkCrashSymbolicationProbe PRIVATE -Wl,--build-id=sha1)
    # Shipping presets strip every link (-s); the probe must keep its DWARF so
    # the canary can split it into the symbol store.
    get_target_property(_spark_probe_link_options SparkCrashSymbolicationProbe LINK_OPTIONS)
    list(FILTER _spark_probe_link_options EXCLUDE REGEX ":-s>$")
    set_property(TARGET SparkCrashSymbolicationProbe PROPERTY LINK_OPTIONS ${_spark_probe_link_options})
    target_link_libraries(SparkCrashSymbolicationProbe PRIVATE SparkEngineLib)
    add_dependencies(SparkTests SparkCrashSymbolicationProbe)
    add_test(
        NAME CrashReporter_Symbolication
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/Tools/run_crash_symbolication_canary.py
            --probe $<TARGET_FILE:SparkCrashSymbolicationProbe>
            --probe-source ${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/CrashSymbolicationProbe.cpp
            --objcopy ${CMAKE_OBJCOPY}
            --addr2line ${CMAKE_ADDR2LINE})
    set_tests_properties(CrashReporter_Symbolication PROPERTIES
        LABELS "crash-canary;integration"
        TIMEOUT 300)
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

# CI-110 / EDT-210: execute the shipped editor against an isolated project.
# On Windows this is the stable-v1 Win32/D3D11 editor. On Linux the editor needs
# an X11/GL display, so every editor run is wrapped in xvfb-run (Mesa llvmpipe):
# that proves the Linux executable starts, loads a project, saves/reopens a
# scene and shuts down cleanly. It is not D3D11 editor certification. Without
# xvfb-run the test is still registered and fails, so the lane cannot pass by
# skipping.
if(TARGET SparkEditor AND (WIN32 OR (UNIX AND NOT APPLE)))
    set(_spark_editor_smoke_launcher_arg)
    set(_spark_editor_smoke_labels "release;editor;editor-integration;process")
    set(_spark_editor_smoke_timeout 45)
    if(NOT WIN32)
        find_program(SPARK_XVFB_RUN_EXECUTABLE xvfb-run)
        if(NOT SPARK_XVFB_RUN_EXECUTABLE)
            message(WARNING
                "xvfb-run was not found: SparkEditorExecutableSmoke is registered but will fail "
                "until xvfb is installed (the Linux editor needs an X server).")
        endif()
        set(_spark_editor_smoke_launcher_arg
            "-DSPARK_EDITOR_REQUIRE_XVFB=ON" "-DSPARK_EDITOR_XVFB_RUN=${SPARK_XVFB_RUN_EXECUTABLE}")
        set(_spark_editor_smoke_labels "editor;editor-integration;process;linux")
        # Four editor launches, each starting its own Xvfb server.
        set(_spark_editor_smoke_timeout 180)
    endif()
    add_dependencies(SparkTests SparkEditor)
    add_test(
        NAME SparkEditorExecutableSmoke
        COMMAND ${CMAKE_COMMAND}
            "-DSPARK_EDITOR=$<TARGET_FILE:SparkEditor>"
            "-DSPARK_EDITOR_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSPARK_EDITOR_WORK_DIR=${CMAKE_BINARY_DIR}/editor-executable-smoke/$<CONFIG>"
            ${_spark_editor_smoke_launcher_arg}
            -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkEditorSmoke.cmake)
    set_tests_properties(SparkEditorExecutableSmoke PROPERTIES
        LABELS "${_spark_editor_smoke_labels}"
        TIMEOUT ${_spark_editor_smoke_timeout})
    if(NOT WIN32)
        # llvmpipe rendering is CPU-bound; keep parallel ctest from starving it.
        set_tests_properties(SparkEditorExecutableSmoke PROPERTIES RUN_SERIAL TRUE)
    endif()
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

        # SEC2: -batch keeps the source layout under -o and refuses a plan in which
        # two sources would write the same artifact (previously the later compile
        # silently overwrote the earlier one and the run exited 0).
        add_test(
            NAME SparkShaderCompilerBatchOutputs
            COMMAND ${CMAKE_COMMAND}
                "-DSPARK_SHADER_COMPILER=$<TARGET_FILE:SparkShaderCompiler>"
                "-DSPARK_SHADER_FIXTURE=${CMAKE_SOURCE_DIR}/Tests/Fixtures/ShaderCompiler/BasicVS.hlsl"
                "-DSPARK_SHADER_OUTPUT_DIR=${CMAKE_BINARY_DIR}/shader-compiler-batch"
                -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkShaderCompilerBatchOutputs.cmake)
        set_tests_properties(SparkShaderCompilerBatchOutputs PROPERTIES
            LABELS "release;shader;process;security"
            TIMEOUT 60)
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

    # REL-110: release SBOM vulnerability gate (stdlib; runs on every lane). Its release.yml
    # wiring cases need PyYAML: skipped without it here, enforced in the supply-chain CI job.
    add_test(
        NAME ArtifactIntegrity_VulnerabilityGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_verify_vulnerability_findings.py)
    set_tests_properties(ArtifactIntegrity_VulnerabilityGate PROPERTIES
        LABELS "release;security;supply-chain;unit;python"
        TIMEOUT 60)

    # REL-110: the existing release integrity suites (stable-publication
    # consumer verifier, detached-signature bundle verify/extract, release
    # asset boundary, build provenance). They exercise POSIX publication
    # boundaries and the openssl CLI, so they run in the Linux ctest lanes;
    # Windows keeps its native Authenticode gate in the release workflow.
    if(NOT WIN32)
        # The consumer receipt publisher (receipt_publication.py) supports
        # only Linux O_TMPFILE and Windows handles, and refuses every other
        # POSIX system by design, so its suite is registered on Linux only.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME ArtifactIntegrity_PublishedStableRelease
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/.github/scripts/test_verify_published_stable_release.py)
            set_tests_properties(ArtifactIntegrity_PublishedStableRelease PROPERTIES
                LABELS "release;integrity;python"
                TIMEOUT 120)
        endif()
        add_test(
            NAME ArtifactIntegrity_ReleaseBundle
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_verify_release_bundle.py)
        set_tests_properties(ArtifactIntegrity_ReleaseBundle PROPERTIES
            LABELS "release;integrity;python"
            TIMEOUT 120
            # Every case needs the openssl CLI; a host without it reports the
            # suite as skipped rather than as a passing cryptographic check.
            SKIP_REGULAR_EXPRESSION "OK \\(skipped=")
        add_test(
            NAME ArtifactIntegrity_SignatureBundleExtract
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_extract_release_signature_bundle.py)
        set_tests_properties(ArtifactIntegrity_SignatureBundleExtract PROPERTIES
            LABELS "release;integrity;python"
            TIMEOUT 120)
        add_test(
            NAME ArtifactIntegrity_ReleaseAssetBoundary
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_verify_release_asset_boundary.py)
        set_tests_properties(ArtifactIntegrity_ReleaseAssetBoundary PROPERTIES
            LABELS "release;integrity;python"
            TIMEOUT 120)
        add_test(
            NAME ArtifactIntegrity_BuildProvenance
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_release_build_provenance.py)
        set_tests_properties(ArtifactIntegrity_BuildProvenance PROPERTIES
            LABELS "release;integrity;python"
            TIMEOUT 120)
        # REL-190: fail-closed stable release notes generator and its
        # release.yml wiring. This is only the notes check; the candidate
        # qualification rehearsal is ReleaseProfileRehearsal_Qualification below.
        add_test(
            NAME ReleaseProfileRehearsal_ReleaseNotes
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_release_notes.py)
        set_tests_properties(ReleaseProfileRehearsal_ReleaseNotes PROPERTIES
            LABELS "release;integrity;python"
            TIMEOUT 120)
    endif()

    # REL-190: tools/release_qualification.py, which profile-required-gates runs
    # for a versioned candidate: stage readiness, every required item's CI jobs
    # able to block publication, and a well-formed exact-SHA record. The real
    # contract must be refused while REL-190 is blocked.
    add_test(
        NAME ReleaseProfileRehearsal_Qualification
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_release_qualification.py -v)
    set_tests_properties(ReleaseProfileRehearsal_Qualification PROPERTIES
        FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
        LABELS "release;integrity;python"
        TIMEOUT 120)

    # REL-191: the predecessor-stage rehearsal. Runs every predecessor case of
    # the stage contract (including the N-1 evidence boundary) and of the
    # qualifier, which must refuse the real predecessor stage while it is blocked.
    add_test(
        NAME ReleaseProfilePredecessorRehearsal_Qualification
        COMMAND ${Python3_EXECUTABLE} -B -m unittest -v -k predecessor
                Tests/Tools/test_release_stages.py Tests/Tools/test_release_qualification.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(ReleaseProfilePredecessorRehearsal_Qualification PROPERTIES
        FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
        LABELS "release;integrity;python"
        TIMEOUT 120)

    # REL-110: protected stable-release approval event recorder, its consumer
    # digest binding, and the release.yml wiring.
    add_test(
        NAME ReleaseApproval_Record
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_record_release_approval.py)
    set_tests_properties(ReleaseApproval_Record PROPERTIES
        LABELS "release;security;unit;python"
        TIMEOUT 60)

    # REL-100: the release.yml tag contract. Stable vX.Y.Z tags must equal the
    # single CMake SPARK_ENGINE_VERSION default and have exactly one versioned
    # CHANGELOG.md heading; nightly tags are unique per run, attempt and SHA.
    add_test(
        NAME TagReleaseContract_StableTag
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_stable_release_tag.py)
    set_tests_properties(TagReleaseContract_StableTag PROPERTIES
        LABELS "release;version;unit;python"
        TIMEOUT 60)
    add_test(
        NAME TagReleaseContract_NightlyTag
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.github/scripts/test_nightly_release_tag.py)
    set_tests_properties(TagReleaseContract_NightlyTag PROPERTIES
        LABELS "release;version;unit;python"
        TIMEOUT 60)
    # REL-100: tools/verify_package_versions.py, which release.yml build-windows
    # runs on the CPack ZIP: archive name, every Spark executable's
    # VS_VERSIONINFO and the SDK ConfigVersion embed exactly X.Y.Z.
    add_test(
        NAME TagReleaseContract_PackagedVersions
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_verify_package_versions.py -v)
    set_tests_properties(TagReleaseContract_PackagedVersions PROPERTIES
        FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
        LABELS "release;version;unit;python"
        TIMEOUT 60)

    # CI-120: the root option guard rejects undeclared -D ENABLE_*/SPARK_*/BUILD_*
    # names (fixture configures of cmake/SparkOptionGuard.cmake + preset audit).
    add_test(
        NAME BuildOptions_UnknownOptionRejected
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_build_option_guard.py)
    set_tests_properties(BuildOptions_UnknownOptionRejected PROPERTIES
        LABELS "build-contract;ci;governance;unit;python"
        TIMEOUT 120)

    # CI-120: documented cmake/ctest/cpack commands (README, CLAUDE.md, wiki,
    # docs) resolve against CMakePresets.json trees, configurations and pins.
    add_test(
        NAME BuildOptions_DocumentedCommandsMatchPresets
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_documented_build_commands.py)
    set_tests_properties(BuildOptions_DocumentedCommandsMatchPresets PROPERTIES
        LABELS "build-contract;ci;governance;docs;unit;python"
        TIMEOUT 120)

    # CI-120: SPARK_STRICT_DEPS=ON fails configure when any dependency declared by
    # ThirdParty/dependencies.lock is missing (fixture configures of the real audit
    # module against the real manifest, one removal per locked dependency).
    add_test(
        NAME StrictDependencies_LockClosureMissingIsFatal
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_strict_dependencies.py)
    set_tests_properties(StrictDependencies_LockClosureMissingIsFatal PROPERTIES
        LABELS "build-contract;ci;governance;supply-chain;unit;python"
        TIMEOUT 180)

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
    # OPS-110 server soak (tools/ops/server_soak.py reads /proc, so Linux only).
    # ServerSoak_Harness proves every failure mode against a stand-in server;
    # it is not labelled server-soak because only a real-server soak may
    # satisfy that label. Server_Soak runs the real SparkServer with SparkGame
    # for 60 s under provisional budgets; the 30-minute release smoke and
    # longer scheduled soaks run tools/ops/server_soak.py --duration directly.
    if(UNIX AND NOT APPLE)
        add_test(
            NAME ServerSoak_Harness
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_server_soak.py
                FitTests DrainSequenceTests ConfigTests SummaryTests HarnessTests QueueTests)
        set_tests_properties(ServerSoak_Harness PROPERTIES
            LABELS "ops;server;unit;python"
            TIMEOUT 180)
        if(TARGET SparkServer AND TARGET SparkGame)
            add_dependencies(SparkTests SparkServer SparkGame)
            add_test(
                NAME Server_Soak
                COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/Tests/Tools/test_server_soak.py ServerSoakProcess)
            set_tests_properties(Server_Soak PROPERTIES
                ENVIRONMENT "SPARK_SERVER=$<TARGET_FILE:SparkServer>;SPARK_SERVER_MODULE=$<TARGET_FILE:SparkGame>;SPARK_SERVER_SOAK_REQUIRED=1;SPARK_SERVER_SOAK_SUMMARY=${CMAKE_BINARY_DIR}/server-soak-summary.json"
                LABELS "server-soak;server;integration;process"
                RUN_SERIAL TRUE
                TIMEOUT 300)
        endif()
    endif()
    # OPS-110 recovery drill (tools/ops/server_recovery_drill.py): detect a
    # killed or wedged server from its health file alone, drain it through the
    # stop file, and restart it until the same build reports ready and ticks.
    # ServerRecoveryDrill_Harness proves every failure mode against a stand-in
    # server; like ServerSoak_Harness it is not labelled recovery-drill, which
    # only a real-server drill (and DATA-120's persistence drills) may carry.
    add_test(
        NAME ServerRecoveryDrill_Harness
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_server_recovery_drill.py
            ConfigTests SummaryTests HarnessTests RunbookParityTests)
    set_tests_properties(ServerRecoveryDrill_Harness PROPERTIES
        LABELS "ops;server;unit;python"
        TIMEOUT 120)
    # DATA-120 / OPS-110 / PERF-100: the scheduled operations workflow keeps its
    # rehearsal contract (cron, exact SHA, blocking jobs, soak lengths, evidence).
    add_test(
        NAME OperationsScheduledWorkflow_Policy
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_operations_scheduled_workflow.py)
    set_tests_properties(OperationsScheduledWorkflow_Policy PROPERTIES
        LABELS "governance;ops;unit;python"
        FAIL_REGULAR_EXPRESSION "Ran 0 tests"
        TIMEOUT 60)
    # OPS-110: the versioned spark-server-health/1 contract that external consumers parse.
    add_test(
        NAME ServerHealth_ExternalContract
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_SOURCE_DIR}/Tests/Tools/test_validate_server_health.py)
    set_tests_properties(ServerHealth_ExternalContract PROPERTIES
        LABELS "observability;ops;unit;python"
        FAIL_REGULAR_EXPRESSION "Ran 0 tests"
        TIMEOUT 60)
    if(TARGET SparkServer AND TARGET SparkGame AND (WIN32 OR (UNIX AND NOT APPLE)))
        add_dependencies(SparkTests SparkServer SparkGame)
        add_test(
            NAME Server_RecoveryDrill
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_server_recovery_drill.py ServerRecoveryDrillProcess)
        set_tests_properties(Server_RecoveryDrill PROPERTIES
            ENVIRONMENT "SPARK_SERVER=$<TARGET_FILE:SparkServer>;SPARK_SERVER_MODULE=$<TARGET_FILE:SparkGame>;SPARK_SERVER_DRILL_REQUIRED=1;SPARK_SERVER_DRILL_SUMMARY=${CMAKE_BINARY_DIR}/server-recovery-drill-summary.json"
            LABELS "recovery-drill;server;integration;process"
            RUN_SERIAL TRUE
            TIMEOUT 180)
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
        # site-data-generation-determinism regenerates docs/api in the source tree
        # (publish_generated_tree swaps it through a docs/.api.previous-* directory),
        # and the contract suites walk docs/ while resolving references. Run in
        # parallel, a walk hit the moving tree ("Required file does not exist:
        # docs/.api.previous-*/ComponentIndex.md"), so every site-data test that
        # reads or writes that tree holds this lock.
        set(SPARK_SITE_DATA_TREE_LOCK "site-data-source-tree")
        set_tests_properties(site-data-contract PROPERTIES
            LABELS "readiness;site-data;unit;python"
            RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
            TIMEOUT 300)

        # RDY-000: the fast cross-reference subset -- the checked-in contract under
        # the strict validator, dependency closure, promotion evidence, selector and
        # referenced-path existence, prose ID references, and the handoff being current.
        add_test(
            NAME readiness-cross-references
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_contract.py
                LiveContractTests
                TransitiveDependencyTests
                ReadyPromotionTests
                ReadyAndPassingEvidenceTests
                SelectorResolutionTests
                MissingReferencedPathTests
                ProseCrossReferenceTests
                GenerationAndCiTests)
        set_tests_properties(readiness-cross-references PROPERTIES
            LABELS "readiness;site-data;unit;python"
            RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
            TIMEOUT 180)

        # RDY-000: two clean generate.py runs produce byte-identical bundles; the
        # comparator's own mutation cases prove a one-byte or file-set drift fails.
        add_test(
            NAME site-data-generation-determinism
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_generation_determinism.py)
        set_tests_properties(site-data-generation-determinism PROPERTIES
            LABELS "readiness;site-data;python;slow"
            RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
            TIMEOUT 900)

        # DOC-400: docs/site/content.json and docs-catalog.json carry no hardcoded
        # count, commit SHA, CI run ID, or version literal; mutable claims must
        # reference bundle metric identifiers. validate.py enforces the same rule.
        add_test(
            NAME no-hardcoded-claims
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_contract.py
                NoHardcodedClaimsTests)
        set_tests_properties(no-hardcoded-claims PROPERTIES
            LABELS "readiness;site-data;unit;python"
            RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
            TIMEOUT 180)

        add_test(
            NAME public-wiki-claim-boundaries
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_public_wording.py -v)
        # Each case runs the full contract validator (about 20-40 s on a desktop).
        set_tests_properties(public-wiki-claim-boundaries PROPERTIES
            LABELS "readiness;site-data;unit;python"
            RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
            FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
            TIMEOUT 180)

        # DOC-400: a generate.py publication's documents, search records, source
        # URLs and page links agree (tools/site-data/docs_parity.py, run by
        # validate_published_bundle); each hash-consistent mutation must fail.
        # generate.py republishes docs/api in the source tree, hence the lock.
        add_test(
            NAME live-docs-parity
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_docs_parity.py -v)
        set_tests_properties(live-docs-parity PROPERTIES
            LABELS "readiness;site-data;unit;python"
            RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
            FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
            TIMEOUT 600)

        # Direct publication generates API docs, then validates links/routes
        # before it may replace the previous output.
        add_test(
            NAME site-data-publisher-docs
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_data_publisher.py -v)
        set_tests_properties(site-data-publisher-docs PROPERTIES
            LABELS "readiness;site-data;unit;python"
            RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
            FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
            TIMEOUT 60)

        add_test(
            NAME site-runtime-test-runner
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Tools/test_site_runtime_tests.py)
        set_tests_properties(site-runtime-test-runner PROPERTIES
            LABELS "readiness;site-data;unit;python"
            FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
            TIMEOUT 60)

        # DOC-400: the reference site runtime verifier (tools/site-data/runtime)
        # checked against a bundle generate.py produces here plus tampered,
        # oversize, malformed, and path-escaping variants, each also fed to
        # validate.py validate_published_bundle so the two validators cannot
        # drift. Needs Node.js 20+ (WebCrypto, node:test); the site-runtime-contract
        # job in site-data.yml is its CI home.
        find_program(SPARK_NODE_EXECUTABLE NAMES node)
        if(SPARK_NODE_EXECUTABLE)
            execute_process(
                COMMAND ${SPARK_NODE_EXECUTABLE} -p "process.versions.node.split('.')[0]"
                OUTPUT_VARIABLE _spark_node_major
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _spark_node_result)
        endif()
        if(SPARK_NODE_EXECUTABLE AND _spark_node_result EQUAL 0 AND _spark_node_major GREATER_EQUAL 20)
            add_test(
                NAME runtime-bundle-validation
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Tests/Tools/run_site_runtime_tests.py
                    ${SPARK_NODE_EXECUTABLE}
                    test/verifyBundle.test.mjs
                    test/freshness.test.mjs
                    test/siteDataRuntime.test.mjs
                    test/browserRuntime.test.mjs
                    test/bootstrap.test.mjs
                    test/siteDataRuntime.fixture.test.mjs
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/site-data/runtime)
            set_tests_properties(runtime-bundle-validation PROPERTIES
                LABELS "readiness;site-data;unit;node"
                ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}"
                # verifyBundle.test.mjs runs generate.py in the source tree, which
                # republishes docs/api through a docs/.api.staging-* directory.
                RESOURCE_LOCK "${SPARK_SITE_DATA_TREE_LOCK}"
                TIMEOUT 600)
        else()
            message(STATUS "runtime-bundle-validation not registered: Node.js 20+ was not found")
        endif()
    endif()

    add_test(
        NAME SparkCliContract
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/Tools/spark-cli/tests
            -p test_*.py
            -v
    )
    set_tests_properties(SparkCliContract PROPERTIES LABELS "cli;integration" TIMEOUT 180)

    # ASSET-220: every CLI and packager claim (Tools/spark-cli/claims.json) names tests that
    # exist, and every documented or parsed command and option is claimed.
    add_test(
        NAME CLI_ClaimsMatchBehavior
        COMMAND ${Python3_EXECUTABLE} -B -m unittest test_spark_cli_claims -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/Tools/spark-cli/tests)
    set_tests_properties(CLI_ClaimsMatchBehavior PROPERTIES
        FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
        LABELS "cli;packaging;unit"
        TIMEOUT 120)

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

# ---------------------------------------------------------------------
# SparkTests precompiled header
# ---------------------------------------------------------------------
# Every test translation unit includes TestFramework.h and most pull in the same
# standard headers; parsing them is the bulk of a test file's compile time. The
# PCH holds only TestFramework.h and standard-library headers: no engine or
# platform header, so tests that define WIN32_LEAN_AND_MEAN, _XM_NO_INTRINSICS_
# or `private public` before an engine include keep their current meaning.
# Engine, editor, module and launcher sources compiled into SparkTests are
# excluded so their include order is exactly what their own targets use.
# Lanes configured with CMAKE_DISABLE_PRECOMPILE_HEADERS=ON (Windows sccache,
# clang-tidy) build without it, which keeps missing-include regressions visible.
target_precompile_headers(SparkTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/TestFramework.h"
    <algorithm>
    <array>
    <atomic>
    <chrono>
    <cmath>
    <cstdint>
    <cstring>
    <filesystem>
    <fstream>
    <functional>
    <limits>
    <memory>
    <sstream>
    <string>
    <thread>
    <unordered_map>
    <vector>
)
# SEC-120 parser regressions execute the shipped code through SparkTests. The
# exact-one pin prevents an absent or renamed case from reading as a pass.
foreach(_spark_sec120_case IN ITEMS
    DataTable_RejectsOversizedDocumentBeforeParsing
    DataTable_RejectsTrailingJSONWithoutPartialRows
    DataTableRegistry_RejectsOversizedValidFileBeforeReading
    DataTable_NumericLookingStringRoundTripsAsValidJSON
    DataTable_CsvRoundTripKeepsEmptySingleCellRow
    DataTable_CsvRoundTripKeepsTrailingCarriageReturn
    Dialogue_LoadFailureDoesNotPartiallyReplaceTree
    Dialogue_RejectsOversizedValidDocumentBeforeParsing
    Dialogue_LoadsCrlfDocument
    StringTable_MalformedReloadIsTransactional
    StringTable_LoadAcceptsBomAndMergesEntries
    FPSMultiplayer_WireDecoderRejectsInvalidBooleanAndActionBits
    FPSMultiplayer_SnapshotDecoderRejectsMalformedRecordWhenBatchIsZero
)
    add_test(NAME SEC120_${_spark_sec120_case}
        COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
    set_tests_properties(SEC120_${_spark_sec120_case} PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=${_spark_sec120_case};SPARK_TEST_EXPECT_COUNT=1"
        LABELS "security;fuzz-regression"
        TIMEOUT 30)
endforeach()
unset(_spark_sec120_case)

# This editor source is compiled into SparkTests only when ImGui is available.
if(TARGET imgui)
    add_test(NAME SEC120_Editor_Collab_MalformedFrame_DoesNotPartiallyPublish
        COMMAND $<TARGET_FILE:SparkTests> --warn-is-error --empty-is-error)
    set_tests_properties(SEC120_Editor_Collab_MalformedFrame_DoesNotPartiallyPublish PROPERTIES
        ENVIRONMENT "SPARK_TEST_NAME=Editor_Collab_MalformedFrame_DoesNotPartiallyPublish;SPARK_TEST_EXPECT_COUNT=1"
        LABELS "editor;collaboration;security;fuzz-regression"
        TIMEOUT 30)
endif()

get_target_property(_spark_tests_sources SparkTests SOURCES)
foreach(_spark_tests_source IN LISTS _spark_tests_sources)
    cmake_path(ABSOLUTE_PATH _spark_tests_source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _spark_tests_source_abs)
    cmake_path(IS_PREFIX CMAKE_CURRENT_SOURCE_DIR "${_spark_tests_source_abs}" NORMALIZE _spark_tests_source_is_test)
    if(NOT _spark_tests_source_is_test OR NOT _spark_tests_source MATCHES "\\.cpp$")
        set_source_files_properties("${_spark_tests_source}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
endforeach()
unset(_spark_tests_sources)
unset(_spark_tests_source)
unset(_spark_tests_source_abs)
unset(_spark_tests_source_is_test)
