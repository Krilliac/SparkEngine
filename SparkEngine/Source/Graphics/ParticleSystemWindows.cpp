/**
 * @file ParticleSystemWindows.cpp
 * @brief Windows/D3D11 ParticleSystem manager and console helpers — split from ParticleSystem.cpp.
 *        The ParticleEmitter implementation lives in ParticleSystemWindowsEmitter.cpp.
 */
#include "ParticleSystem.h"
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "../Utils/Validate.h"
#include <algorithm>
#include <sstream>

using namespace DirectX;

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

#endif // SPARK_PLATFORM_WINDOWS
