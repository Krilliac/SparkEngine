/**
 * @file ARPGDemoEncounter.h
 * @brief Small deterministic encounter loop that turns the ARPG systems into a playable example.
 */

#pragma once

#include "Dungeon/ARPGDungeonSystem.h"
#include "Enums/ARPGEnums.h"

#include <cstdint>
#include <string>

namespace ARPG
{
    class ARPGHeroSystem;
    class ARPGCombatSystem;
    class ARPGLootSystem;
    class ARPGDungeonSystem;
    class ARPGSkillSystem;
    class ARPGMonsterSystem;
    struct HeroData;
    struct MonsterData;

    struct ARPGDemoEncounterState
    {
        uint32_t heroId = 0;
        uint32_t targetMonsterId = 0;
        uint32_t primarySkillId = 0;
        uint32_t killsOnFloor = 0;
        uint32_t totalKills = 0;
        float lastDamage = 0.0f;
        bool lastAttackWasSkill = false;
        bool runComplete = false;                               ///< Boss on the goal floor defeated
        ARPGMonsterRank lastDropRank = ARPGMonsterRank::Normal; ///< Rank the last loot drop was rolled for
        ARPGItemRarity lastDropRarity = ARPGItemRarity::Normal; ///< Rarity of the last loot drop
        uint32_t lastDropItemId = 0;                            ///< 0 until the first drop
    };

    /**
     * @brief Owns no subsystems; orchestrates one finite hero-versus-monster dungeon run.
     *
     * Each regular floor is cleared by KillsPerFloor kills. The run ends on RunGoalFloor (the first boss
     * floor): defeating its boss marks the run complete and no further targets spawn until Restart().
     */
    class ARPGDemoEncounter
    {
      public:
        static constexpr uint32_t KillsPerFloor = 3;
        static constexpr int RunGoalFloor = ARPGDungeonSystem::BOSS_FLOOR_INTERVAL;

        bool Initialize(ARPGHeroSystem* heroes, ARPGCombatSystem* combat, ARPGLootSystem* loot,
                        ARPGDungeonSystem* dungeon, ARPGSkillSystem* skills, ARPGMonsterSystem* monsters);
        void Shutdown();
        void Update();
        void Restart();

        bool BasicAttack();
        bool UsePrimarySkill();

        [[nodiscard]] const ARPGDemoEncounterState& GetState() const { return m_state; }
        [[nodiscard]] bool IsRunComplete() const { return m_state.runComplete; }
        [[nodiscard]] const HeroData* GetHero() const;
        [[nodiscard]] const MonsterData* GetTarget() const;
        [[nodiscard]] std::string GetStatusString() const;
        [[nodiscard]] std::string SerializeState() const;
        [[nodiscard]] bool CanRestoreState(const std::string& serializedState) const;
        bool RestoreState(const std::string& serializedState);

      private:
        bool ResolveAttack(float damage, ARPGDamageType type, bool isSkill);
        void SpawnNextTarget();
        void HandleDefeat(float xpReward, ARPGMonsterRank rank);

        ARPGHeroSystem* m_heroes = nullptr;
        ARPGCombatSystem* m_combat = nullptr;
        ARPGLootSystem* m_loot = nullptr;
        ARPGDungeonSystem* m_dungeon = nullptr;
        ARPGSkillSystem* m_skills = nullptr;
        ARPGMonsterSystem* m_monsters = nullptr;
        ARPGDemoEncounterState m_state;
    };
} // namespace ARPG
