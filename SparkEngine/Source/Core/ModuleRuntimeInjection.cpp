/**
 * @file ModuleRuntimeInjection.cpp
 * @brief Private implementation of the public module runtime injection bridge.
 */

#include <Spark/ModuleRuntimeInjection.h>

#include "Core/EngineContext.h"
#include "Physics/PhysicsSystem.h"

namespace Spark::Detail
{
    void InjectModuleEngineRuntime(void* hostEngineContext)
    {
        EngineContext::SetInjected(static_cast<EngineContext*>(hostEngineContext));

        // Jolt keeps allocator, factory, and type-registration globals per image.
        // Initialize this module image's copies before module-side physics use.
        PhysicsSystem::EnsureImageRuntime();
    }
} // namespace Spark::Detail
