/**
 * @file AudioComponents.h
 * @brief ECS audio components: AudioSourceComponent, ScriptAudioCues
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
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
    std::string soundName; ///< Sound asset name registered with AudioEngine.
    float volume = 1.0f;   ///< Playback volume [0, 2]; values > 1 amplify.
    float pitch = 1.0f;    ///< Playback pitch multiplier (0, 4]; 1 = normal speed.
    /// Distance at which attenuation begins (meters). Copied to
    /// AudioSource::MinDistance on bind and consumed by the engine's rolloff.
    float minDistance = 1.0f;
    /// Distance at which the sound is fully attenuated (meters). Copied to
    /// AudioSource::MaxDistance on bind and consumed by the engine's rolloff.
    float maxDistance = 50.0f;
    bool is3D = true;                     ///< When true, the sound is spatialized using entity position.
    bool loop = false;                    ///< Restart playback automatically when the clip ends.
    bool playOnAwake = false;             ///< Begin playing immediately when the entity is created.
    bool isPlaying = false;               ///< Runtime state: whether the source is currently audible.
    Spark::AudioHandle audioSourceHandle; ///< Opaque handle to the underlying XAudio2 source voice.
    /// AudioSource::Generation observed when the handle was bound. Pooled voices
    /// are recycled, so a handle alone cannot tell "still mine" from "reused by
    /// another entity" -- always check AudioEngine::IsSourceLive(handle, this).
    uint32_t audioSourceGeneration = 0;
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

// =============================================================================
// ScriptAudioCues
// =============================================================================

/**
 * @brief One-shot sound requests scripts made on an entity since the last audio tick.
 *
 * Written by the script `playSound(EntityID, const string &in)` binding and
 * drained by the next AudioUpdateSystem tick (Audio phase), which starts each
 * cue through AudioEngine and clears the queue. The script path never touches
 * the audio device, so it works headless and on servers.
 *
 * Runtime-only request queue: it is not reflected or serialized. Game thread
 * only (scripts and the Audio phase both run there). The queue is bounded by
 * kMaxPending; a request beyond it is counted in `dropped` rather than grown,
 * so a script calling playSound() every frame without an audio system cannot
 * leak. Draining keeps the vector's capacity, so steady-state requests do not
 * reallocate the queue (each cue still owns its sound-name string).
 *
 * A cue is positioned where its entity's Transform was when the script asked,
 * so a pickup that moves itself away after playing still sounds where it was
 * collected. Cues on an entity destroyed before the Audio phase are discarded
 * with the entity.
 */
struct ScriptAudioCues
{
    /// A single pending playSound() request.
    struct Cue
    {
        std::string soundName;               ///< Sound asset name registered with AudioEngine.
        DirectX::XMFLOAT3 position{0, 0, 0}; ///< Entity position at request time (valid when positional).
        bool positional = false;             ///< True when the entity had a Transform: played as a 3D sound.
    };

    static constexpr std::size_t kMaxPending = 16; ///< Requests held per entity between drains.

    std::vector<Cue> pending; ///< Requests not yet handed to AudioEngine.
    uint32_t requested = 0;   ///< Valid playSound() calls; requested == played + dropped + pending.size().
    uint32_t played = 0;      ///< Cues AudioEngine started.
    uint32_t dropped = 0;     ///< Cues lost to a full queue or refused by AudioEngine (unknown sound, no voice).
};
