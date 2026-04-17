/**
 * @file EngineRuntime.cpp
 * @brief Storage for the process-wide EngineRuntime instance.
 *
 * Defined in the engine library (not the exe) so the runtime storage is
 * shared between the platform entry points and any library code that
 * needs access to subsystem ownership during init/shutdown.
 */

#include "EngineRuntime.h"

#include "Engine/Events/EventSystem.h"
#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "Audio/AudioEngine.h"
#include "Audio/IAudioBackend.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Utils/Timer.h"
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
#include "Physics/PhysicsSystem.h"
#endif

EngineRuntime& GetEngineRuntime()
{
    static EngineRuntime s_runtime;
    return s_runtime;
}
