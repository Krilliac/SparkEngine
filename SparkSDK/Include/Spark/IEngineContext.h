/**
 * @file IEngineContext.h
 * @brief Service locator interface for accessing engine subsystems
 *
 * IEngineContext is the primary interface through which game modules access
 * engine functionality. The engine creates a concrete implementation and
 * passes it to each module during OnLoad(). Modules should store this
 * pointer and use it to access graphics, input, physics, and other systems.
 *
 * This replaces the old approach of passing individual system pointers
 * (GraphicsEngine*, InputManager*) to IGameModule::Initialize().
 */

#pragma once

#include <cstdint>

// Forward declarations — engine types accessible through the context.
// Modules include the specific engine headers they need for full definitions.
class GraphicsEngine;
class InputManager;
class Timer;
class AudioEngine;
class PhysicsSystem;
class SceneManager;
class AngelScriptEngine;
class World;

namespace Spark
{

    class EventBus;

    namespace Animation
    {
        class AnimationSystem;
    }

    namespace AI
    {
        class AISystem;
    }

    class NetworkManager;
    class SaveSystem;
    class CoroutineScheduler;

    /**
     * @brief Service locator providing access to all engine subsystems
     *
     * Game modules receive an IEngineContext* during OnLoad() and use it
     * to query for engine services. This decouples modules from the engine's
     * internal structure and allows the engine to evolve without breaking
     * the module API.
     *
     * Prefer using IEngineContext over global variables (g_graphics, g_input, etc.)
     * which are deprecated and will be removed in a future release.
     */
    class IEngineContext
    {
      public:
        virtual ~IEngineContext() = default;

        /** @brief Get the graphics/rendering engine */
        virtual GraphicsEngine* GetGraphics() = 0;
        virtual const GraphicsEngine* GetGraphics() const = 0;

        /** @brief Get the input manager */
        virtual InputManager* GetInput() = 0;
        virtual const InputManager* GetInput() const = 0;

        /** @brief Get the frame timer */
        virtual Timer* GetTimer() = 0;
        virtual const Timer* GetTimer() const = 0;

        /** @brief Get the event bus for publish/subscribe messaging */
        virtual EventBus* GetEventBus() = 0;
        virtual const EventBus* GetEventBus() const = 0;

        /** @brief Get the audio engine (may return nullptr if audio init failed) */
        virtual AudioEngine* GetAudio() = 0;
        virtual const AudioEngine* GetAudio() const = 0;

        /** @brief Get the physics system (may return nullptr if not initialized) */
        virtual PhysicsSystem* GetPhysics() = 0;
        virtual const PhysicsSystem* GetPhysics() const = 0;

        /** @brief Get the animation system (may return nullptr if not initialized) */
        virtual Animation::AnimationSystem* GetAnimation() { return nullptr; }
        virtual const Animation::AnimationSystem* GetAnimation() const { return nullptr; }

        /** @brief Get the AI system (may return nullptr if not initialized) */
        virtual AI::AISystem* GetAI() { return nullptr; }
        virtual const AI::AISystem* GetAI() const { return nullptr; }

        /** @brief Get the network manager (may return nullptr if networking is disabled) */
        virtual NetworkManager* GetNetwork() { return nullptr; }
        virtual const NetworkManager* GetNetwork() const { return nullptr; }

        /** @brief Get the ECS world (may return nullptr if not initialized) */
        virtual World* GetWorld() { return nullptr; }
        virtual const World* GetWorld() const { return nullptr; }

        /** @brief Get the scene manager (may return nullptr if not initialized) */
        virtual SceneManager* GetSceneManager() { return nullptr; }
        virtual const SceneManager* GetSceneManager() const { return nullptr; }

        /** @brief Get the AngelScript engine (may return nullptr if scripting is disabled) */
        virtual AngelScriptEngine* GetScriptEngine() { return nullptr; }
        virtual const AngelScriptEngine* GetScriptEngine() const { return nullptr; }

        /** @brief Get the save system (may return nullptr if not initialized) */
        virtual SaveSystem* GetSaveSystem() { return nullptr; }
        virtual const SaveSystem* GetSaveSystem() const { return nullptr; }

        /** @brief Get the coroutine scheduler (may return nullptr if not initialized) */
        virtual CoroutineScheduler* GetCoroutineScheduler() { return nullptr; }
        virtual const CoroutineScheduler* GetCoroutineScheduler() const { return nullptr; }

        /** @brief Check if the engine is running in headless/dedicated server mode */
        virtual bool IsHeadless() const { return false; }

        /** @brief Get the packed engine version (0xMMmmpp) */
        virtual uint32_t GetEngineVersion() const = 0;

        /** @brief Get the SDK ABI version */
        virtual uint32_t GetSDKVersion() const = 0;

        /**
         * @brief Initialize all registered subsystems in dependency order
         *
         * Performs a topological sort of subsystems based on declared dependencies
         * and calls Initialize() on each in the correct order.
         *
         * @return true if all subsystems initialized successfully
         */
        virtual bool InitializeAll() { return true; }

        /**
         * @brief Shut down all subsystems in reverse dependency order
         *
         * Calls Shutdown() on each subsystem in reverse topological order,
         * ensuring dependents are shut down before their dependencies.
         */
        virtual void ShutdownAll() {}
    };

} // namespace Spark
