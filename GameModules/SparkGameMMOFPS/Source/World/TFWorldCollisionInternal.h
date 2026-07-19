/**
 * @file TFWorldCollisionInternal.h
 * @brief Shared internals for the TFWorldCollision*.cpp split parts: the body
 *        cap, the degenerate-AABB floor and the degree->radian factor used by
 *        both the scene Build() part and the W10/W11 decor-OBB part. Include
 *        only from the TFWorldCollision translation units.
 */
#pragma once

#include <cstddef>

namespace Terrafront
{
    namespace WorldCollisionDetail
    {

        constexpr size_t kMaxBodies = 512;       // safety cap (scene ships 109 objects)
        constexpr float kMinHalfExtentM = 0.05f; // degenerate-AABB floor
        constexpr float kDegToRad = 0.01745329252f;

    } // namespace WorldCollisionDetail
} // namespace Terrafront
