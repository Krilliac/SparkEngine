/**
 * @file EngineRuntime.cpp
 * @brief Storage for the process-wide EngineRuntime instance.
 *
 * Defined in the engine library (not the exe) so the runtime storage is
 * shared between the platform entry points and any library code that
 * needs access to subsystem ownership during init/shutdown.
 */

#include "EngineRuntime.h"

#include "AssetIntegration.h"
#include "EngineContext.h"
#include "Engine/Events/EventSystem.h"
#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "Audio/AudioEngine.h"
#include "Audio/IAudioBackend.h"
#include "Engine/Gameplay/WeaponManager.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/RHI/RHIBridge.h"
#include "Input/InputManager.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Timer.h"
#ifdef SPARK_JOLT_PHYSICS_AVAILABLE
#include "Physics/PhysicsSystem.h"
#endif

EngineRuntime::EngineRuntime() = default;
EngineRuntime::~EngineRuntime() = default;

void EngineRuntime::InitializeHeadlessAssetServices(EngineContext& context)
{
    if (!fileCache)
        fileCache = std::make_unique<Spark::LocalFileCache>();
    if (!assetRegistry)
        assetRegistry = std::make_unique<Spark::AssetRegistry>();

    context.SetFileCache(fileCache.get());
    context.SetAssetRegistry(assetRegistry.get());
}

void EngineRuntime::ShutdownHeadlessAssetServices()
{
    assetRegistry.reset();
    fileCache.reset();
}

bool EngineRuntime::InitializeHeadlessRhi()
{
    // A new headless RHI lifetime starts; never report an earlier run's count.
    headlessRhiLiveResourcesAtShutdown.reset();
    if (headlessRhiBridge)
        return headlessRhiBridge->IsHeadless() && headlessRhiBridge->GetDevice() != nullptr;

    auto bridge = std::make_unique<Spark::RHI::RHIBridge>();
    if (!bridge->Initialize(nullptr, 1, 1, Spark::RHI::GraphicsBackend::None, false) || !bridge->IsHeadless() ||
        bridge->GetActiveBackend() != Spark::RHI::GraphicsBackend::None || bridge->GetDevice() == nullptr)
    {
        bridge->Shutdown();
        return false;
    }

    headlessRhiBridge = std::move(bridge);
    return true;
}

void EngineRuntime::ShutdownHeadlessRhi() noexcept
{
    if (!headlessRhiBridge)
        return;

    headlessRhiBridge->Shutdown();
    headlessRhiLiveResourcesAtShutdown = headlessRhiBridge->GetNullResourcesLiveAtShutdown();
    headlessRhiBridge.reset();
}

EngineRuntime& GetEngineRuntime()
{
    static EngineRuntime s_runtime;
    return s_runtime;
}
