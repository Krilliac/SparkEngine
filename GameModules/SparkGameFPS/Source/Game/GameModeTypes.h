/**
 * @file GameModeTypes.h
 * @brief Shared public types for the SparkGameFPS game-mode system
 */

#pragma once

#include "Core/Platform.h"

#include <functional>
#include <string>
#include <vector>

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

} // namespace Spark
