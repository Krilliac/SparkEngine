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
