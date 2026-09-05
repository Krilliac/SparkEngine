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
#include "Core/Platform.h"

#include "Enums/GameSystemEnums.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <string>
#include <functional>
#include <chrono>

using SparkEditor::DamageZoneType;

class Player;
class GameObject;

namespace Spark
{
    class EventBus;

    /**
 * @brief Environmental damage zone
 *
 * Defines an area that deals damage over time to players and vehicles.
 * Supports various hazard types with different damage patterns.
 */
    struct DamageZone
    {
        std::string name;                           ///< Human-readable zone name (for editor and kill feed).
        DamageZoneType type = DamageZoneType::LAVA; ///< Hazard type (affects VFX, SFX, and damage behavior).

        // Shape (axis-aligned box)
        XMFLOAT3 center = {0, 0, 0};      ///< World-space center of the damage volume.
        XMFLOAT3 halfExtents = {5, 1, 5}; ///< Half-dimensions of the AABB (meters).

        // Damage parameters
        float damagePerSecond = 25.0f; ///< Damage dealt per second while inside the zone.
        float tickInterval = 0.5f;     ///< How often damage is applied (seconds between ticks).
        bool instantKill = false;      ///< True = kill immediately on contact (e.g. death pit).
        float slowFactor = 0.5f;       ///< Speed multiplier while in zone (1.0 = no slowdown).

        bool isActive = true; ///< Runtime toggle (false = zone is dormant).

        /**
     * @brief Check if a point is inside this zone
     */
        bool Contains(const XMFLOAT3& point) const;
    };

    /**
 * @brief Respawn point in the world
 */
    struct RespawnPoint
    {
        std::string name;              ///< Spawn point name (for editor labeling and selection).
        XMFLOAT3 position = {0, 2, 0}; ///< World-space spawn position.
        XMFLOAT3 rotation = {0, 0, 0}; ///< Initial camera euler angles (pitch, yaw, roll) on spawn.
        bool isActive = true;          ///< Whether this spawn point is currently usable.
        int teamID = -1;               ///< Team restriction: -1 = any team, 0+ = specific team only.
        int priority = 0;              ///< Selection weight: higher = preferred by the spawn algorithm.
    };

    /**
 * @brief Kill/death tracking entry
 */
    struct KillRecord
    {
        std::string killerName; ///< Display name of the player who got the kill.
        std::string victimName; ///< Display name of the player who was killed.
        std::string weaponName; ///< Weapon or cause of death (e.g. "AK-47", "Fall Damage").
        bool headshot = false;  ///< Whether this was a headshot kill (for bonus scoring).
        float distance = 0.0f;  ///< Distance between killer and victim at time of kill (meters).
        std::chrono::steady_clock::time_point timestamp; ///< When the kill occurred (for kill feed expiry).
    };

    /**
 * @brief Player score tracking
 */
    struct KillTrackerScore
    {
        int kills = 0;                    ///< Total kills this match.
        int deaths = 0;                   ///< Total deaths this match.
        int assists = 0;                  ///< Damage-assist kills (dealt damage but didn't finish).
        int score = 0;                    ///< Composite score (kills, objectives, bonuses).
        int longestStreak = 0;            ///< Best killstreak achieved this match.
        int currentStreak = 0;            ///< Active killstreak (reset on death).
        float totalDamageDealt = 0.0f;    ///< Cumulative damage dealt to other players.
        float totalDamageReceived = 0.0f; ///< Cumulative damage taken from all sources.
        float totalHealingDone = 0.0f;    ///< Cumulative healing applied to self or teammates.
        int vehicleKills = 0;             ///< Kills scored while in a vehicle.
        int vehiclesDestroyed = 0;        ///< Enemy vehicles destroyed.
        int objectivesCompleted = 0;      ///< Objective captures, flag returns, etc.

        float GetKDRatio() const { return deaths > 0 ? (float)kills / deaths : (float)kills; }
    };

    /**
 * @brief Damage zone and environmental hazard system
 */
    class DamageZoneSystem
    {
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
        int CreateLavaZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents);
        int CreateAcidZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents);
        int CreateElectricZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents);
        int CreateVoidZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents);

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
    class RespawnSystem
    {
      public:
        RespawnSystem();
        ~RespawnSystem() = default;

        bool Initialize();
        void Update(float deltaTime);

        /**
     * @brief Connect the engine event bus so respawns publish PlayerRespawnEvent.
     *
     * Without a bus the system still scores and times respawns, but nothing can
     * observe them, so callers should wire this during initialization.
     */
        void SetEventBus(EventBus* bus) { m_eventBus = bus; }

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
     * @brief Record a player death: scores it and arms the respawn timer.
     *
     * The player object itself is not touched here. Reactivating the player is
     * driven by the PlayerRespawnEvent published from RespawnPlayer(), which
     * keeps this system independent of the renderer-owned Player class.
     */
        void OnPlayerDeath(const std::string& killerName, const std::string& weapon, bool headshot);

        /**
     * @brief Arm the respawn countdown without scoring a death.
     *
     * Used when a session is restored from a save taken while the player was dead:
     * the death was scored when it happened, so OnPlayerDeath() would double-count it,
     * but the countdown still has to be armed or the restored player can never revive.
     */
        void ArmRespawn();

        /**
     * @brief Respawn now: picks a spawn point and publishes PlayerRespawnEvent.
     * @return false when no death is pending (nothing to respawn from) or when no
     *         event bus is attached. Publishing the event is the only thing that
     *         revives the player, so without a bus the pending death is deliberately
     *         kept: reporting success would strand the player dead forever.
     */
        bool RespawnPlayer();

        /** @brief True while a death has been recorded and no respawn has happened yet. */
        bool IsWaitingForRespawn() const { return m_waitingForRespawn; }

        /**
     * @brief Check if respawn is ready
     */
        bool IsRespawnReady() const { return m_respawnTimer <= 0.0f; }

        /**
     * @brief Get remaining respawn time (clamped at zero, never negative)
     */
        float GetRespawnTimeRemaining() const { return m_respawnTimer; }

        // === Settings ===

        void SetRespawnDelay(float delay) { m_respawnDelay = delay; }
        float GetRespawnDelay() const { return m_respawnDelay; }
        void SetAutoRespawn(bool enabled) { m_autoRespawn = enabled; }

        // === Score Tracking ===

        KillTrackerScore& GetPlayerScore() { return m_playerScore; }
        const KillTrackerScore& GetPlayerScore() const { return m_playerScore; }
        const std::vector<KillRecord>& GetKillHistory() const { return m_killHistory; }

        /**
     * @brief Register a kill for scoring
     */
        void RecordKill(const std::string& killer, const std::string& victim, const std::string& weapon, bool headshot,
                        float distance);

        // Console integration
        std::string Console_GetScoreboard() const;
        std::string Console_GetKillHistory() const;

      private:
        std::vector<RespawnPoint> m_spawnPoints;
        float m_respawnDelay = 5.0f;
        float m_respawnTimer = 0.0f;
        bool m_autoRespawn = true;
        bool m_waitingForRespawn = false;
        bool m_missingBusReported = false; ///< One-shot guard for the no-event-bus warning.
        EventBus* m_eventBus = nullptr;    ///< Engine event bus (not owned).

        KillTrackerScore m_playerScore;
        std::vector<KillRecord> m_killHistory;

        static constexpr int MAX_SPAWN_POINTS = 32;
        static constexpr int MAX_KILL_HISTORY = 100;
    };

} // namespace Spark
