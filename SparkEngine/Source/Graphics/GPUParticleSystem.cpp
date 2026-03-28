/**
 * @file GPUParticleSystem.cpp
 * @brief Implementation of GPU-accelerated particle system using D3D11 compute shaders
 * @author Spark Engine Team
 * @date 2026
 */

#include "GPUParticleSystem.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#include <cstring>
#endif

#include <algorithm>
#include <format>

#ifdef SPARK_PLATFORM_WINDOWS

static const wchar_t* kEmitShaderPath = L"Shaders/HLSL/Compute/ParticleEmit.hlsl";
static const wchar_t* kSimulateShaderPath = L"Shaders/HLSL/Compute/ParticleSimulate.hlsl";
static const wchar_t* kBitonicSortShaderPath = L"Shaders/HLSL/Compute/ParticleBitonicSort.hlsl";
static const char* kCSEntryPoint = "CSMain";
static const char* kCSShaderModel = "cs_5_0";

static uint32_t NextPowerOfTwo(uint32_t v)
{
    if (v == 0)
        return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

static HRESULT CompileCSFromFile(const wchar_t* path, ID3D11Device* device,
                                 Microsoft::WRL::ComPtr<ID3D11ComputeShader>& outShader)
{
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, kCSEntryPoint, kCSShaderModel,
                                    compileFlags, 0, shaderBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr))
        return hr;
    return device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                       outShader.GetAddressOf());
}

/// Helper: create a structured buffer with optional SRV and UAV
static HRESULT CreateStructuredBuffer(ID3D11Device* device, uint32_t elementCount, uint32_t stride,
                                      const void* initData, UINT bindFlags, UINT uavFlags,
                                      ComPtr<ID3D11Buffer>& outBuffer, ComPtr<ID3D11UnorderedAccessView>& outUAV,
                                      ComPtr<ID3D11ShaderResourceView>* outSRV = nullptr)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = elementCount * stride;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;

    D3D11_SUBRESOURCE_DATA initSub = {};
    initSub.pSysMem = initData;
    HRESULT hr = device->CreateBuffer(&desc, initData ? &initSub : nullptr, outBuffer.GetAddressOf());
    if (FAILED(hr))
        return hr;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = elementCount;
    uavDesc.Buffer.Flags = uavFlags;
    hr = device->CreateUnorderedAccessView(outBuffer.Get(), &uavDesc, outUAV.GetAddressOf());
    if (FAILED(hr))
        return hr;

    if (outSRV)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = elementCount;
        hr = device->CreateShaderResourceView(outBuffer.Get(), &srvDesc, outSRV->GetAddressOf());
    }
    return hr;
}

#endif // SPARK_PLATFORM_WINDOWS

GPUParticleSystem::~GPUParticleSystem()
{
    Shutdown();
}

#ifdef SPARK_PLATFORM_WINDOWS

HRESULT GPUParticleSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (m_initialized)
        return S_OK;
    if (!device || !context)
        return E_INVALIDARG;
    m_device = device;
    m_context = context;
    HRESULT hr = CompileComputeShaders();
    if (FAILED(hr))
        return hr;
    m_nextEmitterId = 1;
    m_initialized = true;
    return S_OK;
}

HRESULT GPUParticleSystem::CompileComputeShaders()
{
    HRESULT hr = CompileCSFromFile(kEmitShaderPath, m_device, m_emitCS);
    if (FAILED(hr))
        return hr;
    hr = CompileCSFromFile(kSimulateShaderPath, m_device, m_simulateCS);
    if (FAILED(hr))
        return hr;
    hr = CompileCSFromFile(kBitonicSortShaderPath, m_device, m_bitonicSortCS);
    if (FAILED(hr))
        return hr;
    m_clearAliveCS = nullptr;
    return S_OK;
}

#else

bool GPUParticleSystem::Initialize()
{
    m_initialized = true;
    return true;
}

#endif // SPARK_PLATFORM_WINDOWS

void GPUParticleSystem::Shutdown()
{
    if (!m_initialized)
        return;
#ifdef SPARK_PLATFORM_WINDOWS
    m_emitters.clear();
    m_emitCS.Reset();
    m_simulateCS.Reset();
    m_bitonicSortCS.Reset();
    m_clearAliveCS.Reset();
    m_device = nullptr;
    m_context = nullptr;
#endif
    m_nextEmitterId = 0;
    m_initialized = false;
}

// =============================================================================
// Emitter creation / destruction
// =============================================================================

uint32_t GPUParticleSystem::CreateEmitter(const ParticleEmitterDesc& desc, uint32_t capacity)
{
    if (!m_initialized)
        return UINT32_MAX;

#ifdef SPARK_PLATFORM_WINDOWS
    // Clamp and round up to power of two (required for bitonic sort)
    capacity = std::min(capacity, kMaxGPUParticlesPerEmitter);
    capacity = std::max(capacity, 256u);
    capacity = NextPowerOfTwo(capacity);

    auto emitter = std::make_unique<GPUEmitterResources>();
    emitter->desc = desc;
    emitter->capacity = capacity;
    emitter->playing = desc.playOnAwake;

    HRESULT hr = CreateEmitterBuffers(*emitter);
    if (FAILED(hr))
        return UINT32_MAX;

    uint32_t id = m_nextEmitterId++;
    m_emitters[id] = std::move(emitter);
    return id;
#else
    (void)desc;
    (void)capacity;
    return m_nextEmitterId++;
#endif
}

void GPUParticleSystem::DestroyEmitter(uint32_t emitterId)
{
#ifdef SPARK_PLATFORM_WINDOWS
    m_emitters.erase(emitterId);
#else
    (void)emitterId;
#endif
}

void GPUParticleSystem::DestroyAllEmitters()
{
#ifdef SPARK_PLATFORM_WINDOWS
    m_emitters.clear();
#endif
}

// =============================================================================
// Buffer creation
// =============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

HRESULT GPUParticleSystem::CreateEmitterBuffers(GPUEmitterResources& emitter)
{
    const uint32_t cap = emitter.capacity;
    const UINT rwFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr;

    // Particle pool (RW structured buffer with SRV)
    hr = CreateStructuredBuffer(m_device, cap, sizeof(GPUParticleData), nullptr, rwFlags, 0, emitter.particleBuffer,
                                emitter.particleUAV, &emitter.particleSRV);
    if (FAILED(hr))
        return hr;

    // Dead list (append/consume, initialized with sequential indices)
    std::vector<uint32_t> deadIndices(cap);
    for (uint32_t i = 0; i < cap; ++i)
        deadIndices[i] = i;
    hr = CreateStructuredBuffer(m_device, cap, sizeof(uint32_t), deadIndices.data(), D3D11_BIND_UNORDERED_ACCESS,
                                D3D11_BUFFER_UAV_FLAG_APPEND, emitter.deadListBuffer, emitter.deadListUAV);
    if (FAILED(hr))
        return hr;

    // Alive list (current frame, with SRV for rendering)
    hr = CreateStructuredBuffer(m_device, cap, sizeof(uint32_t), nullptr, rwFlags, D3D11_BUFFER_UAV_FLAG_APPEND,
                                emitter.aliveListBuffer, emitter.aliveListUAV, &emitter.aliveListSRV);
    if (FAILED(hr))
        return hr;

    // Alive list (swap target)
    hr = CreateStructuredBuffer(m_device, cap, sizeof(uint32_t), nullptr, rwFlags, D3D11_BUFFER_UAV_FLAG_APPEND,
                                emitter.aliveListSwapBuffer, emitter.aliveListSwapUAV);
    if (FAILED(hr))
        return hr;

    // Sort keys (key + index pairs)
    hr = CreateStructuredBuffer(m_device, cap, sizeof(uint32_t) * 2, nullptr, rwFlags, 0, emitter.sortKeysBuffer,
                                emitter.sortKeysUAV, &emitter.sortKeysSRV);
    if (FAILED(hr))
        return hr;

    // Indirect draw arguments (non-structured, uses DRAWINDIRECT_ARGS misc flag)
    {
        uint32_t initArgs[4] = {0, 1, 0, 0};
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(initArgs);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = initArgs;
        hr = m_device->CreateBuffer(&desc, &initData, emitter.indirectArgsBuffer.GetAddressOf());
        if (FAILED(hr))
            return hr;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 4;
        hr = m_device->CreateUnorderedAccessView(emitter.indirectArgsBuffer.Get(), &uavDesc,
                                                 emitter.indirectArgsUAV.GetAddressOf());
        if (FAILED(hr))
            return hr;
    }

    // Per-emitter constant buffer
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(GPUParticleEmitterCB);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&desc, nullptr, emitter.emitterCBBuffer.GetAddressOf());
        if (FAILED(hr))
            return hr;
    }

    emitter.initialized = true;
    return S_OK;
}

#endif // SPARK_PLATFORM_WINDOWS

// =============================================================================
// Emitter control
// =============================================================================

void GPUParticleSystem::SetEmitterPosition(uint32_t emitterId, const XMFLOAT3& position)
{
#ifdef SPARK_PLATFORM_WINDOWS
    auto it = m_emitters.find(emitterId);
    if (it != m_emitters.end())
        it->second->position = position;
#else
    (void)emitterId;
    (void)position;
#endif
}

void GPUParticleSystem::SetEmitterPlaying(uint32_t emitterId, bool playing)
{
#ifdef SPARK_PLATFORM_WINDOWS
    auto it = m_emitters.find(emitterId);
    if (it != m_emitters.end())
        it->second->playing = playing;
#else
    (void)emitterId;
    (void)playing;
#endif
}

// =============================================================================
// Update: dispatch compute pipeline
// =============================================================================

void GPUParticleSystem::Update(float deltaTime)
{
    if (!m_initialized)
        return;
#ifdef SPARK_PLATFORM_WINDOWS
    for (auto& [id, emitter] : m_emitters)
    {
        if (emitter->playing && emitter->initialized)
            DispatchEmitter(*emitter, deltaTime);
    }
#else
    (void)deltaTime;
#endif
}

#ifdef SPARK_PLATFORM_WINDOWS

void GPUParticleSystem::DispatchEmitter(GPUEmitterResources& emitter, float deltaTime)
{
    emitter.emissionAccumulator += emitter.desc.emissionRate * deltaTime;
    uint32_t emitCount = static_cast<uint32_t>(emitter.emissionAccumulator);
    emitter.emissionAccumulator -= static_cast<float>(emitCount);
    emitter.elapsedTime += deltaTime;
    emitCount = std::min(emitCount, emitter.capacity);

    UpdateEmitterCB(emitter, deltaTime);

    ID3D11Buffer* cbs[] = {emitter.emitterCBBuffer.Get()};

    // Pass 1: Emit new particles (pop dead list, push alive list)
    if (emitCount > 0)
    {
        m_context->CSSetShader(m_emitCS.Get(), nullptr, 0);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        UINT initCounts[] = {UINT(-1), UINT(-1), UINT(-1)};
        ID3D11UnorderedAccessView* uavs[] = {emitter.particleUAV.Get(), emitter.deadListUAV.Get(),
                                             emitter.aliveListUAV.Get()};
        m_context->CSSetUnorderedAccessViews(0, 3, uavs, initCounts);
        m_context->Dispatch((emitCount + kParticleThreadGroupSize - 1) / kParticleThreadGroupSize, 1, 1);
    }

    // Pass 2: Simulate (integrate forces, kill expired, compact alive, write sort keys)
    {
        m_context->CSSetShader(m_simulateCS.Get(), nullptr, 0);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        UINT initCounts[] = {UINT(-1), UINT(-1), 0, UINT(-1), UINT(-1), UINT(-1)};
        ID3D11UnorderedAccessView* uavs[] = {emitter.particleUAV.Get(),      emitter.aliveListUAV.Get(),
                                             emitter.aliveListSwapUAV.Get(), emitter.deadListUAV.Get(),
                                             emitter.sortKeysUAV.Get(),      emitter.indirectArgsUAV.Get()};
        m_context->CSSetUnorderedAccessViews(0, 6, uavs, initCounts);
        m_context->Dispatch((emitter.capacity + kParticleThreadGroupSize - 1) / kParticleThreadGroupSize, 1, 1);
    }

    // Unbind UAVs to avoid hazards between passes
    ID3D11UnorderedAccessView* nullUAVs[6] = {};
    m_context->CSSetUnorderedAccessViews(0, 6, nullUAVs, nullptr);

    // Pass 3: Bitonic sort alive particles by camera distance
    DispatchBitonicSort(emitter);

    // Swap alive lists for next frame
    std::swap(emitter.aliveListBuffer, emitter.aliveListSwapBuffer);
    std::swap(emitter.aliveListUAV, emitter.aliveListSwapUAV);

    m_context->CSSetShader(nullptr, nullptr, 0);
}

void GPUParticleSystem::DispatchBitonicSort(GPUEmitterResources& emitter)
{
    m_context->CSSetShader(m_bitonicSortCS.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[] = {emitter.sortKeysUAV.Get()};
    m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    uint32_t threadGroups = std::max(1u, emitter.capacity / (kParticleThreadGroupSize * 2));
    m_context->Dispatch(threadGroups, 1, 1);

    ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
}

void GPUParticleSystem::UpdateEmitterCB(GPUEmitterResources& emitter, float deltaTime)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(emitter.emitterCBBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
        return;

    float pendingEmission = emitter.emissionAccumulator + emitter.desc.emissionRate * deltaTime;
    uint32_t emitCount = std::min(static_cast<uint32_t>(pendingEmission), emitter.capacity);

    GPUParticleEmitterCB cb = {};
    cb.emitterPosition = emitter.position;
    cb.deltaTime = deltaTime;
    cb.gravity = emitter.desc.gravity;
    cb.drag = emitter.desc.drag;
    cb.emissionRate = emitter.desc.emissionRate;
    cb.shapeRadius = emitter.desc.shapeRadius;
    cb.coneAngle = emitter.desc.coneAngle;
    cb.maxParticles = emitter.capacity;
    cb.lifetimeRange = {emitter.desc.lifetime.min, emitter.desc.lifetime.max, emitter.desc.startSpeed.min,
                        emitter.desc.startSpeed.max};
    cb.sizeRange = {emitter.desc.startSize.min, emitter.desc.startSize.max, emitter.desc.startRotation.min,
                    emitter.desc.startRotation.max};
    cb.rotSpeedRange = {emitter.desc.rotationSpeed.min, emitter.desc.rotationSpeed.max, 0.0f, 0.0f};
    cb.emitCount = emitCount;
    cb.emitterShape = static_cast<uint32_t>(emitter.desc.shape);
    cb.gravityMultiplier = emitter.desc.gravityMultiplier;
    cb.totalTime = emitter.elapsedTime;

    std::memcpy(mapped.pData, &cb, sizeof(GPUParticleEmitterCB));
    m_context->Unmap(emitter.emitterCBBuffer.Get(), 0);
}

void GPUParticleSystem::DispatchCS(ID3D11ComputeShader* shader, uint32_t threadGroupCountX)
{
    m_context->CSSetShader(shader, nullptr, 0);
    m_context->Dispatch(threadGroupCountX, 1, 1);
}

#endif // SPARK_PLATFORM_WINDOWS

// =============================================================================
// Render
// =============================================================================

void GPUParticleSystem::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    if (!m_initialized)
        return;
#ifdef SPARK_PLATFORM_WINDOWS
    for (const auto& [id, emitter] : m_emitters)
    {
        if (!emitter->playing || !emitter->initialized)
            continue;

        ID3D11ShaderResourceView* srvs[] = {emitter->particleSRV.Get(), emitter->aliveListSRV.Get()};
        m_context->VSSetShaderResources(0, 2, srvs);
        m_context->DrawInstancedIndirect(emitter->indirectArgsBuffer.Get(), 0);

        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        m_context->VSSetShaderResources(0, 2, nullSRVs);
    }
#else
    (void)view;
    (void)projection;
#endif
}

// =============================================================================
// Queries
// =============================================================================

uint32_t GPUParticleSystem::GetEmitterCount() const
{
#ifdef SPARK_PLATFORM_WINDOWS
    return static_cast<uint32_t>(m_emitters.size());
#else
    return 0;
#endif
}

std::string GPUParticleSystem::GetStatus() const
{
    if (!m_initialized)
        return "GPU Particle System: Not initialized\n";

    std::string status = "GPU Particle System:\n";
#ifdef SPARK_PLATFORM_WINDOWS
    uint32_t totalCapacity = 0;
    size_t gpuMemory = 0;

    for (const auto& [id, emitter] : m_emitters)
    {
        totalCapacity += emitter->capacity;
        // pool + dead + alive*2 + sortKeys + indirectArgs + CB
        gpuMemory += emitter->capacity * (sizeof(GPUParticleData) + sizeof(uint32_t) * 5);
        gpuMemory += sizeof(uint32_t) * 4 + sizeof(GPUParticleEmitterCB);
    }

    status += std::format("  Emitters:          {}\n", m_emitters.size());
    status += std::format("  Total capacity:    {}\n", totalCapacity);
    status += std::format("  GPU memory (est):  {} KB\n", gpuMemory / 1024);

    for (const auto& [id, emitter] : m_emitters)
    {
        status += std::format("  [{}] \"{}\": cap={}, playing={}, elapsed={:.1f}s\n", id, emitter->desc.name,
                              emitter->capacity, emitter->playing, emitter->elapsedTime);
    }
#else
    status += "  Platform: non-Windows (stub)\n";
#endif
    return status;
}
