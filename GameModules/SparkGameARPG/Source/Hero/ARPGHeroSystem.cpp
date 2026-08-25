/**
 * @file ARPGHeroSystem.cpp
 * @brief Hero class definitions, stat growth, leveling, and attribute allocation
 */

#include "ARPGHeroSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ARPG
{

    bool ARPGHeroSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterClassTemplates();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG hero system initialized with %zu classes", m_classDefs.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Hero system initialized (" +
                                                    std::to_string(m_classDefs.size()) + " classes)");
        return true;
    }

    void ARPGHeroSystem::Shutdown()
    {
        m_heroes.clear();
    }

    void ARPGHeroSystem::Update(float deltaTime)
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        for (auto& [id, hero] : m_heroes)
        {
            (void)id;
            if (hero.health <= 0.0f)
                continue;

            hero.health = std::min(hero.maxHealth, hero.health + hero.maxHealth * 0.01f * deltaTime);
            hero.mana = std::min(hero.maxMana, hero.mana + hero.maxMana * 0.05f * deltaTime);
        }
    }

    void ARPGHeroSystem::RegisterClassTemplates()
    {
        m_classDefs.clear();

        // Barbarian — raw physical power, high health
        HeroClassDef barbarian;
        barbarian.id = ARPGHeroClass::Barbarian;
        barbarian.name = "Barbarian";
        barbarian.description = "A savage warrior of unmatched strength and fury";
        barbarian.baseStrength = 30.0f;
        barbarian.baseDexterity = 20.0f;
        barbarian.baseIntelligence = 10.0f;
        barbarian.baseVitality = 25.0f;
        barbarian.baseHealth = 200.0f;
        barbarian.baseMana = 40.0f;
        barbarian.healthPerLevel = 18.0f;
        barbarian.manaPerLevel = 2.0f;
        barbarian.baseMoveSpeed = 5.5f;
        barbarian.growth = {2.5f, 1.0f, 0.5f, 2.0f};
        m_classDefs.push_back(barbarian);

        // Sorceress — elemental magic, high mana
        HeroClassDef sorceress;
        sorceress.id = ARPGHeroClass::Sorceress;
        sorceress.name = "Sorceress";
        sorceress.description = "A master of elemental magic wielding fire, cold, and lightning";
        sorceress.baseStrength = 10.0f;
        sorceress.baseDexterity = 15.0f;
        sorceress.baseIntelligence = 35.0f;
        sorceress.baseVitality = 10.0f;
        sorceress.baseHealth = 100.0f;
        sorceress.baseMana = 150.0f;
        sorceress.healthPerLevel = 8.0f;
        sorceress.manaPerLevel = 12.0f;
        sorceress.baseMoveSpeed = 4.5f;
        sorceress.growth = {0.5f, 1.0f, 3.0f, 1.0f};
        m_classDefs.push_back(sorceress);

        // Necromancer — summons and dark magic
        HeroClassDef necromancer;
        necromancer.id = ARPGHeroClass::Necromancer;
        necromancer.name = "Necromancer";
        necromancer.description = "A dark summoner commanding undead legions and poison";
        necromancer.baseStrength = 15.0f;
        necromancer.baseDexterity = 15.0f;
        necromancer.baseIntelligence = 30.0f;
        necromancer.baseVitality = 15.0f;
        necromancer.baseHealth = 120.0f;
        necromancer.baseMana = 120.0f;
        necromancer.healthPerLevel = 10.0f;
        necromancer.manaPerLevel = 10.0f;
        necromancer.baseMoveSpeed = 4.5f;
        necromancer.growth = {1.0f, 1.0f, 2.5f, 1.5f};
        m_classDefs.push_back(necromancer);

        // Paladin — holy warrior, balanced stats
        HeroClassDef paladin;
        paladin.id = ARPGHeroClass::Paladin;
        paladin.name = "Paladin";
        paladin.description = "A holy warrior wielding divine auras and righteous combat";
        paladin.baseStrength = 25.0f;
        paladin.baseDexterity = 15.0f;
        paladin.baseIntelligence = 15.0f;
        paladin.baseVitality = 25.0f;
        paladin.baseHealth = 170.0f;
        paladin.baseMana = 80.0f;
        paladin.healthPerLevel = 14.0f;
        paladin.manaPerLevel = 6.0f;
        paladin.baseMoveSpeed = 5.0f;
        paladin.growth = {2.0f, 1.0f, 1.5f, 2.0f};
        m_classDefs.push_back(paladin);

        // Amazon — ranged and javelin attacks
        HeroClassDef amazon;
        amazon.id = ARPGHeroClass::Amazon;
        amazon.name = "Amazon";
        amazon.description = "A swift warrior specializing in javelin and bow attacks";
        amazon.baseStrength = 20.0f;
        amazon.baseDexterity = 30.0f;
        amazon.baseIntelligence = 10.0f;
        amazon.baseVitality = 20.0f;
        amazon.baseHealth = 140.0f;
        amazon.baseMana = 60.0f;
        amazon.healthPerLevel = 12.0f;
        amazon.manaPerLevel = 5.0f;
        amazon.baseMoveSpeed = 6.0f;
        amazon.growth = {1.5f, 2.5f, 0.5f, 1.5f};
        m_classDefs.push_back(amazon);

        // Rogue — stealth, traps, and critical strikes
        HeroClassDef rogue;
        rogue.id = ARPGHeroClass::Rogue;
        rogue.name = "Rogue";
        rogue.description = "A nimble assassin striking from shadows with lethal precision";
        rogue.baseStrength = 15.0f;
        rogue.baseDexterity = 30.0f;
        rogue.baseIntelligence = 15.0f;
        rogue.baseVitality = 15.0f;
        rogue.baseHealth = 120.0f;
        rogue.baseMana = 70.0f;
        rogue.healthPerLevel = 10.0f;
        rogue.manaPerLevel = 6.0f;
        rogue.baseMoveSpeed = 6.5f;
        rogue.growth = {1.0f, 2.5f, 1.5f, 1.0f};
        m_classDefs.push_back(rogue);
    }

    // === Class queries ===

    const HeroClassDef* ARPGHeroSystem::GetClassDef(ARPGHeroClass id) const
    {
        for (const auto& cls : m_classDefs)
        {
            if (cls.id == id)
                return &cls;
        }
        return nullptr;
    }

    std::string ARPGHeroSystem::GetHeroListString() const
    {
        std::ostringstream ss;
        ss << "=== ARPG Hero Classes ===\n";
        for (const auto& cls : m_classDefs)
        {
            ss << cls.name << " - " << cls.description << "\n";
            ss << "  STR:" << cls.baseStrength << " DEX:" << cls.baseDexterity << " INT:" << cls.baseIntelligence
               << " VIT:" << cls.baseVitality << "\n";
            ss << "  HP:" << cls.baseHealth << "(+" << cls.healthPerLevel << "/lv)" << " MP:" << cls.baseMana << "(+"
               << cls.manaPerLevel << "/lv)" << " Speed:" << cls.baseMoveSpeed << "\n";
        }

        if (!m_heroes.empty())
        {
            ss << "\n=== Active Heroes ===\n";
            for (const auto& [id, hero] : m_heroes)
            {
                ss << hero.name << " [" << id << "] Lv" << hero.level << " XP:" << hero.experience << "/"
                   << hero.xpToNextLevel << " HP:" << hero.health << "/" << hero.maxHealth << " MP:" << hero.mana << "/"
                   << hero.maxMana << "\n";
            }
        }
        return ss.str();
    }

    // === Hero CRUD ===

    uint32_t ARPGHeroSystem::CreateHero(const std::string& name, ARPGHeroClass heroClass)
    {
        const auto* classDef = GetClassDef(heroClass);
        if (!classDef)
            return 0;

        HeroData hero;
        hero.heroId = m_nextHeroId++;
        hero.name = name;
        hero.heroClass = heroClass;
        hero.level = 1;
        hero.experience = 0;
        hero.xpToNextLevel = CalculateXPForLevel(2);
        hero.strength = classDef->baseStrength;
        hero.dexterity = classDef->baseDexterity;
        hero.intelligence = classDef->baseIntelligence;
        hero.vitality = classDef->baseVitality;
        hero.maxHealth = classDef->baseHealth;
        hero.health = classDef->baseHealth;
        hero.maxMana = classDef->baseMana;
        hero.mana = classDef->baseMana;
        hero.moveSpeed = classDef->baseMoveSpeed;
        hero.freeAttributePoints = 0;

        uint32_t id = hero.heroId;
        m_heroes[id] = hero;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG hero created: %s (%s)", name.c_str(), classDef->name.c_str());
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Hero created: " + name + " (" + classDef->name + ")");
        return id;
    }

    HeroData* ARPGHeroSystem::GetHero(uint32_t heroId)
    {
        auto it = m_heroes.find(heroId);
        return it != m_heroes.end() ? &it->second : nullptr;
    }

    const HeroData* ARPGHeroSystem::GetHero(uint32_t heroId) const
    {
        auto it = m_heroes.find(heroId);
        return it != m_heroes.end() ? &it->second : nullptr;
    }

    // === Leveling ===

    uint32_t ARPGHeroSystem::CalculateXPForLevel(int level) const
    {
        // Diablo-style exponential curve: 100 * level^2
        return static_cast<uint32_t>(100.0f * std::pow(static_cast<float>(level), 2.0f));
    }

    void ARPGHeroSystem::GainExperience(uint32_t heroId, uint32_t amount)
    {
        auto* hero = GetHero(heroId);
        if (!hero || hero->level >= MAX_LEVEL)
            return;

        hero->experience += amount;

        while (hero->experience >= hero->xpToNextLevel && hero->level < MAX_LEVEL)
        {
            hero->experience -= hero->xpToNextLevel;
            LevelUp(*hero);
        }
    }

    void ARPGHeroSystem::LevelUp(HeroData& hero)
    {
        hero.level++;
        hero.freeAttributePoints += ATTRIBUTE_POINTS_PER_LEVEL;
        hero.xpToNextLevel = CalculateXPForLevel(hero.level + 1);

        const auto* classDef = GetClassDef(hero.heroClass);
        if (classDef)
        {
            hero.strength += classDef->growth.strength;
            hero.dexterity += classDef->growth.dexterity;
            hero.intelligence += classDef->growth.intelligence;
            hero.vitality += classDef->growth.vitality;

            hero.maxHealth += classDef->healthPerLevel;
            hero.maxMana += classDef->manaPerLevel;

            // Full restore on level up
            hero.health = hero.maxHealth;
            hero.mana = hero.maxMana;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG %s reached level %d", hero.name.c_str(), hero.level);
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] " + hero.name + " reached level " +
                                                    std::to_string(hero.level));
    }

    void ARPGHeroSystem::AllocateAttribute(uint32_t heroId, const std::string& attribute)
    {
        auto* hero = GetHero(heroId);
        if (!hero || hero->freeAttributePoints <= 0)
            return;

        if (attribute == "strength")
            hero->strength += 1.0f;
        else if (attribute == "dexterity")
            hero->dexterity += 1.0f;
        else if (attribute == "intelligence")
            hero->intelligence += 1.0f;
        else if (attribute == "vitality")
        {
            hero->vitality += 1.0f;
            hero->maxHealth += 4.0f; // Vitality grants bonus HP
        }
        else
            return;

        hero->freeAttributePoints--;
    }

    void ARPGHeroSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("ARPG Hero System"))
        {
            ImGui::Text("Classes: %zu | Heroes: %zu", m_classDefs.size(), m_heroes.size());

            if (ImGui::TreeNode("Hero Classes"))
            {
                for (const auto& cls : m_classDefs)
                {
                    if (ImGui::TreeNode(cls.name.c_str()))
                    {
                        ImGui::Text("%s", cls.description.c_str());
                        ImGui::Text("STR:%.0f DEX:%.0f INT:%.0f VIT:%.0f", cls.baseStrength, cls.baseDexterity,
                                    cls.baseIntelligence, cls.baseVitality);
                        ImGui::Text("HP:%.0f (+%.0f/lv) MP:%.0f (+%.0f/lv)", cls.baseHealth, cls.healthPerLevel,
                                    cls.baseMana, cls.manaPerLevel);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Active Heroes"))
            {
                for (const auto& [id, hero] : m_heroes)
                {
                    if (ImGui::TreeNode(hero.name.c_str()))
                    {
                        ImGui::Text("Level: %d | XP: %u/%u", hero.level, hero.experience, hero.xpToNextLevel);
                        ImGui::Text("HP: %.0f/%.0f | MP: %.0f/%.0f", hero.health, hero.maxHealth, hero.mana,
                                    hero.maxMana);
                        ImGui::Text("Free points: %d", hero.freeAttributePoints);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace ARPG
