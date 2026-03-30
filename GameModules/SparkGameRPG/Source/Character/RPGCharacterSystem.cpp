/**
 * @file RPGCharacterSystem.cpp
 * @brief Character class definitions, stat growth, leveling, and equipment
 */

#include "RPGCharacterSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cmath>
#include <sstream>

namespace RPG
{

    bool RPGCharacterSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultClasses();

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Character system initialized (" +
                                                    std::to_string(m_classes.size()) + " classes)");
        return true;
    }

    void RPGCharacterSystem::Shutdown()
    {
        m_characters.clear();
    }

    void RPGCharacterSystem::RegisterDefaultClasses()
    {
        m_classes.clear();

        // Warrior — high STR/CON, tanky melee fighter
        ClassDef warrior;
        warrior.id = CharacterClass::Warrior;
        warrior.name = "Warrior";
        warrior.description = "A stalwart melee fighter excelling in strength and endurance";
        warrior.baseStats = {14, 10, 6, 8, 14, 8};
        warrior.growth = {2.5f, 1.5f, 0.5f, 1.0f, 2.0f, 0.5f};
        warrior.baseHealth = 150.0f;
        warrior.baseMana = 30.0f;
        warrior.healthPerLevel = 15.0f;
        warrior.manaPerLevel = 2.0f;
        warrior.abilities = {
            {1, "Power Strike", "A devastating melee blow", false, 4.0f, 10.0f, DamageType::Physical, 25.0f, 1},
            {2, "Shield Bash", "Stuns the target briefly", false, 8.0f, 15.0f, DamageType::Physical, 15.0f, 3},
            {3, "Iron Skin", "Passively reduces physical damage taken by 10%", true, 0, 0, DamageType::Physical, 0, 5},
            {4, "Cleave", "Strikes all enemies in a frontal arc", false, 6.0f, 20.0f, DamageType::Physical, 20.0f, 8},
        };
        m_classes.push_back(warrior);

        // Mage — high INT/WIS, devastating elemental magic
        ClassDef mage;
        mage.id = CharacterClass::Mage;
        mage.name = "Mage";
        mage.description = "A master of arcane arts wielding devastating elemental spells";
        mage.baseStats = {6, 8, 16, 12, 8, 10};
        mage.growth = {0.5f, 1.0f, 2.5f, 2.0f, 0.5f, 1.5f};
        mage.baseHealth = 80.0f;
        mage.baseMana = 120.0f;
        mage.healthPerLevel = 6.0f;
        mage.manaPerLevel = 12.0f;
        mage.abilities = {
            {10, "Fireball", "Hurls a blazing sphere", false, 3.0f, 20.0f, DamageType::Fire, 30.0f, 1},
            {11, "Frost Nova", "Freezes nearby enemies", false, 10.0f, 30.0f, DamageType::Ice, 20.0f, 4},
            {12, "Arcane Mind", "Passively increases mana regeneration by 15%", true, 0, 0, DamageType::Lightning, 0,
             6},
            {13, "Chain Lightning", "Lightning arcs between targets", false, 8.0f, 35.0f, DamageType::Lightning, 35.0f,
             10},
        };
        m_classes.push_back(mage);

        // Ranger — high DEX, ranged attacks and tracking
        ClassDef ranger;
        ranger.id = CharacterClass::Ranger;
        ranger.name = "Ranger";
        ranger.description = "A skilled marksman who thrives in the wilderness";
        ranger.baseStats = {10, 16, 8, 10, 10, 6};
        ranger.growth = {1.5f, 2.5f, 1.0f, 1.0f, 1.5f, 0.5f};
        ranger.baseHealth = 110.0f;
        ranger.baseMana = 60.0f;
        ranger.healthPerLevel = 10.0f;
        ranger.manaPerLevel = 5.0f;
        ranger.abilities = {
            {20, "Aimed Shot", "A precise, high-damage arrow", false, 5.0f, 15.0f, DamageType::Physical, 28.0f, 1},
            {21, "Rapid Fire", "Fires a quick volley of arrows", false, 8.0f, 20.0f, DamageType::Physical, 12.0f, 3},
            {22, "Eagle Eye", "Passively increases critical hit chance by 8%", true, 0, 0, DamageType::Physical, 0, 5},
            {23, "Trap", "Places a damaging trap at location", false, 12.0f, 25.0f, DamageType::Physical, 22.0f, 7},
        };
        m_classes.push_back(ranger);

        // Cleric — high WIS/CHA, healer and support
        ClassDef cleric;
        cleric.id = CharacterClass::Cleric;
        cleric.name = "Cleric";
        cleric.description = "A divine healer who channels holy power to mend and protect";
        cleric.baseStats = {8, 6, 10, 16, 12, 14};
        cleric.growth = {1.0f, 0.5f, 1.5f, 2.5f, 1.5f, 2.0f};
        cleric.baseHealth = 100.0f;
        cleric.baseMana = 100.0f;
        cleric.healthPerLevel = 9.0f;
        cleric.manaPerLevel = 10.0f;
        cleric.abilities = {
            {30, "Holy Light", "Heals a single target", false, 3.0f, 20.0f, DamageType::Holy, -35.0f, 1},
            {31, "Smite", "Strikes with holy energy", false, 5.0f, 15.0f, DamageType::Holy, 22.0f, 2},
            {32, "Blessing", "Passively increases party healing by 10%", true, 0, 0, DamageType::Holy, 0, 6},
            {33, "Sanctuary", "Creates a protective zone", false, 20.0f, 40.0f, DamageType::Holy, -50.0f, 10},
        };
        m_classes.push_back(cleric);

        // Rogue — high DEX/CHA, stealth and burst damage
        ClassDef rogue;
        rogue.id = CharacterClass::Rogue;
        rogue.name = "Rogue";
        rogue.description = "A cunning assassin who strikes from the shadows";
        rogue.baseStats = {8, 16, 8, 8, 8, 14};
        rogue.growth = {1.0f, 2.5f, 0.5f, 1.0f, 1.0f, 2.0f};
        rogue.baseHealth = 90.0f;
        rogue.baseMana = 50.0f;
        rogue.healthPerLevel = 8.0f;
        rogue.manaPerLevel = 4.0f;
        rogue.abilities = {
            {40, "Backstab", "Deals massive damage from behind", false, 6.0f, 15.0f, DamageType::Physical, 40.0f, 1},
            {41, "Poison Blade", "Coats weapon in shadow toxin", false, 10.0f, 20.0f, DamageType::Shadow, 18.0f, 4},
            {42, "Evasion", "Passively increases dodge chance by 12%", true, 0, 0, DamageType::Physical, 0, 5},
            {43, "Shadow Step", "Teleports behind the target", false, 15.0f, 25.0f, DamageType::Shadow, 10.0f, 9},
        };
        m_classes.push_back(rogue);

        // Paladin — balanced STR/WIS/CON, holy warrior
        ClassDef paladin;
        paladin.id = CharacterClass::Paladin;
        paladin.name = "Paladin";
        paladin.description = "A holy warrior blending martial prowess with divine protection";
        paladin.baseStats = {12, 8, 8, 12, 14, 10};
        paladin.growth = {2.0f, 0.5f, 1.0f, 2.0f, 2.0f, 1.5f};
        paladin.baseHealth = 130.0f;
        paladin.baseMana = 70.0f;
        paladin.healthPerLevel = 12.0f;
        paladin.manaPerLevel = 7.0f;
        paladin.abilities = {
            {50, "Holy Strike", "A weapon blow infused with holy light", false, 4.0f, 15.0f, DamageType::Holy, 22.0f,
             1},
            {51, "Lay on Hands", "Heals self for a large amount", false, 30.0f, 0, DamageType::Holy, -60.0f, 3},
            {52, "Aura of Protection", "Passively reduces all damage taken by 5%", true, 0, 0, DamageType::Holy, 0, 6},
            {53, "Divine Judgment", "Smites the target with holy wrath", false, 12.0f, 35.0f, DamageType::Holy, 45.0f,
             12},
        };
        m_classes.push_back(paladin);
    }

    // === Class queries ===

    const ClassDef* RPGCharacterSystem::GetClassDef(CharacterClass id) const
    {
        for (const auto& cls : m_classes)
        {
            if (cls.id == id)
                return &cls;
        }
        return nullptr;
    }

    std::string RPGCharacterSystem::GetClassListString() const
    {
        std::ostringstream ss;
        ss << "=== RPG Character Classes ===\n";
        for (const auto& cls : m_classes)
        {
            ss << cls.name << " - " << cls.description << "\n";
            ss << "  Base: STR " << cls.baseStats.strength << " DEX " << cls.baseStats.dexterity << " INT "
               << cls.baseStats.intelligence << " WIS " << cls.baseStats.wisdom << " CON " << cls.baseStats.constitution
               << " CHA " << cls.baseStats.charisma << "\n";
            ss << "  HP " << cls.baseHealth << " (+" << cls.healthPerLevel << "/lv) | MP " << cls.baseMana << " (+"
               << cls.manaPerLevel << "/lv)\n";
            ss << "  Abilities: ";
            for (size_t i = 0; i < cls.abilities.size(); ++i)
            {
                if (i > 0)
                    ss << ", ";
                ss << cls.abilities[i].name << "(Lv" << cls.abilities[i].requiredLevel << ")";
            }
            ss << "\n";
        }
        return ss.str();
    }

    // === Character CRUD ===

    uint32_t RPGCharacterSystem::CreateCharacter(const std::string& name, CharacterClass classId)
    {
        const auto* classDef = GetClassDef(classId);
        if (!classDef)
            return 0;

        CharacterData character;
        character.characterId = m_nextCharId++;
        character.name = name;
        character.classId = classId;
        character.level = 1;
        character.xp = 0;
        character.xpToNextLevel = CalculateXPForLevel(2);
        character.baseStats = classDef->baseStats;
        character.maxHealth = classDef->baseHealth;
        character.currentHealth = classDef->baseHealth;
        character.maxMana = classDef->baseMana;
        character.currentMana = classDef->baseMana;
        character.freeStatPoints = 0;

        uint32_t id = character.characterId;
        m_characters[id] = character;

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Character created: " + name + " (" + classDef->name + ")");
        return id;
    }

    CharacterData* RPGCharacterSystem::GetCharacter(uint32_t characterId)
    {
        auto it = m_characters.find(characterId);
        return it != m_characters.end() ? &it->second : nullptr;
    }

    const CharacterData* RPGCharacterSystem::GetCharacter(uint32_t characterId) const
    {
        auto it = m_characters.find(characterId);
        return it != m_characters.end() ? &it->second : nullptr;
    }

    // === Leveling ===

    uint32_t RPGCharacterSystem::CalculateXPForLevel(int level) const
    {
        // XP curve: 100 * level^1.5
        return static_cast<uint32_t>(100.0f * std::pow(static_cast<float>(level), 1.5f));
    }

    void RPGCharacterSystem::AddXP(uint32_t characterId, uint32_t amount)
    {
        auto* character = GetCharacter(characterId);
        if (!character || character->level >= MAX_LEVEL)
            return;

        character->xp += amount;

        while (character->xp >= character->xpToNextLevel && character->level < MAX_LEVEL)
        {
            character->xp -= character->xpToNextLevel;
            LevelUp(*character);
        }
    }

    void RPGCharacterSystem::LevelUp(CharacterData& character)
    {
        character.level++;
        character.freeStatPoints += STAT_POINTS_PER_LEVEL;
        character.xpToNextLevel = CalculateXPForLevel(character.level + 1);

        // Apply class stat growth
        const auto* classDef = GetClassDef(character.classId);
        if (classDef)
        {
            character.baseStats.strength += classDef->growth.strength;
            character.baseStats.dexterity += classDef->growth.dexterity;
            character.baseStats.intelligence += classDef->growth.intelligence;
            character.baseStats.wisdom += classDef->growth.wisdom;
            character.baseStats.constitution += classDef->growth.constitution;
            character.baseStats.charisma += classDef->growth.charisma;

            character.maxHealth += classDef->healthPerLevel;
            character.maxMana += classDef->manaPerLevel;
            character.currentHealth = character.maxHealth; // Full heal on level up
            character.currentMana = character.maxMana;
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] " + character.name + " reached level " +
                                                    std::to_string(character.level));
    }

    void RPGCharacterSystem::AllocateStatPoint(uint32_t characterId, const std::string& statName)
    {
        auto* character = GetCharacter(characterId);
        if (!character || character->freeStatPoints <= 0)
            return;

        if (statName == "strength")
            character->baseStats.strength += 1.0f;
        else if (statName == "dexterity")
            character->baseStats.dexterity += 1.0f;
        else if (statName == "intelligence")
            character->baseStats.intelligence += 1.0f;
        else if (statName == "wisdom")
            character->baseStats.wisdom += 1.0f;
        else if (statName == "constitution")
            character->baseStats.constitution += 1.0f;
        else if (statName == "charisma")
            character->baseStats.charisma += 1.0f;
        else
            return;

        character->freeStatPoints--;
        RecalculateDerivedStats(*character);
    }

    CharacterStats RPGCharacterSystem::ComputeEffectiveStats(uint32_t characterId) const
    {
        const auto* character = GetCharacter(characterId);
        if (!character)
            return {};

        CharacterStats effective = character->baseStats;

        // Add equipment bonuses
        for (const auto& equip : character->equipment)
        {
            if (equip.itemId == 0)
                continue;
            effective.strength += equip.statBonus.strength;
            effective.dexterity += equip.statBonus.dexterity;
            effective.intelligence += equip.statBonus.intelligence;
            effective.wisdom += equip.statBonus.wisdom;
            effective.constitution += equip.statBonus.constitution;
            effective.charisma += equip.statBonus.charisma;
        }

        return effective;
    }

    void RPGCharacterSystem::RecalculateDerivedStats(CharacterData& character) const
    {
        const auto* classDef = GetClassDef(character.classId);
        if (!classDef)
            return;

        // Health scales with constitution, mana with intelligence
        float conBonus = (character.baseStats.constitution - 10.0f) * 2.0f;
        float intBonus = (character.baseStats.intelligence - 10.0f) * 1.5f;

        float healthRatio = character.currentHealth / character.maxHealth;
        float manaRatio = character.currentMana / character.maxMana;

        character.maxHealth = classDef->baseHealth + classDef->healthPerLevel * (character.level - 1) + conBonus;
        character.maxMana = classDef->baseMana + classDef->manaPerLevel * (character.level - 1) + intBonus;

        // Maintain health/mana percentage
        character.currentHealth = character.maxHealth * healthRatio;
        character.currentMana = character.maxMana * manaRatio;
    }

    // === Equipment ===

    void RPGCharacterSystem::Equip(uint32_t characterId, ItemSlot slot, uint32_t itemId, const CharacterStats& bonus)
    {
        auto* character = GetCharacter(characterId);
        if (!character)
            return;

        auto slotIndex = static_cast<size_t>(slot);
        if (slotIndex >= character->equipment.size())
            return;

        character->equipment[slotIndex].slot = slot;
        character->equipment[slotIndex].itemId = itemId;
        character->equipment[slotIndex].statBonus = bonus;

        RecalculateDerivedStats(*character);
    }

    void RPGCharacterSystem::Unequip(uint32_t characterId, ItemSlot slot)
    {
        auto* character = GetCharacter(characterId);
        if (!character)
            return;

        auto slotIndex = static_cast<size_t>(slot);
        if (slotIndex >= character->equipment.size())
            return;

        character->equipment[slotIndex] = {};
        RecalculateDerivedStats(*character);
    }

    void RPGCharacterSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG Character System"))
        {
            ImGui::Text("Classes: %zu | Characters: %zu", m_classes.size(), m_characters.size());

            if (ImGui::TreeNode("Classes"))
            {
                for (const auto& cls : m_classes)
                {
                    if (ImGui::TreeNode(cls.name.c_str()))
                    {
                        ImGui::Text("%s", cls.description.c_str());
                        ImGui::Text("STR:%.0f DEX:%.0f INT:%.0f WIS:%.0f CON:%.0f CHA:%.0f", cls.baseStats.strength,
                                    cls.baseStats.dexterity, cls.baseStats.intelligence, cls.baseStats.wisdom,
                                    cls.baseStats.constitution, cls.baseStats.charisma);
                        ImGui::Text("HP:%.0f (+%.0f/lv) MP:%.0f (+%.0f/lv)", cls.baseHealth, cls.healthPerLevel,
                                    cls.baseMana, cls.manaPerLevel);
                        ImGui::Text("Abilities: %zu", cls.abilities.size());
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Characters"))
            {
                for (const auto& [id, ch] : m_characters)
                {
                    const auto* cls = GetClassDef(ch.classId);
                    if (ImGui::TreeNode(ch.name.c_str()))
                    {
                        ImGui::Text("Class: %s | Level: %d | XP: %u/%u", cls ? cls->name.c_str() : "?", ch.level, ch.xp,
                                    ch.xpToNextLevel);
                        ImGui::Text("HP: %.0f/%.0f | MP: %.0f/%.0f", ch.currentHealth, ch.maxHealth, ch.currentMana,
                                    ch.maxMana);
                        ImGui::Text("Free stat points: %d", ch.freeStatPoints);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
