/**
 * @file TFBotSystemInternal.h
 * @brief Shared internals for the TFBotSystem*.cpp split parts: bot tuning
 *        constants, the W2 TFRegionSystem contract-detection shim, the W9
 *        class-ability seam shim and small math helpers. Include only from
 *        the TFBotSystem translation units.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h" // kTFRespawnDelaySec

// W9 bots-v2: the class-abilities lane's system is optional at this lane's
// compile time. When the header exists in the tree, include it so the seam
// concepts below see the full type; when it does not, TFGameContext has no
// `abilities` member either (the integrator lands both together) and every
// ability call in this file collapses to a compile-time no-op.
#if __has_include("Game/TFAbilitySystem.h")
#include "Game/TFAbilitySystem.h"
#endif

#include <concepts>
#include <cstdint>
#include <numbers>
#include <type_traits>

namespace Terrafront
{
    namespace BotDetail
    {

        inline constexpr float kThinkIntervalSec = 0.20f; // 5 Hz brain
        inline constexpr float kEngageRangeM = 60.0f;     // acquire targets inside this
        inline constexpr float kDisengageRangeM = 70.0f;  // keep firing until this (hysteresis)
        inline constexpr float kSprintBeyondM = 80.0f;    // sprint when objective is far
        inline constexpr float kHoldFireCloseM = 15.0f;   // stop advancing inside this
        inline constexpr float kAimErrorDeg = 1.5f;       // +-1.5 deg cone on every shot
        inline constexpr float kChestHeightM = 1.20f;     // aim point above target feet
        inline constexpr float kLosStepM = 2.0f;          // terrain march step (TerrainBlocked-style)
        inline constexpr float kStuckWindowSec = 2.0f;    // pos unchanged this long -> jump
        inline constexpr float kStuckEpsM = 0.25f;
        inline constexpr float kSpawnRetrySec = 1.0f;
        inline constexpr float kRespawnWaitSec = kTFRespawnDelaySec + 0.25f; // outlast the server timer
        inline constexpr uint32_t kSelectableClasses = 5;                    // Ghost..Bulwark (no Colossus, DESIGN §1)

        // engage-vs-advance (health/ammo fitness)
        inline constexpr float kLowHealthFrac = 0.35f; // below this pool fraction -> give ground

        // fireteam cohesion
        inline constexpr float kRegroupBeyondM = 60.0f; // strung out past this -> close on the leader

        // vehicle use (driver seat 0 only)
        inline constexpr float kVehSeekBeyondM = 120.0f; // objective farther than this -> want wheels
        inline constexpr float kVehScanRadiusM = 60.0f;  // hull must be within this to walk to it
        inline constexpr float kVehExitWithinM = 60.0f;  // objective closer than this -> dismount
        inline constexpr float kVehEnterTryM = 3.0f;     // issue the seat op inside this XZ range
        inline constexpr float kVehStuckSec = 3.0f;      // wedged ride pose this long -> recover
        inline constexpr double kVehRetrySec = 10.0;     // cooldown after a failed/abandoned plan
        inline constexpr uint8_t kVehMaxEnterTries = 3;

        // driving v2 (W9 bots-v2)
        inline constexpr float kVehProbeAheadM = 12.0f;    // terrain look-ahead probe distance
        inline constexpr float kVehProbeSideRad = 0.7f;    // ~40 deg side probes around a wall
        inline constexpr float kVehClimbLimitM = 3.0f;     // rise beyond this ahead = treat as a wall
        inline constexpr double kVehReverseSec = 1.6;      // reverse-out recovery phase length
        inline constexpr double kVehWedgeWindowSec = 20.0; // 2nd wedge inside this -> dismount
        inline constexpr float kVehHardTurnRad = 1.5f;     // ease throttle past this yaw error

        // Vulture VTOL flight (W9 bots-v2; lift axis = TFB_Jump/TFB_Crouch)
        inline constexpr float kVulCruiseMinAglM = 25.0f; // climb below this while cruising
        inline constexpr float kVulCruiseMaxAglM = 45.0f; // descend above this while cruising
        inline constexpr float kVulLandWithinM = 60.0f;   // start the landing descent inside this
        inline constexpr float kVulExitAglM = 1.5f;       // try the (landed-gated) exit below this
        inline constexpr float kVulStuckSec = 6.0f;       // hover wedge window (yaw-in-place is slow)

        // class abilities (W9 bots-v2)
        inline constexpr double kAbilityCheckSec = 2.5; // situational trigger rate limit
        inline constexpr float kMedtechHealRadiusM = 15.0f;
        inline constexpr float kMedtechHurtFrac = 0.7f; // friendly pool below this -> heal
        inline constexpr float kStrikerGapM = 25.0f;    // target farther than this -> jets

        // local obstacle avoidance (W12 bot-navigation)
        inline constexpr float kFeelerLenM = 3.0f;         // chest-height feeler ray length
        inline constexpr float kFeelerSideRad = 0.5236f;   // ±30 deg side feelers
        inline constexpr float kAvoidSteerRad = 0.9f;      // detour angle past a blocked forward
        inline constexpr float kBackTurnRad = 2.0944f;     // 120 deg pocket escape turn
        inline constexpr double kBackTurnSec = 1.0;        // pocket escape leg length
        inline constexpr double kBlockedMemorySec = 5.0;   // remember a blocked heading this long
        inline constexpr float kUnstickMinMoveM = 1.5f;    // < this progress inside the window = stalled
        inline constexpr double kUnstickWindowSec = 3.0;   // progress measurement window
        inline constexpr double kUnstickSteerSec = 2.0;    // random-heading escape leg length
        inline constexpr uint8_t kUnstickScatterAfter = 3; // consecutive unsticks -> chaos scatter

        // chaos pilots (W9 bots-v2): deterministic vehicle exercise for tf_validate
        inline constexpr uint32_t kChaosVulturePilotSlot = 1; // AUC; flies a Vulture
        inline constexpr uint32_t kChaosDriverPilotSlot = 2;  // HLX; drives a Drifter
        inline constexpr double kPilotBuyRetrySec = 4.0;
        inline constexpr uint32_t kBotChaosFluxGrant = 200; // Drifter money for every bot
        inline constexpr uint16_t kPilotXPGrant = 45000;    // rank >= 15 (500 * n^1.6): Vulture gate

        // ---------------------------------------------------------------------------
        // W2 TFRegionSystem contract detection (kept from the regions-parallel wave).
        // The `if constexpr` discard only works inside a template, so the helpers
        // deduce R from the ctx pointer instead of naming TFRegionSystem directly.
        // ---------------------------------------------------------------------------

        template <typename R>
        concept TFHasRegionQueries = requires(const R& r, RegionId id, FactionId f) {
            { r.OwnerOf(id) } -> std::convertible_to<FactionId>;
            { r.IsCapturable(id, f) } -> std::convertible_to<bool>;
        };

        template <typename R>
        concept TFHasCaptureProgress = requires(const R& r, RegionId id, FactionId& f, bool& c) {
            { r.CaptureProgress(id, f, c) } -> std::convertible_to<float>;
        };

        template <typename R> FactionId QueryRegionOwner(const R* regions, RegionId id, FactionId fallback)
        {
            if constexpr (TFHasRegionQueries<R>)
            {
                if (regions)
                    return regions->OwnerOf(id);
            }
            (void)regions;
            (void)id;
            return fallback;
        }

        template <typename R>
        bool QueryRegionCapturable(const R* regions, RegionId id, FactionId attacker, bool fallback)
        {
            if constexpr (TFHasRegionQueries<R>)
            {
                if (regions)
                    return regions->IsCapturable(id, attacker);
            }
            (void)regions;
            (void)id;
            (void)attacker;
            return fallback;
        }

        /// Capture progress toward `outCapturing` (0 when the region system is not
        /// queryable — bots then fall back to pure nearest-region marching).
        template <typename R>
        float QueryCaptureProgress(const R* regions, RegionId id, FactionId& outCapturing, bool& outContested)
        {
            if constexpr (TFHasCaptureProgress<R>)
            {
                if (regions)
                    return regions->CaptureProgress(id, outCapturing, outContested);
            }
            (void)regions;
            (void)id;
            outCapturing = FactionId::None;
            outContested = false;
            return 0.0f;
        }

        // ---------------------------------------------------------------------------
        // W9 class-ability seam detection (same philosophy as the region shim
        // above): the class-abilities lane lands TFAbilitySystem + a
        // TFGameContext::abilities pointer together (integrator). Until both
        // exist in this tree these helpers compile to constant-false no-ops;
        // once they land, bots drive the real public CanUseAbility/UseAbility
        // seam with no change needed here. The `if constexpr` discard only works
        // inside a template, so C is deduced from the ctx pointer.
        // ---------------------------------------------------------------------------

        template <typename C>
        concept TFHasAbilityCtxPtr = requires(C& c) { c.abilities; };

        template <typename A>
        concept TFHasAbilityApi = requires(A& a, PlayerId p) {
            { a.CanUseAbility(p) } -> std::convertible_to<bool>;
            a.UseAbility(p);
        };

        /// True when the seam is compiled in AND published at runtime.
        template <typename C> bool AbilitySeamPresent(C* ctx)
        {
            if constexpr (TFHasAbilityCtxPtr<C>)
            {
                if (ctx && ctx->abilities)
                {
                    using A = std::remove_pointer_t<std::remove_cvref_t<decltype(ctx->abilities)>>;
                    if constexpr (TFHasAbilityApi<A>)
                        return true;
                }
            }
            (void)ctx;
            return false;
        }

        /// CanUseAbility -> UseAbility through the seam. True only when an
        /// activation actually happened.
        template <typename C> bool TryUseAbilitySeam(C* ctx, PlayerId player)
        {
            if constexpr (TFHasAbilityCtxPtr<C>)
            {
                if (ctx && ctx->abilities)
                {
                    using A = std::remove_pointer_t<std::remove_cvref_t<decltype(ctx->abilities)>>;
                    if constexpr (TFHasAbilityApi<A>)
                    {
                        if (ctx->abilities->CanUseAbility(player))
                        {
                            ctx->abilities->UseAbility(player);
                            return true;
                        }
                    }
                }
            }
            (void)ctx;
            (void)player;
            return false;
        }

        /// Static ownership guess when the live region system is not queryable yet:
        /// regions.json initial owner (indexed by RegionId), else the home faction.
        inline FactionId FallbackOwner(const ContinentDef& cont, const RegionDef& r)
        {
            if (r.id < cont.initialOwner.size())
                return cont.initialOwner[r.id];
            return r.homeFaction;
        }

        inline float WrapPi(float a)
        {
            constexpr float kPi = std::numbers::pi_v<float>;
            constexpr float kTau = 2.0f * kPi;
            while (a > kPi)
                a -= kTau;
            while (a < -kPi)
                a += kTau;
            return a;
        }

    } // namespace BotDetail
} // namespace Terrafront
