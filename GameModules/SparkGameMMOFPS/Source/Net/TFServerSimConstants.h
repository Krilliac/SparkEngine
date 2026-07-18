/**
 * @file TFServerSimConstants.h
 * @brief Shared simulation constants for the TFServerSim implementation split
 *        (TFServerSim.cpp / TFServerSimMovement.cpp / TFServerSimNetHandlers.cpp).
 *        Internal to those translation units — not part of the class interface.
 */
#pragma once

#include <cstddef>

namespace Terrafront
{

    // Engine-side movement limits. Balance numbers (run/sprint speed) come from
    // classes.json via TFDataTables; these are simulation constants.
    inline constexpr float kWorldMin = 0.0f;
    inline constexpr float kWorldMax = 4096.0f;
    inline constexpr float kPitchLimitRad = 1.55f;
    inline constexpr float kRespawnDelaySec = 8.0f; // DESIGN.md §4 default
    inline constexpr float kSpeedTolerance = 1.25f; // sprint * this == hard cap
    inline constexpr int kMaxInputsPerTick = 3;     // catch-up bound per fixed tick
    inline constexpr size_t kMaxQueuedInputs = 32;
    inline constexpr float kDefaultRunSpeed = 5.2f; // fallback if classes.json missing
    inline constexpr float kDefaultSprintSpeed = 7.2f;
    inline constexpr float kRadToDeg = 57.2957795f;
    // W13 anti-cheat lane: TF_FireEvent claimed origin vs the server's
    // trusted pawn position (see TFServerValidation.h). Generous relative
    // to real network latency/prediction drift (well under a meter) so it
    // only ever fires on a gross, no-legitimate-cause lie.
    inline constexpr float kMaxFireOriginDivergenceM = 6.0f;

} // namespace Terrafront
