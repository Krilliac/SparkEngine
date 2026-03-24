/**
 * @file RacingHUDSystem.h
 * @brief Racing HUD overlay (speedometer, minimap, standings)
 * @author Spark Engine Team
 * @date 2026
 *
 * Renders racing-specific HUD elements using the engine's UI system
 * and ImGui for debug visualization.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RacingEnums.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Racing
{

    /**
     * @brief Data fed to the HUD each frame
     */
    struct HUDData
    {
        // Speedometer
        float speed = 0.0f;      ///< Current speed in km/h
        float maxSpeed = 200.0f; ///< Max speed for gauge range
        float rpm = 0.0f;        ///< Engine RPM (0-1 normalized)
        uint32_t gear = 1;       ///< Current gear number

        // Race info
        uint32_t position = 1;    ///< Current race position
        uint32_t totalRacers = 8; ///< Total number of racers
        uint32_t currentLap = 1;
        uint32_t totalLaps = 3;
        float lapTime = 0.0f;
        float bestLapTime = -1.0f;
        float totalTime = 0.0f;

        // Boost
        float nitroLevel = 1.0f;  ///< 0-1 nitro remaining
        float boostActive = 0.0f; ///< Remaining boost time

        // Race state
        RaceState raceState = RaceState::Countdown;
        float countdown = 3.0f;
    };

    /**
     * @brief Entry in the minimap/standings display
     */
    struct MinimapEntry
    {
        float x = 0.0f;
        float z = 0.0f;
        uint32_t position = 0;
        bool isPlayer = false;
    };

    /**
     * @brief Renders racing HUD elements
     *
     * The HUD is data-driven: the race manager and vehicle system push
     * current values into HUDData each frame, and the HUD system renders
     * gauges and overlays from that snapshot.
     */
    class RacingHUDSystem
    {
      public:
        RacingHUDSystem() = default;
        ~RacingHUDSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Update HUD data from race systems
        void SetHUDData(const HUDData& data);

        /// Update minimap entries from race manager positions
        void SetMinimapEntries(const std::vector<MinimapEntry>& entries);

        /// Toggle individual HUD elements
        void SetSpeedometerVisible(bool visible) { m_showSpeedometer = visible; }
        void SetMinimapVisible(bool visible) { m_showMinimap = visible; }
        void SetStandingsVisible(bool visible) { m_showStandings = visible; }

        const HUDData& GetHUDData() const { return m_data; }

      private:
        void RenderSpeedometer();
        void RenderTachometer();
        void RenderMinimap();
        void RenderPositionIndicator();
        void RenderLapCounter();
        void RenderBoostMeter();
        void RenderCountdown();

        static const char* GetPositionSuffix(uint32_t position);

        Spark::IEngineContext* m_context{nullptr};
        HUDData m_data;
        std::vector<MinimapEntry> m_minimapEntries;

        bool m_showSpeedometer = true;
        bool m_showMinimap = true;
        bool m_showStandings = true;
        bool m_initialized{false};
    };

} // namespace Racing
