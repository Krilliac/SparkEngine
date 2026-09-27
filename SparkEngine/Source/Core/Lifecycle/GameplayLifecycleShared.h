/**
 * @file GameplayLifecycleShared.h
 * @brief Shared lifecycle functions for gameplay and debug subsystem management
 */

#pragma once
#include <cstdint>
#include <string>

namespace Spark::ECS
{
    class PhaseSystemManager;
} // namespace Spark::ECS

namespace Spark::Core::Lifecycle
{
    /// Install the engine's standard Logger sinks (stderr + rotating per-user log
    /// file + SparkConsole bridge) exactly once per process. Platform entry points
    /// call it as early as possible so startup logging reaches the file;
    /// InitializeDebugSystemsImpl calls it again and gets the same result.
    /// @return Path of the engine log file, or empty when no file could be opened.
    std::string InstallEngineLogSinksImpl();

    /// Bring up logging and diagnostics. Detectors are optional, so this only
    /// fails by throwing. @return true when diagnostics are ready.
    bool InitializeDebugSystemsImpl();

    /// Publish and start the network service. @return false only when the
    /// EngineContext is missing; a network service that cannot start is logged
    /// and tolerated (offline play stays valid).
    bool InitializeNetworkingSystemsImpl();

    /// Create the AngelScript engine and publish it on the EngineContext, bound to
    /// the context World and EventBus. Game modules compile and attach scripts in
    /// OnLoad, so every host calls this from its core init before loading modules;
    /// InitializeGameplaySystemsImpl calls it again for callers without a host.
    /// Main thread only. Idempotent: returns true at once while its own engine is
    /// published and up; an engine some other owner started is never adopted.
    /// The engine object is process-static; ShutdownScriptingServiceImpl releases it.
    /// @return false when the EngineContext is missing or AngelScript fails to start.
    bool InitializeScriptingServiceImpl();

    /// Withdraw the published script engine from the EngineContext and shut it
    /// down after every module OnUnload. An engine that was never published (one
    /// another owner started) is left alone. Idempotent.
    void ShutdownScriptingServiceImpl();

    /// Initialize every gameplay subsystem and the ECS phase pipeline.
    /// @return false when the EngineContext is missing.
    bool InitializeGameplaySystemsImpl();

    /// Register the canonical ECS phase systems (Physics -> Animation -> AI ->
    /// Audio -> Gameplay -> PreRender -> Render) into the lifecycle-owned
    /// PhaseSystemManager. Called by InitializeGameplaySystemsImpl; safe to
    /// call again (rebuilds the set instead of accumulating duplicates).
    void InitializeEcsPhaseSystemsImpl();

    /// Access the lifecycle-owned PhaseSystemManager that
    /// UpdateGameplaySystemsImpl pumps every frame. Empty until
    /// InitializeEcsPhaseSystemsImpl has run.
    Spark::ECS::PhaseSystemManager& GetPhaseSystemManagerImpl();
    void UpdateGameplaySystemsImpl(float dt);
    void UpdateDebugSystemsImpl(float dt);
    void ShutdownGameplaySystemsImpl();
    void ShutdownDebugSystemsImpl();
    uint64_t GetGameplayFrameCountImpl();
    void LogMissingModuleWarningsImpl();
} // namespace Spark::Core::Lifecycle
