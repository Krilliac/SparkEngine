/**
 * @file GameplayLifecycleShared.h
 * @brief Shared lifecycle functions for gameplay and debug subsystem management
 */

#pragma once
#include <cstdint>

namespace Spark::Core::Lifecycle
{
    void InitializeDebugSystemsImpl();
    void InitializeNetworkingSystemsImpl();
    void InitializeGameplaySystemsImpl();
    void UpdateGameplaySystemsImpl(float dt);
    void UpdateDebugSystemsImpl(float dt);
    void ShutdownGameplaySystemsImpl();
    void ShutdownDebugSystemsImpl();
    uint64_t GetGameplayFrameCountImpl();
    void LogMissingModuleWarningsImpl();
} // namespace Spark::Core::Lifecycle
