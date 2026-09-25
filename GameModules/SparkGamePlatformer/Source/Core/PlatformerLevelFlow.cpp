/**
 * @file PlatformerLevelFlow.cpp
 * @brief Per-frame platformer level orchestration shared by the module and its tests.
 */

#include "PlatformerLevelFlow.h"
#include "Checkpoint/PlatformerCheckpointSystem.h"
#include "Collectible/PlatformerCollectibleSystem.h"
#include "Hazard/PlatformerHazardSystem.h"
#include "Level/PlatformerLevelSystem.h"
#include "Player/PlatformerPlayerController.h"

#include <algorithm>
#include <cmath>

namespace Platformer
{
    PlatformerLevelFlow::PlatformerLevelFlow(PlatformerLevelSystem& level, PlatformerPlayerController& player,
                                             PlatformerCollectibleSystem& collectibles, PlatformerHazardSystem& hazards,
                                             PlatformerCheckpointSystem& checkpoints)
        : m_level(level), m_player(player), m_collectibles(collectibles), m_hazards(hazards), m_checkpoints(checkpoints)
    {
    }

    bool PlatformerLevelFlow::StartLevel(uint32_t index)
    {
        if (!m_level.LoadLevel(index))
            return false;

        const SpawnPoint spawn = m_level.GetCurrentSpawnPoint();
        m_checkpoints.ResetLevel(index);
        m_checkpoints.SetActiveLevel(index);
        m_checkpoints.SetLevelSpawn(spawn.x, spawn.y, spawn.z);
        m_collectibles.ResetLevel(index);
        m_player.Respawn();
        return true;
    }

    LevelFrameResult PlatformerLevelFlow::StepFrame(float deltaTime)
    {
        LevelFrameResult result{};
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return result;

        m_level.Update(deltaTime);
        m_player.Update(deltaTime);
        m_collectibles.Update(deltaTime);
        m_hazards.Update(deltaTime);

        const PlayerPosition position = m_player.GetPlayerPosition();
        for (const CollectedPickup& pickup :
             m_collectibles.CheckCollection(position.x, position.y, position.z, m_player.IsMagnetActive()))
        {
            ++result.pickups;
            switch (pickup.type)
            {
            case CollectibleType::AbilityOrb:
                m_player.UnlockAbility(pickup.abilityType);
                break;
            case CollectibleType::HealthPickup:
            case CollectibleType::ExtraLife:
                m_player.GrantLives(std::max(1, pickup.value));
                break;
            default:
                break;
            }
        }

        m_checkpoints.CheckActivation(position.x, position.y, position.z);

        float knockbackX = 0.0f;
        float knockbackY = 0.0f;
        const int damage = m_hazards.CheckHazardCollision(position.x, position.y, position.z, knockbackX, knockbackY);
        if (damage > 0 && m_player.TakeDamage(damage))
        {
            result.hazardDamage = damage;
            m_player.ApplyImpulse(knockbackX, knockbackY);
        }

        result.goalReached = m_level.TryCompleteAtPosition(position.x, position.y, position.z, 0);

        m_checkpoints.Update(deltaTime);
        return result;
    }

    void PlatformerLevelFlow::StepFixed(float fixedDeltaTime)
    {
        if (!std::isfinite(fixedDeltaTime) || fixedDeltaTime <= 0.0f)
            return;

        m_level.StepPlatforms(fixedDeltaTime);
        m_player.FixedUpdate(fixedDeltaTime);

        const PlayerPosition position = m_player.GetPlayerPosition();
        float windX = 0.0f;
        float windY = 0.0f;
        m_hazards.GetWindForce(position.x, position.y, position.z, windX, windY);
        m_player.ApplyImpulse(windX * fixedDeltaTime, windY * fixedDeltaTime);
    }
} // namespace Platformer
