/**
 * @file TFSquadSystem.h
 * @brief Squads of 6: invite/accept/leave, squad spawn, waypoint.
 *
 * OWNERSHIP: this header + TFSquadSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W4 implementation (DESIGN §3/§4):
 *  - Server-side squad registry: per-faction squads, max kMaxSquadSize (6)
 *    members, leader-driven Invite/Kick/Promote/SetWaypoint, invite handshake
 *    (Invite -> Accept), auto-promote on leader leave, disconnect sweep.
 *  - Wire: TFMsg::SquadMsg both directions. C->S carries the op request;
 *    S->C reuses TF_SquadMsg as a compact state ECHO (op = what happened,
 *    targetPlayer = subject). A joiner receives one Accept echo per existing
 *    member (roster sync) + a Promote echo (leader) + the waypoint, so the
 *    client mirror needs no extra snapshot message.
 *  - Squad-leader spawn rule: GetSquadLeaderSpawn() serves TFPlayerSystem's
 *    spawnKind==3 with a 30 s per-requester cooldown tracked here.
 *  - Client mirror lives in this same class (local squad roster, pending
 *    invite, waypoint) — registered after link-up so it wins the handler slot
 *    over TFClientNet's accepted-but-unrouted no-op (TFRegionSystem pattern).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <unordered_map>
#include <vector>

namespace Terrafront
{

    constexpr SquadId kInvalidSquad = 0;            ///< 0 == "no squad"
    constexpr float kSquadSpawnCooldownSec = 30.0f; ///< DESIGN §4 squad-leader spawn cd

    class TFSquadSystem
    {
      public:
        TFSquadSystem();
        ~TFSquadSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- W4 cross-system API -------------------------------------------------

        /// Server: TFServerSim routes TFMsg::SquadMsg here (typed).
        void ServerHandleSquadMsg(PlayerId sender, const TF_SquadMsg& msg);

        /// Server: raw-payload convenience so the TFServerSim routing hook is one
        /// line (size-validates, then forwards to the typed handler).
        void ServerHandleSquadMsgRaw(PlayerId sender, const void* data, size_t size);

        /// Squad a player belongs to (kInvalidSquad if none). Server registry on
        /// the authority; falls back to the local mirror for the local player.
        SquadId SquadOf(PlayerId player) const;

        /// True when `player` is in the LOCAL player's squad. Unlike SquadOf,
        /// this also resolves OTHER players on pure clients (client mirror roster).
        bool IsLocalSquadMember(PlayerId player) const;

        /// Server: squad-leader spawn rule for TFPlayerSystem spawnKind==3.
        /// Succeeds only if `requester` is in a squad, is NOT the leader, the
        /// leader has an alive pawn, and the 30 s per-requester cooldown has
        /// elapsed — on success writes the leader's feet position to outPos and
        /// STARTS the cooldown. Returns false otherwise (query is then free).
        bool GetSquadLeaderSpawn(PlayerId requester, float outPos[3]);

        /// Server: seconds left on `requester`'s squad-spawn cooldown (0 = ready).
        /// For TF_SpawnReply{reason=timer, respawnDelay}.
        float SquadSpawnCooldownRemaining(PlayerId requester) const;

        // --- chat-social lane additions (additive; server stays authoritative) --

        /// Read-only copy of the LOCAL player's squad mirror for external UI
        /// (TFSocialPanel squad tab). Valid on every role: on the authority the
        /// local player's mirror is fed directly by SendEchoTo.
        struct LocalSquadView
        {
            SquadId squad = kInvalidSquad;
            PlayerId leader = kInvalidPlayer;
            std::vector<PlayerId> members;
            bool hasWaypoint = false;
            float wp[3]{};
            SquadId inviteSquad = kInvalidSquad; // pending invite (if any)
            PlayerId inviteFrom = kInvalidPlayer;
        };

        LocalSquadView GetLocalSquadView() const
        {
            LocalSquadView v;
            v.squad = m_mirror.squad;
            v.leader = m_mirror.leader;
            v.members = m_mirror.members;
            v.hasWaypoint = m_mirror.hasWaypoint;
            v.wp[0] = m_mirror.wp[0];
            v.wp[1] = m_mirror.wp[1];
            v.wp[2] = m_mirror.wp[2];
            v.inviteSquad = m_mirror.inviteSquad;
            v.inviteFrom = m_mirror.inviteFrom;
            return v;
        }

        /// External-UI entry to the existing (private) SendOp routing — direct
        /// server call on listen host / standalone, TFClientNet::SendMsg on pure
        /// clients. Same validation path as the debug-panel buttons.
        void UiSendOp(SquadOp op, PlayerId target = kInvalidPlayer, const float* wp = nullptr)
        {
            SendOp(op, target, wp);
        }

      private:
        // --- server registry ---
        struct Squad
        {
            SquadId id = kInvalidSquad;
            FactionId faction = FactionId::None;
            PlayerId leader = kInvalidPlayer;
            std::vector<PlayerId> members; // includes leader
            bool hasWaypoint = false;
            float wp[3]{};
        };

        // --- client mirror (also the listen-host local player's view) ---
        struct Mirror
        {
            SquadId squad = kInvalidSquad;
            PlayerId leader = kInvalidPlayer;
            std::vector<PlayerId> members;
            bool hasWaypoint = false;
            float wp[3]{};
            SquadId inviteSquad = kInvalidSquad; // pending invite
            PlayerId inviteFrom = kInvalidPlayer;
        };

        // server op handlers
        void ServerCreate(PlayerId sender);
        void ServerInvite(PlayerId sender, PlayerId target);
        void ServerAccept(PlayerId sender);
        void ServerKickOrLeave(PlayerId sender, PlayerId target, SquadOp op);
        void ServerPromote(PlayerId sender, PlayerId target);
        void ServerSetWaypoint(PlayerId sender, const TF_SquadMsg& msg);

        Squad* FindSquad(SquadId id);
        FactionId FactionOfPlayer(PlayerId player) const;
        SquadId AllocSquadId();
        /// Removes `player` from its squad (echoes `echoOp`, auto-promotes /
        /// disbands, fires EvSquadChanged). No-op if not in a squad.
        void RemoveFromSquad(PlayerId player, SquadOp echoOp);
        void SweepDisconnected();

        // wire helpers
        static TF_SquadMsg MakeEcho(SquadOp op, SquadId squad, PlayerId target);
        void SendEchoTo(PlayerId target, const TF_SquadMsg& echo);
        void EchoToMembers(const Squad& squad, const TF_SquadMsg& echo);

        // client mirror
        void ClientHandleEcho(const TF_SquadMsg& msg);
        void ResetMirror() { m_mirror = Mirror{}; }
        /// UI entry: routes an op to the authority (direct call on listen host /
        /// standalone, TFClientNet::SendMsg on pure clients).
        void SendOp(SquadOp op, PlayerId target, const float* wp = nullptr);

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0};

        // server state (authority only)
        std::unordered_map<SquadId, Squad> m_squads;
        std::unordered_map<PlayerId, SquadId> m_squadOf;
        std::unordered_map<PlayerId, SquadId> m_invites;     // invitee -> squad
        std::unordered_map<PlayerId, double> m_spawnCdUntil; // squad-spawn cd
        SquadId m_nextSquadId{1};
        float m_sweepAccum{0.0f};

        // client state
        Mirror m_mirror;
        bool m_clientHandlers{false};

#ifdef SPARK_HAS_IMGUI
        int m_uiTargetId{0}; // debug UI: invite/kick/promote target player id
#endif
    };

} // namespace Terrafront
