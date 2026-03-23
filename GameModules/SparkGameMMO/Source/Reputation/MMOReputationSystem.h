/**
 * @file MMOReputationSystem.h
 * @brief Faction reputation tracking with tiered rewards and spillover
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief Reward unlocked at a reputation tier
    struct ReputationReward
    {
        ReputationTier requiredTier = ReputationTier::Friendly;
        std::string description;
        uint32_t itemDefId = 0;
        float vendorDiscount = 0.0f;
    };

    /// @brief Faction definition
    struct FactionDef
    {
        uint32_t id = 0;
        std::string name;
        std::string description;
        int startingRep = 0;
        std::vector<ReputationReward> rewards;
        std::vector<std::pair<uint32_t, float>> allies;  ///< (factionId, mult)
        std::vector<std::pair<uint32_t, float>> enemies; ///< (factionId, mult)
    };

    /// @brief Runtime standing with one faction
    struct FactionStanding
    {
        uint32_t factionId = 0;
        int reputation = 0;
        ReputationTier tier = ReputationTier::Neutral;

        static ReputationTier GetTierForValue(int rep);
        float GetProgressInTier() const;
    };

    /// @brief Per-player reputation state
    struct ReputationState
    {
        std::unordered_map<uint32_t, FactionStanding> standings;
        ReputationTier GetTier(uint32_t factionId) const;
        int GetRep(uint32_t factionId) const;
    };

    /**
     * @brief Faction reputation management with spillover
     */
    class MMOReputationSystem
    {
      public:
        MMOReputationSystem() = default;
        ~MMOReputationSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        void RegisterFaction(const FactionDef& def);
        const FactionDef* GetFaction(uint32_t id) const;

        /// Add rep with spillover to allies/enemies
        void AddReputation(ReputationState& state, uint32_t factionId, int amount);
        void SetReputation(ReputationState& state, uint32_t factionId, int value);
        void InitializePlayerRep(ReputationState& state) const;

        std::vector<const ReputationReward*> GetAvailableRewards(const ReputationState& state,
                                                                 uint32_t factionId) const;
        bool HasReachedTier(const ReputationState& state, uint32_t factionId, ReputationTier tier) const;

        size_t GetFactionCount() const { return m_factions.size(); }
        std::string GetStandingsString(const ReputationState& state) const;

      private:
        void RegisterDefaultFactions();
        static void UpdateTier(FactionStanding& standing);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, FactionDef> m_factions;
    };

} // namespace MMO
