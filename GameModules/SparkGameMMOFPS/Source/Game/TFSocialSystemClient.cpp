/**
 * @file TFSocialSystemClient.cpp
 * @brief TFSocialSystem client mirror: roster / friends / blocked / recent
 *        views, op-reply feedback, friend online/offline notices and the
 *        client-side net handler lifecycle. Split from TFSocialSystem.cpp;
 *        the shared helpers live in TFSocialSystemInternal.h.
 */
#include "Game/TFSocialSystem.h"

#include "Game/TFSocialSystemInternal.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace Terrafront
{

    using namespace SocialDetail;

    namespace
    {

        const char* OpName(SocialOp op)
        {
            switch (op)
            {
            case SocialOp::FriendAdd:
                return "friend add";
            case SocialOp::FriendRemove:
                return "friend remove";
            case SocialOp::BlockAdd:
                return "block";
            case SocialOp::BlockRemove:
                return "unblock";
            case SocialOp::RequestLists:
                return "list refresh";
            default:
                return "?";
            }
        }

        const char* ResultName(SocialOpResult r)
        {
            switch (r)
            {
            case SocialOpResult::Ok:
                return "ok";
            case SocialOpResult::NotFound:
                return "no such character";
            case SocialOpResult::Self:
                return "that's you";
            case SocialOpResult::AlreadyListed:
                return "already on the list";
            case SocialOpResult::NotListed:
                return "not on the list";
            case SocialOpResult::ListFull:
                return "list is full";
            case SocialOpResult::BadName:
                return "bad name";
            case SocialOpResult::ServerError:
                return "server error";
            default:
                return "?";
            }
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Client mirror
    // ---------------------------------------------------------------------------

    void TFSocialSystem::ResetMirror()
    {
        m_roster.clear();
        m_friends.clear();
        m_blocked.clear();
        m_recent.clear();
        m_lastOpFeedback.clear();
        m_friendNotices.clear();
    }

    void TFSocialSystem::RefreshMirrorOnlineFlags()
    {
        auto refresh = [this](std::vector<EntryView>& list)
        {
            for (EntryView& e : list)
            {
                e.online = false;
                for (const auto& [id, view] : m_roster)
                {
                    (void)id;
                    if (NameEq(view.name, e.name))
                    {
                        e.online = true;
                        if (view.faction != FactionId::None)
                            e.faction = view.faction;
                        break;
                    }
                }
            }
        };
        refresh(m_friends);
        refresh(m_recent);
        refresh(m_blocked);
    }

    void TFSocialSystem::ClientHandleOpReply(const void* data, size_t size)
    {
        if (data == nullptr || size != sizeof(TF_SocialOpReply))
            return;
        TF_SocialOpReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        const std::string name = NameFromWire(rep.targetName);
        char line[96];
        std::snprintf(line, sizeof(line), "%s '%s': %s", OpName(static_cast<SocialOp>(rep.op)), name.c_str(),
                      ResultName(static_cast<SocialOpResult>(rep.result)));
        m_lastOpFeedback = line;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social: %s", line);
    }

    void TFSocialSystem::ClientHandleList(const void* data, size_t size)
    {
        if (data == nullptr || size < sizeof(TF_SocialListHeader))
            return;
        TF_SocialListHeader header;
        std::memcpy(&header, data, sizeof(header));
        if (size != sizeof(header) + static_cast<size_t>(header.count) * sizeof(TF_SocialEntry))
            return;

        std::vector<EntryView> list;
        list.reserve(header.count);
        const auto* bytes = static_cast<const uint8_t*>(data) + sizeof(header);
        for (uint8_t i = 0; i < header.count; ++i)
        {
            TF_SocialEntry e;
            std::memcpy(&e, bytes + static_cast<size_t>(i) * sizeof(TF_SocialEntry), sizeof(e));
            EntryView view;
            view.name = NameFromWire(e.name);
            view.faction = static_cast<FactionId>(e.faction);
            view.online = e.online != 0;
            list.push_back(std::move(view));
        }

        switch (static_cast<SocialListKind>(header.listKind))
        {
        case SocialListKind::Friends:
            m_friends = std::move(list);
            break;
        case SocialListKind::Blocked:
            m_blocked = std::move(list);
            break;
        case SocialListKind::Recent:
            m_recent = std::move(list);
            break;
        default:
            break;
        }
        RefreshMirrorOnlineFlags();
    }

    void TFSocialSystem::ClientHandleRoster(const void* data, size_t size)
    {
        if (data == nullptr || size < sizeof(TF_RosterHeader))
            return;
        TF_RosterHeader header;
        std::memcpy(&header, data, sizeof(header));
        if (size != sizeof(header) + static_cast<size_t>(header.count) * sizeof(TF_RosterEntry))
            return;

        if (header.full)
            m_roster.clear();

        const auto* bytes = static_cast<const uint8_t*>(data) + sizeof(header);
        for (uint8_t i = 0; i < header.count; ++i)
        {
            TF_RosterEntry e;
            std::memcpy(&e, bytes + static_cast<size_t>(i) * sizeof(TF_RosterEntry), sizeof(e));
            const std::string name = NameFromWire(e.name);
            if (e.online)
            {
                m_roster[e.playerId] = RosterView{name, static_cast<FactionId>(e.faction)};
                if (!header.full && IsFriendName(name))
                    m_friendNotices.push_back(name + " is online");
            }
            else
            {
                auto it = m_roster.find(e.playerId);
                if (it != m_roster.end())
                {
                    if (IsFriendName(it->second.name))
                        m_friendNotices.push_back(it->second.name + " went offline");
                    m_roster.erase(it);
                }
            }
        }
        RefreshMirrorOnlineFlags();
    }

    // ---------------------------------------------------------------------------
    // Client net handler lifecycle
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFSocialSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFSocialSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        nm.RegisterHandler(static_cast<MessageType>(kTFSocialMsg_OpReply), [this](const NetworkMessage& m)
                           { ClientHandleOpReply(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFSocialMsg_List),
                           [this](const NetworkMessage& m) { ClientHandleList(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFSocialMsg_Roster),
                           [this](const NetworkMessage& m) { ClientHandleRoster(m.payload.data(), m.payload.size()); });

        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social mirror handlers registered");
    }

    void TFSocialSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with no-ops so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFSocialMsg_OpReply, kTFSocialMsg_List, kTFSocialMsg_Roster})
        {
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        }
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
