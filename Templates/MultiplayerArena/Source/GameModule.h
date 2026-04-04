/**
 * @file GameModule.h
 * @brief {{PROJECT_NAME}} — Multiplayer arena game module
 *
 * Multiplayer arena template with networking, lobby system, team management,
 * scoreboard, and respawn. Uses SparkEngine's NetworkManager for replication.
 */

#pragma once

#include <Spark/SparkSDK.h>

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Network player
// ============================================================================

struct NetPlayer
{
    uint32_t playerId = 0;
    std::string name;
    uint8_t team = 0;    ///< 0 = unassigned, 1 = red, 2 = blue
    float health = 100.0f;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    uint32_t assists = 0;
    uint32_t score = 0;
    bool isReady = false;
    bool isAlive = true;
    float respawnTimer = 0.0f;
};

// ============================================================================
// Match state
// ============================================================================

enum class MatchPhase : uint8_t
{
    Lobby,
    Countdown,
    InProgress,
    Overtime,
    PostMatch
};

struct MatchState
{
    MatchPhase phase = MatchPhase::Lobby;
    float matchTimer = 0.0f;
    float matchDuration = 300.0f;    ///< 5 minutes
    float countdownTimer = 5.0f;
    uint32_t teamRedScore = 0;
    uint32_t teamBlueScore = 0;
    uint32_t scoreLimit = 50;
    uint32_t minPlayers = 2;
};

// ============================================================================
// Multiplayer Arena Game Module
// ============================================================================

class {{PROJECT_NAME}}Module : public Spark::IModule
{
  public:
    {{PROJECT_NAME}}Module() = default;
    ~{{PROJECT_NAME}}Module() override = default;

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "{{PROJECT_NAME}}";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        m_match = MatchState{};
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        switch (m_match.phase)
        {
        case MatchPhase::Lobby:
            UpdateLobby(deltaTime);
            break;
        case MatchPhase::Countdown:
            m_match.countdownTimer -= deltaTime;
            if (m_match.countdownTimer <= 0.0f)
            {
                m_match.phase = MatchPhase::InProgress;
            }
            break;
        case MatchPhase::InProgress:
            UpdateMatch(deltaTime);
            break;
        case MatchPhase::PostMatch:
            break;
        default:
            break;
        }
    }

    void OnRender() override
    {
        // Render scoreboard, lobby UI, countdown, kill feed
    }

  private:
    void UpdateLobby([[maybe_unused]] float deltaTime)
    {
        uint32_t readyCount = 0;
        for (const auto& p : m_players)
        {
            if (p.isReady)
                readyCount++;
        }
        if (readyCount >= m_match.minPlayers && readyCount == m_players.size())
        {
            m_match.phase = MatchPhase::Countdown;
            m_match.countdownTimer = 5.0f;
        }
    }

    void UpdateMatch(float deltaTime)
    {
        m_match.matchTimer += deltaTime;

        // Respawn timers
        for (auto& p : m_players)
        {
            if (!p.isAlive)
            {
                p.respawnTimer -= deltaTime;
                if (p.respawnTimer <= 0.0f)
                {
                    p.isAlive = true;
                    p.health = 100.0f;
                }
            }
        }

        // Check win conditions
        if (m_match.teamRedScore >= m_match.scoreLimit || m_match.teamBlueScore >= m_match.scoreLimit ||
            m_match.matchTimer >= m_match.matchDuration)
        {
            m_match.phase = MatchPhase::PostMatch;
        }
    }

    Spark::IEngineContext* m_context = nullptr;
    MatchState m_match;
    std::vector<NetPlayer> m_players;
};
