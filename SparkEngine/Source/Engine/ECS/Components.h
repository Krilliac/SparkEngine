/// @file Components.h
/// @brief ECS components and World class (umbrella header).
/// Prefer including specific component headers for faster compilation.

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
    enum class EntityEventCleanupMode
    {
        Enabled,
        Suppressed
    };

    explicit World(EntityEventCleanupMode entityEventCleanupMode = EntityEventCleanupMode::Enabled) noexcept
        : m_entityEventCleanupMode(entityEventCleanupMode)
    {
    }

    /**
     * @brief Immutable hierarchy snapshot used to retire an entity without allocating.
     *
     * Build every plan before crossing a transaction's live-world boundary, then pass
     * the plans to DestroyEntity(const EntityRetirementPlan&). Lifecycle observers and
     * EntityEventBus cleanup retain the same ordering as DestroyEntity(EntityID).
     */
    struct EntityRetirementPlan
    {
        EntityID entity = entt::null;
        EntityID parent = entt::null;
        std::vector<EntityID> children;
    };

    using RetirementSnapshotProbe = void (*)(EntityID entity);

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

    /**
     * @brief Snapshot hierarchy links for a later allocation-free retirement.
     *
     * The returned child list owns its copy. Allocation and the optional testing
     * probe therefore run before callers enter an irreversible live-world commit.
     */
    [[nodiscard]] EntityRetirementPlan PrepareEntityRetirement(EntityID entity) const
    {
        EntityRetirementPlan plan;
        plan.entity = entity;
        if (!m_registry.valid(entity))
            return plan;

        if (m_retirementSnapshotProbe)
            m_retirementSnapshotProbe(entity);
        if (const Transform* transform = m_registry.try_get<Transform>(entity))
        {
            plan.parent = transform->parent;
            plan.children = transform->children;
        }
        return plan;
    }

    /** @brief Install a narrow per-World probe used by transactional boundary tests. */
    void SetRetirementSnapshotProbeForTesting(RetirementSnapshotProbe probe) noexcept
    {
        m_retirementSnapshotProbe = probe;
    }

    void DestroyEntity(EntityID entity) { DestroyEntity(PrepareEntityRetirement(entity)); }

    /**
     * @brief Retire an entity using hierarchy links copied before the commit boundary.
     *
     * This path performs no hierarchy allocation. EnTT lifecycle observers still run
     * synchronously during registry destruction and their exceptions propagate.
     */
    void DestroyEntity(const EntityRetirementPlan& plan)
    {
        const EntityID entity = plan.entity;
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
        if (m_registry.try_get<Transform>(entity))
        {
            if (plan.parent != entt::null && m_registry.valid(plan.parent))
            {
                if (Transform* parentTransform = m_registry.try_get<Transform>(plan.parent))
                    std::erase(parentTransform->children, entity);
            }

            for (EntityID child : plan.children)
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

        // Candidate worlds used by transactional restore must not erase global
        // subscriptions that belong to a live entity with the same identifier.
        if (m_entityEventCleanupMode == EntityEventCleanupMode::Enabled)
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
    RetirementSnapshotProbe m_retirementSnapshotProbe = nullptr;
    EntityEventCleanupMode m_entityEventCleanupMode = EntityEventCleanupMode::Enabled;
};
