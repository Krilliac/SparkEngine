/**
 * @file TFOutfitSystemServer.cpp
 * @brief TFOutfitSystem server op handlers: TF_OutfitRequest dispatch,
 *        Create/Invite/Accept/Decline/Leave/Kick/SetRank/Disband with the
 *        rank-policy checks, and the leader auto-promotion / disband-on-empty
 *        path. Split from TFOutfitSystem.cpp; the shared helpers live in
 *        TFOutfitSystemInternal.h.
 */
#include "Game/TFOutfitSystem.h"

#include "Game/TFOutfitSystemInternal.h"
#include "Persistence/TFSavePaths.h"
#include "Utils/LogMacros.h"

#include <cstring>

namespace Terrafront
{

    using namespace OutfitDetail;

    // ---------------------------------------------------------------------------
    // Server: op dispatch
    // ---------------------------------------------------------------------------

    bool TFOutfitSystem::EnsureStoreOpen()
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return false;
        if (m_store.IsOpen())
            return true;
        if (m_storeOpenFailed)
            return false; // quarantined/corrupt file: do not retry (and re-quarantine) every op
        const std::filesystem::path path = SavePaths::File("outfits.json");
        if (!path.empty() && m_store.Open(path))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] authority opened outfit store at %s (%zu outfits)",
                           SavePaths::Utf8ForLog(path).c_str(), m_store.OutfitCount());
            RolloverIfNeeded(); // server-load weekly rollover path (shared with the Update tick)
            return true;
        }
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] authority failed to open outfit store at %s",
                        path.empty() ? "<invalid save path>" : SavePaths::Utf8ForLog(path).c_str());
        m_storeOpenFailed = true;
        return false;
    }

    const TFOutfitSystem::BoundChar* TFOutfitSystem::BoundCharOf(PlayerId player) const
    {
        const auto it = m_boundChars.find(player);
        return it != m_boundChars.end() ? &it->second : nullptr;
    }

    PlayerId TFOutfitSystem::OnlinePlayerOfChar(uint64_t charId) const
    {
        const auto it = m_playerOfChar.find(charId);
        return it != m_playerOfChar.end() ? it->second : kInvalidPlayer;
    }

    PlayerId TFOutfitSystem::FindOnlinePlayerByCharName(const std::string& name) const
    {
        for (const auto& [player, bc] : m_boundChars)
            if (EqualsNoCase(bc.name, name))
                return player;
        return kInvalidPlayer;
    }

    void TFOutfitSystem::ServerHandleOutfitMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_OutfitRequest) || sender == kInvalidPlayer || !data)
        {
            ++m_badPackets;
            return;
        }
        TF_OutfitRequest req;
        std::memcpy(&req, data, sizeof(req));
        req.name[sizeof(req.name) - 1] = '\0';
        req.tag[sizeof(req.tag) - 1] = '\0';
        ServerHandleOutfitMsg(sender, req);
    }

    void TFOutfitSystem::ServerHandleOutfitMsg(PlayerId sender, const TF_OutfitRequest& req)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        const TFOutfitOp op = static_cast<TFOutfitOp>(req.op);
        uint32_t outfitId = 0;
        TFOutfitResult result = TFOutfitResult::ServerError;

        if (!EnsureStoreOpen())
        {
            SendReply(sender, op, TFOutfitResult::ServerError, 0);
            return;
        }

        const std::string name = FieldToString(req.name, sizeof(req.name));
        const std::string tag = FieldToString(req.tag, sizeof(req.tag));

        switch (op)
        {
        case TFOutfitOp::Create:
            result = ServerCreate(sender, name, tag, outfitId);
            break;
        case TFOutfitOp::Invite:
            result = ServerInvite(sender, name, outfitId);
            break;
        case TFOutfitOp::Accept:
            result = ServerAccept(sender, outfitId);
            break;
        case TFOutfitOp::Decline:
            result = ServerDecline(sender);
            break;
        case TFOutfitOp::Leave:
            result = ServerLeave(sender, outfitId);
            break;
        case TFOutfitOp::Kick:
            result = ServerKick(sender, req.targetCharId, outfitId);
            break;
        case TFOutfitOp::SetRank:
            result = ServerSetRank(sender, req.targetCharId, static_cast<TFOutfitRank>(req.rank), outfitId);
            break;
        case TFOutfitOp::Disband:
            result = ServerDisband(sender, outfitId);
            break;
        case TFOutfitOp::Leaderboard:
            // The snapshot IS the reply — no TF_OutfitReply (it would flash the
            // panel's op-status line on every silent 60 s refresh).
            ServerSendLeaderboard(sender);
            return;
        default:
            ++m_badPackets;
            result = TFOutfitResult::BadRequest;
            break;
        }

        SendReply(sender, op, result, outfitId);
        if (result != TFOutfitResult::Ok)
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit op %u from player %u rejected: %s",
                           static_cast<unsigned>(op), sender, OutfitResultText(result));
    }

    TFOutfitResult TFOutfitSystem::ServerCreate(PlayerId sender, const std::string& name, const std::string& tag,
                                                uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        if (m_store.FindByCharacter(bc->charId))
            return TFOutfitResult::AlreadyInOutfit;
        if (!ValidateOutfitName(name))
            return TFOutfitResult::NameInvalid;
        if (!ValidateOutfitTag(tag))
            return TFOutfitResult::TagInvalid;
        if (m_store.FindByName(name))
            return TFOutfitResult::NameTaken;
        if (m_store.FindByTag(tag))
            return TFOutfitResult::TagTaken;

        const TFOutfitRecord* rec = m_store.Create(name, tag, bc->charId, bc->name, NowMs());
        if (!rec)
            return TFOutfitResult::ServerError;

        outOutfitId = rec->id;
        m_invites.erase(bc->charId); // creating your own outfit voids a pending invite

        SendRosterTo(sender, rec);
        BroadcastTag(sender, rec->tag);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit '%s' [%s] created by character %llu", rec->name.c_str(),
                       rec->tag.c_str(), static_cast<unsigned long long>(bc->charId));
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerInvite(PlayerId sender, const std::string& targetName, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        outOutfitId = rec->id;

        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        if (!me || me->rank == TFOutfitRank::Member)
            return TFOutfitResult::NotPermitted; // Officer+ may invite
        if (rec->members.size() >= kTFMaxOutfitMembers)
            return TFOutfitResult::OutfitFull;
        if (targetName.empty())
            return TFOutfitResult::BadRequest;

        const PlayerId targetPlayer = FindOnlinePlayerByCharName(targetName);
        if (targetPlayer == kInvalidPlayer || targetPlayer == sender)
            return TFOutfitResult::NoSuchPlayer;
        const BoundChar* targetBc = BoundCharOf(targetPlayer);
        if (!targetBc)
            return TFOutfitResult::NoSuchPlayer;
        if (m_store.FindByCharacter(targetBc->charId))
            return TFOutfitResult::TargetInOutfit;

        m_invites[targetBc->charId] = rec->id; // latest invite wins (one pending per character)

        TF_OutfitInvite inv{};
        inv.outfitId = rec->id;
        CopyField(inv.name, sizeof(inv.name), rec->name);
        CopyField(inv.tag, sizeof(inv.tag), rec->tag);
        CopyField(inv.inviter, sizeof(inv.inviter), bc->name);
        SendWireTo(targetPlayer, kTFMsgOutfitInvite, &inv, sizeof(inv));
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerAccept(PlayerId sender, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        if (m_store.FindByCharacter(bc->charId))
        {
            m_invites.erase(bc->charId);
            return TFOutfitResult::AlreadyInOutfit;
        }
        const auto invIt = m_invites.find(bc->charId);
        if (invIt == m_invites.end())
            return TFOutfitResult::NoInvite;
        const uint32_t outfitId = invIt->second;

        const TFOutfitRecord* rec = m_store.FindById(outfitId);
        if (!rec)
        {
            m_invites.erase(invIt);
            return TFOutfitResult::NoInvite; // outfit disbanded since the invite
        }
        if (rec->members.size() >= kTFMaxOutfitMembers)
            return TFOutfitResult::OutfitFull;

        if (!m_store.AddMember(outfitId, bc->charId, bc->name, TFOutfitRank::Member, NowMs()))
            return TFOutfitResult::ServerError;
        m_invites.erase(bc->charId);
        outOutfitId = outfitId;

        rec = m_store.FindById(outfitId); // re-fetch: mutation may have re-seated storage
        if (rec)
            BroadcastRoster(*rec);
        BroadcastTag(sender, rec ? rec->tag : std::string{});
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerDecline(PlayerId sender)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        if (m_invites.erase(bc->charId) == 0)
            return TFOutfitResult::NoInvite;
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerLeave(PlayerId sender, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        const uint32_t outfitId = rec->id;
        outOutfitId = outfitId;
        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        const bool wasLeader = me && me->rank == TFOutfitRank::Leader;

        if (!m_store.RemoveMember(outfitId, bc->charId))
            return TFOutfitResult::ServerError;
        ServerRemoveMembership(outfitId, bc->charId, sender);

        if (wasLeader)
            ServerAfterLeaderChange(outfitId); // may disband-on-empty
        if (const TFOutfitRecord* remaining = m_store.FindById(outfitId))
            BroadcastRoster(*remaining);
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerKick(PlayerId sender, uint64_t targetCharId, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        outOutfitId = rec->id;

        if (targetCharId == 0 || targetCharId == bc->charId)
            return TFOutfitResult::BadRequest; // leaving is not a kick
        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        const TFOutfitMemberRecord* target = rec->FindMember(targetCharId);
        if (!me)
            return TFOutfitResult::ServerError;
        if (!target)
            return TFOutfitResult::NoSuchMember;

        // Leader kicks anyone below Leader; Officer kicks Members only.
        const bool permitted = (me->rank == TFOutfitRank::Leader && target->rank != TFOutfitRank::Leader) ||
                               (me->rank == TFOutfitRank::Officer && target->rank == TFOutfitRank::Member);
        if (!permitted)
            return TFOutfitResult::NotPermitted;

        const uint32_t outfitId = rec->id;
        if (!m_store.RemoveMember(outfitId, targetCharId))
            return TFOutfitResult::ServerError;
        ServerRemoveMembership(outfitId, targetCharId, OnlinePlayerOfChar(targetCharId));

        if (const TFOutfitRecord* remaining = m_store.FindById(outfitId))
            BroadcastRoster(*remaining);
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerSetRank(PlayerId sender, uint64_t targetCharId, TFOutfitRank rank,
                                                 uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        outOutfitId = rec->id;

        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        if (!me || me->rank != TFOutfitRank::Leader)
            return TFOutfitResult::NotPermitted; // rank changes are Leader-only
        const TFOutfitMemberRecord* target = rec->FindMember(targetCharId);
        if (!target)
            return TFOutfitResult::NoSuchMember;
        if (targetCharId == bc->charId)
            return TFOutfitResult::BadRequest; // no self-rank ops (transfer names a target)
        if (rank > TFOutfitRank::Leader)
            return TFOutfitResult::BadRequest;

        const uint32_t outfitId = rec->id;
        if (rank == TFOutfitRank::Leader)
        {
            // Leadership transfer: exactly one Leader — old leader steps to Officer.
            if (!m_store.SetMemberRank(outfitId, bc->charId, TFOutfitRank::Officer) ||
                !m_store.SetMemberRank(outfitId, targetCharId, TFOutfitRank::Leader))
                return TFOutfitResult::ServerError;
        }
        else
        {
            if (!m_store.SetMemberRank(outfitId, targetCharId, rank))
                return TFOutfitResult::ServerError;
        }

        if (const TFOutfitRecord* updated = m_store.FindById(outfitId))
            BroadcastRoster(*updated);
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerDisband(PlayerId sender, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        if (!me || me->rank != TFOutfitRank::Leader)
            return TFOutfitResult::NotPermitted;

        const uint32_t outfitId = rec->id;
        outOutfitId = outfitId;

        // Clear every online member's mirror + tag BEFORE the record vanishes.
        for (const TFOutfitMemberRecord& m : rec->members)
        {
            const PlayerId p = OnlinePlayerOfChar(m.charId);
            if (p == kInvalidPlayer)
                continue;
            SendRosterTo(p, nullptr);
            BroadcastTag(p, std::string{});
        }
        // Void pending invites into the dead outfit.
        for (auto it = m_invites.begin(); it != m_invites.end();)
        {
            if (it->second == outfitId)
                it = m_invites.erase(it);
            else
                ++it;
        }

        if (!m_store.Disband(outfitId))
            return TFOutfitResult::ServerError;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit %u disbanded by character %llu", outfitId,
                       static_cast<unsigned long long>(bc->charId));
        return TFOutfitResult::Ok;
    }

    void TFOutfitSystem::ServerRemoveMembership(uint32_t /*outfitId*/, uint64_t /*charId*/, PlayerId onlinePlayer)
    {
        if (onlinePlayer == kInvalidPlayer)
            return;
        SendRosterTo(onlinePlayer, nullptr);       // clears the mirror
        BroadcastTag(onlinePlayer, std::string{}); // clears the nameplate tag everywhere
    }

    void TFOutfitSystem::ServerAfterLeaderChange(uint32_t outfitId)
    {
        const TFOutfitRecord* rec = m_store.FindById(outfitId);
        if (!rec)
            return;
        if (rec->members.empty())
        {
            m_store.Disband(outfitId);
            for (auto it = m_invites.begin(); it != m_invites.end();)
            {
                if (it->second == outfitId)
                    it = m_invites.erase(it);
                else
                    ++it;
            }
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit %u disbanded (last member left)", outfitId);
            return;
        }
        if (rec->Leader())
            return; // still has a leader

        // Auto-promote: senior Officer (earliest joinedAtMs), else senior Member.
        const TFOutfitMemberRecord* heir = nullptr;
        for (const TFOutfitMemberRecord& m : rec->members)
        {
            if (!heir)
            {
                heir = &m;
                continue;
            }
            const bool mBetter = (m.rank > heir->rank) || (m.rank == heir->rank && m.joinedAtMs < heir->joinedAtMs);
            if (mBetter)
                heir = &m;
        }
        if (heir)
        {
            m_store.SetMemberRank(outfitId, heir->charId, TFOutfitRank::Leader);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit %u leadership auto-passed to character %llu",
                           outfitId, static_cast<unsigned long long>(heir->charId));
        }
    }

} // namespace Terrafront
