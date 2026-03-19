/**
 * @file AnimationComponents.h
 * @brief ECS animation components: AnimationController, ParticleEmitterComponent
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
#include <vector>

// =============================================================================
// ParticleEmitterComponent
// =============================================================================

/**
 * @brief Spawns particles from the entity's position using a named effect preset.
 *
 * The ParticleUpdateSystem manages emitter lifetime. The actual particle
 * simulation runs on the GPU via the underlying particle system handle.
 */
struct ParticleEmitterComponent
{
    std::string effectName;                   ///< Name of the particle effect preset (e.g. "fire", "sparks").
    bool autoPlay = true;                     ///< Start emitting automatically on entity creation.
    bool isPlaying = false;                   ///< Runtime state: whether the emitter is actively spawning.
    float emissionRate = 10.0f;               ///< Particles spawned per second.
    float lifetime = 1.0f;                    ///< Lifetime of each particle (seconds).
    DirectX::XMFLOAT4 startColor{1, 1, 1, 1}; ///< Initial particle color (RGBA).
    float startSize = 0.1f;                   ///< Initial particle size (world units).
    float startSpeed = 1.0f;                  ///< Initial emission velocity (units/second).
    Spark::ParticleHandle emitterHandle;      ///< Opaque handle to the GPU-side particle system.

    /**
     * @brief Validate that particle parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        ASSERT_MSG(emissionRate >= 0.0f, "Particle emissionRate must be non-negative");
        ASSERT_MSG(lifetime > 0.0f, "Particle lifetime must be positive");
        ASSERT_MSG(startSize > 0.0f, "Particle startSize must be positive");
        return emissionRate >= 0.0f && lifetime > 0.0f && startSize > 0.0f;
    }
};

// =============================================================================
// AnimationController
// =============================================================================

/**
 * @brief Controls skeletal animation playback on an entity.
 *
 * The AnimationUpdateSystem advances `currentTime` each frame, handles
 * looping/completion, and computes `normalizedTime` for blend weights
 * and gameplay synchronization (e.g. attack hit windows).
 */
struct AnimationController
{
    std::string currentAnimation;                 ///< Name of the currently playing animation clip.
    std::string defaultAnimation;                 ///< Fallback animation when no other is active (e.g. "idle").
    float playbackSpeed = 1.0f;                   ///< Speed multiplier (negative = reverse playback).
    float currentTime = 0.0f;                     ///< Elapsed time within the current clip (seconds).
    bool playing = true;                          ///< Whether the animation is actively advancing.
    bool loop = true;                             ///< Restart from the beginning when the clip ends.
    float duration = 0.0f;                        ///< Total clip duration (seconds); 0 = not yet loaded.
    float normalizedTime = 0.0f;                  ///< Progress through the clip [0, 1]; useful for gameplay sync.
    std::vector<std::string> availableAnimations; ///< List of animation clip names this entity can play.
    Spark::AnimationHandle animInstanceHandle;    ///< Opaque handle to the runtime AnimationInstance.
};
