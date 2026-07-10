/**
 * @file TFRedeployRules.h
 * @brief Shared client/server eligibility rules for map redeploys (W7).
 *
 * OWNERSHIP: ui-map-keys lane authored this seam; the CONTINENTS lane is the
 * sanctioned extender (SetExtraRule below — e.g. the sanctuary rule) and may
 * evolve the hook signature in coordination via wiringNotes.
 *
 * ONE predicate, run on BOTH sides:
 *   - client (TFMapScreen): pre-check to enable/disable the REDEPLOY button
 *     and pick the denial text — pure UX, never trusted.
 *   - server (TFServerSim::HandleRedeployRequest, lane wiringNotes): the
 *     authoritative check before the pawn is moved. The server additionally
 *     enforces the per-player redeploy interval (kTFRedeployIntervalSec),
 *     which is server state and deliberately NOT part of this predicate.
 *
 * Baseline rule (PS2 spirit): alive, on foot (not seated in a vehicle), and
 * the target region is friendly AND spawnable (TFRegionSystem::CanSpawnAt ==
 * owned by your faction + conduit-linked to your skyanchor) AND not contested.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>
#include <functional>

namespace Terrafront::TFRedeployRules
{

    /// Client-side countdown between clicking REDEPLOY and the request going out.
    inline constexpr float kTFRedeployCountdownSec = 5.0f;

    /// Server-enforced minimum interval between accepted redeploys per player.
    inline constexpr float kTFRedeployIntervalSec = 15.0f;

    /// Extra veto hook (continents lane: sanctuary rule). Return kTFRedeployOk
    /// (0) to allow, or a kTFRedeploy* reason (TFRedeployProtocol.h) to refuse —
    /// use kTFRedeployBlocked for lane-specific rules. Runs on BOTH sides after
    /// the baseline checks pass; install once from your system's Initialize.
    using ExtraRule = std::function<uint8_t(const TFGameContext& ctx, PlayerId player, RegionId region)>;

    /// Install/replace the extra rule (pass {} to clear). Not thread-safe by
    /// design — call from Initialize, before the first Update.
    void SetExtraRule(ExtraRule rule);

    /// Baseline + extra-rule redeploy eligibility. Returns kTFRedeployOk (0) or
    /// the first failing kTFRedeploy* reason (TFRedeployProtocol.h). Never
    /// checks the server cooldown (server-only state, see file header).
    uint8_t CanRedeploy(const TFGameContext& ctx, PlayerId player, FactionId faction, RegionId region);

    /// Resolve the region's spawn point for an ACCEPTED redeploy (mirrors
    /// TFPlayerSystem::FindRegionSpawn: first spawns[] entry or the region
    /// center, terrain-clamped, yaw facing map center). False when the region
    /// is unknown or not spawnable for `faction`.
    bool ResolveSpawnPos(const TFGameContext& ctx, RegionId region, FactionId faction, float outPos[3], float& outYaw);

    /// Short human string for a kTFRedeploy* reason (UI denial text).
    const char* ReasonText(uint8_t reason);

} // namespace Terrafront::TFRedeployRules
