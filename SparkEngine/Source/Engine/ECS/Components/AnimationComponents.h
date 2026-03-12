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

struct ParticleEmitterComponent
{
    std::string effectName;
    bool autoPlay = true;
    bool isPlaying = false;
    float emissionRate = 10.0f;
    float lifetime = 1.0f;
    DirectX::XMFLOAT4 startColor{1, 1, 1, 1};
    float startSize = 0.1f;
    float startSpeed = 1.0f;
    Spark::ParticleHandle emitterHandle;

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

struct AnimationController
{
    std::string currentAnimation;
    std::string defaultAnimation;
    float playbackSpeed = 1.0f;
    float currentTime = 0.0f;
    bool playing = true;
    bool loop = true;
    float duration = 0.0f;
    float normalizedTime = 0.0f;
    std::vector<std::string> availableAnimations;
    Spark::AnimationHandle animInstanceHandle;
};
