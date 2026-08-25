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

#include "Spark/SparkSDK.h"
#include "Utils/EventBus.h"

#include <vector>

namespace RTS
{
    class RTSBuildingSystem;
    class RTSCommandSystem;
    class RTSResourceSystem;
    class RTSUnitSystem;

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
         * @param context  Engine context providing access to subsystems.
         * @return true on success, false if a required subsystem is missing.
         */
        bool Initialize(Spark::IEngineContext* context, RTSUnitSystem* unitSystem = nullptr,
                        RTSBuildingSystem* buildingSystem = nullptr, RTSResourceSystem* resourceSystem = nullptr,
                        RTSCommandSystem* commandSystem = nullptr);

        /**
         * @brief Per-frame update for engine-system integrations.
         * @param deltaTime  Frame delta time in seconds.
         */
        void Update(float deltaTime);

        /** @brief Tear down subscriptions and release resources. */
        void Shutdown();

        // --- Save / Load helpers exposed for console commands ---

        /** @brief Save the full RTS match state to the given slot. */
        bool SaveMatch(const std::string& slotName) const;

        /** @brief Load an RTS match state from the given slot. */
        bool LoadMatch(const std::string& slotName) const;

        /** @brief Match SaveSystem's portable slot-name policy. */
        static bool IsValidSlotName(const std::string& slotName);

        /** @brief Set the weather type by name ("clear", "rain", "fog", "storm", "snow"). */
        void SetWeather(const std::string& weatherName) const;

        /** @brief Set the time of day (0-24 hour). */
        void SetTimeOfDay(float hour) const;

      private:
        // Setup helpers called from Initialize()
        void SetupAI();
        void SetupEvents();
        void SetupAudio();
        void SetupWeather();
        void SetupDestruction();
        void SetupSaveSystem();
        void SetupCoroutines();

        Spark::IEngineContext* m_context{nullptr};
        RTSUnitSystem* m_unitSystem{nullptr};
        RTSBuildingSystem* m_buildingSystem{nullptr};
        RTSResourceSystem* m_resourceSystem{nullptr};
        RTSCommandSystem* m_commandSystem{nullptr};

        // RAII event subscription handles (auto-unsubscribe on destruction)
        std::vector<Spark::SubscriptionHandle> m_eventHandles;

        // Track whether combat music is active to avoid redundant transitions
        bool m_inCombat{false};

        // Autosave interval tracking
        float m_autosaveTimer{0.0f};
        static constexpr float AutosaveInterval = 120.0f; // 2 minutes
    };

} // namespace RTS
