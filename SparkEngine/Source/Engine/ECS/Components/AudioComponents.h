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
#include "../../../Utils/Assert.h"
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

    /**
     * @brief Validate that audio parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        ASSERT_MSG(volume >= 0.0f && volume <= 2.0f, "AudioSource volume must be in [0, 2]");
        ASSERT_MSG(pitch > 0.0f && pitch <= 4.0f, "AudioSource pitch must be in (0, 4]");
        ASSERT_MSG(minDistance >= 0.0f, "AudioSource minDistance must be non-negative");
        ASSERT_MSG(maxDistance > minDistance, "AudioSource maxDistance must exceed minDistance");
        return volume >= 0.0f && volume <= 2.0f && pitch > 0.0f && pitch <= 4.0f && minDistance >= 0.0f &&
               maxDistance > minDistance;
    }
};
