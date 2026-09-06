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

    void InitializeDebugSystemsImpl();
    void InitializeNetworkingSystemsImpl();
    void InitializeGameplaySystemsImpl();

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
