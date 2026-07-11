/**
 * @file TFSocialSystem.cpp
 * @brief Friends / block list / recent players + online-player roster
 *        (see TFSocialSystem.h for the full design note).
 */
#include "Game/TFSocialSystem.h"

#include "Game/TFSquadSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Persistence/TFDatabase.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr float kSocialPollPeriodSec = 0.5f;   // enter/leave detection cadence
        constexpr float kSocialSaveDebounceSec = 5.0f; // recent-list flush debounce

        int64_t NowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        /// ASCII case-insensitive name compare (character names are alnum+space).
        bool NameEq(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        void CopyName(char (&out)[kTFSocialNameLen], const std::string& name)
        {
            const size_t n = std::min(name.size(), kTFSocialNameLen - 1);
            std::memcpy(out, name.data(), n);
            out[n] = '\0';
        }

        std::string NameFromWire(const char* field)
        {
            return std::string(field, strnlen(field, kTFSocialNameLen));
        }

        std::string TrimmedName(const std::string& raw)
        {
            size_t begin = 0, end = raw.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])))
                --end;
            return raw.substr(begin, end - begin);
        }

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

    TFSocialSystem::TFSocialSystem() = default;
    TFSocialSystem::~TFSocialSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSocialSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Recent-players sources (authority only; guarded inside the handlers).
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnBusPlayerKilled(ev); });
        events.Subscribe<EvSquadChanged>([this](const EvSquadChanged& ev) { OnBusSquadChanged(ev); });

        RegisterConsoleCommands();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSocialSystem initialized");
        return true;
    }

    void TFSocialSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        if (m_ctx->IsAuthority())
        {
            m_pollAccum += deltaTime;
            if (m_pollAccum >= kSocialPollPeriodSec)
            {
                m_pollAccum = 0.0f;
                ServerPollEnteredPlayers();
            }
            StoreFlushIfDue(deltaTime);
#ifdef ENABLE_NETWORKING
            if (ServerNetActive() && !m_serverHandlers)
                EnsureServerHandlers();
#endif
        }

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFSquadSystem pattern: register AFTER
        // link-up so ours wins the per-type handler slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            ResetMirror();
        }
#endif
    }

    void TFSocialSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFSocialSystem::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        if (m_serverHandlers)
            ReleaseServerHandlers();
#endif
        if (m_friendCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_friend");
            m_friendCmd = false;
        }
        if (m_blockCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_block");
            m_blockCmd = false;
        }
        if (m_storeDirty)
        {
            if (StoreSaveToDisk())
                m_storeDirty = false;
        }
        m_store.clear();
        m_online.clear();
        ResetMirror();
        m_storeLoaded = false;
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Client API
    // ---------------------------------------------------------------------------

    void TFSocialSystem::ClientSendOp(SocialOp op, const std::string& targetName)
    {
        if (!m_initialized || !m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;

        TF_SocialOp msg{};
        msg.op = static_cast<uint8_t>(op);
        CopyName(msg.targetName, TrimmedName(targetName));

        if (m_ctx->IsAuthority())
        {
            // Mirror the enter-world gate networked TF_SocialOp traffic is held to
            // (TFSquadSystem::SendOp precedent). The gate only exists under
            // ENABLE_NETWORKING — without it, onboarding is inert and social
            // features stay dormant by design.
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
            ServerHandleSocialOp(m_ctx->localPlayer, msg);
#endif
        }
        else if (m_ctx->clientNet)
        {
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFSocialMsg_Op), &msg, sizeof(msg));
        }
    }

    bool TFSocialSystem::NameOfPlayer(PlayerId player, std::string& out) const
    {
        auto it = m_roster.find(player);
        if (it == m_roster.end())
            return false;
        out = it->second.name;
        return true;
    }

    PlayerId TFSocialSystem::PlayerIdByName(const std::string& name) const
    {
        for (const auto& [id, view] : m_roster)
        {
            if (NameEq(view.name, name))
                return id;
        }
        return kInvalidPlayer;
    }

    FactionId TFSocialSystem::RosterFactionOf(PlayerId player) const
    {
        auto it = m_roster.find(player);
        return it == m_roster.end() ? FactionId::None : it->second.faction;
    }

    bool TFSocialSystem::IsBlockedName(const std::string& name) const
    {
        return std::any_of(m_blocked.begin(), m_blocked.end(),
                           [&](const EntryView& e) { return NameEq(e.name, name); });
    }

    bool TFSocialSystem::IsBlockedPlayer(PlayerId player) const
    {
        auto it = m_roster.find(player);
        return it != m_roster.end() && IsBlockedName(it->second.name);
    }

    bool TFSocialSystem::IsFriendName(const std::string& name) const
    {
        return std::any_of(m_friends.begin(), m_friends.end(),
                           [&](const EntryView& e) { return NameEq(e.name, name); });
    }

    std::vector<std::string> TFSocialSystem::DrainFriendNotices()
    {
        std::vector<std::string> out;
        out.swap(m_friendNotices);
        return out;
    }

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

    // ---------------------------------------------------------------------------
    // Persistence (atomic JSON, TFDatabase tmp+rename pattern)
    // ---------------------------------------------------------------------------

    bool TFSocialSystem::StoreEnsureLoaded()
    {
        if (m_storeLoaded)
            return true;
        if (m_storeLoadFailed)
            return false;

        namespace fs = std::filesystem;
        std::error_code existsEc;
        if (!fs::exists(m_storePath, existsEc))
        {
            m_storeLoaded = true; // fresh store
            return true;
        }

        std::ifstream in(m_storePath, std::ios::binary);
        std::ostringstream ss;
        if (in.is_open())
            ss << in.rdbuf();
        in.close();

        // Strict parse (W9): the lenient Parse accepts truncated/torn files as a
        // partial object, which could pass the structural checks below with
        // silently dropped rows; strict rejection routes them into quarantine.
        Spark::Json::Value root;
        std::string parseError;
        const bool parsedOk = Spark::Json::ParseStrict(ss.str(), &root, &parseError);
        if (!parsedOk)
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social store %s rejected by strict JSON parse: %s",
                            m_storePath.c_str(), parseError.c_str());
        if (!parsedOk || !root.IsObject() || !root.HasKey("characters") || !root["characters"].IsArray())
        {
            // Corrupt/foreign content: quarantine instead of silently overwriting
            // on the next flush (TFDatabase::Open precedent).
            std::error_code renameEc;
            const std::string backup = m_storePath + ".corrupt-" + std::to_string(NowMs()) + ".bak";
            fs::rename(m_storePath, backup, renameEc);
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social store unreadable; quarantined to %s (ok=%d)",
                            backup.c_str(), renameEc ? 0 : 1);
            m_storeLoadFailed = renameEc ? true : false; // writable again once quarantined
            m_storeLoaded = !m_storeLoadFailed;
            return m_storeLoaded;
        }

        const auto& arr = root["characters"];
        for (size_t i = 0; i < arr.Size(); ++i)
        {
            const auto& row = arr[i];
            if (!row.IsObject())
                continue;
            const auto charId = static_cast<uint64_t>(row["charId"].AsNumber(0.0));
            if (charId == 0)
                continue;
            SocialRecord rec;
            if (row.HasKey("friends") && row["friends"].IsArray())
            {
                const auto& friends = row["friends"];
                for (size_t f = 0; f < friends.Size(); ++f)
                    if (friends[f].IsString())
                        rec.friends.push_back(friends[f].AsString());
            }
            if (row.HasKey("blocked") && row["blocked"].IsArray())
            {
                const auto& blocked = row["blocked"];
                for (size_t b = 0; b < blocked.Size(); ++b)
                    if (blocked[b].IsString())
                        rec.blocked.push_back(blocked[b].AsString());
            }
            if (row.HasKey("recent") && row["recent"].IsArray())
            {
                const auto& recent = row["recent"];
                for (size_t r = 0; r < recent.Size(); ++r)
                {
                    const auto& rrow = recent[r];
                    if (!rrow.IsObject() || !rrow["name"].IsString())
                        continue;
                    RecentRec rr;
                    rr.name = rrow["name"].AsString();
                    rr.lastSeenMs = static_cast<int64_t>(rrow["lastSeenMs"].AsNumber(0.0));
                    rec.recent.push_back(std::move(rr));
                }
            }
            m_store[charId] = std::move(rec);
        }

        m_storeLoaded = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social store loaded (%zu character(s))", m_store.size());
        return true;
    }

    bool TFSocialSystem::StoreSaveToDisk() const
    {
        namespace fs = std::filesystem;

        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        Spark::Json::Value characters = Spark::Json::Value::MakeArray();
        for (const auto& [charId, rec] : m_store)
        {
            if (rec.friends.empty() && rec.blocked.empty() && rec.recent.empty())
                continue;
            Spark::Json::Value row = Spark::Json::Value::MakeObject();
            row["charId"] = Spark::Json::Value(static_cast<double>(charId));

            Spark::Json::Value friends = Spark::Json::Value::MakeArray();
            for (const std::string& n : rec.friends)
                friends.PushBack(Spark::Json::Value(n));
            row["friends"] = std::move(friends);

            Spark::Json::Value blocked = Spark::Json::Value::MakeArray();
            for (const std::string& n : rec.blocked)
                blocked.PushBack(Spark::Json::Value(n));
            row["blocked"] = std::move(blocked);

            Spark::Json::Value recent = Spark::Json::Value::MakeArray();
            for (const RecentRec& r : rec.recent)
            {
                Spark::Json::Value rrow = Spark::Json::Value::MakeObject();
                rrow["name"] = Spark::Json::Value(r.name);
                rrow["lastSeenMs"] = Spark::Json::Value(static_cast<double>(r.lastSeenMs));
                recent.PushBack(std::move(rrow));
            }
            row["recent"] = std::move(recent);

            characters.PushBack(std::move(row));
        }
        root["characters"] = std::move(characters);

        std::error_code ec;
        const auto parentPath = fs::path(m_storePath).parent_path();
        if (!parentPath.empty())
            fs::create_directories(parentPath, ec);

        const std::string tmpFile = m_storePath + ".tmp";
        {
            std::ofstream out(tmpFile, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                return false;
            out << Spark::Json::StringifyPretty(root);
            if (!out.good())
                return false;
        }

        fs::rename(tmpFile, m_storePath, ec); // atomic replace (MoveFileEx semantics)
        if (ec)
        {
            fs::remove(m_storePath, ec);
            fs::rename(tmpFile, m_storePath, ec);
            if (ec)
                return false;
        }
        return true;
    }

    void TFSocialSystem::StoreFlushIfDue(float dt)
    {
        if (!m_storeDirty)
        {
            m_saveAccum = 0.0f;
            return;
        }
        m_saveAccum += dt;
        if (m_saveAccum < kSocialSaveDebounceSec)
            return;
        m_saveAccum = 0.0f;
        if (StoreSaveToDisk())
            m_storeDirty = false;
    }

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
    // Net handler lifecycle
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFSocialSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    bool TFSocialSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server;
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

    // ---------------------------------------------------------------------------
    // Console commands (registered from Initialize — TFRegionSystem pattern)
    // ---------------------------------------------------------------------------

    void TFSocialSystem::RegisterConsoleCommands()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        auto joinedName = [](const std::vector<std::string>& args, size_t firstIndex) -> std::string
        {
            std::string name;
            for (size_t i = firstIndex; i < args.size(); ++i)
            {
                if (!name.empty())
                    name += ' ';
                name += args[i];
            }
            return name;
        };

        if (!console.HasCommand("tf_friend"))
        {
            console.RegisterCommand(
                "tf_friend",
                [this, joinedName](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] social system not ready";
                    if (args.size() < 2)
                        return "usage: tf_friend <add|remove> <character name>";
                    const std::string name = joinedName(args, 1);
                    if (args[0] == "add")
                        ClientSendOp(SocialOp::FriendAdd, name);
                    else if (args[0] == "remove")
                        ClientSendOp(SocialOp::FriendRemove, name);
                    else
                        return "usage: tf_friend <add|remove> <character name>";
                    return "[TF] friend " + args[0] + " '" + name + "' requested";
                },
                "Add or remove a friend by character name", "TERRAFRONT", "tf_friend <add|remove> <name>");
            m_friendCmd = true;
        }

        if (!console.HasCommand("tf_block"))
        {
            console.RegisterCommand(
                "tf_block",
                [this, joinedName](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] social system not ready";
                    if (args.size() < 2)
                        return "usage: tf_block <add|remove> <character name>";
                    const std::string name = joinedName(args, 1);
                    if (args[0] == "add")
                        ClientSendOp(SocialOp::BlockAdd, name);
                    else if (args[0] == "remove")
                        ClientSendOp(SocialOp::BlockRemove, name);
                    else
                        return "usage: tf_block <add|remove> <character name>";
                    return "[TF] block " + args[0] + " '" + name + "' requested";
                },
                "Block or unblock a character by name (blocked players' chat is hidden)", "TERRAFRONT",
                "tf_block <add|remove> <name>");
            m_blockCmd = true;
        }
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFSocialSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Social"))
            return;

        ImGui::Text("roster: %zu online   friends %zu   blocked %zu   recent %zu", m_roster.size(), m_friends.size(),
                    m_blocked.size(), m_recent.size());
        for (const auto& [id, view] : m_roster)
            ImGui::Text("  p%u '%s' [%s]", id, view.name.c_str(), FactionTag(view.faction));

        if (m_ctx && m_ctx->IsAuthority())
        {
            ImGui::Separator();
            ImGui::Text("server store: %zu character record(s)%s%s", m_store.size(), m_storeLoaded ? " (loaded)" : "",
                        m_storeDirty ? " (dirty)" : "");
            ImGui::Text("server online: %zu", m_online.size());
        }
#endif
    }

} // namespace Terrafront
