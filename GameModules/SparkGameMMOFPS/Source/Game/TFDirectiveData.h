/**
 * @file TFDirectiveData.h
 * @brief Data-driven directive (mission) definitions — code tables, no new file formats.
 *
 * OWNERSHIP: this header belongs to the TFDirectiveSystem agent (with
 * TFDirectiveSystem.h/.cpp). Balance numbers live here so a rebalance is a
 * one-file diff; the system logic never hardcodes goals or rewards.
 *
 * Directives are long-running per-player objectives (kill infantry, capture
 * regions, destroy vehicles, place deployables) with THREE reward tiers each.
 * Progress is server-tracked and session-scoped (like W2 progression,
 * PlayerIds are session-scoped; durable per-character directive persistence
 * would ride the same TFCharacterSystem seam progression uses — out of scope
 * this wave, documented in TFDirectiveSystem.h).
 */
#pragma once

#include "Core/TFTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Terrafront
{

    /// XP reason code used for directive tier payouts. The reason field is an
    /// opaque uint8_t on the wire and TFProgressionSystem.h documents that other
    /// systems may extend the canonical list (0-10 are taken there — 10 is
    /// kXPReasonUnlock); 11 is claimed here for directives so client XP toasts
    /// can label the payout later.
    inline constexpr uint8_t kXPReasonDirective = 11;

    /// What a directive counts. `TFDirectiveDef::param` refines the filter:
    ///  - KillInfantry:    param 1 = headshot kills only, 0 = any hostile kill
    ///  - CaptureRegions:  param unused (any capture participation counts)
    ///  - DestroyVehicles: param = VehicleId filter (0 == VehicleId::None == any)
    ///  - DeployItems:     param = DeployableKind filter (0xFF = any; 0 is a real kind)
    enum class TFDirectiveKind : uint8_t
    {
        KillInfantry = 0,
        CaptureRegions,
        DestroyVehicles,
        DeployItems,
        COUNT
    };

    /// DeployItems "any kind" sentinel (DeployableKind::FabTurret == 0 is real).
    inline constexpr uint8_t kTFDirectiveAnyParam = 0xFF;

    /// One reward tier: reach `goal` cumulative progress, get paid once.
    struct TFDirectiveTier
    {
        uint32_t goal; ///< cumulative progress needed (strictly increasing per def)
        uint16_t xp;   ///< paid via TFProgressionSystem::ServerAwardXP (kXPReasonDirective)
        uint16_t flux; ///< paid via TFProgressionSystem::ServerGrantFlux (wallet-capped)
    };

    inline constexpr size_t kTFDirectiveTiers = 3;

    /// One directive definition. Pure data — the system interprets kind/param.
    struct TFDirectiveDef
    {
        uint16_t id; ///< stable id (UI / console / future persistence key)
        TFDirectiveKind kind;
        uint8_t param; ///< kind-specific filter, see TFDirectiveKind docs
        const char* name;
        const char* desc;
        std::array<TFDirectiveTier, kTFDirectiveTiers> tiers;
    };

    /// The directive table. Names/tuning are content; ids must stay unique.
    inline constexpr std::array<TFDirectiveDef, 6> kTFDirectives = {{
        {1,
         TFDirectiveKind::KillInfantry,
         0,
         "Scorched Earth",
         "Kill enemy infantry",
         {{{10, 150, 25}, {50, 400, 75}, {200, 1200, 200}}}},
        {2,
         TFDirectiveKind::KillInfantry,
         1,
         "Marksman",
         "Kill enemies with headshots",
         {{{5, 150, 25}, {25, 450, 75}, {100, 1500, 250}}}},
        {3,
         TFDirectiveKind::CaptureRegions,
         0,
         "Territorial Assault",
         "Participate in region captures",
         {{{3, 200, 50}, {15, 600, 150}, {50, 2000, 400}}}},
        {4,
         TFDirectiveKind::DestroyVehicles,
         0,
         "Wrecking Crew",
         "Destroy enemy vehicles",
         {{{3, 200, 50}, {15, 600, 150}, {50, 2000, 400}}}},
        {5,
         TFDirectiveKind::DestroyVehicles,
         static_cast<uint8_t>(VehicleId::Aegis),
         "Aegis Hunter",
         "Destroy Aegis armored transports",
         {{{2, 250, 60}, {10, 750, 180}, {30, 2200, 450}}}},
        {6,
         TFDirectiveKind::DeployItems,
         kTFDirectiveAnyParam,
         "Supply Line",
         "Place support deployables",
         {{{5, 100, 25}, {25, 300, 75}, {100, 1000, 200}}}},
    }};

    inline constexpr size_t kTFDirectiveCount = kTFDirectives.size();

} // namespace Terrafront
