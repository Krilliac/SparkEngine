#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "GameMechanics.h"
#include "Player.h"
#include "Utils/Assert.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace DirectX;

namespace Spark {

// ============================================================================
// DamageZone Implementation
// ============================================================================

bool DamageZone::Contains(const XMFLOAT3& point) const
{
    if (!isActive) return false;
    return (point.x >= center.x - halfExtents.x && point.x <= center.x + halfExtents.x &&
            point.y >= center.y - halfExtents.y && point.y <= center.y + halfExtents.y &&
            point.z >= center.z - halfExtents.z && point.z <= center.z + halfExtents.z);
}

// ============================================================================
// DamageZoneSystem Implementation
// ============================================================================

DamageZoneSystem::DamageZoneSystem()
{
}

bool DamageZoneSystem::Initialize()
{
    m_zones.clear();
    m_damageTickTimer = 0.0f;
    return true;
}

void DamageZoneSystem::Update(float deltaTime, Player* player)
{
    if (!player || !player->IsAlive()) return;

    m_damageTickTimer += deltaTime;

    XMFLOAT3 playerPos = player->GetPosition();

    for (const auto& zone : m_zones) {
        if (!zone.isActive || !zone.Contains(playerPos)) continue;

        // Instant kill zones
        if (zone.instantKill) {
            player->TakeDamage(99999.0f);
            return;
        }

        // Periodic damage
        if (m_damageTickTimer >= zone.tickInterval) {
            float damage = zone.damagePerSecond * zone.tickInterval;

            switch (zone.type) {
            case DamageZoneType::LAVA:
                player->TakeDamage(damage);
                break;

            case DamageZoneType::ACID:
                player->TakeDamage(damage * 0.7f); // Acid is slower but persistent
                break;

            case DamageZoneType::ELECTRIC:
                // Electric does burst damage every tick
                player->TakeDamage(damage * 1.5f);
                break;

            case DamageZoneType::RADIATION:
                player->TakeDamage(damage * 0.5f); // Slow constant damage
                break;

            case DamageZoneType::VOID:
                player->TakeDamage(99999.0f); // Instant kill
                break;
            }
        }
    }

    // Reset tick timer
    if (m_damageTickTimer >= 0.5f) {
        m_damageTickTimer = 0.0f;
    }
}

int DamageZoneSystem::AddZone(const DamageZone& zone)
{
    if ((int)m_zones.size() >= MAX_DAMAGE_ZONES) return -1;
    m_zones.push_back(zone);
    return static_cast<int>(m_zones.size() - 1);
}

void DamageZoneSystem::RemoveZone(int index)
{
    if (index >= 0 && index < (int)m_zones.size()) {
        m_zones.erase(m_zones.begin() + index);
    }
}

void DamageZoneSystem::RemoveZoneByName(const std::string& name)
{
    m_zones.erase(
        std::remove_if(m_zones.begin(), m_zones.end(),
            [&name](const DamageZone& z) { return z.name == name; }),
        m_zones.end());
}

DamageZone* DamageZoneSystem::GetZone(int index)
{
    if (index >= 0 && index < (int)m_zones.size()) {
        return &m_zones[index];
    }
    return nullptr;
}

void DamageZoneSystem::ClearZones()
{
    m_zones.clear();
}

bool DamageZoneSystem::IsInDamageZone(const XMFLOAT3& position) const
{
    for (const auto& zone : m_zones) {
        if (zone.Contains(position)) return true;
    }
    return false;
}

DamageZoneType DamageZoneSystem::GetZoneTypeAt(const XMFLOAT3& position) const
{
    for (const auto& zone : m_zones) {
        if (zone.Contains(position)) return zone.type;
    }
    return DamageZoneType::LAVA; // default
}

int DamageZoneSystem::CreateLavaZone(const std::string& name, const XMFLOAT3& center,
                                      const XMFLOAT3& halfExtents)
{
    DamageZone zone;
    zone.name = name;
    zone.type = DamageZoneType::LAVA;
    zone.center = center;
    zone.halfExtents = halfExtents;
    zone.damagePerSecond = 50.0f;
    zone.tickInterval = 0.25f;
    zone.slowFactor = 0.3f;
    return AddZone(zone);
}

int DamageZoneSystem::CreateAcidZone(const std::string& name, const XMFLOAT3& center,
                                      const XMFLOAT3& halfExtents)
{
    DamageZone zone;
    zone.name = name;
    zone.type = DamageZoneType::ACID;
    zone.center = center;
    zone.halfExtents = halfExtents;
    zone.damagePerSecond = 20.0f;
    zone.tickInterval = 0.5f;
    zone.slowFactor = 0.5f;
    return AddZone(zone);
}

int DamageZoneSystem::CreateElectricZone(const std::string& name, const XMFLOAT3& center,
                                          const XMFLOAT3& halfExtents)
{
    DamageZone zone;
    zone.name = name;
    zone.type = DamageZoneType::ELECTRIC;
    zone.center = center;
    zone.halfExtents = halfExtents;
    zone.damagePerSecond = 35.0f;
    zone.tickInterval = 1.0f; // Shocks every second
    zone.slowFactor = 0.7f;
    return AddZone(zone);
}

int DamageZoneSystem::CreateVoidZone(const std::string& name, const XMFLOAT3& center,
                                      const XMFLOAT3& halfExtents)
{
    DamageZone zone;
    zone.name = name;
    zone.type = DamageZoneType::VOID;
    zone.center = center;
    zone.halfExtents = halfExtents;
    zone.instantKill = true;
    return AddZone(zone);
}

std::string DamageZoneSystem::Console_ListZones() const
{
    std::stringstream ss;
    ss << "Damage Zones (" << m_zones.size() << "/" << MAX_DAMAGE_ZONES << "):
";

    const char* typeNames[] = { "Lava", "Acid", "Electric", "Radiation", "Void" };

    for (size_t i = 0; i < m_zones.size(); ++i) {
        const auto& z = m_zones[i];
        int typeIdx = static_cast<int>(z.type);
        const char* typeName = (typeIdx >= 0 && typeIdx <= 4) ? typeNames[typeIdx] : "Unknown";
        ss << "  [" << i << "] \"" << z.name << "\" Type:" << typeName
           << " DPS:" << z.damagePerSecond
           << " Center:(" << z.center.x << "," << z.center.y << "," << z.center.z << ")"
           << (z.instantKill ? " [INSTANT KILL]" : "")
           << "
";
    }
    return ss.str();
}

// ============================================================================
// RespawnSystem Implementation
// ============================================================================

RespawnSystem::RespawnSystem()
{
}

bool RespawnSystem::Initialize()
{
    m_spawnPoints.clear();
    m_killHistory.clear();
    m_respawnTimer = 0.0f;
    m_waitingForRespawn = false;
    m_deadPlayer = nullptr;

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
    if (m_waitingForRespawn) {
        m_respawnTimer -= deltaTime;
        if (m_respawnTimer <= 0.0f && m_autoRespawn && m_deadPlayer) {
            RespawnPlayer(m_deadPlayer);
        }
    }
}

int RespawnSystem::AddSpawnPoint(const RespawnPoint& point)
{
    if ((int)m_spawnPoints.size() >= MAX_SPAWN_POINTS) return -1;
    m_spawnPoints.push_back(point);
    return static_cast<int>(m_spawnPoints.size() - 1);
}

void RespawnSystem::RemoveSpawnPoint(int index)
{
    if (index >= 0 && index < (int)m_spawnPoints.size()) {
        m_spawnPoints.erase(m_spawnPoints.begin() + index);
    }
}

RespawnPoint RespawnSystem::GetBestSpawnPoint(int teamID) const
{
    const RespawnPoint* best = nullptr;
    int bestPriority = -999999;

    for (const auto& point : m_spawnPoints) {
        if (!point.isActive) continue;
        if (teamID >= 0 && point.teamID >= 0 && point.teamID != teamID) continue;
        if (point.priority > bestPriority) {
            bestPriority = point.priority;
            best = &point;
        }
    }

    if (best) return *best;

    // Default fallback
    RespawnPoint fallback;
    fallback.name = "Fallback";
    fallback.position = {0, 2, 0};
    return fallback;
}

void RespawnSystem::OnPlayerDeath(Player* player, const std::string& killerName,
                                   const std::string& weapon, bool headshot)
{
    if (!player) return;

    m_deadPlayer = player;
    m_waitingForRespawn = true;
    m_respawnTimer = m_respawnDelay;

    // Update score
    m_playerScore.deaths++;
    m_playerScore.currentStreak = 0;

    // Record kill
    RecordKill(killerName, "Player", weapon, headshot, 0.0f);
}

void RespawnSystem::RespawnPlayer(Player* player)
{
    if (!player) return;

    RespawnPoint spawn = GetBestSpawnPoint(-1);

    // Restore player
    player->Console_SetHealth(player->GetMaxHealth());
    player->Console_SetArmor(0);
    player->Console_SetPosition(spawn.position.x, spawn.position.y, spawn.position.z);
    player->SetActive(true);

    m_waitingForRespawn = false;
    m_deadPlayer = nullptr;
}

void RespawnSystem::RecordKill(const std::string& killer, const std::string& victim,
                                const std::string& weapon, bool headshot, float distance)
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
    while ((int)m_killHistory.size() > MAX_KILL_HISTORY) {
        m_killHistory.erase(m_killHistory.begin());
    }
}

std::string RespawnSystem::Console_GetScoreboard() const
{
    std::stringstream ss;
    ss << "=== SCOREBOARD ===
";
    ss << "Kills: " << m_playerScore.kills << "
";
    ss << "Deaths: " << m_playerScore.deaths << "
";
    ss << "K/D Ratio: " << m_playerScore.GetKDRatio() << "
";
    ss << "Current Streak: " << m_playerScore.currentStreak << "
";
    ss << "Longest Streak: " << m_playerScore.longestStreak << "
";
    ss << "Score: " << m_playerScore.score << "
";
    ss << "Vehicle Kills: " << m_playerScore.vehicleKills << "
";
    ss << "Damage Dealt: " << m_playerScore.totalDamageDealt << "
";
    ss << "Damage Received: " << m_playerScore.totalDamageReceived << "
";
    return ss.str();
}

std::string RespawnSystem::Console_GetKillHistory() const
{
    std::stringstream ss;
    ss << "=== KILL HISTORY (last " << m_killHistory.size() << ") ===
";
    for (const auto& kill : m_killHistory) {
        ss << kill.killerName << " -> " << kill.victimName
           << " [" << kill.weaponName << "]"
           << (kill.headshot ? " HEADSHOT" : "")
           << "
";
    }
    return ss.str();
}

} // namespace Spark
#endif // SPARK_PLATFORM_WINDOWS
