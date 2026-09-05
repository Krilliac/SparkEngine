/**
 * @file GameMechanicsRespawn.cpp
 * @brief RespawnSystem: death scoring, respawn timing, and respawn publication.
 *
 * Split out of GameMechanics.cpp so the respawn state machine carries no
 * dependency on the renderer-owned Player class: the death -> timer -> respawn
 * loop is driven entirely by values and a PlayerRespawnEvent, which is what the
 * host game subscribes to in order to move and reactivate the player.
 */

#include "GameMechanics.h"

#include "Core/Platform.h"
#include "Engine/Events/EventSystem.h"
#include "Utils/EventBus.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <sstream>

namespace Spark
{
    RespawnSystem::RespawnSystem() {}

    bool RespawnSystem::Initialize()
    {
        m_spawnPoints.clear();
        m_killHistory.clear();
        m_respawnTimer = 0.0f;
        m_waitingForRespawn = false;
        m_missingBusReported = false;

        // Add default spawn point
        RespawnPoint defaultSpawn;
        defaultSpawn.name = "Default Spawn";
        defaultSpawn.position = {0, 2, -5};
        defaultSpawn.isActive = true;
        m_spawnPoints.push_back(defaultSpawn);

        return true;
    }

    void RespawnSystem::Update(float deltaTime)
    {
        if (!m_waitingForRespawn)
            return;

        // Clamp at zero. A free-running negative timer makes GetRespawnTimeRemaining()
        // report a countdown that keeps growing more negative while auto-respawn is off,
        // and re-enabling auto-respawn later would then fire an instant respawn out of
        // an arbitrarily stale value instead of the elapsed delay.
        m_respawnTimer = std::max(0.0f, m_respawnTimer - deltaTime);
        if (m_respawnTimer <= 0.0f && m_autoRespawn)
        {
            RespawnPlayer();
        }
    }

    int RespawnSystem::AddSpawnPoint(const RespawnPoint& point)
    {
        if ((int)m_spawnPoints.size() >= MAX_SPAWN_POINTS)
            return -1;
        m_spawnPoints.push_back(point);
        return static_cast<int>(m_spawnPoints.size() - 1);
    }

    void RespawnSystem::RemoveSpawnPoint(int index)
    {
        if (index >= 0 && index < (int)m_spawnPoints.size())
        {
            m_spawnPoints.erase(m_spawnPoints.begin() + index);
        }
    }

    RespawnPoint RespawnSystem::GetBestSpawnPoint(int teamID) const
    {
        const RespawnPoint* best = nullptr;
        int bestPriority = -999999;

        for (const auto& point : m_spawnPoints)
        {
            if (!point.isActive)
                continue;
            if (teamID >= 0 && point.teamID >= 0 && point.teamID != teamID)
                continue;
            if (point.priority > bestPriority)
            {
                bestPriority = point.priority;
                best = &point;
            }
        }

        if (best)
            return *best;

        // Default fallback
        RespawnPoint fallback;
        fallback.name = "Fallback";
        fallback.position = {0, 2, 0};
        return fallback;
    }

    void RespawnSystem::OnPlayerDeath(const std::string& killerName, const std::string& weapon, bool headshot)
    {
        m_waitingForRespawn = true;
        m_respawnTimer = m_respawnDelay;

        // Update score
        m_playerScore.deaths++;
        m_playerScore.currentStreak = 0;

        // Record kill
        RecordKill(killerName, "Player", weapon, headshot, 0.0f);
    }

    void RespawnSystem::ArmRespawn()
    {
        m_waitingForRespawn = true;
        m_respawnTimer = m_respawnDelay;
    }

    bool RespawnSystem::RespawnPlayer()
    {
        if (!m_waitingForRespawn)
            return false;

        // Publishing PlayerRespawnEvent is the only thing that heals, moves and
        // reactivates the player. Consuming the pending death without a bus to publish
        // on would leave this state machine believing the player respawned while the
        // player stays dead and its Update() keeps early-returning - an unrecoverable
        // silent death. Keep the death pending and report the failure instead.
        if (!m_eventBus)
        {
            if (!m_missingBusReported)
            {
                m_missingBusReported = true;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "RespawnSystem: no event bus attached - the pending respawn cannot be published and "
                               "the player stays dead");
            }
            return false;
        }

        const RespawnPoint spawn = GetBestSpawnPoint(-1);

        m_waitingForRespawn = false;
        m_respawnTimer = 0.0f;

        PlayerRespawnEvent event{};
        event.entityId = 0;
        event.spawnX = spawn.position.x;
        event.spawnY = spawn.position.y;
        event.spawnZ = spawn.position.z;
        m_eventBus->Publish(event);
        return true;
    }

    void RespawnSystem::RecordKill(const std::string& killer, const std::string& victim, const std::string& weapon,
                                   bool headshot, float distance)
    {
        KillRecord record;
        record.killerName = killer;
        record.victimName = victim;
        record.weaponName = weapon;
        record.headshot = headshot;
        record.distance = distance;
        record.timestamp = std::chrono::steady_clock::now();

        m_killHistory.push_back(record);

        // Trim history
        while ((int)m_killHistory.size() > MAX_KILL_HISTORY)
        {
            m_killHistory.erase(m_killHistory.begin());
        }
    }

    std::string RespawnSystem::Console_GetScoreboard() const
    {
        std::stringstream ss;
        ss << "=== SCOREBOARD ===\n";
        ss << "Kills: " << m_playerScore.kills << "\n";
        ss << "Deaths: " << m_playerScore.deaths << "\n";
        ss << "K/D Ratio: " << m_playerScore.GetKDRatio() << "\n";
        ss << "Current Streak: " << m_playerScore.currentStreak << "\n";
        ss << "Longest Streak: " << m_playerScore.longestStreak << "\n";
        ss << "Score: " << m_playerScore.score << "\n";
        ss << "Vehicle Kills: " << m_playerScore.vehicleKills << "\n";
        ss << "Damage Dealt: " << m_playerScore.totalDamageDealt << "\n";
        ss << "Damage Received: " << m_playerScore.totalDamageReceived << "\n";
        return ss.str();
    }

    std::string RespawnSystem::Console_GetKillHistory() const
    {
        std::stringstream ss;
        ss << "=== KILL HISTORY (last " << m_killHistory.size() << ") ===\n";
        for (const auto& kill : m_killHistory)
        {
            ss << kill.killerName << " -> " << kill.victimName << " [" << kill.weaponName << "]"
               << (kill.headshot ? " HEADSHOT" : "") << "\n";
        }
        return ss.str();
    }
} // namespace Spark
