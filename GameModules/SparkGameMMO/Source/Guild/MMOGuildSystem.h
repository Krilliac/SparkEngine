/**
 * @file MMOGuildSystem.h
 * @brief Guild creation, rank management, permissions, and guild bank
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages guild lifecycle, member management with rank-based permissions,
 * message-of-the-day, guild bank deposits/withdrawals, and activity logging.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief A guild member entry
    struct GuildMember
    {
        uint32_t playerId = 0;
        std::string name;
        GuildRank rank = GuildRank::Initiate;
        int level = 1;
        bool isOnline = false;
        float contribution = 0.0f;
        std::string note;
    };

    /// @brief A guild with members, bank, and permissions
    struct Guild
    {
        uint32_t id = 0;
        std::string name;
        std::string tag;
        std::string motd;
        uint32_t leaderId = 0;
        int bankCurrency = 0;
        int maxMembers = 50;
        std::vector<GuildMember> members;
        std::vector<std::string> log;
        std::unordered_map<int, GuildPermission> rankPermissions;

        int GetMemberCount() const { return static_cast<int>(members.size()); }
        bool IsFull() const { return GetMemberCount() >= maxMembers; }
    };

    /// @brief Per-player guild state
    struct GuildMembership
    {
        uint32_t guildId = 0;
        GuildRank rank = GuildRank::Initiate;
        bool InGuild() const { return guildId != 0; }
    };

    /**
     * @brief Guild management system
     */
    class MMOGuildSystem
    {
      public:
        MMOGuildSystem() = default;
        ~MMOGuildSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        // === Guild Lifecycle ===
        uint32_t CreateGuild(const std::string& name, const std::string& tag, uint32_t founderId,
                             const std::string& founderName);
        bool DisbandGuild(uint32_t guildId, uint32_t requesterId);

        // === Member Management ===
        bool InviteMember(uint32_t guildId, uint32_t inviterId, uint32_t newMemberId, const std::string& newMemberName);
        bool RemoveMember(uint32_t guildId, uint32_t requesterId, uint32_t targetId);
        bool PromoteMember(uint32_t guildId, uint32_t requesterId, uint32_t targetId);
        bool DemoteMember(uint32_t guildId, uint32_t requesterId, uint32_t targetId);
        bool LeaveGuild(uint32_t guildId, uint32_t playerId);

        // === Guild Data ===
        bool SetMotd(uint32_t guildId, uint32_t requesterId, const std::string& motd);
        bool DepositCurrency(uint32_t guildId, int amount);
        bool WithdrawCurrency(uint32_t guildId, uint32_t requesterId, int amount);

        // === Queries ===
        const Guild* GetGuild(uint32_t guildId) const;
        const Guild* FindByName(const std::string& name) const;
        bool HasPerm(uint32_t guildId, uint32_t playerId, GuildPermission perm) const;
        size_t GetGuildCount() const { return m_guilds.size(); }

        std::string GetGuildInfoString(uint32_t guildId) const;
        std::string GetGuildListString() const;

      private:
        Guild* GetGuildMut(uint32_t guildId);
        void SetDefaultPermissions(Guild& guild);
        GuildPermission GetRankPerms(const Guild& guild, GuildRank rank) const;
        const GuildMember* FindMember(const Guild& guild, uint32_t playerId) const;

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, Guild> m_guilds;
        uint32_t m_nextGuildId = 1;

        static constexpr int MAX_GUILDS = 100;
        static constexpr int MAX_LOG = 200;
    };

} // namespace MMO
