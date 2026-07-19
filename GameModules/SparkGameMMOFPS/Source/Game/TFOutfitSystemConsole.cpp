/**
 * @file TFOutfitSystemConsole.cpp
 * @brief TFOutfitSystem console commands (tf_outfit_*): registered from
 *        Initialize (TFRegionSystem pattern — the contended
 *        Console/TFCommands.cpp is never touched). Split from
 *        TFOutfitSystem.cpp; the shared helpers live in
 *        TFOutfitSystemInternal.h.
 */
#include "Game/TFOutfitSystem.h"

#include "Game/TFOutfitSystemInternal.h"
#include "Utils/SparkConsole.h"

namespace Terrafront
{

    using namespace OutfitDetail;

    // ---------------------------------------------------------------------------
    // Console commands (registered from Initialize — TFRegionSystem pattern; the
    // contended Console/TFCommands.cpp is never touched)
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::RegisterConsoleCommands()
    {
        if (m_cmdsRegistered)
            return;
        auto& console = Spark::SimpleConsole::GetInstance();

        auto joinArgs = [](const std::vector<std::string>& args, size_t first, size_t lastExclusive) -> std::string
        {
            std::string out;
            for (size_t i = first; i < lastExclusive && i < args.size(); ++i)
            {
                if (!out.empty())
                    out += ' ';
                out += args[i];
            }
            return out;
        };

        auto memberByName = [this](const std::string& name, uint64_t& outCharId) -> bool
        {
            for (const MirrorMember& m : m_mirror.members)
            {
                if (EqualsNoCase(m.name, name))
                {
                    outCharId = m.charId;
                    return true;
                }
            }
            return false;
        };

        console.RegisterCommand(
            "tf_outfit_create",
            [this, joinArgs](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "usage: tf_outfit_create <name...> <tag>";
                const std::string tag = args.back();
                const std::string name = joinArgs(args, 0, args.size() - 1);
                if (!ValidateOutfitName(name))
                    return std::string("[TF] ") + OutfitResultText(TFOutfitResult::NameInvalid);
                if (!ValidateOutfitTag(tag))
                    return std::string("[TF] ") + OutfitResultText(TFOutfitResult::TagInvalid);
                ClientRequestCreate(name, tag);
                return "[TF] outfit create requested: '" + name + "' [" + tag + "]";
            },
            "Create an outfit: tf_outfit_create <name...> <tag>", "TERRAFRONT", "tf_outfit_create <name...> <tag>");

        console.RegisterCommand(
            "tf_outfit_invite",
            [this, joinArgs](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "usage: tf_outfit_invite <character name...>";
                const std::string target = joinArgs(args, 0, args.size());
                ClientRequestInvite(target);
                return "[TF] outfit invite requested for '" + target + "'";
            },
            "Invite an online character to your outfit (Officer+)", "TERRAFRONT",
            "tf_outfit_invite <character name...>");

        console.RegisterCommand(
            "tf_outfit_accept",
            [this](const std::vector<std::string>&) -> std::string
            {
                if (!m_mirror.hasInvite)
                    return "[TF] no pending outfit invite";
                ClientRequestAccept();
                return "[TF] accepting invite to '" + m_mirror.inviteName + "'";
            },
            "Accept the pending outfit invite", "TERRAFRONT", "tf_outfit_accept");

        console.RegisterCommand(
            "tf_outfit_decline",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestDecline();
                return "[TF] outfit invite declined";
            },
            "Decline the pending outfit invite", "TERRAFRONT", "tf_outfit_decline");

        console.RegisterCommand(
            "tf_outfit_leave",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestLeave();
                return "[TF] outfit leave requested";
            },
            "Leave your outfit (leadership auto-passes; empty outfits disband)", "TERRAFRONT", "tf_outfit_leave");

        console.RegisterCommand(
            "tf_outfit_kick",
            [this, joinArgs, memberByName](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "usage: tf_outfit_kick <member name...>";
                const std::string target = joinArgs(args, 0, args.size());
                uint64_t charId = 0;
                if (!memberByName(target, charId))
                    return "[TF] '" + target + "' is not on your roster (open the outfit panel to sync)";
                ClientRequestKick(charId);
                return "[TF] kick requested for '" + target + "'";
            },
            "Kick an outfit member by name (Leader: anyone; Officer: members)", "TERRAFRONT",
            "tf_outfit_kick <member name...>");

        console.RegisterCommand(
            "tf_outfit_rank",
            [this, joinArgs, memberByName](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "usage: tf_outfit_rank <member name...> <member|officer|leader>";
                const std::string rankStr = args.back();
                const std::string target = joinArgs(args, 0, args.size() - 1);
                TFOutfitRank rank;
                if (EqualsNoCase(rankStr, "member"))
                    rank = TFOutfitRank::Member;
                else if (EqualsNoCase(rankStr, "officer"))
                    rank = TFOutfitRank::Officer;
                else if (EqualsNoCase(rankStr, "leader"))
                    rank = TFOutfitRank::Leader;
                else
                    return "usage: tf_outfit_rank <member name...> <member|officer|leader>";
                uint64_t charId = 0;
                if (!memberByName(target, charId))
                    return "[TF] '" + target + "' is not on your roster";
                ClientRequestSetRank(charId, rank);
                return "[TF] rank change requested: '" + target + "' -> " + OutfitRankName(rank);
            },
            "Set an outfit member's rank (Leader only; 'leader' transfers leadership)", "TERRAFRONT",
            "tf_outfit_rank <member name...> <member|officer|leader>");

        console.RegisterCommand(
            "tf_outfit_disband",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestDisband();
                return "[TF] outfit disband requested";
            },
            "Disband your outfit (Leader only)", "TERRAFRONT", "tf_outfit_disband");

        console.RegisterCommand(
            "tf_outfit_status",
            [this](const std::vector<std::string>&) -> std::string
            {
                std::string out;
                if (m_mirror.outfitId == 0)
                    out = "[TF] not in an outfit";
                else
                {
                    out = "[TF] outfit '" + m_mirror.name + "' [" + m_mirror.tag +
                          "] members=" + std::to_string(m_mirror.totalMembers) +
                          " yourRank=" + OutfitRankName(LocalRank());
                    for (const MirrorMember& m : m_mirror.members)
                        out += "\n  " + m.name + " (" + OutfitRankName(m.rank) + (m.online ? ", online)" : ")");
                }
                if (m_mirror.hasInvite)
                    out += "\n[TF] pending invite: '" + m_mirror.inviteName + "' [" + m_mirror.inviteTag + "] from " +
                           m_mirror.inviteFrom + " (tf_outfit_accept / tf_outfit_decline)";
                if (m_mirror.hasResult)
                    out += std::string("\n[TF] last op result: ") + OutfitResultText(m_mirror.lastResult);
                return out;
            },
            "Show your outfit roster, pending invite and last op result", "TERRAFRONT", "tf_outfit_status");

        console.RegisterCommand(
            "tf_outfit_leaderboard",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestLeaderboard(); // refresh in flight; print what we have
                if (!m_lb.valid)
                    return "[TF] outfit leaderboard requested (re-run to see the snapshot)";
                std::string out = "[TF] outfit leaderboard (ISO week " + std::to_string(m_lb.weekKey) + ", " +
                                  std::to_string(m_lb.totalOutfits) + " outfits):";
                for (size_t i = 0; i < m_lb.rows.size(); ++i)
                {
                    const LbRow& r = m_lb.rows[i];
                    out += "\n  #" + std::to_string(r.rank) + " [" + r.tag + "] " + r.name +
                           "  weekly=" + std::to_string(r.weekly) + "  all-time=" + std::to_string(r.allTime);
                    if (static_cast<int>(i) == m_lb.yourIndex)
                        out += "  <- yours";
                }
                if (m_lb.rows.empty())
                    out += "\n  (no outfits yet)";
                return out;
            },
            "Fetch + print the outfit score leaderboard (kill +1, capture +10, alert win +100)", "TERRAFRONT",
            "tf_outfit_leaderboard");

        m_cmdsRegistered = true;
    }

} // namespace Terrafront
