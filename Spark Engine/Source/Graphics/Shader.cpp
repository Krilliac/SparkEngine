#ifdef SPARK_PLATFORM_WINDOWS
#include "../Core/Platform.h"
// Shader.cpp - Enhanced shader system implementation with AAA features (C++14 compatible)
#include "Shader.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// Helper function for C++14 compatible file existence check
bool FileExists(const std::wstring& filename) {
    DWORD attrs = GetFileAttributesW(filename.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

// Console logging integration
#define LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype) \
    do { \
        std::wstring wstr = wmsg; \
        std::wstring wtypestr = wtype; \
        std::string msg(wstr.begin(), wstr.end()); \
        std::string type(wtypestr.begin(), wtypestr.end()); \
        Spark::SimpleConsole::GetInstance().Log(msg, type); \
    } while(0)

// ============================================================================
// SHADER RESOURCE IMPLEMENTATIONS
// ============================================================================

void VertexShaderResource::Bind(ID3D11DeviceContext* context)
{
    if (m_vertexShader && m_inputLayout) {
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
    if (m_pixelShader) {
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
    : m_device(nullptr)
    , m_context(nullptr)
    , m_activeVariant(-1)
    , m_hotReloadEnabled(true)
    , m_validationEnabled(true)
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
    
    // **FIXED: Add correct search paths pointing to source directory**
    m_searchPaths.clear();
    m_searchPaths.push_back("D:/Spark Engine/SparkEngine/Spark Engine/Shaders/HLSL/");
    m_searchPaths.push_back("../Spark Engine/Shaders/HLSL/");
    m_searchPaths.push_back("../../Spark Engine/Shaders/HLSL/");
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
    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader::Initialize started with AAA features.", L"INFO");
    ASSERT_MSG(device != nullptr, "Shader::Initialize device is null");
    ASSERT_MSG(context != nullptr, "Shader::Initialize context is null");

    m_device = device;
    m_context = context;

    HRESULT hr = CreateConstantBuffers();
    if (FAILED(hr)) {
        std::wstring errorMsg = L"Enhanced Shader constant buffer creation failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Initialize shader resources (C++14 compatible)
    m_vertexShader = std::unique_ptr<VertexShaderResource>(new VertexShaderResource());
    m_pixelShader = std::unique_ptr<PixelShaderResource>(new PixelShaderResource());

    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader system initialized with comprehensive features.", L"SUCCESS");
    return S_OK;
}

void Shader::Shutdown()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader::Shutdown called.", L"INFO");
    
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

HRESULT Shader::LoadVertexShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Loading enhanced vertex shader: ") + filename, L"INFO");
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    ASSERT_MSG(!filename.empty(), "LoadVertexShader: filename empty");
    ASSERT_MSG(m_device != nullptr, "LoadVertexShader: device is null");

    ComPtr<ID3DBlob> vsBlob;
    
    // **FIX: Explicitly specify vertex shader compilation**
    ShaderCompilationFlags vsFlags = flags;
    vsFlags.target = "vs_5_0"; // Force vertex shader target
    
    HRESULT hr = CompileShaderFromFileAdvanced(filename, ShaderType::VERTEX_SHADER, vsFlags, &vsBlob);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compilation failed, trying fallback method", L"WARNING");
        
        // **FALLBACK: Try direct D3DCompileFromFile**
        ComPtr<ID3DBlob> errorBlob;
        hr = D3DCompileFromFile(
            filename.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main",
            "vs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            &vsBlob,
            &errorBlob
        );
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
                std::wstring wErrorString(errorString.begin(), errorString.end());
                LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compilation error: " + wErrorString, L"ERROR");
            }
            return hr;
        }
    }

    m_vertexShader = std::make_unique<VertexShaderResource>();
    hr = m_device->CreateVertexShader(
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        nullptr,
        m_vertexShader->m_vertexShader.GetAddressOf()
    );
    if (FAILED(hr)) {
        std::wstring errorMsg = L"CreateVertexShader failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    m_vertexShader->m_shaderBlob = vsBlob;
    
    hr = CreateInputLayout(vsBlob.Get(), m_vertexShader->m_inputLayout.GetAddressOf());
    if (SUCCEEDED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader and input layout loaded successfully", L"SUCCESS");
    } else {
        std::wstring errorMsg = L"CreateInputLayout failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
    }
    
    return hr;
}

HRESULT Shader::LoadPixelShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Loading enhanced pixel shader: ") + filename, L"INFO");
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    ASSERT_MSG(!filename.empty(), "LoadPixelShader: filename empty");
    ASSERT_MSG(m_device != nullptr, "LoadPixelShader: device is null");

    ComPtr<ID3DBlob> psBlob;
    
    // **FIX: Explicitly specify pixel shader compilation**
    ShaderCompilationFlags psFlags = flags;
    psFlags.target = "ps_5_0"; // Force pixel shader target
    
    HRESULT hr = CompileShaderFromFileAdvanced(filename, ShaderType::PIXEL_SHADER, psFlags, &psBlob);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compilation failed, trying fallback method", L"WARNING");
        
        // **FALLBACK: Try direct D3DCompileFromFile**
        ComPtr<ID3DBlob> errorBlob;
        hr = D3DCompileFromFile(
            filename.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main",
            "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            &psBlob,
            &errorBlob
        );
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
                std::wstring wErrorString(errorString.begin(), errorString.end());
                LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compilation error: " + wErrorString, L"ERROR");
            }
            return hr;
        }
    }

    m_pixelShader = std::make_unique<PixelShaderResource>();
    hr = m_device->CreatePixelShader(
        psBlob->GetBufferPointer(),
        psBlob->GetBufferSize(),
        nullptr,
        m_pixelShader->m_pixelShader.GetAddressOf()
    );

    if (SUCCEEDED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader loaded successfully", L"SUCCESS");
    } else {
        std::wstring errorMsg = L"CreatePixelShader failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
    }

    return hr;
}

void Shader::SetShaders()
{
    ASSERT(m_context != nullptr);
    
    if (m_vertexShader && m_vertexShader->IsValid()) {
        m_vertexShader->Bind(m_context);
    }
    
    if (m_pixelShader && m_pixelShader->IsValid()) {
        m_pixelShader->Bind(m_context);
    }
    
    // Bind constant buffers to appropriate slots
    ID3D11Buffer* buffers[] = {
        m_perFrameBuffer.Get(),
        m_perObjectBuffer.Get(),
        m_perMaterialBuffer.Get(),
        m_lightingDataBuffer.Get(),
        m_postProcessingBuffer.Get()
    };
    
    m_context->VSSetConstantBuffers(0, 5, buffers);
    m_context->PSSetConstantBuffers(0, 5, buffers);
}

void Shader::UnbindShaders()
{
    ASSERT(m_context != nullptr);
    
    if (m_vertexShader) {
        m_vertexShader->Unbind(m_context);
    }
    
    if (m_pixelShader) {
        m_pixelShader->Unbind(m_context);
    }
}

bool Shader::IsValid() const
{
    return m_vertexShader && m_vertexShader->IsValid() &&
           m_pixelShader && m_pixelShader->IsValid();
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
    if (SUCCEEDED(hr)) {
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
    if (SUCCEEDED(hr)) {
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
    if (SUCCEEDED(hr)) {
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
    if (SUCCEEDED(hr)) {
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
    if (SUCCEEDED(hr)) {
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
    if (SUCCEEDED(hr)) {
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
    if (FAILED(hr)) {
        std::wstring errorMsg = L"Failed to create per-frame constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }
    
    // Create per-object constant buffer
    bufferDesc.ByteWidth = sizeof(PerObjectConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perObjectBuffer.GetAddressOf());
    if (FAILED(hr)) {
        std::wstring errorMsg = L"Failed to create per-object constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }
    
    // Create per-material constant buffer
    bufferDesc.ByteWidth = sizeof(PerMaterialConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perMaterialBuffer.GetAddressOf());
    if (FAILED(hr)) {
        std::wstring errorMsg = L"Failed to create per-material constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }
    
    // Create lighting data constant buffer
    bufferDesc.ByteWidth = sizeof(LightingData);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_lightingDataBuffer.GetAddressOf());
    if (FAILED(hr)) {
        std::wstring errorMsg = L"Failed to create lighting data constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }
    
    // Create post-processing constant buffer
    bufferDesc.ByteWidth = sizeof(PostProcessingConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_postProcessingBuffer.GetAddressOf());
    if (FAILED(hr)) {
        std::wstring errorMsg = L"Failed to create post-processing constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }
    
    // Log buffer sizes for debugging
    std::wstring sizeMsg = L"Constant buffer sizes: PerFrame=" + std::to_wstring(sizeof(PerFrameConstants)) +
                          L", PerObject=" + std::to_wstring(sizeof(PerObjectConstants)) +
                          L", PerMaterial=" + std::to_wstring(sizeof(PerMaterialConstants)) +
                          L", Lighting=" + std::to_wstring(sizeof(LightingData)) +
                          L", PostProcess=" + std::to_wstring(sizeof(PostProcessingConstants));
    LOG_TO_CONSOLE_IMMEDIATE(sizeMsg, L"DEBUG");
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Shader constant buffers created successfully", L"SUCCESS");
    return S_OK;
}

HRESULT Shader::CompileShaderFromFileAdvanced(const std::wstring& filename, ShaderType type, 
                                             const ShaderCompilationFlags& flags, ID3DBlob** shaderBlob)
{
    ASSERT(!filename.empty());
    ASSERT(shaderBlob != nullptr);
    
    std::wstring fullPath = filename;
    bool fileFound = false;
    
    // Try to find the file in search paths
    if (!FileExists(fullPath)) {
        for (const auto& searchPath : m_searchPaths) {
            std::wstring testPath = std::wstring(searchPath.begin(), searchPath.end()) + filename;
            if (FileExists(testPath)) {
                fullPath = testPath;
                fileFound = true;
                break;
            }
        }
    } else {
        fileFound = true;
    }
    
    if (!fileFound) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Shader file not found: " + filename, L"ERROR");
        return E_FAIL;
    }
    
    // Determine shader target based on type
    std::string target;
    switch (type) {
        case ShaderType::VERTEX_SHADER:
            target = "vs_5_0";
            break;
        case ShaderType::PIXEL_SHADER:
            target = "ps_5_0";
            break;
        case ShaderType::GEOMETRY_SHADER:
            target = "gs_5_0";
            break;
        case ShaderType::HULL_SHADER:
            target = "hs_5_0";
            break;
        case ShaderType::DOMAIN_SHADER:
            target = "ds_5_0";
            break;
        case ShaderType::COMPUTE_SHADER:
            target = "cs_5_0";
            break;
        default:
            target = "vs_5_0";
            break;
    }
    
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
    if (flags.enableDebug) {
        compileFlags |= D3DCOMPILE_DEBUG;
        compileFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
    }
    if (flags.enableOptimization) {
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }
    if (flags.treatWarningsAsErrors) {
        compileFlags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
    }
    
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(
        fullPath.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        flags.entryPoint.c_str(),
        target.c_str(),
        compileFlags,
        0,
        shaderBlob,
        &errorBlob
    );
    
    if (FAILED(hr)) {
        if (errorBlob) {
            std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorString(errorString.begin(), errorString.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader compilation error: " + wErrorString, L"ERROR");
        }
        
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
    } else {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
    }
    
    return hr;
}

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

HRESULT Shader::CreateInputLayout(ID3DBlob* vertexShaderBlob, ID3D11InputLayout** inputLayout)
{
    // **FIXED: Input layout to match source shader VS_INPUT structure**
    // The source BasicVS.hlsl expects: POSITION, NORMAL, TEXCOORD0
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    HRESULT hr = m_device->CreateInputLayout(
        layoutDesc,
        ARRAYSIZE(layoutDesc),
        vertexShaderBlob->GetBufferPointer(),
        vertexShaderBlob->GetBufferSize(),
        inputLayout);

    if (FAILED(hr)) {
        // **FALLBACK: Try with FLOAT3 positions (our Vertex structure)**
        D3D11_INPUT_ELEMENT_DESC fallbackLayoutDesc[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };

        hr = m_device->CreateInputLayout(
            fallbackLayoutDesc,
            ARRAYSIZE(fallbackLayoutDesc),
            vertexShaderBlob->GetBufferPointer(),
            vertexShaderBlob->GetBufferSize(),
            inputLayout);
    }

    if (FAILED(hr)) {
        std::wstring errorMsg = L"Failed to create input layout with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Input layout created successfully", L"SUCCESS");
    }

    return hr;
}
#endif // SPARK_PLATFORM_WINDOWS
