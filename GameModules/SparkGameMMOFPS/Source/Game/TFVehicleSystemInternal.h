/**
 * @file TFVehicleSystemInternal.h
 * @brief Shared internals for the TFVehicleSystem*.cpp split parts: the world
 *        bounds / unit constants and the XZ distance helper used by more than
 *        one part. Include only from the TFVehicleSystem translation units.
 */
#pragma once

namespace Terrafront
{
    namespace VehicleDetail
    {

        inline constexpr float kRadToDeg = 57.2957795f;
        inline constexpr float kWorldMin = 0.0f;
        inline constexpr float kWorldMax = 4096.0f;

        inline float Dist2XZ(const float a[3], const float bx, const float bz)
        {
            const float dx = a[0] - bx;
            const float dz = a[2] - bz;
            return dx * dx + dz * dz;
        }

    } // namespace VehicleDetail
} // namespace Terrafront
