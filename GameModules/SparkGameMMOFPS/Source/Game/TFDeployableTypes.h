/**
 * @file TFDeployableTypes.h
 * @brief Deployable kind catalog + per-kind placement/stat specs (W6 expansion).
 *
 * OWNERSHIP: deployables lane (same agent as TFDeployableSystem.*).
 *
 * TFTypes.h (FROZEN) defines DeployableKind 0-2. The three W6 kinds below are
 * lane-local constants past DeployableKind::COUNT so this lane compiles with
 * NO edit to the frozen contract header; the wire already carries `kind` as a
 * uint8_t (TF_RepDeployCreate), so extended kinds replicate unchanged. If the
 * integrator later promotes them into the TFTypes.h enum (see wiring notes),
 * delete the three constants + the static_assert here and switch the names —
 * the numeric values are identical either way.
 *
 * All balance numbers for deployables live in kTFDeployableSpecs (one table,
 * same policy as the W3 kStats it replaces); a data-table pass can move them
 * to JSON later without touching the consumers.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Extended kinds (W6). Values continue the frozen enum: 3, 4, 5.
    // ---------------------------------------------------------------------------

    static_assert(static_cast<uint8_t>(DeployableKind::COUNT) == 3,
                  "TFTypes.h DeployableKind grew — fold the extended kind constants below "
                  "into the enum and delete them here (values must stay 3/4/5).");

    /// Fabricator: large resupply pylon — heals nearby infantry (W3 ammo stand-in,
    /// same TF-W4 caveat as FabAmmoPack) and field-repairs nearby friendly deployables.
    constexpr DeployableKind kDeployResupplyStation = static_cast<DeployableKind>(3);
    /// Fabricator: anti-vehicle turret — slow, heavy shots at enemy vehicles only.
    constexpr DeployableKind kDeployAVTurret = static_cast<DeployableKind>(4);
    /// Fabricator: destructible barrier wall (static physics blocker when Jolt is live).
    constexpr DeployableKind kDeployShieldWall = static_cast<DeployableKind>(5);

    /// Total kinds including the extended block (supersedes DeployableKind::COUNT
    /// for range checks inside the deployable system).
    constexpr uint8_t kTFDeployKindCount = 6;

    /// Per-player cap across ALL kinds (per-kind caps live in the spec table).
    /// At the total cap a placement is refused; at a per-kind cap the oldest of
    /// that kind is replaced instead (the established W3 behavior, generalized).
    constexpr uint8_t kTFDeployMaxActivePerPlayer = 5;

    // ---------------------------------------------------------------------------
    // Per-kind spec (stats + placement validation inputs)
    // ---------------------------------------------------------------------------

    struct TFDeployableSpec
    {
        const char* name;         ///< short display name (debug UI / logs)
        uint8_t requiredClass;    ///< ClassId (as uint8_t) that may place this kind
        float health;             ///< max hit points
        float lifeSec;            ///< lifetime seconds
        uint8_t maxActive;        ///< per-player cap for this kind (oldest replaced at cap)
        float minSpacingM;        ///< min distance to ANY other deployable, meters
        float maxSlopeTan;        ///< max terrain grade (rise/run) allowed at the drop point
        float overlapRadiusM;     ///< physics overlap probe radius, meters
        float visualScaleMult[3]; ///< extra scale applied on top of the data-table visual
    };

    /// Indexed by DeployableKind value. Kinds 0-2 keep their exact W3 numbers
    /// (health/life from the old kStats; spacing/slope are new validation, W6).
    /// requiredClass: Medtech = 2, Fabricator = 3 (ClassId order in TFTypes.h).
    constexpr TFDeployableSpec kTFDeployableSpecs[kTFDeployKindCount] = {
        {"Fab Turret", 3, 300.0f, 60.0f, 1, 6.0f, 0.70f, 0.60f, {1.0f, 1.0f, 1.0f}},         // 0 FabTurret
        {"Ammo Pack", 3, 150.0f, 60.0f, 1, 3.0f, 0.70f, 0.50f, {1.0f, 1.0f, 1.0f}},          // 1 FabAmmoPack
        {"Med Beacon", 2, 150.0f, 45.0f, 1, 3.0f, 0.70f, 0.50f, {1.0f, 1.0f, 1.0f}},         // 2 MedBeacon
        {"Resupply Station", 3, 400.0f, 120.0f, 1, 10.0f, 0.55f, 1.20f, {1.8f, 1.4f, 1.8f}}, // 3 ResupplyStation
        {"AV Turret", 3, 350.0f, 90.0f, 1, 8.0f, 0.60f, 0.80f, {1.3f, 1.6f, 1.3f}},          // 4 AVTurret
        {"Shield Wall", 3, 800.0f, 90.0f, 2, 4.0f, 0.50f, 1.00f, {3.0f, 2.0f, 0.45f}},       // 5 ShieldWall
    };

    /// Spec for `kind`, or nullptr when the kind byte is out of range (bad wire
    /// data / stale client). Never returns null for a kind this lane defines.
    inline const TFDeployableSpec* TFDeploySpecOf(DeployableKind kind)
    {
        const uint8_t k = static_cast<uint8_t>(kind);
        return (k < kTFDeployKindCount) ? &kTFDeployableSpecs[k] : nullptr;
    }

    /// Data-table visual row to use for `kind`. The W6 kinds reuse a W1-staged
    /// base row (deployables.json only has rows 0-2) scaled by visualScaleMult;
    /// bespoke rows can be added to deployables.json later without code changes
    /// here becoming wrong — a real row for an extended kind simply never existed,
    /// so the alias keeps resolving until the data lane adds one and this alias
    /// is retired (see wiring notes).
    inline DeployableKind TFDeployVisualAlias(DeployableKind kind)
    {
        if (kind == kDeployResupplyStation)
            return DeployableKind::FabAmmoPack; // big crate
        if (kind == kDeployAVTurret)
            return DeployableKind::FabTurret; // taller mast
        if (kind == kDeployShieldWall)
            return DeployableKind::FabAmmoPack; // stretched crate slab
        return kind;
    }

} // namespace Terrafront
