/**
 * @file GPUParticleSystem.cpp
 * @brief Platform-independent GPUParticleSystem code (destructor only)
 *
 * All platform-specific methods have been split into:
 *   - GPUParticleSystemWindows.cpp — D3D11 compute shader pipeline
 *   - GPUParticleSystemLinux.cpp   — No-op stubs for headless/NullRHI
 */

#include "GPUParticleSystem.h"

GPUParticleSystem::~GPUParticleSystem()
{
    Shutdown();
}
