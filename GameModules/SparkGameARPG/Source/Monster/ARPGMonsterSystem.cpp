/**
 * @file ARPGMonsterSystem.cpp
 * @brief Monster templates, spawning, level scaling, and champion affix assignment
 */

#include "ARPGMonsterSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <random>
#include <sstream>

namespace ARPG
{

    static std::mt19937& GetMonsterRNG()
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        return rng;
    }

    bool ARPGMonsterSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterMonsterTemplates();

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Monster system initialized (" +
                                                    std::to_string(m_templates.size()) + " templates)");
        return true;
    }

    void ARPGMonsterSystem::Shutdown()
    {
        m_activeMonsters.clear();
    }

    void ARPGMonsterSystem::Update([[maybe_unused]] float deltaTime)
    {
        // Remove dead monsters (health <= 0)
        std::erase_if(m_activeMonsters, [](const MonsterData& m) { return m.health <= 0.0f; });
    }

    void ARPGMonsterSystem::RegisterMonsterTemplates()
    {
        m_templates.clear();

        m_templates.push_back({"Skeleton", 40.0f, 6.0f, ARPGDamageType::Physical, 3.5f, 15.0f, 0.12f});
        m_templates.push_back({"Zombie", 65.0f, 8.0f, ARPGDamageType::Physical, 2.0f, 18.0f, 0.10f});
        m_templates.push_back({"Fallen Demon", 35.0f, 10.0f, ARPGDamageType::Fire, 4.5f, 20.0f, 0.15f});
        m_templates.push_back({"Spider", 25.0f, 5.0f, ARPGDamageType::Poison, 5.0f, 12.0f, 0.08f});
        m_templates.push_back({"Golem", 120.0f, 15.0f, ARPGDamageType::Physical, 1.5f, 35.0f, 0.20f});
        m_templates.push_back({"Wraith", 30.0f, 12.0f, ARPGDamageType::Cold, 4.0f, 25.0f, 0.18f});
        m_templates.push_back({"Succubus", 45.0f, 14.0f, ARPGDamageType::Arcane, 3.5f, 28.0f, 0.22f});
        m_templates.push_back({"Blood Knight", 80.0f, 18.0f, ARPGDamageType::Physical, 3.0f, 40.0f, 0.25f});
    }

    const MonsterTemplate* ARPGMonsterSystem::FindTemplate(const std::string& name) const
    {
        for (const auto& tmpl : m_templates)
        {
            if (tmpl.name == name)
                return &tmpl;
        }
        return nullptr;
    }

    void ARPGMonsterSystem::ApplyRankModifiers(MonsterData& monster) const
    {
        // Rank multipliers for HP, damage, XP, and loot
        switch (monster.rank)
        {
        case ARPGMonsterRank::Champion:
            monster.maxHealth *= 3.0f;
            monster.damage *= 1.5f;
            monster.xpReward *= 3.0f;
            monster.lootChance *= 2.0f;
            break;
        case ARPGMonsterRank::Elite:
            monster.maxHealth *= 5.0f;
            monster.damage *= 2.0f;
            monster.xpReward *= 5.0f;
            monster.lootChance *= 3.0f;
            break;
        case ARPGMonsterRank::MiniBoss:
            monster.maxHealth *= 10.0f;
            monster.damage *= 3.0f;
            monster.xpReward *= 10.0f;
            monster.lootChance = 1.0f; // Guaranteed drop
            break;
        case ARPGMonsterRank::Boss:
            monster.maxHealth *= 20.0f;
            monster.damage *= 4.0f;
            monster.xpReward *= 20.0f;
            monster.lootChance = 1.0f; // Guaranteed drop
            break;
        default:
            break;
        }

        monster.health = monster.maxHealth;
        monster.lootChance = std::min(monster.lootChance, 1.0f);
    }

    void ARPGMonsterSystem::AssignChampionAffixes(MonsterData& monster, int count) const
    {
        static constexpr int AFFIX_COUNT = static_cast<int>(ChampionAffix::Count);
        std::uniform_int_distribution<int> dist(0, AFFIX_COUNT - 1);

        for (int i = 0; i < count; ++i)
        {
            auto affix = static_cast<ChampionAffix>(dist(GetMonsterRNG()));

            // Avoid duplicate affixes
            bool duplicate = false;
            for (auto existing : monster.affixes)
            {
                if (existing == affix)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;

            monster.affixes.push_back(affix);

            // Apply affix stat modifiers
            switch (affix)
            {
            case ChampionAffix::ExtraFast:
                monster.moveSpeed *= 1.5f;
                break;
            case ChampionAffix::FireEnchanted:
                monster.damageType = ARPGDamageType::Fire;
                monster.damage *= 1.2f;
                break;
            case ChampionAffix::LightningEnchanted:
                monster.damageType = ARPGDamageType::Lightning;
                monster.damage *= 1.15f;
                break;
            case ChampionAffix::Frozen:
                monster.damageType = ARPGDamageType::Cold;
                break;
            case ChampionAffix::Vampiric:
                monster.maxHealth *= 1.3f;
                monster.health = monster.maxHealth;
                break;
            case ChampionAffix::Plagued:
                monster.damageType = ARPGDamageType::Poison;
                break;
            case ChampionAffix::Shielding:
                monster.maxHealth *= 1.2f;
                monster.health = monster.maxHealth;
                break;
            default:
                break;
            }
        }
    }

    MonsterData ARPGMonsterSystem::SpawnMonster(const std::string& templateName, int level, ARPGMonsterRank rank)
    {
        const auto* tmpl = FindTemplate(templateName);
        if (!tmpl)
        {
            // Fallback to first template if not found
            if (m_templates.empty())
                return {};
            tmpl = &m_templates[0];
        }

        MonsterData monster;
        monster.monsterId = m_nextMonsterId++;
        monster.name = tmpl->name;
        monster.rank = rank;
        monster.level = level;
        monster.damageType = tmpl->damageType;
        monster.moveSpeed = tmpl->baseMoveSpeed;
        monster.lootChance = tmpl->baseLootChance;

        // Scale base stats with level
        float levelScale = 1.0f + static_cast<float>(level - 1) * 0.15f;
        monster.maxHealth = tmpl->baseHealth * levelScale;
        monster.damage = tmpl->baseDamage * levelScale;
        monster.xpReward = tmpl->baseXpReward * levelScale;

        // Apply rank multipliers
        ApplyRankModifiers(monster);

        // Champions get 1-3 affixes, elites get 2-3
        if (rank == ARPGMonsterRank::Champion)
        {
            std::uniform_int_distribution<int> dist(1, 2);
            AssignChampionAffixes(monster, dist(GetMonsterRNG()));
        }
        else if (rank == ARPGMonsterRank::Elite)
        {
            std::uniform_int_distribution<int> dist(2, 3);
            AssignChampionAffixes(monster, dist(GetMonsterRNG()));
        }

        m_activeMonsters.push_back(monster);
        return monster;
    }

    std::vector<MonsterData> ARPGMonsterSystem::SpawnElitePack(int level, int packSize)
    {
        // Pick a random template for the pack
        std::uniform_int_distribution<size_t> dist(0, m_templates.size() - 1);
        const auto& tmpl = m_templates[dist(GetMonsterRNG())];

        std::vector<MonsterData> pack;

        // Leader is Elite, rest are Champions
        pack.push_back(SpawnMonster(tmpl.name, level, ARPGMonsterRank::Elite));
        for (int i = 1; i < packSize; ++i)
        {
            pack.push_back(SpawnMonster(tmpl.name, level, ARPGMonsterRank::Champion));
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Spawned elite pack: " + tmpl.name + " x" +
                                                    std::to_string(packSize));
        return pack;
    }

    MonsterData ARPGMonsterSystem::SpawnBoss(int level)
    {
        // Bosses use unique names
        static const char* bossNames[] = {"Andariel", "Duriel", "Mephisto", "Diablo", "Baal"};
        std::uniform_int_distribution<int> dist(0, 4);
        int bossIndex = dist(GetMonsterRNG());

        MonsterData boss = SpawnMonster("Blood Knight", level, ARPGMonsterRank::Boss);
        boss.name = bossNames[bossIndex];

        // Bosses always get 3 affixes
        AssignChampionAffixes(boss, 3);

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Boss spawned: " + boss.name + " (Lv" +
                                                    std::to_string(level) + ")");
        return boss;
    }

    std::string ARPGMonsterSystem::GetMonsterListString() const
    {
        static const char* affixNames[] = {
            "Extra Fast", "Fire Enchanted", "Lightning Enchanted", "Frozen", "Teleporter",
            "Vampiric",   "Plagued",        "Shielding",           "Arcane", "Molten"};

        std::ostringstream ss;
        ss << "=== ARPG Monster Templates ===\n";
        for (const auto& tmpl : m_templates)
        {
            ss << tmpl.name << " - HP:" << tmpl.baseHealth << " DMG:" << tmpl.baseDamage
               << " Speed:" << tmpl.baseMoveSpeed << " XP:" << tmpl.baseXpReward
               << " Loot:" << static_cast<int>(tmpl.baseLootChance * 100.0f) << "%\n";
        }

        if (!m_activeMonsters.empty())
        {
            ss << "\n=== Active Monsters (" << m_activeMonsters.size() << ") ===\n";
            for (const auto& mon : m_activeMonsters)
            {
                ss << mon.name << " Lv" << mon.level << " HP:" << mon.health << "/" << mon.maxHealth;
                if (!mon.affixes.empty())
                {
                    ss << " [";
                    for (size_t i = 0; i < mon.affixes.size(); ++i)
                    {
                        if (i > 0)
                            ss << ", ";
                        auto idx = static_cast<size_t>(mon.affixes[i]);
                        ss << ((idx < 10) ? affixNames[idx] : "?");
                    }
                    ss << "]";
                }
                ss << "\n";
            }
        }
        return ss.str();
    }

    void ARPGMonsterSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("ARPG Monster System"))
        {
            ImGui::Text("Templates: %zu | Active: %zu", m_templates.size(), m_activeMonsters.size());

            if (ImGui::TreeNode("Templates"))
            {
                for (const auto& tmpl : m_templates)
                {
                    ImGui::Text("%s - HP:%.0f DMG:%.0f SPD:%.1f", tmpl.name.c_str(), tmpl.baseHealth, tmpl.baseDamage,
                                tmpl.baseMoveSpeed);
                }
                ImGui::TreePop();
            }

            if (!m_activeMonsters.empty() && ImGui::TreeNode("Active"))
            {
                for (const auto& mon : m_activeMonsters)
                {
                    ImGui::Text("%s Lv%d HP:%.0f/%.0f", mon.name.c_str(), mon.level, mon.health, mon.maxHealth);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace ARPG
