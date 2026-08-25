/**
 * @file ARPGDemoEncounter.h
 * @brief Small deterministic encounter loop that turns the ARPG systems into a playable example.
 */

#pragma once

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
    };

    /**
     * @brief Owns no subsystems; orchestrates one repeatable hero-versus-monster loop.
     */
    class ARPGDemoEncounter
    {
      public:
        bool Initialize(ARPGHeroSystem* heroes, ARPGCombatSystem* combat, ARPGLootSystem* loot,
                        ARPGDungeonSystem* dungeon, ARPGSkillSystem* skills, ARPGMonsterSystem* monsters);
        void Shutdown();
        void Update();
        void Restart();

        bool BasicAttack();
        bool UsePrimarySkill();

        [[nodiscard]] const ARPGDemoEncounterState& GetState() const { return m_state; }
        [[nodiscard]] const HeroData* GetHero() const;
        [[nodiscard]] const MonsterData* GetTarget() const;
        [[nodiscard]] std::string GetStatusString() const;

      private:
        bool ResolveAttack(float damage, ARPGDamageType type, bool isSkill);
        void SpawnNextTarget();
        void HandleDefeat(float xpReward);

        ARPGHeroSystem* m_heroes = nullptr;
        ARPGCombatSystem* m_combat = nullptr;
        ARPGLootSystem* m_loot = nullptr;
        ARPGDungeonSystem* m_dungeon = nullptr;
        ARPGSkillSystem* m_skills = nullptr;
        ARPGMonsterSystem* m_monsters = nullptr;
        ARPGDemoEncounterState m_state;

        static constexpr uint32_t KillsPerFloor = 3;
    };
} // namespace ARPG
