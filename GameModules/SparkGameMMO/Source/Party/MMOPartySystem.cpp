/**
 * @file MMOPartySystem.cpp
 * @brief Party formation, role assignment, ready checks, and loot rules
 */

#include "MMOPartySystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace MMO
{

    bool MMOPartySystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO party system initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Party system initialized");
        return true;
    }

    void MMOPartySystem::Update(float dt)
    {
        for (auto& [id, party] : m_parties)
        {
            if (party.state == PartyState::ReadyCheck)
            {
                party.readyCheckTimer -= dt;
                if (party.readyCheckTimer <= 0.0f)
                {
                    party.state = PartyState::Idle;
                    // Reset ready flags
                    for (auto& m : party.members)
                        m.isReady = false;
                }
            }
        }
    }

    void MMOPartySystem::Shutdown()
    {
        m_parties.clear();
    }

    uint32_t MMOPartySystem::CreateParty(uint32_t leaderId, const std::string& leaderName)
    {
        Party party;
        party.partyId = m_nextPartyId++;
        party.leaderId = leaderId;

        PartyMember leader;
        leader.playerId = leaderId;
        leader.name = leaderName;
        leader.isLeader = true;
        party.members.push_back(leader);

        uint32_t id = party.partyId;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Party created: ID %u, leader=%s", id, leaderName.c_str());
        m_parties[id] = std::move(party);
        return id;
    }

    bool MMOPartySystem::DisbandParty(uint32_t partyId, uint32_t requesterId)
    {
        auto* party = GetPartyMut(partyId);
        if (!party || party->leaderId != requesterId)
            return false;
        m_parties.erase(partyId);
        return true;
    }

    bool MMOPartySystem::InviteMember(uint32_t partyId, uint32_t inviterId, uint32_t newId, const std::string& newName)
    {
        auto* party = GetPartyMut(partyId);
        if (!party || party->IsFull())
            return false;
        if (!IsLeader(partyId, inviterId))
            return false;

        // Check not already in party
        for (const auto& m : party->members)
        {
            if (m.playerId == newId)
                return false;
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Party member joined: %s (party %u)", newName.c_str(), partyId);
        PartyMember member;
        member.playerId = newId;
        member.name = newName;
        party->members.push_back(member);
        return true;
    }

    bool MMOPartySystem::RemoveMember(uint32_t partyId, uint32_t requesterId, uint32_t targetId)
    {
        auto* party = GetPartyMut(partyId);
        if (!party || !IsLeader(partyId, requesterId) || targetId == party->leaderId)
            return false;

        auto it = std::find_if(party->members.begin(), party->members.end(),
                               [targetId](const PartyMember& m) { return m.playerId == targetId; });
        if (it == party->members.end())
            return false;

        party->members.erase(it);
        return true;
    }

    bool MMOPartySystem::LeaveParty(uint32_t partyId, uint32_t playerId)
    {
        auto* party = GetPartyMut(partyId);
        if (!party)
            return false;

        auto it = std::find_if(party->members.begin(), party->members.end(),
                               [playerId](const PartyMember& m) { return m.playerId == playerId; });
        if (it == party->members.end())
            return false;

        bool wasLeader = (playerId == party->leaderId);
        party->members.erase(it);

        if (party->members.empty())
        {
            m_parties.erase(partyId);
            return true;
        }

        // Transfer leadership if leader left
        if (wasLeader)
        {
            party->leaderId = party->members.front().playerId;
            party->members.front().isLeader = true;
        }
        return true;
    }

    bool MMOPartySystem::TransferLeader(uint32_t partyId, uint32_t currentLeaderId, uint32_t newLeaderId)
    {
        auto* party = GetPartyMut(partyId);
        if (!party || party->leaderId != currentLeaderId)
            return false;

        auto* newLeader = FindMember(*party, newLeaderId);
        if (!newLeader)
            return false;

        // Clear old leader flag
        for (auto& m : party->members)
            m.isLeader = false;

        party->leaderId = newLeaderId;
        newLeader->isLeader = true;
        return true;
    }

    bool MMOPartySystem::SetRole(uint32_t partyId, uint32_t playerId, PartyRole role)
    {
        auto* party = GetPartyMut(partyId);
        if (!party)
            return false;
        auto* member = FindMember(*party, playerId);
        if (!member)
            return false;
        member->role = role;
        return true;
    }

    bool MMOPartySystem::SetLootRule(uint32_t partyId, uint32_t requesterId, LootRule rule)
    {
        auto* party = GetPartyMut(partyId);
        if (!party || party->leaderId != requesterId)
            return false;
        party->lootRule = rule;
        return true;
    }

    bool MMOPartySystem::StartReadyCheck(uint32_t partyId, uint32_t requesterId)
    {
        auto* party = GetPartyMut(partyId);
        if (!party || party->leaderId != requesterId)
            return false;
        if (party->state == PartyState::ReadyCheck)
            return false;

        party->state = PartyState::ReadyCheck;
        party->readyCheckTimer = READY_CHECK_TIMEOUT;
        for (auto& m : party->members)
            m.isReady = false;
        return true;
    }

    bool MMOPartySystem::RespondReady(uint32_t partyId, uint32_t playerId, bool ready)
    {
        auto* party = GetPartyMut(partyId);
        if (!party || party->state != PartyState::ReadyCheck)
            return false;

        auto* member = FindMember(*party, playerId);
        if (!member)
            return false;
        member->isReady = ready;

        // Check if all responded
        bool allReady = true;
        for (const auto& m : party->members)
        {
            if (!m.isReady)
            {
                allReady = false;
                break;
            }
        }
        if (allReady)
            party->state = PartyState::Idle;

        return true;
    }

    void MMOPartySystem::UpdateMemberHealth(uint32_t partyId, uint32_t playerId, float healthPct)
    {
        auto* party = GetPartyMut(partyId);
        if (!party)
            return;
        auto* member = FindMember(*party, playerId);
        if (member)
            member->healthPercent = healthPct;
    }

    const Party* MMOPartySystem::GetParty(uint32_t partyId) const
    {
        auto it = m_parties.find(partyId);
        return it != m_parties.end() ? &it->second : nullptr;
    }

    bool MMOPartySystem::IsLeader(uint32_t partyId, uint32_t playerId) const
    {
        const auto* party = GetParty(partyId);
        return party && party->leaderId == playerId;
    }

    Party* MMOPartySystem::GetPartyMut(uint32_t partyId)
    {
        auto it = m_parties.find(partyId);
        return it != m_parties.end() ? &it->second : nullptr;
    }

    PartyMember* MMOPartySystem::FindMember(Party& party, uint32_t playerId)
    {
        for (auto& m : party.members)
        {
            if (m.playerId == playerId)
                return &m;
        }
        return nullptr;
    }

    std::string MMOPartySystem::GetPartyInfoString(uint32_t partyId) const
    {
        const auto* party = GetParty(partyId);
        if (!party)
            return "Party not found";
        std::ostringstream ss;
        ss << "Party [" << partyId << "] (" << party->GetMemberCount() << "/" << Party::MAX_SIZE << "):\n";
        for (const auto& m : party->members)
        {
            ss << "  " << m.name << " (Lv" << m.level << ") " << (m.isLeader ? "[Leader]" : "")
               << " HP:" << static_cast<int>(m.healthPercent * 100) << "%\n";
        }
        return ss.str();
    }

    void MMOPartySystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Party System"))
        {
            ImGui::Text("Active Parties: %zu", m_parties.size());
            for (const auto& [id, party] : m_parties)
            {
                std::string label = "Party #" + std::to_string(id);
                if (ImGui::TreeNode(label.c_str()))
                {
                    ImGui::Text("Members: %d/%d", party.GetMemberCount(), Party::MAX_SIZE);
                    for (const auto& m : party.members)
                    {
                        ImGui::Text("  %s%s Lv%d HP:%.0f%%", m.isLeader ? "[L] " : "    ", m.name.c_str(), m.level,
                                    m.healthPercent * 100);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
