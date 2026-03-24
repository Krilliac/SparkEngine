/**
 * @file PlatformerHazardSystem.h
 * @brief Environmental hazards: spikes, lava, crushers, sawblades, lasers
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages all environmental hazards in platformer levels:
 * - Spikes (instant damage on contact)
 * - Lava (damage over time, kills on submersion)
 * - Crushers (moving platforms that squish the player)
 * - Sawblades (rotating hazards following a path)
 * - Projectile launchers (timed projectiles)
 * - Wind zones (push player in a direction)
 * - Laser beams (toggling on/off)
 * - Damage calculation with knockback
 * - Invincibility frame awareness
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/PlatformerEnums.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Platformer
{

    /// @brief A single hazard instance placed in a level
    struct HazardInstance
    {
        uint32_t id = 0;
        HazardType type{HazardType::Spikes};
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float width = 2.0f;
        float height = 1.0f;
        float depth = 2.0f;
        int damage = 1;
        bool active = true;

        // Crusher: travel range and speed
        float crusherEndY = 0.0f;
        float crusherSpeed = 5.0f;
        float crusherPauseTime = 1.0f;

        // Sawblade: path endpoints and speed
        float pathEndX = 0.0f;
        float pathEndY = 0.0f;
        float pathSpeed = 3.0f;
        float bladeRadius = 1.5f;

        // Projectile launcher: fire rate and projectile speed
        float fireInterval = 2.0f;
        float projectileSpeed = 10.0f;
        float fireDirX = 1.0f;
        float fireDirY = 0.0f;

        // Wind zone: push direction and strength
        float windDirX = 0.0f;
        float windDirY = 1.0f;
        float windStrength = 8.0f;

        // Laser beam: toggle timing
        float laserOnTime = 2.0f;
        float laserOffTime = 1.5f;

        // Knockback direction and force (applied to player on hit)
        float knockbackForce = 5.0f;
    };

    /// @brief Active projectile in flight
    struct ActiveProjectile
    {
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float velX = 0.0f;
        float velY = 0.0f;
        float lifetime = 0.0f;
        float maxLifetime = 5.0f;
        float radius = 0.3f;
        int damage = 1;
    };

    /**
     * @brief Manages environmental hazards and their interaction with the player
     *
     * Each hazard type has its own update logic (crushers cycle, sawblades patrol,
     * lasers toggle). Damage is applied through the player controller's TakeDamage
     * method, which handles invincibility frames and knockback.
     */
    class PlatformerHazardSystem
    {
      public:
        PlatformerHazardSystem() = default;
        ~PlatformerHazardSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Render();
        void Shutdown();

        void RenderDebugUI();

        /// @brief Total hazards placed in the current level
        size_t GetHazardCount() const { return m_hazards.size(); }

        /// @brief Check if a point is inside any active hazard's damage zone
        /// @return Damage amount (0 if no collision), knockback direction via out params
        int CheckHazardCollision(float x, float y, float z, float& knockbackX, float& knockbackY) const;

        /// @brief Get wind force at a given position (for wind zones)
        void GetWindForce(float x, float y, float z, float& forceX, float& forceY) const;

      private:
        void BuildDemoHazards();
        void UpdateCrushers(float deltaTime);
        void UpdateSawblades(float deltaTime);
        void UpdateProjectiles(float deltaTime);
        void UpdateLasers(float deltaTime);
        bool PointInBox(float px, float py, float pz, const HazardInstance& hazard) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<HazardInstance> m_hazards;
        std::vector<ActiveProjectile> m_projectiles;
        uint32_t m_nextId{1};
        float m_globalTimer{0.0f};
        bool m_initialized{false};
    };

} // namespace Platformer
