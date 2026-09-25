/**
 * @file RTSEngineSystems.h
 * @brief Wires SparkEngine subsystems (AI, events, audio, weather, destruction,
 *        save, coroutines) into the RTS game module
 * @author Spark Engine Team
 * @date 2026
 *
 * RTSEngineSystems owns the configuration and subscription state for every
 * engine service the RTS module consumes. It is created, updated, and
 * destroyed by SparkGameRTSModule alongside the six gameplay subsystems.
 */

#pragma once

#include "Simulation/RTSSkirmishSimulation.h"
#include "Spark/SparkSDK.h"
#include "Utils/EventBus.h"

#include <vector>

namespace RTS
{
    /**
     * @brief Bridges SparkEngine services into the RTS module
     *
     * Registers AI behavior trees, event subscriptions, music tracks,
     * weather effects, destruction patterns, save-state serializers,
     * and coroutine-based timers for RTS gameplay.
     */
    class RTSEngineSystems
    {
      public:
        RTSEngineSystems() = default;
        ~RTSEngineSystems() = default;

        RTSEngineSystems(const RTSEngineSystems&) = delete;
        RTSEngineSystems& operator=(const RTSEngineSystems&) = delete;

        /**
         * @brief Initialize all engine-system integrations.
         * @param context     Engine context providing access to subsystems.
         * @param systems     Gameplay systems whose state SaveMatch/LoadMatch persist (non-owning).
         * @param simulation  Fixed-step skirmish clock saved and resumed with the match (non-owning).
         * @return true on success, false if a required subsystem is missing.
         */
        bool Initialize(Spark::IEngineContext* context, const RTSSkirmishSystems& systems = {},
                        RTSSkirmishSimulation* simulation = nullptr);

        /**
         * @brief Per-frame update for engine-system integrations.
         * @param deltaTime  Frame delta time in seconds.
         */
        void Update(float deltaTime);

        /** @brief Tear down subscriptions and release resources. */
        void Shutdown();

        // --- Save / Load helpers exposed for console commands ---

        /** @brief Save the full skirmish state (records, orders, match, fog, and sim tick) to the given slot. */
        bool SaveMatch(const std::string& slotName) const;

        /**
         * @brief Load a skirmish from the given slot and resume it at the saved tick.
         *
         * Only the current snapshot version is accepted; a version 1 slot, or any damaged or truncated state,
         * is rejected without changing the running match.
         */
        bool LoadMatch(const std::string& slotName) const;

        /** @brief Match SaveSystem's portable slot-name policy. */
        static bool IsValidSlotName(const std::string& slotName);

        /** @brief Set the weather type by name ("clear", "rain", "fog", "storm", "snow"). */
        void SetWeather(const std::string& weatherName) const;

        /** @brief Set the time of day (0-24 hour). */
        void SetTimeOfDay(float hour) const;

      private:
        /** @return true when every gameplay system and the simulation are bound. */
        bool HasMatchState() const;

        // Setup helpers called from Initialize()
        void SetupAI();
        void SetupEvents();
        void SetupAudio();
        void SetupWeather();
        void SetupDestruction();
        void SetupSaveSystem();
        void SetupCoroutines();

        Spark::IEngineContext* m_context{nullptr};
        RTSSkirmishSystems m_systems;
        RTSSkirmishSimulation* m_simulation{nullptr};

        // RAII event subscription handles (auto-unsubscribe on destruction)
        std::vector<Spark::SubscriptionHandle> m_eventHandles;

        // Track whether combat music is active to avoid redundant transitions
        bool m_inCombat{false};

        // Autosave interval tracking
        float m_autosaveTimer{0.0f};
        static constexpr float AutosaveInterval = 120.0f; // 2 minutes
    };

} // namespace RTS
