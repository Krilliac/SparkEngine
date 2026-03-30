/**
 * @file MMOWorldBossSystem.cpp
 * @brief World boss definitions, phase management, contribution tracking
 */

#include "MMOWorldBossSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace MMO
{

    bool MMOWorldBossSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultBosses();
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] World boss system initialized (" +
                                                    std::to_string(m_bossDefs.size()) + " bosses)");
        return true;
    }

    void MMOWorldBossSystem::Shutdown()
    {
        m_bossDefs.clear();
        m_instances.clear();
    }

    void MMOWorldBossSystem::RegisterDefaultBosses()
    {
        WorldBossDef ancientGolem;
        ancientGolem.id = 1;
        ancientGolem.name = "Ancient Golem";
        ancientGolem.title = "Guardian of the Wilderness";
        ancientGolem.description = "A massive stone construct that patrols the wilderness frontier";
        ancientGolem.spawnX = 2500.0f;
        ancientGolem.spawnY = 10.0f;
        ancientGolem.spawnZ = 0.0f;
        ancientGolem.maxHealth = 500000.0f;
        ancientGolem.baseDamage = 150.0f;
        ancientGolem.detectionRange = 80.0f;
        ancientGolem.minPlayersForLoot = 5;
        ancientGolem.spawnCooldown = 7200.0f; // 2 hours
        ancientGolem.lootTable = "boss_loot";
        ancientGolem.transitions = {
            {BossPhase::Phase1, BossPhase::Transition, 0.5f, "The Ancient Golem crumbles and reforms!", 4.0f},
            {BossPhase::Phase2, BossPhase::Transition, 0.2f, "The Ancient Golem enters a frenzy!", 3.0f},
        };
        ancientGolem.abilities = {
            {"Ground Slam", 25.0f, 0.0f, 800.0f, 20.0f, BossPhase::Phase1, "The ground trembles!", 3.0f},
            {"Boulder Toss", 15.0f, 0.0f, 400.0f, 10.0f, BossPhase::Phase1, "", 0.0f},
            {"Earthquake", 40.0f, 0.0f, 1200.0f, 30.0f, BossPhase::Phase2, "The earth splits apart!", 4.0f},
            {"Stone Rain", 20.0f, 0.0f, 600.0f, 25.0f, BossPhase::Phase2, "Rocks fall from above!", 2.0f},
        };
        RegisterBoss(ancientGolem);

        WorldBossDef voidDragon;
        voidDragon.id = 2;
        voidDragon.name = "Void Dragon";
        voidDragon.title = "Herald of the Abyss";
        voidDragon.description = "A dragon corrupted by void energy that emerges from dimensional rifts";
        voidDragon.spawnX = -500.0f;
        voidDragon.spawnY = 50.0f;
        voidDragon.spawnZ = 3000.0f;
        voidDragon.maxHealth = 800000.0f;
        voidDragon.baseDamage = 250.0f;
        voidDragon.detectionRange = 120.0f;
        voidDragon.minPlayersForLoot = 10;
        voidDragon.spawnCooldown = 14400.0f; // 4 hours
        voidDragon.lootTable = "boss_loot";
        voidDragon.transitions = {
            {BossPhase::Phase1, BossPhase::Transition, 0.7f, "The Void Dragon takes flight!", 5.0f},
            {BossPhase::Phase2, BossPhase::Transition, 0.4f, "Dimensional rifts open everywhere!", 4.0f},
            {BossPhase::Phase3, BossPhase::Transition, 0.15f, "The Void Dragon channels ultimate power!", 3.0f},
        };
        voidDragon.abilities = {
            {"Void Breath", 20.0f, 0.0f, 1000.0f, 15.0f, BossPhase::Phase1, "The dragon inhales deeply!", 3.0f},
            {"Tail Sweep", 10.0f, 0.0f, 500.0f, 12.0f, BossPhase::Phase1, "", 0.0f},
            {"Dimensional Rift", 30.0f, 0.0f, 1500.0f, 20.0f, BossPhase::Phase2, "A rift tears open!", 4.0f},
            {"Void Nova", 45.0f, 0.0f, 2000.0f, 35.0f, BossPhase::Phase3, "Reality itself shatters!", 5.0f},
        };
        RegisterBoss(voidDragon);
    }

    void MMOWorldBossSystem::RegisterBoss(const WorldBossDef& def)
    {
        m_bossDefs[def.id] = def;
        // Start dormant with spawn timer
        WorldBossInstance inst;
        inst.bossDefId = def.id;
        inst.currentPhase = BossPhase::Dormant;
        inst.maxHealth = def.maxHealth;
        inst.spawnTimer = def.spawnCooldown;
        m_instances[def.id] = inst;
    }

    const WorldBossDef* MMOWorldBossSystem::GetBossDef(uint32_t id) const
    {
        auto it = m_bossDefs.find(id);
        return it != m_bossDefs.end() ? &it->second : nullptr;
    }

    bool MMOWorldBossSystem::SpawnBoss(uint32_t bossDefId)
    {
        const auto* def = GetBossDef(bossDefId);
        if (!def)
            return false;

        auto& inst = m_instances[bossDefId];
        if (inst.IsAlive())
            return false;

        inst.bossDefId = bossDefId;
        inst.currentPhase = BossPhase::Phase1;
        inst.currentHealth = def->maxHealth;
        inst.maxHealth = def->maxHealth;
        inst.spawnTimer = 0.0f;
        inst.phaseTimer = 0.0f;
        inst.enrageTimer = ENRAGE_TIME;
        inst.encounterDuration = 0.0f;
        inst.activeAbilities.clear();
        inst.contributions.clear();

        // Copy abilities for the current phase
        for (const auto& ab : def->abilities)
        {
            if (ab.activeInPhase == BossPhase::Phase1)
            {
                inst.activeAbilities.push_back(ab);
                inst.activeAbilities.back().cooldownRemaining = ab.cooldown * 0.5f; // Start at half CD
            }
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] WORLD BOSS SPAWNED: " + def->name + " - " + def->title);
        return true;
    }

    bool MMOWorldBossSystem::DamageBoss(uint32_t bossDefId, uint32_t attackerId, const std::string& attackerName,
                                        float damage)
    {
        auto it = m_instances.find(bossDefId);
        if (it == m_instances.end() || !it->second.IsAlive())
            return false;

        auto& inst = it->second;
        inst.currentHealth -= damage;

        // Track contribution
        auto& contrib = inst.contributions[attackerId];
        contrib.playerId = attackerId;
        contrib.playerName = attackerName;
        contrib.damageDealt += damage;

        // Check death
        if (inst.currentHealth <= 0.0f)
        {
            inst.currentHealth = 0.0f;
            const auto* def = GetBossDef(bossDefId);
            if (def)
                DefeatBoss(inst, *def);
            return true;
        }

        // Check phase transitions
        const auto* def = GetBossDef(bossDefId);
        if (def)
            UpdatePhases(inst, *def);

        return true;
    }

    void MMOWorldBossSystem::RecordHealing(uint32_t bossDefId, uint32_t healerId, const std::string& healerName,
                                           float amount)
    {
        auto it = m_instances.find(bossDefId);
        if (it == m_instances.end() || !it->second.IsAlive())
            return;

        auto& contrib = it->second.contributions[healerId];
        contrib.playerId = healerId;
        contrib.playerName = healerName;
        contrib.healingDone += amount;
    }

    void MMOWorldBossSystem::ResetBoss(uint32_t bossDefId)
    {
        auto it = m_instances.find(bossDefId);
        if (it == m_instances.end())
            return;

        const auto* def = GetBossDef(bossDefId);
        auto& inst = it->second;
        inst.currentPhase = BossPhase::Dormant;
        inst.currentHealth = 0.0f;
        inst.spawnTimer = def ? def->spawnCooldown : 3600.0f;
        inst.contributions.clear();
        inst.activeAbilities.clear();

        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] World boss reset: " + (def ? def->name : "Unknown"));
    }

    void MMOWorldBossSystem::UpdatePhases(WorldBossInstance& inst, const WorldBossDef& def)
    {
        float healthPct = inst.GetHealthPct();

        for (const auto& trans : def.transitions)
        {
            if (inst.currentPhase == trans.from && healthPct <= trans.healthThreshold)
            {
                inst.currentPhase = trans.to;
                inst.phaseTimer = trans.transitionDuration;

                if (!trans.announcement.empty())
                {
                    Spark::SimpleConsole::GetInstance().LogInfo("[MMO] " + trans.announcement);
                }

                // Transition completes → move to next phase
                // Map transition destinations
                if (trans.from == BossPhase::Phase1)
                    inst.currentPhase = BossPhase::Phase2;
                else if (trans.from == BossPhase::Phase2)
                    inst.currentPhase = BossPhase::Phase3;

                // Update active abilities for new phase
                inst.activeAbilities.clear();
                for (const auto& ab : def.abilities)
                {
                    if (ab.activeInPhase == inst.currentPhase || ab.activeInPhase == BossPhase::Phase1)
                    {
                        inst.activeAbilities.push_back(ab);
                        inst.activeAbilities.back().cooldownRemaining = ab.cooldown * 0.3f;
                    }
                }
                break;
            }
        }
    }

    void MMOWorldBossSystem::UpdateAbilities(WorldBossInstance& inst, float dt)
    {
        for (auto& ab : inst.activeAbilities)
        {
            ab.cooldownRemaining -= dt;
            if (ab.cooldownRemaining <= 0.0f)
            {
                ab.cooldownRemaining = ab.cooldown;
                // Log ability usage
                if (!ab.warningMessage.empty())
                {
                    Spark::SimpleConsole::GetInstance().LogInfo("[MMO] " + ab.warningMessage);
                }
            }
        }
    }

    void MMOWorldBossSystem::DefeatBoss(WorldBossInstance& inst, const WorldBossDef& def)
    {
        inst.currentPhase = BossPhase::Defeated;
        inst.spawnTimer = def.spawnCooldown;
        CalculateContributions(inst);

        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] WORLD BOSS DEFEATED: " + def.name + " (" +
                                                    std::to_string(inst.GetParticipantCount()) + " participants)");
    }

    void MMOWorldBossSystem::CalculateContributions(WorldBossInstance& inst)
    {
        float totalDamage = 0.0f;
        for (auto& [id, c] : inst.contributions)
            totalDamage += c.damageDealt;

        if (totalDamage <= 0.0f)
            return;

        for (auto& [id, c] : inst.contributions)
        {
            float damagePct = c.damageDealt / totalDamage;
            float healPct = c.healingDone / std::max(totalDamage, 1.0f);
            c.score = damagePct * 70.0f + healPct * 30.0f;
        }
    }

    void MMOWorldBossSystem::Update(float dt)
    {
        for (auto& [id, inst] : m_instances)
        {
            if (inst.currentPhase == BossPhase::Dormant)
            {
                inst.spawnTimer -= dt;
                // Auto-spawn when timer expires
                if (inst.spawnTimer <= 0.0f)
                    SpawnBoss(id);
                continue;
            }

            if (inst.currentPhase == BossPhase::Defeated)
            {
                inst.spawnTimer -= dt;
                if (inst.spawnTimer <= 0.0f)
                {
                    inst.currentPhase = BossPhase::Dormant;
                    const auto* def = GetBossDef(id);
                    inst.spawnTimer = def ? def->spawnCooldown : 3600.0f;
                }
                continue;
            }

            if (!inst.IsAlive())
                continue;

            inst.encounterDuration += dt;
            inst.enrageTimer -= dt;

            // Update ability cooldowns
            UpdateAbilities(inst, dt);

            // Update participation time for all contributors
            for (auto& [pid, c] : inst.contributions)
                c.participationTime += dt;

            // Enrage: double damage after timer expires
            if (inst.enrageTimer <= 0.0f && inst.enrageTimer > -dt)
            {
                Spark::SimpleConsole::GetInstance().LogInfo("[MMO] World boss has enraged!");
            }

            // Reset if no participants for 60 seconds
            if (inst.contributions.empty() && inst.encounterDuration > 60.0f)
                ResetBoss(id);
        }
    }

    const WorldBossInstance* MMOWorldBossSystem::GetBossInstance(uint32_t bossDefId) const
    {
        auto it = m_instances.find(bossDefId);
        return it != m_instances.end() ? &it->second : nullptr;
    }

    std::vector<BossContribution> MMOWorldBossSystem::GetContributionRanking(uint32_t bossDefId) const
    {
        std::vector<BossContribution> result;
        auto it = m_instances.find(bossDefId);
        if (it == m_instances.end())
            return result;

        for (const auto& [id, c] : it->second.contributions)
            result.push_back(c);

        std::sort(result.begin(), result.end(),
                  [](const BossContribution& a, const BossContribution& b) { return a.score > b.score; });
        return result;
    }

    bool MMOWorldBossSystem::QualifiesForLoot(uint32_t bossDefId, uint32_t playerId) const
    {
        auto it = m_instances.find(bossDefId);
        if (it == m_instances.end())
            return false;

        auto cit = it->second.contributions.find(playerId);
        if (cit == it->second.contributions.end())
            return false;

        float totalDamage = 0.0f;
        for (const auto& [id, c] : it->second.contributions)
            totalDamage += c.damageDealt;

        if (totalDamage <= 0.0f)
            return false;

        return (cit->second.damageDealt / totalDamage) >= MIN_CONTRIBUTION_PCT;
    }

    bool MMOWorldBossSystem::IsBossActive(uint32_t bossDefId) const
    {
        auto it = m_instances.find(bossDefId);
        return it != m_instances.end() && it->second.IsAlive();
    }

    BossPhase MMOWorldBossSystem::GetBossPhase(uint32_t bossDefId) const
    {
        auto it = m_instances.find(bossDefId);
        return it != m_instances.end() ? it->second.currentPhase : BossPhase::Dormant;
    }

    std::string MMOWorldBossSystem::GetBossStatusString() const
    {
        std::ostringstream ss;
        ss << "World Bosses:\n";
        for (const auto& [id, inst] : m_instances)
        {
            const auto* def = GetBossDef(id);
            std::string name = def ? def->name : ("Boss#" + std::to_string(id));
            ss << "  " << name << ": ";

            if (inst.currentPhase == BossPhase::Dormant)
                ss << "Dormant (spawns in " << static_cast<int>(inst.spawnTimer) << "s)";
            else if (inst.currentPhase == BossPhase::Defeated)
                ss << "Defeated (respawns in " << static_cast<int>(inst.spawnTimer) << "s)";
            else
                ss << "ACTIVE Phase" << static_cast<int>(inst.currentPhase)
                   << " HP:" << static_cast<int>(inst.GetHealthPct() * 100) << "%" << " (" << inst.GetParticipantCount()
                   << " players)";
            ss << "\n";
        }
        return ss.str();
    }

    void MMOWorldBossSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO World Boss System"))
        {
            for (const auto& [id, inst] : m_instances)
            {
                const auto* def = GetBossDef(id);
                std::string name = def ? (def->name + " - " + def->title) : "Unknown";
                if (ImGui::TreeNode(name.c_str()))
                {
                    ImGui::Text("Phase: %d", static_cast<int>(inst.currentPhase));
                    if (inst.IsAlive())
                    {
                        ImGui::ProgressBar(inst.GetHealthPct(), ImVec2(-1, 0), "HP");
                        ImGui::Text("Participants: %d", inst.GetParticipantCount());
                        ImGui::Text("Duration: %.0fs", inst.encounterDuration);
                        ImGui::Text("Enrage in: %.0fs", std::max(0.0f, inst.enrageTimer));
                    }
                    else
                    {
                        ImGui::Text("Spawn in: %.0fs", inst.spawnTimer);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
