/**
 * @file TFSocialSystem.h
 * @brief Friends / block list / recent players + the online-player roster.
 *
 * OWNERSHIP: chat-social lane. Uniform module lifecycle (Initialize/Update/
 * FixedUpdate/Shutdown/RenderDebugUI) — wiring into Main.cpp is applied by the
 * integrator (see the lane wiring notes); until then the file compiles inert.
 *
 * Design (TFSquadSystem pattern — server registry + client mirror in ONE class):
 *  - SERVER (authority roles): polls TFServerSim's public enter-world state
 *    (IsEnteredWorld / ActiveCharacterOf) each 0.5 s to detect players entering
 *    or leaving the world — NO TFServerSim edits needed. On enter it resolves
 *    the character name/faction through ctx->db, pushes the player's persisted
 *    friends/blocked/recent snapshots plus a FULL online roster, and broadcasts
 *    a roster delta to everyone else. TF_SocialOp mutations arrive either via
 *    a self-registered NetworkManager handler (socket clients) or a direct call
 *    from ClientSendOp (listen-host/standalone local player), both gated on
 *    TFServerSim::IsEnteredWorld.
 *  - PERSISTENCE: own atomic-JSON file "Saves/terrafront_social.json" keyed by
 *    charId (TFDatabase tmp+rename pattern). TFDatabase itself is NOT touched —
 *    it is contended and its record schema belongs to the onboarding lane.
 *  - RECENT PLAYERS: kill/death pairs (EvPlayerKilled) and squad joins
 *    (EvSquadChanged) record both characters on each other's recent list
 *    (kTFMaxRecent, newest first). Saves are debounced (kSocialSaveDebounceSec).
 *  - CLIENT MIRROR: roster (playerId -> name/faction/online) + friends/blocked/
 *    recent views. Chat + social UI resolve player names and block state here.
 *    Handlers self-register after link-up (TFSquadSystem pattern).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFSocialProtocol.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    class TFSocialSystem
    {
      public:
        /// Client-side view of one friends/blocked/recent row.
        struct EntryView
        {
            std::string name;
            FactionId faction{FactionId::None};
            bool online{false};
        };

        /// Client-side view of one online player (name resolution for chat/UI).
        struct RosterView
        {
            std::string name;
            FactionId faction{FactionId::None};
        };

        TFSocialSystem();
        ~TFSocialSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- client API (UI: TFChatWindow / TFSocialPanel) ---------------------

        /// Send one friend/block mutation to the authority (direct call on
        /// listen host / standalone, TFClientNet::SendMsg on pure clients).
        void ClientSendOp(SocialOp op, const std::string& targetName);

        /// Resolve an online player's character name (roster mirror). False when
        /// unknown — callers fall back to "HOST"/"p<N>" labels.
        bool NameOfPlayer(PlayerId player, std::string& out) const;

        /// Reverse lookup: online player id by exact character name
        /// (kInvalidPlayer if that name is not currently in world).
        PlayerId PlayerIdByName(const std::string& name) const;

        /// FactionId of an online player from the roster (None if unknown).
        FactionId RosterFactionOf(PlayerId player) const;

        bool IsBlockedName(const std::string& name) const;
        bool IsBlockedPlayer(PlayerId player) const;
        bool IsFriendName(const std::string& name) const;

        const std::vector<EntryView>& Friends() const { return m_friends; }
        const std::vector<EntryView>& Blocked() const { return m_blocked; }
        const std::vector<EntryView>& Recent() const { return m_recent; }
        const std::unordered_map<PlayerId, RosterView>& Roster() const { return m_roster; }

        /// Last op feedback line for the social panel status row ("" if none).
        const std::string& LastOpFeedback() const { return m_lastOpFeedback; }

        /// One-shot "friend came online / went offline" lines for the chat
        /// window (drained by the consumer).
        std::vector<std::string> DrainFriendNotices();

        // --- server entry points -------------------------------------------------

        /// Authority: apply one TF_SocialOp from `sender` (size-validated raw
        /// payload). Called by the self-registered socket handler and by
        /// ClientSendOp on listen host / standalone.
        void ServerHandleSocialOpRaw(PlayerId sender, const void* data, size_t size);

      private:
        // --- server store (persisted, keyed by charId) --------------------------
        struct RecentRec
        {
            std::string name;
            int64_t lastSeenMs{0};
        };
        struct SocialRecord
        {
            std::vector<std::string> friends;
            std::vector<std::string> blocked;
            std::vector<RecentRec> recent;
        };
        struct OnlineInfo
        {
            uint64_t charId{0};
            std::string name;
            FactionId faction{FactionId::None};
        };

        // server logic
        void ServerPollEnteredPlayers();
        void ServerOnPlayerEntered(PlayerId player);
        void ServerOnPlayerLeft(PlayerId player);
        void ServerHandleSocialOp(PlayerId sender, const TF_SocialOp& op);
        void ServerTouchRecentPair(PlayerId a, PlayerId b);
        void ServerTouchRecent(uint64_t charId, const std::string& otherName);
        void OnBusPlayerKilled(const EvPlayerKilled& ev);
        void OnBusSquadChanged(const EvSquadChanged& ev);

        bool IsCharacterOnline(const std::string& name) const;
        FactionId FactionOfCharacterName(const std::string& name) const;

        // server sends (local player -> direct mirror call; remote -> socket)
        void SendPayloadTo(PlayerId player, uint16_t msgId, const void* data, size_t size);
        void SendListTo(PlayerId player, SocialListKind kind);
        void SendAllListsTo(PlayerId player);
        void SendFullRosterTo(PlayerId player);
        void BroadcastRosterDelta(PlayerId changed, bool online);
        void SendOpReplyTo(PlayerId player, SocialOp op, SocialOpResult result, const std::string& name);

        // persistence (atomic JSON, TFDatabase tmp+rename pattern)
        bool StoreEnsureLoaded();
        bool StoreSaveToDisk() const;
        void StoreMarkDirty() { m_storeDirty = true; }
        void StoreFlushIfDue(float dt);

        // client mirror
        void ClientHandleOpReply(const void* data, size_t size);
        void ClientHandleList(const void* data, size_t size);
        void ClientHandleRoster(const void* data, size_t size);
        void ResetMirror();
        void RefreshMirrorOnlineFlags();

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        bool ServerNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
        void EnsureServerHandlers();
        void ReleaseServerHandlers();
#endif

        void RegisterConsoleCommands();

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        // --- server state (authority only) --------------------------------------
        std::unordered_map<uint64_t, SocialRecord> m_store; // by charId
        bool m_storeLoaded{false};
        bool m_storeLoadFailed{false};
        mutable bool m_storeDirty{false};
        float m_saveAccum{0.0f};
        std::string m_storePath{"Saves/terrafront_social.json"};

        std::unordered_map<PlayerId, OnlineInfo> m_online;
        float m_pollAccum{0.0f};
        bool m_serverHandlers{false};

        // --- client mirror -------------------------------------------------------
        std::unordered_map<PlayerId, RosterView> m_roster;
        std::vector<EntryView> m_friends;
        std::vector<EntryView> m_blocked;
        std::vector<EntryView> m_recent;
        std::string m_lastOpFeedback;
        std::vector<std::string> m_friendNotices;
        bool m_clientHandlers{false};
        bool m_friendCmd{false};
        bool m_blockCmd{false};
    };

} // namespace Terrafront
