/**
 * @file ShaderWindows.cpp
 * @brief Windows/D3D11 implementation — split from Shader.cpp
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @file Shader.cpp
 * @brief Core shader state: construction, initialization, binding, and constant buffers
 *
 * Manages the Shader object lifecycle (construction, initialization, shutdown),
 * pipeline binding/unbinding, constant buffer creation and updates, and thread-safe
 * metrics access. Compilation logic lives in ShaderCompilation.cpp; hot-reload and
 * variant management live in ShaderHotReload.cpp; console integration methods live
 * in ShaderConsoleOps.cpp.
 */
#include "Shader.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
// Phase U: activated Tier 2 graphics orphan — process-wide singleton file
// watcher that recompiles shaders when their source .hlsl files change on
// disk. Wired into Shader::Initialize / Shader::LoadFromFile /
// Shader::HotReloadShaders so every Shader instance shares the same
// registry of watched directories.
#include "ShaderHotReload.h"
// Phase V: activated Tier 2 graphics orphan — persistent on-disk shader
// cache. Wired into Shader::Initialize so every Shader instance queries
// the same process-wide cache before calling the backend compiler.
#include "ShaderDiskCache.h"
// Phase W: activated Tier 2 graphics orphan — in-memory shader cross-
// compilation cache. Shared singleton reachable from any Shader call
// path so asset cookers and tests have a stable handle to the cross-
// compile surface.
#include "ShaderCrossCompiler.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include "RHI/RHIFactory.h"
#include "RHI/RHITypes.h"
#include "Utils/LocalFileCache.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// Console logging — use centralized macros from LogMacros.h
#include "../Utils/LogMacros.h"

// ============================================================================
// SHADER RESOURCE IMPLEMENTATIONS
// ============================================================================

void VertexShaderResource::Bind(ID3D11DeviceContext* context)
{
    if (m_vertexShader && m_inputLayout)
    {
        context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        context->IASetInputLayout(m_inputLayout.Get());
    }
}

void VertexShaderResource::Unbind(ID3D11DeviceContext* context)
{
    context->VSSetShader(nullptr, nullptr, 0);
    context->IASetInputLayout(nullptr);
}

void PixelShaderResource::Bind(ID3D11DeviceContext* context)
{
    if (m_pixelShader)
    {
        context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    }
}

void PixelShaderResource::Unbind(ID3D11DeviceContext* context)
{
    context->PSSetShader(nullptr, nullptr, 0);
}

// ============================================================================
// MAIN SHADER CLASS IMPLEMENTATION
// ============================================================================

Shader::Shader()
    : m_device(nullptr), m_context(nullptr), m_activeVariant(-1), m_hotReloadEnabled(true), m_validationEnabled(true)
{
    // Initialize default compilation flags for C++14 compatibility
    m_defaultFlags.enableDebug = false;
    m_defaultFlags.enableOptimization = true;
    m_defaultFlags.enableValidation = true;
    m_defaultFlags.treatWarningsAsErrors = false;
    m_defaultFlags.entryPoint = "main";

    // Initialize metrics for C++14 compatibility
    m_metrics.compiledShaders = 0;
    m_metrics.failedCompilations = 0;
    m_metrics.activeVariants = 0;
    m_metrics.hotReloadCount = 0;
    m_metrics.lastCompileTime = 0.0f;
    m_metrics.totalCompileTime = 0.0f;
    m_metrics.shaderMemoryUsage = 0;
    m_metrics.hotReloadEnabled = false;

    // Search paths for shader files — relative only, no hardcoded absolute paths
    m_searchPaths.clear();
    m_searchPaths.push_back("../SparkEngine/Shaders/HLSL/");
    m_searchPaths.push_back("../../SparkEngine/Shaders/HLSL/");
    m_searchPaths.push_back("Shaders/HLSL/");
    m_searchPaths.push_back("Assets/Shaders/");
    m_searchPaths.push_back("./");

    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader system constructed with AAA features and console integration.", L"INFO");
}

Shader::~Shader()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader destructor called.", L"INFO");
    Shutdown();
}

HRESULT Shader::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);

    m_device = device;
    m_context = context;

    HRESULT hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Shader constant buffer creation failed with HR=0x%08lX",
                        static_cast<long>(hr));
        return hr;
    }

    m_vertexShader = std::make_unique<VertexShaderResource>();
    m_pixelShader = std::make_unique<PixelShaderResource>();

    // Phase O: activate the shader variant system alongside the existing
    // shader resources. Zero dependencies on the D3D11 device — pure
    // CPU keyword / variant bookkeeping.
    m_variantSystem.Initialize();

    // Phase U: activate Spark::Graphics::ShaderHotReload singleton. The
    // first Shader::Initialize call initialises the process-wide file
    // watcher with each search path that exists on disk; subsequent
    // Shader instances and LoadFromFile calls add further watch
    // directories on demand via AddWatchDirectory.
    auto& hotReload = Spark::Graphics::ShaderHotReload::GetInstance();
    if (!hotReload.IsWatching())
    {
        std::error_code ec;
        bool initialized = false;
        for (const auto& path : m_searchPaths)
        {
            if (std::filesystem::exists(path, ec))
            {
                hotReload.Initialize(path);
                initialized = true;
                break;
            }
        }
        if (!initialized)
        {
            hotReload.Initialize(".");
        }
    }
    else
    {
        for (const auto& path : m_searchPaths)
        {
            std::error_code ec;
            if (std::filesystem::exists(path, ec))
            {
                hotReload.AddWatchDirectory(path);
            }
        }
    }

    // Phase V: activate Spark::Graphics::ShaderDiskCache singleton. The
    // first Shader::Initialize call creates the cache directory under
    // the working directory ("ShaderCache/"); subsequent Shader
    // instances reuse the same cache. Tests may override the directory
    // via GetShaderDiskCache().Initialize(path) before the first
    // Shader::Initialize runs.
    auto& diskCache = Spark::Graphics::GetShaderDiskCache();
    if (!diskCache.IsInitialized())
    {
        diskCache.Initialize(std::filesystem::path("ShaderCache"));
    }

    // Phase W: activate Spark::Graphics::ShaderCrossCompiler singleton.
    // In-memory compile cache used by CompileAll / CompileAsync. The
    // internal per-target Compile* functions are currently stubs that
    // report success without producing bytecode — Phase W wires the
    // lifecycle so tests and future asset cookers that need a
    // cross-target compile surface have a shared instance to talk to,
    // and so any real DXC / SPIRV-Cross integration slots into this
    // existing activation.
    auto& crossCompiler = Spark::Graphics::GetShaderCrossCompiler();
    if (!crossCompiler.IsInitialized())
    {
        crossCompiler.Initialize();
    }

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader system initialized");
    return S_OK;
}

void Shader::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader system shutting down (%zu cached, %zu variants)",
                   m_shaderCache.size(), m_shaderVariants.size());

    // Clear shader cache
    m_shaderCache.clear();
    m_shaderVariants.clear();

    // Reset DirectX resources
    m_postProcessingBuffer.Reset();
    m_lightingDataBuffer.Reset();
    m_perMaterialBuffer.Reset();
    m_perObjectBuffer.Reset();
    m_perFrameBuffer.Reset();

    m_pixelShader.reset();
    m_vertexShader.reset();

    // Phase O: tear down the variant system — clears all keyword
    // registrations and groups so a subsequent Initialize starts clean.
    m_variantSystem.Shutdown();

    m_device = nullptr;
    m_context = nullptr;

    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader shutdown complete.", L"INFO");
}

// ============================================================================
// SHADER BINDING AND STATE
// ============================================================================

void Shader::SetShaders()
{
    ASSERT(m_context != nullptr);

    if (m_vertexShader && m_vertexShader->IsValid())
    {
        m_vertexShader->Bind(m_context);
    }

    if (m_pixelShader && m_pixelShader->IsValid())
    {
        m_pixelShader->Bind(m_context);
    }

    // Bind constant buffers to appropriate slots
    ID3D11Buffer* buffers[] = {m_perFrameBuffer.Get(), m_perObjectBuffer.Get(), m_perMaterialBuffer.Get(),
                               m_lightingDataBuffer.Get(), m_postProcessingBuffer.Get()};

    m_context->VSSetConstantBuffers(0, 5, buffers);
    m_context->PSSetConstantBuffers(0, 5, buffers);
}

void Shader::UnbindShaders()
{
    ASSERT(m_context != nullptr);

    if (m_vertexShader)
    {
        m_vertexShader->Unbind(m_context);
    }

    if (m_pixelShader)
    {
        m_pixelShader->Unbind(m_context);
    }
}

bool Shader::IsValid() const
{
    return m_vertexShader && m_vertexShader->IsValid() && m_pixelShader && m_pixelShader->IsValid();
}

// ============================================================================
// CONSTANT BUFFER MANAGEMENT
// ============================================================================

void Shader::UpdatePerFrameConstants(const PerFrameConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perFrameBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perFrameBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        PerFrameConstants* data = reinterpret_cast<PerFrameConstants*>(mapped.pData);
        *data = constants;

        // Transpose matrices for HLSL
        data->ViewMatrix = XMMatrixTranspose(constants.ViewMatrix);
        data->ProjectionMatrix = XMMatrixTranspose(constants.ProjectionMatrix);
        data->ViewProjectionMatrix = XMMatrixTranspose(constants.ViewProjectionMatrix);

        m_context->Unmap(m_perFrameBuffer.Get(), 0);
    }
}

void Shader::UpdatePerObjectConstants(const PerObjectConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perObjectBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perObjectBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        PerObjectConstants* data = reinterpret_cast<PerObjectConstants*>(mapped.pData);
        *data = constants;

        // Transpose matrices for HLSL
        data->WorldMatrix = XMMatrixTranspose(constants.WorldMatrix);
        data->WorldViewProjectionMatrix = XMMatrixTranspose(constants.WorldViewProjectionMatrix);
        data->WorldInverseTransposeMatrix = XMMatrixTranspose(constants.WorldInverseTransposeMatrix);
        data->PreviousWorldMatrix = XMMatrixTranspose(constants.PreviousWorldMatrix);

        m_context->Unmap(m_perObjectBuffer.Get(), 0);
    }
}

void Shader::UpdatePerMaterialConstants(const PerMaterialConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perMaterialBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perMaterialBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        auto dataPtr = reinterpret_cast<PerMaterialConstants*>(mapped.pData);
        *dataPtr = constants;
        m_context->Unmap(m_perMaterialBuffer.Get(), 0);
    }
}

void Shader::UpdateLightingData(const LightingData& lightingData)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_lightingDataBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_lightingDataBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        auto dataPtr = reinterpret_cast<LightingData*>(mapped.pData);
        *dataPtr = lightingData;
        m_context->Unmap(m_lightingDataBuffer.Get(), 0);
    }
}

void Shader::UpdatePostProcessingConstants(const PostProcessingConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_postProcessingBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_postProcessingBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        auto dataPtr = reinterpret_cast<PostProcessingConstants*>(mapped.pData);
        *dataPtr = constants;
        m_context->Unmap(m_postProcessingBuffer.Get(), 0);
    }
}

void Shader::UpdateConstantBuffer(const ConstantBuffer& cb)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perObjectBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perObjectBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        ConstantBuffer* data = reinterpret_cast<ConstantBuffer*>(mapped.pData);
        data->World = XMMatrixTranspose(cb.World);
        data->View = XMMatrixTranspose(cb.View);
        data->Projection = XMMatrixTranspose(cb.Projection);

        m_context->Unmap(m_perObjectBuffer.Get(), 0);
    }
}

HRESULT Shader::CreateConstantBuffers()
{
    ASSERT(m_device != nullptr);

    LOG_TO_CONSOLE_IMMEDIATE(L"Creating shader constant buffers...", L"INFO");

    // Create per-frame constant buffer
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(PerFrameConstants);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perFrameBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create per-frame constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create per-object constant buffer
    bufferDesc.ByteWidth = sizeof(PerObjectConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perObjectBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create per-object constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create per-material constant buffer
    bufferDesc.ByteWidth = sizeof(PerMaterialConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perMaterialBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create per-material constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create lighting data constant buffer
    bufferDesc.ByteWidth = sizeof(LightingData);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_lightingDataBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create lighting data constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create post-processing constant buffer
    bufferDesc.ByteWidth = sizeof(PostProcessingConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_postProcessingBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create post-processing constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Log buffer sizes for debugging
    std::wstring sizeMsg = L"Constant buffer sizes: PerFrame=" + std::to_wstring(sizeof(PerFrameConstants)) +
                           L", PerObject=" + std::to_wstring(sizeof(PerObjectConstants)) + L", PerMaterial=" +
                           std::to_wstring(sizeof(PerMaterialConstants)) + L", Lighting=" +
                           std::to_wstring(sizeof(LightingData)) + L", PostProcess=" +
                           std::to_wstring(sizeof(PostProcessingConstants));
    LOG_TO_CONSOLE_IMMEDIATE(sizeMsg, L"DEBUG");

    LOG_TO_CONSOLE_IMMEDIATE(L"Shader constant buffers created successfully", L"SUCCESS");
    return S_OK;
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void Shader::NotifyStateChange()
{
    // Notify any registered callbacks about shader state changes
    LOG_TO_CONSOLE_IMMEDIATE(L"Shader state changed", L"DEBUG");
}

Shader::ShaderMetrics Shader::GetMetricsThreadSafe() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

#endif // inner SPARK_PLATFORM_WINDOWS


#endif // SPARK_PLATFORM_WINDOWS
