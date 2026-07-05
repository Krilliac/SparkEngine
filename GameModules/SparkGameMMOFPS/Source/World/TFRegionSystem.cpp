/**
 * @file TFRegionSystem.cpp
 * @brief W2 territory war: authoritative 1 Hz capture loop, lattice rules,
 *        Dominion detection/hold/reset, contract accessors and the local
 *        capture-bar HUD feed. Wire + persistence live in TFRegionSystemNet.cpp.
 */
#include "World/TFRegionSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "UI/TFHUD.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <deque>

namespace Terrafront {

namespace {

constexpr float kCaptureTickSec     = 1.0f;   // capture loop cadence
constexpr float kCaptureRadiusM     = 10.0f;  // DESIGN §4: stand-in radius
constexpr float kCaptureRadiusSq    = kCaptureRadiusM * kCaptureRadiusM;
constexpr float kDefenderDecayMult  = 2.0f;   // defenders alone: 2x bleed-back
constexpr float kEmptyDecayMult     = 1.0f;   // nobody present: 1x bleed-back
constexpr float kSaveIntervalSec    = 30.0f;  // periodic dirty-save backstop
constexpr float kDominionHoldSec    = 600.0f; // 10 min lock, then soft reset

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
    if (tier == "facility") return 500;  // DESIGN §4 capture XP by tier
    if (tier == "fort")     return 250;
    return 100;                          // outpost
}

/// Canonical per-tier capture reason (TFProgressionSystem.h reason table).
uint8_t XPReasonForTier(const std::string& tier)
{
    if (tier == "facility") return kXPReasonCaptureFacility;
    if (tier == "fort")     return kXPReasonCaptureFort;
    return kXPReasonCaptureOutpost;
}

} // namespace

TFRegionSystem::TFRegionSystem() = default;
TFRegionSystem::~TFRegionSystem() { if (m_initialized) Shutdown(); }

bool TFRegionSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    events.Subscribe<EvDataReloaded>([this](const EvDataReloaded&) { OnDataReloaded(); });

    m_initialized = true;
    EnsureState();   // data tables boot before us (Main.cpp order), so this
                     // seeds ownership + attempts the persisted-territory load
    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] TFRegionSystem initialized (%zu regions)", m_state.size());
    return true;
}

void TFRegionSystem::Update(float deltaTime)
{
    (void)deltaTime;
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
        FeedLocalCaptureHUD();
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
    if (m_ctx && m_ctx->IsAuthority() && m_dirty)
        PersistNow();
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
            fresh[i].owner = def.homeFaction;   // skyanchors are indestructible homes
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
    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] regions: data reload — %zu regions, ownership %s",
                   n, preserve ? "preserved" : "reseeded from initialOwnership");
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

float TFRegionSystem::CaptureProgress(RegionId region, FactionId& outCapturing,
                                      bool& outContested) const
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
    const RegionDef* def = (m_ctx->data && m_ctx->data->IsLoaded())
                               ? m_ctx->data->GetRegion(region) : nullptr;
    if (!def || def->tier == "skyanchor")
        return false; // skyanchors are indestructible
    if (m_state[region].owner == newOwner)
        return true;  // already there — success, nothing to announce
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

        const float     oldProgress  = st.progress;
        const FactionId oldCapturing = st.capturing;
        const bool      oldContested = st.contested;

        st.contested = distinct >= 2; // several factions on the point => frozen

        if (distinct == 1)
        {
            const auto f = static_cast<FactionId>(std::countr_zero(presentMask));
            if (f == st.owner)
            {
                // Defenders alone: bleed back at 2x.
                st.progress = std::max(0.0f,
                    st.progress - kDefenderDecayMult * dt / def.captureSec);
                if (st.progress <= 0.0f)
                    st.capturing = FactionId::None;
            }
            else if (IsCapturable(def.id, f))
            {
                if (st.capturing != f)
                {
                    st.capturing = f;   // new besieger starts from zero
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
            st.progress = std::max(0.0f,
                st.progress - kEmptyDecayMult * dt / def.captureSec);
            if (st.progress <= 0.0f)
                st.capturing = FactionId::None;
        }

        if (st.contested != oldContested && m_events)
            m_events->Fire(EvRegionContested{def.id, st.contested});

        if (st.progress != oldProgress || st.capturing != oldCapturing ||
            st.contested != oldContested)
        {
#ifdef ENABLE_NETWORKING
            SendCaptureTick(i);
#endif
        }
    }
}

void TFRegionSystem::FlipOwner(size_t idx, FactionId newOwner, bool awardXp)
{
    const RegionDef* def = m_ctx->data ? m_ctx->data->GetRegion(static_cast<RegionId>(idx))
                                       : nullptr;
    RegionState& st = m_state[idx];
    const FactionId oldOwner = st.owner;

    st.owner = newOwner;
    st.capturing = FactionId::None;
    st.progress = 0.0f;
    st.contested = false;
    ++m_flips;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] region %u '%s' captured: %s -> %s",
                   static_cast<unsigned>(idx), def ? def->name.c_str() : "?",
                   FactionTag(oldOwner), FactionTag(newOwner));
    // Console echo: flips are the war's headline events and the automated
    // smoke reads them from exec_results.log (SPARK_LOG goes to the file log).
    Spark::SimpleConsole::GetInstance().LogInfo(
        std::string("[TF] REGION FLIP: ") + (def ? def->name.c_str() : "?") + " " +
        FactionTag(oldOwner) + " -> " + FactionTag(newOwner));

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
    m_ctx->players->ForEachAlivePawn([this, &def, newOwner, amount, reason](const PawnInfo& p) {
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
                   "[TF] ===== DOMINION: %s locks the Cindral Wastes for %.0f min =====",
                   FactionName(faction), kDominionHoldSec / 60.0f);

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
    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] territory soft reset (%s) — reseeding from initialOwnership", reason);
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
                       m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) &&
                       pawn.alive;
    if (alive)
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
            m_ctx->hud->SetCaptureProgress(st.progress, st.capturing, true);
            return;
        }
    }
    m_ctx->hud->SetCaptureProgress(0.0f, FactionId::None, false);
}

// ---------------------------------------------------------------------------
// Debug UI
// ---------------------------------------------------------------------------

void TFRegionSystem::RenderDebugUI()
{
#ifdef ENABLE_EDITOR
    if (!m_showDebug)
        return;
    if (ImGui::Begin("TF Regions", &m_showDebug))
    {
        ImGui::Text("held: MRA %u  AUC %u  HLX %u  neutral %u   hash 0x%08X",
                    RegionsHeld(FactionId::MRA), RegionsHeld(FactionId::AUC),
                    RegionsHeld(FactionId::HLX), RegionsHeld(FactionId::None),
                    TerritoryHash());
        ImGui::Text("flips %u   ticksTx %u   stateRx %u   tickRx %u   bad %u",
                    m_flips, m_ticksSent, m_stateRx, m_tickRx, m_badPackets);
        if (m_domActive)
        {
            float col[4];
            FactionColor(m_domFaction, col);
            ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]),
                               "DOMINION: %s — reset in %.0fs",
                               FactionName(m_domFaction),
                               std::max(0.0, m_domEndsAt - m_time));
        }
        ImGui::Separator();

        const bool haveData = m_ctx && m_ctx->data && m_ctx->data->IsLoaded();
        for (size_t i = 0; i < m_state.size(); ++i)
        {
            const RegionState& st = m_state[i];
            const RegionDef* def = haveData
                ? m_ctx->data->GetRegion(static_cast<RegionId>(i)) : nullptr;
            float col[4];
            FactionColor(st.owner, col);
            ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]), "%-3s",
                               FactionTag(st.owner));
            ImGui::SameLine();
            ImGui::Text("%2zu %-20s %-9s", i,
                        def ? def->name.c_str() : "?", def ? def->tier.c_str() : "?");
            if (st.capturing != FactionId::None || st.progress > 0.0f)
            {
                ImGui::SameLine();
                char overlay[48];
                std::snprintf(overlay, sizeof(overlay), "%s %.0f%%%s",
                              FactionTag(st.capturing), st.progress * 100.0f,
                              st.contested ? " CONTESTED" : "");
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
