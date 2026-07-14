/**
 * @file LightComponents.h
 * @brief ECS lighting component: LightComponent
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

// =============================================================================
// LightComponent
// =============================================================================

/**
 * @brief Light source attached to an entity, consumed by the LightingSystem.
 *
 * Directional lights illuminate the entire scene (sun/moon). Point lights
 * radiate in all directions with distance attenuation. Spot lights project
 * a cone with configurable inner/outer angles for soft falloff.
 */
struct LightComponent
{
    /// Light source type, determines which parameters are active.
    enum class Type
    {
        Directional, ///< Infinite-distance parallel rays (sun). Ignores range.
        Point,       ///< Omnidirectional with distance attenuation.
        Spot         ///< Cone-shaped with inner/outer angle falloff.
    };

    Type type = Type::Point;          ///< Light source type.
    DirectX::XMFLOAT3 color{1, 1, 1}; ///< Light color (linear RGB, not sRGB).
    float intensity = 1.0f;           ///< Brightness multiplier (candelas for point/spot, lux for directional).
    float range = 10.0f;              ///< Maximum influence distance in meters (Point/Spot only).
    float spotAngle = 45.0f;          ///< Outer cone angle in degrees (Spot only); full falloff at this angle.
    float spotInnerAngle = 30.0f;     ///< Inner cone angle in degrees (Spot only); full intensity within this angle.
    bool castShadows = false;         ///< Generate a shadow map for this light (expensive for point lights).
    int shadowMapResolution = 1024;   ///< Shadow map width/height in texels (must be power of 2).

    /**
     * @brief Validate that light parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        ASSERT_MSG(intensity >= 0.0f, "Light intensity must be non-negative");
        ASSERT_MSG(range > 0.0f, "Light range must be positive");
        ASSERT_MSG(spotAngle > 0.0f && spotAngle <= 180.0f, "Spot angle must be in (0, 180]");
        ASSERT_MSG(spotInnerAngle >= 0.0f && spotInnerAngle <= spotAngle, "Spot inner angle must be in [0, spotAngle]");
        ASSERT_MSG(shadowMapResolution > 0 && (shadowMapResolution & (shadowMapResolution - 1)) == 0,
                   "Shadow map resolution must be a positive power of 2");
        return intensity >= 0.0f && range > 0.0f && spotAngle > 0.0f && spotAngle <= 180.0f && spotInnerAngle >= 0.0f &&
               spotInnerAngle <= spotAngle && shadowMapResolution > 0;
    }
};
