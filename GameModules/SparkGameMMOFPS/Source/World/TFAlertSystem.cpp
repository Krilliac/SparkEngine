/**
 * @file TFAlertSystem.cpp
 * @brief Continent alerts: randomized 8-12 min scheduler, Territory Rush /
 *        Facility Control scoring, winner XP payouts, TF_AlertState wire
 *        traffic, and the local HUD banner + end splash. See TFAlertSystem.h
 *        for the full design note.
 */
#include "World/TFAlertSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "World/TFRegionSystem.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

namespace Terrafront
{

    namespace
    {
        /// Faction wire index (0..2) -> FactionId. Inverse of FactionIdx.
        FactionId FactionOfIdx(size_t idx)
        {
            return static_cast<FactionId>(idx + 1);
        }

        bool PlayableFaction(FactionId f)
        {
            return f == FactionId::MRA || f == FactionId::AUC || f == FactionId::HLX;
        }
    } // namespace

    const char* TFAlertName(TFAlertType type)
    {
        switch (type)
        {
        case TFAlertType::TerritoryRush:
            return "Territory Rush";
        case TFAlertType::FacilityControl:
            return "Facility Control";
        }
        return "Alert";
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    TFAlertSystem::TFAlertSystem() = default;
    TFAlertSystem::~TFAlertSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFAlertSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvRegionCaptured>([this](const EvRegionCaptured& ev) { OnRegionCaptured(ev); });
        events.Subscribe<EvXPAwarded>([this](const EvXPAwarded& ev) { OnXPAwarded(ev); });

        m_rng.seed(std::random_device{}());
        RerollIdleTimer();

        // Console self-registration (TFRegionSystem tf_capture_debug pattern —
        // no TFCommands.cpp edit needed).
        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_alert"))
        {
            console.RegisterCommand(
                "tf_alert",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] alert system not ready";
                    const std::string sub = args.empty() ? "status" : args[0];
                    if (sub == "status")
                        return StatusString();
                    if (!AuthorityActive())
                        return "[TF] tf_alert " + sub + ": authority roles only";
                    if (sub == "start")
                    {
                        TFAlertType type = TFAlertType::TerritoryRush;
                        if (args.size() >= 2 && args[1] == "facility")
                            type = TFAlertType::FacilityControl;
                        else if (args.size() >= 2 && args[1] != "rush")
                            return "[TF] tf_alert start: unknown type '" + args[1] + "' (rush|facility)";
                        return ServerStartAlert(type) ? "[TF] alert started: " + std::string(TFAlertName(type))
                                                      : "[TF] failed to start alert (data tables loaded?)";
                    }
                    if (sub == "stop")
                        return ServerStopAlert() ? "[TF] alert ended early (winner/XP flow ran)"
                                                 : "[TF] no alert running";
                    return "[TF] usage: tf_alert [start [rush|facility] | stop | status]";
                },
                "Continent alerts: start/stop a timed event or show scheduler status", "TERRAFRONT",
                "tf_alert [start [rush|facility] | stop | status]");
            m_consoleCmd = true;
        }

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFAlertSystem initialized (next auto alert in %.0fs)",
                       static_cast<double>(m_idleLeft));
        return true;
    }

    void TFAlertSystem::Shutdown()
    {
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        m_knownClients.clear();
#endif
        if (m_consoleCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_alert");
            m_consoleCmd = false;
        }
        m_phase = TFAlertPhase::Idle;
        for (auto& set : m_participants)
            set.clear();
        m_mirrorValid = false;
        m_initialized = false;
    }

    void TFAlertSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFMedalSystem pattern: registered
        // after link-up so the real handler wins the per-type slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_mirrorValid = false;
        }
#endif

        if (m_ctx->IsAuthority())
        {
            TickServer(deltaTime);
#ifdef ENABLE_NETWORKING
            PollJoins();
#endif
        }
        else if (m_mirrorValid && m_mirror.phase == static_cast<uint8_t>(TFAlertPhase::Ended))
        {
            // Pure client: run the splash TTL down locally; the server's
            // follow-up Idle broadcast clears it anyway (belt and braces).
            m_mirror.secondsLeft -= deltaTime;
            if (m_mirror.secondsLeft <= 0.0f)
                m_mirror.phase = static_cast<uint8_t>(TFAlertPhase::Idle);
        }
    }

    void TFAlertSystem::FixedUpdate(float)
    {
        // Nothing simulation-facing lives here (determinism contract): all alert
        // logic is wall-clock server bookkeeping in Update().
    }

    // ---------------------------------------------------------------------------
    // Server
    // ---------------------------------------------------------------------------

    bool TFAlertSystem::AuthorityActive() const
    {
        return m_initialized && m_ctx && m_ctx->IsAuthority();
    }

    uint32_t TFAlertSystem::AlivePawnCount() const
    {
        if (!m_ctx || !m_ctx->players)
            return 0;
        uint32_t alive = 0;
        m_ctx->players->ForEachAlivePawn([&](const PawnInfo&) { ++alive; });
        return alive;
    }

    RegionId TFAlertSystem::ResolveFacilityTarget() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return kInvalidRegion;
        const auto& regions = m_ctx->data->GetContinent().regions;
        for (const RegionDef& r : regions)
        {
            if (r.key == "fluxwell")
                return r.id;
        }
        for (const RegionDef& r : regions)
        {
            if (r.tier == "facility")
                return r.id;
        }
        return kInvalidRegion;
    }

    void TFAlertSystem::RerollIdleTimer()
    {
        std::uniform_real_distribution<float> dist(kTFAlertIdleMinSec, kTFAlertIdleMaxSec);
        m_idleLeft = dist(m_rng);
    }

    void TFAlertSystem::TickServer(float dt)
    {
        switch (m_phase)
        {
        case TFAlertPhase::Idle:
        {
            // "Uneventful play": no alert running. The window only counts down
            // while somebody is actually playing (alive pawns include bots).
            if (AlivePawnCount() == 0)
                return;
            m_idleLeft -= dt;
            if (m_idleLeft <= 0.0f)
            {
                std::uniform_int_distribution<int> coin(0, 1);
                TFAlertType type = coin(m_rng) == 0 ? TFAlertType::TerritoryRush : TFAlertType::FacilityControl;
                if (type == TFAlertType::FacilityControl && ResolveFacilityTarget() == kInvalidRegion)
                    type = TFAlertType::TerritoryRush; // tables not loaded / no facility
                ServerStartAlert(type);
            }
            return;
        }
        case TFAlertPhase::Running:
        {
            m_secondsLeft -= dt;

            // Facility Control: score == whole seconds of target ownership.
            if (m_type == TFAlertType::FacilityControl && m_ctx->regions && m_target != kInvalidRegion)
            {
                const FactionId owner = m_ctx->regions->OwnerOf(m_target);
                if (PlayableFaction(owner))
                {
                    const size_t idx = FactionIdx(owner);
                    m_holdAccum[idx] += dt;
                    m_scores[idx] = static_cast<uint32_t>(m_holdAccum[idx]);
                }
            }

            m_broadcastAccum += dt;
            if (m_broadcastAccum >= 1.0f / kTFAlertBroadcastHz)
            {
                m_broadcastAccum = 0.0f;
                BroadcastState();
                if (m_events)
                    m_events->Fire(EvAlertScoreTick{*std::max_element(m_scores.begin(), m_scores.end())});
            }

            if (m_secondsLeft <= 0.0f)
                EndAlert("timer");
            return;
        }
        case TFAlertPhase::Ended:
        {
            m_secondsLeft -= dt;
            if (m_secondsLeft <= 0.0f)
            {
                m_phase = TFAlertPhase::Idle;
                m_winner = FactionId::None;
                RerollIdleTimer();
                BroadcastState(); // clear remote mirrors
            }
            return;
        }
        }
    }

    bool TFAlertSystem::ServerStartAlert(TFAlertType type)
    {
        if (!AuthorityActive())
            return false;

        RegionId target = kInvalidRegion;
        if (type == TFAlertType::FacilityControl)
        {
            target = ResolveFacilityTarget();
            if (target == kInvalidRegion)
                return false;
        }

        // Restart-safe: a running alert is silently reset (console/testing path;
        // the scheduler itself only starts from Idle). No payout on the loser.
        m_phase = TFAlertPhase::Running;
        m_type = type;
        m_target = target;
        m_secondsLeft = kTFAlertDurationSec;
        m_scores.fill(0);
        m_holdAccum.fill(0.0f);
        for (auto& set : m_participants)
            set.clear();
        m_winner = FactionId::None;
        m_broadcastAccum = 0.0f;
        ++m_alertsStarted;

        BroadcastState();
        if (m_events)
            m_events->Fire(EvAlertStarted{type, target});
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] ALERT started: %s (%.0f s, target region %u)", TFAlertName(type),
                       static_cast<double>(kTFAlertDurationSec), static_cast<unsigned>(target));
        return true;
    }

    bool TFAlertSystem::ServerStopAlert()
    {
        if (!AuthorityActive() || m_phase != TFAlertPhase::Running)
            return false;
        EndAlert("console stop");
        return true;
    }

    void TFAlertSystem::EndAlert(const char* how)
    {
        // Winner = strictly highest score; any tie at the top is a draw.
        const uint32_t top = *std::max_element(m_scores.begin(), m_scores.end());
        size_t winners = 0;
        size_t winIdx = 0;
        for (size_t i = 0; i < m_scores.size(); ++i)
        {
            if (m_scores[i] == top)
            {
                ++winners;
                winIdx = i;
            }
        }
        m_winner = (top > 0 && winners == 1) ? FactionOfIdx(winIdx) : FactionId::None;

        // XP bonus to the winning faction's participants through the real
        // progression seam (kXPReasonAlert; the directives/medals precedent).
        if (m_winner != FactionId::None && m_ctx && m_ctx->progression)
        {
            for (PlayerId p : m_participants[FactionIdx(m_winner)])
            {
                m_ctx->progression->ServerAwardXP(p, kTFAlertWinXP, kXPReasonAlert);
                ++m_xpPaid;
            }
        }

        m_phase = TFAlertPhase::Ended;
        m_secondsLeft = kTFAlertEndSplashSec;
        ++m_alertsEnded;

        BroadcastState();
        if (m_events)
            m_events->Fire(EvAlertEnded{m_type, m_winner, top});
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] ALERT ended (%s): %s winner=%s topScore=%u paidParticipants=%u",
                       how, TFAlertName(m_type), FactionTag(m_winner), top,
                       m_winner != FactionId::None ? static_cast<unsigned>(m_participants[FactionIdx(m_winner)].size())
                                                   : 0u);
    }

    // ---------------------------------------------------------------------------
    // Scoring feeds (existing bus events only)
    // ---------------------------------------------------------------------------

    void TFAlertSystem::OnRegionCaptured(const EvRegionCaptured& ev)
    {
        if (!AuthorityActive() || m_phase != TFAlertPhase::Running || m_type != TFAlertType::TerritoryRush)
            return;
        if (PlayableFaction(ev.newOwner))
            m_scores[FactionIdx(ev.newOwner)] += kTFAlertScorePerFlip;
    }

    void TFAlertSystem::OnXPAwarded(const EvXPAwarded& ev)
    {
        if (!AuthorityActive() || m_phase != TFAlertPhase::Running)
            return;
        // Canonical capture-credit reasons (TFProgressionSystem.h 4/5/6) — the
        // same per-player capture participation TFMedalSystem counts. NOTE:
        // kXPReasonAlert itself never matches, so the end-of-alert payout can
        // not re-enter the participant set.
        if (ev.reason != kXPReasonCaptureFacility && ev.reason != kXPReasonCaptureFort &&
            ev.reason != kXPReasonCaptureOutpost)
            return;
        const FactionId f = m_ctx->players ? m_ctx->players->FactionOf(ev.player) : FactionId::None;
        if (!PlayableFaction(f))
            return;
        m_participants[FactionIdx(f)].insert(ev.player);
        if (m_type == TFAlertType::TerritoryRush)
            m_scores[FactionIdx(f)] += kTFAlertScorePerCaptureTick;
    }

    // ---------------------------------------------------------------------------
    // State view + wire
    // ---------------------------------------------------------------------------

    void TFAlertSystem::BuildState(TF_AlertState& out) const
    {
        out = TF_AlertState{};
        out.phase = static_cast<uint8_t>(m_phase);
        out.type = static_cast<uint8_t>(m_type);
        out.winner = static_cast<uint8_t>(m_winner);
        out.regionId = m_target;
        for (size_t i = 0; i < m_scores.size(); ++i)
            out.score[i] = m_scores[i];
        out.secondsLeft = std::max(m_secondsLeft, 0.0f);
    }

    void TFAlertSystem::GetView(TF_AlertState& out) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            BuildState(out);
            return;
        }
        if (m_mirrorValid)
        {
            out = m_mirror;
            return;
        }
        out = TF_AlertState{};
        out.phase = static_cast<uint8_t>(TFAlertPhase::Idle);
        out.regionId = kInvalidRegion;
    }

    void TFAlertSystem::BroadcastState()
    {
        TF_AlertState st{};
        BuildState(st);
        ++m_statesSent;
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
        {
            for (const auto& [id, info] : nm.GetClients())
            {
                if (info.state == Spark::Net::ConnectionState::Connected)
                    SendWire(id, st);
            }
        }
#endif
        // Listen host / standalone local player: RenderUI reads the server
        // state directly through GetView — no loopback mirror write needed.
    }

    void TFAlertSystem::ClientHandleState(const TF_AlertState& st)
    {
        m_mirror = st;
        m_mirrorValid = true;
        ++m_statesRx;
    }

#ifdef ENABLE_NETWORKING

    bool TFAlertSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFAlertSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgAlertState),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_AlertState))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_AlertState st{};
                               std::memcpy(&st, m.payload.data(), sizeof(st));
                               ClientHandleState(st);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] alert mirror handler registered");
    }

    void TFAlertSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgAlertState),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFAlertSystem::SendWire(PlayerId target, const TF_AlertState& st)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFMsgAlertState);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(sizeof(st));
        std::memcpy(msg.payload.data(), &st, sizeof(st));
        nm.SendToClient(target, msg);
    }

    void TFAlertSystem::PollJoins()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;

        // Late-joiner burst: a fresh client gets the current state once while
        // an alert is running or its end splash is showing (idle mirrors are
        // the client default — no traffic needed).
        for (const auto& [id, info] : nm.GetClients())
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            if (m_phase != TFAlertPhase::Idle)
            {
                TF_AlertState st{};
                BuildState(st);
                SendWire(id, st);
                ++m_statesSent;
            }
        }

        // Leaver sweep: recycled-PlayerId hygiene for the participant sets
        // (the TFMedalSystem PollJoinsLeaves precedent).
        std::erase_if(m_knownClients,
                      [this, &nm](PlayerId id)
                      {
                          if (nm.GetClients().contains(id))
                              return false;
                          for (auto& set : m_participants)
                              set.erase(id);
                          return true;
                      });
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Console / status
    // ---------------------------------------------------------------------------

    std::string TFAlertSystem::StatusString() const
    {
        TF_AlertState st{};
        GetView(st);
        std::ostringstream os;
        os << "[TF] alerts: ";
        switch (static_cast<TFAlertPhase>(st.phase))
        {
        case TFAlertPhase::Running:
        {
            os << TFAlertName(static_cast<TFAlertType>(st.type)) << " RUNNING, " << static_cast<int>(st.secondsLeft)
               << "s left";
            if (static_cast<TFAlertType>(st.type) == TFAlertType::FacilityControl)
                os << " (target region " << st.regionId << ")";
            os << "\n  scores: MRA=" << st.score[0] << " AUC=" << st.score[1] << " HLX=" << st.score[2];
            break;
        }
        case TFAlertPhase::Ended:
            os << TFAlertName(static_cast<TFAlertType>(st.type))
               << " ENDED, winner=" << FactionTag(static_cast<FactionId>(st.winner)) << "  scores: MRA=" << st.score[0]
               << " AUC=" << st.score[1] << " HLX=" << st.score[2];
            break;
        case TFAlertPhase::Idle:
        default:
            os << "idle";
            if (m_ctx && m_ctx->IsAuthority())
                os << ", next auto alert in " << static_cast<int>(m_idleLeft) << "s (needs alive pawns)";
            break;
        }
        return os.str();
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFAlertSystem::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        TF_AlertState st{};
        GetView(st);
        const TFAlertPhase phase = static_cast<TFAlertPhase>(st.phase);
        if (phase == TFAlertPhase::Idle)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float width = 380.0f;
        // Just under the HUD compass strip (top 12 px + 26 px tall) and above
        // the medal toast band (0.16 * height).
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - width) * 0.5f, vp->Pos.y + 46.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav;
        if (!ImGui::Begin("##tf_alert_banner", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        const char* name = TFAlertName(static_cast<TFAlertType>(st.type));

        if (phase == TFAlertPhase::Running)
        {
            const int total = static_cast<int>(st.secondsLeft);
            char head[96];
            std::snprintf(head, sizeof(head), "ALERT: %s  %d:%02d", name, total / 60, total % 60);
            const float headW = ImGui::CalcTextSize(head).x;
            ImGui::SetCursorPosX((width - headW) * 0.5f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "%s", head);

            if (static_cast<TFAlertType>(st.type) == TFAlertType::FacilityControl && m_ctx->data &&
                m_ctx->data->IsLoaded())
            {
                const auto& regions = m_ctx->data->GetContinent().regions;
                if (st.regionId < regions.size())
                {
                    const std::string& rn = regions[st.regionId].name;
                    const float rw = ImGui::CalcTextSize(rn.c_str()).x;
                    ImGui::SetCursorPosX((width - rw) * 0.5f);
                    ImGui::TextDisabled("%s", rn.c_str());
                }
            }

            // Per-faction score bars, normalized to the current leader.
            uint32_t top = 1;
            for (uint32_t s : st.score)
                top = std::max(top, s);
            for (size_t i = 0; i < 3; ++i)
            {
                const FactionId f = FactionOfIdx(i);
                float col[4];
                FactionColor(f, col);
                char label[48];
                std::snprintf(label, sizeof(label), "%s %u", FactionTag(f), st.score[i]);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(col[0], col[1], col[2], 0.90f));
                ImGui::ProgressBar(static_cast<float>(st.score[i]) / static_cast<float>(top), ImVec2(-1.0f, 15.0f),
                                   label);
                ImGui::PopStyleColor();
            }
        }
        else // Ended: victory / defeat / draw splash
        {
            const FactionId winner = static_cast<FactionId>(st.winner);
            const FactionId mine = m_ctx->localFaction;
            const char* verdict = "ALERT OVER - DRAW"; // ASCII only: default ImGui font has no em-dash glyph
            ImVec4 color(0.75f, 0.75f, 0.75f, 1.0f);
            char buf[96];
            if (winner != FactionId::None)
            {
                if (PlayableFaction(mine) && mine == winner)
                {
                    std::snprintf(buf, sizeof(buf), "VICTORY - %s", name);
                    verdict = buf;
                    color = ImVec4(0.35f, 0.95f, 0.40f, 1.0f);
                }
                else if (PlayableFaction(mine))
                {
                    std::snprintf(buf, sizeof(buf), "DEFEAT - %s wins %s", FactionTag(winner), name);
                    verdict = buf;
                    color = ImVec4(0.95f, 0.30f, 0.25f, 1.0f);
                }
                else
                {
                    std::snprintf(buf, sizeof(buf), "%s wins %s", FactionName(winner), name);
                    verdict = buf;
                }
            }
            const float w = ImGui::CalcTextSize(verdict).x;
            ImGui::SetCursorPosX((width - w) * 0.5f);
            ImGui::TextColored(color, "%s", verdict);

            char scores[96];
            std::snprintf(scores, sizeof(scores), "MRA %u  |  AUC %u  |  HLX %u", st.score[0], st.score[1],
                          st.score[2]);
            const float sw = ImGui::CalcTextSize(scores).x;
            ImGui::SetCursorPosX((width - sw) * 0.5f);
            ImGui::TextDisabled("%s", scores);
        }

        ImGui::End();
    }

    void TFAlertSystem::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("Alerts (TFAlertSystem)"))
            return;
        TF_AlertState st{};
        GetView(st);
        ImGui::Text("phase=%u type=%s target=%u secondsLeft=%.1f", static_cast<unsigned>(st.phase),
                    TFAlertName(static_cast<TFAlertType>(st.type)), static_cast<unsigned>(st.regionId),
                    static_cast<double>(st.secondsLeft));
        ImGui::Text("scores: MRA=%u AUC=%u HLX=%u", st.score[0], st.score[1], st.score[2]);
        if (m_ctx && m_ctx->IsAuthority())
            ImGui::Text("idleLeft=%.0fs participants=%zu/%zu/%zu", static_cast<double>(m_idleLeft),
                        m_participants[0].size(), m_participants[1].size(), m_participants[2].size());
        ImGui::Text("started=%u ended=%u xpPaid=%u sent=%u rx=%u bad=%u", m_alertsStarted, m_alertsEnded, m_xpPaid,
                    m_statesSent, m_statesRx, m_badPackets);
    }

#else

    void TFAlertSystem::RenderUI() {}
    void TFAlertSystem::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
