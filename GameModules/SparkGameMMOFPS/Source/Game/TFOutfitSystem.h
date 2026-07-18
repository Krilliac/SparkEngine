/**
 * @file TFOutfitSystem.h
 * @brief Outfits (PS2-style clans/guilds): create, invite/accept, ranks,
 *        kick, leave, disband — server-authoritative, per-CHARACTER membership.
 *
 * OWNERSHIP: outfits lane (this header + TFOutfitSystem.cpp,
 * Persistence/TFOutfitStore.h/.cpp, UI/TFOutfitPanel.h/.cpp).
 *
 * Design (mirrors TFSquadSystem: server registry + client mirror in one class):
 *  - SERVER (authority roles): owns the durable outfit registry via
 *    TFOutfitStore ("Saves/outfits.json", opened lazily like TFServerSim::
 *    EnsureAuthorityDatabaseOpen opens "Saves/terrafront.db"). Membership is
 *    keyed by the durable CHARACTER id (TFServerSim::ActiveCharacterOf), not
 *    the session PlayerId, so it survives reconnects. Player<->character
 *    binding is learned from ServerOnCharacterEntered (TFServerSim::
 *    HandleEnterWorld wiring — see wave wiringNotes) with an EvPlayerSpawned
 *    fallback so the system still functions before that hook is applied.
 *  - WIRE: reserved TFMsg block 0x5438-0x543F. C->S TF_OutfitRequest rides
 *    TFServerSim::RouteClientMessage (enter-world-gated choke point, wiring
 *    snippet in the wave report). S->C replies/rosters/tag-updates are sent
 *    directly by this system (TFSquadSystem::SendEchoTo pattern: direct
 *    mirror call for the listen-host/standalone local player, socket for
 *    real clients). Struct layouts live in TFOutfitSystemTypes.h (packed +
 *    static_asserted); TFNetProtocol.h only gains the enum ids (contended file).
 *  - CLIENT: mirror of the local player's outfit (roster + pending invite +
 *    last op result) plus a global PlayerId->tag map fed by broadcast
 *    TF_OutfitTagUpdate, exposed via GetOutfitTag() for nameplate/killfeed/
 *    scoreboard render sites (render-site snippets in the wave report).
 *    Client handlers self-register on pure clients (TFSquadSystem
 *    EnsureClientHandlers pattern) — no TFClientNet edits.
 *
 * Rank policy (server-enforced):
 *  - Leader: everything (invite, kick anyone, promote/demote, transfer
 *    leadership via SetRank(target, Leader) — old leader becomes Officer —
 *    and disband). Exactly one Leader per outfit.
 *  - Officer: invite + kick Members.
 *  - Member: leave only.
 *  - Leader leave auto-promotes (senior Officer, else senior Member);
 *    the last member leaving disbands the outfit.
 *
 * W12 outfit-leaderboards (score aggregation section):
 *  - SERVER: outfits accumulate competition score from member actions on
 *    existing surfaces only — EvPlayerKilled kills +1 (TFMedalSystem's
 *    suicide/team-kill filter), capture participation +10 (canonical XP
 *    reasons 4/5/6 riding EvXPAwarded) and alert wins +100 (kXPReasonAlert 13,
 *    TFAlertSystem's per-winning-participant payout). Persisted per outfit in
 *    outfits.json (weekly + all-time, additive keys); the weekly column rolls
 *    over by ISO week number via ONE shared RolloverIfNeeded() (called on
 *    server load and on the tick crossing the boundary).
 *  - WIRE: reserved block 0x5480-0x5483. The request is TFOutfitOp::
 *    Leaderboard on the already-routed TF_OutfitRequest (no new C->S id);
 *    0x5480 S->C TF_OutfitLeaderboard answers with the top 10 (ranked weekly,
 *    then all-time) + the requester's own row at its true rank.
 *  - CLIENT: LeaderboardMirror consumed by TFOutfitPanel's Leaderboard tab,
 *    which re-requests every kTFOutfitLbRefreshSec while visible.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Persistence/TFOutfitStore.h"
#include "Game/TFOutfitSystemTypes.h" // wire protocol: TFMsg ids, op/result enums, packed structs, tag-label helper

#include <string>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // System
    // ---------------------------------------------------------------------------

    class TFOutfitSystem
    {
      public:
        TFOutfitSystem();
        ~TFOutfitSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- cross-system API ----------------------------------------------------

        /// Outfit tag of `player` ("" when none / unknown). Valid until the next
        /// outfit mutation — copy it, do not hold the pointer across frames.
        /// Authority roles resolve live from the registry; pure clients read the
        /// broadcast tag map. THE nameplate/killfeed/scoreboard integration point.
        const char* GetOutfitTag(PlayerId player) const;

        /// Outfit id of `player` (0 == none/unknown). AUTHORITY-only truth —
        /// resolves live from the registry like GetOutfitTag's authority path;
        /// pure clients always get 0 (membership of OTHER players is not
        /// mirrored client-side). W8 ui-polish addition: the outfit chat
        /// channel's server-side recipient check (TFServerSim::HandleChatMsg).
        uint32_t OutfitIdOf(PlayerId player) const;

        /// Server: TFServerSim routes TFMsg 0x5438 (OutfitRequest) here
        /// (size-validates, then dispatches) — same one-line hook shape as
        /// TFSquadSystem::ServerHandleSquadMsgRaw.
        void ServerHandleOutfitMsgRaw(PlayerId sender, const void* data, size_t size);

        /// Server: player<->character binding, called from TFServerSim::
        /// HandleEnterWorld (preferred wiring — immediate tag/roster delivery).
        /// Idempotent; also reached via the EvPlayerSpawned fallback.
        void ServerOnCharacterEntered(PlayerId player, uint64_t charId, const std::string& charName);

        /// Server: session teardown, called from TFServerSim::CleanupPlayerSession
        /// (preferred wiring). The periodic entered-world sweep is the fallback.
        void ServerOnPlayerLeft(PlayerId player);

        // --- client entry points (TFOutfitPanel / social tab / console) ---------
        void ClientRequestCreate(const std::string& name, const std::string& tag);
        void ClientRequestInvite(const std::string& characterName);
        void ClientRequestAccept();
        void ClientRequestDecline();
        void ClientRequestLeave();
        void ClientRequestKick(uint64_t targetCharId);
        void ClientRequestSetRank(uint64_t targetCharId, TFOutfitRank rank);
        void ClientRequestDisband();
        void ClientRequestLeaderboard(); ///< W12: fetch/refresh the leaderboard snapshot

        // --- client mirror (read-only view for the UI) ---------------------------
        struct MirrorMember
        {
            uint64_t charId = 0;
            std::string name;
            TFOutfitRank rank = TFOutfitRank::Member;
            bool online = false;
        };

        struct Mirror
        {
            uint32_t outfitId = 0;   ///< 0 == not in an outfit
            uint64_t yourCharId = 0; ///< your member row's character id (from the roster)
            std::string name;
            std::string tag;
            uint16_t totalMembers = 0;
            std::vector<MirrorMember> members;

            // pending invite
            bool hasInvite = false;
            uint32_t inviteOutfitId = 0;
            std::string inviteName, inviteTag, inviteFrom;

            // last op result (UI status line; resultAge counts up in Update)
            bool hasResult = false;
            TFOutfitOp lastOp = TFOutfitOp::Create;
            TFOutfitResult lastResult = TFOutfitResult::Ok;
            float resultAge = 0.0f;
        };

        const Mirror& LocalMirror() const { return m_mirror; }
        bool InOutfit() const { return m_mirror.outfitId != 0; }
        TFOutfitRank LocalRank() const;

        // --- client leaderboard mirror (W12, read-only view for the UI) ---------
        struct LbRow
        {
            uint32_t outfitId = 0;
            uint32_t rank = 0;
            uint64_t weekly = 0;
            uint64_t allTime = 0;
            std::string name;
            std::string tag;
        };

        struct LeaderboardMirror
        {
            bool valid = false; ///< a TF_OutfitLeaderboard snapshot has arrived
            uint16_t totalOutfits = 0;
            uint32_t weekKey = 0; ///< ISO week of the weekly column
            int yourIndex = -1;   ///< index of your outfit in rows (-1 == none)
            std::vector<LbRow> rows;
        };

        const LeaderboardMirror& LocalLeaderboard() const { return m_lb; }

        // --- validation (public for UI pre-checks + unit tests) -----------------
        static bool ValidateOutfitName(const std::string& name); ///< 3-24, alnum + single internal spaces
        static bool ValidateOutfitTag(const std::string& tag);   ///< 2-5, alnum only

      private:
        struct BoundChar
        {
            uint64_t charId = 0;
            std::string name;
        };

        // server op handlers
        void ServerHandleOutfitMsg(PlayerId sender, const TF_OutfitRequest& req);
        TFOutfitResult ServerCreate(PlayerId sender, const std::string& name, const std::string& tag,
                                    uint32_t& outOutfitId);
        TFOutfitResult ServerInvite(PlayerId sender, const std::string& targetName, uint32_t& outOutfitId);
        TFOutfitResult ServerAccept(PlayerId sender, uint32_t& outOutfitId);
        TFOutfitResult ServerDecline(PlayerId sender);
        TFOutfitResult ServerLeave(PlayerId sender, uint32_t& outOutfitId);
        TFOutfitResult ServerKick(PlayerId sender, uint64_t targetCharId, uint32_t& outOutfitId);
        TFOutfitResult ServerSetRank(PlayerId sender, uint64_t targetCharId, TFOutfitRank rank, uint32_t& outOutfitId);
        TFOutfitResult ServerDisband(PlayerId sender, uint32_t& outOutfitId);

        // server helpers
        bool EnsureStoreOpen(); ///< lazy, authority-only (EnsureAuthorityDatabaseOpen pattern)
        const BoundChar* BoundCharOf(PlayerId player) const;
        PlayerId OnlinePlayerOfChar(uint64_t charId) const; ///< kInvalidPlayer if offline
        PlayerId FindOnlinePlayerByCharName(const std::string& name) const;
        void ServerRemoveMembership(uint32_t outfitId, uint64_t charId, PlayerId onlinePlayer);
        void ServerAfterLeaderChange(uint32_t outfitId); ///< auto-promote / disband-on-empty
        void SweepDepartedPlayers();

        // --- W12 competition score (server aggregation) -------------------------
        /// Weekly rollover — THE single shared path: called after a successful
        /// store open (server load) AND from the authority Update when a tick
        /// crosses the ISO-week boundary (m_lastWeekKey guards the sweep).
        void RolloverIfNeeded();
        void ServerAddOutfitScore(PlayerId player, uint32_t points); ///< resolves player -> outfit, no-op if none
        void ServerSendLeaderboard(PlayerId requester);              ///< build + send TF_OutfitLeaderboard
        void OnPlayerKilledScore(const EvPlayerKilled& ev);          ///< kills +1 (medal-system kill filter)
        void OnXPAwardedScore(const EvXPAwarded& ev);                ///< captures +10 (4/5/6), alert wins +100 (13)

        // wire send (server side)
        void SendReply(PlayerId player, TFOutfitOp op, TFOutfitResult result, uint32_t outfitId);
        void SendRosterTo(PlayerId player, const TFOutfitRecord* outfit); ///< nullptr == cleared roster
        void BroadcastRoster(const TFOutfitRecord& outfit);
        void BroadcastTag(PlayerId player, const std::string& tag);
        void SendTagTableTo(PlayerId player);
        void SendWireTo(PlayerId player, uint16_t msgId, const void* payload, size_t size);

        // client side
        void SendRequest(const TF_OutfitRequest& req);
        void ClientHandleWire(uint16_t msgId, const void* data, size_t size);
        void ClientHandleReply(const TF_OutfitReply& rep);
        void ClientHandleRoster(const TF_OutfitRoster& roster);
        void ClientHandleTagUpdate(const TF_OutfitTagUpdate& upd);
        void ClientHandleInvite(const TF_OutfitInvite& inv);
        void ClientHandleLeaderboard(const TF_OutfitLeaderboard& lb);
        void NoteResult(TFOutfitOp op, TFOutfitResult result);

        void OnPlayerSpawned(const EvPlayerSpawned& ev); ///< server binding fallback

        void RegisterConsoleCommands();

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        // --- server state (authority only) ---
        TFOutfitStore m_store;
        bool m_storeOpenFailed{false};                        ///< don't retry a quarantined/corrupt file every op
        std::unordered_map<PlayerId, BoundChar> m_boundChars; ///< entered players with a character
        std::unordered_map<uint64_t, PlayerId> m_playerOfChar;
        std::unordered_map<uint64_t, uint32_t> m_invites; ///< invitee charId -> outfit id
        float m_sweepAccum{0.0f};
        uint32_t m_lastWeekKey{0}; ///< last ISO week RolloverWeek was run for (0 == never)
        uint32_t m_scoreEvents{0}; ///< debug: score-yielding events aggregated this session

        // --- client state (all roles with a local player) ---
        Mirror m_mirror;
        LeaderboardMirror m_lb;
        std::unordered_map<PlayerId, std::string> m_tags; ///< broadcast tag map (pure-client render path)
        bool m_clientHandlers{false};
        bool m_cmdsRegistered{false};
        uint32_t m_badPackets{0};
    };

} // namespace Terrafront
