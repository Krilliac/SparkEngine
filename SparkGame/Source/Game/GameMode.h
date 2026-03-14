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
#include "Core/Platform.h"
#include "Utils/StateMachine.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

namespace Spark
{

    /**
 * @brief Available game mode types
 */
    enum class GameModeType
    {
        FreePlay,         ///< No rules, sandbox mode
        Deathmatch,       ///< Free-for-all, score limit
        TeamDeathmatch,   ///< Team-based kill scoring
        CaptureTheFlag,   ///< Flag capture objectives
        Domination,       ///< Control point capture
        Elimination,      ///< Last player/team standing
        GunGame,          ///< Weapon progression on kills
        SearchAndDestroy, ///< Plant/defuse objectives
        KingOfTheHill,    ///< Hold area for time
        Survival          ///< Wave-based AI defense
    };

    /**
 * @brief Team identifiers
 */
    enum class Team
    {
        None = 0,
        Alpha = 1,
        Bravo = 2,
        Spectator = 3
    };

    /**
 * @brief Spawn point definition
 */
    struct SpawnPoint
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 rotation; ///< Euler angles
        Team team = Team::None;
        bool isActive = true;
        std::string name;

        SpawnPoint() : position{}, rotation{} {}
        SpawnPoint(float x, float y, float z) : position{x, y, z}, rotation{} {}
    };

    /**
 * @brief Player score tracking
 */
    struct PlayerScore
    {
        std::string playerName;
        Team team = Team::None;
        int kills = 0;
        int deaths = 0;
        int assists = 0;
        int objectiveScore = 0;
        int totalScore = 0;
        float damageDealt = 0.0f;
        float damageTaken = 0.0f;
        int headshots = 0;
        float longestKillStreak = 0;
        int currentStreak = 0;

        float GetKDRatio() const { return deaths > 0 ? static_cast<float>(kills) / deaths : static_cast<float>(kills); }
    };

    /**
 * @brief Game mode rules configuration
 */
    struct GameModeRules
    {
        GameModeType type = GameModeType::FreePlay;
        std::string modeName = "Free Play";

        // Score limits
        int scoreLimit = 50;      ///< Score to win (kills for DM, points for objectives)
        int roundLimit = 1;       ///< Number of rounds
        float timeLimit = 600.0f; ///< Time limit per round in seconds (0 = no limit)

        // Respawn settings
        float respawnDelay = 3.0f; ///< Seconds before respawn
        bool autoRespawn = true;   ///< Auto-respawn or wait for input
        int maxLives = 0;          ///< 0 = unlimited

        // Gameplay modifiers
        float damageMultiplier = 1.0f;
        float healthMultiplier = 1.0f;
        float speedMultiplier = 1.0f;
        bool friendlyFire = false;
        bool headshots = true;
        float headshotMultiplier = 2.0f;

        // Weapon restrictions
        bool allWeaponsAvailable = true;
        std::vector<int> allowedWeapons; ///< Weapon type IDs if restricted

        // Team settings
        bool teamsEnabled = false;
        int maxTeamSize = 8;
        bool autoBalance = true;

        // Score values
        int killPoints = 100;
        int deathPenalty = 0;
        int assistPoints = 25;
        int objectivePoints = 200;
        int headshotBonus = 50;
    };

    /**
 * @brief Round state
 */
    enum class RoundState
    {
        WaitingForPlayers,
        Countdown,
        InProgress,
        RoundEnd,
        MatchEnd
    };

    /**
 * @brief Round result
 */
    struct RoundResult
    {
        int roundNumber = 0;
        Team winningTeam = Team::None;
        std::string mvpPlayer;
        int alphaScore = 0;
        int bravoScore = 0;
        float roundDuration = 0.0f;
    };

    /**
 * @brief Game mode event callbacks
 */
    struct GameModeEvents
    {
        std::function<void(const std::string& player)> onPlayerKill;
        std::function<void(const std::string& player)> onPlayerDeath;
        std::function<void(int roundNum)> onRoundStart;
        std::function<void(const RoundResult&)> onRoundEnd;
        std::function<void(Team winner)> onMatchEnd;
        std::function<void(const std::string& player, int streak)> onKillStreak;
        std::function<void(const std::string& player)> onFirstBlood;
        std::function<void(Team team, int score)> onScoreUpdate;
    };

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
        bool m_firstBloodOccurred = false;

        std::unordered_map<std::string, PlayerScore> m_playerScores;
        std::vector<SpawnPoint> m_spawnPoints;
        std::vector<RoundResult> m_roundResults;

        // Team scores (for team modes)
        int m_alphaScore = 0;
        int m_bravoScore = 0;

        GameModeEvents m_events;

        void CheckWinCondition();
        void UpdateCountdown(float dt);
        Team GetLeadingTeam() const;
        std::string GetMVP() const;
    };

} // namespace Spark
