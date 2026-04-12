/**
 * @file GPUParticleSystemLinux.cpp
 * @brief Linux GPU particle system stubs — no-op implementations for headless/NullRHI
 *
 * Provides stub implementations of all GPUParticleSystem methods so the engine
 * compiles and links on Linux without D3D11. The Windows counterpart with the
 * real compute shader pipeline lives in GPUParticleSystemWindows.cpp.
 */

#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GPUParticleSystem.h"
#include "Utils/LogMacros.h"
#include <algorithm>

// ============================================================================
// Lifecycle
// ============================================================================

bool GPUParticleSystem::Initialize()
{
    m_initialized = true;
    return true;
}

void GPUParticleSystem::Shutdown()
{
    if (!m_initialized)
        return;
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUParticleSystem shutting down");
    m_nextEmitterId = 0;
    m_initialized = false;
}

// ============================================================================
// Emitter creation / destruction
// ============================================================================

uint32_t GPUParticleSystem::CreateEmitter(const ParticleEmitterDesc& desc, uint32_t capacity)
{
    if (!m_initialized)
        return UINT32_MAX;
    (void)desc;
    (void)capacity;
    return m_nextEmitterId++;
}

void GPUParticleSystem::DestroyEmitter(uint32_t emitterId)
{
    (void)emitterId;
}

void GPUParticleSystem::DestroyAllEmitters() {}

// ============================================================================
// Emitter control
// ============================================================================

void GPUParticleSystem::SetEmitterPosition(uint32_t emitterId, const XMFLOAT3& position)
{
    (void)emitterId;
    (void)position;
}

void GPUParticleSystem::SetEmitterPlaying(uint32_t emitterId, bool playing)
{
    (void)emitterId;
    (void)playing;
}

// ============================================================================
// Update / Render
// ============================================================================

void GPUParticleSystem::Update(float deltaTime)
{
    (void)deltaTime;
}

void GPUParticleSystem::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    (void)view;
    (void)projection;
}

// ============================================================================
// Queries
// ============================================================================

uint32_t GPUParticleSystem::GetEmitterCount() const
{
    return 0;
}

std::string GPUParticleSystem::GetStatus() const
{
    if (!m_initialized)
        return "GPU Particle System: Not initialized\n";

    std::string status = "GPU Particle System:\n";
    status += "  Platform: non-Windows (stub)\n";
    return status;
}

#endif // !SPARK_PLATFORM_WINDOWS
