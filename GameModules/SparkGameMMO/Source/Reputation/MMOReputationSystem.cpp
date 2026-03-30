/**
 * @file MMOReputationSystem.cpp
 * @brief Faction definitions, reputation changes with spillover, tier rewards
 */

#include "MMOReputationSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <sstream>

namespace MMO
{

    // Tier thresholds
    static constexpr int TIER_HATED = -6000;
    static constexpr int TIER_HOSTILE = -3000;
    static constexpr int TIER_UNFRIENDLY = -1000;
    static constexpr int TIER_NEUTRAL = 0;
    static constexpr int TIER_FRIENDLY = 3000;
    static constexpr int TIER_HONORED = 9000;
    static constexpr int TIER_REVERED = 21000;

    ReputationTier FactionStanding::GetTierForValue(int rep)
    {
        if (rep >= TIER_REVERED)
            return ReputationTier::Exalted;
        if (rep >= TIER_HONORED)
            return ReputationTier::Revered;
        if (rep >= TIER_FRIENDLY)
            return ReputationTier::Honored;
        if (rep >= TIER_NEUTRAL)
            return ReputationTier::Friendly;
        if (rep >= TIER_UNFRIENDLY)
            return ReputationTier::Neutral;
        if (rep >= TIER_HOSTILE)
            return ReputationTier::Unfriendly;
        if (rep >= TIER_HATED)
            return ReputationTier::Hostile;
        return ReputationTier::Hated;
    }

    float FactionStanding::GetProgressInTier() const
    {
        // Simplified: returns 0..1 progress within current tier bracket
        int tierStart = 0;
        int tierEnd = 3000;
        switch (tier)
        {
        case ReputationTier::Hated:
            tierStart = -9000;
            tierEnd = TIER_HATED;
            break;
        case ReputationTier::Hostile:
            tierStart = TIER_HATED;
            tierEnd = TIER_HOSTILE;
            break;
        case ReputationTier::Unfriendly:
            tierStart = TIER_HOSTILE;
            tierEnd = TIER_UNFRIENDLY;
            break;
        case ReputationTier::Neutral:
            tierStart = TIER_UNFRIENDLY;
            tierEnd = TIER_NEUTRAL;
            break;
        case ReputationTier::Friendly:
            tierStart = TIER_NEUTRAL;
            tierEnd = TIER_FRIENDLY;
            break;
        case ReputationTier::Honored:
            tierStart = TIER_FRIENDLY;
            tierEnd = TIER_HONORED;
            break;
        case ReputationTier::Revered:
            tierStart = TIER_HONORED;
            tierEnd = TIER_REVERED;
            break;
        case ReputationTier::Exalted:
            return 1.0f;
        default:
            break;
        }
        int range = tierEnd - tierStart;
        if (range <= 0)
            return 1.0f;
        return static_cast<float>(reputation - tierStart) / range;
    }

    ReputationTier ReputationState::GetTier(uint32_t factionId) const
    {
        auto it = standings.find(factionId);
        return it != standings.end() ? it->second.tier : ReputationTier::Neutral;
    }

    int ReputationState::GetRep(uint32_t factionId) const
    {
        auto it = standings.find(factionId);
        return it != standings.end() ? it->second.reputation : 0;
    }

    bool MMOReputationSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultFactions();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO reputation system initialized: %zu factions", m_factions.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Reputation system initialized (" +
                                                    std::to_string(m_factions.size()) + " factions)");
        return true;
    }

    void MMOReputationSystem::Shutdown()
    {
        m_factions.clear();
    }

    void MMOReputationSystem::RegisterDefaultFactions()
    {
        FactionDef townGuard;
        townGuard.id = 1;
        townGuard.name = "Town Guard";
        townGuard.description = "Protectors of the TownSquare safe zone";
        townGuard.startingRep = 0;
        townGuard.rewards = {
            {ReputationTier::Friendly, "Access to guard armory", 60, 0.0f},
            {ReputationTier::Honored, "10% vendor discount", 0, 0.1f},
            {ReputationTier::Revered, "Elite guard equipment", 61, 0.15f},
            {ReputationTier::Exalted, "Shadow Cloak", 62, 0.2f},
        };
        townGuard.enemies = {{3, 0.5f}};
        RegisterFaction(townGuard);

        FactionDef wildernessRangers;
        wildernessRangers.id = 2;
        wildernessRangers.name = "Wilderness Rangers";
        wildernessRangers.description = "Scouts and survivalists of the open world";
        wildernessRangers.startingRep = 0;
        wildernessRangers.rewards = {
            {ReputationTier::Friendly, "Ranger camp access", 0, 0.0f},
            {ReputationTier::Honored, "Tracking abilities", 0, 0.05f},
            {ReputationTier::Exalted, "Ranger Commander title", 101, 0.15f},
        };
        wildernessRangers.allies = {{1, 0.25f}};
        RegisterFaction(wildernessRangers);

        FactionDef shadowCult;
        shadowCult.id = 3;
        shadowCult.name = "Shadow Cult";
        shadowCult.description = "Mysterious denizens of the Shadow Crypt";
        shadowCult.startingRep = -3000; // Start hostile
        shadowCult.rewards = {
            {ReputationTier::Neutral, "Cease hostilities", 0, 0.0f},
            {ReputationTier::Friendly, "Shadow Crypt key", 100, 0.0f},
            {ReputationTier::Exalted, "Shadow magic training", 0, 0.0f},
        };
        shadowCult.enemies = {{1, 0.5f}};
        RegisterFaction(shadowCult);

        FactionDef battlegroundFaction;
        battlegroundFaction.id = 4;
        battlegroundFaction.name = "Arena Masters";
        battlegroundFaction.description = "Organizers of the PvP Battleground";
        battlegroundFaction.startingRep = 0;
        battlegroundFaction.rewards = {
            {ReputationTier::Friendly, "Arena weapon cache", 30, 0.0f},
            {ReputationTier::Honored, "Arena armor set", 60, 0.1f},
            {ReputationTier::Exalted, "Gladiator title and exclusive gear", 32, 0.2f},
        };
        RegisterFaction(battlegroundFaction);
    }

    void MMOReputationSystem::RegisterFaction(const FactionDef& def)
    {
        m_factions[def.id] = def;
    }

    const FactionDef* MMOReputationSystem::GetFaction(uint32_t id) const
    {
        auto it = m_factions.find(id);
        return it != m_factions.end() ? &it->second : nullptr;
    }

    void MMOReputationSystem::InitializePlayerRep(ReputationState& state) const
    {
        for (const auto& [id, def] : m_factions)
        {
            FactionStanding standing;
            standing.factionId = id;
            standing.reputation = def.startingRep;
            UpdateTier(standing);
            state.standings[id] = standing;
        }
    }

    void MMOReputationSystem::AddReputation(ReputationState& state, uint32_t factionId, int amount)
    {
        const auto* def = GetFaction(factionId);
        if (!def)
            return;

        auto& standing = state.standings[factionId];
        standing.factionId = factionId;
        ReputationTier oldTier = standing.tier;
        standing.reputation += amount;
        UpdateTier(standing);

        if (standing.tier != oldTier)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "Reputation tier changed: %s -> tier %d (rep=%d)",
                           def->name.c_str(), static_cast<int>(standing.tier), standing.reputation);
            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogInfo("[MMO] Reputation with " + def->name + " changed to " +
                            std::to_string(static_cast<int>(standing.tier)));
        }

        // Spillover to allies
        for (const auto& [allyId, mult] : def->allies)
        {
            auto& allyStanding = state.standings[allyId];
            allyStanding.factionId = allyId;
            allyStanding.reputation += static_cast<int>(amount * mult);
            UpdateTier(allyStanding);
        }

        // Spillover to enemies (negative)
        for (const auto& [enemyId, mult] : def->enemies)
        {
            auto& enemyStanding = state.standings[enemyId];
            enemyStanding.factionId = enemyId;
            enemyStanding.reputation -= static_cast<int>(amount * mult);
            UpdateTier(enemyStanding);
        }
    }

    void MMOReputationSystem::SetReputation(ReputationState& state, uint32_t factionId, int value)
    {
        auto& standing = state.standings[factionId];
        standing.factionId = factionId;
        standing.reputation = value;
        UpdateTier(standing);
    }

    void MMOReputationSystem::UpdateTier(FactionStanding& standing)
    {
        standing.tier = FactionStanding::GetTierForValue(standing.reputation);
    }

    std::vector<const ReputationReward*> MMOReputationSystem::GetAvailableRewards(const ReputationState& state,
                                                                                  uint32_t factionId) const
    {
        std::vector<const ReputationReward*> result;
        const auto* def = GetFaction(factionId);
        if (!def)
            return result;

        ReputationTier currentTier = state.GetTier(factionId);
        for (const auto& reward : def->rewards)
        {
            if (static_cast<int>(currentTier) >= static_cast<int>(reward.requiredTier))
                result.push_back(&reward);
        }
        return result;
    }

    bool MMOReputationSystem::HasReachedTier(const ReputationState& state, uint32_t factionId,
                                             ReputationTier tier) const
    {
        return static_cast<int>(state.GetTier(factionId)) >= static_cast<int>(tier);
    }

    std::string MMOReputationSystem::GetStandingsString(const ReputationState& state) const
    {
        std::ostringstream ss;
        ss << "Faction Standings:\n";
        for (const auto& [id, standing] : state.standings)
        {
            const auto* def = GetFaction(id);
            std::string name = def ? def->name : ("Faction#" + std::to_string(id));
            ss << "  " << name << ": " << standing.reputation << " (Tier " << static_cast<int>(standing.tier) << ")\n";
        }
        return ss.str();
    }

    void MMOReputationSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Reputation System"))
        {
            ImGui::Text("Factions: %zu", m_factions.size());
            for (const auto& [id, def] : m_factions)
            {
                if (ImGui::TreeNode(def.name.c_str()))
                {
                    ImGui::Text("ID: %u", id);
                    ImGui::Text("%s", def.description.c_str());
                    ImGui::Text("Starting Rep: %d", def.startingRep);
                    ImGui::Text("Rewards: %zu", def.rewards.size());
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
