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
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

// =============================================================================
// LightComponent
// =============================================================================

struct LightComponent
{
    enum class Type
    {
        Directional,
        Point,
        Spot
    };

    Type type = Type::Point;
    DirectX::XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngle = 45.0f;
    float spotInnerAngle = 30.0f;
    bool castShadows = false;
    int shadowMapResolution = 1024;

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
