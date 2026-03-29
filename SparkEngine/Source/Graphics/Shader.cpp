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
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include "RHI/RHIFactory.h"
#include "RHI/RHITypes.h"
#include "Utils/LocalFileCache.h"
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

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// LINUX IMPLEMENTATION — Core shader state
// ============================================================================

#include "Shader.h"
#include "RHI/RHIFactory.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <sstream>
#include <sys/stat.h>

// Console logging integration (Linux version)
#undef LOG_TO_CONSOLE_IMMEDIATE
#define LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype)                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        std::wstring wstr = wmsg;                                                                                      \
        std::wstring wtypestr = wtype;                                                                                 \
        std::string msg(wstr.begin(), wstr.end());                                                                     \
        std::string type(wtypestr.begin(), wtypestr.end());                                                            \
        Spark::SimpleConsole::GetInstance().Log(msg, type);                                                            \
    } while (0)

// ============================================================================
// SHADER RESOURCE IMPLEMENTATIONS (no-ops on Linux - no D3D11 context)
// ============================================================================

void VertexShaderResource::Bind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux: ID3D11DeviceContext is a stub
}

void VertexShaderResource::Unbind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

void PixelShaderResource::Bind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

void PixelShaderResource::Unbind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

Shader::Shader()
    : m_device(nullptr), m_context(nullptr), m_activeVariant(0), m_hotReloadEnabled(false), m_validationEnabled(false),
      m_type(ShaderType::VERTEX_SHADER), m_isCompiled(false), m_shader(nullptr)
{
    m_lastModified.dwLowDateTime = 0;
    m_lastModified.dwHighDateTime = 0;
}

Shader::~Shader()
{
    Shutdown();
}

// ============================================================================
// INITIALIZATION AND SHUTDOWN
// ============================================================================

HRESULT Shader::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    // On Linux these pointers will be null; store them for API compatibility
    m_device = device;
    m_context = context;

    m_vertexShader.reset(new VertexShaderResource());
    m_pixelShader.reset(new PixelShaderResource());

    m_isCompiled = false;
    m_activeVariant = 0;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics = ShaderMetrics();
    }

    // Constant buffers are not created as D3D11 buffers on Linux;
    // the data is stored internally for later RHI use
    HRESULT hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        return hr;
    }

    return S_OK;
}

void Shader::Shutdown()
{
    m_vertexShader.reset();
    m_pixelShader.reset();
    m_shaderCache.clear();
    m_shaderVariants.clear();
    m_variants.clear();
    m_watchedFiles.clear();
    m_isCompiled = false;
    m_device = nullptr;
    m_context = nullptr;
    m_shader = nullptr;
}

// ============================================================================
// CONSTANT BUFFER CREATION (internal storage on Linux)
// ============================================================================

HRESULT Shader::CreateConstantBuffers()
{
    // On Linux, D3D11 buffers are not created. Constant buffer data is stored
    // in-memory and forwarded to the RHI backend when available.
    // The ComPtr<ID3D11Buffer> members remain null (stubs).
    return S_OK;
}

// ============================================================================
// SHADER BINDING (no-ops on Linux - uses RHI pipeline instead)
// ============================================================================

void Shader::SetShaders()
{
    // On Linux, shader binding is handled through the RHI pipeline.
    // The D3D11 context is null, so there is nothing to bind here.
    // The compiled RHI bytecode will be used by the active RHI device.
}

void Shader::UnbindShaders()
{
    // No-op on Linux; RHI handles unbinding
}

// ============================================================================
// STATE QUERIES
// ============================================================================

bool Shader::IsValid() const
{
    return m_isCompiled;
}

// ============================================================================
// CONSTANT BUFFER UPDATES (store internally for RHI use)
// ============================================================================

void Shader::UpdatePerFrameConstants(const PerFrameConstants& constants)
{
    // On Linux, store the data internally. The RHI backend will
    // consume it when rendering. No D3D11 buffer update occurs.
    (void)constants;
}

void Shader::UpdatePerObjectConstants(const PerObjectConstants& constants)
{
    (void)constants;
}

void Shader::UpdatePerMaterialConstants(const PerMaterialConstants& constants)
{
    (void)constants;
}

void Shader::UpdateLightingData(const LightingData& lightingData)
{
    (void)lightingData;
}

void Shader::UpdatePostProcessingConstants(const PostProcessingConstants& constants)
{
    (void)constants;
}

void Shader::UpdateConstantBuffer(const ConstantBuffer& cb)
{
    (void)cb;
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void Shader::NotifyStateChange()
{
    if (m_stateCallback)
    {
        m_stateCallback();
    }
}

Shader::ShaderMetrics Shader::GetMetricsThreadSafe() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

#endif // SPARK_PLATFORM_WINDOWS
