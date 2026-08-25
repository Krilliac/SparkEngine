/**
 * @file RPGDemoSession.cpp
 * @brief Playable travel, combat, quest, and inventory loop for SparkGameRPG
 */

#include "RPGDemoSession.h"

#include "Character/RPGCharacterSystem.h"
#include "Combat/RPGCombatSystem.h"
#include "Engine/Gameplay/QuestSystem.h"
#include "NPC/RPGNPCSystem.h"
#include "World/RPGWorldSetup.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace RPG
{
    namespace
    {
        const char* GetClassName(CharacterClass characterClass)
        {
            switch (characterClass)
            {
            case CharacterClass::Warrior:
                return "Warrior";
            case CharacterClass::Mage:
                return "Mage";
            case CharacterClass::Ranger:
                return "Ranger";
            case CharacterClass::Cleric:
                return "Cleric";
            case CharacterClass::Rogue:
                return "Rogue";
            case CharacterClass::Paladin:
                return "Paladin";
            default:
                return "Unknown";
            }
        }

        struct RPGDemoSnapshot
        {
            CharacterClass characterClass = CharacterClass::Warrior;
            int level = 0;
            uint32_t xp = 0;
            uint32_t xpToNextLevel = 0;
            float currentHealth = 0.0f;
            float maxHealth = 0.0f;
            float currentMana = 0.0f;
            float maxMana = 0.0f;
            CharacterStats stats;
            int freeStatPoints = 0;
            uint32_t areaId = 0;
            uint64_t encounterOrdinal = 0;
            uint32_t activeEnemyId = 0;
            float activeEnemyHealth = 0.0f;
            std::string activeEnemyName;
            RPGInventoryData inventory;
            std::array<EquipmentBonus, static_cast<size_t>(ItemSlot::Count)> equipment{};
            std::vector<Spark::Gameplay::QuestProgressSnapshot> quests;
        };

        bool IsFinite(const CharacterStats& stats)
        {
            return std::isfinite(stats.strength) && std::isfinite(stats.dexterity) &&
                   std::isfinite(stats.intelligence) && std::isfinite(stats.wisdom) &&
                   std::isfinite(stats.constitution) && std::isfinite(stats.charisma);
        }

        bool ParseRPGDemoSnapshot(const std::string& serializedState, const RPGInventorySystem* inventorySystem,
                                  const RPGWorldSetup* world, RPGDemoSnapshot& result)
        {
            if (!inventorySystem || !world)
                return false;

            std::istringstream snapshot(serializedState);
            std::string magic;
            int version = 0;
            int characterClass = 0;
            size_t slotCount = 0;
            if (!(snapshot >> magic >> version >> characterClass >> result.level >> result.xp >> result.xpToNextLevel >>
                  result.currentHealth >> result.maxHealth >> result.currentMana >> result.maxMana >>
                  result.stats.strength >> result.stats.dexterity >> result.stats.intelligence >> result.stats.wisdom >>
                  result.stats.constitution >> result.stats.charisma >> result.freeStatPoints >> result.areaId >>
                  result.encounterOrdinal >> result.activeEnemyId >> result.activeEnemyHealth >>
                  std::quoted(result.activeEnemyName) >> result.inventory.maxSlots >> result.inventory.maxWeight >>
                  result.inventory.currency >> slotCount) ||
                magic != "RPGDEMO" || version != 2 || characterClass < 0 ||
                characterClass >= static_cast<int>(CharacterClass::Count) || result.level < 1 || result.level > 50 ||
                result.xpToNextLevel == 0 || !std::isfinite(result.currentHealth) || !std::isfinite(result.maxHealth) ||
                !std::isfinite(result.currentMana) || !std::isfinite(result.maxMana) || !IsFinite(result.stats) ||
                result.maxHealth <= 0.0f || result.currentHealth < 0.0f || result.currentHealth > result.maxHealth ||
                result.maxMana < 0.0f || result.currentMana < 0.0f || result.currentMana > result.maxMana ||
                result.freeStatPoints < 0 || !world->GetArea(result.areaId) || result.encounterOrdinal > 1000000 ||
                !std::isfinite(result.activeEnemyHealth) || result.activeEnemyName.size() > 128 ||
                ((result.activeEnemyId == 0) != result.activeEnemyName.empty()) ||
                (result.activeEnemyId == 0 ? result.activeEnemyHealth != 0.0f : result.activeEnemyHealth <= 0.0f) ||
                result.inventory.maxSlots < 1 || result.inventory.maxSlots > 256 ||
                !std::isfinite(result.inventory.maxWeight) || result.inventory.maxWeight <= 0.0f ||
                result.inventory.currency < 0 || slotCount > static_cast<size_t>(result.inventory.maxSlots))
                return false;

            result.characterClass = static_cast<CharacterClass>(characterClass);
            result.inventory.slots.reserve(slotCount);
            for (size_t index = 0; index < slotCount; ++index)
            {
                RPGItemStack item;
                if (!(snapshot >> item.itemDefId >> item.count) || item.itemDefId == 0 || item.count <= 0)
                    return false;
                const RPGItemDef* definition = inventorySystem->GetItem(item.itemDefId);
                if (!definition || item.count > definition->maxStackSize)
                    return false;
                result.inventory.slots.push_back(item);
            }
            if (inventorySystem->GetTotalWeight(result.inventory) > result.inventory.maxWeight + 0.001f)
                return false;

            size_t equipmentCount = 0;
            if (!(snapshot >> equipmentCount) || equipmentCount != result.equipment.size())
                return false;
            for (size_t index = 0; index < equipmentCount; ++index)
            {
                EquipmentBonus& equipment = result.equipment[index];
                equipment.slot = static_cast<ItemSlot>(index);
                if (!(snapshot >> equipment.itemId >> equipment.statBonus.strength >> equipment.statBonus.dexterity >>
                      equipment.statBonus.intelligence >> equipment.statBonus.wisdom >>
                      equipment.statBonus.constitution >> equipment.statBonus.charisma) ||
                    !IsFinite(equipment.statBonus))
                    return false;
                for (float& resistance : equipment.resistances)
                {
                    if (!(snapshot >> resistance) || !std::isfinite(resistance))
                        return false;
                }

                if (equipment.itemId != 0)
                {
                    const RPGItemDef* item = inventorySystem->GetItem(equipment.itemId);
                    if (!item || item->equipSlot != equipment.slot)
                        return false;
                }
            }

            size_t questCount = 0;
            if (!(snapshot >> questCount) || questCount > 64)
                return false;
            result.quests.reserve(questCount);
            for (size_t questIndex = 0; questIndex < questCount; ++questIndex)
            {
                Spark::Gameplay::QuestProgressSnapshot quest;
                int state = 0;
                size_t objectiveCount = 0;
                if (!(snapshot >> quest.questId >> state >> objectiveCount) || state < 1 || state > 3 ||
                    objectiveCount > 64)
                    return false;
                quest.state = static_cast<Spark::Gameplay::QuestState>(state);
                quest.objectiveCounts.resize(objectiveCount);
                for (uint32_t& count : quest.objectiveCounts)
                {
                    if (!(snapshot >> count))
                        return false;
                }
                result.quests.push_back(std::move(quest));
            }
            snapshot >> std::ws;
            return snapshot.eof() && Spark::Gameplay::QuestSystem::GetInstance().ValidateEntityState(result.quests);
        }
    } // namespace

    bool RPGDemoSession::Initialize(RPGCharacterSystem* characters, RPGCombatSystem* combat,
                                    RPGInventorySystem* inventory, RPGNPCSystem* npcs, RPGWorldSetup* world)
    {
        if (!characters || !combat || !inventory || !npcs || !world)
        {
            return false;
        }

        m_characters = characters;
        m_combat = combat;
        m_inventory = inventory;
        m_npcs = npcs;
        m_world = world;
        Reset();
        return true;
    }

    void RPGDemoSession::Shutdown()
    {
        if (m_activeEncounterId != 0 && m_combat)
        {
            m_combat->EndEncounter(m_activeEncounterId);
        }

        if (m_playerCharacterId != 0)
        {
            Spark::Gameplay::QuestSystem::GetInstance().ClearEntityState(m_playerCharacterId);
            if (m_combat)
                m_combat->ClearCharacterState(m_playerCharacterId);
            if (m_characters)
                m_characters->DestroyCharacter(m_playerCharacterId);
        }

        m_playerInventory = {};
        m_playerCharacterId = 0;
        m_currentAreaId = 1;
        m_activeEncounterId = 0;
        m_activeEnemyId = 0;
        m_activeEnemyName.clear();
        m_activeEnemyHealth = 0.0f;
        m_encounterOrdinal = 0;
        m_lastAction = "Adventure stopped";
        m_characters = nullptr;
        m_combat = nullptr;
        m_inventory = nullptr;
        m_npcs = nullptr;
        m_world = nullptr;
    }

    std::string RPGDemoSession::Reset(CharacterClass characterClass)
    {
        if (!m_characters || !m_combat || !m_inventory || !m_world || characterClass >= CharacterClass::Count)
        {
            return "RPG demo session is not available";
        }

        if (m_activeEncounterId != 0)
        {
            m_combat->EndEncounter(m_activeEncounterId);
        }
        auto& quests = Spark::Gameplay::QuestSystem::GetInstance();
        if (m_playerCharacterId != 0)
        {
            quests.ClearEntityState(m_playerCharacterId);
            m_combat->ClearCharacterState(m_playerCharacterId);
            m_characters->DestroyCharacter(m_playerCharacterId);
        }

        m_playerInventory = {};
        m_playerInventory.maxSlots = 12;
        m_playerInventory.maxWeight = 60.0f;
        m_playerInventory.currency = 25;
        m_playerCharacterId = m_characters->CreateCharacter("Rowan", characterClass);
        m_currentAreaId = 1;
        m_activeEncounterId = 0;
        m_activeEnemyId = 0;
        m_activeEnemyName.clear();
        m_activeEnemyHealth = 0.0f;
        m_encounterOrdinal = 0;

        EquipStarterGear(characterClass);
        m_inventory->AddItem(m_playerInventory, 1, 3);
        quests.StartQuest(m_playerCharacterId, 1);

        m_lastAction = std::string("New ") + GetClassName(characterClass) + " adventure started in Oakhollow";
        return m_lastAction;
    }

    void RPGDemoSession::EquipStarterGear(CharacterClass characterClass)
    {
        uint32_t weaponId = 10;
        if (characterClass == CharacterClass::Mage || characterClass == CharacterClass::Cleric)
        {
            weaponId = 12;
        }
        else if (characterClass == CharacterClass::Ranger || characterClass == CharacterClass::Rogue)
        {
            weaponId = 13;
        }

        const auto* weapon = m_inventory->GetItem(weaponId);
        if (weapon && m_inventory->AddItem(m_playerInventory, weaponId) == 1)
        {
            m_characters->Equip(m_playerCharacterId, weapon->equipSlot, weaponId, weapon->statBonus);
        }

        if (characterClass == CharacterClass::Warrior || characterClass == CharacterClass::Paladin)
        {
            const auto* shield = m_inventory->GetItem(20);
            if (shield && m_inventory->AddItem(m_playerInventory, 20) == 1)
            {
                m_characters->Equip(m_playerCharacterId, shield->equipSlot, 20, shield->statBonus);
            }
        }
    }

    std::string RPGDemoSession::Travel(uint32_t areaId)
    {
        if (!m_world || !m_combat || m_activeEncounterId != 0)
        {
            return m_activeEncounterId != 0 ? "Defeat or flee from the current enemy before travelling"
                                            : "RPG demo session is not available";
        }

        const auto* currentArea = m_world->GetArea(m_currentAreaId);
        const auto* destination = m_world->GetArea(areaId);
        if (!currentArea || !destination)
        {
            return "Unknown RPG area: " + std::to_string(areaId);
        }
        if (areaId == m_currentAreaId)
        {
            return "Already in " + destination->name;
        }
        if (std::find(currentArea->connectedAreas.begin(), currentArea->connectedAreas.end(), areaId) ==
            currentArea->connectedAreas.end())
        {
            return destination->name + " is not connected to " + currentArea->name;
        }

        m_currentAreaId = areaId;
        auto& quests = Spark::Gameplay::QuestSystem::GetInstance();
        quests.ReportProgress(m_playerCharacterId, Spark::Gameplay::QuestObjective::Type::Reach, areaId, 1);

        if (destination->isSafeZone || destination->encounters.empty())
        {
            m_lastAction = "Travelled safely to " + destination->name;
            return m_lastAction;
        }

        const auto& enemy = destination->encounters[m_encounterOrdinal % destination->encounters.size()];
        ++m_encounterOrdinal;
        m_activeEnemyId = enemy.enemyId;
        m_activeEnemyName = enemy.enemyName;
        m_activeEnemyHealth = 35.0f + static_cast<float>(enemy.maxLevel) * 10.0f;
        m_activeEncounterId = m_combat->StartEncounter(m_playerCharacterId, m_activeEnemyId);
        m_lastAction = "Encountered " + m_activeEnemyName + " in " + destination->name;
        return m_lastAction;
    }

    const AbilityDef* RPGDemoSession::GetPrimaryAbility() const
    {
        const auto* character = m_characters ? m_characters->GetCharacter(m_playerCharacterId) : nullptr;
        const auto* classDef = character ? m_characters->GetClassDef(character->classId) : nullptr;
        if (!character || !classDef)
        {
            return nullptr;
        }

        const auto ability = std::find_if(classDef->abilities.begin(), classDef->abilities.end(),
                                          [character](const AbilityDef& candidate) {
                                              return !candidate.isPassive && candidate.baseDamage > 0.0f &&
                                                     candidate.requiredLevel <= character->level;
                                          });
        return ability != classDef->abilities.end() ? &*ability : nullptr;
    }

    std::string RPGDemoSession::Attack()
    {
        auto* character = m_characters ? m_characters->GetCharacter(m_playerCharacterId) : nullptr;
        const auto* ability = GetPrimaryAbility();
        if (!character || !ability || !m_combat)
        {
            return "RPG demo session is not available";
        }
        if (m_activeEncounterId == 0)
        {
            return "No enemy is currently engaged; travel to a dangerous area first";
        }
        if (character->currentHealth <= 0.0f)
        {
            return "Rowan is defeated; rest in Oakhollow or restart the adventure";
        }
        if (character->currentMana < ability->manaCost)
        {
            return "Not enough mana for " + ability->name;
        }
        if (!m_combat->UseAbility(m_playerCharacterId, ability->id, ability->cooldown))
        {
            return ability->name + " is still on cooldown";
        }

        character->currentMana -= ability->manaCost;
        const auto stats = m_characters->ComputeEffectiveStats(m_playerCharacterId);
        const auto* combo = m_combat->GetComboState(m_playerCharacterId);
        const ComboState noCombo{};
        const ResistanceProfile noResistance{};
        const auto result = m_combat->CalculateDamage(ability->baseDamage, ability->damageType, stats.strength, 8.0f,
                                                      noResistance, combo ? *combo : noCombo);
        m_combat->RegisterHit(m_playerCharacterId);
        m_activeEnemyHealth = std::max(0.0f, m_activeEnemyHealth - result.mitigatedDamage);

        std::ostringstream message;
        message << ability->name << " hit " << m_activeEnemyName << " for " << static_cast<int>(result.mitigatedDamage)
                << " damage";
        if (result.isCritical)
        {
            message << " (critical)";
        }

        if (m_activeEnemyHealth <= 0.0f)
        {
            FinishEnemy();
            message << "; enemy defeated";
        }
        else
        {
            const auto* area = m_world->GetArea(m_currentAreaId);
            const float retaliation = 4.0f + (area ? static_cast<float>(area->suggestedLevel) * 2.0f : 0.0f);
            character->currentHealth = std::max(0.0f, character->currentHealth - retaliation);
            message << "; " << m_activeEnemyName << " retaliated for " << static_cast<int>(retaliation);
            if (character->currentHealth <= 0.0f)
            {
                m_combat->EndEncounter(m_activeEncounterId);
                m_combat->ResetCombo(m_playerCharacterId);
                m_activeEncounterId = 0;
                m_activeEnemyId = 0;
                m_activeEnemyName.clear();
                m_activeEnemyHealth = 0.0f;
                message << "; Rowan was defeated";
            }
        }
        m_lastAction = message.str();
        return m_lastAction;
    }

    std::string RPGDemoSession::Flee()
    {
        if (m_activeEncounterId == 0 || !m_combat)
        {
            return "No active encounter to flee from";
        }

        m_combat->EndEncounter(m_activeEncounterId);
        m_combat->ResetCombo(m_playerCharacterId);
        m_currentAreaId = 1;
        m_activeEncounterId = 0;
        m_activeEnemyId = 0;
        m_activeEnemyName.clear();
        m_activeEnemyHealth = 0.0f;
        m_lastAction = "Escaped back to Oakhollow";
        return m_lastAction;
    }

    void RPGDemoSession::FinishEnemy()
    {
        m_combat->EndEncounter(m_activeEncounterId);
        m_combat->ResetCombo(m_playerCharacterId);
        m_characters->AddXP(m_playerCharacterId, 50);
        m_inventory->AddItem(m_playerInventory, 1, 1);

        auto& quests = Spark::Gameplay::QuestSystem::GetInstance();
        quests.ReportProgress(m_playerCharacterId, Spark::Gameplay::QuestObjective::Type::Kill, m_activeEnemyId, 1);
        if (m_currentAreaId == 2)
        {
            m_inventory->AddItem(m_playerInventory, 200, 1);
            quests.ReportProgress(m_playerCharacterId, Spark::Gameplay::QuestObjective::Type::Collect, 200, 1);
        }
        for (const uint32_t questId : quests.GetActiveQuests(m_playerCharacterId))
        {
            if (quests.IsQuestComplete(m_playerCharacterId, questId))
            {
                quests.CompleteQuest(m_playerCharacterId, questId);
            }
        }

        m_activeEncounterId = 0;
        m_activeEnemyId = 0;
        m_activeEnemyName.clear();
        m_activeEnemyHealth = 0.0f;
    }

    std::string RPGDemoSession::Rest()
    {
        auto* character = m_characters ? m_characters->GetCharacter(m_playerCharacterId) : nullptr;
        const auto* area = m_world ? m_world->GetArea(m_currentAreaId) : nullptr;
        if (!character || !area)
        {
            return "RPG demo session is not available";
        }
        if (m_activeEncounterId != 0 || !area->isSafeZone)
        {
            return "Resting is only available outside combat in a safe area";
        }

        character->currentHealth = character->maxHealth;
        character->currentMana = character->maxMana;
        m_lastAction = "Rested at the Oakhollow inn; health and mana restored";
        return m_lastAction;
    }

    std::string RPGDemoSession::Talk(uint32_t npcId)
    {
        const auto* npc = m_npcs ? m_npcs->GetNPC(npcId) : nullptr;
        if (!npc)
        {
            return "Unknown NPC: " + std::to_string(npcId);
        }
        if (m_activeEncounterId != 0)
        {
            return "Cannot start a conversation during combat";
        }
        if (npc->areaId != m_currentAreaId)
        {
            return npc->name + " is not in the current area";
        }

        auto& quests = Spark::Gameplay::QuestSystem::GetInstance();
        quests.ReportProgress(m_playerCharacterId, Spark::Gameplay::QuestObjective::Type::Talk, npcId, 1);
        for (const uint32_t questId : quests.GetActiveQuests(m_playerCharacterId))
        {
            if (quests.IsQuestComplete(m_playerCharacterId, questId))
            {
                quests.CompleteQuest(m_playerCharacterId, questId);
            }
        }

        m_lastAction = "Talked with " + npc->name;
        if (npc->questId != 0)
        {
            m_lastAction += " (offers quest " + std::to_string(npc->questId) + ')';
        }
        return m_lastAction;
    }

    std::string RPGDemoSession::UseItem(uint32_t itemId)
    {
        auto* character = m_characters ? m_characters->GetCharacter(m_playerCharacterId) : nullptr;
        const auto* item = m_inventory ? m_inventory->GetItem(itemId) : nullptr;
        if (!character || !item)
        {
            return item ? "RPG demo session is not available" : "Unknown item: " + std::to_string(itemId);
        }
        if (!item->isConsumable)
        {
            return item->name + " is not consumable";
        }
        if (!m_inventory->HasItem(m_playerInventory, itemId))
        {
            return item->name + " is not in the inventory";
        }

        const float oldHealth = character->currentHealth;
        const float oldMana = character->currentMana;
        character->currentHealth = std::min(character->maxHealth, character->currentHealth + item->healAmount);
        character->currentMana = std::min(character->maxMana, character->currentMana + item->manaRestore);
        if (character->currentHealth == oldHealth && character->currentMana == oldMana)
        {
            return item->name + " is not needed right now";
        }

        m_inventory->RemoveItem(m_playerInventory, itemId, 1);
        m_lastAction = "Used " + item->name;
        return m_lastAction;
    }

    std::string RPGDemoSession::AcceptQuest(uint32_t questId)
    {
        if (m_playerCharacterId == 0)
        {
            return "RPG demo session is not available";
        }

        return Spark::Gameplay::QuestSystem::GetInstance().StartQuest(m_playerCharacterId, questId)
                   ? "Accepted quest " + std::to_string(questId)
                   : "Quest " + std::to_string(questId) + " is unavailable or already active";
    }

    std::string RPGDemoSession::GetStatusString() const
    {
        const auto* character = m_characters ? m_characters->GetCharacter(m_playerCharacterId) : nullptr;
        const auto* area = m_world ? m_world->GetArea(m_currentAreaId) : nullptr;
        if (!character || !area || !m_inventory)
        {
            return "RPG demo session is not available";
        }

        std::ostringstream status;
        status << "=== Oakhollow Adventure ===\n";
        status << character->name << " - " << GetClassName(character->classId) << " Lv" << character->level << " (XP "
               << character->xp << '/' << character->xpToNextLevel << ")\n";
        status << "HP " << static_cast<int>(character->currentHealth) << '/' << static_cast<int>(character->maxHealth)
               << " | MP " << static_cast<int>(character->currentMana) << '/' << static_cast<int>(character->maxMana)
               << "\n";
        status << "Area: " << area->name << " | Gold: " << m_playerInventory.currency
               << " | Carry: " << m_inventory->GetTotalWeight(m_playerInventory) << '/' << m_playerInventory.maxWeight
               << " kg\n";
        status << "Active quests: "
               << Spark::Gameplay::QuestSystem::GetInstance().GetActiveQuests(m_playerCharacterId).size() << '\n';
        if (m_activeEncounterId != 0)
        {
            status << "Enemy: " << m_activeEnemyName << " (HP " << static_cast<int>(m_activeEnemyHealth) << ")\n";
        }
        status << "Last action: " << m_lastAction;
        return status.str();
    }

    std::string RPGDemoSession::SerializeState() const
    {
        const CharacterData* character = m_characters ? m_characters->GetCharacter(m_playerCharacterId) : nullptr;
        if (!character || !m_world || !m_world->GetArea(m_currentAreaId))
            return {};

        const auto questState = Spark::Gameplay::QuestSystem::GetInstance().CaptureEntityState(m_playerCharacterId);
        std::ostringstream snapshot;
        snapshot << std::setprecision(std::numeric_limits<float>::max_digits10) << "RPGDEMO 2 "
                 << static_cast<int>(character->classId) << ' ' << character->level << ' ' << character->xp << ' '
                 << character->xpToNextLevel << ' ' << character->currentHealth << ' ' << character->maxHealth << ' '
                 << character->currentMana << ' ' << character->maxMana << ' ' << character->baseStats.strength << ' '
                 << character->baseStats.dexterity << ' ' << character->baseStats.intelligence << ' '
                 << character->baseStats.wisdom << ' ' << character->baseStats.constitution << ' '
                 << character->baseStats.charisma << ' ' << character->freeStatPoints << ' ' << m_currentAreaId << ' '
                 << static_cast<uint64_t>(m_encounterOrdinal) << ' ' << m_activeEnemyId << ' ' << m_activeEnemyHealth
                 << ' ' << std::quoted(m_activeEnemyName) << ' ' << m_playerInventory.maxSlots << ' '
                 << m_playerInventory.maxWeight << ' ' << m_playerInventory.currency << ' '
                 << m_playerInventory.slots.size();
        for (const RPGItemStack& item : m_playerInventory.slots)
            snapshot << ' ' << item.itemDefId << ' ' << item.count;

        snapshot << ' ' << character->equipment.size();
        for (const EquipmentBonus& equipment : character->equipment)
        {
            snapshot << ' ' << equipment.itemId << ' ' << equipment.statBonus.strength << ' '
                     << equipment.statBonus.dexterity << ' ' << equipment.statBonus.intelligence << ' '
                     << equipment.statBonus.wisdom << ' ' << equipment.statBonus.constitution << ' '
                     << equipment.statBonus.charisma;
            for (float resistance : equipment.resistances)
                snapshot << ' ' << resistance;
        }

        snapshot << ' ' << questState.size();
        for (const Spark::Gameplay::QuestProgressSnapshot& quest : questState)
        {
            snapshot << ' ' << quest.questId << ' ' << static_cast<int>(quest.state) << ' '
                     << quest.objectiveCounts.size();
            for (uint32_t count : quest.objectiveCounts)
                snapshot << ' ' << count;
        }
        return snapshot.str();
    }

    bool RPGDemoSession::CanRestoreState(const std::string& serializedState) const
    {
        RPGDemoSnapshot snapshot;
        return m_characters && m_combat && ParseRPGDemoSnapshot(serializedState, m_inventory, m_world, snapshot);
    }

    bool RPGDemoSession::RestoreState(const std::string& serializedState)
    {
        if (!m_characters || !m_combat || !m_inventory || !m_world)
            return false;

        RPGDemoSnapshot snapshot;
        if (!ParseRPGDemoSnapshot(serializedState, m_inventory, m_world, snapshot))
            return false;

        Reset(snapshot.characterClass);
        CharacterData* character = m_characters->GetCharacter(m_playerCharacterId);
        if (!character)
            return false;
        character->level = snapshot.level;
        character->xp = snapshot.xp;
        character->xpToNextLevel = snapshot.xpToNextLevel;
        character->currentHealth = snapshot.currentHealth;
        character->maxHealth = snapshot.maxHealth;
        character->currentMana = snapshot.currentMana;
        character->maxMana = snapshot.maxMana;
        character->baseStats = snapshot.stats;
        character->equipment = snapshot.equipment;
        character->freeStatPoints = snapshot.freeStatPoints;

        auto& questSystem = Spark::Gameplay::QuestSystem::GetInstance();
        questSystem.ClearEntityState(m_playerCharacterId);
        if (!questSystem.RestoreEntityState(m_playerCharacterId, snapshot.quests))
        {
            Reset(snapshot.characterClass);
            return false;
        }

        m_playerInventory = std::move(snapshot.inventory);
        m_currentAreaId = snapshot.areaId;
        m_encounterOrdinal = static_cast<size_t>(snapshot.encounterOrdinal);
        m_activeEnemyId = snapshot.activeEnemyId;
        m_activeEnemyName = std::move(snapshot.activeEnemyName);
        m_activeEnemyHealth = snapshot.activeEnemyHealth;
        m_activeEncounterId = m_activeEnemyId == 0 ? 0 : m_combat->StartEncounter(m_playerCharacterId, m_activeEnemyId);
        if (m_activeEnemyId != 0 && m_activeEncounterId == 0)
        {
            Reset(snapshot.characterClass);
            return false;
        }
        m_lastAction = "Loaded saved Oakhollow adventure";
        return true;
    }

} // namespace RPG
