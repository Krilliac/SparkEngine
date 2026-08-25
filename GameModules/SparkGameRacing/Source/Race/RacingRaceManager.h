/**
 * @file RacingRaceManager.h
 * @brief Race lifecycle, timing, and position tracking
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the full race lifecycle from countdown through finish, tracks
 * lap times and split times at checkpoints, computes live positions,
 * and supports multiple race modes.
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
     * @brief Race format modes
     */
    enum class RaceMode : uint8_t
    {
        TimeTrial = 0,    ///< Solo — beat the clock
        SingleRace = 1,   ///< One race against AI
        Championship = 2, ///< Multi-race series with points
        Count = 3
    };

    /**
     * @brief Per-racer state tracked by the race manager
     */
    struct RacerState
    {
        uint32_t vehicleId = 0;
        std::string name;
        bool isPlayer = false;

        // Position tracking
        uint32_t currentLap = 0;
        uint32_t lastCheckpoint = 0;
        float distanceAlongTrack = 0.0f; ///< For position sorting
        uint32_t position = 0;           ///< 1-based race position

        // Timing
        float totalTime = 0.0f;
        float currentLapTime = 0.0f;
        float bestLapTime = -1.0f; ///< -1 means no completed lap yet
        std::vector<float> splitTimes;
        std::vector<float> lapTimes;

        // Status
        bool finished = false;
        bool dnf = false;
        float finishTime = 0.0f;
        uint32_t championshipPoints = 0;
    };

    /**
     * @brief Championship series standing
     */
    struct ChampionshipEntry
    {
        uint32_t vehicleId = 0;
        std::string name;
        uint32_t totalPoints = 0;
        uint32_t wins = 0;
    };

    /** Serializable race-manager state kept outside the ECS world. */
    struct RacingRaceSnapshot
    {
        std::vector<RacerState> racers;
        std::vector<ChampionshipEntry> championship;
        RaceState state = RaceState::Countdown;
        RaceMode mode = RaceMode::SingleRace;
        float countdownTimer = 3.0f;
        float raceTime = 0.0f;
        uint32_t totalLaps = 3;
        float dnfTimeout = 300.0f;
    };

    /**
     * @brief Orchestrates race state, timing, and results
     *
     * Call StartRace() to begin a countdown. Each frame, Update() advances
     * timers, checks checkpoint crossings via the track system, updates
     * positions, and transitions to Finished when all racers complete
     * the required laps (or DNF on timeout).
     */
    class RacingRaceManager
    {
      public:
        RacingRaceManager() = default;
        ~RacingRaceManager() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Register a racer before the race starts
        void RegisterRacer(uint32_t vehicleId, const std::string& name, bool isPlayer);

        /// Begin the countdown sequence
        void StartRace(RaceMode mode, uint32_t totalLaps);

        /// Notify that a racer crossed a checkpoint
        void OnCheckpointCrossed(uint32_t vehicleId, uint32_t checkpointIndex);

        /// Notify that a racer completed a lap
        void OnLapCompleted(uint32_t vehicleId);

        /// Mark one racer as did-not-finish without ending a race that still has active racers.
        void MarkDNF(uint32_t vehicleId);

        /// Update a racer's track distance (for position sorting)
        void UpdateRacerDistance(uint32_t vehicleId, float distance);

        /// Recompute standings after a batch of current-frame distance updates.
        void RefreshPositions();

        RaceState GetState() const { return m_state; }
        RaceMode GetMode() const { return m_mode; }
        float GetCountdownTimer() const { return m_countdownTimer; }
        float GetRaceTime() const { return m_raceTime; }
        uint32_t GetTotalLaps() const { return m_totalLaps; }

        const RacerState* GetRacer(uint32_t vehicleId) const;
        const std::vector<RacerState>& GetRacers() const { return m_racers; }
        size_t GetRacerCount() const { return m_racers.size(); }
        uint32_t GetPlayerPosition() const;
        float GetPlayerBestLap() const;

        RacingRaceSnapshot CaptureState() const;
        bool RestoreState(const RacingRaceSnapshot& snapshot);

        std::string GetStandingsString() const;
        std::string GetResultsString() const;

      private:
        void UpdateCountdown(float deltaTime);
        void UpdateRacing(float deltaTime);
        void SortPositions();
        void CheckFinishConditions();
        void AwardChampionshipPoints();

        /// Points awarded per position: 25, 18, 15, 12, 10, 8, 6, 4, 2, 1
        static uint32_t GetPointsForPosition(uint32_t position);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<RacerState> m_racers;
        std::vector<ChampionshipEntry> m_championship;

        RaceState m_state = RaceState::Countdown;
        RaceMode m_mode = RaceMode::SingleRace;
        float m_countdownTimer = 3.0f;
        float m_raceTime = 0.0f;
        uint32_t m_totalLaps = 3;
        float m_dnfTimeout = 300.0f; ///< 5 minutes before DNF
        bool m_initialized{false};
    };

} // namespace Racing
