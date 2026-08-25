/**
 * @file GameMode.cpp
 * @brief Implementation of the FPS game mode system
 * @author Spark Engine Team
 * @date 2025
 */

#include "GameMode.h"
#include "Utils/ContainerUtils.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Spark
{

    GameMode::GameMode() = default;

    bool GameMode::Initialize(const GameModeRules& rules)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        const bool validRules = rules.scoreLimit >= 0 && rules.roundLimit > 0 && std::isfinite(rules.timeLimit) &&
                                rules.timeLimit >= 0.0f && std::isfinite(rules.respawnDelay) &&
                                rules.respawnDelay >= 0.0f && std::isfinite(rules.damageMultiplier) &&
                                rules.damageMultiplier > 0.0f && std::isfinite(rules.healthMultiplier) &&
                                rules.healthMultiplier > 0.0f && std::isfinite(rules.speedMultiplier) &&
                                rules.speedMultiplier > 0.0f;
        if (!validRules)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "Rejected invalid GameMode rules for '%s'",
                            rules.modeName.c_str());
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing GameMode: %s", rules.modeName.c_str());
        m_rules = rules;
        m_roundState = RoundState::WaitingForPlayers;
        m_matchActive = false;
        m_currentRound = 0;
        m_alphaScore = 0;
        m_bravoScore = 0;
        m_roundAlphaScore = 0;
        m_roundBravoScore = 0;
        m_firstBloodOccurred = false;
        m_roundEndTimer = 0.0f;
        m_playerScores.clear();
        m_roundPlayerKills.clear();
        m_roundPlayerScores.clear();
        m_roundResults.clear();

        // Set up round-state FSM — callbacks keep m_roundState in sync so that
        // external code querying GetRoundState() always sees the correct value.
        m_roundFSM.Reset();

        m_roundFSM.AddState(RoundState::WaitingForPlayers);

        m_roundFSM.AddState(RoundState::Countdown, nullptr,
                            [this](float dt)
                            {
                                m_roundState = RoundState::Countdown;
                                UpdateCountdown(dt);
                            });

        m_roundFSM.AddState(RoundState::InProgress, nullptr,
                            [this](float dt)
                            {
                                m_roundState = RoundState::InProgress;
                                m_roundElapsed += dt;
                                if (m_rules.timeLimit > 0.0f)
                                {
                                    m_roundTimeRemaining -= dt;
                                    if (m_roundTimeRemaining <= 0.0f)
                                    {
                                        m_roundTimeRemaining = 0.0f;
                                        EndRound(GetLeadingTeam());
                                        return;
                                    }
                                }
                                CheckWinCondition();
                            });

        m_roundFSM.AddState(
            RoundState::RoundEnd, [this]() { m_roundState = RoundState::RoundEnd; },
            [this](float dt)
            {
                m_roundEndTimer = (std::max)(0.0f, m_roundEndTimer - dt);
                if (m_roundEndTimer <= 0.0f && m_matchActive && m_currentRound < m_rules.roundLimit)
                {
                    StartRound();
                }
            });
        m_roundFSM.AddState(RoundState::MatchEnd, [this]() { m_roundState = RoundState::MatchEnd; });

        m_roundFSM.Start(RoundState::WaitingForPlayers);
        return true;
    }

    void GameMode::Update(float deltaTime)
    {
        if (!m_matchActive || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        m_roundFSM.Tick(deltaTime);
    }

    void GameMode::StartMatch()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Starting match");
        m_matchActive = true;
        m_currentRound = 0;
        m_alphaScore = 0;
        m_bravoScore = 0;
        m_roundAlphaScore = 0;
        m_roundBravoScore = 0;
        m_firstBloodOccurred = false;
        m_roundResults.clear();

        // Reset all player scores
        for (auto& [name, score] : m_playerScores)
        {
            score.kills = 0;
            score.deaths = 0;
            score.assists = 0;
            score.objectiveScore = 0;
            score.totalScore = 0;
            score.damageDealt = 0;
            score.damageTaken = 0;
            score.headshots = 0;
            score.currentStreak = 0;
            score.longestKillStreak = 0.0f;
        }

        StartRound();
    }

    void GameMode::EndMatch()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Ending match");
        m_matchActive = false;
        m_roundState = RoundState::MatchEnd;
        m_roundFSM.TransitionTo(RoundState::MatchEnd);

        Team winner = GetLeadingTeam();
        if (m_events.onMatchEnd)
        {
            m_events.onMatchEnd(winner);
        }
    }

    void GameMode::StartRound()
    {
        if (!m_matchActive || m_currentRound >= m_rules.roundLimit)
        {
            return;
        }

        m_currentRound++;
        m_roundState = RoundState::Countdown;
        m_roundFSM.TransitionTo(RoundState::Countdown);
        m_countdownTimer = 3.0f; // 3 second countdown
        m_roundElapsed = 0.0f;
        m_roundTimeRemaining = m_rules.timeLimit;
        m_roundAlphaScore = 0;
        m_roundBravoScore = 0;
        m_roundPlayerKills.clear();
        m_roundPlayerScores.clear();

        // Reset per-round player stats
        for (auto& [name, score] : m_playerScores)
        {
            score.currentStreak = 0;
        }

        if (m_events.onRoundStart)
        {
            m_events.onRoundStart(m_currentRound);
        }
    }

    void GameMode::EndRound(Team winningTeam)
    {
        if (!m_matchActive || m_roundState == RoundState::RoundEnd || m_roundState == RoundState::MatchEnd)
        {
            return;
        }

        m_roundState = RoundState::RoundEnd;
        m_roundEndTimer = 3.0f;
        m_roundFSM.TransitionTo(RoundState::RoundEnd);

        RoundResult result;
        result.roundNumber = m_currentRound;
        result.winningTeam = winningTeam;
        result.mvpPlayer = GetMVP();
        result.alphaScore = m_roundAlphaScore;
        result.bravoScore = m_roundBravoScore;
        result.roundDuration = m_roundElapsed;
        m_roundResults.push_back(result);

        if (m_events.onRoundEnd)
        {
            m_events.onRoundEnd(result);
        }

        // Check if match should end
        if (m_currentRound >= m_rules.roundLimit)
        {
            EndMatch();
        }
    }

    void GameMode::UpdateCountdown(float dt)
    {
        m_countdownTimer = (std::max)(0.0f, m_countdownTimer - dt);
        if (m_countdownTimer <= 0.0f)
        {
            m_roundState = RoundState::InProgress;
            m_roundFSM.TransitionTo(RoundState::InProgress);
        }
    }

    // === Player Management ===

    void GameMode::AddPlayer(const std::string& name, Team team)
    {
        if (!Spark::ContainerUtils::Contains(m_playerScores, name))
        {
            PlayerScore score;
            score.playerName = name;
            score.team = team;
            m_playerScores[name] = score;
        }
    }

    void GameMode::RemovePlayer(const std::string& name)
    {
        m_playerScores.erase(name);
        m_roundPlayerKills.erase(name);
        m_roundPlayerScores.erase(name);
    }

    void GameMode::SetPlayerTeam(const std::string& name, Team team)
    {
        auto it = m_playerScores.find(name);
        if (it != m_playerScores.end())
        {
            it->second.team = team;
        }
    }

    // === Scoring ===

    void GameMode::RecordKill(const std::string& killer, const std::string& victim, bool headshot)
    {
        auto killerIt = m_playerScores.find(killer);
        auto victimIt = m_playerScores.find(victim);

        if (killer == victim)
        {
            if (victimIt != m_playerScores.end())
            {
                auto& score = victimIt->second;
                score.deaths++;
                score.totalScore -= m_rules.deathPenalty;
                score.currentStreak = 0;
                m_roundPlayerScores[victim] -= m_rules.deathPenalty;
                if (m_events.onPlayerDeath)
                {
                    m_events.onPlayerDeath(victim);
                }
            }
            return;
        }

        if (killerIt != m_playerScores.end())
        {
            auto& ks = killerIt->second;
            ks.kills++;
            ks.totalScore += m_rules.killPoints;
            ks.currentStreak++;
            ks.longestKillStreak = (std::max)(ks.longestKillStreak, static_cast<float>(ks.currentStreak));
            m_roundPlayerKills[killer]++;
            m_roundPlayerScores[killer] += m_rules.killPoints;

            if (headshot)
            {
                ks.headshots++;
                ks.totalScore += m_rules.headshotBonus;
                m_roundPlayerScores[killer] += m_rules.headshotBonus;
            }

            // First blood
            if (!m_firstBloodOccurred)
            {
                m_firstBloodOccurred = true;
                if (m_events.onFirstBlood)
                {
                    m_events.onFirstBlood(killer);
                }
            }

            // Kill streak notification
            if (ks.currentStreak >= 3 && m_events.onKillStreak)
            {
                m_events.onKillStreak(killer, ks.currentStreak);
            }

            // Update team score
            if (m_rules.teamsEnabled)
            {
                if (ks.team == Team::Alpha)
                {
                    m_alphaScore += m_rules.killPoints;
                    m_roundAlphaScore += m_rules.killPoints;
                }
                else if (ks.team == Team::Bravo)
                {
                    m_bravoScore += m_rules.killPoints;
                    m_roundBravoScore += m_rules.killPoints;
                }
                if (m_events.onScoreUpdate)
                {
                    m_events.onScoreUpdate(ks.team, ks.team == Team::Alpha ? m_alphaScore : m_bravoScore);
                }
            }

            if (m_events.onPlayerKill)
            {
                m_events.onPlayerKill(killer);
            }
        }

        if (victimIt != m_playerScores.end())
        {
            victimIt->second.deaths++;
            victimIt->second.totalScore -= m_rules.deathPenalty;
            victimIt->second.currentStreak = 0;
            m_roundPlayerScores[victim] -= m_rules.deathPenalty;

            if (m_events.onPlayerDeath)
            {
                m_events.onPlayerDeath(victim);
            }
        }
    }

    void GameMode::RecordAssist(const std::string& player)
    {
        auto it = m_playerScores.find(player);
        if (it != m_playerScores.end())
        {
            it->second.assists++;
            it->second.totalScore += m_rules.assistPoints;
            m_roundPlayerScores[player] += m_rules.assistPoints;
        }
    }

    void GameMode::RecordObjectiveScore(const std::string& player, int points)
    {
        auto it = m_playerScores.find(player);
        if (it != m_playerScores.end())
        {
            it->second.objectiveScore += points;
            it->second.totalScore += points;
            m_roundPlayerScores[player] += points;

            if (m_rules.teamsEnabled)
            {
                if (it->second.team == Team::Alpha)
                {
                    m_alphaScore += points;
                    m_roundAlphaScore += points;
                }
                else if (it->second.team == Team::Bravo)
                {
                    m_bravoScore += points;
                    m_roundBravoScore += points;
                }
            }
        }
    }

    void GameMode::AddScore(const std::string& player, int points)
    {
        auto it = m_playerScores.find(player);
        if (it != m_playerScores.end())
        {
            it->second.totalScore += points;
            m_roundPlayerScores[player] += points;
        }
    }

    // === Spawn Points ===

    void GameMode::AddSpawnPoint(const SpawnPoint& spawn)
    {
        m_spawnPoints.push_back(spawn);
    }

    void GameMode::ClearSpawnPoints()
    {
        m_spawnPoints.clear();
    }

    SpawnPoint GameMode::GetBestSpawnPoint(Team team) const
    {
        // Filter spawn points by team
        std::vector<const SpawnPoint*> candidates;
        for (const auto& sp : m_spawnPoints)
        {
            if (sp.isActive && (sp.team == team || sp.team == Team::None))
            {
                candidates.push_back(&sp);
            }
        }

        if (candidates.empty())
        {
            // Default spawn at origin
            return SpawnPoint(0.0f, 1.0f, 0.0f);
        }

        // Simple: pick a random-ish spawn (use hash of current round + time as pseudo-random)
        size_t idx = (m_currentRound * 7 + candidates.size() * 3) % candidates.size();
        return *candidates[idx];
    }

    // === Scoreboard ===

    const PlayerScore* GameMode::GetPlayerScore(const std::string& name) const
    {
        auto it = m_playerScores.find(name);
        return it != m_playerScores.end() ? &it->second : nullptr;
    }

    std::vector<PlayerScore> GameMode::GetScoreboard() const
    {
        std::vector<PlayerScore> scores;
        scores.reserve(m_playerScores.size());
        for (const auto& [name, score] : m_playerScores)
        {
            scores.push_back(score);
        }
        std::sort(scores.begin(), scores.end(),
                  [](const PlayerScore& a, const PlayerScore& b)
                  {
                      if (a.totalScore != b.totalScore)
                          return a.totalScore > b.totalScore;
                      if (a.kills != b.kills)
                          return a.kills > b.kills;
                      if (a.deaths != b.deaths)
                          return a.deaths < b.deaths;
                      return a.playerName < b.playerName;
                  });
        return scores;
    }

    std::vector<PlayerScore> GameMode::GetTeamScoreboard(Team team) const
    {
        std::vector<PlayerScore> scores;
        for (const auto& [name, score] : m_playerScores)
        {
            if (score.team == team)
            {
                scores.push_back(score);
            }
        }
        std::sort(scores.begin(), scores.end(),
                  [](const PlayerScore& a, const PlayerScore& b)
                  {
                      if (a.totalScore != b.totalScore)
                          return a.totalScore > b.totalScore;
                      if (a.kills != b.kills)
                          return a.kills > b.kills;
                      if (a.deaths != b.deaths)
                          return a.deaths < b.deaths;
                      return a.playerName < b.playerName;
                  });
        return scores;
    }

    int GameMode::GetTeamScore(Team team) const
    {
        if (team == Team::Alpha)
            return m_alphaScore;
        if (team == Team::Bravo)
            return m_bravoScore;
        return 0;
    }

    // === Win Conditions ===

    void GameMode::CheckWinCondition()
    {
        if (m_rules.type == GameModeType::FreePlay || m_rules.type == GameModeType::Survival)
            return;

        if (m_rules.teamsEnabled)
        {
            int targetScore = m_rules.scoreLimit;
            if (m_rules.type == GameModeType::TeamDeathmatch)
            {
                targetScore *= m_rules.killPoints;
            }
            else if (m_rules.type == GameModeType::CaptureTheFlag)
            {
                targetScore *= m_rules.objectivePoints;
            }

            if (m_roundAlphaScore >= targetScore)
            {
                EndRound(Team::Alpha);
            }
            else if (m_roundBravoScore >= targetScore)
            {
                EndRound(Team::Bravo);
            }
        }
        else
        {
            // FFA: check if any player reached score limit
            for (const auto& playerEntry : m_playerScores)
            {
                const auto& name = playerEntry.first;
                const auto roundKills = m_roundPlayerKills.find(name);
                if (roundKills != m_roundPlayerKills.end() && roundKills->second >= m_rules.scoreLimit)
                {
                    EndRound(Team::None);
                    break;
                }
            }
        }
    }

    Team GameMode::GetLeadingTeam() const
    {
        if (!m_rules.teamsEnabled)
            return Team::None;
        if (m_alphaScore > m_bravoScore)
            return Team::Alpha;
        if (m_bravoScore > m_alphaScore)
            return Team::Bravo;
        return Team::None; // Tie
    }

    std::string GameMode::GetMVP() const
    {
        std::string mvp;
        int highestScore = (std::numeric_limits<int>::min)();
        for (const auto& playerEntry : m_playerScores)
        {
            const auto& name = playerEntry.first;
            const auto roundScore = m_roundPlayerScores.find(name);
            const int value = roundScore != m_roundPlayerScores.end() ? roundScore->second : 0;
            if (value > highestScore || (value == highestScore && (mvp.empty() || name < mvp)))
            {
                highestScore = value;
                mvp = name;
            }
        }
        return mvp;
    }

    // === Presets ===

    GameModeRules GameMode::GetPreset(GameModeType type)
    {
        GameModeRules rules;
        rules.type = type;

        switch (type)
        {
        case GameModeType::FreePlay:
            rules.modeName = "Free Play";
            rules.scoreLimit = 0;
            rules.timeLimit = 0;
            rules.respawnDelay = 0;
            break;

        case GameModeType::Deathmatch:
            rules.modeName = "Deathmatch";
            rules.scoreLimit = 30;
            rules.timeLimit = 600.0f;
            rules.respawnDelay = 3.0f;
            rules.teamsEnabled = false;
            break;

        case GameModeType::TeamDeathmatch:
            rules.modeName = "Team Deathmatch";
            rules.scoreLimit = 75;
            rules.timeLimit = 600.0f;
            rules.respawnDelay = 5.0f;
            rules.teamsEnabled = true;
            rules.friendlyFire = false;
            break;

        case GameModeType::CaptureTheFlag:
            rules.modeName = "Capture the Flag";
            rules.scoreLimit = 3;
            rules.timeLimit = 900.0f;
            rules.roundLimit = 1;
            rules.respawnDelay = 5.0f;
            rules.teamsEnabled = true;
            rules.objectivePoints = 500;
            break;

        case GameModeType::Domination:
            rules.modeName = "Domination";
            rules.scoreLimit = 200;
            rules.timeLimit = 600.0f;
            rules.respawnDelay = 5.0f;
            rules.teamsEnabled = true;
            break;

        case GameModeType::Elimination:
            rules.modeName = "Elimination";
            rules.scoreLimit = 1;
            rules.roundLimit = 12;
            rules.timeLimit = 120.0f;
            rules.maxLives = 1;
            rules.autoRespawn = false;
            rules.teamsEnabled = true;
            break;

        case GameModeType::GunGame:
            rules.modeName = "Gun Game";
            rules.scoreLimit = 18;
            rules.timeLimit = 0;
            rules.respawnDelay = 2.0f;
            rules.allWeaponsAvailable = false;
            break;

        case GameModeType::SearchAndDestroy:
            rules.modeName = "Search and Destroy";
            rules.scoreLimit = 1;
            rules.roundLimit = 12;
            rules.timeLimit = 90.0f;
            rules.maxLives = 1;
            rules.autoRespawn = false;
            rules.teamsEnabled = true;
            break;

        case GameModeType::KingOfTheHill:
            rules.modeName = "King of the Hill";
            rules.scoreLimit = 250;
            rules.timeLimit = 600.0f;
            rules.respawnDelay = 5.0f;
            rules.teamsEnabled = true;
            break;

        case GameModeType::Survival:
            rules.modeName = "Survival";
            rules.scoreLimit = 0;
            rules.timeLimit = 0;
            rules.respawnDelay = 10.0f;
            rules.maxLives = 3;
            rules.teamsEnabled = false;
            rules.damageMultiplier = 1.5f;
            break;
        }

        return rules;
    }

    const char* GameMode::GameModeTypeToString(GameModeType type)
    {
        switch (type)
        {
        case GameModeType::FreePlay:
            return "Free Play";
        case GameModeType::Deathmatch:
            return "Deathmatch";
        case GameModeType::TeamDeathmatch:
            return "Team Deathmatch";
        case GameModeType::CaptureTheFlag:
            return "Capture the Flag";
        case GameModeType::Domination:
            return "Domination";
        case GameModeType::Elimination:
            return "Elimination";
        case GameModeType::GunGame:
            return "Gun Game";
        case GameModeType::SearchAndDestroy:
            return "Search and Destroy";
        case GameModeType::KingOfTheHill:
            return "King of the Hill";
        case GameModeType::Survival:
            return "Survival";
        default:
            return "Unknown";
        }
    }

} // namespace Spark
