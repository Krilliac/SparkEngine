/**
 * @file EngineRuntime.h
 * @brief Private container for engine-owned subsystem instances.
 *
 * Holds the unique_ptrs that own the engine's core subsystems for the
 * process lifetime. Populated during engine startup (SparkEngine.cpp +
 * SparkEngine{Windows,Linux}.cpp) and torn down during shutdown.
 *
 * This struct replaces the previous pattern of individual per-subsystem
 * file-scope globals declared `extern` in each platform entry file.
 *
 * External code should NOT touch these fields directly. Access live
 * subsystem pointers through EngineContext::Get() instead. This header
 * is intended only for Core/ entry-point and lifecycle files that own
 * the subsystem lifetimes.
 */

#pragma once

#include <memory>

class GraphicsEngine;
class InputManager;
class Timer;
class AudioEngine;
class ModuleManager;
class PhysicsSystem;

namespace Spark
{
    class EventBus;
    class ModuleHotReloadManager;
    namespace Audio
    {
        class IAudioBackend;
    }
} // namespace Spark

/**
 * @brief Ownership container for engine subsystem unique_ptrs.
 *
 * Lifetime is managed by SparkEngine.cpp's init/shutdown sequence. Fields
 * are nullptr until the corresponding subsystem is created and remain
 * nullptr on paths that skip the subsystem (e.g. headless without audio).
 */
struct EngineRuntime
{
    std::unique_ptr<GraphicsEngine> graphics;
    std::unique_ptr<InputManager> input;
    std::unique_ptr<Timer> timer;
    std::unique_ptr<Spark::EventBus> eventBus;
    std::unique_ptr<ModuleManager> moduleManager;
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<Spark::Audio::IAudioBackend> audioBackend;
    std::unique_ptr<Spark::ModuleHotReloadManager> moduleHotReload;
#ifdef SPARK_JOLT_PHYSICS_AVAILABLE
    std::unique_ptr<PhysicsSystem> physics;
#endif
};

/**
 * @brief Access the process-wide EngineRuntime instance.
 *
 * Safe to call from any phase: fields are default-constructed (nullptr)
 * at static init and released during ShutdownEngine().
 */
EngineRuntime& GetEngineRuntime();
