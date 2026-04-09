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
#include "Spark/ServiceInterfaces.h"

// Forward declarations — engine types accessible through the context.
// Modules include the specific engine headers they need for full definitions.
class GraphicsEngine;
class InputManager;
class Timer;
class AudioEngine;
class PhysicsSystem;
class SceneManager;
class AngelScriptEngine;
class AssetPipeline;
class World;
class SparkEngineCamera;

namespace Spark
{

    class EventBus;

    namespace Animation
    {
        class AnimationManager;
        using AnimationSystem = AnimationManager;
    } // namespace Animation

    namespace AI
    {
        class AISystem;
    }

    namespace Cinematic
    {
        class SequencerManager;
    }

    namespace Gameplay
    {
        class AbilitySystem;
        class WeaponSystem;
        class ConditionSystem;
        class InstanceManager;
    } // namespace Gameplay

    namespace VR
    {
        class VRSystem;
    }

    namespace Streaming
    {
        class SeamlessAreaManager;
    }

    namespace Audio
    {
        class MusicManager;
    }

    class VirtualFileSystem;

    namespace Net
    {
        class NetworkManager;
    }
    using NetworkManager = Net::NetworkManager;
    class SaveSystem;
    class CoroutineScheduler;
    class LocalFileCache;
    class AssetRegistry;
    class WeatherSystem;
    class TimeOfDaySystem;
    class DialogueSystem;
    class ModSystem;
    class ReplaySystem;
    class LocalizationSystem;
    class TweenSystem;
    class DestructionSystem;

    namespace UI
    {
        class UISystem;
    }

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
     *
     * All optional getters return nullptr when the subsystem is not initialized
     * or disabled. Always null-check before use.
     */
    class IEngineContext
    {
      public:
        virtual ~IEngineContext() = default;

        // =====================================================================
        // Core subsystems (always available after engine init)
        // =====================================================================

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

        // =====================================================================
        // Optional subsystems (may return nullptr if not initialized/disabled)
        // =====================================================================

        /** @brief Get the audio engine */
        virtual ::AudioEngine* GetAudio() = 0;
        virtual const ::AudioEngine* GetAudio() const = 0;

        /** @brief Get the physics system */
        virtual PhysicsSystem* GetPhysics() = 0;
        virtual const PhysicsSystem* GetPhysics() const = 0;

        /** @brief Get the animation system */
        virtual Animation::AnimationSystem* GetAnimation() { return nullptr; }
        virtual const Animation::AnimationSystem* GetAnimation() const { return nullptr; }

        /** @brief Get the AI system */
        virtual AI::AISystem* GetAI() { return nullptr; }
        virtual const AI::AISystem* GetAI() const { return nullptr; }

        /** @brief Get the network manager (requires ENABLE_NETWORKING) */
        virtual NetworkManager* GetNetwork() { return nullptr; }
        virtual const NetworkManager* GetNetwork() const { return nullptr; }

        /** @brief Get the network lifecycle interface (DI-friendly). */
        virtual INetworkService* GetNetworkService() { return nullptr; }
        virtual const INetworkService* GetNetworkService() const { return nullptr; }

        /** @brief Get the telemetry lifecycle interface (DI-friendly). */
        virtual ITelemetryService* GetTelemetryService() { return nullptr; }
        virtual const ITelemetryService* GetTelemetryService() const { return nullptr; }

        /** @brief Get the gameplay tag lifecycle interface (DI-friendly). */
        virtual IGameplayTagService* GetGameplayTagService() { return nullptr; }
        virtual const IGameplayTagService* GetGameplayTagService() const { return nullptr; }

        /** @brief Get the ECS world for entity/component access */
        virtual World* GetWorld() { return nullptr; }
        virtual const World* GetWorld() const { return nullptr; }

        /** @brief Get the scene manager */
        virtual SceneManager* GetSceneManager() { return nullptr; }
        virtual const SceneManager* GetSceneManager() const { return nullptr; }

        /** @brief Get the AngelScript engine (requires scripting enabled) */
        virtual AngelScriptEngine* GetScriptEngine() { return nullptr; }
        virtual const AngelScriptEngine* GetScriptEngine() const { return nullptr; }

        /** @brief Get the save system */
        virtual SaveSystem* GetSaveSystem() { return nullptr; }
        virtual const SaveSystem* GetSaveSystem() const { return nullptr; }

        /** @brief Get the asset pipeline for model/texture/audio loading */
        virtual ::AssetPipeline* GetAssetPipeline() { return nullptr; }
        virtual const ::AssetPipeline* GetAssetPipeline() const { return nullptr; }

        /** @brief Get the coroutine scheduler for async tasks */
        virtual CoroutineScheduler* GetCoroutineScheduler() { return nullptr; }
        virtual const CoroutineScheduler* GetCoroutineScheduler() const { return nullptr; }

        /** @brief Get the local file cache */
        virtual LocalFileCache* GetFileCache() { return nullptr; }
        virtual const LocalFileCache* GetFileCache() const { return nullptr; }

        /** @brief Get the asset registry for handle-based asset lookups */
        virtual AssetRegistry* GetAssetRegistry() { return nullptr; }
        virtual const AssetRegistry* GetAssetRegistry() const { return nullptr; }

        /** @brief Get the weather system */
        virtual WeatherSystem* GetWeather() { return nullptr; }
        virtual const WeatherSystem* GetWeather() const { return nullptr; }

        /** @brief Get the time-of-day system (day/night cycle) */
        virtual TimeOfDaySystem* GetTimeOfDay() { return nullptr; }
        virtual const TimeOfDaySystem* GetTimeOfDay() const { return nullptr; }

        /** @brief Get the UI system */
        virtual UI::UISystem* GetUI() { return nullptr; }
        virtual const UI::UISystem* GetUI() const { return nullptr; }

        /** @brief Get the dialogue system */
        virtual DialogueSystem* GetDialogue() { return nullptr; }
        virtual const DialogueSystem* GetDialogue() const { return nullptr; }

        /** @brief Get the modding system */
        virtual ModSystem* GetModSystem() { return nullptr; }
        virtual const ModSystem* GetModSystem() const { return nullptr; }

        /** @brief Get the replay recording/playback system */
        virtual ReplaySystem* GetReplay() { return nullptr; }
        virtual const ReplaySystem* GetReplay() const { return nullptr; }

        /** @brief Get the localization/i18n system */
        virtual LocalizationSystem* GetLocalization() { return nullptr; }
        virtual const LocalizationSystem* GetLocalization() const { return nullptr; }

        /** @brief Get the tween/interpolation system */
        virtual TweenSystem* GetTween() { return nullptr; }
        virtual const TweenSystem* GetTween() const { return nullptr; }

        /** @brief Get the ability system (spells, auras, procs) */
        virtual Gameplay::AbilitySystem* GetAbilities() { return nullptr; }
        virtual const Gameplay::AbilitySystem* GetAbilities() const { return nullptr; }

        /** @brief Get the destruction system for destructible objects */
        virtual DestructionSystem* GetDestruction() { return nullptr; }
        virtual const DestructionSystem* GetDestruction() const { return nullptr; }

        /** @brief Get the cinematic sequencer manager */
        virtual Cinematic::SequencerManager* GetCinematic() { return nullptr; }
        virtual const Cinematic::SequencerManager* GetCinematic() const { return nullptr; }

        /** @brief Get the VR system (requires ENABLE_VR) */
        virtual VR::VRSystem* GetVR() { return nullptr; }
        virtual const VR::VRSystem* GetVR() const { return nullptr; }

        /** @brief Get the active camera */
        virtual ::SparkEngineCamera* GetCamera() { return nullptr; }
        virtual const ::SparkEngineCamera* GetCamera() const { return nullptr; }

        /** @brief Get the weapon system (weapon definitions, fire modes, recoil) */
        virtual Gameplay::WeaponSystem* GetWeapons() { return nullptr; }
        virtual const Gameplay::WeaponSystem* GetWeapons() const { return nullptr; }

        /** @brief Get the condition system (universal gameplay conditions) */
        virtual Gameplay::ConditionSystem* GetConditions() { return nullptr; }
        virtual const Gameplay::ConditionSystem* GetConditions() const { return nullptr; }

        /** @brief Get the instance manager (encounters, lockouts) */
        virtual Gameplay::InstanceManager* GetInstances() { return nullptr; }
        virtual const Gameplay::InstanceManager* GetInstances() const { return nullptr; }

        /** @brief Get the seamless area streaming manager */
        virtual Streaming::SeamlessAreaManager* GetAreaStreaming() { return nullptr; }
        virtual const Streaming::SeamlessAreaManager* GetAreaStreaming() const { return nullptr; }

        /** @brief Get the music manager (crossfading, playlists, combat intensity) */
        virtual Audio::MusicManager* GetMusic() { return nullptr; }
        virtual const Audio::MusicManager* GetMusic() const { return nullptr; }

        /** @brief Get the virtual file system (mount-priority layered filesystem) */
        virtual VirtualFileSystem* GetVFS() { return nullptr; }
        virtual const VirtualFileSystem* GetVFS() const { return nullptr; }

        // =====================================================================
        // Engine state queries
        // =====================================================================

        /** @brief Check if the engine is running in headless/dedicated server mode */
        virtual bool IsHeadless() const { return false; }

        /** @brief Get the packed engine version (0xMMmmpp) */
        virtual uint32_t GetEngineVersion() const = 0;

        /** @brief Get the SDK ABI version */
        virtual uint32_t GetSDKVersion() const = 0;

        /**
         * @brief Get elapsed time since engine start in seconds
         * @return Total runtime in seconds, or 0.0 if not available
         */
        virtual double GetElapsedTime() const { return 0.0; }

        /**
         * @brief Get the current frame number (monotonically increasing)
         * @return Frame count since engine start, or 0 if not available
         */
        virtual uint64_t GetFrameNumber() const { return 0; }

        // =====================================================================
        // Subsystem lifecycle
        // =====================================================================

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
