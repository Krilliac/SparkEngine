/**
 * @file TFOutfitSystemWire.cpp
 * @brief TFOutfitSystem wire halves: server-side sends (replies, chunked
 *        rosters, tag broadcasts), client requests, the client mirror message
 *        handlers and the pure-client NetworkManager handler registration.
 *        Split from TFOutfitSystem.cpp; the shared helpers live in
 *        TFOutfitSystemInternal.h.
 */
#include "Game/TFOutfitSystem.h"

#include "Game/TFOutfitSystemInternal.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>
#include <utility>

namespace Terrafront
{

    using namespace OutfitDetail;

    // ---------------------------------------------------------------------------
    // Server: wire send
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::SendReply(PlayerId player, TFOutfitOp op, TFOutfitResult result, uint32_t outfitId)
    {
        TF_OutfitReply rep{};
        rep.op = static_cast<uint8_t>(op);
        rep.result = static_cast<uint8_t>(result);
        rep.outfitId = outfitId;
        SendWireTo(player, kTFMsgOutfitReply, &rep, sizeof(rep));
    }

    void TFOutfitSystem::SendRosterTo(PlayerId player, const TFOutfitRecord* outfit)
    {
        if (player == kInvalidPlayer)
            return;

        if (!outfit)
        {
            TF_OutfitRoster cleared{};
            cleared.outfitId = 0;
            SendWireTo(player, kTFMsgOutfitRoster, &cleared, sizeof(cleared));
            return;
        }

        const BoundChar* bc = BoundCharOf(player);
        const uint64_t yourCharId = bc ? bc->charId : 0;
        const size_t total = outfit->members.size();
        const size_t chunks = std::max<size_t>(1, (total + kTFOutfitRosterChunk - 1) / kTFOutfitRosterChunk);

        for (size_t chunk = 0; chunk < chunks; ++chunk)
        {
            TF_OutfitRoster r{};
            r.outfitId = outfit->id;
            r.yourCharId = yourCharId;
            CopyField(r.name, sizeof(r.name), outfit->name);
            CopyField(r.tag, sizeof(r.tag), outfit->tag);
            r.totalMembers = static_cast<uint16_t>(std::min<size_t>(total, 0xFFFF));
            r.chunkIndex = static_cast<uint8_t>(chunk);

            const size_t base = chunk * kTFOutfitRosterChunk;
            const size_t n = std::min<size_t>(kTFOutfitRosterChunk, total - std::min(total, base));
            r.count = static_cast<uint8_t>(n);
            for (size_t i = 0; i < n; ++i)
            {
                const TFOutfitMemberRecord& m = outfit->members[base + i];
                TF_OutfitMemberBrief& b = r.members[i];
                b.charId = m.charId;
                CopyField(b.name, sizeof(b.name), m.name);
                b.rank = static_cast<uint8_t>(m.rank);
                b.online = OnlinePlayerOfChar(m.charId) != kInvalidPlayer ? 1 : 0;
            }
            SendWireTo(player, kTFMsgOutfitRoster, &r, sizeof(r));
        }
    }

    void TFOutfitSystem::BroadcastRoster(const TFOutfitRecord& outfit)
    {
        for (const TFOutfitMemberRecord& m : outfit.members)
        {
            const PlayerId p = OnlinePlayerOfChar(m.charId);
            if (p != kInvalidPlayer)
                SendRosterTo(p, &outfit);
        }
    }

    void TFOutfitSystem::BroadcastTag(PlayerId player, const std::string& tag)
    {
        TF_OutfitTagUpdate upd{};
        upd.player = player;
        CopyField(upd.tag, sizeof(upd.tag), tag);
        for (const auto& [p, bc] : m_boundChars)
            SendWireTo(p, kTFMsgOutfitTagUpdate, &upd, sizeof(upd));
    }

    void TFOutfitSystem::SendTagTableTo(PlayerId player)
    {
        for (const auto& [p, bc] : m_boundChars)
        {
            const TFOutfitRecord* rec = m_store.FindByCharacter(bc.charId);
            if (!rec)
                continue;
            TF_OutfitTagUpdate upd{};
            upd.player = p;
            CopyField(upd.tag, sizeof(upd.tag), rec->tag);
            SendWireTo(player, kTFMsgOutfitTagUpdate, &upd, sizeof(upd));
        }
    }

    void TFOutfitSystem::SendWireTo(PlayerId player, uint16_t msgId, const void* payload, size_t size)
    {
        if (player == kInvalidPlayer)
            return;
        // Listen host / standalone: the local player is not a network client —
        // feed the mirror directly (TFSquadSystem::SendEchoTo pattern).
        if (m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer && m_ctx->role != NetRole::Client)
        {
            ClientHandleWire(msgId, payload, size);
            return;
        }
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(player, msg);
#endif
    }

    // ---------------------------------------------------------------------------
    // Client: requests
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::SendRequest(const TF_OutfitRequest& req)
    {
        if (!m_initialized || !m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;

        if (m_ctx->IsAuthority())
        {
            // Mirror the RouteClientMessage enter-world gate for the direct
            // authority path (TFSquadSystem::SendOp defense-in-depth pattern).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            ServerHandleOutfitMsg(m_ctx->localPlayer, req);
        }
        else if (m_ctx->clientNet)
        {
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgOutfitRequest), &req, sizeof(req));
        }
    }

    void TFOutfitSystem::ClientRequestCreate(const std::string& name, const std::string& tag)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Create);
        CopyField(req.name, sizeof(req.name), name);
        CopyField(req.tag, sizeof(req.tag), tag);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestInvite(const std::string& characterName)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Invite);
        CopyField(req.name, sizeof(req.name), characterName);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestAccept()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Accept);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestDecline()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Decline);
        SendRequest(req);
        // Optimistic local clear: the reply only confirms.
        m_mirror.hasInvite = false;
    }

    void TFOutfitSystem::ClientRequestLeave()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Leave);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestKick(uint64_t targetCharId)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Kick);
        req.targetCharId = targetCharId;
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestSetRank(uint64_t targetCharId, TFOutfitRank rank)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::SetRank);
        req.rank = static_cast<uint8_t>(rank);
        req.targetCharId = targetCharId;
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestDisband()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Disband);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestLeaderboard()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Leaderboard);
        SendRequest(req);
    }

    // ---------------------------------------------------------------------------
    // Client: mirror
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::ClientHandleWire(uint16_t msgId, const void* data, size_t size)
    {
        if (!data)
            return;
        switch (msgId)
        {
        case kTFMsgOutfitReply:
        {
            if (size != sizeof(TF_OutfitReply))
                return;
            TF_OutfitReply rep;
            std::memcpy(&rep, data, sizeof(rep));
            ClientHandleReply(rep);
            break;
        }
        case kTFMsgOutfitRoster:
        {
            if (size != sizeof(TF_OutfitRoster))
                return;
            TF_OutfitRoster roster;
            std::memcpy(&roster, data, sizeof(roster));
            roster.name[sizeof(roster.name) - 1] = '\0';
            roster.tag[sizeof(roster.tag) - 1] = '\0';
            ClientHandleRoster(roster);
            break;
        }
        case kTFMsgOutfitTagUpdate:
        {
            if (size != sizeof(TF_OutfitTagUpdate))
                return;
            TF_OutfitTagUpdate upd;
            std::memcpy(&upd, data, sizeof(upd));
            upd.tag[sizeof(upd.tag) - 1] = '\0';
            ClientHandleTagUpdate(upd);
            break;
        }
        case kTFMsgOutfitInvite:
        {
            if (size != sizeof(TF_OutfitInvite))
                return;
            TF_OutfitInvite inv;
            std::memcpy(&inv, data, sizeof(inv));
            inv.name[sizeof(inv.name) - 1] = '\0';
            inv.tag[sizeof(inv.tag) - 1] = '\0';
            inv.inviter[sizeof(inv.inviter) - 1] = '\0';
            ClientHandleInvite(inv);
            break;
        }
        case kTFMsgOutfitLeaderboard:
        {
            if (size != sizeof(TF_OutfitLeaderboard))
                return;
            TF_OutfitLeaderboard lb;
            std::memcpy(&lb, data, sizeof(lb));
            for (TF_OutfitLbRow& row : lb.rows)
            {
                row.name[sizeof(row.name) - 1] = '\0';
                row.tag[sizeof(row.tag) - 1] = '\0';
            }
            ClientHandleLeaderboard(lb);
            break;
        }
        default:
            break;
        }
    }

    void TFOutfitSystem::ClientHandleReply(const TF_OutfitReply& rep)
    {
        NoteResult(static_cast<TFOutfitOp>(rep.op), static_cast<TFOutfitResult>(rep.result));
    }

    void TFOutfitSystem::ClientHandleRoster(const TF_OutfitRoster& roster)
    {
        if (roster.outfitId == 0)
        {
            // Left / kicked / disbanded: clear outfit fields, keep the tag map.
            m_mirror.outfitId = 0;
            m_mirror.yourCharId = 0;
            m_mirror.name.clear();
            m_mirror.tag.clear();
            m_mirror.totalMembers = 0;
            m_mirror.members.clear();
            return;
        }

        if (roster.chunkIndex == 0 || roster.outfitId != m_mirror.outfitId)
        {
            m_mirror.members.clear();
            m_mirror.outfitId = roster.outfitId;
            m_mirror.yourCharId = roster.yourCharId;
            m_mirror.name = FieldToString(roster.name, sizeof(roster.name));
            m_mirror.tag = FieldToString(roster.tag, sizeof(roster.tag));
            m_mirror.totalMembers = roster.totalMembers;
            m_mirror.hasInvite = false; // joining/being synced voids any stale invite UI
        }

        const uint8_t n = std::min<uint8_t>(roster.count, kTFOutfitRosterChunk);
        for (uint8_t i = 0; i < n; ++i)
        {
            const TF_OutfitMemberBrief& b = roster.members[i];
            MirrorMember m;
            m.charId = b.charId;
            m.name = FieldToString(b.name, sizeof(b.name));
            m.rank = static_cast<TFOutfitRank>(std::min<uint8_t>(b.rank, static_cast<uint8_t>(TFOutfitRank::Leader)));
            m.online = b.online != 0;
            m_mirror.members.push_back(std::move(m));
        }
    }

    void TFOutfitSystem::ClientHandleTagUpdate(const TF_OutfitTagUpdate& upd)
    {
        const std::string tag = FieldToString(upd.tag, sizeof(upd.tag));
        if (tag.empty())
            m_tags.erase(upd.player);
        else
            m_tags[upd.player] = tag;
    }

    void TFOutfitSystem::ClientHandleInvite(const TF_OutfitInvite& inv)
    {
        m_mirror.hasInvite = true;
        m_mirror.inviteOutfitId = inv.outfitId;
        m_mirror.inviteName = FieldToString(inv.name, sizeof(inv.name));
        m_mirror.inviteTag = FieldToString(inv.tag, sizeof(inv.tag));
        m_mirror.inviteFrom = FieldToString(inv.inviter, sizeof(inv.inviter));
    }

    void TFOutfitSystem::ClientHandleLeaderboard(const TF_OutfitLeaderboard& lb)
    {
        m_lb.rows.clear();
        m_lb.valid = true;
        m_lb.totalOutfits = lb.totalOutfits;
        m_lb.weekKey = lb.weekKey;
        const uint8_t n = std::min<uint8_t>(lb.count, kTFOutfitLbMaxRows);
        m_lb.yourIndex = lb.yourIndex < n ? static_cast<int>(lb.yourIndex) : -1;
        m_lb.rows.reserve(n);
        for (uint8_t i = 0; i < n; ++i)
        {
            const TF_OutfitLbRow& r = lb.rows[i];
            LbRow row;
            row.outfitId = r.outfitId;
            row.rank = r.rank;
            row.weekly = r.weekly;
            row.allTime = r.allTime;
            row.name = FieldToString(r.name, sizeof(r.name));
            row.tag = FieldToString(r.tag, sizeof(r.tag));
            m_lb.rows.push_back(std::move(row));
        }
    }

    void TFOutfitSystem::NoteResult(TFOutfitOp op, TFOutfitResult result)
    {
        m_mirror.hasResult = true;
        m_mirror.lastOp = op;
        m_mirror.lastResult = result;
        m_mirror.resultAge = 0.0f;
        if (result == TFOutfitResult::Ok && (op == TFOutfitOp::Accept || op == TFOutfitOp::Decline))
            m_mirror.hasInvite = false;
    }

    // ---------------------------------------------------------------------------
    // Client handlers (pure clients only; TFSquadSystem EnsureClientHandlers
    // pattern — the S->C ids in the reserved block are exclusively ours, so no
    // handler-slot contention with TFClientNet)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFOutfitSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFOutfitSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        for (uint16_t id : {kTFMsgOutfitReply, kTFMsgOutfitRoster, kTFMsgOutfitTagUpdate, kTFMsgOutfitInvite,
                            kTFMsgOutfitLeaderboard})
        {
            nm.RegisterHandler(static_cast<MessageType>(id), [this, id](const NetworkMessage& m)
                               { ClientHandleWire(id, m.payload.data(), m.payload.size()); });
        }
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit mirror handlers registered");
    }

    void TFOutfitSystem::ReleaseClientHandlers()
    {
        // No per-type removal in NetworkManager; overwrite with no-ops so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFMsgOutfitReply, kTFMsgOutfitRoster, kTFMsgOutfitTagUpdate, kTFMsgOutfitInvite,
                            kTFMsgOutfitLeaderboard})
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
