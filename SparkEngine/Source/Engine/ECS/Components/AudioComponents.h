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
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

// =============================================================================
// AudioSourceComponent
// =============================================================================

/**
 * @brief Attaches a positional or global audio source to an entity.
 *
 * For 3D sources, the AudioUpdateSystem computes per-frame velocity from
 * the position delta (current - previous) to drive XAudio2 Doppler effects.
 */
struct AudioSourceComponent
{
    std::string soundName;                       ///< Sound asset name registered with AudioEngine.
    float volume = 1.0f;                         ///< Playback volume [0, 2]; values > 1 amplify.
    float pitch = 1.0f;                          ///< Playback pitch multiplier (0, 4]; 1 = normal speed.
    float minDistance = 1.0f;                    ///< Distance at which attenuation begins (meters).
    float maxDistance = 50.0f;                   ///< Distance at which the sound is fully attenuated (meters).
    bool is3D = true;                            ///< When true, the sound is spatialized using entity position.
    bool loop = false;                           ///< Restart playback automatically when the clip ends.
    bool playOnAwake = false;                    ///< Begin playing immediately when the entity is created.
    bool isPlaying = false;                      ///< Runtime state: whether the source is currently audible.
    Spark::AudioHandle audioSourceHandle;        ///< Opaque handle to the underlying XAudio2 source voice.
    DirectX::XMFLOAT3 previousPosition{0, 0, 0}; ///< Last frame's position, used to compute velocity for Doppler.

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
