/**
 * @file ParticleSystemLinuxEmitter.cpp
 * @brief Linux ParticleEmitter implementation — split from ParticleSystemLinux.cpp,
 *        which keeps the ParticleSystem manager and console helpers.
 */
#include "Core/Platform.h"
#include "ParticleSystem.h"
#include "Utils/MathUtils.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include <cmath>
#include <random>

static thread_local std::mt19937 s_rng(std::random_device{}());

static float RandomFloat(float lo, float hi)
{
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(s_rng);
}

static float RandomFloat01()
{
    return RandomFloat(0.0f, 1.0f);
}

// ============================================================================
// ParticleEmitter (Linux stub)
// ============================================================================

ParticleEmitter::ParticleEmitter(const ParticleEmitterDesc& desc) : m_desc(desc)
{
    m_particles.resize(desc.maxParticles);
    for (auto& p : m_particles)
        p.alive = false;

    m_vertexData.reserve(desc.maxParticles);

    if (desc.playOnAwake)
        m_playing = true;
}

ParticleEmitter::~ParticleEmitter() = default;

HRESULT ParticleEmitter::Initialize(ID3D11Device* /*device*/)
{
    // No GPU resources on Linux
    return S_OK;
}

void ParticleEmitter::Update(float deltaTime)
{
    if (!m_playing)
        return;

    m_elapsedTime += deltaTime;

    if (!m_desc.loop && m_desc.duration > 0.0f && m_elapsedTime >= m_desc.duration)
    {
        m_playing = false;
    }

    if (m_playing)
    {
        m_emissionAccumulator += m_desc.emissionRate * deltaTime;
        while (m_emissionAccumulator >= 1.0f)
        {
            EmitParticle();
            m_emissionAccumulator -= 1.0f;
        }

        if (m_desc.burstInterval > 0.0f)
        {
            m_burstTimer += deltaTime;
            if (m_burstTimer >= m_desc.burstInterval)
            {
                m_burstTimer = 0.0f;
                Burst();
            }
        }
    }

    m_activeCount = 0;
    for (auto& p : m_particles)
    {
        if (!p.alive)
            continue;
        UpdateParticle(p, deltaTime);
        if (p.alive)
            m_activeCount++;
    }

    m_bufferDirty = true;
}

void ParticleEmitter::Render(ID3D11DeviceContext* /*context*/, const XMMATRIX& /*view*/, const XMMATRIX& /*projection*/)
{
    // No-op on Linux - no GPU rendering
}

void ParticleEmitter::Play()
{
    m_playing = true;
}
void ParticleEmitter::Stop()
{
    m_playing = false;
}
void ParticleEmitter::Pause()
{
    m_playing = false;
}

void ParticleEmitter::Reset()
{
    m_playing = m_desc.playOnAwake;
    m_emissionAccumulator = 0.0f;
    m_elapsedTime = 0.0f;
    m_burstTimer = 0.0f;
    m_activeCount = 0;
    for (auto& p : m_particles)
        p.alive = false;
}

void ParticleEmitter::Burst(int count)
{
    int burstCount = (count >= 0) ? count : m_desc.burstCount;
    for (int i = 0; i < burstCount; ++i)
        EmitParticle();
}

bool ParticleEmitter::IsAlive() const
{
    if (m_playing)
        return true;
    return m_activeCount > 0;
}

void ParticleEmitter::EmitParticle()
{
    for (auto& p : m_particles)
    {
        if (p.alive)
            continue;

        float t = RandomFloat01();
        p.alive = true;
        p.position = GetRandomSpawnPosition();
        p.lifetime = m_desc.lifetime.Evaluate(t);
        p.maxLifetime = p.lifetime;
        p.age = 0.0f;
        p.size = m_desc.startSize.Evaluate(RandomFloat01());
        p.rotation = m_desc.startRotation.Evaluate(RandomFloat01());
        p.rotationSpeed = m_desc.rotationSpeed.Evaluate(RandomFloat01());

        XMFLOAT3 dir = GetRandomSpawnDirection();
        float speed = m_desc.startSpeed.Evaluate(RandomFloat01());
        p.velocity = {dir.x * speed, dir.y * speed, dir.z * speed};

        p.color = SampleColorGradient(0.0f);
        return;
    }
}

void ParticleEmitter::UpdateParticle(Particle& p, float dt)
{
    p.lifetime -= dt;
    if (p.lifetime <= 0.0f)
    {
        p.alive = false;
        return;
    }

    p.age = 1.0f - (p.lifetime / p.maxLifetime);

    if (m_desc.gravityMultiplier != 0.0f)
    {
        p.velocity.x += m_desc.gravity.x * m_desc.gravityMultiplier * dt;
        p.velocity.y += m_desc.gravity.y * m_desc.gravityMultiplier * dt;
        p.velocity.z += m_desc.gravity.z * m_desc.gravityMultiplier * dt;
    }

    if (m_desc.drag > 0.0f)
    {
        float damping = 1.0f / (1.0f + m_desc.drag * dt);
        p.velocity.x *= damping;
        p.velocity.y *= damping;
        p.velocity.z *= damping;
    }

    p.position.x += p.velocity.x * dt;
    p.position.y += p.velocity.y * dt;
    p.position.z += p.velocity.z * dt;

    p.rotation += p.rotationSpeed * dt;

    p.color = SampleColorGradient(p.age);
    p.size = m_desc.startSize.Evaluate(0.5f) * SampleSizeCurve(p.age);
}

XMFLOAT4 ParticleEmitter::SampleColorGradient(float t) const
{
    const auto& keys = m_desc.colorOverLife;
    if (keys.empty())
        return {1, 1, 1, 1};
    if (keys.size() == 1)
        return keys[0].color;

    for (size_t i = 0; i + 1 < keys.size(); ++i)
    {
        if (t >= keys[i].time && t <= keys[i + 1].time)
        {
            float timeDelta = keys[i + 1].time - keys[i].time;
            float localT = (std::abs(timeDelta) > 1e-6f) ? (t - keys[i].time) / timeDelta : 0.0f;
            return {keys[i].color.x + localT * (keys[i + 1].color.x - keys[i].color.x),
                    keys[i].color.y + localT * (keys[i + 1].color.y - keys[i].color.y),
                    keys[i].color.z + localT * (keys[i + 1].color.z - keys[i].color.z),
                    keys[i].color.w + localT * (keys[i + 1].color.w - keys[i].color.w)};
        }
    }
    return keys.back().color;
}

float ParticleEmitter::SampleSizeCurve(float t) const
{
    const auto& curve = m_desc.sizeOverLife;
    if (curve.empty())
        return 1.0f;
    if (curve.size() == 1)
        return curve[0].second;

    for (size_t i = 0; i + 1 < curve.size(); ++i)
    {
        if (t >= curve[i].first && t <= curve[i + 1].first)
        {
            float localT = (t - curve[i].first) / (curve[i + 1].first - curve[i].first);
            return curve[i].second + localT * (curve[i + 1].second - curve[i].second);
        }
    }
    return curve.back().second;
}

XMFLOAT3 ParticleEmitter::GetRandomSpawnPosition() const
{
    XMFLOAT3 pos = m_position;

    switch (m_desc.shape)
    {
    case EmitterShape::Point:
        break;
    case EmitterShape::Sphere:
    {
        float theta = RandomFloat(0.0f, 6.28318f);
        float phi = acosf(RandomFloat(-1.0f, 1.0f));
        float r = m_desc.shapeRadius * cbrtf(RandomFloat01());
        pos.x += r * sinf(phi) * cosf(theta);
        pos.y += r * sinf(phi) * sinf(theta);
        pos.z += r * cosf(phi);
        break;
    }
    case EmitterShape::Box:
        pos.x += RandomFloat(-m_desc.shapeExtents.x, m_desc.shapeExtents.x);
        pos.y += RandomFloat(-m_desc.shapeExtents.y, m_desc.shapeExtents.y);
        pos.z += RandomFloat(-m_desc.shapeExtents.z, m_desc.shapeExtents.z);
        break;
    case EmitterShape::Circle:
    {
        float angle = RandomFloat(0.0f, 6.28318f);
        float r = m_desc.shapeRadius * sqrtf(RandomFloat01());
        pos.x += r * cosf(angle);
        pos.z += r * sinf(angle);
        break;
    }
    case EmitterShape::Cone:
    {
        float angle = RandomFloat(0.0f, 6.28318f);
        float coneRad = m_desc.coneAngle * MathUtils::DEG_TO_RAD;
        float r = m_desc.shapeRadius * sqrtf(RandomFloat01());
        float spread = tanf(coneRad * 0.5f) * r;
        pos.x += spread * cosf(angle);
        pos.z += spread * sinf(angle);
        break;
    }
    }

    return pos;
}

XMFLOAT3 ParticleEmitter::GetRandomSpawnDirection() const
{
    switch (m_desc.shape)
    {
    case EmitterShape::Sphere:
    {
        float theta = RandomFloat(0.0f, 6.28318f);
        float phi = acosf(RandomFloat(-1.0f, 1.0f));
        return {sinf(phi) * cosf(theta), sinf(phi) * sinf(theta), cosf(phi)};
    }
    case EmitterShape::Cone:
    {
        float coneRad = m_desc.coneAngle * MathUtils::DEG_TO_RAD;
        float angle = RandomFloat(0.0f, 6.28318f);
        float spread = tanf(coneRad * 0.5f) * RandomFloat01();
        float y = 1.0f / sqrtf(1.0f + spread * spread);
        return {spread * cosf(angle) * y, y, spread * sinf(angle) * y};
    }
    default:
        return {0, 1, 0};
    }
}

void ParticleEmitter::UpdateVertexBuffer()
{
    m_vertexData.clear();
    m_vertexData.reserve(static_cast<size_t>(m_activeCount));
    for (const auto& p : m_particles)
    {
        if (!p.alive)
            continue;
        m_vertexData.push_back({p.position, p.color, p.size, p.rotation});
    }
}

#endif // !SPARK_PLATFORM_WINDOWS
