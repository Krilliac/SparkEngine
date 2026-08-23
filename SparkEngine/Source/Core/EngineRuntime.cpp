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
#include "Graphics/GraphicsEngine.h"
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

EngineRuntime& GetEngineRuntime()
{
    static EngineRuntime s_runtime;
    return s_runtime;
}
