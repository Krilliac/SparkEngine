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
#include "Engine/ECS/Components.h"

#include <algorithm>
#include <cmath>

namespace Platformer
{
    namespace
    {
        // Blender platformer kit (tools/blender/author_platformer_kit.py): metres, pivot at the ground-contact
        // centre, front facing +Z. floating_platform is a 3 x 1 x 3 m island whose turf top is y = 1, and
        // spring_pad is 1 m across with its launch cap at y = 0.58.
        constexpr float PlatformMeshFootprint = 3.0f;
        constexpr float PlatformMeshHeight = 1.0f;
        constexpr float SpringPadCapHeight = 0.58f;
        constexpr float GoalFlagBackInset = 0.6f; ///< Flag stands this far inside the goal platform's back edge

        ::World* EngineWorld(const PlatformerLevelSystem& level)
        {
            Spark::IEngineContext* context = level.GetContext();
            return context ? context->GetWorld() : nullptr;
        }

        /// Kit transform for one platform collider: the mesh's walkable top sits on the collider's top surface.
        Transform PlatformKitTransform(const PlatformCollider& collider)
        {
            const float centreX = (collider.minX + collider.maxX) * 0.5f;
            const float centreZ = (collider.minZ + collider.maxZ) * 0.5f;
            const float width = collider.maxX - collider.minX;
            const float depth = collider.maxZ - collider.minZ;
            if (collider.type == PlatformType::Bouncy)
            {
                // A Bouncy platform is one big spring pad (uniformly scaled to the pad's footprint).
                const float scale = std::min(width, depth);
                return Transform{{centreX, collider.maxY - SpringPadCapHeight * scale, centreZ},
                                 {0.0f, 0.0f, 0.0f},
                                 {scale, scale, scale}};
            }
            return Transform{{centreX, collider.maxY - PlatformMeshHeight, centreZ},
                             {0.0f, 0.0f, 0.0f},
                             {width / PlatformMeshFootprint, 1.0f, depth / PlatformMeshFootprint}};
        }
    } // namespace

    PlatformerLevelFlow::PlatformerLevelFlow(PlatformerLevelSystem& level, PlatformerPlayerController& player,
                                             PlatformerCollectibleSystem& collectibles, PlatformerHazardSystem& hazards,
                                             PlatformerCheckpointSystem& checkpoints)
        : m_level(level), m_player(player), m_collectibles(collectibles), m_hazards(hazards), m_checkpoints(checkpoints)
    {
    }

    PlatformerLevelFlow::~PlatformerLevelFlow()
    {
        RemoveLevelKit();
    }

    bool PlatformerLevelFlow::StartLevel(uint32_t index)
    {
        if (!m_level.LoadLevel(index))
            return false;

        PlaceLevelKit();

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
        SyncLevelKit();
        m_player.FixedUpdate(fixedDeltaTime);

        const PlayerPosition position = m_player.GetPlayerPosition();
        float windX = 0.0f;
        float windY = 0.0f;
        m_hazards.GetWindForce(position.x, position.y, position.z, windX, windY);
        m_player.ApplyImpulse(windX * fixedDeltaTime, windY * fixedDeltaTime);
    }

    void PlatformerLevelFlow::PlaceLevelKit()
    {
        RemoveLevelKit();
        ::World* world = EngineWorld(m_level);
        if (!world)
            return; // No engine world (headless tests): the level plays without meshes.

        m_kitWorld = world;
        const std::vector<PlatformCollider>& colliders = m_level.GetActiveColliders();
        for (const PlatformCollider& collider : colliders)
        {
            const bool springPad = collider.type == PlatformType::Bouncy;
            EntityID entity = world->CreateEntity(springPad ? "Platformer_SpringPad" : "Platformer_Platform");
            world->AddComponent<Transform>(entity, PlatformKitTransform(collider));
            world->AddComponent<MeshRenderer>(entity).meshPath =
                springPad ? "Assets/Models/Platformer/Kit/spring_pad.obj"
                          : "Assets/Models/Platformer/Kit/floating_platform.obj";
            m_kitEntities.push_back(static_cast<uint32_t>(entity));
        }

        // Every level's last platform is its goal platform, centred on the goal point
        // (PlatformerLevelSystem::Build*Level); the flag stands near its back edge, turned to face the camera
        // (which follows from -Z) with the pennant flying on toward +X.
        if (!colliders.empty())
        {
            const PlatformCollider& goal = colliders.back();
            EntityID entity = world->CreateEntity("Platformer_GoalFlag");
            world->AddComponent<Transform>(
                entity, Transform{{(goal.minX + goal.maxX) * 0.5f, goal.maxY, goal.maxZ - GoalFlagBackInset},
                                  {0.0f, 180.0f, 0.0f},
                                  {1.0f, 1.0f, 1.0f}});
            world->AddComponent<MeshRenderer>(entity).meshPath = "Assets/Models/Platformer/Kit/goal_flag.obj";
            m_kitEntities.push_back(static_cast<uint32_t>(entity));
        }
    }

    void PlatformerLevelFlow::SyncLevelKit()
    {
        if (m_kitEntities.empty() || EngineWorld(m_level) != m_kitWorld)
            return;

        // Moving, falling and disappearing platforms change their colliders every step; the meshes follow them.
        const std::vector<PlatformCollider>& colliders = m_level.GetActiveColliders();
        const size_t count = std::min(colliders.size(), m_kitEntities.size());
        for (size_t index = 0; index < count; ++index)
        {
            const auto entity = static_cast<EntityID>(m_kitEntities[index]);
            if (!m_kitWorld->GetRegistry().valid(entity))
                continue;
            auto* transform = m_kitWorld->GetComponent<Transform>(entity);
            auto* renderer = m_kitWorld->GetComponent<MeshRenderer>(entity);
            if (!transform || !renderer)
                continue;
            transform->position = PlatformKitTransform(colliders[index]).position;
            renderer->visible = colliders[index].solid;
            renderer->worldMatrixDirty = true;
        }
    }

    void PlatformerLevelFlow::RemoveLevelKit()
    {
        // Only touch the world the entities were created in, and only while the engine still exposes it.
        if (m_kitWorld && EngineWorld(m_level) == m_kitWorld)
        {
            for (uint32_t entityId : m_kitEntities)
            {
                const auto entity = static_cast<EntityID>(entityId);
                if (m_kitWorld->GetRegistry().valid(entity))
                    m_kitWorld->DestroyEntity(entity);
            }
        }
        m_kitEntities.clear();
        m_kitWorld = nullptr;
    }
} // namespace Platformer
