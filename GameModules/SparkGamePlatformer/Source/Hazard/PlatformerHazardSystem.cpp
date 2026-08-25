/**
 * @file PlatformerHazardSystem.cpp
 * @brief Environmental hazard logic: crushers, sawblades, projectiles, lasers
 */

#include "PlatformerHazardSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace Platformer
{

    bool PlatformerHazardSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        BuildDemoHazards();

        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Platformer hazard system initialized with %zu hazards",
                       m_hazards.size());
        console.LogInfo("[Platformer Hazard] System initialized with " + std::to_string(m_hazards.size()) + " hazards");
        return true;
    }

    void PlatformerHazardSystem::BuildDemoHazards()
    {
        // Spike pit beneath the stepping stones (Level 0)
        HazardInstance spikes{};
        spikes.id = m_nextId++;
        spikes.type = HazardType::Spikes;
        spikes.posX = 14.0f;
        spikes.posY = -1.0f;
        spikes.posZ = 0.0f;
        spikes.width = 20.0f;
        spikes.height = 1.0f;
        spikes.depth = 4.0f;
        spikes.damage = 1;
        spikes.knockbackForce = 6.0f;
        m_hazards.push_back(spikes);

        // Sawblade patrolling a platform edge
        HazardInstance sawblade{};
        sawblade.id = m_nextId++;
        sawblade.type = HazardType::Sawblade;
        sawblade.posX = 55.0f;
        sawblade.posY = 13.0f;
        sawblade.posZ = 0.0f;
        sawblade.pathEndX = 70.0f;
        sawblade.pathEndY = 13.0f;
        sawblade.pathSpeed = 4.0f;
        sawblade.bladeRadius = 1.5f;
        sawblade.damage = 1;
        sawblade.knockbackForce = 8.0f;
        sawblade.originX = sawblade.posX;
        sawblade.originY = sawblade.posY;
        m_hazards.push_back(sawblade);

        // Crusher in the cave section
        HazardInstance crusher{};
        crusher.id = m_nextId++;
        crusher.type = HazardType::Crusher;
        crusher.posX = 80.0f;
        crusher.posY = 15.0f;
        crusher.posZ = 0.0f;
        crusher.width = 4.0f;
        crusher.height = 3.0f;
        crusher.depth = 4.0f;
        crusher.crusherEndY = 6.0f;
        crusher.crusherSpeed = 8.0f;
        crusher.crusherPauseTime = 1.5f;
        crusher.damage = 1;
        crusher.originX = crusher.posX;
        crusher.originY = crusher.posY;
        m_hazards.push_back(crusher);

        // Projectile launcher guarding a gem
        HazardInstance launcher{};
        launcher.id = m_nextId++;
        launcher.type = HazardType::Projectile;
        launcher.posX = 45.0f;
        launcher.posY = 5.0f;
        launcher.posZ = 0.0f;
        launcher.fireInterval = 2.5f;
        launcher.projectileSpeed = 8.0f;
        launcher.fireDirX = 1.0f;
        launcher.fireDirY = 0.0f;
        launcher.damage = 1;
        m_hazards.push_back(launcher);

        // Wind zone (updraft) to help reach a high platform
        HazardInstance wind{};
        wind.id = m_nextId++;
        wind.type = HazardType::WindZone;
        wind.posX = 38.0f;
        wind.posY = 0.0f;
        wind.posZ = 0.0f;
        wind.width = 4.0f;
        wind.height = 20.0f;
        wind.depth = 4.0f;
        wind.windDirX = 0.0f;
        wind.windDirY = 1.0f;
        wind.windStrength = 12.0f;
        wind.damage = 0; // Wind doesn't deal damage
        m_hazards.push_back(wind);

        // Laser beam toggling on/off
        HazardInstance laser{};
        laser.id = m_nextId++;
        laser.type = HazardType::LaserBeam;
        laser.posX = 60.0f;
        laser.posY = 8.0f;
        laser.posZ = 0.0f;
        laser.width = 0.3f;
        laser.height = 6.0f;
        laser.depth = 0.3f;
        laser.laserOnTime = 2.0f;
        laser.laserOffTime = 1.5f;
        laser.damage = 1;
        laser.knockbackForce = 4.0f;
        m_hazards.push_back(laser);

        // Lava pool beneath the desert level gap
        HazardInstance lava{};
        lava.id = m_nextId++;
        lava.type = HazardType::Lava;
        lava.posX = 30.0f;
        lava.posY = -2.0f;
        lava.posZ = 0.0f;
        lava.width = 30.0f;
        lava.height = 2.0f;
        lava.depth = 6.0f;
        lava.damage = 2; // Lava deals more damage
        lava.knockbackForce = 10.0f;
        m_hazards.push_back(lava);
    }

    void PlatformerHazardSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        m_globalTimer += deltaTime;

        UpdateCrushers(deltaTime);
        UpdateSawblades(deltaTime);
        UpdateProjectiles(deltaTime);
        UpdateLasers(deltaTime);
    }

    void PlatformerHazardSystem::UpdateCrushers(float deltaTime)
    {
        for (auto& hazard : m_hazards)
        {
            if (hazard.type != HazardType::Crusher)
                continue;

            if (hazard.crusherSpeed <= 0.0f)
                continue;

            // Simple oscillation between start Y and crusherEndY
            float cycleTime = std::abs(hazard.originY - hazard.crusherEndY) / hazard.crusherSpeed;
            if (cycleTime <= 0.0f)
                continue;

            float totalCycle = cycleTime * 2.0f + hazard.crusherPauseTime * 2.0f;
            if (totalCycle <= 0.0f)
                continue;

            float t = std::fmod(m_globalTimer, totalCycle);

            const float startY = hazard.originY;
            if (t < cycleTime)
            {
                // Moving down
                float progress = t / cycleTime;
                hazard.posY = startY + (hazard.crusherEndY - startY) * progress;
            }
            else if (t < cycleTime + hazard.crusherPauseTime)
            {
                // Paused at bottom
                hazard.posY = hazard.crusherEndY;
            }
            else if (t < cycleTime * 2.0f + hazard.crusherPauseTime)
            {
                // Moving up
                float progress = (t - cycleTime - hazard.crusherPauseTime) / cycleTime;
                hazard.posY = hazard.crusherEndY + (startY - hazard.crusherEndY) * progress;
            }
            // else: paused at top

            (void)deltaTime;
        }
    }

    void PlatformerHazardSystem::UpdateSawblades(float deltaTime)
    {
        (void)deltaTime;

        for (auto& hazard : m_hazards)
        {
            if (hazard.type != HazardType::Sawblade)
                continue;

            // Ping-pong between start and end positions
            const float deltaX = hazard.pathEndX - hazard.originX;
            const float deltaY = hazard.pathEndY - hazard.originY;
            float pathLength = std::sqrt(deltaX * deltaX + deltaY * deltaY);

            if (pathLength < 0.01f || hazard.pathSpeed <= 0.0f)
                continue;

            float travelTime = pathLength / hazard.pathSpeed;
            float t = std::fmod(m_globalTimer, travelTime * 2.0f) / travelTime;

            // Ping-pong: 0->1 forward, 1->2 backward
            float progress = (t <= 1.0f) ? t : (2.0f - t);

            hazard.posX = hazard.originX + deltaX * progress;
            hazard.posY = hazard.originY + deltaY * progress;
        }
    }

    void PlatformerHazardSystem::UpdateProjectiles(float deltaTime)
    {
        // Spawn projectiles from launchers
        for (const auto& hazard : m_hazards)
        {
            if (hazard.type != HazardType::Projectile)
                continue;

            // Fire at regular intervals
            float timeSinceLastFire = std::fmod(m_globalTimer, hazard.fireInterval);
            if (timeSinceLastFire < deltaTime)
            {
                ActiveProjectile proj{};
                proj.posX = hazard.posX;
                proj.posY = hazard.posY;
                proj.posZ = hazard.posZ;
                proj.velX = hazard.fireDirX * hazard.projectileSpeed;
                proj.velY = hazard.fireDirY * hazard.projectileSpeed;
                proj.damage = hazard.damage;
                m_projectiles.push_back(proj);
                SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Platformer projectile fired from (%.0f, %.0f)", hazard.posX,
                                hazard.posY);
            }
        }

        // Update active projectiles
        for (auto& proj : m_projectiles)
        {
            proj.posX += proj.velX * deltaTime;
            proj.posY += proj.velY * deltaTime;
            proj.lifetime += deltaTime;
        }

        // Remove expired projectiles
        m_projectiles.erase(std::remove_if(m_projectiles.begin(), m_projectiles.end(),
                                           [](const ActiveProjectile& p) { return p.lifetime >= p.maxLifetime; }),
                            m_projectiles.end());
    }

    void PlatformerHazardSystem::UpdateLasers(float deltaTime)
    {
        (void)deltaTime;

        for (auto& hazard : m_hazards)
        {
            if (hazard.type != HazardType::LaserBeam)
                continue;

            float cycleTime = hazard.laserOnTime + hazard.laserOffTime;
            float t = std::fmod(m_globalTimer, cycleTime);
            bool wasActive = hazard.active;
            hazard.active = (t < hazard.laserOnTime);
            if (hazard.active != wasActive)
                SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Platformer laser %u toggled %s", hazard.id,
                                hazard.active ? "ON" : "OFF");
        }
    }

    bool PlatformerHazardSystem::PointInBox(float px, float py, float pz, const HazardInstance& hazard) const
    {
        float halfW = hazard.width * 0.5f;
        float halfH = hazard.height * 0.5f;
        float halfD = hazard.depth * 0.5f;

        return (px >= hazard.posX - halfW && px <= hazard.posX + halfW && py >= hazard.posY - halfH &&
                py <= hazard.posY + halfH && pz >= hazard.posZ - halfD && pz <= hazard.posZ + halfD);
    }

    int PlatformerHazardSystem::CheckHazardCollision(float x, float y, float z, float& knockbackX,
                                                     float& knockbackY) const
    {
        knockbackX = 0.0f;
        knockbackY = 0.0f;

        for (const auto& hazard : m_hazards)
        {
            if (!hazard.active || hazard.damage <= 0)
                continue;

            // Wind zones don't deal damage
            if (hazard.type == HazardType::WindZone)
                continue;

            // Sawblades use radius check
            if (hazard.type == HazardType::Sawblade)
            {
                float dx = x - hazard.posX;
                float dy = y - hazard.posY;
                float distSq = dx * dx + dy * dy;
                if (distSq <= hazard.bladeRadius * hazard.bladeRadius)
                {
                    float dist = std::sqrt(distSq);
                    if (dist > 0.01f)
                    {
                        knockbackX = (dx / dist) * hazard.knockbackForce;
                        knockbackY = (dy / dist) * hazard.knockbackForce;
                    }
                    else
                    {
                        knockbackY = hazard.knockbackForce;
                    }
                    return hazard.damage;
                }
                continue;
            }

            // Box-based collision for other hazard types
            if (PointInBox(x, y, z, hazard))
            {
                // Knockback direction: away from hazard center
                float dx = x - hazard.posX;
                float dy = y - hazard.posY;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > 0.01f)
                {
                    knockbackX = (dx / dist) * hazard.knockbackForce;
                    knockbackY = (dy / dist) * hazard.knockbackForce;
                }
                else
                {
                    knockbackY = hazard.knockbackForce;
                }
                return hazard.damage;
            }
        }

        // Check projectile collisions
        for (const auto& proj : m_projectiles)
        {
            float dx = x - proj.posX;
            float dy = y - proj.posY;
            float distSq = dx * dx + dy * dy;
            if (distSq <= proj.radius * proj.radius)
            {
                float dist = std::sqrt(distSq);
                if (dist > 0.01f)
                {
                    knockbackX = (dx / dist) * 5.0f;
                    knockbackY = (dy / dist) * 5.0f;
                }
                return proj.damage;
            }
        }

        return 0;
    }

    void PlatformerHazardSystem::GetWindForce(float x, float y, float z, float& forceX, float& forceY) const
    {
        forceX = 0.0f;
        forceY = 0.0f;

        for (const auto& hazard : m_hazards)
        {
            if (hazard.type != HazardType::WindZone || !hazard.active)
                continue;

            if (PointInBox(x, y, z, hazard))
            {
                forceX += hazard.windDirX * hazard.windStrength;
                forceY += hazard.windDirY * hazard.windStrength;
            }
        }
    }

    void PlatformerHazardSystem::Render()
    {
        if (!m_initialized)
            return;

        // In a full implementation, this would render:
        // - Spike meshes with sharp geometry
        // - Lava with animated scrolling texture and glow
        // - Crusher block with chains/pistons
        // - Sawblade spinning mesh on its patrol path
        // - Projectile meshes in flight with trails
        // - Wind zone particle effects (rising dust/leaves)
        // - Laser beam with glow shader (hidden when inactive)
    }

    void PlatformerHazardSystem::Shutdown()
    {
        m_hazards.clear();
        m_projectiles.clear();
        m_initialized = false;
    }

    void PlatformerHazardSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Platformer Hazards"))
            return;

        ImGui::Text("Hazards: %zu", m_hazards.size());
        ImGui::Text("Active Projectiles: %zu", m_projectiles.size());
        ImGui::Separator();

        for (const auto& hazard : m_hazards)
        {
            ImGui::PushID(static_cast<int>(hazard.id));
            const char* typeName = "Unknown";
            switch (hazard.type)
            {
            case HazardType::Spikes:
                typeName = "Spikes";
                break;
            case HazardType::Lava:
                typeName = "Lava";
                break;
            case HazardType::Crusher:
                typeName = "Crusher";
                break;
            case HazardType::Sawblade:
                typeName = "Sawblade";
                break;
            case HazardType::Projectile:
                typeName = "Projectile";
                break;
            case HazardType::WindZone:
                typeName = "WindZone";
                break;
            case HazardType::LaserBeam:
                typeName = "LaserBeam";
                break;
            default:
                break;
            }

            if (ImGui::TreeNode(typeName))
            {
                ImGui::Text("Position: (%.1f, %.1f, %.1f)", hazard.posX, hazard.posY, hazard.posZ);
                ImGui::Text("Active: %s", hazard.active ? "Yes" : "No");
                ImGui::Text("Damage: %d", hazard.damage);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
#endif
    }

} // namespace Platformer
