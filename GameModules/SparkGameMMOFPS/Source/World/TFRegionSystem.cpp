/**
 * @file TFRegionSystem.cpp
 * @brief W2 territory war: authoritative 1 Hz capture loop, lattice rules,
 *        Dominion detection/hold/reset, contract accessors and the local
 *        capture-bar HUD feed. Wire + persistence live in TFRegionSystemNet.cpp.
 */
#include "World/TFRegionSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFCaptureFx.h"      // W13 capture-fx lane: tower beam/burst/standing-ring
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVisualUtils.h"    // FactionStructureMaterial for capture-point banners
#include "World/TFRegionDecor.h"   // W9: per-tier building-kit decor (owned member)
#include "World/TFWorldSetup.h"    // TerrainHeightAt for landmark placement
#include "Engine/ECS/Components.h" // Transform, MeshRenderer for capture landmarks
#include "UI/TFHUD.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include "Spark/IEngineContext.h" // tf_capture_debug: GetWorld() null-probe (headless ECS gap)

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <deque>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr float kCaptureTickSec = 1.0f;  // capture loop cadence
        constexpr float kCaptureRadiusM = 10.0f; // DESIGN §4: stand-in radius
        constexpr float kCaptureRadiusSq = kCaptureRadiusM * kCaptureRadiusM;
        constexpr float kDefenderDecayMult = 2.0f; // defenders alone: 2x bleed-back
        constexpr float kEmptyDecayMult = 1.0f;    // nobody present: 1x bleed-back
        constexpr float kSaveIntervalSec = 30.0f;  // periodic dirty-save backstop
        constexpr float kDominionHoldSec = 600.0f; // 10 min lock, then soft reset

        bool IsPlayableFaction(FactionId f)
        {
            return f == FactionId::MRA || f == FactionId::AUC || f == FactionId::HLX;
        }

        /// Alive pawn feet position within 10 m (XZ) of any of the region's points?
        bool InCaptureRadius(const RegionDef& def, const float pos[3])
        {
            for (const auto& pt : def.capturePoints)
            {
                const float dx = pos[0] - pt[0];
                const float dz = pos[2] - pt[1];
                if (dx * dx + dz * dz <= kCaptureRadiusSq)
                    return true;
            }
            return false;
        }

        uint16_t XPForTier(const std::string& tier)
        {
            if (tier == "facility")
                return 500; // DESIGN §4 capture XP by tier
            if (tier == "fort")
                return 250;
            return 100; // outpost
        }

        /// Canonical per-tier capture reason (TFProgressionSystem.h reason table).
        uint8_t XPReasonForTier(const std::string& tier)
        {
            if (tier == "facility")
                return kXPReasonCaptureFacility;
            if (tier == "fort")
                return kXPReasonCaptureFort;
            return kXPReasonCaptureOutpost;
        }

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

    void TFRegionSystem::UpdateCaptureVisuals(float dt)
    {
        // Viewer-only decorative landmarks; needs the ECS world, terrain, and data.
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || !m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const auto& regions = m_ctx->data->GetContinent().regions;
        if (regions.empty())
            return;

        // W13 capture-fx lane: advance/redraw the pooled owner-flip burst
        // particles + rising flares (independent of the spawn-once/retint work
        // below — pure pool bookkeeping, safe to call every frame regardless of
        // whether any region actually changed).
        TFCaptureFx::Get().Update(*m_ctx, dt);

        constexpr float kBannerHeightM = 11.5f; // just under the 12.7m cap tower top
        constexpr float kBannerSpinDegPerSec = 24.0f;

        if (!m_capVisualsSpawned)
        {
            m_capBannerEnt.assign(regions.size(), 0u);
            m_capBannerOwner.assign(regions.size(), -2);
            for (size_t i = 0; i < regions.size(); ++i)
            {
                const RegionDef& r = regions[i];
                const float x = r.centerX, z = r.centerZ;
                const float y = m_ctx->world->TerrainHeightAt(x, z);

                // Skyanchors are the faction warpgates: place the big faction
                // gate structure and no capture tower/banner (they never flip).
                if (r.tier == "skyanchor")
                {
                    const char* gate = nullptr;
                    switch (r.homeFaction)
                    {
                    case FactionId::MRA:
                        gate = "Assets/Models/MMOFPS/buildings/warpgate_mra.obj";
                        break;
                    case FactionId::AUC:
                        gate = "Assets/Models/MMOFPS/buildings/warpgate_auc.obj";
                        break;
                    case FactionId::HLX:
                        gate = "Assets/Models/MMOFPS/buildings/warpgate_hlx.obj";
                        break;
                    default:
                        break;
                    }
                    if (gate)
                    {
                        const auto wg = world->CreateEntity("TF_Warpgate");
                        Transform& wt = world->AddComponent<Transform>(wg);
                        wt.position = {x, y, z};
                        MeshRenderer& wmr = world->AddComponent<MeshRenderer>(wg);
                        wmr.meshPath = gate;
                        wmr.materialPath = FactionStructureMaterial(*m_ctx, r.homeFaction);
                        wmr.castShadows = true;
                        wmr.emissive = 0.45f; // warpgate reads as a lit faction beacon
                    }
                    continue;
                }

                const auto tower = world->CreateEntity("TF_CapTower");
                Transform& tt = world->AddComponent<Transform>(tower);
                tt.position = {x, y, z};
                MeshRenderer& tmr = world->AddComponent<MeshRenderer>(tower);
                tmr.meshPath = "Assets/Models/MMOFPS/buildings/cap_tower.obj";
                tmr.materialPath = FactionStructureMaterial(*m_ctx, FactionId::None);
                tmr.castShadows = true;

                const auto banner = world->CreateEntity("TF_CapBanner");
                Transform& bt = world->AddComponent<Transform>(banner);
                bt.position = {x, y + kBannerHeightM, z};
                MeshRenderer& bmr = world->AddComponent<MeshRenderer>(banner);
                bmr.meshPath = "Assets/Models/MMOFPS/buildings/cap_banner_ring.obj";
                bmr.materialPath = FactionStructureMaterial(*m_ctx, FactionId::None);
                bmr.castShadows = false;
                bmr.emissive = 0.55f; // owner-colored banner glows so it reads at range
                m_capBannerEnt[i] = static_cast<uint32_t>(banner);
            }
            m_capVisualsSpawned = true;
        }

        // Per-frame: spin each banner, retint it when its region owner changes,
        // and drive the W13 capture-fx lane's tower progress beam + one-shot
        // owner-flip burst off the same banner entity (capture-fx section of
        // this file; TFCaptureFx.h/.cpp own the actual FX).
        auto& registry = world->GetRegistry();
        for (size_t i = 0; i < m_capBannerEnt.size() && i < m_state.size(); ++i)
        {
            if (m_capBannerEnt[i] == 0u)
                continue;
            const auto e = static_cast<EntityID>(m_capBannerEnt[i]);
            if (!registry.valid(e))
                continue;
            Transform* t = registry.try_get<Transform>(e);
            if (t)
                t->rotation.y += kBannerSpinDegPerSec * dt;
            const int owner = static_cast<int>(m_state[i].owner);
            if (owner != m_capBannerOwner[i])
            {
                const bool firstPaint = (m_capBannerOwner[i] == -2); // spawn-time sentinel, not a real flip
                m_capBannerOwner[i] = owner;
                if (MeshRenderer* mr = registry.try_get<MeshRenderer>(e))
                    mr->materialPath = FactionStructureMaterial(*m_ctx, m_state[i].owner);
                if (!firstPaint && t)
                {
                    const float towerBase[3] = {t->position.x, t->position.y - kBannerHeightM, t->position.z};
                    TFCaptureFx::Get().SpawnOwnerFlipBurst(*m_ctx, towerBase, m_state[i].owner);
                }
            }
            if (t)
            {
                const float towerBase[3] = {t->position.x, t->position.y - kBannerHeightM, t->position.z};
                TFCaptureFx::Get().SubmitTowerBeam(*m_ctx, towerBase, m_state[i].progress, m_state[i].capturing,
                                                   m_state[i].contested);
            }
        }

        // Standing-in-point ground ring under the local player: does its own
        // pawn-position + capturable-point lookup off the public accessors
        // (capture-fx section), so it needs nothing else from this loop.
        TFCaptureFx::Get().UpdateStandingRing(*m_ctx);
    }

    void TFRegionSystem::FixedUpdate(float fixedDeltaTime)
    {
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

    void TFRegionSystem::Shutdown()
    {
        if (!m_initialized)
            return;
        if (m_decor)
        {
            m_decor->Shutdown();
            m_decor.reset();
        }
        if (m_ctx && m_ctx->IsAuthority() && m_dirty)
            PersistNow();
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
        m_domActive = false;
        m_initialized = false;
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

    // ---------------------------------------------------------------------------
    // Authoritative capture loop
    // ---------------------------------------------------------------------------

    void TFRegionSystem::TickCapture(float dt)
    {
        if (m_domActive)
            return; // continent locked — captures frozen for the hold
        if (!m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        std::vector<PawnInfo> pawns;
        pawns.reserve(16);
        m_ctx->players->ForEachAlivePawn([&pawns](const PawnInfo& p) { pawns.push_back(p); });

        const auto& regions = m_ctx->data->GetContinent().regions;
        const size_t n = std::min(regions.size(), m_state.size());

        for (size_t i = 0; i < n; ++i)
        {
            const RegionDef& def = regions[i];
            if (def.tier == "skyanchor" || def.captureSec <= 0.0f || def.capturePoints.empty())
                continue;

            RegionState& st = m_state[i];

            uint32_t presentMask = 0;
            for (const PawnInfo& p : pawns)
            {
                if (!IsPlayableFaction(p.faction))
                    continue;
                if (InCaptureRadius(def, p.pos))
                    presentMask |= 1u << static_cast<uint32_t>(p.faction);
            }
            const int distinct = std::popcount(presentMask);

            const float oldProgress = st.progress;
            const FactionId oldCapturing = st.capturing;
            const bool oldContested = st.contested;

            st.contested = distinct >= 2; // several factions on the point => frozen

            if (distinct == 1)
            {
                const auto f = static_cast<FactionId>(std::countr_zero(presentMask));
                if (f == st.owner)
                {
                    // Defenders alone: bleed back at 2x.
                    st.progress = std::max(0.0f, st.progress - kDefenderDecayMult * dt / def.captureSec);
                    if (st.progress <= 0.0f)
                        st.capturing = FactionId::None;
                }
                else if (IsCapturable(def.id, f))
                {
                    if (st.capturing != f)
                    {
                        st.capturing = f; // new besieger starts from zero
                        st.progress = 0.0f;
                    }
                    st.progress += dt / def.captureSec;
                    if (st.progress >= 1.0f)
                    {
                        FlipOwner(i, f, /*awardXp*/ true); // broadcasts full state
                        continue;
                    }
                }
                // present but not lattice-linked: frozen, nothing advances
            }
            else if (distinct == 0 && st.progress > 0.0f)
            {
                st.progress = std::max(0.0f, st.progress - kEmptyDecayMult * dt / def.captureSec);
                if (st.progress <= 0.0f)
                    st.capturing = FactionId::None;
            }

            if (st.contested != oldContested && m_events)
                m_events->Fire(EvRegionContested{def.id, st.contested});

            // Broadcast on change, and RE-broadcast while the point is in any
            // active state (contested stand-off, frozen unlinked attacker, live
            // progress): TF_CaptureTick is unreliable, so a dropped edge packet
            // would otherwise leave every client's mirror stale until the next
            // change (2026-07-10 play-test: "progress never advances" on the HUD).
            const bool changed =
                st.progress != oldProgress || st.capturing != oldCapturing || st.contested != oldContested;
            const bool active = st.contested || st.capturing != FactionId::None || st.progress > 0.0f;
            if (changed || active)
            {
#ifdef ENABLE_NETWORKING
                SendCaptureTick(i);
#endif
            }
        }
    }

    void TFRegionSystem::FlipOwner(size_t idx, FactionId newOwner, bool awardXp)
    {
        const RegionDef* def = m_ctx->data ? m_ctx->data->GetRegion(static_cast<RegionId>(idx)) : nullptr;
        RegionState& st = m_state[idx];
        const FactionId oldOwner = st.owner;

        st.owner = newOwner;
        st.capturing = FactionId::None;
        st.progress = 0.0f;
        st.contested = false;
        ++m_flips;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] region %u '%s' captured: %s -> %s", static_cast<unsigned>(idx),
                       def ? def->name.c_str() : "?", FactionTag(oldOwner), FactionTag(newOwner));
        // Console echo: flips are the war's headline events and the automated
        // smoke reads them from exec_results.log (SPARK_LOG goes to the file log).
        Spark::SimpleConsole::GetInstance().LogInfo(std::string("[TF] REGION FLIP: ") +
                                                    (def ? def->name.c_str() : "?") + " " + FactionTag(oldOwner) +
                                                    " -> " + FactionTag(newOwner));

        if (m_events)
            m_events->Fire(EvRegionCaptured{static_cast<RegionId>(idx), newOwner, oldOwner});

        if (awardXp && def && IsPlayableFaction(newOwner))
            AwardCaptureXP(*def, newOwner);

#ifdef ENABLE_NETWORKING
        SendRegionState(kInvalidPlayer, idx, /*reliable*/ true);
#endif

        m_dirty = true;
        PersistNow();
        CheckDominion();
    }

    void TFRegionSystem::AwardCaptureXP(const RegionDef& def, FactionId newOwner)
    {
        if (!m_ctx->progression || !m_ctx->players)
            return;
        const uint16_t amount = XPForTier(def.tier);
        const uint8_t reason = XPReasonForTier(def.tier);
        m_ctx->players->ForEachAlivePawn(
            [this, &def, newOwner, amount, reason](const PawnInfo& p)
            {
                if (p.faction != newOwner || p.owner == kInvalidPlayer)
                    return;
                if (InCaptureRadius(def, p.pos))
                    m_ctx->progression->ServerAwardXP(p.owner, amount, reason);
            });
    }

    // ---------------------------------------------------------------------------
    // Dominion
    // ---------------------------------------------------------------------------

    void TFRegionSystem::CheckDominion()
    {
        if (m_domActive || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const auto& regions = m_ctx->data->GetContinent().regions;
        FactionId holder = FactionId::None;
        for (size_t i = 0; i < regions.size() && i < m_state.size(); ++i)
        {
            if (regions[i].tier == "skyanchor")
                continue;
            const FactionId owner = m_state[i].owner;
            if (!IsPlayableFaction(owner))
                return;
            if (holder == FactionId::None)
                holder = owner;
            else if (holder != owner)
                return;
        }
        if (holder != FactionId::None)
            StartDominion(holder);
    }

    void TFRegionSystem::StartDominion(FactionId faction)
    {
        m_domActive = true;
        m_domFaction = faction;
        m_domEndsAt = m_time + kDominionHoldSec;

        // W2 ceremony = the log line + the lock itself; TF-W4: fireworks.
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] ===== DOMINION: %s locks the Cindral Wastes for %.0f min =====", FactionName(faction),
                       kDominionHoldSec / 60.0f);

        if (m_events)
            m_events->Fire(EvDominion{faction});

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            SendFullStateTo(kInvalidPlayer);
#endif

        m_dirty = true;
        PersistNow();
    }

    void TFRegionSystem::SoftResetToInitial(const char* reason)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] territory soft reset (%s) — reseeding from initialOwnership",
                       reason);
        m_domActive = false;
        m_domFaction = FactionId::None;
        RebuildFromData(/*preserveOwners*/ false);

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            SendFullStateTo(kInvalidPlayer);
#endif

        m_dirty = true;
        PersistNow();
    }

    // ---------------------------------------------------------------------------
    // Local player capture-bar feed (all roles with a local player)
    // ---------------------------------------------------------------------------

    void TFRegionSystem::FeedLocalCaptureHUD()
    {
        if (!m_ctx->hud || !m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        PawnInfo pawn{};
        const bool alive = m_ctx->localPlayer != kInvalidPlayer &&
                           m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) && pawn.alive;
        // Same eligibility predicate as the server tick (TickCapture): only pawns
        // with a playable faction are counted on a point, so only they may see the
        // capture indicator — a faction-less player showed a forever-frozen bar in
        // the 2026-07-10 play-test. pawn.faction is the replicated server truth;
        // localFaction covers the frames before the local pawn record mirrors in.
        const FactionId f = alive && IsPlayableFaction(pawn.faction) ? pawn.faction : m_ctx->localFaction;
        if (alive && IsPlayableFaction(f))
        {
            const auto& regions = m_ctx->data->GetContinent().regions;
            const size_t n = std::min(regions.size(), m_state.size());
            for (size_t i = 0; i < n; ++i)
            {
                const RegionDef& def = regions[i];
                if (def.tier == "skyanchor" || def.capturePoints.empty())
                    continue;
                if (!InCaptureRadius(def, pawn.pos))
                    continue;
                const RegionState& st = m_state[i];
                // Show the bar only when the point can react to this player:
                // a capture is live (progress/capturing/contested), or the
                // player's faction could start one here (lattice-linked, not
                // already theirs). Standing on an owned or unlinked point shows
                // nothing — matching exactly what the server tick would do.
                const bool live = st.progress > 0.0f || st.capturing != FactionId::None || st.contested;
                if (!live && !IsCapturable(def.id, f))
                    continue;
                m_ctx->hud->SetCaptureProgress(st.progress, st.capturing, true);
                return;
            }
        }
        m_ctx->hud->SetCaptureProgress(0.0f, FactionId::None, false);
    }

    // ---------------------------------------------------------------------------
    // tf_capture_debug (console) — makes a frozen capture bar diagnosable
    // ---------------------------------------------------------------------------

    std::string TFRegionSystem::DebugCaptureReport() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded() || m_state.empty())
            return "[TF] capture debug: no region data";

        const auto& regions = m_ctx->data->GetContinent().regions;
        const size_t n = std::min(regions.size(), m_state.size());

        // Occupancy sweep — the same pawn set + radius test the 1 Hz tick uses.
        struct Occ
        {
            uint32_t byFaction[static_cast<size_t>(FactionId::COUNT)]{};
            uint32_t noFaction{0};
        };
        std::vector<Occ> occ(n);
        if (m_ctx->players)
        {
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        const RegionDef& def = regions[i];
                        if (def.tier == "skyanchor" || def.capturePoints.empty())
                            continue;
                        if (!InCaptureRadius(def, p.pos))
                            continue;
                        if (IsPlayableFaction(p.faction))
                            ++occ[i].byFaction[static_cast<size_t>(p.faction)];
                        else
                            ++occ[i].noFaction; // the capture tick skips these pawns
                    }
                });
        }

        std::ostringstream os;
        os << "[TF] capture debug (" << (m_ctx->IsAuthority() ? "authority truth" : "client mirror") << "):";

        // 2026-07-10 root-cause tripwire: on an authority with no engine ECS
        // World every pawn position reads (0,0,0), so occupancy is impossible
        // and capture silently never ticks (the headless boot once registered
        // no World service). Say so instead of printing all-zero occupancy.
        if (m_ctx->IsAuthority() && (!m_ctx->engine || m_ctx->engine->GetWorld() == nullptr))
            os << "\n  !! NO ENGINE ECS WORLD: pawn positions are frozen at the origin — occupancy/capture "
                  "CANNOT work (headless boot missing SetWorld?)";
        char buf[96];
        for (size_t i = 0; i < n; ++i)
        {
            const RegionDef& def = regions[i];
            if (def.tier == "skyanchor" || def.capturePoints.empty())
                continue;
            const RegionState& st = m_state[i];
            os << "\n  [" << i << "] " << def.name << " owner=" << FactionTag(st.owner)
               << "  occ MRA:" << occ[i].byFaction[static_cast<size_t>(FactionId::MRA)]
               << " AUC:" << occ[i].byFaction[static_cast<size_t>(FactionId::AUC)]
               << " HLX:" << occ[i].byFaction[static_cast<size_t>(FactionId::HLX)];
            if (occ[i].noFaction > 0)
                os << " none:" << occ[i].noFaction << "(IGNORED)";
            std::snprintf(buf, sizeof(buf), "  capturing=%s %.0f%%%s", FactionTag(st.capturing), st.progress * 100.0f,
                          st.contested ? " CONTESTED" : "");
            os << buf;
            if (!m_ctx->IsAuthority())
            {
                if (st.lastNetAt >= 0.0)
                {
                    std::snprintf(buf, sizeof(buf), "  repAge=%.1fs", std::max(0.0, m_time - st.lastNetAt));
                    os << buf;
                }
                else
                {
                    os << "  repAge=never";
                }
            }
        }

        // Local-player eligibility line: was the tester's pawn even counted?
        if (m_ctx->HasLocalPlayer() && m_ctx->players && m_ctx->localPlayer != kInvalidPlayer)
        {
            PawnInfo pawn{};
            if (m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
            {
                char line[160];
                std::snprintf(line, sizeof(line), "\n  local pawn: faction=%s%s %s pos=(%.0f %.0f %.0f)",
                              FactionTag(pawn.faction), IsPlayableFaction(pawn.faction) ? "" : " (NOT counted)",
                              pawn.alive ? "alive" : "dead", pawn.pos[0], pawn.pos[1], pawn.pos[2]);
                os << line;
            }
            else
            {
                os << "\n  local pawn: none (deploy first)";
            }
        }
        return os.str();
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFRegionSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Regions", &m_showDebug))
        {
            ImGui::Text("held: MRA %u  AUC %u  HLX %u  neutral %u   hash 0x%08X", RegionsHeld(FactionId::MRA),
                        RegionsHeld(FactionId::AUC), RegionsHeld(FactionId::HLX), RegionsHeld(FactionId::None),
                        TerritoryHash());
            ImGui::Text("flips %u   ticksTx %u   stateRx %u   tickRx %u   bad %u", m_flips, m_ticksSent, m_stateRx,
                        m_tickRx, m_badPackets);
            if (m_decor)
                ImGui::Text("decor: visible %u   culled %u", m_decor->VisibleDecorCount(), m_decor->CulledDecorCount());
            if (m_domActive)
            {
                float col[4];
                FactionColor(m_domFaction, col);
                ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]), "DOMINION: %s — reset in %.0fs",
                                   FactionName(m_domFaction), std::max(0.0, m_domEndsAt - m_time));
            }
            ImGui::Separator();

            const bool haveData = m_ctx && m_ctx->data && m_ctx->data->IsLoaded();
            for (size_t i = 0; i < m_state.size(); ++i)
            {
                const RegionState& st = m_state[i];
                const RegionDef* def = haveData ? m_ctx->data->GetRegion(static_cast<RegionId>(i)) : nullptr;
                float col[4];
                FactionColor(st.owner, col);
                ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]), "%-3s", FactionTag(st.owner));
                ImGui::SameLine();
                ImGui::Text("%2zu %-20s %-9s", i, def ? def->name.c_str() : "?", def ? def->tier.c_str() : "?");
                if (st.capturing != FactionId::None || st.progress > 0.0f)
                {
                    ImGui::SameLine();
                    char overlay[48];
                    std::snprintf(overlay, sizeof(overlay), "%s %.0f%%%s", FactionTag(st.capturing),
                                  st.progress * 100.0f, st.contested ? " CONTESTED" : "");
                    ImGui::ProgressBar(st.progress, ImVec2(140.0f, 0.0f), overlay);
                }
                else if (st.contested)
                {
                    ImGui::SameLine();
                    ImGui::TextUnformatted("CONTESTED");
                }
            }
        }
        ImGui::End();
#endif
    }

} // namespace Terrafront
