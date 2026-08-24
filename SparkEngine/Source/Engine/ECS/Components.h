/**
 * @file Components.h
 * @brief ECS components and World class for Spark Engine (umbrella header)
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This is the umbrella header that includes all ECS component headers.
 * For faster compilation, prefer including only the specific component
 * headers you need:
 *
 *   #include "Components/CoreComponents.h"       // Transform, Camera, etc.
 *   #include "Components/PhysicsComponents.h"     // RigidBody, Collider
 *   #include "Components/AudioComponents.h"       // AudioSource
 *   #include "Components/LightComponents.h"       // LightComponent
 *   #include "Components/AnimationComponents.h"   // Animation, Particles
 *   #include "Components/AIComponents.h"          // AI, NetworkIdentity
 *   #include "Components/GameplayComponents.h"    // Tags, Health, Weather, etc.
 *   #include "Components/FPSComponents.h"         // Decals, Projectiles, Interactions
 *
 * ## Component categories
 *
 * | Category       | Components                                               |
 * |----------------|----------------------------------------------------------|
 * | Core           | NameComponent, Transform, MeshRenderer, Camera, Script  |
 * | Physics        | RigidBodyComponent, ColliderComponent                    |
 * | Audio          | AudioSourceComponent                                     |
 * | Lighting       | LightComponent                                           |
 * | Particles      | ParticleEmitterComponent                                 |
 * | Animation      | AnimationController                                      |
 * | AI             | AIComponent                                              |
 * | Networking     | NetworkIdentity                                          |
 * | Tags/Metadata  | TagComponent, ActiveComponent, HealthComponent           |
 * | FPS            | DecalComponent, ProjectileComponent, InteractionComponent|
 */

#pragma once

#include "../../Utils/Assert.h"
#include "../../Utils/Validate.h"
#include "../../Utils/EntityEventBus.h"

// Core types: EntityID, NameComponent, Transform, MeshRenderer, Camera, Script
#include "Components/CoreComponents.h"

// Physics: RigidBodyComponent, ColliderComponent
#include "Components/PhysicsComponents.h"

// Audio: AudioSourceComponent
#include "Components/AudioComponents.h"

// Lighting: LightComponent
#include "Components/LightComponents.h"

// Animation & Particles: AnimationController, ParticleEmitterComponent
#include "Components/AnimationComponents.h"

// AI: AIComponent
#include "Components/AIComponents.h"

// Networking: NetworkIdentity (R6.2 — separated from AI)
#include "Components/NetworkComponents.h"

// Gameplay: TagComponent, ActiveComponent, HealthComponent, Weather, Inventory, Quests
#include "Components/GameplayComponents.h"

// 2D/2.5D: SpriteRenderer, SpriteAnimator, Camera2D, TilemapComponent, RigidBody2D, Collider2D, Parallax
#include "Components/Sprite2DComponents.h"

// FPS: DecalComponent, ProjectileComponent, InteractionComponent
#include "Components/FPSComponents.h"

// Spline: SplineComponent, SplineFollowerComponent
#include "Components/SplineComponents.h"

// Volumes & Probes: TriggerVolume, PostProcessVolume, ReflectionProbe, LightProbe,
// NavObstacle, WaterPlane, FogVolume, LODGroup, SpawnPoint, AudioReverbZone
#include "Components/VolumeComponents.h"

// Placement: WindZone, PhysicsJoint, Occluder, CoverPoint, TacticalPoint,
// Destructible, CinematicTrigger, DialogueTrigger, AreaBoundary, Billboard
#include "Components/PlacementComponents.h"

// Advanced: AudioListener, CharacterController, NavRegion, NavLink, Skybox,
// ConstantForce, ForceRegion, Ragdoll, SoftBody, Vehicle, BuoyancyVolume,
// SpringArm, LineRenderer, TrailRenderer, Text3D, FoliageVolume
#include "Components/AdvancedPlacementComponents.h"

// =============================================================================
// World
// =============================================================================

/**
 * @class World
 * @brief Thin wrapper around an EnTT registry providing the ECS entity/component API.
 *
 * World is the central hub of the ECS runtime. It owns the EnTT registry and
 * exposes a strongly-typed interface for creating entities and managing their
 * components. Every Scene owns exactly one World instance.
 */
class World
{
  public:
    EntityID CreateEntity(const std::string& name = "")
    {
        EntityID entity = m_registry.create();
        SPARK_REQUIRE(Spark::LogCategory::ECS, entity != entt::null);
        SPARK_LOG_TRACE(Spark::LogCategory::ECS, "Created entity %u%s%s", static_cast<uint32_t>(entity),
                        name.empty() ? "" : " name=", name.c_str());
        if (!name.empty())
        {
            m_registry.emplace<NameComponent>(entity, NameComponent{name});
        }
        return entity;
    }

    /**
     * Attach an entity to a parent while maintaining both Transform edges.
     * Use this instead of assigning Transform::parent directly so destruction
     * can repair hierarchy links without an O(entity-count) world scan.
     */
    bool SetParent(EntityID child, EntityID parent)
    {
        if (!m_registry.valid(child) || child == parent || (parent != entt::null && !m_registry.valid(parent)))
            return false;

        // Reject cycles before changing either side of the relationship.
        EntityID ancestor = parent;
        std::vector<EntityID> visited;
        while (ancestor != entt::null)
        {
            if (ancestor == child)
                return false;
            if (std::find(visited.begin(), visited.end(), ancestor) != visited.end())
                return false;
            visited.push_back(ancestor);
            const Transform* ancestorTransform = m_registry.try_get<Transform>(ancestor);
            if (!ancestorTransform || ancestorTransform->parent == ancestor ||
                !m_registry.valid(ancestorTransform->parent))
                break;
            ancestor = ancestorTransform->parent;
        }

        Transform& childTransform = m_registry.get_or_emplace<Transform>(child);
        if (childTransform.parent != entt::null && m_registry.valid(childTransform.parent))
        {
            if (Transform* oldParent = m_registry.try_get<Transform>(childTransform.parent))
                std::erase(oldParent->children, child);
        }

        childTransform.parent = parent;
        if (parent != entt::null)
        {
            Transform& parentTransform = m_registry.get_or_emplace<Transform>(parent);
            if (std::find(parentTransform.children.begin(), parentTransform.children.end(), child) ==
                parentTransform.children.end())
                parentTransform.children.push_back(child);
        }
        return true;
    }

    void DestroyEntity(EntityID entity)
    {
        if (!m_registry.valid(entity))
        {
            SPARK_LOG_WARN(Spark::LogCategory::ECS, "DestroyEntity called with invalid entity %u",
                           static_cast<uint32_t>(entity));
            return;
        }
        SPARK_LOG_TRACE(Spark::LogCategory::ECS, "Destroying entity %u", static_cast<uint32_t>(entity));

        // Repair the two hierarchy edges owned by this transform.  Walking
        // every Transform here makes bulk destruction quadratic (100k
        // independent entities previously required ~5 billion visits).
        // Parent/children are maintained as reciprocal links by SetParent,
        // so cleanup is proportional to this entity's
        // immediate family instead of the entire world.
        if (const Transform* transform = m_registry.try_get<Transform>(entity))
        {
            const EntityID parent = transform->parent;
            const std::vector<EntityID> children = transform->children;

            if (parent != entt::null && m_registry.valid(parent))
            {
                if (Transform* parentTransform = m_registry.try_get<Transform>(parent))
                    std::erase(parentTransform->children, entity);
            }

            for (EntityID child : children)
            {
                if (child == entity || child == entt::null || !m_registry.valid(child))
                    continue;
                if (Transform* childTransform = m_registry.try_get<Transform>(child);
                    childTransform && childTransform->parent == entity)
                {
                    childTransform->parent = entt::null;
                }
            }
        }

        // Clean up any per-entity event subscriptions before destroying
        Spark::EntityEventBus::Global().RemoveEntity(static_cast<Spark::EventEntityID>(entity));

        m_registry.destroy(entity);
    }

    template <typename T, typename... Args> T& AddComponent(EntityID entity, Args&&... args)
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::ECS, m_registry.valid(entity), "AddComponent called with invalid entity");
        SPARK_REQUIRE_MSG(Spark::LogCategory::ECS, !m_registry.all_of<T>(entity),
                          "Entity already has the requested component");
        return m_registry.emplace<T>(entity, std::forward<Args>(args)...);
    }

    template <typename T> T* GetComponent(EntityID entity)
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::ECS, m_registry.valid(entity), "GetComponent called with invalid entity");
        return m_registry.try_get<T>(entity);
    }
    template <typename T> const T* GetComponent(EntityID entity) const
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::ECS, m_registry.valid(entity), "GetComponent called with invalid entity");
        return m_registry.try_get<T>(entity);
    }

    template <typename T> bool HasComponent(EntityID entity) const
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::ECS, m_registry.valid(entity), "HasComponent called with invalid entity");
        return m_registry.all_of<T>(entity);
    }

    template <typename T> void RemoveComponent(EntityID entity)
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::ECS, m_registry.valid(entity),
                          "RemoveComponent called with invalid entity");
        SPARK_REQUIRE_MSG(Spark::LogCategory::ECS, m_registry.all_of<T>(entity),
                          "RemoveComponent called but entity does not have the component");
        m_registry.remove<T>(entity);
    }

    template <typename... Components> auto GetEntitiesWith() { return m_registry.view<Components...>(); }
    template <typename... Components> auto GetEntitiesWith() const { return m_registry.view<Components...>(); }

    size_t GetEntityCount() const
    {
        size_t count = 0;
        for ([[maybe_unused]] auto entity : m_registry.template storage<entt::entity>()->each())
            ++count;
        return count;
    }

    entt::registry& GetRegistry() { return m_registry; }
    const entt::registry& GetRegistry() const { return m_registry; }

  private:
    entt::registry m_registry;
};
