/**
 * @file TFRegionSystem.cpp
 * @brief W2 territory war: lifecycle, region topology/seeding, lattice rules
 *        and the frozen contract accessors. The authoritative capture loop +
 *        Dominion live in TFRegionSystemCapture.cpp, the HUD/visual/debug
 *        surfaces in TFRegionSystemUi.cpp, wire + persistence in
 *        TFRegionSystemNet.cpp.
 */
#include "World/TFRegionSystem.h"

#include "Data/TFDataTables.h"
#include "World/TFRegionDecor.h"          // W9: per-tier building-kit decor (owned member)
#include "World/TFRegionSystemInternal.h" // RegionDetail: IsPlayableFaction
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include "Utils/TFPerfCounters.h" // TF-W13 server-perf lane: Capture phase timing

#include <algorithm>
#include <deque>

namespace Terrafront
{

    using namespace RegionDetail;

    namespace
    {

        constexpr float kCaptureTickSec = 1.0f;   // capture loop cadence
        constexpr float kSaveIntervalSec = 30.0f; // periodic dirty-save backstop

    } // namespace

    TFRegionSystem::TFRegionSystem() = default;
    TFRegionSystem::~TFRegionSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFRegionSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvDataReloaded>([this](const EvDataReloaded&) { OnDataReloaded(); });

        m_initialized = true;
        EnsureState(); // data tables boot before us (Main.cpp order), so this
                       // seeds ownership + attempts the persisted-territory load

        // Diagnosability (2026-07-10 play-test "indicator shows, progress frozen"):
        // per-region occupancy/progress/replication-age dump, valid on any role.
        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_capture_debug"))
        {
            console.RegisterCommand(
                "tf_capture_debug", [this](const std::vector<std::string>&) -> std::string
                { return m_initialized ? DebugCaptureReport() : std::string("[TF] region system not ready"); },
                "Capture diagnosis: per-region occupants by faction, capturing/progress/"
                "contested, replicated-progress age",
                "TERRAFRONT", "tf_capture_debug");
            m_debugCmd = true;
        }

        // W9 world-decor lane / W10 decor-collision: role-agnostic decor layout
        // + static OBBs on BOTH roles (the visual stamp stays viewer-only
        // inside TFRegionDecor); driven from Update() below on every role.
        m_decor = std::make_unique<TFRegionDecor>();
        m_decor->Initialize(ctx);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFRegionSystem initialized (%zu regions)", m_state.size());
        return true;
    }

    void TFRegionSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        EnsureState();

#ifdef ENABLE_NETWORKING
        // Client mirror handlers. Registered from Update() ON PURPOSE: Main.cpp
        // updates TFClientNet before us, and TFClientNet registers accepted-but-
        // unrouted no-ops for RegionState/CaptureTick on the same link-up frame —
        // registering afterwards makes our real handlers win the single handler
        // slot per message type.
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
            ReleaseClientHandlers();
#endif

        if (m_ctx->HasLocalPlayer())
        {
            FeedLocalCaptureHUD();
            UpdateCaptureVisuals(deltaTime);
        }

        // W10 decor-collision lane: decor updates on EVERY role now — its
        // layout is role-agnostic and it registers static OBBs for collidable
        // pieces on both server and client; only the visual stamp stays
        // HasLocalPlayer-gated (inside TFRegionDecor). Cheap no-op after all
        // its one-shot passes ran.
        if (m_decor)
            m_decor->Update();
    }

    void TFRegionSystem::FixedUpdate(float fixedDeltaTime)
    {
        Terrafront::TFPerfCounters::ScopedTimer _tfPerf(Terrafront::TFPerfCounters::Phase::Capture);
        if (!m_initialized || !m_ctx)
            return;

        m_time += fixedDeltaTime;
        EnsureState();

        if (!m_ctx->IsAuthority() || m_state.empty())
            return;

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            ServerPollNewClients();
        else if (!m_knownClients.empty())
            m_knownClients.clear();
#endif

        // Dominion hold expiry.
        if (m_domActive && m_time >= m_domEndsAt)
            SoftResetToInitial("Dominion hold expired");

        // 1 Hz capture tick.
        m_captureAccum += fixedDeltaTime;
        while (m_captureAccum >= kCaptureTickSec)
        {
            m_captureAccum -= kCaptureTickSec;
            TickCapture(kCaptureTickSec);
        }

        // Periodic dirty-save backstop (flips already persist immediately).
        m_saveAccum += fixedDeltaTime;
        if (m_saveAccum >= kSaveIntervalSec)
        {
            m_saveAccum = 0.0f;
            if (m_dirty)
                PersistNow();
        }
    }

    bool TFRegionSystem::Checkpoint()
    {
        return !m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_dirty || PersistNow();
    }

    bool TFRegionSystem::Shutdown()
    {
        if (!m_initialized)
            return true;
        if (!Checkpoint())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] region shutdown refused: territory checkpoint failed; state remains initialized");
            return false;
        }
        if (m_decor)
        {
            m_decor->Shutdown();
            m_decor.reset();
        }
        if (m_debugCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_capture_debug");
            m_debugCmd = false;
        }
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        m_knownClients.clear();
#endif
        m_state.clear();
        m_persistLoaded = false;
        m_persistBlocked = false;
        m_domActive = false;
        m_initialized = false;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Topology / seeding
    // ---------------------------------------------------------------------------

    void TFRegionSystem::EnsureState()
    {
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const size_t n = m_ctx->data->GetContinent().regions.size();
        if (m_state.size() != n)
            RebuildFromData(false);
        if (!m_persistLoaded && !m_state.empty())
        {
            m_persistLoaded = true;
            if (m_ctx->IsAuthority())
                LoadPersisted();
        }
    }

    void TFRegionSystem::RebuildFromData(bool preserveOwners)
    {
        const ContinentDef& cont = m_ctx->data->GetContinent();
        const size_t n = cont.regions.size();

        std::vector<RegionState> fresh(n);
        for (size_t i = 0; i < n; ++i)
        {
            const RegionDef& def = cont.regions[i];
            if (def.tier == "skyanchor")
                fresh[i].owner = def.homeFaction; // skyanchors are indestructible homes
            else if (preserveOwners && i < m_state.size())
                fresh[i] = m_state[i];
            else if (i < cont.initialOwner.size())
                fresh[i].owner = cont.initialOwner[i];
        }
        m_state = std::move(fresh);
    }

    void TFRegionSystem::OnDataReloaded()
    {
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const size_t n = m_ctx->data->GetContinent().regions.size();
        const bool preserve = (n == m_state.size());
        RebuildFromData(preserve);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] regions: data reload — %zu regions, ownership %s", n,
                       preserve ? "preserved" : "reseeded from initialOwnership");
    }

    // ---------------------------------------------------------------------------
    // W2 contract accessors (serve server truth or client mirror identically)
    // ---------------------------------------------------------------------------

    FactionId TFRegionSystem::OwnerOf(RegionId region) const
    {
        return region < m_state.size() ? m_state[region].owner : FactionId::None;
    }

    bool TFRegionSystem::IsCapturable(RegionId region, FactionId attacker) const
    {
        if (region >= m_state.size() || !IsPlayableFaction(attacker))
            return false;
        if (m_state[region].owner == attacker)
            return false;
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        const RegionDef* def = m_ctx->data->GetRegion(region);
        if (!def || def->tier == "skyanchor" || def->captureSec <= 0.0f)
            return false;
        // Lattice rule: conduit-linked to territory the attacker owns.
        for (RegionId nb : def->neighbors)
            if (nb < m_state.size() && m_state[nb].owner == attacker)
                return true;
        return false;
    }

    float TFRegionSystem::CaptureProgress(RegionId region, FactionId& outCapturing, bool& outContested) const
    {
        if (region >= m_state.size())
        {
            outCapturing = FactionId::None;
            outContested = false;
            return 0.0f;
        }
        const RegionState& st = m_state[region];
        outCapturing = st.capturing;
        outContested = st.contested;
        return st.progress;
    }

    uint32_t TFRegionSystem::RegionsHeld(FactionId faction) const
    {
        uint32_t n = 0;
        for (const RegionState& st : m_state)
            if (st.owner == faction)
                ++n;
        return n;
    }

    bool TFRegionSystem::CanSpawnAt(RegionId region, FactionId faction) const
    {
        if (region >= m_state.size() || !IsPlayableFaction(faction))
            return false;
        if (m_state[region].owner != faction)
            return false;
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        const ContinentDef& cont = m_ctx->data->GetContinent();

        // BFS from the faction's skyanchor(s) across owned regions only:
        // "owned + conduit-linked" == an unbroken owned chain back home.
        std::vector<char> visited(m_state.size(), 0);
        std::deque<RegionId> open;
        for (size_t i = 0; i < cont.regions.size() && i < m_state.size(); ++i)
        {
            if (cont.regions[i].tier == "skyanchor" && cont.regions[i].homeFaction == faction)
            {
                open.push_back(static_cast<RegionId>(i));
                visited[i] = 1;
            }
        }
        while (!open.empty())
        {
            const RegionId cur = open.front();
            open.pop_front();
            if (cur == region)
                return true;
            const RegionDef* def = m_ctx->data->GetRegion(cur);
            if (!def)
                continue;
            for (RegionId nb : def->neighbors)
            {
                if (nb >= m_state.size() || visited[nb] || m_state[nb].owner != faction)
                    continue;
                visited[nb] = 1;
                open.push_back(nb);
            }
        }
        return false;
    }

    uint32_t TFRegionSystem::TerritoryHash() const
    {
        uint32_t h = 2166136261u; // FNV-1a
        for (const RegionState& st : m_state)
        {
            h ^= static_cast<uint32_t>(st.owner);
            h *= 16777619u;
        }
        return h;
    }

    bool TFRegionSystem::DominionActive(FactionId& outFaction, float& outSecondsLeft) const
    {
        outFaction = m_domActive ? m_domFaction : FactionId::None;
        outSecondsLeft = m_domActive ? static_cast<float>(std::max(0.0, m_domEndsAt - m_time)) : 0.0f;
        return m_domActive;
    }

    bool TFRegionSystem::ServerForceOwner(RegionId region, FactionId newOwner)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || region >= m_state.size())
            return false;
        if (newOwner != FactionId::None && !IsPlayableFaction(newOwner))
            return false;
        const RegionDef* def = (m_ctx->data && m_ctx->data->IsLoaded()) ? m_ctx->data->GetRegion(region) : nullptr;
        if (!def || def->tier == "skyanchor")
            return false; // skyanchors are indestructible
        if (m_state[region].owner == newOwner)
            return true; // already there — success, nothing to announce
        FlipOwner(region, newOwner, /*awardXp*/ false);
        return true;
    }

    bool TFRegionSystem::SaveNow()
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return false;
        return PersistNow();
    }

} // namespace Terrafront
