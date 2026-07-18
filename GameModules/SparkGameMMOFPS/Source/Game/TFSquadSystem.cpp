/**
 * @file TFSquadSystem.cpp
 * @brief Squads of 6 (W4): lifecycle (clock, disconnect-sweep cadence, net
 *        handler lifecycle gating), the SquadOf/IsLocalSquadMember queries and
 *        the debug UI. The server registry + TF_SquadMsg ops live in
 *        TFSquadSystemServer.cpp; the client mirror, UI waypoint entries and
 *        wire helpers live in TFSquadSystemClient.cpp; shared roster helpers
 *        live in TFSquadSystemInternal.h.
 *
 * Wire convention (see TFSquadSystem.h): C->S TF_SquadMsg is the op request;
 * S->C reuses the SAME struct as a state echo — op = what happened,
 * targetPlayer = subject player. Roster sync for a joiner is one Accept echo
 * per existing member + a Promote echo naming the leader + the waypoint.
 */
#include "Game/TFSquadSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <cstddef>

namespace Terrafront
{

    namespace
    {

        constexpr float kSweepPeriodSec = 1.0f; // disconnect sweep cadence

    } // namespace

    TFSquadSystem::TFSquadSystem() = default;
    TFSquadSystem::~TFSquadSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSquadSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSquadSystem initialized");
        return true;
    }

    void TFSquadSystem::Update(float deltaTime)
    {
        // W11 squad-v2 fix: the clock, disconnect sweep, and request-TTL used to
        // live in FixedUpdate — which Main.cpp's OnFixedUpdate NEVER calls for
        // this system, so m_clock froze at 0: one squad-leader spawn charged a
        // cooldown that never expired, and disconnected players were never swept
        // out of squads. They now run here (Update IS wired). None of this is
        // sim state — cooldown/TTL bookkeeping only — so frame-time cadence is
        // fine and determinism is untouched.
        m_clock += deltaTime;

        // Expire stale leader-side waypoint request pings (client mirror state).
        if (!m_mirror.wpRequests.empty())
            std::erase_if(m_mirror.wpRequests, [this](const WaypointRequest& r) { return r.until <= m_clock; });

        if (m_ctx && m_ctx->IsAuthority())
        {
            m_sweepAccum += deltaTime;
            if (m_sweepAccum >= kSweepPeriodSec)
            {
                m_sweepAccum = 0.0f;
                SweepDisconnected();
            }
        }

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle. Registered AFTER link-up so our real
        // handler wins the single per-type slot over TFClientNet's
        // accepted-but-unrouted no-op (TFRegionSystem pattern).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            ResetMirror();
        }

        // W11 squad-v2: 0x546C rides NetworkManager directly (block id outside
        // the frozen TFMsg enum), so the server route self-registers here — the
        // TFSocialSystem::EnsureServerHandlers pattern, enter-world gate mirrored.
        if (m_ctx && m_ctx->IsAuthority() && ServerNetActive() && !m_serverHandlers)
            EnsureServerHandlers();
#endif
    }

    void TFSquadSystem::FixedUpdate(float fixedDeltaTime)
    {
        // Intentionally empty (W11 squad-v2): Main.cpp's OnFixedUpdate never
        // wired this system, so everything time-based moved to Update() — see
        // the note there. Kept as a no-op for the frozen lifecycle contract.
        (void)fixedDeltaTime;
    }

    void TFSquadSystem::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        if (m_serverHandlers)
            ReleaseServerHandlers();
#endif
        m_squads.clear();
        m_squadOf.clear();
        m_invites.clear();
        m_spawnCdUntil.clear();
        ResetMirror();
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // W4 cross-system API
    // ---------------------------------------------------------------------------

    SquadId TFSquadSystem::SquadOf(PlayerId player) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            auto it = m_squadOf.find(player);
            return it == m_squadOf.end() ? kInvalidSquad : it->second;
        }
        if (m_ctx && player == m_ctx->localPlayer)
            return m_mirror.squad;
        return kInvalidSquad;
    }

    bool TFSquadSystem::IsLocalSquadMember(PlayerId player) const
    {
        if (!m_ctx || player == kInvalidPlayer)
            return false;
        if (m_ctx->IsAuthority())
        {
            const SquadId mine = SquadOf(m_ctx->localPlayer);
            return mine != kInvalidSquad && SquadOf(player) == mine;
        }
        if (m_mirror.squad == kInvalidSquad)
            return false;
        for (const PlayerId member : m_mirror.members)
            if (member == player)
                return true;
        return false;
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFSquadSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Squads"))
            return;

        // --- local player view (mirror) -----------------------------------------
        if (m_ctx && m_ctx->HasLocalPlayer())
        {
            if (m_mirror.squad == kInvalidSquad)
            {
                ImGui::TextUnformatted("no squad");
                if (ImGui::Button("Create squad"))
                    SendOp(SquadOp::Create, kInvalidPlayer);
                if (m_mirror.inviteSquad != kInvalidSquad)
                {
                    ImGui::Text("invite: squad %u from player %u", m_mirror.inviteSquad, m_mirror.inviteFrom);
                    ImGui::SameLine();
                    if (ImGui::Button("Accept"))
                        SendOp(SquadOp::Accept, kInvalidPlayer);
                }
            }
            else
            {
                ImGui::Text("squad %u  (%zu/%u members)", m_mirror.squad, m_mirror.members.size(), kMaxSquadSize);
                for (const PlayerId member : m_mirror.members)
                    ImGui::Text("  %s p%u%s", member == m_mirror.leader ? "*" : " ", member,
                                member == m_ctx->localPlayer ? " (you)" : "");
                if (m_mirror.hasWaypoint)
                {
                    ImGui::Text("waypoint: (%.0f %.0f %.0f)", m_mirror.wp[0], m_mirror.wp[1], m_mirror.wp[2]);
                    if (IsLocalSquadLeader())
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear##wp"))
                            UiClearWaypoint();
                    }
                }

                // W11 squad-v2: leader-side waypoint request pings.
                for (size_t i = 0; i < m_mirror.wpRequests.size(); ++i)
                {
                    const WaypointRequest& rq = m_mirror.wpRequests[i];
                    ImGui::Text("wp request p%u (%.0f %.0f %.0f)", rq.from, rq.wp[0], rq.wp[1], rq.wp[2]);
                    ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("Set"))
                    {
                        PromoteWaypointRequest(i);
                        ImGui::PopID();
                        break; // vector mutated — bail this frame
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X"))
                    {
                        DismissWaypointRequest(i);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }

                if (ImGui::Button("Leave"))
                    SendOp(SquadOp::Leave, kInvalidPlayer);

                const bool isLeader = m_mirror.leader == m_ctx->localPlayer;
                if (isLeader)
                {
                    ImGui::InputInt("target player", &m_uiTargetId);
                    const auto target = static_cast<PlayerId>(m_uiTargetId);
                    if (ImGui::Button("Invite"))
                        SendOp(SquadOp::Invite, target);
                    ImGui::SameLine();
                    if (ImGui::Button("Kick"))
                        SendOp(SquadOp::Kick, target);
                    ImGui::SameLine();
                    if (ImGui::Button("Promote"))
                        SendOp(SquadOp::Promote, target);

                    PawnInfo self{};
                    if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, self) && self.alive)
                    {
                        if (ImGui::Button("Set waypoint here"))
                            SendOp(SquadOp::SetWaypoint, kInvalidPlayer, self.pos);
                    }
                }
            }
            const float cd = SquadSpawnCooldownRemaining(m_ctx->localPlayer);
            if (cd > 0.0f)
                ImGui::Text("squad-spawn cooldown: %.0fs", cd);
        }

        // --- authority registry --------------------------------------------------
        if (m_ctx && m_ctx->IsAuthority())
        {
            ImGui::Separator();
            ImGui::Text("server squads : %zu   invites pending: %zu", m_squads.size(), m_invites.size());
            for (const auto& [id, squad] : m_squads)
            {
                ImGui::Text("squad %u [%s] leader p%u  %zu/%u%s", id, FactionTag(squad.faction), squad.leader,
                            squad.members.size(), kMaxSquadSize, squad.hasWaypoint ? "  wp" : "");
            }
        }
#endif // SPARK_HAS_IMGUI
    }

} // namespace Terrafront
