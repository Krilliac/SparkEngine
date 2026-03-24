/**
 * @file PlatformerEngineSystems.h
 * @brief Wires SparkEngine subsystems (audio, events, save, destruction,
 *        replay, coroutines, localization) into the Platformer game module
 * @author Spark Engine Team
 * @date 2026
 *
 * PlatformerEngineSystems owns the configuration and subscription state for
 * every engine service the Platformer module consumes. It is created, updated,
 * and destroyed by SparkGamePlatformerModule alongside the six gameplay subsystems.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include "Utils/EventBus.h"

#include <string>
#include <vector>

namespace Platformer
{

    /**
     * @brief Bridges SparkEngine services into the Platformer module
     *
     * Registers music tracks, event subscriptions, save-state serializers,
     * destruction fracture patterns, replay recording, coroutine-based timers,
     * and localized string tables for platformer gameplay.
     */
    class PlatformerEngineSystems
    {
      public:
        PlatformerEngineSystems() = default;
        ~PlatformerEngineSystems() = default;

        PlatformerEngineSystems(const PlatformerEngineSystems&) = delete;
        PlatformerEngineSystems& operator=(const PlatformerEngineSystems&) = delete;

        /**
         * @brief Initialize all engine-system integrations.
         * @param context  Engine context providing access to subsystems.
         * @return true on success, false if a required subsystem is missing.
         */
        bool Initialize(Spark::IEngineContext* context);

        /**
         * @brief Per-frame update for engine-system integrations.
         * @param deltaTime  Frame delta time in seconds.
         */
        void Update(float deltaTime);

        /** @brief Tear down subscriptions and release resources. */
        void Shutdown();

        // --- Save / Load helpers exposed for console commands ---

        /** @brief Save the platformer progress to the given slot. */
        bool SaveProgress(const std::string& slotName) const;

        /** @brief Load platformer progress from the given slot. */
        bool LoadProgress(const std::string& slotName) const;

        // --- Replay helpers exposed for console commands ---

        /** @brief Start recording a speedrun replay for the current level. */
        void StartReplayRecording();

        /** @brief Stop recording and save the replay. */
        void StopReplayRecording();

        /** @brief Toggle ghost playback of the personal-best replay. */
        void ToggleGhostPlayback();

      private:
        // Setup helpers called from Initialize()
        void SetupAudio();
        void SetupEvents();
        void SetupSaveSystem();
        void SetupDestruction();
        void SetupReplay();
        void SetupCoroutines();
        void SetupLocalization();

        Spark::IEngineContext* m_context{nullptr};

        // RAII event subscription handles (auto-unsubscribe on destruction)
        std::vector<Spark::SubscriptionHandle> m_eventHandles;

        // Track whether tense music is active to avoid redundant transitions
        bool m_nearHazard{false};

        // Replay state
        bool m_recording{false};
        bool m_ghostActive{false};

        // Autosave interval tracking
        float m_autosaveTimer{0.0f};
        static constexpr float AutosaveInterval = 60.0f; // 1 minute
    };

} // namespace Platformer
