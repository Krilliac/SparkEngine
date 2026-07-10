/**
 * @file TFSanctuaryZone.h
 * @brief Sanctuary Haven zone constants + pure positional helpers (continents lane).
 *
 * OWNERSHIP: continents lane (2026-07-10 warpgate/sanctuary wave), alongside
 * World/TFTravelSystem.h/.cpp and Assets/Scenes/MMOFPS/sanctuary_haven.scene.
 *
 * ## Architecture: single-shared-sim position isolation (the "mapId trick")
 * TERRAFRONT v1 keeps ONE authoritative sim world (TFServerSim clamps movement
 * to [0,4096]^2, TFReplication broadcasts every pawn to every client, one
 * AreaServer covers the whole continent). A true multi-map split would have to
 * touch all three of those systems plus the frozen wire protocol. Instead the
 * sanctuary is a RESERVED COORDINATE ZONE of the same sim world: the NW corner
 * rectangle below, far outside every regions.json plateau (nearest region
 * center, Skyanchor Aurelia at 2048/3600, is ~1.7 km from the pad).
 *
 * The uint8 mapId still exists as a first-class concept — 0 = Sanctuary Haven,
 * 1 = Cindral Wastes — but v1 DERIVES it from position via TFTravel_MapIdAt().
 * Everything that must behave differently in the sanctuary (no damage, no
 * redeploy, travel terminal) keys off these pure functions, so a future real
 * multi-map upgrade only has to replace the derivation, not the call sites.
 *
 * ## Determinism contract
 * These constants participate in TFWorldSetup::TerrainHeightAt (the sanctuary
 * pad plateau) and in the server/client movement + damage rules. They are
 * compile-time constexpr in this shared header — identical on every role by
 * construction (same discipline as TFWorldCollision's tuning block).
 * sanctuary_haven.scene is authored AT these heights; keep them in sync.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>

namespace Terrafront
{

    // ------------------------------------------------------------------ map ids
    constexpr uint8_t kTFMapSanctuary = 0;     ///< Sanctuary Haven (starting zone)
    constexpr uint8_t kTFMapCindralWastes = 1; ///< the live continent
    constexpr uint8_t kTFMapCount = 2;

    // ---------------------------------------------------- sanctuary zone bounds
    // NW-corner rectangle of the shared 4096 m world. Everything inside is
    // sanctuary rules (no damage, no redeploy, terminal travel only).
    constexpr float kTFSanctuaryMinX = 40.0f;
    constexpr float kTFSanctuaryMaxX = 600.0f;
    constexpr float kTFSanctuaryMinZ = 3496.0f;
    constexpr float kTFSanctuaryMaxZ = 4056.0f;

    // ------------------------------------------------------- sanctuary platform
    // Flat pad blended into TFWorldSetup::TerrainHeightAt exactly like the
    // regions.json plateaus (radius/skirt smoothstep). Scene objects in
    // sanctuary_haven.scene sit at kTFSanctuaryPadY.
    constexpr float kTFSanctuaryCenterX = 320.0f;
    constexpr float kTFSanctuaryCenterZ = 3776.0f;
    constexpr float kTFSanctuaryPadY = 24.0f;
    constexpr float kTFSanctuaryPlateauRadius = 200.0f;
    constexpr float kTFSanctuaryPlateauSkirt = 150.0f;

    /// Travel terminal (authored in sanctuary_haven.scene as term_sanctuary).
    constexpr float kTFSanctuaryTerminalX = 320.0f;
    constexpr float kTFSanctuaryTerminalZ = 3720.0f;
    /// Walk-up prompt / server-side validation radius around the terminal (XZ).
    constexpr float kTFTravelTerminalUseM = 6.0f;

    /// Spawn pads (XZ; Y is TerrainHeightAt == the pad plateau). Placement is
    /// pad[player % 4] so simultaneous arrivals don't stack on one point.
    constexpr float kTFSanctuarySpawnPads[4][2] = {
        {296.0f, 3808.0f},
        {344.0f, 3808.0f},
        {296.0f, 3744.0f},
        {344.0f, 3744.0f},
    };
    constexpr uint32_t kTFSanctuarySpawnPadCount = 4;

    // ------------------------------------------------------------ pure helpers

    /// True when the world XZ lies inside the sanctuary zone rectangle.
    /// Pure + shared-header: safe to call from any system on any role
    /// (TFDamageSystem no-damage rule, redeploy gating, HUD hints).
    inline bool TFTravel_IsInSanctuary(float x, float z)
    {
        return x >= kTFSanctuaryMinX && x <= kTFSanctuaryMaxX && z >= kTFSanctuaryMinZ && z <= kTFSanctuaryMaxZ;
    }

    /// v1 positional mapId derivation (see file header).
    inline uint8_t TFTravel_MapIdAt(float x, float z)
    {
        return TFTravel_IsInSanctuary(x, z) ? kTFMapSanctuary : kTFMapCindralWastes;
    }

    inline const char* TFTravel_MapName(uint8_t mapId)
    {
        return mapId == kTFMapSanctuary ? "Sanctuary Haven" : "Cindral Wastes";
    }

} // namespace Terrafront
