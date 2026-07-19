/**
 * @file TFGrenadeSystemInternal.h
 * @brief Shared internals for the TFGrenadeSystem*.cpp split parts: the boom
 *        flipbook lifetime shared by the mirror-advance and render passes, and
 *        the view-angles -> unit-direction helper shared by the server throw
 *        path and the client send path. Include only from the TFGrenadeSystem
 *        translation units. (Namespace is GrenadeSysDetail — GrenadeDetail is
 *        taken by the public quantization helpers in TFGrenadeSystem.h.)
 */
#pragma once

#include <cmath>

namespace Terrafront
{
    namespace GrenadeSysDetail
    {

        /// Boom presentation (client only): flipbook lifetime, shared by
        /// ClientAdvance (aging/prune) and RenderClientFx (size/frame/fade).
        inline constexpr float kBoomLifeSec = 0.45f;

        /// TFWeaponSystem::BuildViewRay convention (yaw/pitch radians -> unit dir).
        inline void ViewDir(float yaw, float pitch, float out[3])
        {
            const float cp = std::cos(pitch);
            out[0] = cp * std::sin(yaw);
            out[1] = -std::sin(pitch);
            out[2] = cp * std::cos(yaw);
        }

    } // namespace GrenadeSysDetail
} // namespace Terrafront
