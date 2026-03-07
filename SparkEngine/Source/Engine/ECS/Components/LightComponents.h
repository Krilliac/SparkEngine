/**
 * @file LightComponents.h
 * @brief ECS lighting component: LightComponent
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

// =============================================================================
// LightComponent
// =============================================================================

struct LightComponent {
    enum class Type { Directional, Point, Spot };

    Type type = Type::Point;
    DirectX::XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngle = 45.0f;
    float spotInnerAngle = 30.0f;
    bool castShadows = false;
    int shadowMapResolution = 1024;
};
