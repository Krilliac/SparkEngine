/**
 * @file MMOPartySystem.h
 * @brief Party/group system for cooperative MMO content
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages party formation, role assignment, ready checks,
 * loot distribution rules, and group-wide health tracking.
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

    /// @brief A member within a party
    struct PartyMember
    {
        uint32_t playerId = 0;
        std::string name;
        PartyRole role = PartyRole::None;
        int level = 1;
        float healthPercent = 1.0f;
        bool isOnline = true;
        bool isReady = false;
        bool isLeader = false;
    };

    /// @brief Party state
    enum class PartyState : uint8_t
    {
        Idle,
        ReadyCheck,
        InDungeon,
        Queued
    };

    /// @brief A party/group of players
    struct Party
    {
        uint32_t partyId = 0;
        uint32_t leaderId = 0;
        std::vector<PartyMember> members;
        LootRule lootRule = LootRule::FreeForAll;
        PartyState state = PartyState::Idle;
        float readyCheckTimer = 0.0f;

        int GetMemberCount() const { return static_cast<int>(members.size()); }
        bool IsFull() const { return GetMemberCount() >= MAX_SIZE; }
        static constexpr int MAX_SIZE = 5;
    };

    /**
     * @brief Party and group management
     */
    class MMOPartySystem
    {
      public:
        MMOPartySystem() = default;
        ~MMOPartySystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float dt);
        void Shutdown();
        void RenderDebugUI();

        // === Party Lifecycle ===
        uint32_t CreateParty(uint32_t leaderId, const std::string& leaderName);
        bool DisbandParty(uint32_t partyId, uint32_t requesterId);

        // === Members ===
        bool InviteMember(uint32_t partyId, uint32_t inviterId, uint32_t newId, const std::string& newName);
        bool RemoveMember(uint32_t partyId, uint32_t requesterId, uint32_t targetId);
        bool LeaveParty(uint32_t partyId, uint32_t playerId);
        bool TransferLeader(uint32_t partyId, uint32_t currentLeaderId, uint32_t newLeaderId);

        // === Roles ===
        bool SetRole(uint32_t partyId, uint32_t playerId, PartyRole role);
        bool SetLootRule(uint32_t partyId, uint32_t requesterId, LootRule rule);

        // === Ready Check ===
        bool StartReadyCheck(uint32_t partyId, uint32_t requesterId);
        bool RespondReady(uint32_t partyId, uint32_t playerId, bool ready);

        // === Health Updates ===
        void UpdateMemberHealth(uint32_t partyId, uint32_t playerId, float healthPct);

        // === Queries ===
        const Party* GetParty(uint32_t partyId) const;
        bool IsLeader(uint32_t partyId, uint32_t playerId) const;
        size_t GetPartyCount() const { return m_parties.size(); }

        std::string GetPartyInfoString(uint32_t partyId) const;

      private:
        Party* GetPartyMut(uint32_t partyId);
        PartyMember* FindMember(Party& party, uint32_t playerId);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, Party> m_parties;
        uint32_t m_nextPartyId = 1;

        static constexpr float READY_CHECK_TIMEOUT = 30.0f;
    };

} // namespace MMO
