/** @file TFTerrainModel.cpp @brief Analytic continent ground shared by server, client and handoff. */
#include "World/TFTerrainModel.h"

#include "Data/TFDataTables.h"
#include "World/TFSanctuaryZone.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace Terrafront
{
    namespace
    {
        float SmoothStep(float e0, float e1, float x)
        {
            if (e1 <= e0)
            {
                return x < e0 ? 0.0f : 1.0f;
            }
            const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        float PlateauHeight(const TFTerrainParams& params, const std::string& tier)
        {
            if (tier == "skyanchor")
            {
                return params.plateauSky;
            }
            if (tier == "fort")
            {
                return params.plateauFort;
            }
            if (tier == "facility")
            {
                return params.plateauFacility;
            }
            return params.plateauOutpost;
        }

        float* TerrainKey(TFTerrainParams& params, const std::string& key)
        {
            struct Entry
            {
                const char* key;
                float TFTerrainParams::*field;
            };
            static constexpr Entry kKeys[] = {
                {"tfBaseHeight", &TFTerrainParams::baseHeight},
                {"tfDuneAmp", &TFTerrainParams::duneAmp},
                {"tfDunePeriodX", &TFTerrainParams::dunePeriodX},
                {"tfDunePeriodZ", &TFTerrainParams::dunePeriodZ},
                {"tfRidgeAmp", &TFTerrainParams::ridgeAmp},
                {"tfCanyonX", &TFTerrainParams::canyonX},
                {"tfCanyonHalfWidth", &TFTerrainParams::canyonHalfW},
                {"tfCanyonZ0", &TFTerrainParams::canyonZ0},
                {"tfCanyonZ1", &TFTerrainParams::canyonZ1},
                {"tfCanyonDepth", &TFTerrainParams::canyonDepth},
                {"tfPlateauRadius", &TFTerrainParams::plateauRadius},
                {"tfPlateauSkirt", &TFTerrainParams::plateauSkirt},
                {"tfPlateauSkyanchor", &TFTerrainParams::plateauSky},
                {"tfPlateauFort", &TFTerrainParams::plateauFort},
                {"tfPlateauFacility", &TFTerrainParams::plateauFacility},
                {"tfPlateauOutpost", &TFTerrainParams::plateauOutpost},
            };
            for (const Entry& entry : kKeys)
            {
                if (key == entry.key)
                {
                    return &(params.*entry.field);
                }
            }
            return nullptr;
        }
    } // namespace

    bool TFLoadTerrainParams(const std::string& scenePath, TFTerrainParams& params)
    {
        std::ifstream file(scenePath);
        if (!file.is_open())
        {
            return false;
        }
        TFTerrainParams loaded = params;
        std::string line;
        bool inTerrain = false;
        while (std::getline(file, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#' || line[0] == ';')
            {
                continue;
            }
            if (line.front() == '[' && line.back() == ']')
            {
                inTerrain = (line == "[Terrain]");
                continue;
            }
            const size_t eq = line.find('=');
            if (!inTerrain || eq == std::string::npos || !line.starts_with("tf"))
            {
                continue;
            }
            if (float* field = TerrainKey(loaded, line.substr(0, eq)))
            {
                *field = std::strtof(line.c_str() + eq + 1, nullptr);
            }
        }
        params = loaded;
        return true;
    }

    float TFTerrainHeightAt(const TFTerrainParams& p, const std::vector<RegionDef>* regions, float x, float z)
    {
        // Dune base
        float h = p.baseHeight + p.duneAmp * std::sin(x * p.dunePeriodX) * std::cos(z * p.dunePeriodZ) +
                  p.ridgeAmp * std::sin(x * 0.013f + z * 0.011f);

        // Canyon between SW (AUC) and SE (HLX) territory
        const float nx = (x - p.canyonX) / p.canyonHalfW;
        if (std::fabs(nx) < 1.0f)
        {
            const float across = 1.0f - nx * nx;
            const float along = SmoothStep(p.canyonZ0 - 300.0f, p.canyonZ0, z) *
                                (1.0f - SmoothStep(p.canyonZ1, p.canyonZ1 + 300.0f, z));
            h -= p.canyonDepth * across * along;
        }

        // Flat mesa plateau around each region center (scene objects sit at exactly these heights). Regions are
        // far apart relative to the skirt, so sequential blending is order-independent in practice.
        if (regions)
        {
            for (const RegionDef& r : *regions)
            {
                const float dx = x - r.centerX;
                const float dz = z - r.centerZ;
                const float distSq = dx * dx + dz * dz;
                const float outer = p.plateauRadius + p.plateauSkirt;
                if (distSq >= outer * outer)
                {
                    continue;
                }
                const float w = 1.0f - SmoothStep(p.plateauRadius, outer, std::sqrt(distSq));
                h += (PlateauHeight(p, r.tier) - h) * w;
            }
        }

        // Sanctuary Haven pad: a flat plateau at the reserved NW-corner zone, blended exactly like the region
        // plateaus. Its constants are compile-time (TFSanctuaryZone.h), so inside the pad radius the ground is
        // the same on every continent regardless of that continent's parameters.
        {
            const float dx = x - kTFSanctuaryCenterX;
            const float dz = z - kTFSanctuaryCenterZ;
            const float distSq = dx * dx + dz * dz;
            const float outer = kTFSanctuaryPlateauRadius + kTFSanctuaryPlateauSkirt;
            if (distSq < outer * outer)
            {
                const float w = 1.0f - SmoothStep(kTFSanctuaryPlateauRadius, outer, std::sqrt(distSq));
                h += (kTFSanctuaryPadY - h) * w;
            }
        }
        return h;
    }
} // namespace Terrafront
