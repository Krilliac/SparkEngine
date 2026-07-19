/**
 * @file TFRegionSystemInternal.h
 * @brief Shared internals for the TFRegionSystem*.cpp split parts: the
 *        playable-faction predicate plus the capture-radius constants and
 *        stand-in test used by the capture tick, the HUD feed and the
 *        tf_capture_debug report. Include only from the TFRegionSystem
 *        translation units.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"

namespace Terrafront
{
    namespace RegionDetail
    {

        constexpr float kCaptureRadiusM = 10.0f; // DESIGN §4: stand-in radius
        constexpr float kCaptureRadiusSq = kCaptureRadiusM * kCaptureRadiusM;

        inline bool IsPlayableFaction(FactionId f)
        {
            return f == FactionId::MRA || f == FactionId::AUC || f == FactionId::HLX;
        }

        /// Alive pawn feet position within 10 m (XZ) of any of the region's points?
        inline bool InCaptureRadius(const RegionDef& def, const float pos[3])
        {
            for (const auto& pt : def.capturePoints)
            {
                const float dx = pos[0] - pt[0];
                const float dz = pos[2] - pt[1];
                if (dx * dx + dz * dz <= kCaptureRadiusSq)
                    return true;
            }
            return false;
        }

    } // namespace RegionDetail
} // namespace Terrafront
