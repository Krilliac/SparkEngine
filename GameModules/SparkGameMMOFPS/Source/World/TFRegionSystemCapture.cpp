/**
 * @file TFRegionSystemCapture.cpp
 * @brief TFRegionSystem authoritative side: the 1 Hz capture tick over the
 *        alive-pawn set, owner flips + capture XP, and Dominion
 *        detection/hold/soft-reset. Lifecycle + contract accessors live in
 *        TFRegionSystem.cpp, wire + persistence in TFRegionSystemNet.cpp.
 */
#include "World/TFRegionSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "World/TFRegionSystemInternal.h" // RegionDetail: IsPlayableFaction, InCaptureRadius
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <bit>

namespace Terrafront
{

    using namespace RegionDetail;

    namespace
    {

        constexpr float kDefenderDecayMult = 2.0f; // defenders alone: 2x bleed-back
        constexpr float kEmptyDecayMult = 1.0f;    // nobody present: 1x bleed-back
        constexpr float kDominionHoldSec = 600.0f; // 10 min lock, then soft reset

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

} // namespace Terrafront
