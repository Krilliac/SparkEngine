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

#include "Core/PlatformerProgress.h"
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
         * @param context   Engine context providing access to subsystems.
         * @param progress  Gameplay systems whose progress SaveProgress/LoadProgress persist (non-owning; they
         *                  must outlive this object). Without them saving and loading fail.
         * @return true on success, false if a required subsystem is missing.
         */
        bool Initialize(Spark::IEngineContext* context, const PlatformerProgressSystems& progress = {});

        /**
         * @brief Per-frame update for engine-system integrations.
         * @param deltaTime  Frame delta time in seconds.
         */
        void Update(float deltaTime);

        /** @brief Tear down subscriptions and release resources. */
        void Shutdown();

        // --- Save / Load helpers exposed for console commands ---

        /**
         * @brief Save the ECS world plus platformer progress (PlatformerProgress::StateKey) to a slot.
         * @return false when the slot name is invalid, a subsystem or gameplay system is missing, or the write fails
         */
        bool SaveProgress(const std::string& slotName) const;

        /**
         * @brief Load a slot and restore platformer progress.
         *
         * The progress entry is decoded and validated against the live systems before the SaveSystem commits the
         * world, so a missing, corrupt, or incompatible entry leaves both the world and the gameplay systems
         * unchanged.
         */
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
        PlatformerProgressSystems m_progressSystems{};

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
