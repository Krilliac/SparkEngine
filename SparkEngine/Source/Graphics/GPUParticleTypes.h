/**
 * @file GPUParticleTypes.h
 * @brief Type definitions for the GPU particle system
 * @author Spark Engine Team
 * @date 2025
 *
 * Enums, structs, and configuration types used by ParticleEmitter
 * and ParticleSystem. Extracted from ParticleSystem.h for modularity.
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <utility>

/**
 * @brief Emitter shape types for particle spawning
 */
enum class EmitterShape
{
    Point,  ///< All particles spawn at a single point
    Sphere, ///< Particles spawn on/within a sphere
    Cone,   ///< Particles spawn within a cone
    Box,    ///< Particles spawn within a box volume
    Circle  ///< Particles spawn on/within a circle (2D)
};

/**
 * @brief Blend modes for particle rendering
 */
enum class ParticleBlendMode
{
    Additive,     ///< Additive blending (fire, sparks, glow)
    AlphaBlend,   ///< Standard alpha blending (smoke, dust)
    Multiply,     ///< Multiplicative blending (shadows, darkening)
    Premultiplied ///< Pre-multiplied alpha blending
};

/**
 * @brief Space in which particles simulate
 */
enum class ParticleSpace
{
    World, ///< Particles simulate in world space (detach from emitter)
    Local  ///< Particles simulate relative to emitter
};

/**
 * @brief Value range for randomized particle properties
 */
struct FloatRange
{
    float min = 0.0f;
    float max = 0.0f;

    FloatRange() = default;
    FloatRange(float value) : min(value), max(value) {}
    FloatRange(float lo, float hi) : min(lo), max(hi) {}

    float Evaluate(float t) const { return min + t * (max - min); }
};

/**
 * @brief Color gradient keyframe
 */
struct ColorKey
{
    float time = 0.0f;             ///< Normalized time [0..1]
    XMFLOAT4 color = {1, 1, 1, 1}; ///< RGBA color at this time
};

/**
 * @brief Individual particle data (SOA-friendly layout)
 */
struct Particle
{
    XMFLOAT3 position;
    XMFLOAT3 velocity;
    XMFLOAT4 color;
    float size;
    float rotation;
    float rotationSpeed;
    float lifetime;
    float maxLifetime;
    float age; ///< Normalized age [0..1]
    bool alive;
};

/**
 * @brief Particle vertex for GPU rendering
 */
struct ParticleVertex
{
    XMFLOAT3 position;
    XMFLOAT4 color;
    float size;
    float rotation;
};

/**
 * @brief Particle emitter configuration
 */
struct ParticleEmitterDesc
{
    std::string name;

    // Emission
    float emissionRate = 10.0f; ///< Particles per second
    int maxParticles = 1000;    ///< Maximum live particles
    int burstCount = 0;         ///< Particles to emit in a burst
    float burstInterval = 0.0f; ///< Time between bursts (0 = one-shot)

    // Shape
    EmitterShape shape = EmitterShape::Point;
    float shapeRadius = 1.0f;          ///< Radius for sphere/cone/circle
    XMFLOAT3 shapeExtents = {1, 1, 1}; ///< Half-extents for box shape
    float coneAngle = 45.0f;           ///< Cone angle in degrees

    // Particle properties
    FloatRange lifetime = {1.0f, 2.0f};
    FloatRange startSpeed = {1.0f, 5.0f};
    FloatRange startSize = {0.1f, 0.5f};
    FloatRange startRotation = {0.0f, 6.28f};
    FloatRange rotationSpeed = {0.0f, 0.0f};

    // Color over lifetime (gradient)
    std::vector<ColorKey> colorOverLife = {{0.0f, {1, 1, 1, 1}}, {1.0f, {1, 1, 1, 0}}};

    // Size over lifetime multiplier curve
    std::vector<std::pair<float, float>> sizeOverLife = {{0.0f, 1.0f}, {1.0f, 0.0f}};

    // Physics
    XMFLOAT3 gravity = {0, -9.81f, 0};
    float gravityMultiplier = 0.0f; ///< 0 = no gravity
    float drag = 0.0f;              ///< Velocity damping

    // Rendering
    ParticleBlendMode blendMode = ParticleBlendMode::Additive;
    ParticleSpace space = ParticleSpace::World;
    std::string texturePath; ///< Optional billboard texture

    // Sub-emitters
    std::string onDeathEmitter;     ///< Emitter to trigger when particle dies
    std::string onCollisionEmitter; ///< Emitter to trigger on collision

    // Flags
    bool loop = true;
    bool playOnAwake = true;
    bool prewarm = false;
    float duration = 0.0f; ///< Total duration (0 = infinite with loop)
};
