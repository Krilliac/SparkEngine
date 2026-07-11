/**
 * @file TFDirectiveSystem.cpp
 * @brief Directive progress tracking + tiered payouts (see header for design).
 *
 * Event flow: EvPlayerKilled / EvVehicleDestroyed / EvXPAwarded(capture) /
 * EvDeployablePlaced (W8) -> AddProgress -> tier crossings ->
 * TFProgressionSystem::ServerAwardXP (which fires the client TF_XPEvent toast
 * for free) + ServerGrantFlux. The DeployItems diff-poll over
 * TFDeployableSystem survives as a fallback for placements that appear
 * without the event; the shared known-entity set prevents double credit.
 */
#include "Game/TFDirectiveSystem.h"

#include "Game/TFDeployableSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <string>

namespace Terrafront
{

    namespace
    {

        /// DeployItems diff-poll cadence. Placements are rare (one per player per
        /// kind, replace-destroys), so 0.5 s is plenty and keeps the sweep cheap.
        constexpr float kDeployPollSec = 0.5f;

        bool IsCaptureReason(uint8_t reason)
        {
            return reason == kXPReasonCaptureFacility || reason == kXPReasonCaptureFort ||
                   reason == kXPReasonCaptureOutpost;
        }

    } // namespace

    TFDirectiveSystem::TFDirectiveSystem() = default;
    TFDirectiveSystem::~TFDirectiveSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDirectiveSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });
        events.Subscribe<EvVehicleDestroyed>([this](const EvVehicleDestroyed& ev) { OnVehicleDestroyed(ev); });
        events.Subscribe<EvXPAwarded>([this](const EvXPAwarded& ev) { OnXPAwarded(ev); });
        events.Subscribe<EvDeployablePlaced>([this](const EvDeployablePlaced& ev) { OnDeployablePlaced(ev); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDirectiveSystem initialized (%zu directives, %zu tiers each)",
                       kTFDirectiveCount, kTFDirectiveTiers);
        return true;
    }

    void TFDirectiveSystem::Update(float deltaTime)
    {
        // NOTE: Main.cpp does not route FixedUpdate here (TFProgressionSystem
        // precedent); the deploy poll is paced on the frame update.
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        PollDeployables(deltaTime);
    }

    void TFDirectiveSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFDirectiveSystem::Shutdown()
    {
        m_players.clear();
        m_knownDeployables.clear();
        m_deploySeeded = false;
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Cross-system surface
    // ---------------------------------------------------------------------------

    void TFDirectiveSystem::ForEachStatus(PlayerId player, const std::function<void(const Status&)>& fn) const
    {
        const auto it = m_players.find(player);
        for (size_t i = 0; i < kTFDirectiveCount; ++i)
        {
            Status s{};
            s.def = &kTFDirectives[i];
            s.progress = it != m_players.end() ? it->second.progress[i] : 0u;
            s.tiersDone = it != m_players.end() ? it->second.tiersDone[i] : uint8_t{0};
            fn(s);
        }
    }

    uint32_t TFDirectiveSystem::ProgressOf(PlayerId player, uint16_t directiveId) const
    {
        const int idx = IndexOfId(directiveId);
        if (idx < 0)
            return 0;
        const auto it = m_players.find(player);
        return it != m_players.end() ? it->second.progress[static_cast<size_t>(idx)] : 0u;
    }

    uint8_t TFDirectiveSystem::TiersDoneOf(PlayerId player, uint16_t directiveId) const
    {
        const int idx = IndexOfId(directiveId);
        if (idx < 0)
            return 0;
        const auto it = m_players.find(player);
        return it != m_players.end() ? it->second.tiersDone[static_cast<size_t>(idx)] : uint8_t{0};
    }

    void TFDirectiveSystem::ClearPlayer(PlayerId player)
    {
        m_players.erase(player);
    }

    // ---------------------------------------------------------------------------
    // Progress sources
    // ---------------------------------------------------------------------------

    void TFDirectiveSystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        if (ev.killer == kInvalidPlayer || ev.killer == ev.victim)
            return; // environment death / suicide: no credit

        // No credit for team kills (mirrors TFProgressionSystem::OnPlayerKilled).
        if (m_ctx->players)
        {
            const FactionId kf = m_ctx->players->FactionOf(ev.killer);
            const FactionId vf = m_ctx->players->FactionOf(ev.victim);
            if (kf != FactionId::None && kf == vf)
                return;
        }

        AddProgress(ev.killer, TFDirectiveKind::KillInfantry, static_cast<uint8_t>(ev.headshot ? 1 : 0), 1);
    }

    void TFDirectiveSystem::OnVehicleDestroyed(const EvVehicleDestroyed& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        if (ev.destroyer == kInvalidPlayer)
            return; // decay / environment

        AddProgress(ev.destroyer, TFDirectiveKind::DestroyVehicles, static_cast<uint8_t>(ev.kind), 1);
    }

    void TFDirectiveSystem::OnXPAwarded(const EvXPAwarded& ev)
    {
        // RE-ENTRANCY GUARD — keep this filter FIRST: GrantTier below calls
        // ServerAwardXP(kXPReasonDirective), which synchronously fires EvXPAwarded
        // back into this handler while AddProgress holds a Rec reference. Only
        // capture reasons may fall through to AddProgress.
        if (!IsCaptureReason(ev.reason))
            return;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        // TFRegionSystem::AwardCaptureXP already resolved per-player capture
        // participation (alive, right faction, inside the radius) — one award
        // per player per flip, so each award is exactly one capture credit.
        AddProgress(ev.player, TFDirectiveKind::CaptureRegions, 0, 1);
    }

    void TFDirectiveSystem::OnDeployablePlaced(const EvDeployablePlaced& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        // Mark the entity known FIRST so the fallback diff-poll below can never
        // double-credit this placement, then credit immediately -- no 0.5 s poll
        // lag, and no missed credit when a deployable is placed and destroyed
        // inside a single poll window. AddProgress may re-enter the bus via
        // GrantTier -> ServerAwardXP -> EvXPAwarded; OnXPAwarded's reason filter
        // keeps that safe (same contract as the other handlers).
        m_knownDeployables.insert(ev.entity);
        AddProgress(ev.owner, TFDirectiveKind::DeployItems, static_cast<uint8_t>(ev.kind), 1);
    }

    void TFDirectiveSystem::PollDeployables(float dt)
    {
        if (!m_ctx->deployables)
            return;

        m_deployPollAccum += dt;
        if (m_deployPollAccum < kDeployPollSec)
            return;
        m_deployPollAccum = 0.0f;

        std::unordered_set<EntityId> current;
        current.reserve(m_knownDeployables.size() + 4);

        m_ctx->deployables->ForEachDeployable(
            [this, &current](const TFDeployableView& v)
            {
                current.insert(v.entity);
                if (m_deploySeeded && v.owner != kInvalidPlayer &&
                    m_knownDeployables.find(v.entity) == m_knownDeployables.end())
                {
                    AddProgress(v.owner, TFDirectiveKind::DeployItems, static_cast<uint8_t>(v.kind), 1);
                }
            });

        m_knownDeployables = std::move(current);
        m_deploySeeded = true; // first poll = baseline only (mid-session boot)
    }

    // ---------------------------------------------------------------------------
    // Progress core + payout
    // ---------------------------------------------------------------------------

    bool TFDirectiveSystem::Matches(const TFDirectiveDef& def, uint8_t value)
    {
        switch (def.kind)
        {
        case TFDirectiveKind::KillInfantry:
            return def.param == 0 || value == 1; // param 1 = headshots only
        case TFDirectiveKind::CaptureRegions:
            return true;
        case TFDirectiveKind::DestroyVehicles:
            return def.param == 0 || def.param == value; // 0 == VehicleId::None == any
        case TFDirectiveKind::DeployItems:
            return def.param == kTFDirectiveAnyParam || def.param == value;
        default:
            return false;
        }
    }

    int TFDirectiveSystem::IndexOfId(uint16_t directiveId)
    {
        for (size_t i = 0; i < kTFDirectiveCount; ++i)
            if (kTFDirectives[i].id == directiveId)
                return static_cast<int>(i);
        return -1;
    }

    void TFDirectiveSystem::AddProgress(PlayerId player, TFDirectiveKind kind, uint8_t value, uint32_t amount)
    {
        if (player == kInvalidPlayer || amount == 0)
            return;

        Rec& rec = m_players[player];
        for (size_t i = 0; i < kTFDirectiveCount; ++i)
        {
            const TFDirectiveDef& def = kTFDirectives[i];
            if (def.kind != kind || !Matches(def, value))
                continue;

            rec.progress[i] += amount;
            ++m_progressTicks;

            // Pay every newly crossed tier (a burst can cross more than one).
            // GrantTier re-enters the event bus (EvXPAwarded), but OnXPAwarded
            // filters kXPReasonDirective before touching m_players, so `rec`
            // stays valid across the call.
            while (rec.tiersDone[i] < kTFDirectiveTiers && rec.progress[i] >= def.tiers[rec.tiersDone[i]].goal)
            {
                GrantTier(player, def, rec.tiersDone[i]);
                ++rec.tiersDone[i];
            }
        }
    }

    void TFDirectiveSystem::GrantTier(PlayerId player, const TFDirectiveDef& def, uint8_t tierIdx)
    {
        const TFDirectiveTier& tier = def.tiers[tierIdx];

        if (m_ctx->progression)
        {
            // ServerAwardXP also sends the existing TF_XPEvent toast to the
            // owning client; ServerGrantFlux wallet-caps and syncs the wallet.
            m_ctx->progression->ServerAwardXP(player, tier.xp, kXPReasonDirective);
            if (tier.flux > 0)
                m_ctx->progression->ServerGrantFlux(player, tier.flux);
        }
        ++m_tiersGranted;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u completed directive '%s' tier %u (+%u XP, +%u flux)",
                       player, def.name, static_cast<unsigned>(tierIdx) + 1u, tier.xp, tier.flux);
        Spark::SimpleConsole::GetInstance().LogInfo(
            std::string("[TF] DIRECTIVE: '") + def.name + "' tier " + std::to_string(tierIdx + 1) + " complete (+" +
            std::to_string(tier.xp) + " XP, +" + std::to_string(tier.flux) + " flux)");
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFDirectiveSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Directives"))
            return;

        ImGui::Text("players tracked: %zu  progress ticks: %u  tiers paid: %u", m_players.size(), m_progressTicks,
                    m_tiersGranted);

        if (!m_ctx || !m_ctx->IsAuthority())
        {
            ImGui::TextDisabled("(client: directive truth lives on the server)");
            return;
        }

        const PlayerId local = m_ctx->localPlayer;
        if (local == kInvalidPlayer)
            return;

        if (ImGui::BeginTable("tf_directives", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Directive");
            ImGui::TableSetupColumn("Progress");
            ImGui::TableSetupColumn("Next goal");
            ImGui::TableSetupColumn("Tiers");
            ImGui::TableHeadersRow();

            ForEachStatus(local,
                          [](const Status& s)
                          {
                              ImGui::TableNextRow();
                              ImGui::TableSetColumnIndex(0);
                              ImGui::TextUnformatted(s.def->name);
                              if (ImGui::IsItemHovered())
                                  ImGui::SetTooltip("%s", s.def->desc);
                              ImGui::TableSetColumnIndex(1);
                              ImGui::Text("%u", s.progress);
                              ImGui::TableSetColumnIndex(2);
                              if (s.tiersDone < kTFDirectiveTiers)
                                  ImGui::Text("%u", s.def->tiers[s.tiersDone].goal);
                              else
                                  ImGui::TextUnformatted("done");
                              ImGui::TableSetColumnIndex(3);
                              ImGui::Text("%u/%u", static_cast<unsigned>(s.tiersDone),
                                          static_cast<unsigned>(kTFDirectiveTiers));
                          });

            ImGui::EndTable();
        }
#endif
    }

} // namespace Terrafront
