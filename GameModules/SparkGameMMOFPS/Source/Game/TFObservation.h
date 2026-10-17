/**
 * @file TFObservation.h
 * @brief TF-110: one process's view of the TERRAFRONT world in a
 *        machine-comparable text form (tf_observe, Console/TFCommandsHarness.cpp).
 *
 * The authority fills a TFObservation from its own systems and a pure client
 * fills it from its replicated mirrors (TFPlayerSystem, TFRegionSystem and
 * TFVehicleSystem serve both roles through the same API). FormatObservation
 * emits one "[TF-OBSERVE]" header line followed by body lines sorted by id,
 * with positions quantized to kTFObservePosQuantumM, so a harness can diff the
 * body lines of a server and its clients to prove they converged. The header
 * carries per-process fields (role, clock) and is not part of that comparison.
 *
 * Header-only so the format is unit-tested without the module
 * (Tests/TestTFObservation.cpp).
 */
#pragma once

#include "Core/TFTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Terrafront
{

    /// Position quantum for observation output: small enough to catch real
    /// divergence, large enough that interpolation jitter formats identically.
    constexpr float kTFObservePosQuantumM = 0.25f;

    struct TFObservedPawn
    {
        PlayerId id = kInvalidPlayer;
        uint8_t faction = 0;
        uint8_t cls = 0;
        int16_t health = 0;
        float pos[3] = {0.0f, 0.0f, 0.0f};
    };

    struct TFObservedVehicle
    {
        uint32_t netId = 0;
        uint16_t def = 0;
        PlayerId driver = kInvalidPlayer;
        int16_t hp = 0;
        float pos[3] = {0.0f, 0.0f, 0.0f};
    };

    struct TFObservation
    {
        std::string role;   ///< "server", "host" or "client" (header only)
        double clock = 0.0; ///< this process's clock in seconds (header only)
        PlayerId self = kInvalidPlayer;
        std::string continentKey;
        std::vector<TFObservedPawn> pawns; ///< alive pawns
        std::vector<std::pair<RegionId, uint8_t>> regionOwners;
        std::vector<TFObservedVehicle> vehicles;
        std::string loadoutPrimary; ///< self's saved primary key (empty = class default)
        uint32_t flux = 0;
        uint16_t rank = 0;
    };

    /// Round to the observation quantum; -0 prints as 0.
    inline float QuantizeObservedMeters(float meters)
    {
        const float q = std::round(meters / kTFObservePosQuantumM) * kTFObservePosQuantumM;
        return q == 0.0f ? 0.0f : q;
    }

    inline std::string FormatObservedPos(const float pos[3])
    {
        return std::format("{:.2f},{:.2f},{:.2f}", QuantizeObservedMeters(pos[0]), QuantizeObservedMeters(pos[1]),
                           QuantizeObservedMeters(pos[2]));
    }

    inline std::string FormatObservation(const TFObservation& obs)
    {
        std::vector<TFObservedPawn> pawns = obs.pawns;
        std::sort(pawns.begin(), pawns.end(),
                  [](const TFObservedPawn& a, const TFObservedPawn& b) { return a.id < b.id; });
        std::vector<std::pair<RegionId, uint8_t>> regions = obs.regionOwners;
        std::sort(regions.begin(), regions.end());
        std::vector<TFObservedVehicle> vehicles = obs.vehicles;
        std::sort(vehicles.begin(), vehicles.end(),
                  [](const TFObservedVehicle& a, const TFObservedVehicle& b) { return a.netId < b.netId; });

        std::string out =
            std::format("[TF-OBSERVE] role={} clock={:.3f} self={} continent={} pawns={} regions={} "
                        "vehicles={}",
                        obs.role, obs.clock, obs.self, obs.continentKey, pawns.size(), regions.size(), vehicles.size());
        out += std::format("\n[TF-OBSERVE] self id={} loadout={} flux={} rank={}", obs.self,
                           obs.loadoutPrimary.empty() ? "default" : obs.loadoutPrimary, obs.flux, obs.rank);
        for (const TFObservedPawn& p : pawns)
            out += std::format("\n[TF-OBSERVE] pawn id={} faction={} class={} health={} pos={}", p.id, p.faction, p.cls,
                               p.health, FormatObservedPos(p.pos));
        for (const auto& [region, owner] : regions)
            out += std::format("\n[TF-OBSERVE] region id={} owner={}", region, owner);
        for (const TFObservedVehicle& v : vehicles)
            out += std::format("\n[TF-OBSERVE] vehicle net={} def={} driver={} hp={} pos={}", v.netId, v.def, v.driver,
                               v.hp, FormatObservedPos(v.pos));
        return out;
    }

    /// View angles (radians, camera convention) that aim from `eye` at
    /// `target`: forward = (cos(pitch) sin(yaw), -sin(pitch), cos(pitch) cos(yaw)),
    /// the BuildViewRay convention TFWeaponSystem fires along.
    inline void TFAimAngles(const float eye[3], const float target[3], float& outYaw, float& outPitch)
    {
        const float dx = target[0] - eye[0];
        const float dy = target[1] - eye[1];
        const float dz = target[2] - eye[2];
        outYaw = std::atan2(dx, dz);
        outPitch = std::atan2(-dy, std::sqrt(dx * dx + dz * dz));
    }

} // namespace Terrafront
