/**
 * @file GameMechanics.h
 * @brief Additional game mechanics for enhanced gameplay
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides damage zones (lava, acid, electric), a respawn system,
 * kill tracking, and an environmental hazard system for dynamic
 * level design and engaging gameplay.
 */

#pragma once
#include "../Core/Platform.h"

#include "../Enums/GameSystemEnums.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <string>
#include <functional>
#include <chrono>

using namespace DirectX;
using SparkEditor::DamageZoneType;

class Player;
class GameObject;

namespace Spark {

/**
 * @brief Environmental damage zone
 *
 * Defines an area that deals damage over time to players and vehicles.
 * Supports various hazard types with different damage patterns.
 */
struct DamageZone {
    std::string name;
    DamageZoneType type = DamageZoneType::LAVA;

    // Shape (axis-aligned box)
    XMFLOAT3 center = {0, 0, 0};
    XMFLOAT3 halfExtents = {5, 1, 5};

    // Damage parameters
    float damagePerSecond = 25.0f;
    float tickInterval = 0.5f;             ///< How often damage is applied
    bool instantKill = false;              ///< True = kill immediately on contact
    float slowFactor = 0.5f;               ///< Speed multiplier while in zone

    bool isActive = true;

    /**
     * @brief Check if a point is inside this zone
     */
    bool Contains(const XMFLOAT3& point) const;
};

/**
 * @brief Respawn point in the world
 */
struct RespawnPoint {
    std::string name;
    XMFLOAT3 position = {0, 2, 0};
    XMFLOAT3 rotation = {0, 0, 0};        ///< Camera look direction on spawn
    bool isActive = true;
    int teamID = -1;                       ///< -1 = any team, 0+ = specific team
    int priority = 0;                      ///< Higher = preferred spawn point
};

/**
 * @brief Kill/death tracking entry
 */
struct KillRecord {
    std::string killerName;
    std::string victimName;
    std::string weaponName;
    bool headshot = false;
    float distance = 0.0f;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief Player score tracking
 */
struct PlayerScore {
    int kills = 0;
    int deaths = 0;
    int assists = 0;
    int score = 0;
    int longestStreak = 0;
    int currentStreak = 0;
    float totalDamageDealt = 0.0f;
    float totalDamageReceived = 0.0f;
    float totalHealingDone = 0.0f;
    int vehicleKills = 0;
    int vehiclesDestroyed = 0;
    int objectivesCompleted = 0;

    float GetKDRatio() const { return deaths > 0 ? (float)kills / deaths : (float)kills; }
};

/**
 * @brief Damage zone and environmental hazard system
 */
class DamageZoneSystem {
public:
    DamageZoneSystem();
    ~DamageZoneSystem() = default;

    bool Initialize();
    void Update(float deltaTime, Player* player);

    int AddZone(const DamageZone& zone);
    void RemoveZone(int index);
    void RemoveZoneByName(const std::string& name);
    DamageZone* GetZone(int index);
    const std::vector<DamageZone>& GetZones() const { return m_zones; }
    void ClearZones();

    bool IsInDamageZone(const XMFLOAT3& position) const;
    DamageZoneType GetZoneTypeAt(const XMFLOAT3& position) const;

    // Preset creation
    int CreateLavaZone(const std::string& name, const XMFLOAT3& center,
                       const XMFLOAT3& halfExtents);
    int CreateAcidZone(const std::string& name, const XMFLOAT3& center,
                       const XMFLOAT3& halfExtents);
    int CreateElectricZone(const std::string& name, const XMFLOAT3& center,
                           const XMFLOAT3& halfExtents);
    int CreateVoidZone(const std::string& name, const XMFLOAT3& center,
                       const XMFLOAT3& halfExtents);

    // Console integration
    std::string Console_ListZones() const;

private:
    std::vector<DamageZone> m_zones;
    float m_damageTickTimer = 0.0f;
    static constexpr int MAX_DAMAGE_ZONES = 32;
};

/**
 * @brief Respawn system for player death and revival
 */
class RespawnSystem {
public:
    RespawnSystem();
    ~RespawnSystem() = default;

    bool Initialize();
    void Update(float deltaTime);

    // === Spawn Points ===

    int AddSpawnPoint(const RespawnPoint& point);
    void RemoveSpawnPoint(int index);
    const std::vector<RespawnPoint>& GetSpawnPoints() const { return m_spawnPoints; }

    /**
     * @brief Get the best spawn point for respawn
     * @param teamID Team to spawn for (-1 = any)
     * @return Best spawn point, or default if none
     */
    RespawnPoint GetBestSpawnPoint(int teamID = -1) const;

    // === Respawn Logic ===

    /**
     * @brief Handle player death
     */
    void OnPlayerDeath(Player* player, const std::string& killerName,
                       const std::string& weapon, bool headshot);

    /**
     * @brief Respawn the player
     */
    void RespawnPlayer(Player* player);

    /**
     * @brief Check if respawn is ready
     */
    bool IsRespawnReady() const { return m_respawnTimer <= 0.0f; }

    /**
     * @brief Get remaining respawn time
     */
    float GetRespawnTimeRemaining() const { return m_respawnTimer; }

    // === Settings ===

    void SetRespawnDelay(float delay) { m_respawnDelay = delay; }
    float GetRespawnDelay() const { return m_respawnDelay; }
    void SetAutoRespawn(bool enabled) { m_autoRespawn = enabled; }

    // === Score Tracking ===

    PlayerScore& GetPlayerScore() { return m_playerScore; }
    const PlayerScore& GetPlayerScore() const { return m_playerScore; }
    const std::vector<KillRecord>& GetKillHistory() const { return m_killHistory; }

    /**
     * @brief Register a kill for scoring
     */
    void RecordKill(const std::string& killer, const std::string& victim,
                    const std::string& weapon, bool headshot, float distance);

    // Console integration
    std::string Console_GetScoreboard() const;
    std::string Console_GetKillHistory() const;

private:
    std::vector<RespawnPoint> m_spawnPoints;
    float m_respawnDelay = 5.0f;
    float m_respawnTimer = 0.0f;
    bool m_autoRespawn = true;
    bool m_waitingForRespawn = false;
    Player* m_deadPlayer = nullptr;

    PlayerScore m_playerScore;
    std::vector<KillRecord> m_killHistory;

    static constexpr int MAX_SPAWN_POINTS = 32;
    static constexpr int MAX_KILL_HISTORY = 100;
};

} // namespace Spark
