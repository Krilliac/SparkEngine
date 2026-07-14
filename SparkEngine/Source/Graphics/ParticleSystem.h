/**
 * @file ParticleSystem.h
 * @brief GPU-friendly particle system for visual effects
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a complete particle emitter and effect system supporting
 * explosions, trails, muzzle flashes, environmental effects, and more.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>

using Microsoft::WRL::ComPtr;

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

/**
 * @brief Particle emitter instance
 */
class ParticleEmitter
{
  public:
    ParticleEmitter(const ParticleEmitterDesc& desc);
    ~ParticleEmitter();

    HRESULT Initialize(ID3D11Device* device);
    void Update(float deltaTime);
    void Render(ID3D11DeviceContext* context, const XMMATRIX& view, const XMMATRIX& projection);

    // Playback control
    void Play();
    void Stop();
    void Pause();
    void Reset();
    void Burst(int count = -1); ///< -1 uses desc.burstCount

    // Transform
    void SetPosition(const XMFLOAT3& pos) { m_position = pos; }
    void SetRotation(const XMFLOAT3& rot) { m_rotation = rot; }
    const XMFLOAT3& GetPosition() const { return m_position; }

    // State
    bool IsPlaying() const { return m_playing; }
    bool IsAlive() const;
    int GetActiveParticleCount() const { return m_activeCount; }
    const ParticleEmitterDesc& GetDesc() const { return m_desc; }

  private:
    void EmitParticle();
    void UpdateParticle(Particle& p, float dt);
    XMFLOAT4 SampleColorGradient(float t) const;
    float SampleSizeCurve(float t) const;
    XMFLOAT3 GetRandomSpawnPosition() const;
    XMFLOAT3 GetRandomSpawnDirection() const;
    void UpdateVertexBuffer();

    ParticleEmitterDesc m_desc;
    std::vector<Particle> m_particles;

    XMFLOAT3 m_position = {0, 0, 0};
    XMFLOAT3 m_rotation = {0, 0, 0};

    bool m_playing = false;
    float m_emissionAccumulator = 0.0f;
    float m_elapsedTime = 0.0f;
    float m_burstTimer = 0.0f;
    int m_activeCount = 0;

    // GPU resources
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    std::vector<ParticleVertex> m_vertexData;
    bool m_bufferDirty = true;
};

/**
 * @brief Particle system manager - manages all emitters and preset effects
 */
class ParticleSystem
{
  public:
    ParticleSystem();
    ~ParticleSystem();

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();
    void Update(float deltaTime);
    void Render(const XMMATRIX& view, const XMMATRIX& projection);

    // Emitter management
    ParticleEmitter* CreateEmitter(const ParticleEmitterDesc& desc);
    ParticleEmitter* GetEmitter(const std::string& name) const;
    void DestroyEmitter(const std::string& name);
    void DestroyAllEmitters();

    // Preset effects (convenience methods)
    ParticleEmitter* SpawnExplosion(const XMFLOAT3& position, float radius = 3.0f);
    ParticleEmitter* SpawnMuzzleFlash(const XMFLOAT3& position, const XMFLOAT3& direction);
    ParticleEmitter* SpawnSparks(const XMFLOAT3& position, const XMFLOAT3& normal, int count = 20);
    ParticleEmitter* SpawnSmoke(const XMFLOAT3& position, float duration = 3.0f);
    ParticleEmitter* SpawnTrail(const XMFLOAT3& startPos);

    // Metrics
    int GetTotalActiveParticles() const;
    int GetEmitterCount() const { return static_cast<int>(m_emitters.size()); }

    // Console integration
    std::string Console_ListEmitters() const;
    std::string Console_GetEmitterInfo(const std::string& name) const;
    void Console_SpawnEffect(const std::string& effectType, float x, float y, float z);

  private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    std::unordered_map<std::string, std::unique_ptr<ParticleEmitter>> m_emitters;
    int m_nextEmitterID = 0;
};
