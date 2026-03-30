/**
 * @file MMOCharacterSystem.cpp
 * @brief Race/class definitions, character creation and selection
 */

#include "MMOCharacterSystem.h"
#include "Utils/ContainerUtils.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <sstream>
#include "Utils/LogMacros.h"

namespace MMO
{

    bool MMOCharacterSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultRaces();
        RegisterDefaultClasses();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Character system initialized (%s races, %s classes)",
                       std::to_string(m_races.size()).c_str(), std::to_string(m_classes.size()).c_str());
        return true;
    }

    void MMOCharacterSystem::Shutdown()
    {
        m_characters.clear();
        m_accountCharacters.clear();
    }

    void MMOCharacterSystem::RegisterDefaultRaces()
    {
        m_races.clear();

        // Human — balanced, versatile
        RaceDef human;
        human.id = RaceId::Human;
        human.name = "Human";
        human.description = "Adaptable and resilient, humans excel in all disciplines";
        human.lore = "The most populous race, humans built the great city of TownSquare as a beacon of civilization.";
        human.baseStats = {100, 50, 100, 10, 10, 10, 5, 5.0f};
        human.startingAreaId = 1;
        human.spawnX = 0;
        human.spawnY = 1;
        human.spawnZ = 0;
        human.startingItems = {5, 4}; // Bandage, Combat Ration
        human.allowedClasses = {ClassId::Warrior, ClassId::Ranger, ClassId::Mage, ClassId::Healer, ClassId::Engineer};
        m_races.push_back(human);

        // Elf — high agility and intellect, lower health
        RaceDef elf;
        elf.id = RaceId::Elf;
        elf.name = "Elf";
        elf.description = "Graceful and attuned to magic, elves favor precision over brute force";
        elf.lore = "Ancient forest-dwellers who emerged from the Wilderness to trade with other races.";
        elf.baseStats = {85, 70, 90, 8, 14, 14, 3, 5.5f};
        elf.startingAreaId = 2;
        elf.spawnX = 550;
        elf.spawnY = 1;
        elf.spawnZ = 0;
        elf.startingItems = {2, 10}; // Mana Elixir, Herb
        elf.allowedClasses = {ClassId::Ranger, ClassId::Mage, ClassId::Healer};
        m_races.push_back(elf);

        // Dwarf — high health and strength, lower speed
        RaceDef dwarf;
        dwarf.id = RaceId::Dwarf;
        dwarf.name = "Dwarf";
        dwarf.description = "Stout and unyielding, dwarves are master crafters and fierce fighters";
        dwarf.lore = "Tunneling deep beneath the mountains, dwarves forge the finest weapons in the land.";
        dwarf.baseStats = {120, 35, 110, 14, 8, 8, 10, 4.5f};
        dwarf.startingAreaId = 1;
        dwarf.spawnX = 50;
        dwarf.spawnY = 1;
        dwarf.spawnZ = 50;
        dwarf.startingItems = {1, 13}; // Health Potion, Iron Ore
        dwarf.allowedClasses = {ClassId::Warrior, ClassId::Healer, ClassId::Engineer};
        m_races.push_back(dwarf);

        // Orc — high strength and stamina, lower intellect
        RaceDef orc;
        orc.id = RaceId::Orc;
        orc.name = "Orc";
        orc.description = "Powerful and battle-hardened, orcs thrive in combat and the wilds";
        orc.lore = "Once feared raiders, the orcs now seek honor through arena combat in the Battleground.";
        orc.baseStats = {110, 30, 120, 15, 10, 6, 8, 5.2f};
        orc.startingAreaId = 2;
        orc.spawnX = 600;
        orc.spawnY = 1;
        orc.spawnZ = -50;
        orc.startingItems = {1, 19}; // Health Potion, Raw Meat
        orc.allowedClasses = {ClassId::Warrior, ClassId::Ranger, ClassId::Engineer};
        m_races.push_back(orc);
    }

    void MMOCharacterSystem::RegisterDefaultClasses()
    {
        m_classes.clear();

        // Warrior — tank/melee DPS
        ClassDef warrior;
        warrior.id = ClassId::Warrior;
        warrior.name = "Warrior";
        warrior.description = "Frontline fighter wielding heavy weapons and thick armor";
        warrior.role = "Tank/DPS";
        warrior.statModifiers = {20, -10, 15, 5, 0, -3, 10, 0};
        warrior.startingItems = {30}; // Iron Sword
        warrior.defaultPartyRole = PartyRole::Tank;
        warrior.iconName = "icon_warrior";
        m_classes.push_back(warrior);

        // Ranger — ranged DPS, tracking
        ClassDef ranger;
        ranger.id = ClassId::Ranger;
        ranger.name = "Ranger";
        ranger.description = "Skilled marksman and tracker, deadly at range";
        ranger.role = "DPS";
        ranger.statModifiers = {0, 0, 5, 0, 8, 2, 0, 0.5f};
        ranger.startingItems = {};
        ranger.defaultPartyRole = PartyRole::DPS;
        ranger.iconName = "icon_ranger";
        m_classes.push_back(ranger);

        // Mage — spell DPS, crowd control
        ClassDef mage;
        mage.id = ClassId::Mage;
        mage.name = "Mage";
        mage.description = "Master of arcane forces, devastating area damage";
        mage.role = "DPS";
        mage.statModifiers = {-15, 30, -10, -3, 0, 10, -5, 0};
        mage.startingItems = {2}; // Mana Elixir
        mage.defaultPartyRole = PartyRole::DPS;
        mage.iconName = "icon_mage";
        m_classes.push_back(mage);

        // Healer — support, healing
        ClassDef healer;
        healer.id = ClassId::Healer;
        healer.name = "Healer";
        healer.description = "Devoted to restoring health and protecting allies";
        healer.role = "Healer";
        healer.statModifiers = {-5, 25, 0, -2, 0, 8, 0, 0};
        healer.startingItems = {1, 5};       // Health Potion, Bandage
        healer.startingRecipes = {100, 500}; // Health Potion recipe, Field Bandage recipe
        healer.defaultPartyRole = PartyRole::Healer;
        healer.iconName = "icon_healer";
        m_classes.push_back(healer);

        // Engineer — gadgets, turrets, explosives
        ClassDef engineer;
        engineer.id = ClassId::Engineer;
        engineer.name = "Engineer";
        engineer.description = "Inventive tinkerer deploying turrets, grenades, and gadgets";
        engineer.role = "Support/DPS";
        engineer.statModifiers = {5, 10, 5, 2, 3, 5, 3, 0};
        engineer.startingItems = {16};    // Precision Tool
        engineer.startingRecipes = {300}; // Frag Grenade recipe
        engineer.defaultPartyRole = PartyRole::Support;
        engineer.iconName = "icon_engineer";
        m_classes.push_back(engineer);
    }

    // === Queries ===

    const RaceDef* MMOCharacterSystem::GetRace(RaceId id) const
    {
        for (const auto& race : m_races)
        {
            if (race.id == id)
                return &race;
        }
        return nullptr;
    }

    const ClassDef* MMOCharacterSystem::GetClass(ClassId id) const
    {
        for (const auto& cls : m_classes)
        {
            if (cls.id == id)
                return &cls;
        }
        return nullptr;
    }

    bool MMOCharacterSystem::IsValidCombination(RaceId race, ClassId classId) const
    {
        const auto* raceDef = GetRace(race);
        if (!raceDef)
            return false;
        return Spark::ContainerUtils::Contains(raceDef->allowedClasses, classId);
    }

    // === Character CRUD ===

    bool MMOCharacterSystem::ValidateName(const std::string& name, std::string& errorOut) const
    {
        if (name.size() < MIN_NAME_LENGTH)
        {
            errorOut = "Name must be at least " + std::to_string(MIN_NAME_LENGTH) + " characters";
            return false;
        }
        if (name.size() > MAX_NAME_LENGTH)
        {
            errorOut = "Name cannot exceed " + std::to_string(MAX_NAME_LENGTH) + " characters";
            return false;
        }

        // Must start with uppercase letter
        if (!std::isupper(static_cast<unsigned char>(name[0])))
        {
            errorOut = "Name must start with an uppercase letter";
            return false;
        }

        // Only alphanumeric
        for (char c : name)
        {
            if (!std::isalpha(static_cast<unsigned char>(c)))
            {
                errorOut = "Name may only contain letters";
                return false;
            }
        }

        if (!IsNameAvailable(name))
        {
            errorOut = "Name is already taken";
            return false;
        }

        return true;
    }

    bool MMOCharacterSystem::IsNameAvailable(const std::string& name) const
    {
        for (const auto& [id, ch] : m_characters)
        {
            if (ch.name == name)
                return false;
        }
        return true;
    }

    CharacterCreateResult MMOCharacterSystem::CreateCharacter(uint32_t accountId, const CharacterCreateRequest& request)
    {
        CharacterCreateResult result;

        // Validate name
        if (!ValidateName(request.name, result.errorMessage))
            return result;

        // Validate race/class
        if (!IsValidCombination(request.race, request.classId))
        {
            result.errorMessage = "Invalid race/class combination";
            return result;
        }

        // Check character slot limit (default 4)
        auto& charIds = m_accountCharacters[accountId];
        if (charIds.size() >= 4)
        {
            result.errorMessage = "Maximum character limit reached";
            return result;
        }

        // Create character
        CharacterSummary summary;
        summary.characterId = m_nextCharId++;
        summary.name = request.name;
        summary.race = request.race;
        summary.classId = request.classId;
        summary.level = 1;
        summary.appearance = request.appearance;

        const auto* raceDef = GetRace(request.race);
        if (raceDef)
        {
            summary.areaId = raceDef->startingAreaId;
            summary.areaName = (summary.areaId == 1) ? "TownSquare" : "Wilderness";
        }

        uint32_t id = summary.characterId;
        m_characters[id] = summary;
        charIds.push_back(id);

        result.success = true;
        result.characterId = id;

        const auto* classDef = GetClass(request.classId);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Character created: %s (%s %s)", request.name.c_str(),
                       (raceDef ? raceDef->name : "?").c_str(), (classDef ? classDef->name : "?").c_str());
        return result;
    }

    bool MMOCharacterSystem::DeleteCharacter(uint32_t accountId, uint32_t characterId)
    {
        auto acctIt = m_accountCharacters.find(accountId);
        if (acctIt == m_accountCharacters.end())
            return false;

        auto& charIds = acctIt->second;
        auto it = std::find(charIds.begin(), charIds.end(), characterId);
        if (it == charIds.end())
            return false;

        charIds.erase(it);

        auto charIt = m_characters.find(characterId);
        if (charIt != m_characters.end())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Character deleted: %s", charIt->second.name.c_str());
            m_characters.erase(charIt);
        }
        return true;
    }

    std::vector<CharacterSummary> MMOCharacterSystem::GetCharacters(uint32_t accountId) const
    {
        std::vector<CharacterSummary> result;
        auto it = m_accountCharacters.find(accountId);
        if (it == m_accountCharacters.end())
            return result;

        for (uint32_t charId : it->second)
        {
            auto cit = m_characters.find(charId);
            if (cit != m_characters.end())
                result.push_back(cit->second);
        }
        return result;
    }

    const CharacterSummary* MMOCharacterSystem::GetCharacter(uint32_t characterId) const
    {
        auto it = m_characters.find(characterId);
        return it != m_characters.end() ? &it->second : nullptr;
    }

    // === Stat computation ===

    StatBlock MMOCharacterSystem::ComputeStats(RaceId race, ClassId classId, int level) const
    {
        StatBlock stats = {};
        const auto* raceDef = GetRace(race);
        const auto* classDef = GetClass(classId);

        if (raceDef)
            stats = raceDef->baseStats;

        if (classDef)
        {
            stats.health += classDef->statModifiers.health;
            stats.mana += classDef->statModifiers.mana;
            stats.stamina += classDef->statModifiers.stamina;
            stats.strength += classDef->statModifiers.strength;
            stats.agility += classDef->statModifiers.agility;
            stats.intellect += classDef->statModifiers.intellect;
            stats.armor += classDef->statModifiers.armor;
            stats.moveSpeed += classDef->statModifiers.moveSpeed;
        }

        // Per-level scaling (2% per level after 1)
        float levelMult = 1.0f + (level - 1) * 0.02f;
        stats.health *= levelMult;
        stats.mana *= levelMult;
        stats.stamina *= levelMult;
        stats.strength *= levelMult;
        stats.agility *= levelMult;
        stats.intellect *= levelMult;

        return stats;
    }

    CharacterSaveData MMOCharacterSystem::BuildNewCharacterData(uint32_t accountId,
                                                                const CharacterCreateRequest& req) const
    {
        CharacterSaveData data;
        data.accountId = accountId;
        data.name = req.name;
        data.level = 1;
        data.xp = 0;

        const auto* raceDef = GetRace(req.race);
        if (raceDef)
        {
            data.areaId = raceDef->startingAreaId;
            data.posX = raceDef->spawnX;
            data.posY = raceDef->spawnY;
            data.posZ = raceDef->spawnZ;
        }

        auto stats = ComputeStats(req.race, req.classId, 1);
        data.health = stats.health;
        data.maxHealth = stats.health;
        data.mana = stats.mana;
        data.maxMana = stats.mana;

        return data;
    }

    std::string MMOCharacterSystem::GetCharacterListString(uint32_t accountId) const
    {
        auto chars = GetCharacters(accountId);
        if (chars.empty())
            return "No characters found for account " + std::to_string(accountId);

        std::ostringstream ss;
        ss << "Characters for account " << accountId << ":\n";
        for (const auto& ch : chars)
        {
            const auto* raceDef = GetRace(ch.race);
            const auto* classDef = GetClass(ch.classId);
            ss << "  [" << ch.characterId << "] " << ch.name << " - Lv" << ch.level << " "
               << (raceDef ? raceDef->name : "?") << " " << (classDef ? classDef->name : "?") << " @ " << ch.areaName
               << "\n";
        }
        return ss.str();
    }

    void MMOCharacterSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Character System"))
        {
            ImGui::Text("Races: %zu | Classes: %zu | Characters: %zu", m_races.size(), m_classes.size(),
                        m_characters.size());

            if (ImGui::TreeNode("Races"))
            {
                for (const auto& race : m_races)
                {
                    if (ImGui::TreeNode(race.name.c_str()))
                    {
                        ImGui::Text("%s", race.description.c_str());
                        ImGui::Text("HP:%.0f MP:%.0f STR:%.0f AGI:%.0f INT:%.0f", race.baseStats.health,
                                    race.baseStats.mana, race.baseStats.strength, race.baseStats.agility,
                                    race.baseStats.intellect);
                        ImGui::Text("Start Area: %u (%.0f, %.0f, %.0f)", race.startingAreaId, race.spawnX, race.spawnY,
                                    race.spawnZ);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Classes"))
            {
                for (const auto& cls : m_classes)
                {
                    if (ImGui::TreeNode(cls.name.c_str()))
                    {
                        ImGui::Text("Role: %s", cls.role.c_str());
                        ImGui::Text("%s", cls.description.c_str());
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
