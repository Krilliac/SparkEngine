/**
 * @file MultiplayerCombat.cpp
 * @brief FPSMultiplayerSystem server combat: lag-compensated hit validation, projectiles, damage reports
 */

#include "MultiplayerSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace SparkFPS
{

    namespace
    {
        // Player hitbox: an axis-aligned box standing on the player origin. The eye
        // (the projectile muzzle height in ApplyClientInput/SendInput) sits inside it.
        constexpr float kHitboxHalfWidth = 0.4f;
        constexpr float kHitboxHeight = 1.8f;
        constexpr float kEyeHeight = 1.0f;

        // The server's validation ray starts this far past the face where it leaves the
        // attacker's own hitbox, so the shooter never occludes the shot at any elevation.
        constexpr float kMuzzleClearance = 0.01f;

        // A client damage report names a victim; the attacker's last applied yaw must point
        // at that victim's hitbox within this extra slack, which covers the aim drift between
        // the input the server last applied and the shot.
        constexpr float kAimToleranceRadians = 0.1745f; // 10 degrees

        void HitboxBounds(const NetworkPlayerState& state, DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax)
        {
            outMin = {state.posX - kHitboxHalfWidth, state.posY, state.posZ - kHitboxHalfWidth};
            outMax = {state.posX + kHitboxHalfWidth, state.posY + kHitboxHeight, state.posZ + kHitboxHalfWidth};
        }

        // Distance along a unit @p dir from @p point (inside the box) to where the ray
        // leaves the box: the nearest far-side slab.
        float RayExitDistance(const std::array<float, 3>& point, const std::array<float, 3>& dir,
                              const DirectX::XMFLOAT3& boxMin, const DirectX::XMFLOAT3& boxMax)
        {
            const std::array<float, 3> lo{boxMin.x, boxMin.y, boxMin.z};
            const std::array<float, 3> hi{boxMax.x, boxMax.y, boxMax.z};
            float exit = std::numeric_limits<float>::max();
            for (size_t axis = 0; axis < 3; ++axis)
            {
                if (std::abs(dir[axis]) < 1e-8f)
                {
                    continue;
                }
                const float farFace = dir[axis] > 0.0f ? hi[axis] : lo[axis];
                exit = (std::min)(exit, (farFace - point[axis]) / dir[axis]);
            }
            return (std::max)(0.0f, exit);
        }

        // Swept test of the segment [from, to] against an AABB. A fast projectile moves
        // several metres per tick, so a point-in-radius test would tunnel through players.
        bool SegmentEntersBox(const std::array<float, 3>& from, const std::array<float, 3>& to,
                              const DirectX::XMFLOAT3& boxMin, const DirectX::XMFLOAT3& boxMax, float& outEntry)
        {
            const std::array<float, 3> lo{boxMin.x, boxMin.y, boxMin.z};
            const std::array<float, 3> hi{boxMax.x, boxMax.y, boxMax.z};
            float entry = 0.0f;
            float exit = 1.0f;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                const float delta = to[axis] - from[axis];
                if (std::abs(delta) < 1e-8f)
                {
                    if (from[axis] < lo[axis] || from[axis] > hi[axis])
                    {
                        return false;
                    }
                    continue;
                }

                float t1 = (lo[axis] - from[axis]) / delta;
                float t2 = (hi[axis] - from[axis]) / delta;
                if (t1 > t2)
                {
                    std::swap(t1, t2);
                }
                entry = (std::max)(entry, t1);
                exit = (std::min)(exit, t2);
                if (entry > exit)
                {
                    return false;
                }
            }
            outEntry = entry;
            return true;
        }
    } // namespace

    // ============================================================================
    // Combat
    // ============================================================================

    void FPSMultiplayerSystem::RecordLagCompensationHistory()
    {
        auto& network = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::HistorySnapshot snapshot;
        snapshot.timestamp = network.GetServerTime();
        snapshot.entities.reserve(m_playerStates.size());
        for (const auto& [id, state] : m_playerStates)
        {
            if (!state.isAlive)
            {
                continue;
            }

            Spark::Net::HistorySnapshot::EntityState entity{};
            entity.networkID = id;
            entity.position = {state.posX, state.posY, state.posZ};
            entity.rotation = {state.pitch, state.yaw, 0.0f};
            HitboxBounds(state, entity.boundsMin, entity.boundsMax);
            snapshot.entities.push_back(entity);
        }
        network.GetLagCompensator().RecordSnapshot(snapshot);
    }

    void FPSMultiplayerSystem::ValidateHit(uint32_t attackerId, uint32_t victimId, float damage)
    {
        // Damage arrives from projectiles and from client damage reports; a NaN or
        // non-positive amount would corrupt health or heal the victim.
        if (!std::isfinite(damage) || damage <= 0.0f)
        {
            return;
        }

        // The amount is server-owned: a report or client-fired projectile can deal at most
        // the one weapon's per-hit damage, never the figure the client chose.
        damage = (std::min)(damage, ProjectileData{}.damage);

        auto victimIt = m_playerStates.find(victimId);
        if (victimIt == m_playerStates.end() || !victimIt->second.isAlive)
        {
            return;
        }

        if (m_isServer)
        {
            // Only a present attacker can be credited; otherwise m_scores would grow a
            // default-constructed entry for an unknown id.
            auto attackerIt = m_playerStates.find(attackerId);
            if (attackerIt == m_playerStates.end() || attackerId == victimId)
            {
                return;
            }

            const NetworkPlayerState& attacker = attackerIt->second;
            const NetworkPlayerState& victim = victimIt->second;
            const float dx = victim.posX - attacker.posX;
            const float dy = victim.posY - attacker.posY;
            const float dz = victim.posZ - attacker.posZ;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!(distance > 1e-4f))
            {
                return;
            }

            // Eye-to-eye ray against the lag-compensated hitboxes. The hit counts only
            // when the first box along the ray is the victim's, so another player
            // standing in the line of fire blocks it.
            // The ray starts where it leaves the attacker's own hitbox, whatever the
            // elevation, so the shooter's box is never the first one hit.
            const std::array<float, 3> dir{dx / distance, dy / distance, dz / distance};
            const std::array<float, 3> eye{attacker.posX, attacker.posY + kEyeHeight, attacker.posZ};
            DirectX::XMFLOAT3 attackerMin;
            DirectX::XMFLOAT3 attackerMax;
            HitboxBounds(attacker, attackerMin, attackerMax);
            const float muzzle = RayExitDistance(eye, dir, attackerMin, attackerMax) + kMuzzleClearance;
            const DirectX::XMFLOAT3 rayDir(dir[0], dir[1], dir[2]);
            const DirectX::XMFLOAT3 rayOrigin(eye[0] + dir[0] * muzzle, eye[1] + dir[1] * muzzle,
                                              eye[2] + dir[2] * muzzle);
            auto& network = Spark::Net::NetworkManager::GetInstance();
            const float halfRTT = network.GetEstimatedRTT() * 0.0005f;
            const auto result = network.ValidateHit(network.GetServerTime(), halfRTT, rayOrigin, rayDir);
            if (!result.hit || result.entityID != victimId)
            {
                return;
            }
        }

        victimIt->second.health -= damage;

        if (victimIt->second.health <= 0.0f)
        {
            victimIt->second.health = 0.0f;
            victimIt->second.isAlive = false;

            // Update scores
            m_scores[attackerId].kills++;
            m_scores[attackerId].score += 100;
            m_scores[victimId].deaths++;

            // Start respawn timer
            m_respawnTimers[victimId] = m_respawnTime;
        }
    }

    void FPSMultiplayerSystem::OnProjectileFired(uint32_t clientId, const ProjectileData& proj)
    {
        if (!m_isServer)
        {
            return;
        }

        auto ownerIt = m_playerStates.find(clientId);
        if (ownerIt == m_playerStates.end() || !ownerIt->second.isAlive)
        {
            return;
        }

        ProjectileData serverProj = proj;
        serverProj.projectileId = (serverProj.projectileId == 0) ? m_nextProjectileId++ : serverProj.projectileId;
        serverProj.ownerId = clientId;
        serverProj.active = true;
        serverProj.positionX = serverProj.originX;
        serverProj.positionY = serverProj.originY;
        serverProj.positionZ = serverProj.originZ;
        serverProj.velocityX = serverProj.dirX * serverProj.speed;
        serverProj.velocityY = serverProj.dirY * serverProj.speed;
        serverProj.velocityZ = serverProj.dirZ * serverProj.speed;
        m_projectiles[serverProj.projectileId] = serverProj;
    }

    void FPSMultiplayerSystem::OnPlayerDamaged(uint32_t attackerId, uint32_t victimId, float damage)
    {
        if (!m_isServer)
        {
            return;
        }

        // A report is a client claim. Beyond ValidateHit's line-of-fire check, the attacker
        // must be alive and aiming at the victim: its last applied yaw must point into the
        // victim's hitbox (widened by kAimToleranceRadians).
        const auto attackerIt = m_playerStates.find(attackerId);
        const auto victimIt = m_playerStates.find(victimId);
        if (attackerIt == m_playerStates.end() || victimIt == m_playerStates.end() || !attackerIt->second.isAlive)
        {
            return;
        }

        const NetworkPlayerState& attacker = attackerIt->second;
        const NetworkPlayerState& victim = victimIt->second;
        const float dx = victim.posX - attacker.posX;
        const float dz = victim.posZ - attacker.posZ;
        const float planarDistance = std::sqrt(dx * dx + dz * dz);
        const float hitboxHalfDiagonal = kHitboxHalfWidth * std::numbers::sqrt2_v<float>;
        if (planarDistance > hitboxHalfDiagonal)
        {
            float aimError = std::atan2(dz, dx) - attacker.yaw;
            aimError = std::remainder(aimError, 2.0f * std::numbers::pi_v<float>);
            const float allowedError = std::atan(hitboxHalfDiagonal / planarDistance) + kAimToleranceRadians;
            if (!(std::abs(aimError) <= allowedError))
            {
                return;
            }
        }

        ValidateHit(attackerId, victimId, damage);
    }

    void FPSMultiplayerSystem::UpdateProjectiles(float dt)
    {
        if (!(dt >= 0.0f && std::isfinite(dt)))
        {
            return;
        }
        for (auto it = m_projectiles.begin(); it != m_projectiles.end();)
        {
            auto& projectile = it->second;
            const std::array<float, 3> from{projectile.positionX, projectile.positionY, projectile.positionZ};
            projectile.positionX += projectile.velocityX * dt;
            projectile.positionY += projectile.velocityY * dt;
            projectile.positionZ += projectile.velocityZ * dt;
            projectile.lifetime -= dt;
            const std::array<float, 3> to{projectile.positionX, projectile.positionY, projectile.positionZ};

            bool despawned = (projectile.lifetime <= 0.0f);
            if (!despawned && m_isServer)
            {
                // The nearest hitbox along this tick's path takes the hit.
                bool struck = false;
                uint32_t struckId = 0;
                float nearestEntry = 2.0f;
                for (const auto& [playerId, state] : m_playerStates)
                {
                    if (playerId == projectile.ownerId || !state.isAlive)
                    {
                        continue;
                    }

                    DirectX::XMFLOAT3 boxMin;
                    DirectX::XMFLOAT3 boxMax;
                    HitboxBounds(state, boxMin, boxMax);
                    float entry = 0.0f;
                    if (SegmentEntersBox(from, to, boxMin, boxMax, entry) && entry < nearestEntry)
                    {
                        nearestEntry = entry;
                        struckId = playerId;
                        struck = true;
                    }
                }

                if (struck)
                {
                    ValidateHit(projectile.ownerId, struckId, projectile.damage);
                    despawned = true;
                }
            }

            if (despawned)
            {
                it = m_projectiles.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace SparkFPS
