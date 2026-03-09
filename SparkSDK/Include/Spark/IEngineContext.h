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

namespace Spark
{

    class EventBus;

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

        /** @brief Check if the engine is running in headless/dedicated server mode */
        virtual bool IsHeadless() const { return false; }

        /** @brief Get the packed engine version (0xMMmmpp) */
        virtual uint32_t GetEngineVersion() const = 0;

        /** @brief Get the SDK ABI version */
        virtual uint32_t GetSDKVersion() const = 0;
    };

} // namespace Spark
