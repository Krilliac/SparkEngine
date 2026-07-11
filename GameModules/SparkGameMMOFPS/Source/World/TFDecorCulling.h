/**
 * @file TFDecorCulling.h
 * @brief Distance culling for region decor entities (W10 distance-culling lane).
 *
 * OWNERSHIP: distance-culling lane — this header plus the culling section of
 * TFRegionDecor.cpp (TFRegionDecor::UpdateCulling, a deliberately separate
 * function so the decor-collision lane's layout restructure merges trivially).
 *
 * Problem: the ECS render path (TFWorldSetup::RenderWorld) walks every
 * Transform+MeshRenderer entity every frame. W9 decor stamps up to 20 entities
 * per region x 13 regions (~127 in practice), each a full mesh draw whether it
 * is 30 m or 3 km away. MeshRenderer.visible already short-circuits that loop
 * (the `if (!mr.visible) continue;` guard runs before any mesh load or draw),
 * so culling is just flipping that flag from camera distance.
 *
 * Design (dumb and correct, no octrees this wave):
 *  - Each decor entity records its world XZ and a cull class at SPAWN time
 *    (decor never moves): buildings (template kit pieces) cull at 600 m,
 *    props (seeded scatter clutter) at 250 m.
 *  - Hysteresis: an entity becomes visible when the camera is closer than
 *    cullRange, and hidden only beyond cullRange * 1.10 — so edge-of-range
 *    entities never flicker as the camera oscillates around the boundary.
 *  - Re-evaluated every kDecorCullIntervalFrames frames (~0.25 s at 60 fps),
 *    not every frame — 127 distance checks at 4 Hz is noise.
 *
 * VISUAL-ONLY: decor has no physics bodies and never affects movement or
 * collision, so client-side visibility toggling cannot violate the
 * server/client determinism contract.
 */
#pragma once

#include <cstdint>

namespace Terrafront
{

    /// Per-entity cull record captured at spawn time (decor is static).
    struct TFDecorCullEntry
    {
        uint32_t entity = 0;      ///< engine EntityID (0 = unused slot)
        float x = 0.0f;           ///< world X at spawn
        float z = 0.0f;           ///< world Z at spawn
        float cullRange = 250.0f; ///< show when camera XZ distance < this (meters)
        bool visible = true;      ///< last state applied to MeshRenderer.visible
    };

    /// Cull ranges by decor class (stored per entity at spawn).
    constexpr float kDecorCullRangeBuildingM = 600.0f; ///< template kit pieces
    constexpr float kDecorCullRangePropM = 250.0f;     ///< scatter clutter

    /// Hide boundary = cullRange * this (+10%) so boundary camera jitter
    /// cannot flicker an entity between shown and hidden.
    constexpr float kDecorCullHysteresis = 1.10f;

    /// Frames between cull passes (~0.25 s at 60 fps).
    constexpr uint32_t kDecorCullIntervalFrames = 15u;

    /// Pure hysteresis decision: show inside cullRange, hide only beyond
    /// cullRange * kDecorCullHysteresis, keep the current state in between.
    /// Squared distances throughout — no sqrt on the (4 Hz) hot path.
    inline bool DecorShouldBeVisible(float distSqM2, float cullRangeM, bool currentlyVisible)
    {
        const float showSq = cullRangeM * cullRangeM;
        if (!currentlyVisible)
            return distSqM2 < showSq;
        const float hideR = cullRangeM * kDecorCullHysteresis;
        return distSqM2 <= hideR * hideR;
    }

} // namespace Terrafront
