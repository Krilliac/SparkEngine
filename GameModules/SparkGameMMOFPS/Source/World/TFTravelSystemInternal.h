/**
 * @file TFTravelSystemInternal.h
 * @brief Shared internals for the TFTravelSystem*.cpp split parts: the XZ
 *        distance helper used by more than one part. Include only from the
 *        TFTravelSystem translation units.
 */
#pragma once

namespace Terrafront
{
    namespace TravelDetail
    {

        inline float DistSqXZ(const float a[3], float bx, float bz)
        {
            const float dx = a[0] - bx;
            const float dz = a[2] - bz;
            return dx * dx + dz * dz;
        }

    } // namespace TravelDetail
} // namespace Terrafront
