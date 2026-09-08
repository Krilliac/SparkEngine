/**
 * @file GameMode.h
 * @brief FPS game mode system with scoring, rounds, and rules
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides configurable game modes for FPS gameplay including
 * Deathmatch, Team Deathmatch, Capture the Flag, and more.
 */

#pragma once

#include "GameModeTypes.h"
#include "Utils/StateMachine.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{


    /**
 * @brief Complete FPS game mode manager
 *
 * Handles game rules, scoring, round management, spawn points,
 * and team balancing for various FPS game modes.
 */
    class GameMode
    {
      public:
        GameMode();
        ~GameMode() = default;

        /**
     * @brief Initialize the game mode with rules
     * @param rules Game mode rules configuration
     * @return true on success
     */
        bool Initialize(const GameModeRules& rules);

        /**
     * @brief Update game mode logic
     * @param deltaTime Frame delta time
     */
        void Update(float deltaTime);

        /**
     * @brief Start a new match
     */
        void StartMatch();

        /**
     * @brief End the current match
     */
        void EndMatch();

        /**
     * @brief Start a new round
     */
        void StartRound();

        /**
     * @brief End the current round
     * @param winningTeam Winning team for this round
     */
        void EndRound(Team winningTeam = Team::None);

        // === Player Management ===

        void AddPlayer(const std::string& name, Team team = Team::None);
        void RemovePlayer(const std::string& name);
        void SetPlayerTeam(const std::string& name, Team team);

        // === Scoring ===

        void RecordKill(const std::string& killer, const std::string& victim, bool headshot = false);
        void RecordAssist(const std::string& player);
        void RecordObjectiveScore(const std::string& player, int points);
        void AddScore(const std::string& player, int points);

        /**
         * @brief Restore a player's persisted kills/deaths/score after a save is loaded.
         *
         * The scoreboard is match state, not ECS state, so a loaded save has to put it
         * back explicitly; without this the counters silently reset to zero on quickload.
         * The player is registered first when the match does not know the name yet, so a
         * restore into a fresh session works.
         *
         * @return false when @p name is empty or a counter is negative (nothing written).
         */
        bool RestorePlayerScore(const std::string& name, int kills, int deaths, int totalScore);

        // === Spawn Points ===

        void AddSpawnPoint(const SpawnPoint& spawn);
        void ClearSpawnPoints();
        SpawnPoint GetBestSpawnPoint(Team team = Team::None) const;
        const std::vector<SpawnPoint>& GetSpawnPoints() const { return m_spawnPoints; }

        // === Getters ===

        const GameModeRules& GetRules() const { return m_rules; }
        GameModeRules& GetRules() { return m_rules; }
        RoundState GetRoundState() const { return m_roundState; }
        int GetCurrentRound() const { return m_currentRound; }
        float GetRoundTimeRemaining() const { return m_roundTimeRemaining; }
        float GetCountdownTime() const { return m_countdownTimer; }
        float GetRoundTransitionTime() const { return m_roundEndTimer; }
        bool IsMatchActive() const { return m_matchActive; }

        const PlayerScore* GetPlayerScore(const std::string& name) const;
        std::vector<PlayerScore> GetScoreboard() const;
        std::vector<PlayerScore> GetTeamScoreboard(Team team) const;
        int GetTeamScore(Team team) const;

        const std::vector<RoundResult>& GetRoundResults() const { return m_roundResults; }

        // === Events ===

        GameModeEvents& GetEvents() { return m_events; }

        // === Presets ===

        static GameModeRules GetPreset(GameModeType type);
        static const char* GameModeTypeToString(GameModeType type);

      private:
        GameModeRules m_rules;
        RoundState m_roundState = RoundState::WaitingForPlayers;
        Spark::StateMachine<RoundState> m_roundFSM; ///< Drives round lifecycle updates
        bool m_matchActive = false;
        int m_currentRound = 0;
        float m_roundTimeRemaining = 0.0f;
        float m_roundElapsed = 0.0f;
        float m_countdownTimer = 0.0f;
        float m_roundEndTimer = 0.0f;
        bool m_firstBloodOccurred = false;

        std::unordered_map<std::string, PlayerScore> m_playerScores;
        std::unordered_map<std::string, int> m_roundPlayerKills;
        std::unordered_map<std::string, int> m_roundPlayerScores;
        std::vector<SpawnPoint> m_spawnPoints;
        std::vector<RoundResult> m_roundResults;

        // Team scores (for team modes)
        int m_alphaScore = 0;
        int m_bravoScore = 0;
        int m_roundAlphaScore = 0;
        int m_roundBravoScore = 0;

        GameModeEvents m_events;

        void CheckWinCondition();
        void UpdateCountdown(float dt);
        Team GetLeadingTeam() const;
        std::string GetMVP() const;
    };

} // namespace Spark
