/**
 * @file TFSocialSystemWire.cpp
 * @brief TFSocialSystem server->client sends: list/roster snapshot builders,
 *        roster delta broadcast and op-reply feedback, routed either straight
 *        into the local client mirror (listen host / standalone) or through
 *        NetworkManager for socket clients. Split from TFSocialSystem.cpp;
 *        the shared helpers live in TFSocialSystemInternal.h.
 */
#include "Game/TFSocialSystem.h"

#include "Game/TFSocialSystemInternal.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace Terrafront
{

    using namespace SocialDetail;

    // ---------------------------------------------------------------------------
    // Server: sends (local player -> direct mirror call, remote -> socket)
    // ---------------------------------------------------------------------------

    void TFSocialSystem::SendPayloadTo(PlayerId player, uint16_t msgId, const void* data, size_t size)
    {
        if (player == kInvalidPlayer)
            return;
        // Listen host / standalone: the local player is not a network client —
        // feed the client mirror directly (server + mirror share this instance).
        if (m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer && m_ctx->role != NetRole::Client)
        {
            if (msgId == kTFSocialMsg_OpReply)
                ClientHandleOpReply(data, size);
            else if (msgId == kTFSocialMsg_List)
                ClientHandleList(data, size);
            else if (msgId == kTFSocialMsg_Roster)
                ClientHandleRoster(data, size);
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
        if (size > 0)
            std::memcpy(msg.payload.data(), data, size);
        nm.SendToClient(player, msg);
#else
        (void)data;
        (void)size;
#endif
    }

    void TFSocialSystem::SendListTo(PlayerId player, SocialListKind kind)
    {
        auto onlineIt = m_online.find(player);
        if (onlineIt == m_online.end() || onlineIt->second.charId == 0)
            return;
        const SocialRecord& rec = m_store[onlineIt->second.charId];

        std::vector<TF_SocialEntry> entries;
        auto pushEntry = [&](const std::string& name)
        {
            TF_SocialEntry e{};
            CopyName(e.name, name);
            e.faction = static_cast<uint8_t>(FactionOfCharacterName(name));
            e.online = IsCharacterOnline(name) ? 1 : 0;
            entries.push_back(e);
        };

        switch (kind)
        {
        case SocialListKind::Friends:
            for (const std::string& n : rec.friends)
                pushEntry(n);
            break;
        case SocialListKind::Blocked:
            for (const std::string& n : rec.blocked)
                pushEntry(n);
            break;
        case SocialListKind::Recent:
            for (const RecentRec& r : rec.recent)
                pushEntry(r.name);
            break;
        default:
            return;
        }

        TF_SocialListHeader header{};
        header.listKind = static_cast<uint8_t>(kind);
        header.count = static_cast<uint8_t>(std::min<size_t>(entries.size(), 255));

        std::vector<uint8_t> buffer(sizeof(header) + entries.size() * sizeof(TF_SocialEntry));
        std::memcpy(buffer.data(), &header, sizeof(header));
        if (!entries.empty())
            std::memcpy(buffer.data() + sizeof(header), entries.data(), entries.size() * sizeof(TF_SocialEntry));
        SendPayloadTo(player, kTFSocialMsg_List, buffer.data(), buffer.size());
    }

    void TFSocialSystem::SendAllListsTo(PlayerId player)
    {
        SendListTo(player, SocialListKind::Friends);
        SendListTo(player, SocialListKind::Blocked);
        SendListTo(player, SocialListKind::Recent);
    }

    void TFSocialSystem::SendFullRosterTo(PlayerId player)
    {
        std::vector<TF_RosterEntry> entries;
        entries.reserve(m_online.size());
        for (const auto& [id, info] : m_online)
        {
            TF_RosterEntry e{};
            e.playerId = id;
            CopyName(e.name, info.name);
            e.faction = static_cast<uint8_t>(info.faction);
            e.online = 1;
            entries.push_back(e);
        }

        TF_RosterHeader header{};
        header.full = 1;
        header.count = static_cast<uint8_t>(std::min<size_t>(entries.size(), 255));

        std::vector<uint8_t> buffer(sizeof(header) + entries.size() * sizeof(TF_RosterEntry));
        std::memcpy(buffer.data(), &header, sizeof(header));
        if (!entries.empty())
            std::memcpy(buffer.data() + sizeof(header), entries.data(), entries.size() * sizeof(TF_RosterEntry));
        SendPayloadTo(player, kTFSocialMsg_Roster, buffer.data(), buffer.size());
    }

    void TFSocialSystem::BroadcastRosterDelta(PlayerId changed, bool online)
    {
        TF_RosterEntry entry{};
        entry.playerId = changed;
        entry.online = online ? 1 : 0;
        if (online)
        {
            auto it = m_online.find(changed);
            if (it != m_online.end())
            {
                CopyName(entry.name, it->second.name);
                entry.faction = static_cast<uint8_t>(it->second.faction);
            }
        }

        TF_RosterHeader header{};
        header.full = 0;
        header.count = 1;

        uint8_t buffer[sizeof(header) + sizeof(entry)];
        std::memcpy(buffer, &header, sizeof(header));
        std::memcpy(buffer + sizeof(header), &entry, sizeof(entry));

        for (const auto& [player, info] : m_online)
        {
            (void)info;
            if (player != changed)
                SendPayloadTo(player, kTFSocialMsg_Roster, buffer, sizeof(buffer));
        }
    }

    void TFSocialSystem::SendOpReplyTo(PlayerId player, SocialOp op, SocialOpResult result, const std::string& name)
    {
        TF_SocialOpReply rep{};
        rep.op = static_cast<uint8_t>(op);
        rep.result = static_cast<uint8_t>(result);
        CopyName(rep.targetName, name);
        SendPayloadTo(player, kTFSocialMsg_OpReply, &rep, sizeof(rep));
    }

} // namespace Terrafront
