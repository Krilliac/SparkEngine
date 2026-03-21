/**
 * @file MMODungeonSystem.cpp
 * @brief Dungeon definitions, instance management, boss encounters, lockouts
 */

#include "MMODungeonSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace MMO
{

    bool DungeonInstance::AllBossesDefeated() const
    {
        for (const auto& b : bosses)
        {
            if (!b.defeated)
                return false;
        }
        return !bosses.empty();
    }

    bool MMODungeonSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultDungeons();
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Dungeon system initialized (" +
                                                    std::to_string(m_dungeons.size()) + " dungeons)");
        return true;
    }

    void MMODungeonSystem::Shutdown()
    {
        m_dungeons.clear();
        m_instances.clear();
    }

    void MMODungeonSystem::RegisterDefaultDungeons()
    {
        DungeonDef shadowCrypt;
        shadowCrypt.id = 1;
        shadowCrypt.name = "Shadow Crypt";
        shadowCrypt.description = "An ancient crypt infested with shadow creatures";
        shadowCrypt.scenePath = "Assets/Scenes/shadow_crypt.scene";
        shadowCrypt.minLevel = 10;
        shadowCrypt.recommendedPlayers = 5;
        shadowCrypt.bosses = {
            {1, "Crypt Keeper", 15000.0f, 15000.0f, 300, false, 0},
            {2, "Shadow Lord", 30000.0f, 30000.0f, 600, false, 0},
        };
        RegisterDungeon(shadowCrypt);

        DungeonDef forgottenMine;
        forgottenMine.id = 2;
        forgottenMine.name = "Forgotten Mine";
        forgottenMine.description = "Collapsed mine tunnels with dangerous machinery";
        forgottenMine.scenePath = "Assets/Scenes/forgotten_mine.scene";
        forgottenMine.minLevel = 5;
        forgottenMine.recommendedPlayers = 3;
        forgottenMine.maxPlayers = 3;
        forgottenMine.bosses = {
            {3, "Mine Foreman", 8000.0f, 8000.0f, 240, false, 0},
        };
        RegisterDungeon(forgottenMine);

        DungeonDef voidSpire;
        voidSpire.id = 3;
        voidSpire.name = "Void Spire";
        voidSpire.description = "A floating tower piercing the void between dimensions";
        voidSpire.scenePath = "Assets/Scenes/void_spire.scene";
        voidSpire.minLevel = 20;
        voidSpire.recommendedPlayers = 5;
        voidSpire.bosses = {
            {4, "Void Sentinel", 25000.0f, 25000.0f, 480, false, 0},
            {5, "Void Weaver", 20000.0f, 20000.0f, 420, false, 0},
            {6, "The Architect", 50000.0f, 50000.0f, 900, false, 0},
        };
        RegisterDungeon(voidSpire);
    }

    void MMODungeonSystem::RegisterDungeon(const DungeonDef& def)
    {
        m_dungeons[def.id] = def;
    }

    const DungeonDef* MMODungeonSystem::GetDungeon(uint32_t id) const
    {
        auto it = m_dungeons.find(id);
        return it != m_dungeons.end() ? &it->second : nullptr;
    }

    uint32_t MMODungeonSystem::CreateInstance(uint32_t dungeonDefId, DungeonDifficulty diff,
                                              const std::vector<uint32_t>& playerIds)
    {
        if (static_cast<int>(m_instances.size()) >= MAX_INSTANCES)
            return 0;

        const auto* def = GetDungeon(dungeonDefId);
        if (!def)
            return 0;

        DungeonInstance inst;
        inst.instanceId = m_nextInstanceId++;
        inst.dungeonDefId = dungeonDefId;
        inst.difficulty = diff;
        inst.state = InstanceState::Active;
        inst.bosses = def->bosses;

        // Scale boss health by difficulty
        float healthMult = GetHealthMult(diff, dungeonDefId);
        for (auto& boss : inst.bosses)
        {
            boss.maxHealth *= healthMult;
            boss.currentHealth = boss.maxHealth;
        }

        for (uint32_t pid : playerIds)
            inst.playerIds.insert(pid);

        uint32_t id = inst.instanceId;
        m_instances[id] = std::move(inst);

        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Created dungeon instance: " + def->name + " (ID " +
                                                    std::to_string(id) + ")");
        return id;
    }

    bool MMODungeonSystem::EnterInstance(uint32_t instanceId, uint32_t playerId)
    {
        auto it = m_instances.find(instanceId);
        if (it == m_instances.end() || it->second.state != InstanceState::Active)
            return false;
        it->second.playerIds.insert(playerId);
        return true;
    }

    bool MMODungeonSystem::LeaveInstance(uint32_t instanceId, uint32_t playerId)
    {
        auto it = m_instances.find(instanceId);
        if (it == m_instances.end())
            return false;
        it->second.playerIds.erase(playerId);
        return true;
    }

    bool MMODungeonSystem::DefeatBoss(uint32_t instanceId, uint32_t bossId)
    {
        auto it = m_instances.find(instanceId);
        if (it == m_instances.end())
            return false;

        for (auto& boss : it->second.bosses)
        {
            if (boss.id == bossId && !boss.defeated)
            {
                boss.defeated = true;
                boss.currentHealth = 0.0f;

                Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Boss defeated: " + boss.name);

                if (it->second.AllBossesDefeated())
                    it->second.state = InstanceState::Completed;
                return true;
            }
        }
        return false;
    }

    void MMODungeonSystem::RecordWipe(uint32_t instanceId)
    {
        auto it = m_instances.find(instanceId);
        if (it != m_instances.end())
        {
            it->second.wipeCount++;
            // Reset boss health on wipe
            const auto* def = GetDungeon(it->second.dungeonDefId);
            if (def)
            {
                float mult = GetHealthMult(it->second.difficulty, it->second.dungeonDefId);
                for (size_t i = 0; i < it->second.bosses.size() && i < def->bosses.size(); ++i)
                {
                    if (!it->second.bosses[i].defeated)
                    {
                        it->second.bosses[i].currentHealth = def->bosses[i].maxHealth * mult;
                    }
                }
            }
        }
    }

    void MMODungeonSystem::DestroyInstance(uint32_t instanceId)
    {
        m_instances.erase(instanceId);
    }

    const DungeonInstance* MMODungeonSystem::GetInstance(uint32_t instanceId) const
    {
        auto it = m_instances.find(instanceId);
        return it != m_instances.end() ? &it->second : nullptr;
    }

    void MMODungeonSystem::Update(float dt)
    {
        for (auto it = m_instances.begin(); it != m_instances.end();)
        {
            auto& inst = it->second;
            inst.elapsed += dt;

            if (inst.elapsed >= inst.maxDuration && inst.state == InstanceState::Active)
            {
                inst.state = InstanceState::Expired;
                it = m_instances.erase(it);
                continue;
            }

            if (inst.playerIds.empty() && inst.state == InstanceState::Active)
            {
                it = m_instances.erase(it);
                continue;
            }

            ++it;
        }
    }

    bool MMODungeonSystem::HasLockout(const DungeonPlayerState& ps, uint32_t dungeonId, DungeonDifficulty diff) const
    {
        for (const auto& lo : ps.lockouts)
        {
            if (lo.dungeonDefId == dungeonId && lo.difficulty == diff && lo.remaining > 0)
                return true;
        }
        return false;
    }

    void MMODungeonSystem::AddBossLockout(DungeonPlayerState& ps, uint32_t dungeonId, DungeonDifficulty diff,
                                          uint32_t bossId, float duration)
    {
        for (auto& lo : ps.lockouts)
        {
            if (lo.dungeonDefId == dungeonId && lo.difficulty == diff)
            {
                lo.defeatedBossIds.insert(bossId);
                lo.remaining = std::max(lo.remaining, duration);
                return;
            }
        }
        LootLockout lo;
        lo.dungeonDefId = dungeonId;
        lo.difficulty = diff;
        lo.defeatedBossIds.insert(bossId);
        lo.remaining = duration;
        ps.lockouts.push_back(lo);
    }

    void MMODungeonSystem::UpdateLockouts(DungeonPlayerState& ps, float dt)
    {
        for (auto it = ps.lockouts.begin(); it != ps.lockouts.end();)
        {
            it->remaining -= dt;
            if (it->remaining <= 0.0f)
                it = ps.lockouts.erase(it);
            else
                ++it;
        }
    }

    float MMODungeonSystem::GetHealthMult(DungeonDifficulty diff, uint32_t dungeonId) const
    {
        const auto* def = GetDungeon(dungeonId);
        if (!def)
            return 1.0f;
        switch (diff)
        {
        case DungeonDifficulty::Heroic:
            return def->heroicHealthMult;
        case DungeonDifficulty::Mythic:
            return def->mythicHealthMult;
        case DungeonDifficulty::Legendary:
            return def->legendaryHealthMult;
        default:
            return 1.0f;
        }
    }

    std::string MMODungeonSystem::GetDungeonListString() const
    {
        std::ostringstream ss;
        ss << "Dungeons (" << m_dungeons.size() << "):\n";
        for (const auto& [id, d] : m_dungeons)
        {
            ss << "  [" << id << "] " << d.name << " (Lv" << d.minLevel << ", " << d.recommendedPlayers << "p, "
               << d.bosses.size() << " bosses)\n";
        }
        ss << "Active Instances: " << m_instances.size() << "\n";
        return ss.str();
    }

    std::string MMODungeonSystem::GetInstanceInfoString(uint32_t instanceId) const
    {
        const auto* inst = GetInstance(instanceId);
        if (!inst)
            return "Instance not found";
        const auto* def = GetDungeon(inst->dungeonDefId);
        std::ostringstream ss;
        ss << "Instance #" << instanceId << " - " << (def ? def->name : "Unknown") << "\n";
        ss << "Players: " << inst->playerIds.size() << " | Wipes: " << inst->wipeCount << "\n";
        for (const auto& b : inst->bosses)
        {
            ss << "  " << b.name << ": "
               << (b.defeated ? "DEFEATED" : std::to_string(static_cast<int>(b.GetHealthPct() * 100)) + "% HP") << "\n";
        }
        return ss.str();
    }

    void MMODungeonSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Dungeon System"))
        {
            ImGui::Text("Dungeons: %zu | Active Instances: %zu", m_dungeons.size(), m_instances.size());
            for (const auto& [id, d] : m_dungeons)
                ImGui::Text("  [%u] %s - Lv%d, %zup, %zu bosses", id, d.name.c_str(), d.minLevel, d.recommendedPlayers,
                            d.bosses.size());
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
