/**
 * @file TFDeployableSystemInternal.h
 * @brief Shared internals for the TFDeployableSystem*.cpp split parts: the
 *        ammo-pack pulse cadence shared by the placement path and the server
 *        tick, and the squared-distance helper shared by the tick mechanics
 *        and the splash-damage path. Include only from the TFDeployableSystem
 *        translation units. (Namespace is DeploySysDetail so it can never
 *        collide with TFDeployablePlacement.cpp's file-local helpers.)
 */
#pragma once

namespace Terrafront
{
    namespace DeploySysDetail
    {

        inline constexpr float kAmmoPackPulseSec = 5.0f;

        inline float Dist2(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

    } // namespace DeploySysDetail
} // namespace Terrafront
