#include "Core/Platform.h"
#include "Utils/MathUtils.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file ParticleSystem.cpp
 * @brief GPU-accelerated particle emitter and system manager
 *
 * Implements billboard particle emission with configurable rate, burst, duration,
 * and looping. Particles are updated CPU-side (velocity, gravity, lifetime, color
 * interpolation, size scaling) then uploaded to a dynamic vertex buffer for
 * rendering. Thread-local RNG ensures safe parallel emission.
 */
#include "ParticleSystem.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include <algorithm>
#include <random>
#include <cmath>
#include <sstream>

using namespace DirectX;
/// Thread-local Mersenne Twister RNG seeded from hardware entropy.
/// Thread-local avoids contention when multiple emitters update in parallel.
static thread_local std::mt19937 s_rng(std::random_device{}());

/// Returns a uniformly distributed float in [lo, hi].
static float RandomFloat(float lo, float hi)
{
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(s_rng);
}

/// Returns a uniformly distributed float in [0, 1].
static float RandomFloat01()
{
    return RandomFloat(0.0f, 1.0f);
}

// ============================================================================
// ParticleEmitter
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

HRESULT ParticleEmitter::Initialize(ID3D11Device* device)
{
    ASSERT_MSG(device != nullptr, "ParticleEmitter::Initialize device is null");

    // Create dynamic vertex buffer for particle billboards
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(ParticleVertex) * m_desc.maxParticles;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    return device->CreateBuffer(&bd, nullptr, m_vertexBuffer.GetAddressOf());
}

/// Per-frame emitter update: manages lifetime, emission accumulation, burst timing,
/// and per-particle physics (velocity, gravity, color/size interpolation, death).
/// Uses a fractional accumulator for emission rate so sub-frame emission is smooth.
void ParticleEmitter::Update(float deltaTime)
{
    if (!m_playing)
        return;

    m_elapsedTime += deltaTime;

    // Non-looping emitters stop after their duration expires
    if (!m_desc.loop && m_desc.duration > 0.0f && m_elapsedTime >= m_desc.duration)
    {
        m_playing = false;
    }

    // Accumulate fractional particles from emission rate; spawn one per whole unit
    if (m_playing)
    {
        m_emissionAccumulator += m_desc.emissionRate * deltaTime;
        while (m_emissionAccumulator >= 1.0f)
        {
            EmitParticle();
            m_emissionAccumulator -= 1.0f;
        }

        // Periodic burst: spawn burstCount particles at fixed intervals
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

    // Update existing particles
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

void ParticleEmitter::Render(ID3D11DeviceContext* context, const XMMATRIX& view, const XMMATRIX& projection)
{
    if (m_activeCount == 0)
        return;
    if (!m_vertexBuffer)
        return;

    // Update vertex buffer with current particle data
    if (m_bufferDirty)
    {
        UpdateVertexBuffer();

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData)
        {
            memcpy(mapped.pData, m_vertexData.data(), sizeof(ParticleVertex) * m_vertexData.size());
            context->Unmap(m_vertexBuffer.Get(), 0);
        }

        m_bufferDirty = false;
    }

    // Bind vertex buffer and draw
    UINT stride = sizeof(ParticleVertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->Draw(static_cast<UINT>(m_vertexData.size()), 0);
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
    // Find a dead particle slot
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

        // Initial velocity from spawn direction * speed
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

    // Apply gravity
    if (m_desc.gravityMultiplier != 0.0f)
    {
        p.velocity.x += m_desc.gravity.x * m_desc.gravityMultiplier * dt;
        p.velocity.y += m_desc.gravity.y * m_desc.gravityMultiplier * dt;
        p.velocity.z += m_desc.gravity.z * m_desc.gravityMultiplier * dt;
    }

    // Apply drag
    if (m_desc.drag > 0.0f)
    {
        float damping = 1.0f / (1.0f + m_desc.drag * dt);
        p.velocity.x *= damping;
        p.velocity.y *= damping;
        p.velocity.z *= damping;
    }

    // Integrate position
    p.position.x += p.velocity.x * dt;
    p.position.y += p.velocity.y * dt;
    p.position.z += p.velocity.z * dt;

    // Update rotation
    p.rotation += p.rotationSpeed * dt;

    // Sample color and size from curves
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

    // Find the two keys to lerp between
    for (size_t i = 0; i + 1 < keys.size(); ++i)
    {
        if (t >= keys[i].time && t <= keys[i + 1].time)
        {
            float localT = (t - keys[i].time) / (keys[i + 1].time - keys[i].time);
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
        return {0, 1, 0}; // Default upward
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

// ============================================================================
// ParticleSystem
// ============================================================================

ParticleSystem::ParticleSystem() = default;
ParticleSystem::~ParticleSystem()
{
    Shutdown();
}

HRESULT ParticleSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);

    m_device = device;
    m_context = context;
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "ParticleSystem initialized");
    return S_OK;
}

void ParticleSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "ParticleSystem shutting down (%zu emitters)", m_emitters.size());
    m_emitters.clear();
    m_device = nullptr;
    m_context = nullptr;
}

void ParticleSystem::Update(float deltaTime)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_WARN_IF(Spark::LogCategory::Graphics, deltaTime < 0.0f,
                  "ParticleSystem::Update called with negative deltaTime");
    // Update all emitters and remove dead one-shot emitters
    for (auto it = m_emitters.begin(); it != m_emitters.end();)
    {
        it->second->Update(deltaTime);
        if (!it->second->IsAlive() && !it->second->GetDesc().loop)
            it = m_emitters.erase(it);
        else
            ++it;
    }
}

void ParticleSystem::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    for (auto& pair : m_emitters)
    {
        if (pair.second->IsAlive())
            pair.second->Render(m_context, view, projection);
    }
}

ParticleEmitter* ParticleSystem::CreateEmitter(const ParticleEmitterDesc& desc)
{
    auto emitter = std::make_unique<ParticleEmitter>(desc);
    if (m_device)
        emitter->Initialize(m_device);

    std::string name = desc.name.empty() ? "emitter_" + std::to_string(m_nextEmitterID++) : desc.name;

    auto* ptr = emitter.get();
    m_emitters[name] = std::move(emitter);
    return ptr;
}

ParticleEmitter* ParticleSystem::GetEmitter(const std::string& name) const
{
    auto it = m_emitters.find(name);
    return it != m_emitters.end() ? it->second.get() : nullptr;
}

void ParticleSystem::DestroyEmitter(const std::string& name)
{
    m_emitters.erase(name);
}

void ParticleSystem::DestroyAllEmitters()
{
    m_emitters.clear();
}

// ============================================================================
// Preset Effects
// ============================================================================

ParticleEmitter* ParticleSystem::SpawnExplosion(const XMFLOAT3& position, float radius)
{
    ParticleEmitterDesc desc;
    desc.name = "explosion_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Sphere;
    desc.shapeRadius = radius * 0.2f;
    desc.emissionRate = 0.0f;
    desc.burstCount = 80;
    desc.maxParticles = 200;
    desc.lifetime = {0.3f, 1.2f};
    desc.startSpeed = {radius * 2.0f, radius * 5.0f};
    desc.startSize = {0.2f, 0.8f};
    desc.gravityMultiplier = 0.3f;
    desc.drag = 2.0f;
    desc.loop = false;
    desc.duration = 1.5f;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {
        {0.0f, {1.0f, 0.9f, 0.3f, 1.0f}}, // Bright yellow-orange
        {0.3f, {1.0f, 0.4f, 0.1f, 0.9f}}, // Orange-red
        {0.7f, {0.5f, 0.1f, 0.0f, 0.5f}}, // Dark red
        {1.0f, {0.2f, 0.2f, 0.2f, 0.0f}}  // Smoke fade
    };
    desc.sizeOverLife = {{0.0f, 0.5f}, {0.2f, 1.5f}, {1.0f, 0.3f}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    emitter->Burst();
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnMuzzleFlash(const XMFLOAT3& position, const XMFLOAT3& direction)
{
    ParticleEmitterDesc desc;
    desc.name = "muzzle_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Cone;
    desc.coneAngle = 15.0f;
    desc.shapeRadius = 0.1f;
    desc.emissionRate = 0.0f;
    desc.burstCount = 15;
    desc.maxParticles = 30;
    desc.lifetime = {0.03f, 0.08f};
    desc.startSpeed = {10.0f, 25.0f};
    desc.startSize = {0.05f, 0.15f};
    desc.loop = false;
    desc.duration = 0.1f;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {{0.0f, {1.0f, 1.0f, 0.8f, 1.0f}}, {1.0f, {1.0f, 0.6f, 0.1f, 0.0f}}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    emitter->Burst();
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnSparks(const XMFLOAT3& position, const XMFLOAT3& normal, int count)
{
    ParticleEmitterDesc desc;
    desc.name = "sparks_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Cone;
    desc.coneAngle = 60.0f;
    desc.emissionRate = 0.0f;
    desc.burstCount = count;
    desc.maxParticles = count * 2;
    desc.lifetime = {0.2f, 0.6f};
    desc.startSpeed = {5.0f, 15.0f};
    desc.startSize = {0.02f, 0.06f};
    desc.gravityMultiplier = 1.0f;
    desc.loop = false;
    desc.duration = 0.8f;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {
        {0.0f, {1.0f, 0.9f, 0.5f, 1.0f}}, {0.5f, {1.0f, 0.5f, 0.1f, 0.8f}}, {1.0f, {0.5f, 0.1f, 0.0f, 0.0f}}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    emitter->Burst();
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnSmoke(const XMFLOAT3& position, float duration)
{
    ParticleEmitterDesc desc;
    desc.name = "smoke_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Circle;
    desc.shapeRadius = 0.3f;
    desc.emissionRate = 15.0f;
    desc.maxParticles = 200;
    desc.lifetime = {1.0f, 3.0f};
    desc.startSpeed = {0.5f, 2.0f};
    desc.startSize = {0.3f, 0.8f};
    desc.gravityMultiplier = -0.1f; // Rises slightly
    desc.drag = 1.0f;
    desc.loop = false;
    desc.duration = duration;
    desc.blendMode = ParticleBlendMode::AlphaBlend;
    desc.colorOverLife = {{0.0f, {0.5f, 0.5f, 0.5f, 0.0f}},
                          {0.1f, {0.5f, 0.5f, 0.5f, 0.4f}},
                          {0.8f, {0.3f, 0.3f, 0.3f, 0.2f}},
                          {1.0f, {0.2f, 0.2f, 0.2f, 0.0f}}};
    desc.sizeOverLife = {{0.0f, 0.5f}, {0.5f, 1.2f}, {1.0f, 2.0f}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnTrail(const XMFLOAT3& startPos)
{
    ParticleEmitterDesc desc;
    desc.name = "trail_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Point;
    desc.emissionRate = 60.0f;
    desc.maxParticles = 200;
    desc.lifetime = {0.2f, 0.5f};
    desc.startSpeed = {0.0f, 0.5f};
    desc.startSize = {0.05f, 0.15f};
    desc.drag = 3.0f;
    desc.loop = true;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {
        {0.0f, {1.0f, 0.8f, 0.3f, 0.8f}}, {0.5f, {0.8f, 0.3f, 0.1f, 0.4f}}, {1.0f, {0.3f, 0.1f, 0.1f, 0.0f}}};
    desc.sizeOverLife = {{0.0f, 1.0f}, {1.0f, 0.2f}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(startPos);
    return emitter;
}

// ============================================================================
// Console Integration
// ============================================================================

int ParticleSystem::GetTotalActiveParticles() const
{
    int total = 0;
    for (const auto& pair : m_emitters)
        total += pair.second->GetActiveParticleCount();
    return total;
}

std::string ParticleSystem::Console_ListEmitters() const
{
    std::ostringstream ss;
    ss << "=== Particle Emitters (" << m_emitters.size() << ") ===\n";
    for (const auto& pair : m_emitters)
    {
        ss << "  " << pair.first << " | Particles: " << pair.second->GetActiveParticleCount() << "/"
           << pair.second->GetDesc().maxParticles << " | " << (pair.second->IsPlaying() ? "Playing" : "Stopped")
           << "\n";
    }
    ss << "Total active particles: " << GetTotalActiveParticles() << "\n";
    return ss.str();
}

std::string ParticleSystem::Console_GetEmitterInfo(const std::string& name) const
{
    auto* emitter = GetEmitter(name);
    if (!emitter)
        return "Emitter not found: " + name;

    const auto& desc = emitter->GetDesc();
    std::ostringstream ss;
    ss << "=== Emitter: " << name << " ===\n"
       << "  Active: " << emitter->GetActiveParticleCount() << "/" << desc.maxParticles << "\n"
       << "  Rate: " << desc.emissionRate << " particles/sec\n"
       << "  Lifetime: " << desc.lifetime.min << " - " << desc.lifetime.max << "s\n"
       << "  Speed: " << desc.startSpeed.min << " - " << desc.startSpeed.max << "\n"
       << "  Loop: " << (desc.loop ? "Yes" : "No") << "\n"
       << "  Playing: " << (emitter->IsPlaying() ? "Yes" : "No") << "\n";
    return ss.str();
}

void ParticleSystem::Console_SpawnEffect(const std::string& effectType, float x, float y, float z)
{
    XMFLOAT3 pos = {x, y, z};

    if (effectType == "explosion")
        SpawnExplosion(pos);
    else if (effectType == "sparks")
        SpawnSparks(pos, {0, 1, 0});
    else if (effectType == "smoke")
        SpawnSmoke(pos);
    else if (effectType == "muzzle")
        SpawnMuzzleFlash(pos, {0, 0, 1});
    else if (effectType == "trail")
        SpawnTrail(pos);
}

#else // !SPARK_PLATFORM_WINDOWS

#include "ParticleSystem.h"
#include "../Utils/Validate.h"
#include <algorithm>
#include <random>
#include <cmath>
#include <sstream>

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
            float localT = (t - keys[i].time) / (keys[i + 1].time - keys[i].time);
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

// ============================================================================
// ParticleSystem (Linux stub)
// ============================================================================

ParticleSystem::ParticleSystem() = default;
ParticleSystem::~ParticleSystem()
{
    Shutdown();
}

HRESULT ParticleSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device;
    m_context = context;
    return S_OK;
}

void ParticleSystem::Shutdown()
{
    m_emitters.clear();
    m_device = nullptr;
    m_context = nullptr;
}

void ParticleSystem::Update(float deltaTime)
{
    for (auto it = m_emitters.begin(); it != m_emitters.end();)
    {
        it->second->Update(deltaTime);
        if (!it->second->IsAlive() && !it->second->GetDesc().loop)
            it = m_emitters.erase(it);
        else
            ++it;
    }
}

void ParticleSystem::Render(const XMMATRIX& /*view*/, const XMMATRIX& /*projection*/)
{
    // No-op on Linux
}

ParticleEmitter* ParticleSystem::CreateEmitter(const ParticleEmitterDesc& desc)
{
    auto emitter = std::make_unique<ParticleEmitter>(desc);
    emitter->Initialize(nullptr);

    std::string name = desc.name.empty() ? "emitter_" + std::to_string(m_nextEmitterID++) : desc.name;

    auto* ptr = emitter.get();
    m_emitters[name] = std::move(emitter);
    return ptr;
}

ParticleEmitter* ParticleSystem::GetEmitter(const std::string& name) const
{
    auto it = m_emitters.find(name);
    return it != m_emitters.end() ? it->second.get() : nullptr;
}

void ParticleSystem::DestroyEmitter(const std::string& name)
{
    m_emitters.erase(name);
}

void ParticleSystem::DestroyAllEmitters()
{
    m_emitters.clear();
}

ParticleEmitter* ParticleSystem::SpawnExplosion(const XMFLOAT3& position, float radius)
{
    ParticleEmitterDesc desc;
    desc.name = "explosion_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Sphere;
    desc.shapeRadius = radius * 0.2f;
    desc.emissionRate = 0.0f;
    desc.burstCount = 80;
    desc.maxParticles = 200;
    desc.lifetime = {0.3f, 1.2f};
    desc.startSpeed = {radius * 2.0f, radius * 5.0f};
    desc.startSize = {0.2f, 0.8f};
    desc.gravityMultiplier = 0.3f;
    desc.drag = 2.0f;
    desc.loop = false;
    desc.duration = 1.5f;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {{0.0f, {1.0f, 0.9f, 0.3f, 1.0f}},
                          {0.3f, {1.0f, 0.4f, 0.1f, 0.9f}},
                          {0.7f, {0.5f, 0.1f, 0.0f, 0.5f}},
                          {1.0f, {0.2f, 0.2f, 0.2f, 0.0f}}};
    desc.sizeOverLife = {{0.0f, 0.5f}, {0.2f, 1.5f}, {1.0f, 0.3f}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    emitter->Burst();
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnMuzzleFlash(const XMFLOAT3& position, const XMFLOAT3& direction)
{
    ParticleEmitterDesc desc;
    desc.name = "muzzle_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Cone;
    desc.coneAngle = 15.0f;
    desc.shapeRadius = 0.1f;
    desc.emissionRate = 0.0f;
    desc.burstCount = 15;
    desc.maxParticles = 30;
    desc.lifetime = {0.03f, 0.08f};
    desc.startSpeed = {10.0f, 25.0f};
    desc.startSize = {0.05f, 0.15f};
    desc.loop = false;
    desc.duration = 0.1f;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {{0.0f, {1.0f, 1.0f, 0.8f, 1.0f}}, {1.0f, {1.0f, 0.6f, 0.1f, 0.0f}}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    emitter->Burst();
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnSparks(const XMFLOAT3& position, const XMFLOAT3& normal, int count)
{
    ParticleEmitterDesc desc;
    desc.name = "sparks_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Cone;
    desc.coneAngle = 60.0f;
    desc.emissionRate = 0.0f;
    desc.burstCount = count;
    desc.maxParticles = count * 2;
    desc.lifetime = {0.2f, 0.6f};
    desc.startSpeed = {5.0f, 15.0f};
    desc.startSize = {0.02f, 0.06f};
    desc.gravityMultiplier = 1.0f;
    desc.loop = false;
    desc.duration = 0.8f;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {
        {0.0f, {1.0f, 0.9f, 0.5f, 1.0f}}, {0.5f, {1.0f, 0.5f, 0.1f, 0.8f}}, {1.0f, {0.5f, 0.1f, 0.0f, 0.0f}}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    emitter->Burst();
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnSmoke(const XMFLOAT3& position, float duration)
{
    ParticleEmitterDesc desc;
    desc.name = "smoke_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Circle;
    desc.shapeRadius = 0.3f;
    desc.emissionRate = 15.0f;
    desc.maxParticles = 200;
    desc.lifetime = {1.0f, 3.0f};
    desc.startSpeed = {0.5f, 2.0f};
    desc.startSize = {0.3f, 0.8f};
    desc.gravityMultiplier = -0.1f;
    desc.drag = 1.0f;
    desc.loop = false;
    desc.duration = duration;
    desc.blendMode = ParticleBlendMode::AlphaBlend;
    desc.colorOverLife = {{0.0f, {0.5f, 0.5f, 0.5f, 0.0f}},
                          {0.1f, {0.5f, 0.5f, 0.5f, 0.4f}},
                          {0.8f, {0.3f, 0.3f, 0.3f, 0.2f}},
                          {1.0f, {0.2f, 0.2f, 0.2f, 0.0f}}};
    desc.sizeOverLife = {{0.0f, 0.5f}, {0.5f, 1.2f}, {1.0f, 2.0f}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(position);
    return emitter;
}

ParticleEmitter* ParticleSystem::SpawnTrail(const XMFLOAT3& startPos)
{
    ParticleEmitterDesc desc;
    desc.name = "trail_" + std::to_string(m_nextEmitterID);
    desc.shape = EmitterShape::Point;
    desc.emissionRate = 60.0f;
    desc.maxParticles = 200;
    desc.lifetime = {0.2f, 0.5f};
    desc.startSpeed = {0.0f, 0.5f};
    desc.startSize = {0.05f, 0.15f};
    desc.drag = 3.0f;
    desc.loop = true;
    desc.blendMode = ParticleBlendMode::Additive;
    desc.colorOverLife = {
        {0.0f, {1.0f, 0.8f, 0.3f, 0.8f}}, {0.5f, {0.8f, 0.3f, 0.1f, 0.4f}}, {1.0f, {0.3f, 0.1f, 0.1f, 0.0f}}};
    desc.sizeOverLife = {{0.0f, 1.0f}, {1.0f, 0.2f}};

    auto* emitter = CreateEmitter(desc);
    emitter->SetPosition(startPos);
    return emitter;
}

int ParticleSystem::GetTotalActiveParticles() const
{
    int total = 0;
    for (const auto& pair : m_emitters)
        total += pair.second->GetActiveParticleCount();
    return total;
}

std::string ParticleSystem::Console_ListEmitters() const
{
    std::ostringstream ss;
    ss << "=== Particle Emitters (" << m_emitters.size() << ") ===\n";
    for (const auto& pair : m_emitters)
    {
        ss << "  " << pair.first << " | Particles: " << pair.second->GetActiveParticleCount() << "/"
           << pair.second->GetDesc().maxParticles << " | " << (pair.second->IsPlaying() ? "Playing" : "Stopped")
           << "\n";
    }
    ss << "Total active particles: " << GetTotalActiveParticles() << "\n";
    return ss.str();
}

std::string ParticleSystem::Console_GetEmitterInfo(const std::string& name) const
{
    auto* emitter = GetEmitter(name);
    if (!emitter)
        return "Emitter not found: " + name;

    const auto& desc = emitter->GetDesc();
    std::ostringstream ss;
    ss << "=== Emitter: " << name << " ===\n"
       << "  Active: " << emitter->GetActiveParticleCount() << "/" << desc.maxParticles << "\n"
       << "  Rate: " << desc.emissionRate << " particles/sec\n"
       << "  Lifetime: " << desc.lifetime.min << " - " << desc.lifetime.max << "s\n"
       << "  Speed: " << desc.startSpeed.min << " - " << desc.startSpeed.max << "\n"
       << "  Loop: " << (desc.loop ? "Yes" : "No") << "\n"
       << "  Playing: " << (emitter->IsPlaying() ? "Yes" : "No") << "\n";
    return ss.str();
}

void ParticleSystem::Console_SpawnEffect(const std::string& effectType, float x, float y, float z)
{
    XMFLOAT3 pos = {x, y, z};

    if (effectType == "explosion")
        SpawnExplosion(pos);
    else if (effectType == "sparks")
        SpawnSparks(pos, {0, 1, 0});
    else if (effectType == "smoke")
        SpawnSmoke(pos);
    else if (effectType == "muzzle")
        SpawnMuzzleFlash(pos, {0, 0, 1});
    else if (effectType == "trail")
        SpawnTrail(pos);
}

#endif // SPARK_PLATFORM_WINDOWS
