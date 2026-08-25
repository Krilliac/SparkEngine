/**
 * @file RacingEngineSystems.h
 * @brief Wires Spark Engine subsystems into racing gameplay
 * @author Spark Engine Team
 * @date 2026
 *
 * Integrates the racing module with engine infrastructure: MusicManager
 * (race/menu music, engine sounds), EventBus (collisions, race lifecycle),
 * SaveSystem (lap times, career progress), ReplaySystem (race replays,
 * ghost cars), WeatherSystem (track surface conditions), DestructionSystem
 * (barriers, vehicle panels), and CoroutineScheduler (countdown, pit stops).
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <string>
#include <vector>

namespace Racing
{
    class RacingAIDriver;
    class RacingRaceManager;
    class RacingTrackSystem;
    class RacingVehicleSystem;

    /**
     * @brief Bridges Spark Engine systems into racing gameplay logic.
     *
     * Constructed and owned by SparkGameRacingModule. On Initialize() it
     * registers racing-specific assets and handlers with the engine's audio,
     * event, save, replay, weather, destruction, and coroutine systems.
     *
     * Lifetime: created after all racing subsystems are initialized, destroyed
     * before any of them are shut down.
     */
    class RacingEngineSystems
    {
      public:
        RacingEngineSystems() = default;
        ~RacingEngineSystems() = default;

        RacingEngineSystems(const RacingEngineSystems&) = delete;
        RacingEngineSystems& operator=(const RacingEngineSystems&) = delete;

        /**
         * @brief Initialize all engine integrations for racing.
         * @param context  Engine context providing access to all subsystems.
         * @return true on success.
         */
        bool Initialize(Spark::IEngineContext* context, RacingVehicleSystem* vehicleSystem = nullptr,
                        RacingTrackSystem* trackSystem = nullptr, RacingRaceManager* raceManager = nullptr,
                        RacingAIDriver* aiDriver = nullptr);

        /** @brief Per-frame update for engine system integration. */
        void Update(float deltaTime);

        /** @brief Release subscriptions and clean up registrations. */
        void Shutdown();

        /** @brief Render debug UI for engine system integrations. */
        void RenderDebugUI();

        // ---- Console command helpers ----

        /** @brief Save race progress to a named slot. */
        std::string SaveRaceData(const std::string& slotName);

        /** @brief Load race progress from a named slot. */
        std::string LoadRaceData(const std::string& slotName);

        /** @brief Match SaveSystem's portable slot-name policy. */
        static bool IsValidSlotName(const std::string& slotName);

        /** @brief Start or stop replay recording/playback. */
        std::string ToggleReplay(const std::string& action);

        /** @brief Toggle ghost car playback for a given track. */
        std::string ToggleGhost(const std::string& trackName);

        /** @brief Set weather type by name (clear/rain/storm). */
        std::string SetWeather(const std::string& weatherName);

      private:
        void RegisterMusicTracks();
        void SubscribeToEvents();
        void SetupSaveSystem();
        void SetupReplay();
        void SetupWeather();
        void SetupDestruction();
        void RegisterCoroutines();

        Spark::IEngineContext* m_context = nullptr;
        RacingVehicleSystem* m_vehicleSystem = nullptr;
        RacingTrackSystem* m_trackSystem = nullptr;
        RacingRaceManager* m_raceManager = nullptr;
        RacingAIDriver* m_aiDriver = nullptr;
        bool m_initialized = false;

        // RAII event handles -- auto-unsubscribe on destruction
        std::vector<Spark::SubscriptionHandle> m_eventHandles;

        // Weather-to-grip mapping state
        float m_trackGripMultiplier = 1.0f;
    };

} // namespace Racing
