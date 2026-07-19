/**
 * @file TFAlertSystem.cpp
 * @brief Continent alerts core: lifecycle + tf_alert console command, the
 *        randomized 8-12 min scheduler, Territory Rush / Facility Control
 *        scoring and winner XP payouts. Wire + state view live in
 *        TFAlertSystemNet.cpp, the HUD banner / status report in
 *        TFAlertSystemUi.cpp. See TFAlertSystem.h for the full design note.
 */
#include "World/TFAlertSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "World/TFAlertSystemInternal.h" // AlertDetail: FactionOfIdx, PlayableFaction
#include "World/TFRegionSystem.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <random>
#include <string>

namespace Terrafront
{

    using namespace AlertDetail;

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

} // namespace Terrafront
