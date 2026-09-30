/**
 * @file TFTerrainModel.h
 * @brief The analytic TERRAFRONT ground: per-continent heightfield parameters and the height function.
 *
 * Pure and engine-free, so the authoritative server, the predicting client and the continent-handoff tests
 * evaluate the same ground. TFWorldSetup owns one parameter set per process (the continent it loaded) and
 * delegates TerrainHeightAt here. Any thread; no allocation after the parameters are loaded.
 */
#pragma once

#include <string>
#include <vector>

namespace Terrafront
{
    struct RegionDef; // Data/TFDataTables.h

    /// Procedural heightfield parameters of one continent. The defaults equal the tf* keys authored in
    /// cindral_wastes.scene [Terrain]; a scene's own keys override them at load.
    struct TFTerrainParams
    {
        float baseHeight = 8.0f;
        float duneAmp = 4.0f;
        float dunePeriodX = 0.0040f;
        float dunePeriodZ = 0.0035f;
        float ridgeAmp = 2.0f;
        // canyon separating the SW (AUC) and SE (HLX) quadrants
        float canyonX = 2048.0f;
        float canyonHalfW = 260.0f;
        float canyonZ0 = 250.0f;
        float canyonZ1 = 1550.0f;
        float canyonDepth = 16.0f;
        // flat build plateau blended in around every region center
        float plateauRadius = 120.0f;
        float plateauSkirt = 180.0f;
        float plateauSky = 40.0f;
        float plateauFort = 30.0f;
        float plateauFacility = 26.0f;
        float plateauOutpost = 20.0f;
    };

    /// Read the tf* keys of @p scenePath's [Terrain] section into @p params. Returns false, leaving
    /// @p params untouched, when the file cannot be read: the caller must not mistake the defaults (another
    /// continent's ground) for this scene's.
    bool TFLoadTerrainParams(const std::string& scenePath, TFTerrainParams& params);

    /// Ground height at world XZ: dunes, canyon, one plateau per region of @p regions (may be null), and the
    /// Sanctuary Haven pad. Deterministic for identical inputs on every role.
    float TFTerrainHeightAt(const TFTerrainParams& params, const std::vector<RegionDef>* regions, float x, float z);

    /// Terrain floor of last resort at the resolved column, applied after the static-body resolve on server and
    /// predicting client alike (TFWorldSetup::ResolveMoveCollision): never below @p groundHeight, and no
    /// downward speed while held up by it.
    inline void TFApplyTerrainBackstop(float groundHeight, float pos[3], float vel[3])
    {
        if (pos[1] < groundHeight)
        {
            pos[1] = groundHeight;
            if (vel[1] < 0.0f)
            {
                vel[1] = 0.0f;
            }
        }
    }
} // namespace Terrafront
