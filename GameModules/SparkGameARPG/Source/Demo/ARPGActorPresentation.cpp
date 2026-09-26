/**
 * @file ARPGActorPresentation.cpp
 * @brief Hero and monster actors mirrored into the ECS World
 */

#include "ARPGActorPresentation.h"

#include "ARPGDemoEncounter.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Hero/ARPGHeroSystem.h"
#include "Monster/ARPGMonsterSystem.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <string>
#include <string_view>
#include <vector>

namespace ARPG
{
    namespace
    {
        // Crypt layout (see ARPGDungeonSystem::PlaceCryptKit): the hero enters at the origin looking down -Z.
        // Ordinary targets meet the hero in the aisle before the spike trap (z = -7); bosses hold the arena
        // between the necrotic pillars, in front of the portal gate (z = -14).
        constexpr float AISLE_Z = -4.5f;
        constexpr float AISLE_SPACING = 1.6f;
        constexpr float ARENA_Z = -10.5f;
        constexpr float ARENA_SPACING = 2.2f;
        constexpr float HERO_YAW_DEGREES = 180.0f;  ///< Mesh front is +Z; the hero faces into the dungeon (-Z).
        constexpr float MONSTER_YAW_DEGREES = 0.0f; ///< Monsters face back toward the entrance (+Z).

        bool HoldsArena(ARPGMonsterRank rank)
        {
            return rank == ARPGMonsterRank::Boss || rank == ARPGMonsterRank::MiniBoss;
        }

        /// Stronger ranks read as bigger silhouettes.
        float ScaleForRank(ARPGMonsterRank rank)
        {
            switch (rank)
            {
            case ARPGMonsterRank::Champion:
                return 1.15f;
            case ARPGMonsterRank::Elite:
                return 1.3f;
            case ARPGMonsterRank::MiniBoss:
                return 1.45f;
            case ARPGMonsterRank::Boss:
                return 1.6f;
            default:
                return 1.0f;
            }
        }

        /// X offset of slot @p index when @p count actors share one row, centred on the aisle.
        float RowOffset(size_t index, size_t count, float spacing)
        {
            return (static_cast<float>(index) - static_cast<float>(count - 1) * 0.5f) * spacing;
        }

        bool IsLive(World& world, uint32_t entityId)
        {
            return world.GetRegistry().valid(static_cast<EntityID>(entityId));
        }

        /// Returns the actor entity, creating it with its three components when it is missing or was destroyed.
        EntityID EnsureActor(World& world, std::optional<uint32_t>& cachedEntity, std::string_view name)
        {
            if (cachedEntity && IsLive(world, *cachedEntity))
            {
                const auto entity = static_cast<EntityID>(*cachedEntity);
                // Another system may have stripped a component; restore rather than render a partial actor.
                if (!world.HasComponent<Transform>(entity))
                    world.AddComponent<Transform>(entity);
                if (!world.HasComponent<MeshRenderer>(entity))
                    world.AddComponent<MeshRenderer>(entity).meshPath = ARPGActorPresentation::ActorMeshPath;
                if (!world.HasComponent<HealthComponent>(entity))
                    world.AddComponent<HealthComponent>(entity);
                return entity;
            }

            const EntityID entity = world.CreateEntity(std::string(name));
            world.AddComponent<Transform>(entity);
            world.AddComponent<MeshRenderer>(entity).meshPath = ARPGActorPresentation::ActorMeshPath;
            world.AddComponent<HealthComponent>(entity);
            cachedEntity = static_cast<uint32_t>(entity);
            return entity;
        }

        void ApplyPose(World& world, EntityID entity, const DirectX::XMFLOAT3& position, float yawDegrees, float scale)
        {
            Transform* transform = world.GetComponent<Transform>(entity);
            MeshRenderer* renderer = world.GetComponent<MeshRenderer>(entity);
            if (!transform || !renderer)
                return;
            if (transform->position.x == position.x && transform->position.y == position.y &&
                transform->position.z == position.z && transform->rotation.y == yawDegrees &&
                transform->scale.x == scale)
                return;
            transform->position = position;
            transform->rotation = {0.0f, yawDegrees, 0.0f};
            transform->scale = {scale, scale, scale};
            renderer->worldMatrixDirty = true;
        }

        void ApplyHealth(World& world, EntityID entity, float health, float maxHealth)
        {
            HealthComponent* component = world.GetComponent<HealthComponent>(entity);
            if (!component)
                return;
            component->health = health;
            component->maxHealth = maxHealth;
            // The hero and monster systems already resolved this death. Marking it processed keeps the engine
            // LifecycleSystem from treating a mirrored actor as a fresh death event, and keeps the
            // deathProcessed-implies-isDead invariant InvalidStateDetector checks.
            component->isDead = health <= 0.0f;
            component->deathProcessed = component->isDead;
        }
    } // namespace

    bool ARPGActorPresentation::Initialize(Spark::IEngineContext* context, const ARPGDemoEncounter* encounter,
                                           const ARPGMonsterSystem* monsters)
    {
        if (!encounter || !monsters)
            return false;
        m_context = context;
        m_encounter = encounter;
        m_monsters = monsters;
        SyncActors();
        if (m_heroEntity)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG actors: hero and %zu monster(s) placed in the World",
                           m_monsterEntities.size());
        }
        return true;
    }

    void ARPGActorPresentation::Shutdown()
    {
        DestroyActors();
        m_context = nullptr;
        m_encounter = nullptr;
        m_monsters = nullptr;
    }

    void ARPGActorPresentation::SyncActors()
    {
        SyncHero();
        SyncMonsters();
    }

    void ARPGActorPresentation::SyncHero()
    {
        World* world = m_context ? m_context->GetWorld() : nullptr;
        const HeroData* hero = m_encounter ? m_encounter->GetHero() : nullptr;
        if (!world)
            return;
        if (!hero)
        {
            if (m_heroEntity && IsLive(*world, *m_heroEntity))
                world->DestroyEntity(static_cast<EntityID>(*m_heroEntity));
            m_heroEntity.reset();
            return;
        }

        const EntityID entity = EnsureActor(*world, m_heroEntity, HeroEntityName);
        ApplyPose(*world, entity, {0.0f, 0.0f, 0.0f}, HERO_YAW_DEGREES, 1.0f);
        ApplyHealth(*world, entity, hero->health, hero->maxHealth);
    }

    void ARPGActorPresentation::SyncMonsters()
    {
        World* world = m_context ? m_context->GetWorld() : nullptr;
        if (!world || !m_monsters)
            return;

        // Retire actors whose monster was killed and removed, cleared by a restart or replaced by a restore.
        for (auto it = m_monsterEntities.begin(); it != m_monsterEntities.end();)
        {
            if (m_monsters->GetMonster(it->first))
            {
                ++it;
                continue;
            }
            if (IsLive(*world, it->second))
                world->DestroyEntity(static_cast<EntityID>(it->second));
            it = m_monsterEntities.erase(it);
        }

        const std::vector<MonsterData>& active = m_monsters->GetActiveMonsters();
        size_t arenaCount = 0;
        for (const MonsterData& monster : active)
            arenaCount += HoldsArena(monster.rank) ? 1 : 0;
        const size_t aisleCount = active.size() - arenaCount;

        size_t arenaIndex = 0;
        size_t aisleIndex = 0;
        for (const MonsterData& monster : active)
        {
            std::optional<uint32_t> cached;
            if (const auto found = m_monsterEntities.find(monster.monsterId); found != m_monsterEntities.end())
                cached = found->second;
            // The entity name is only built when the actor is (re)created, keeping steady-state syncs allocation-free.
            std::string name;
            if (!cached || !IsLive(*world, *cached))
                name = MonsterEntityPrefix + monster.name;
            const EntityID entity = EnsureActor(*world, cached, name);
            m_monsterEntities[monster.monsterId] = *cached;

            DirectX::XMFLOAT3 position{};
            if (HoldsArena(monster.rank))
                position = {RowOffset(arenaIndex++, arenaCount, ARENA_SPACING), 0.0f, ARENA_Z};
            else
                position = {RowOffset(aisleIndex++, aisleCount, AISLE_SPACING), 0.0f, AISLE_Z};
            ApplyPose(*world, entity, position, MONSTER_YAW_DEGREES, ScaleForRank(monster.rank));
            ApplyHealth(*world, entity, monster.health, monster.maxHealth);
        }
    }

    void ARPGActorPresentation::RebuildAfterWorldLoad()
    {
        // The cached identifiers belong to the replaced World; never destroy through them.
        m_heroEntity.reset();
        m_monsterEntities.clear();

        World* world = m_context ? m_context->GetWorld() : nullptr;
        if (world)
        {
            const std::string_view heroName = HeroEntityName;
            const std::string_view monsterPrefix = MonsterEntityPrefix;
            std::vector<EntityID> restoredActors;
            for (const EntityID entity : world->GetEntitiesWith<NameComponent>())
            {
                const std::string_view name = world->GetComponent<NameComponent>(entity)->name;
                if (name == heroName || name.starts_with(monsterPrefix))
                    restoredActors.push_back(entity);
            }
            for (const EntityID entity : restoredActors)
                world->DestroyEntity(entity);
        }
        SyncActors();
    }

    void ARPGActorPresentation::DestroyActors()
    {
        World* world = m_context ? m_context->GetWorld() : nullptr;
        if (world)
        {
            if (m_heroEntity && IsLive(*world, *m_heroEntity))
                world->DestroyEntity(static_cast<EntityID>(*m_heroEntity));
            for (const auto& [monsterId, entityId] : m_monsterEntities)
            {
                if (IsLive(*world, entityId))
                    world->DestroyEntity(static_cast<EntityID>(entityId));
            }
        }
        m_heroEntity.reset();
        m_monsterEntities.clear();
    }

    std::optional<uint32_t> ARPGActorPresentation::GetHeroEntity() const
    {
        return m_heroEntity;
    }

    std::optional<uint32_t> ARPGActorPresentation::GetMonsterEntity(uint32_t monsterId) const
    {
        const auto found = m_monsterEntities.find(monsterId);
        if (found == m_monsterEntities.end())
            return std::nullopt;
        return found->second;
    }

} // namespace ARPG
