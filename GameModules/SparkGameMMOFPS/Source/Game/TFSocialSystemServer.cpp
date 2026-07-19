/**
 * @file TFSocialSystemServer.cpp
 * @brief TFSocialSystem server registry: enter/leave detection off
 *        TFServerSim's public state, TF_SocialOp list mutations, the
 *        kill-pair/squad-join recent-players feed and the server-side net
 *        handler lifecycle. Split from TFSocialSystem.cpp; the shared
 *        helpers live in TFSocialSystemInternal.h.
 */
#include "Game/TFSocialSystem.h"

#include "Game/TFSocialSystemInternal.h"
#include "Game/TFSquadSystem.h"
#include "Net/TFServerSim.h"
#include "Persistence/TFDatabase.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Terrafront
{

    using namespace SocialDetail;

    namespace
    {

        bool ContainsName(const std::vector<std::string>& names, const std::string& name)
        {
            return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return NameEq(n, name); });
        }

        void EraseName(std::vector<std::string>& names, const std::string& name)
        {
            names.erase(
                std::remove_if(names.begin(), names.end(), [&](const std::string& n) { return NameEq(n, name); }),
                names.end());
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Server: enter/leave detection (poll — no TFServerSim edits required)
    // ---------------------------------------------------------------------------

    void TFSocialSystem::ServerPollEnteredPlayers()
    {
        if (!m_ctx->serverSim)
            return;

        std::vector<PlayerId> candidates;
        if (m_ctx->HasLocalPlayer() && m_ctx->localPlayer != kInvalidPlayer)
            candidates.push_back(m_ctx->localPlayer);
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
        {
            for (const auto& [id, info] : nm.GetClients())
            {
                if (info.state == Spark::Net::ConnectionState::Connected)
                    candidates.push_back(id);
            }
        }
#endif

        for (const PlayerId player : candidates)
        {
            const bool entered = m_ctx->serverSim->IsEnteredWorld(player);
            if (entered && !m_online.contains(player))
                ServerOnPlayerEntered(player);
        }

        // Leave sweep: anyone we track who is no longer entered-world.
        std::vector<PlayerId> gone;
        for (const auto& [player, info] : m_online)
        {
            (void)info;
            if (!m_ctx->serverSim->IsEnteredWorld(player))
                gone.push_back(player);
        }
        for (const PlayerId player : gone)
            ServerOnPlayerLeft(player);
    }

    void TFSocialSystem::ServerOnPlayerEntered(PlayerId player)
    {
        OnlineInfo info;
        info.charId = m_ctx->serverSim ? m_ctx->serverSim->ActiveCharacterOf(player) : 0;
        info.faction = m_ctx->serverSim ? m_ctx->serverSim->GetPlayerFaction(player) : FactionId::None;

        TFCharacterRecord rec;
        if (info.charId != 0 && m_ctx->db && m_ctx->db->IsOpen() && m_ctx->db->FindCharacter(info.charId, rec))
        {
            info.name = rec.name;
            if (info.faction == FactionId::None)
                info.faction = rec.faction;
        }
        else
        {
            char fallback[16];
            std::snprintf(fallback, sizeof(fallback), "p%u", player);
            info.name = fallback;
        }

        m_online[player] = info;
        StoreEnsureLoaded();

        // Push this player's persisted lists + the full roster, then tell
        // everyone else about the newcomer.
        SendAllListsTo(player);
        SendFullRosterTo(player);
        BroadcastRosterDelta(player, true);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social: '%s' (p%u, char %llu) entered world", info.name.c_str(),
                       player, static_cast<unsigned long long>(info.charId));
    }

    void TFSocialSystem::ServerOnPlayerLeft(PlayerId player)
    {
        auto it = m_online.find(player);
        if (it == m_online.end())
            return;
        const std::string name = it->second.name;
        m_online.erase(it);
        BroadcastRosterDelta(player, false);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social: '%s' (p%u) left world", name.c_str(), player);
    }

    bool TFSocialSystem::IsCharacterOnline(const std::string& name) const
    {
        return std::any_of(m_online.begin(), m_online.end(),
                           [&](const auto& kv) { return NameEq(kv.second.name, name); });
    }

    FactionId TFSocialSystem::FactionOfCharacterName(const std::string& name) const
    {
        for (const auto& [id, info] : m_online)
        {
            (void)id;
            if (NameEq(info.name, name))
                return info.faction;
        }
        TFCharacterRecord rec;
        if (m_ctx && m_ctx->db && m_ctx->db->IsOpen() && m_ctx->db->FindCharacterByName(name, rec))
            return rec.faction;
        return FactionId::None;
    }

    // ---------------------------------------------------------------------------
    // Server: TF_SocialOp
    // ---------------------------------------------------------------------------

    void TFSocialSystem::ServerHandleSocialOpRaw(PlayerId sender, const void* data, size_t size)
    {
        if (data == nullptr || size != sizeof(TF_SocialOp))
            return;
        TF_SocialOp op;
        std::memcpy(&op, data, sizeof(op));
        ServerHandleSocialOp(sender, op);
    }

    void TFSocialSystem::ServerHandleSocialOp(PlayerId sender, const TF_SocialOp& op)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        auto onlineIt = m_online.find(sender);
        if (onlineIt == m_online.end() || onlineIt->second.charId == 0)
        {
            SendOpReplyTo(sender, static_cast<SocialOp>(op.op), SocialOpResult::ServerError, "");
            return;
        }
        if (!StoreEnsureLoaded())
        {
            SendOpReplyTo(sender, static_cast<SocialOp>(op.op), SocialOpResult::ServerError, "");
            return;
        }

        const auto kind = static_cast<SocialOp>(op.op);
        if (kind == SocialOp::RequestLists)
        {
            SendAllListsTo(sender);
            SendFullRosterTo(sender);
            return;
        }

        const std::string target = TrimmedName(NameFromWire(op.targetName));
        if (target.empty())
        {
            SendOpReplyTo(sender, kind, SocialOpResult::BadName, target);
            return;
        }
        if (NameEq(target, onlineIt->second.name))
        {
            SendOpReplyTo(sender, kind, SocialOpResult::Self, target);
            return;
        }

        SocialRecord& rec = m_store[onlineIt->second.charId];
        SocialOpResult result = SocialOpResult::ServerError;
        std::string canonical = target;

        switch (kind)
        {
        case SocialOp::FriendAdd:
        case SocialOp::BlockAdd:
        {
            // Target must be a real character (any character, online or not).
            TFCharacterRecord targetRec;
            if (!m_ctx->db || !m_ctx->db->IsOpen() || !m_ctx->db->FindCharacterByName(target, targetRec))
            {
                result = SocialOpResult::NotFound;
                break;
            }
            canonical = targetRec.name;
            std::vector<std::string>& list = (kind == SocialOp::FriendAdd) ? rec.friends : rec.blocked;
            const size_t cap = (kind == SocialOp::FriendAdd) ? kTFMaxFriends : kTFMaxBlocked;
            if (ContainsName(list, canonical))
            {
                result = SocialOpResult::AlreadyListed;
                break;
            }
            if (list.size() >= cap)
            {
                result = SocialOpResult::ListFull;
                break;
            }
            list.push_back(canonical);
            // Blocking someone removes them from the friends list.
            if (kind == SocialOp::BlockAdd && ContainsName(rec.friends, canonical))
                EraseName(rec.friends, canonical);
            StoreMarkDirty();
            result = SocialOpResult::Ok;
            break;
        }
        case SocialOp::FriendRemove:
        case SocialOp::BlockRemove:
        {
            std::vector<std::string>& list = (kind == SocialOp::FriendRemove) ? rec.friends : rec.blocked;
            if (!ContainsName(list, target))
            {
                result = SocialOpResult::NotListed;
                break;
            }
            EraseName(list, target);
            StoreMarkDirty();
            result = SocialOpResult::Ok;
            break;
        }
        default:
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] social: player %u sent unknown SocialOp %u", sender, op.op);
            return;
        }

        SendOpReplyTo(sender, kind, result, canonical);
        if (result == SocialOpResult::Ok)
        {
            // List mutations flush eagerly (recent-list writes stay debounced).
            if (StoreSaveToDisk())
                m_storeDirty = false;
            SendListTo(sender, (kind == SocialOp::FriendAdd || kind == SocialOp::FriendRemove)
                                   ? SocialListKind::Friends
                                   : SocialListKind::Blocked);
            if (kind == SocialOp::BlockAdd)
                SendListTo(sender, SocialListKind::Friends); // block may have unfriended
        }
    }

    // ---------------------------------------------------------------------------
    // Server: recent players (kill pairs + squad joins)
    // ---------------------------------------------------------------------------

    void TFSocialSystem::OnBusPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        if (ev.killer == ev.victim || ev.killer == kInvalidPlayer || ev.victim == kInvalidPlayer)
            return;
        ServerTouchRecentPair(ev.killer, ev.victim);
    }

    void TFSocialSystem::OnBusSquadChanged(const EvSquadChanged& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !ev.joined || !m_ctx->squads)
            return;
        // Record the joiner with every other online member of that squad.
        for (const auto& [player, info] : m_online)
        {
            (void)info;
            if (player != ev.player && m_ctx->squads->SquadOf(player) == ev.squad)
                ServerTouchRecentPair(ev.player, player);
        }
    }

    void TFSocialSystem::ServerTouchRecentPair(PlayerId a, PlayerId b)
    {
        auto aIt = m_online.find(a);
        auto bIt = m_online.find(b);
        if (aIt == m_online.end() || bIt == m_online.end())
            return;
        if (aIt->second.charId == 0 || bIt->second.charId == 0)
            return;
        if (!StoreEnsureLoaded())
            return;
        ServerTouchRecent(aIt->second.charId, bIt->second.name);
        ServerTouchRecent(bIt->second.charId, aIt->second.name);
        SendListTo(a, SocialListKind::Recent);
        SendListTo(b, SocialListKind::Recent);
    }

    void TFSocialSystem::ServerTouchRecent(uint64_t charId, const std::string& otherName)
    {
        SocialRecord& rec = m_store[charId];
        rec.recent.erase(std::remove_if(rec.recent.begin(), rec.recent.end(),
                                        [&](const RecentRec& r) { return NameEq(r.name, otherName); }),
                         rec.recent.end());
        rec.recent.insert(rec.recent.begin(), RecentRec{otherName, NowMs()});
        while (rec.recent.size() > kTFMaxRecent)
            rec.recent.pop_back();
        StoreMarkDirty();
    }

    // ---------------------------------------------------------------------------
    // Server net handler lifecycle
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFSocialSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server;
    }

    void TFSocialSystem::EnsureServerHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        // Enter-world gate mirrors TFServerSim::RouteClientMessage for gameplay
        // ids: a pre-enter-world client cannot mutate social lists.
        nm.RegisterHandler(static_cast<MessageType>(kTFSocialMsg_Op),
                           [this](const NetworkMessage& m)
                           {
                               if (!m_ctx || !m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m.senderID))
                                   return;
                               ServerHandleSocialOpRaw(m.senderID, m.payload.data(), m.payload.size());
                           });

        m_serverHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social server handler registered");
    }

    void TFSocialSystem::ReleaseServerHandlers()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFSocialMsg_Op),
                           [](const Spark::Net::NetworkMessage&) {});
        m_serverHandlers = false;
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
