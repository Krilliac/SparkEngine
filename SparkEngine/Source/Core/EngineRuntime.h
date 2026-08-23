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
class EngineContext;

namespace Spark
{
    class AssetRegistry;
    class EventBus;
    class LocalFileCache;
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
    EngineRuntime();
    ~EngineRuntime();

    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;

    std::unique_ptr<GraphicsEngine> graphics;
    std::unique_ptr<InputManager> input;
    std::unique_ptr<Timer> timer;
    std::unique_ptr<Spark::EventBus> eventBus;
    std::unique_ptr<ModuleManager> moduleManager;
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<Spark::Audio::IAudioBackend> audioBackend;
    std::unique_ptr<Spark::ModuleHotReloadManager> moduleHotReload;
    std::unique_ptr<Spark::LocalFileCache> fileCache;
    std::unique_ptr<Spark::AssetRegistry> assetRegistry;
#ifdef SPARK_JOLT_PHYSICS_AVAILABLE
    std::unique_ptr<PhysicsSystem> physics;
#endif

    /**
     * @brief Create and register the CPU-only asset services required by modules.
     *
     * [startup thread] Safe for headless/server paths: this does not construct
     * GraphicsEngine, AssetPipeline, editor state, or any GPU resource.
     */
    void InitializeHeadlessAssetServices(EngineContext& context);

    /// [shutdown thread] Release owned asset services after EngineContext teardown.
    void ShutdownHeadlessAssetServices();
};

/**
 * @brief Access the process-wide EngineRuntime instance.
 *
 * Safe to call from any phase: fields are default-constructed (nullptr)
 * at static init and released during ShutdownEngine().
 */
EngineRuntime& GetEngineRuntime();
