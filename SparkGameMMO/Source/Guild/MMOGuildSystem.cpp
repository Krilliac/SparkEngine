/**
 * @file MMOGuildSystem.cpp
 * @brief Guild creation, membership, permissions, and bank operations
 */

#include "MMOGuildSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace MMO
{

    bool MMOGuildSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Guild system initialized");
        return true;
    }

    void MMOGuildSystem::Shutdown()
    {
        m_guilds.clear();
    }

    void MMOGuildSystem::SetDefaultPermissions(Guild& guild)
    {
        guild.rankPermissions[static_cast<int>(GuildRank::Initiate)] = GuildPermission::None;
        guild.rankPermissions[static_cast<int>(GuildRank::Member)] = GuildPermission::None;
        guild.rankPermissions[static_cast<int>(GuildRank::Veteran)] = GuildPermission::OfficerChat;
        guild.rankPermissions[static_cast<int>(GuildRank::Officer)] =
            GuildPermission::Invite | GuildPermission::Kick | GuildPermission::EditMotd | GuildPermission::OfficerChat |
            GuildPermission::StartEvent;
        guild.rankPermissions[static_cast<int>(GuildRank::Leader)] = GuildPermission::All;
    }

    GuildPermission MMOGuildSystem::GetRankPerms(const Guild& guild, GuildRank rank) const
    {
        auto it = guild.rankPermissions.find(static_cast<int>(rank));
        return it != guild.rankPermissions.end() ? it->second : GuildPermission::None;
    }

    const GuildMember* MMOGuildSystem::FindMember(const Guild& guild, uint32_t playerId) const
    {
        for (const auto& m : guild.members)
        {
            if (m.playerId == playerId)
                return &m;
        }
        return nullptr;
    }

    Guild* MMOGuildSystem::GetGuildMut(uint32_t guildId)
    {
        auto it = m_guilds.find(guildId);
        return it != m_guilds.end() ? &it->second : nullptr;
    }

    uint32_t MMOGuildSystem::CreateGuild(const std::string& name, const std::string& tag, uint32_t founderId,
                                         const std::string& founderName)
    {
        if (static_cast<int>(m_guilds.size()) >= MAX_GUILDS || FindByName(name))
            return 0;

        Guild guild;
        guild.id = m_nextGuildId++;
        guild.name = name;
        guild.tag = tag;
        guild.leaderId = founderId;
        guild.motd = "Welcome to " + name + "!";
        SetDefaultPermissions(guild);

        GuildMember founder;
        founder.playerId = founderId;
        founder.name = founderName;
        founder.rank = GuildRank::Leader;
        founder.isOnline = true;
        guild.members.push_back(founder);
        guild.log.push_back(founderName + " founded the guild");

        uint32_t guildId = guild.id;
        m_guilds[guildId] = std::move(guild);
        return guildId;
    }

    bool MMOGuildSystem::DisbandGuild(uint32_t guildId, uint32_t requesterId)
    {
        auto it = m_guilds.find(guildId);
        if (it == m_guilds.end() || it->second.leaderId != requesterId)
            return false;
        m_guilds.erase(it);
        return true;
    }

    bool MMOGuildSystem::InviteMember(uint32_t guildId, uint32_t inviterId, uint32_t newMemberId,
                                      const std::string& newMemberName)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild || guild->IsFull() || !HasPerm(guildId, inviterId, GuildPermission::Invite))
            return false;

        if (FindMember(*guild, newMemberId))
            return false;

        GuildMember m;
        m.playerId = newMemberId;
        m.name = newMemberName;
        m.rank = GuildRank::Initiate;
        m.isOnline = true;
        guild->members.push_back(m);
        guild->log.push_back(newMemberName + " joined");
        return true;
    }

    bool MMOGuildSystem::RemoveMember(uint32_t guildId, uint32_t requesterId, uint32_t targetId)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild || targetId == guild->leaderId || !HasPerm(guildId, requesterId, GuildPermission::Kick))
            return false;

        auto it = std::find_if(guild->members.begin(), guild->members.end(),
                               [targetId](const GuildMember& m) { return m.playerId == targetId; });
        if (it == guild->members.end())
            return false;

        guild->log.push_back(it->name + " was removed");
        guild->members.erase(it);
        return true;
    }

    bool MMOGuildSystem::PromoteMember(uint32_t guildId, uint32_t requesterId, uint32_t targetId)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild || !HasPerm(guildId, requesterId, GuildPermission::Promote))
            return false;

        for (auto& m : guild->members)
        {
            if (m.playerId == targetId && static_cast<int>(m.rank) < static_cast<int>(GuildRank::Officer))
            {
                m.rank = static_cast<GuildRank>(static_cast<int>(m.rank) + 1);
                guild->log.push_back(m.name + " promoted to rank " + std::to_string(static_cast<int>(m.rank)));
                return true;
            }
        }
        return false;
    }

    bool MMOGuildSystem::DemoteMember(uint32_t guildId, uint32_t requesterId, uint32_t targetId)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild || targetId == guild->leaderId || !HasPerm(guildId, requesterId, GuildPermission::Promote))
            return false;

        for (auto& m : guild->members)
        {
            if (m.playerId == targetId && static_cast<int>(m.rank) > 0)
            {
                m.rank = static_cast<GuildRank>(static_cast<int>(m.rank) - 1);
                guild->log.push_back(m.name + " demoted");
                return true;
            }
        }
        return false;
    }

    bool MMOGuildSystem::LeaveGuild(uint32_t guildId, uint32_t playerId)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild)
            return false;

        if (playerId == guild->leaderId && guild->members.size() > 1)
            return false; // Must transfer leadership first

        auto it = std::find_if(guild->members.begin(), guild->members.end(),
                               [playerId](const GuildMember& m) { return m.playerId == playerId; });
        if (it == guild->members.end())
            return false;

        guild->log.push_back(it->name + " left");
        guild->members.erase(it);

        if (guild->members.empty())
            m_guilds.erase(guildId);

        return true;
    }

    bool MMOGuildSystem::SetMotd(uint32_t guildId, uint32_t requesterId, const std::string& motd)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild || !HasPerm(guildId, requesterId, GuildPermission::EditMotd))
            return false;
        guild->motd = motd;
        return true;
    }

    bool MMOGuildSystem::DepositCurrency(uint32_t guildId, int amount)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild || amount <= 0)
            return false;
        guild->bankCurrency += amount;
        return true;
    }

    bool MMOGuildSystem::WithdrawCurrency(uint32_t guildId, uint32_t requesterId, int amount)
    {
        auto* guild = GetGuildMut(guildId);
        if (!guild || amount <= 0 || !HasPerm(guildId, requesterId, GuildPermission::WithdrawBank))
            return false;
        if (guild->bankCurrency < amount)
            return false;
        guild->bankCurrency -= amount;
        return true;
    }

    const Guild* MMOGuildSystem::GetGuild(uint32_t guildId) const
    {
        auto it = m_guilds.find(guildId);
        return it != m_guilds.end() ? &it->second : nullptr;
    }

    const Guild* MMOGuildSystem::FindByName(const std::string& name) const
    {
        for (const auto& [id, g] : m_guilds)
        {
            if (g.name == name)
                return &g;
        }
        return nullptr;
    }

    bool MMOGuildSystem::HasPerm(uint32_t guildId, uint32_t playerId, GuildPermission perm) const
    {
        const auto* guild = GetGuild(guildId);
        if (!guild)
            return false;
        const auto* member = FindMember(*guild, playerId);
        if (!member)
            return false;
        return MMO::HasPermission(GetRankPerms(*guild, member->rank), perm);
    }

    std::string MMOGuildSystem::GetGuildInfoString(uint32_t guildId) const
    {
        const auto* g = GetGuild(guildId);
        if (!g)
            return "Guild not found";
        std::ostringstream ss;
        ss << "Guild: " << g->name << " [" << g->tag << "]\n";
        ss << "MOTD: " << g->motd << "\n";
        ss << "Members: " << g->GetMemberCount() << "/" << g->maxMembers << "\n";
        ss << "Bank: " << g->bankCurrency << "g\n";
        for (const auto& m : g->members)
            ss << "  " << m.name << " (Rank " << static_cast<int>(m.rank) << ")\n";
        return ss.str();
    }

    std::string MMOGuildSystem::GetGuildListString() const
    {
        std::ostringstream ss;
        ss << "Guilds (" << m_guilds.size() << "):\n";
        for (const auto& [id, g] : m_guilds)
            ss << "  [" << id << "] " << g.name << " - " << g.GetMemberCount() << " members\n";
        return ss.str();
    }

    void MMOGuildSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Guild System"))
        {
            ImGui::Text("Guilds: %zu", m_guilds.size());
            for (const auto& [id, g] : m_guilds)
            {
                if (ImGui::TreeNode(g.name.c_str()))
                {
                    ImGui::Text("Tag: [%s]", g.tag.c_str());
                    ImGui::Text("Members: %d/%d", g.GetMemberCount(), g.maxMembers);
                    ImGui::Text("Bank: %d", g.bankCurrency);
                    ImGui::Text("MOTD: %s", g.motd.c_str());
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
