/**
 * @file ARPGActorPresentation.h
 * @brief Mirrors the ARPG hero and every live monster into the ECS World as rendered actors
 *
 * The hero, combat and monster systems stay the authority for gameplay state; this class gives that state a
 * body in the engine World. Each frame SyncActors() creates, moves or destroys one entity per actor so the
 * World always holds exactly the hero and the monsters ARPGMonsterSystem reports as active:
 *  - Transform: the hero stands at the crypt entrance (origin) facing down -Z toward the dungeon; ordinary
 *    targets stand in the approach aisle and a boss holds the arena in front of the portal gate. Higher monster
 *    ranks are drawn larger.
 *  - MeshRenderer: Assets/Models/character.obj (the repository-authored humanoid) for hero and monsters.
 *  - HealthComponent: current/max health copied from HeroData / MonsterData. A mirrored death is written with
 *    isDead and deathProcessed both set, because the gameplay systems own death handling: the engine
 *    LifecycleSystem never fires its death callback for these actors.
 *
 * Contract:
 *  - Thread affinity: game thread only (same thread as the module's OnUpdate and console commands).
 *  - Ownership: owns the actor entities it creates and destroys them in Shutdown(); borrows the encounter,
 *    hero and monster systems, which must outlive it.
 *  - Allocation: allocates only when an actor is created (entity name, map node) and in
 *    RebuildAfterWorldLoad(); steady-state syncs of existing actors do not allocate.
 *  - Scalability: linear in live monsters (OD-20 budgets the dungeon slice at 48 active monster actors).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace Spark
{
    class IEngineContext;
}

namespace ARPG
{
    class ARPGDemoEncounter;
    class ARPGMonsterSystem;

    /** @brief Keeps one ECS actor per hero and live monster in the engine World. */
    class ARPGActorPresentation
    {
      public:
        /// Mesh used for the hero and every monster (humanoid, metres, ground pivot, front +Z).
        static constexpr const char* ActorMeshPath = "Assets/Models/character.obj";
        /// Entity names carry this prefix so actors restored by a world load can be found and replaced.
        static constexpr const char* HeroEntityName = "ARPG_Actor_Hero";
        static constexpr const char* MonsterEntityPrefix = "ARPG_Actor_Monster_";

        /**
         * @brief Bind to the engine and the encounter and place the initial actors.
         * @return false when the encounter or monster system is missing. A missing World is not an error:
         *         the gameplay runs headless and SyncActors() becomes a no-op.
         */
        bool Initialize(Spark::IEngineContext* context, const ARPGDemoEncounter* encounter,
                        const ARPGMonsterSystem* monsters);

        /** @brief Destroy every actor entity this presentation owns and drop the bindings. */
        void Shutdown();

        /** @brief Create, move or remove actor entities so the World matches the gameplay state. */
        void SyncActors();

        /**
         * @brief Rebuild the actors after SaveSystem replaced the World's entities.
         *
         * A world load assigns fresh identifiers, so the cached ones may now name unrelated entities. The
         * cached identifiers are dropped without being destroyed, every restored entity carrying an actor name
         * is destroyed, and SyncActors() recreates the actors from the (already restored) gameplay state.
         */
        void RebuildAfterWorldLoad();

        /** @brief World entity (full EnTT identifier) of the hero actor, if one is placed. */
        [[nodiscard]] std::optional<uint32_t> GetHeroEntity() const;
        /** @brief World entity of the actor for @p monsterId, if that monster is live and placed. */
        [[nodiscard]] std::optional<uint32_t> GetMonsterEntity(uint32_t monsterId) const;
        [[nodiscard]] size_t GetMonsterActorCount() const { return m_monsterEntities.size(); }

      private:
        void SyncHero();
        void SyncMonsters();
        void DestroyActors();

        Spark::IEngineContext* m_context{nullptr};
        const ARPGDemoEncounter* m_encounter{nullptr};
        const ARPGMonsterSystem* m_monsters{nullptr};
        std::optional<uint32_t> m_heroEntity;
        std::unordered_map<uint32_t, uint32_t> m_monsterEntities; ///< monsterId -> World entity
    };

} // namespace ARPG
