/**
 * @file AudioComponents.h
 * @brief ECS audio component: AudioSourceComponent
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/OpaqueHandle.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

// =============================================================================
// AudioSourceComponent
// =============================================================================

struct AudioSourceComponent
{
    std::string soundName;
    float volume = 1.0f;
    float pitch = 1.0f;
    float minDistance = 1.0f;
    float maxDistance = 50.0f;
    bool is3D = true;
    bool loop = false;
    bool playOnAwake = false;
    bool isPlaying = false;
    Spark::AudioHandle audioSourceHandle;
    DirectX::XMFLOAT3 previousPosition{0, 0, 0};
};
