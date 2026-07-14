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
#include "Core/Platform.h"
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
        DirectX::XMFLOAT3 position; ///< World-space spawn position.
        DirectX::XMFLOAT3 rotation; ///< Spawn look direction (euler angles).
        Team team = Team::None;     ///< Team restriction (None = any team).
        bool isActive = true;       ///< Whether this point is currently usable.
        std::string name;           ///< Display name for the editor.

        SpawnPoint() : position{}, rotation{} {}
        SpawnPoint(float x, float y, float z) : position{x, y, z}, rotation{} {}
    };

    /**
 * @brief Player score tracking
 */
    struct PlayerScore
    {
        std::string playerName;      ///< Player display name.
        Team team = Team::None;      ///< Player's team assignment.
        int kills = 0;               ///< Total kills this match.
        int deaths = 0;              ///< Total deaths this match.
        int assists = 0;             ///< Damage-assist kills.
        int objectiveScore = 0;      ///< Points from objectives (captures, plants, etc.).
        int totalScore = 0;          ///< Combined score from all sources.
        float damageDealt = 0.0f;    ///< Cumulative damage dealt.
        float damageTaken = 0.0f;    ///< Cumulative damage received.
        int headshots = 0;           ///< Headshot kills.
        float longestKillStreak = 0; ///< Best killstreak achieved.
        int currentStreak = 0;       ///< Active killstreak (reset on death).

        float GetKDRatio() const { return deaths > 0 ? static_cast<float>(kills) / deaths : static_cast<float>(kills); }
    };

    /**
 * @brief Game mode rules configuration
 */
    struct GameModeRules
    {
        GameModeType type = GameModeType::FreePlay; ///< Game mode type.
        std::string modeName = "Free Play";         ///< Display name.

        // Score limits
        int scoreLimit = 50;      ///< Score to win (kills for DM, points for objectives).
        int roundLimit = 1;       ///< Number of rounds per match.
        float timeLimit = 600.0f; ///< Time limit per round in seconds (0 = no limit).

        // Respawn settings
        float respawnDelay = 3.0f; ///< Seconds before respawn.
        bool autoRespawn = true;   ///< Auto-respawn or wait for input.
        int maxLives = 0;          ///< 0 = unlimited lives.

        // Gameplay modifiers
        float damageMultiplier = 1.0f;   ///< Global damage multiplier.
        float healthMultiplier = 1.0f;   ///< Global health multiplier.
        float speedMultiplier = 1.0f;    ///< Global speed multiplier.
        bool friendlyFire = false;       ///< Whether friendly fire is enabled.
        bool headshots = true;           ///< Whether headshots deal bonus damage.
        float headshotMultiplier = 2.0f; ///< Damage multiplier for headshots.

        // Weapon restrictions
        bool allWeaponsAvailable = true; ///< True = no weapon restrictions.
        std::vector<int> allowedWeapons; ///< Weapon type IDs if restricted.

        // Team settings
        bool teamsEnabled = false; ///< Whether team-based gameplay is active.
        int maxTeamSize = 8;       ///< Maximum players per team.
        bool autoBalance = true;   ///< Auto-balance teams on join.

        // Score values
        int killPoints = 100;      ///< Points awarded per kill.
        int deathPenalty = 0;      ///< Points deducted per death.
        int assistPoints = 25;     ///< Points awarded per assist.
        int objectivePoints = 200; ///< Points awarded per objective completion.
        int headshotBonus = 50;    ///< Bonus points for headshot kills.
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
        int roundNumber = 0;           ///< Which round this result is for (1-based).
        Team winningTeam = Team::None; ///< Team that won the round.
        std::string mvpPlayer;         ///< Most valuable player of the round.
        int alphaScore = 0;            ///< Team Alpha's score at round end.
        int bravoScore = 0;            ///< Team Bravo's score at round end.
        float roundDuration = 0.0f;    ///< How long the round lasted (seconds).
    };

    /**
 * @brief Game mode event callbacks
 */
    /// @brief Callbacks fired during game mode lifecycle events.
    struct GameModeEvents
    {
        std::function<void(const std::string& player)> onPlayerKill;             ///< Fired on each kill.
        std::function<void(const std::string& player)> onPlayerDeath;            ///< Fired on each death.
        std::function<void(int roundNum)> onRoundStart;                          ///< Fired when a round begins.
        std::function<void(const RoundResult&)> onRoundEnd;                      ///< Fired when a round ends.
        std::function<void(Team winner)> onMatchEnd;                             ///< Fired when the match concludes.
        std::function<void(const std::string& player, int streak)> onKillStreak; ///< Fired on killstreak milestones.
        std::function<void(const std::string& player)> onFirstBlood;             ///< Fired on the first kill.
        std::function<void(Team team, int score)> onScoreUpdate;                 ///< Fired when a team's score changes.
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
