/**
 * @file GPUParticleSystem.h
 * @brief GPU-accelerated particle system using D3D11 compute shaders
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages GPU particle emitters that run entirely on the GPU via compute
 * shaders for emit, simulate, and bitonic sort passes. Each emitter owns
 * structured buffers for its particle pool, dead list, alive list (double-
 * buffered), sort keys, and indirect draw arguments.
 *
 * ## Pipeline
 * ```
 * CreateEmitter()
 *     -> allocates particle pool + dead/alive lists + sort keys + indirect args
 *
 * Update(dt)
 *     -> [CS] ParticleEmit.hlsl: pop from dead list, initialize new particles
 *     -> [CS] ParticleSimulate.hlsl: integrate forces, kill expired, compact alive
 *     -> [CS] ParticleBitonicSort.hlsl: sort alive by camera distance
 *     -> update indirect draw args with alive count
 *
 * Render(view, proj)
 *     -> bind alive list SRV + particle pool SRV
 *     -> DrawInstancedIndirect using indirect args buffer
 * ```
 *
 * ## Usage
 * @code
 *   auto& gpuParticles = GPUParticleSystem::GetInstance();
 *   gpuParticles.Initialize(device, context);
 *
 *   ParticleEmitterDesc desc;
 *   desc.name = "fire";
 *   desc.maxParticles = 50000;
 *   desc.emissionRate = 500.0f;
 *   uint32_t id = gpuParticles.CreateEmitter(desc);
 *
 *   // Each frame
 *   gpuParticles.Update(deltaTime);
 *   gpuParticles.Render(view, projection);
 * @endcode
 *
 * @see ParticleSystem, GPUParticleTypes.h, GPUSceneBuffer
 */

#pragma once

#include "../Core/Platform.h"
#include "GPUParticleTypes.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
using Microsoft::WRL::ComPtr;
#endif

/// @brief Maximum particles per emitter (1 million)
static constexpr uint32_t kMaxGPUParticlesPerEmitter = 1'048'576;

/// @brief Default particles per emitter
static constexpr uint32_t kDefaultGPUParticleCapacity = 65'536;

/// @brief Thread group size for compute dispatches (must match HLSL)
static constexpr uint32_t kParticleThreadGroupSize = 256;

/**
 * @brief GPU-side particle data matching the HLSL StructuredBuffer layout.
 *
 * 16-byte aligned for GPU structured buffer compatibility.
 */
struct alignas(16) GPUParticleData
{
    XMFLOAT3 position;
    float age;
    XMFLOAT3 velocity;
    float lifetime;
    XMFLOAT4 color;
    float size;
    float rotation;
    float rotationSpeed;
    uint32_t alive;
};

/**
 * @brief Per-emitter constant buffer data sent to compute shaders.
 */
struct alignas(16) GPUParticleEmitterCB
{
    XMFLOAT3 emitterPosition;
    float deltaTime;
    XMFLOAT3 gravity;
    float drag;
    float emissionRate;
    float shapeRadius;
    float coneAngle;
    uint32_t maxParticles;
    XMFLOAT4 lifetimeRange; ///< x=min, y=max, z=speedMin, w=speedMax
    XMFLOAT4 sizeRange;     ///< x=sizeMin, y=sizeMax, z=rotMin, w=rotMax
    XMFLOAT4 rotSpeedRange; ///< x=min, y=max, z=unused, w=unused
    uint32_t emitCount;     ///< Particles to emit this frame
    uint32_t emitterShape;  ///< Cast from EmitterShape enum
    float gravityMultiplier;
    float totalTime;
};

#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @brief GPU resources for a single particle emitter.
 */
struct GPUEmitterResources
{
    ParticleEmitterDesc desc;
    uint32_t capacity = kDefaultGPUParticleCapacity;

    // Structured buffers
    ComPtr<ID3D11Buffer> particleBuffer;      ///< RW particle pool
    ComPtr<ID3D11Buffer> deadListBuffer;      ///< Append/consume dead indices
    ComPtr<ID3D11Buffer> aliveListBuffer;     ///< Current frame alive indices
    ComPtr<ID3D11Buffer> aliveListSwapBuffer; ///< Previous frame alive indices
    ComPtr<ID3D11Buffer> sortKeysBuffer;      ///< Distance sort keys for blending
    ComPtr<ID3D11Buffer> indirectArgsBuffer;  ///< DrawInstancedIndirect arguments
    ComPtr<ID3D11Buffer> emitterCBBuffer;     ///< Per-emitter constant buffer

    // Views
    ComPtr<ID3D11UnorderedAccessView> particleUAV;
    ComPtr<ID3D11UnorderedAccessView> deadListUAV;
    ComPtr<ID3D11UnorderedAccessView> aliveListUAV;
    ComPtr<ID3D11UnorderedAccessView> aliveListSwapUAV;
    ComPtr<ID3D11UnorderedAccessView> sortKeysUAV;
    ComPtr<ID3D11UnorderedAccessView> indirectArgsUAV;

    ComPtr<ID3D11ShaderResourceView> particleSRV;
    ComPtr<ID3D11ShaderResourceView> aliveListSRV;
    ComPtr<ID3D11ShaderResourceView> sortKeysSRV;

    // State
    XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    float emissionAccumulator = 0.0f;
    float elapsedTime = 0.0f;
    bool playing = true;
    bool initialized = false;
};

#endif // SPARK_PLATFORM_WINDOWS

/**
 * @brief GPU-accelerated particle system manager (singleton).
 *
 * Runs particle emission, simulation, sorting, and indirect draw argument
 * generation entirely on the GPU via D3D11 compute shaders. Supports up
 * to 1M particles per emitter with bitonic sort for correct alpha blending.
 */
class GPUParticleSystem
{
  public:
    /// @brief Get the singleton instance.
    [[deprecated("Use EngineContext::Get()->GetSystem<GPUParticleSystem>() instead")]]
    static GPUParticleSystem& GetInstance()
    {
        static GPUParticleSystem instance;
        return instance;
    }

    GPUParticleSystem(const GPUParticleSystem&) = delete;
    GPUParticleSystem& operator=(const GPUParticleSystem&) = delete;

#ifdef SPARK_PLATFORM_WINDOWS
    /**
     * @brief Initialize the GPU particle system and compile compute shaders.
     * @param device   D3D11 device for resource creation.
     * @param context  D3D11 device context for dispatch.
     * @return S_OK on success, error HRESULT on failure.
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
#else
    /**
     * @brief Stub initialization for non-Windows platforms.
     * @return true (no-op).
     */
    bool Initialize();
#endif

    /// @brief Release all GPU resources and emitters.
    void Shutdown();

    /**
     * @brief Create a new GPU particle emitter.
     * @param desc      Emitter configuration.
     * @param capacity  Maximum particle count (clamped to kMaxGPUParticlesPerEmitter).
     * @return Emitter ID, or UINT32_MAX on failure.
     */
    uint32_t CreateEmitter(const ParticleEmitterDesc& desc, uint32_t capacity = kDefaultGPUParticleCapacity);

    /// @brief Destroy an emitter and release its GPU resources.
    void DestroyEmitter(uint32_t emitterId);

    /// @brief Destroy all emitters.
    void DestroyAllEmitters();

    /**
     * @brief Run the GPU simulation pipeline for all active emitters.
     *
     * Dispatches compute shaders: (1) emit new particles, (2) simulate
     * physics and compact alive list, (3) bitonic sort by distance,
     * (4) update indirect draw arguments.
     *
     * @param deltaTime Frame delta time in seconds.
     */
    void Update(float deltaTime);

    /**
     * @brief Render all active emitters using indirect draw.
     * @param view       Camera view matrix.
     * @param projection Camera projection matrix.
     */
    void Render(const XMMATRIX& view, const XMMATRIX& projection);

    // =========================================================================
    // Emitter control
    // =========================================================================

    void SetEmitterPosition(uint32_t emitterId, const XMFLOAT3& position);
    void SetEmitterPlaying(uint32_t emitterId, bool playing);

    // =========================================================================
    // Queries
    // =========================================================================

    uint32_t GetEmitterCount() const;
    bool IsInitialized() const { return m_initialized; }

    /// @brief Console integration: formatted status string.
    std::string GetStatus() const;

  private:
    GPUParticleSystem() = default;
    ~GPUParticleSystem();

#ifdef SPARK_PLATFORM_WINDOWS
    // Shader compilation
    HRESULT CompileComputeShaders();

    // Buffer creation for a single emitter
    HRESULT CreateEmitterBuffers(GPUEmitterResources& emitter);

    // Per-emitter simulation dispatch
    void DispatchEmitter(GPUEmitterResources& emitter, float deltaTime);

    // Bitonic sort dispatch for an emitter's alive list
    void DispatchBitonicSort(GPUEmitterResources& emitter);

    // Update the emitter constant buffer
    void UpdateEmitterCB(GPUEmitterResources& emitter, float deltaTime);

    // Compute dispatch helper
    void DispatchCS(ID3D11ComputeShader* shader, uint32_t threadGroupCountX);

    // =========================================================================
    // D3D11 resources
    // =========================================================================

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Compute shaders
    ComPtr<ID3D11ComputeShader> m_emitCS;
    ComPtr<ID3D11ComputeShader> m_simulateCS;
    ComPtr<ID3D11ComputeShader> m_bitonicSortCS;
    ComPtr<ID3D11ComputeShader> m_clearAliveCS;

    // Emitter storage
    std::unordered_map<uint32_t, std::unique_ptr<GPUEmitterResources>> m_emitters;
#endif // SPARK_PLATFORM_WINDOWS

    uint32_t m_nextEmitterId = 0;
    bool m_initialized = false;
};
