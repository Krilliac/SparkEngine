/**
 * @file RPGDemoSession.h
 * @brief Playable orchestration layer for the SparkGameRPG showcase
 */

#pragma once

#include "Inventory/RPGInventorySystem.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace RPG
{
    class RPGCharacterSystem;
    class RPGCombatSystem;
    class RPGNPCSystem;
    class RPGWorldSetup;

    /**
     * @brief Connects the RPG module's systems into a compact playable adventure.
     *
     * The session owns only transient example state. Registries and algorithms remain
     * in their genre systems, while this class turns them into a travel/combat/loot/
     * quest loop usable from both the console and the editor debug UI.
     */
    class RPGDemoSession
    {
      public:
        bool Initialize(RPGCharacterSystem* characters, RPGCombatSystem* combat, RPGInventorySystem* inventory,
                        RPGNPCSystem* npcs, RPGWorldSetup* world);
        void Shutdown();
        void RenderDebugUI();

        std::string Reset(CharacterClass characterClass = CharacterClass::Warrior);
        std::string Travel(uint32_t areaId);
        std::string Attack();
        std::string Flee();
        std::string Rest();
        std::string Talk(uint32_t npcId);
        std::string UseItem(uint32_t itemId);
        std::string AcceptQuest(uint32_t questId);
        [[nodiscard]] std::string GetStatusString() const;
        [[nodiscard]] std::string SerializeState() const;
        [[nodiscard]] bool CanRestoreState(const std::string& serializedState) const;
        bool RestoreState(const std::string& serializedState);

        [[nodiscard]] uint32_t GetPlayerCharacterId() const { return m_playerCharacterId; }
        [[nodiscard]] uint32_t GetCurrentAreaId() const { return m_currentAreaId; }
        [[nodiscard]] bool IsInCombat() const { return m_activeEncounterId != 0; }

      private:
        void EquipStarterGear(CharacterClass characterClass);
        void FinishEnemy();
        [[nodiscard]] const AbilityDef* GetPrimaryAbility() const;

        RPGCharacterSystem* m_characters = nullptr;
        RPGCombatSystem* m_combat = nullptr;
        RPGInventorySystem* m_inventory = nullptr;
        RPGNPCSystem* m_npcs = nullptr;
        RPGWorldSetup* m_world = nullptr;

        RPGInventoryData m_playerInventory;
        uint32_t m_playerCharacterId = 0;
        uint32_t m_currentAreaId = 1;
        uint32_t m_activeEncounterId = 0;
        uint32_t m_activeEnemyId = 0;
        std::string m_activeEnemyName;
        float m_activeEnemyHealth = 0.0f;
        size_t m_encounterOrdinal = 0;
        std::string m_lastAction = "Adventure ready";
    };

} // namespace RPG
