#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// Shader.cpp - Enhanced shader system implementation with AAA features (C++14 compatible)
#include "Shader.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include "RHI/RHIFactory.h"
#include "RHI/RHITypes.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS

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
    m_searchPaths.push_back("D:/SparkEngine/SparkEngine/SparkEngine/Shaders/HLSL/");
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

// ============================================================================
// SHADER LOADING FROM SOURCE AND FILE
// ============================================================================

HRESULT Shader::LoadShaderFromSource(const std::string& source, ShaderType type, const ShaderCompilationFlags& flags)
{
    ASSERT_MSG(!source.empty(), "LoadShaderFromSource: source is empty");
    ASSERT_MSG(m_device != nullptr, "LoadShaderFromSource: device is null");

    auto startTime = std::chrono::high_resolution_clock::now();

    // Determine shader target
    std::string target = flags.target;
    if (target.empty()) {
        switch (type) {
            case ShaderType::VERTEX_SHADER:   target = "vs_5_0"; break;
            case ShaderType::PIXEL_SHADER:    target = "ps_5_0"; break;
            case ShaderType::GEOMETRY_SHADER: target = "gs_5_0"; break;
            case ShaderType::HULL_SHADER:     target = "hs_5_0"; break;
            case ShaderType::DOMAIN_SHADER:   target = "ds_5_0"; break;
            case ShaderType::COMPUTE_SHADER:  target = "cs_5_0"; break;
            default:                          target = "vs_5_0"; break;
        }
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
    if (flags.enableDebug) {
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }
    if (flags.enableOptimization) {
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }
    if (flags.treatWarningsAsErrors) {
        compileFlags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
    }

    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        source.c_str(),
        source.size(),
        nullptr,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        flags.entryPoint.c_str(),
        target.c_str(),
        compileFlags,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorString(errorString.begin(), errorString.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader source compilation error: " + wErrorString, L"ERROR");
        }
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return hr;
    }

    // Create the appropriate shader object
    switch (type) {
        case ShaderType::VERTEX_SHADER: {
            m_vertexShader = std::make_unique<VertexShaderResource>();
            hr = m_device->CreateVertexShader(
                shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
                nullptr, m_vertexShader->m_vertexShader.GetAddressOf());
            if (SUCCEEDED(hr)) {
                m_vertexShader->m_shaderBlob = shaderBlob;
                hr = CreateInputLayout(shaderBlob.Get(), m_vertexShader->m_inputLayout.GetAddressOf());
            }
            break;
        }
        case ShaderType::PIXEL_SHADER: {
            m_pixelShader = std::make_unique<PixelShaderResource>();
            hr = m_device->CreatePixelShader(
                shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
                nullptr, m_pixelShader->m_pixelShader.GetAddressOf());
            break;
        }
        default:
            LOG_TO_CONSOLE_IMMEDIATE(L"Unsupported shader type for source compilation", L"WARNING");
            return E_NOTIMPL;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    float compileTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
        m_metrics.lastCompileTime = compileTimeMs;
        m_metrics.totalCompileTime += compileTimeMs;
    }

    if (SUCCEEDED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Shader compiled from source successfully", L"SUCCESS");
        m_isCompiled = true;
    }

    return hr;
}

HRESULT Shader::LoadFromFile(const std::string& filePath, ShaderType type, const ShaderCompilationFlags& flags)
{
    ASSERT_MSG(!filePath.empty(), "LoadFromFile: filePath is empty");

    // Read the file contents
    std::ifstream file(filePath);
    if (!file.is_open()) {
        // Try search paths
        for (const auto& searchPath : m_searchPaths) {
            std::string fullPath = searchPath + filePath;
            file.open(fullPath);
            if (file.is_open()) {
                m_filePath = fullPath;
                break;
            }
        }
        if (!file.is_open()) {
            std::wstring wPath(filePath.begin(), filePath.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader file not found: " + wPath, L"ERROR");
            return E_FAIL;
        }
    } else {
        m_filePath = filePath;
    }

    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    m_type = type;

    // Get file modification time for hot reload
    std::wstring wFilePath(m_filePath.begin(), m_filePath.end());
    HANDLE hFile = CreateFileW(wFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        GetFileTime(hFile, nullptr, nullptr, &m_lastModified);
        CloseHandle(hFile);
    }

    // Add to watched files for hot reload
    m_watchedFiles.push_back(wFilePath);

    return LoadShaderFromSource(source, type, flags);
}

// ============================================================================
// SHADER VARIANT MANAGEMENT
// ============================================================================

int Shader::CreateShaderVariant(const std::string& baseName, const std::vector<std::string>& defines)
{
    ShaderVariant variant;
    variant.id = static_cast<int>(m_variants.size());
    variant.name = baseName;
    for (size_t i = 0; i < defines.size(); ++i) {
        variant.name += "_" + defines[i];
    }
    variant.baseName = baseName;
    variant.defines = defines;
    variant.isCompiled = false;

    m_variants.push_back(variant);

    std::wstring msg = L"Created shader variant: ";
    std::string varName = variant.name;
    msg += std::wstring(varName.begin(), varName.end());
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.activeVariants = static_cast<int>(m_variants.size());
    }

    return variant.id;
}

void Shader::SetActiveVariant(int variantId)
{
    if (variantId >= 0 && variantId < static_cast<int>(m_variants.size())) {
        m_activeVariant = variantId;
        LOG_TO_CONSOLE_IMMEDIATE(L"Active shader variant changed", L"DEBUG");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid shader variant ID", L"WARNING");
    }
}

// ============================================================================
// HOT RELOAD
// ============================================================================

int Shader::HotReloadShaders()
{
    if (!m_hotReloadEnabled) {
        return 0;
    }

    int reloadCount = 0;

    for (const auto& watchedFile : m_watchedFiles) {
        HANDLE hFile = CreateFileW(watchedFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        FILETIME currentModified;
        GetFileTime(hFile, nullptr, nullptr, &currentModified);
        CloseHandle(hFile);

        if (CompareFileTime(&currentModified, &m_lastModified) > 0) {
            // File has been modified, reload
            m_lastModified = currentModified;

            std::string narrowPath(watchedFile.begin(), watchedFile.end());
            HRESULT hr = LoadFromFile(narrowPath, m_type, m_defaultFlags);
            if (SUCCEEDED(hr)) {
                reloadCount++;
                LOG_TO_CONSOLE_IMMEDIATE(L"Hot-reloaded shader: " + watchedFile, L"SUCCESS");
            } else {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to hot-reload shader: " + watchedFile, L"ERROR");
            }
        }
    }

    if (reloadCount > 0) {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.hotReloadCount += reloadCount;
        NotifyStateChange();
    }

    return reloadCount;
}

void Shader::UpdateFileMonitoring()
{
    // Check watched files for changes (called from game loop)
    if (m_hotReloadEnabled) {
        HotReloadShaders();
    }
}

// ============================================================================
// STATIC COMPILATION UTILITY (LEGACY)
// ============================================================================

HRESULT Shader::CompileShaderFromFile(const std::wstring& filename,
    const std::string& entryPoint,
    const std::string& shaderModel,
    ID3DBlob** blobOut)
{
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(
        filename.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        shaderModel.c_str(),
        compileFlags,
        0,
        blobOut,
        &errorBlob
    );

    if (FAILED(hr) && errorBlob) {
        std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
        OutputDebugStringA(errorString.c_str());
    }

    return hr;
}

// ============================================================================
// CONSOLE INTEGRATION METHODS
// ============================================================================

Shader::ShaderMetrics Shader::Console_GetMetrics() const
{
    return GetMetricsThreadSafe();
}

void Shader::Console_RecompileAll()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Recompiling all shaders...", L"INFO");

    // Reload from watched files
    for (const auto& watchedFile : m_watchedFiles) {
        std::string narrowPath(watchedFile.begin(), watchedFile.end());
        LoadFromFile(narrowPath, m_type, m_defaultFlags);
    }

    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Shader recompilation complete", L"SUCCESS");
}

void Shader::Console_SetHotReload(bool enabled)
{
    m_hotReloadEnabled = enabled;
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.hotReloadEnabled = enabled;
    }
    std::wstring msg = enabled ? L"Hot reload enabled" : L"Hot reload disabled";
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

void Shader::Console_SetCompilationFlags(bool enableDebug, bool enableOptimization)
{
    m_defaultFlags.enableDebug = enableDebug;
    m_defaultFlags.enableOptimization = enableOptimization;

    std::wstring msg = L"Compilation flags updated: debug=" +
        std::to_wstring(enableDebug) + L", optimization=" + std::to_wstring(enableOptimization);
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

std::string Shader::Console_ListShaders() const
{
    std::stringstream ss;
    ss << "=== Loaded Shaders ===" << std::endl;

    if (m_vertexShader && m_vertexShader->IsValid()) {
        ss << "  [VS] Vertex Shader - Active" << std::endl;
    } else {
        ss << "  [VS] Vertex Shader - Not loaded" << std::endl;
    }

    if (m_pixelShader && m_pixelShader->IsValid()) {
        ss << "  [PS] Pixel Shader - Active" << std::endl;
    } else {
        ss << "  [PS] Pixel Shader - Not loaded" << std::endl;
    }

    // List cached shaders
    for (const auto& entry : m_shaderCache) {
        ss << "  [Cache] " << entry.first;
        ss << (entry.second->IsValid() ? " - Valid" : " - Invalid");
        ss << std::endl;
    }

    // List variants
    for (const auto& variant : m_variants) {
        ss << "  [Variant] " << variant.name;
        ss << (variant.isCompiled ? " - Compiled" : " - Not compiled");
        if (variant.id == m_activeVariant) ss << " (ACTIVE)";
        ss << std::endl;
    }

    // Watched files
    ss << std::endl << "Watched files: " << m_watchedFiles.size() << std::endl;
    for (const auto& wf : m_watchedFiles) {
        std::string narrowPath(wf.begin(), wf.end());
        ss << "  " << narrowPath << std::endl;
    }

    return ss.str();
}

std::string Shader::Console_GetShaderInfo(const std::string& shaderName) const
{
    std::stringstream ss;
    ss << "=== Shader Info: " << shaderName << " ===" << std::endl;

    if (shaderName == "vertex" || shaderName == "vs") {
        ss << "Type: Vertex Shader" << std::endl;
        ss << "Valid: " << (m_vertexShader && m_vertexShader->IsValid() ? "Yes" : "No") << std::endl;
    } else if (shaderName == "pixel" || shaderName == "ps") {
        ss << "Type: Pixel Shader" << std::endl;
        ss << "Valid: " << (m_pixelShader && m_pixelShader->IsValid() ? "Yes" : "No") << std::endl;
    } else {
        // Check cache
        auto it = m_shaderCache.find(shaderName);
        if (it != m_shaderCache.end()) {
            ss << "Found in cache" << std::endl;
            ss << "Valid: " << (it->second->IsValid() ? "Yes" : "No") << std::endl;
        } else {
            ss << "Shader not found" << std::endl;
        }
    }

    // General metrics
    auto metrics = GetMetricsThreadSafe();
    ss << std::endl << "Compilation Stats:" << std::endl;
    ss << "  Compiled: " << metrics.compiledShaders << std::endl;
    ss << "  Failed: " << metrics.failedCompilations << std::endl;
    ss << "  Last compile time: " << metrics.lastCompileTime << " ms" << std::endl;

    if (!m_filePath.empty()) {
        ss << "File path: " << m_filePath << std::endl;
    }

    return ss.str();
}

void Shader::Console_RegisterStateCallback(std::function<void()> callback)
{
    m_stateCallback = callback;
}

int Shader::Console_ValidateShaders()
{
    int errors = 0;

    if (m_vertexShader && !m_vertexShader->IsValid()) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Vertex shader invalid", L"ERROR");
        errors++;
    }

    if (m_pixelShader && !m_pixelShader->IsValid()) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Pixel shader invalid", L"ERROR");
        errors++;
    }

    for (const auto& entry : m_shaderCache) {
        if (!entry.second->IsValid()) {
            std::wstring name(entry.first.begin(), entry.first.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Cached shader invalid: " + name, L"ERROR");
            errors++;
        }
    }

    if (errors == 0) {
        LOG_TO_CONSOLE_IMMEDIATE(L"All shaders validated successfully", L"SUCCESS");
    } else {
        std::wstring msg = L"Shader validation found " + std::to_wstring(errors) + L" error(s)";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"WARNING");
    }

    return errors;
}

void Shader::Console_ClearCache()
{
    m_shaderCache.clear();
    m_shaderVariants.clear();
    m_variants.clear();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.activeVariants = 0;
        m_metrics.shaderMemoryUsage = 0;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Shader cache cleared", L"INFO");
}

void Shader::Console_SetSearchPaths(const std::vector<std::string>& paths)
{
    m_searchPaths = paths;
    std::wstring msg = L"Shader search paths updated (" + std::to_wstring(paths.size()) + L" paths)";
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

// ============================================================================
// RHI CROSS-PLATFORM COMPILATION
// ============================================================================

static Spark::RHI::RHIShaderStage ShaderTypeToRHIStage(ShaderType type)
{
    switch (type) {
        case ShaderType::VERTEX_SHADER:   return Spark::RHI::RHIShaderStage::Vertex;
        case ShaderType::PIXEL_SHADER:    return Spark::RHI::RHIShaderStage::Pixel;
        case ShaderType::GEOMETRY_SHADER: return Spark::RHI::RHIShaderStage::Geometry;
        case ShaderType::HULL_SHADER:     return Spark::RHI::RHIShaderStage::Hull;
        case ShaderType::DOMAIN_SHADER:   return Spark::RHI::RHIShaderStage::Domain;
        case ShaderType::COMPUTE_SHADER:  return Spark::RHI::RHIShaderStage::Compute;
        default:                          return Spark::RHI::RHIShaderStage::Vertex;
    }
}

bool Shader::CompileWithRHI(const std::string& sourceFile, ShaderType type, int targetBackend)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    Spark::RHI::ShaderCompileOptions options;
    options.stage = ShaderTypeToRHIStage(type);
    options.sourceFile = sourceFile;
    options.entryPoint = m_defaultFlags.entryPoint;
    options.optimizationEnabled = m_defaultFlags.enableOptimization;
    options.debugInfoEnabled = m_defaultFlags.enableDebug;
    options.defines = m_defaultFlags.defines;
    options.includePaths = m_defaultFlags.includePaths;
    options.targetBackend = static_cast<Spark::RHI::GraphicsBackend>(targetBackend);

    // Auto-detect source language from file extension
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    auto endTime = std::chrono::high_resolution_clock::now();
    float compileTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.lastCompileTime = compileTimeMs;
        m_metrics.totalCompileTime += compileTimeMs;
    }

    if (!result.success) {
        std::wstring wError(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"RHI shader compilation failed: " + wError, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return false;
    }

    // Save compiled bytecode for later loading
    std::string outputPath = sourceFile;
    size_t dotPos = outputPath.find_last_of('.');
    if (dotPos != std::string::npos) {
        outputPath = outputPath.substr(0, dotPos);
    }

    // Determine output extension based on target
    Spark::RHI::GraphicsBackend backend = static_cast<Spark::RHI::GraphicsBackend>(targetBackend);
    const char* ext = Spark::RHI::GetShaderExtension(backend);
    if (backend == Spark::RHI::GraphicsBackend::Vulkan) {
        outputPath += ".spv";
    } else {
        outputPath += ".compiled";
    }

    Spark::RHI::SaveCompiledShader(outputPath, result.bytecode);

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
    }

    std::wstring wFile(sourceFile.begin(), sourceFile.end());
    LOG_TO_CONSOLE_IMMEDIATE(L"RHI shader compiled successfully: " + wFile, L"SUCCESS");
    return true;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// LINUX IMPLEMENTATION - Uses RHI shader compilation pipeline
// ============================================================================

#include "Shader.h"
#include "RHI/RHIFactory.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <sstream>
#include <sys/stat.h>

// Console logging integration (Linux version)
#define LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype) \
    do { \
        std::wstring wstr = wmsg; \
        std::wstring wtypestr = wtype; \
        std::string msg(wstr.begin(), wstr.end()); \
        std::string type(wtypestr.begin(), wtypestr.end()); \
        Spark::SimpleConsole::GetInstance().Log(msg, type); \
    } while(0)

// Helper: check file existence on Linux
static bool FileExistsLinux(const std::string& path) {
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

// Helper: read file contents into string
static bool ReadFileContents(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Helper: get file modification time as uint64
static uint64_t GetFileModTime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return static_cast<uint64_t>(st.st_mtime);
    }
    return 0;
}

// Helper: convert wstring to narrow string
static std::string WideToNarrow(const std::wstring& wide) {
    return std::string(wide.begin(), wide.end());
}

// Helper: convert ShaderType to RHI stage
static Spark::RHI::RHIShaderStage ShaderTypeToRHIStage(ShaderType type) {
    switch (type) {
        case ShaderType::VERTEX_SHADER:   return Spark::RHI::RHIShaderStage::Vertex;
        case ShaderType::PIXEL_SHADER:    return Spark::RHI::RHIShaderStage::Pixel;
        case ShaderType::GEOMETRY_SHADER: return Spark::RHI::RHIShaderStage::Geometry;
        case ShaderType::HULL_SHADER:     return Spark::RHI::RHIShaderStage::Hull;
        case ShaderType::DOMAIN_SHADER:   return Spark::RHI::RHIShaderStage::Domain;
        case ShaderType::COMPUTE_SHADER:  return Spark::RHI::RHIShaderStage::Compute;
        default:                          return Spark::RHI::RHIShaderStage::Vertex;
    }
}

// ============================================================================
// SHADER RESOURCE IMPLEMENTATIONS (no-ops on Linux - no D3D11 context)
// ============================================================================

void VertexShaderResource::Bind(ID3D11DeviceContext* /*context*/) {
    // No-op on Linux: ID3D11DeviceContext is a stub
}

void VertexShaderResource::Unbind(ID3D11DeviceContext* /*context*/) {
    // No-op on Linux
}

void PixelShaderResource::Bind(ID3D11DeviceContext* /*context*/) {
    // No-op on Linux
}

void PixelShaderResource::Unbind(ID3D11DeviceContext* /*context*/) {
    // No-op on Linux
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

Shader::Shader()
    : m_device(nullptr)
    , m_context(nullptr)
    , m_activeVariant(0)
    , m_hotReloadEnabled(false)
    , m_validationEnabled(false)
    , m_type(ShaderType::VERTEX_SHADER)
    , m_isCompiled(false)
    , m_shader(nullptr)
{
    m_lastModified.dwLowDateTime = 0;
    m_lastModified.dwHighDateTime = 0;
}

Shader::~Shader() {
    Shutdown();
}

// ============================================================================
// INITIALIZATION AND SHUTDOWN
// ============================================================================

HRESULT Shader::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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
    if (FAILED(hr)) {
        return hr;
    }

    return S_OK;
}

void Shader::Shutdown() {
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

HRESULT Shader::CreateConstantBuffers() {
    // On Linux, D3D11 buffers are not created. Constant buffer data is stored
    // in-memory and forwarded to the RHI backend when available.
    // The ComPtr<ID3D11Buffer> members remain null (stubs).
    return S_OK;
}

// ============================================================================
// SHADER LOADING - uses RHI compilation pipeline
// ============================================================================

HRESULT Shader::LoadVertexShader(const std::wstring& filename, const ShaderCompilationFlags& flags) {
    std::string narrowPath = WideToNarrow(filename);

    if (!FileExistsLinux(narrowPath)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader file not found: " + filename, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Add to watched files for hot reload
    if (std::find(m_watchedFiles.begin(), m_watchedFiles.end(), filename) == m_watchedFiles.end()) {
        m_watchedFiles.push_back(filename);
    }

    // Read source code
    std::string sourceCode;
    if (!ReadFileContents(narrowPath, sourceCode)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to read vertex shader: " + filename, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Compile through RHI
    auto startTime = std::chrono::high_resolution_clock::now();

    Spark::RHI::ShaderCompileOptions options;
    options.stage = Spark::RHI::RHIShaderStage::Vertex;
    options.sourceFile = narrowPath;
    options.sourceCode = sourceCode;
    options.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
    options.optimizationEnabled = flags.enableOptimization;
    options.debugInfoEnabled = flags.enableDebug;
    options.defines = flags.defines;
    options.includePaths = flags.includePaths;
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    auto endTime = std::chrono::high_resolution_clock::now();
    float compileTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.lastCompileTime = compileTimeMs;
        m_metrics.totalCompileTime += compileTimeMs;
    }

    if (!result.success) {
        std::wstring wError(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compilation failed: " + wError, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
        m_metrics.shaderMemoryUsage += result.bytecode.size();
    }

    m_isCompiled = true;
    m_type = ShaderType::VERTEX_SHADER;
    m_filePath = narrowPath;

    LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compiled (RHI): " + filename, L"SUCCESS");
    NotifyStateChange();
    return S_OK;
}

HRESULT Shader::LoadPixelShader(const std::wstring& filename, const ShaderCompilationFlags& flags) {
    std::string narrowPath = WideToNarrow(filename);

    if (!FileExistsLinux(narrowPath)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader file not found: " + filename, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Add to watched files for hot reload
    if (std::find(m_watchedFiles.begin(), m_watchedFiles.end(), filename) == m_watchedFiles.end()) {
        m_watchedFiles.push_back(filename);
    }

    // Read source code
    std::string sourceCode;
    if (!ReadFileContents(narrowPath, sourceCode)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to read pixel shader: " + filename, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Compile through RHI
    auto startTime = std::chrono::high_resolution_clock::now();

    Spark::RHI::ShaderCompileOptions options;
    options.stage = Spark::RHI::RHIShaderStage::Pixel;
    options.sourceFile = narrowPath;
    options.sourceCode = sourceCode;
    options.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
    options.optimizationEnabled = flags.enableOptimization;
    options.debugInfoEnabled = flags.enableDebug;
    options.defines = flags.defines;
    options.includePaths = flags.includePaths;
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    auto endTime = std::chrono::high_resolution_clock::now();
    float compileTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.lastCompileTime = compileTimeMs;
        m_metrics.totalCompileTime += compileTimeMs;
    }

    if (!result.success) {
        std::wstring wError(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compilation failed: " + wError, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
        m_metrics.shaderMemoryUsage += result.bytecode.size();
    }

    m_isCompiled = true;
    m_filePath = narrowPath;

    LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compiled (RHI): " + filename, L"SUCCESS");
    NotifyStateChange();
    return S_OK;
}

HRESULT Shader::LoadShaderFromSource(const std::string& source, ShaderType type, const ShaderCompilationFlags& flags) {
    auto startTime = std::chrono::high_resolution_clock::now();

    Spark::RHI::ShaderCompileOptions options;
    options.stage = ShaderTypeToRHIStage(type);
    options.sourceCode = source;
    options.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
    options.optimizationEnabled = flags.enableOptimization;
    options.debugInfoEnabled = flags.enableDebug;
    options.defines = flags.defines;
    options.includePaths = flags.includePaths;
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    auto endTime = std::chrono::high_resolution_clock::now();
    float compileTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.lastCompileTime = compileTimeMs;
        m_metrics.totalCompileTime += compileTimeMs;
    }

    if (!result.success) {
        std::wstring wError(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"Shader source compilation failed: " + wError, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
        m_metrics.shaderMemoryUsage += result.bytecode.size();
    }

    m_isCompiled = true;
    m_type = type;

    NotifyStateChange();
    return S_OK;
}

HRESULT Shader::LoadFromFile(const std::string& filePath, ShaderType type, const ShaderCompilationFlags& flags) {
    if (!FileExistsLinux(filePath)) {
        // Try search paths
        for (const auto& searchPath : m_searchPaths) {
            std::string fullPath = searchPath + "/" + filePath;
            if (FileExistsLinux(fullPath)) {
                return LoadFromFile(fullPath, type, flags);
            }
        }
        std::wstring wPath(filePath.begin(), filePath.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"Shader file not found: " + wPath, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    std::string sourceCode;
    if (!ReadFileContents(filePath, sourceCode)) {
        std::wstring wPath(filePath.begin(), filePath.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to read shader file: " + wPath, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Track file for hot reload
    std::wstring widePath(filePath.begin(), filePath.end());
    if (std::find(m_watchedFiles.begin(), m_watchedFiles.end(), widePath) == m_watchedFiles.end()) {
        m_watchedFiles.push_back(widePath);
    }

    m_filePath = filePath;
    m_type = type;

    // Store modification time for hot reload
    uint64_t modTime = GetFileModTime(filePath);
    m_lastModified.dwLowDateTime = static_cast<uint32_t>(modTime & 0xFFFFFFFF);
    m_lastModified.dwHighDateTime = static_cast<uint32_t>((modTime >> 32) & 0xFFFFFFFF);

    return LoadShaderFromSource(sourceCode, type, flags);
}

// ============================================================================
// SHADER VARIANTS
// ============================================================================

int Shader::CreateShaderVariant(const std::string& baseName, const std::vector<std::string>& defines) {
    ShaderVariant variant;
    variant.id = static_cast<int>(m_variants.size());
    variant.baseName = baseName;
    variant.name = baseName;
    variant.defines = defines;
    variant.isCompiled = false;
    variant.lastModified.dwLowDateTime = 0;
    variant.lastModified.dwHighDateTime = 0;

    // Build variant name from defines
    for (const auto& def : defines) {
        variant.name += "_" + def;
    }

    // Attempt to compile the variant through RHI
    ShaderCompilationFlags flags = m_defaultFlags;
    flags.defines.insert(flags.defines.end(), defines.begin(), defines.end());

    // Try to find and compile the base shader file
    std::string sourceFile = baseName;
    if (FileExistsLinux(sourceFile)) {
        std::string sourceCode;
        if (ReadFileContents(sourceFile, sourceCode)) {
            Spark::RHI::ShaderCompileOptions options;
            options.stage = ShaderTypeToRHIStage(m_type);
            options.sourceFile = sourceFile;
            options.sourceCode = sourceCode;
            options.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
            options.optimizationEnabled = flags.enableOptimization;
            options.debugInfoEnabled = flags.enableDebug;
            options.defines = flags.defines;
            options.includePaths = flags.includePaths;
            options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
            options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
            options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

            Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);
            variant.isCompiled = result.success;
        }
    }

    m_variants.push_back(variant);

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.activeVariants = static_cast<int>(m_variants.size());
    }

    NotifyStateChange();
    return variant.id;
}

void Shader::SetActiveVariant(int variantId) {
    if (variantId >= 0 && variantId < static_cast<int>(m_variants.size())) {
        m_activeVariant = variantId;
        NotifyStateChange();
    }
}

// ============================================================================
// HOT RELOAD
// ============================================================================

int Shader::HotReloadShaders() {
    int reloadCount = 0;

    for (const auto& watchedFile : m_watchedFiles) {
        std::string narrowPath = WideToNarrow(watchedFile);
        uint64_t currentModTime = GetFileModTime(narrowPath);
        uint64_t storedModTime = static_cast<uint64_t>(m_lastModified.dwLowDateTime) |
                                 (static_cast<uint64_t>(m_lastModified.dwHighDateTime) << 32);

        if (currentModTime > storedModTime && currentModTime != 0) {
            HRESULT hr = LoadFromFile(narrowPath, m_type, m_defaultFlags);
            if (SUCCEEDED(hr)) {
                reloadCount++;
            }
        }
    }

    if (reloadCount > 0) {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.hotReloadCount += reloadCount;
    }

    return reloadCount;
}

// ============================================================================
// SHADER BINDING (no-ops on Linux - uses RHI pipeline instead)
// ============================================================================

void Shader::SetShaders() {
    // On Linux, shader binding is handled through the RHI pipeline.
    // The D3D11 context is null, so there is nothing to bind here.
    // The compiled RHI bytecode will be used by the active RHI device.
}

void Shader::UnbindShaders() {
    // No-op on Linux; RHI handles unbinding
}

// ============================================================================
// STATE QUERIES
// ============================================================================

bool Shader::IsValid() const {
    return m_isCompiled;
}

// ============================================================================
// CONSTANT BUFFER UPDATES (store internally for RHI use)
// ============================================================================

void Shader::UpdatePerFrameConstants(const PerFrameConstants& constants) {
    // On Linux, store the data internally. The RHI backend will
    // consume it when rendering. No D3D11 buffer update occurs.
    (void)constants;
}

void Shader::UpdatePerObjectConstants(const PerObjectConstants& constants) {
    (void)constants;
}

void Shader::UpdatePerMaterialConstants(const PerMaterialConstants& constants) {
    (void)constants;
}

void Shader::UpdateLightingData(const LightingData& lightingData) {
    (void)lightingData;
}

void Shader::UpdatePostProcessingConstants(const PostProcessingConstants& constants) {
    (void)constants;
}

void Shader::UpdateConstantBuffer(const ConstantBuffer& cb) {
    (void)cb;
}

// ============================================================================
// CONSOLE INTEGRATION METHODS
// ============================================================================

Shader::ShaderMetrics Shader::Console_GetMetrics() const {
    return GetMetricsThreadSafe();
}

void Shader::Console_RecompileAll() {
    LOG_TO_CONSOLE_IMMEDIATE(L"Recompiling all shaders...", L"INFO");

    for (const auto& watchedFile : m_watchedFiles) {
        std::string narrowPath = WideToNarrow(watchedFile);
        LoadFromFile(narrowPath, m_type, m_defaultFlags);
    }

    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Shader recompilation complete", L"SUCCESS");
}

void Shader::Console_SetHotReload(bool enabled) {
    m_hotReloadEnabled = enabled;
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.hotReloadEnabled = enabled;
    }
    std::wstring msg = enabled ? L"Hot reload enabled" : L"Hot reload disabled";
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

void Shader::Console_SetCompilationFlags(bool enableDebug, bool enableOptimization) {
    m_defaultFlags.enableDebug = enableDebug;
    m_defaultFlags.enableOptimization = enableOptimization;

    std::wstring msg = L"Compilation flags updated: debug=" +
        std::to_wstring(enableDebug) + L", optimization=" + std::to_wstring(enableOptimization);
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

std::string Shader::Console_ListShaders() const {
    std::stringstream ss;
    ss << "=== Loaded Shaders ===" << std::endl;

    ss << "  [VS] Vertex Shader - " << (m_isCompiled && m_type == ShaderType::VERTEX_SHADER ? "Active (RHI)" : "Not loaded") << std::endl;
    ss << "  [PS] Pixel Shader - " << (m_isCompiled && m_type == ShaderType::PIXEL_SHADER ? "Active (RHI)" : "Not loaded") << std::endl;

    // List cached shaders
    for (const auto& entry : m_shaderCache) {
        ss << "  [Cache] " << entry.first;
        ss << (entry.second->IsValid() ? " - Valid" : " - Invalid");
        ss << std::endl;
    }

    // List variants
    for (const auto& variant : m_variants) {
        ss << "  [Variant] " << variant.name;
        ss << (variant.isCompiled ? " - Compiled" : " - Not compiled");
        if (variant.id == m_activeVariant) ss << " (ACTIVE)";
        ss << std::endl;
    }

    // Watched files
    ss << std::endl << "Watched files: " << m_watchedFiles.size() << std::endl;
    for (const auto& wf : m_watchedFiles) {
        std::string narrowPath = WideToNarrow(wf);
        ss << "  " << narrowPath << std::endl;
    }

    return ss.str();
}

std::string Shader::Console_GetShaderInfo(const std::string& shaderName) const {
    std::stringstream ss;
    ss << "=== Shader Info: " << shaderName << " ===" << std::endl;

    if (shaderName == "vertex" || shaderName == "vs") {
        ss << "Type: Vertex Shader" << std::endl;
        ss << "Valid: " << (m_isCompiled && m_type == ShaderType::VERTEX_SHADER ? "Yes" : "No") << std::endl;
        ss << "Backend: RHI (Linux)" << std::endl;
    } else if (shaderName == "pixel" || shaderName == "ps") {
        ss << "Type: Pixel Shader" << std::endl;
        ss << "Valid: " << (m_isCompiled && m_type == ShaderType::PIXEL_SHADER ? "Yes" : "No") << std::endl;
        ss << "Backend: RHI (Linux)" << std::endl;
    } else {
        auto it = m_shaderCache.find(shaderName);
        if (it != m_shaderCache.end()) {
            ss << "Found in cache" << std::endl;
            ss << "Valid: " << (it->second->IsValid() ? "Yes" : "No") << std::endl;
        } else {
            ss << "Shader not found" << std::endl;
        }
    }

    // General metrics
    auto metrics = GetMetricsThreadSafe();
    ss << std::endl << "Compilation Stats:" << std::endl;
    ss << "  Compiled: " << metrics.compiledShaders << std::endl;
    ss << "  Failed: " << metrics.failedCompilations << std::endl;
    ss << "  Last compile time: " << metrics.lastCompileTime << " ms" << std::endl;

    if (!m_filePath.empty()) {
        ss << "File path: " << m_filePath << std::endl;
    }

    return ss.str();
}

void Shader::Console_RegisterStateCallback(std::function<void()> callback) {
    m_stateCallback = callback;
}

int Shader::Console_ValidateShaders() {
    int errors = 0;

    if (!m_isCompiled && !m_filePath.empty()) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Shader not compiled", L"ERROR");
        errors++;
    }

    for (const auto& entry : m_shaderCache) {
        if (!entry.second->IsValid()) {
            std::wstring name(entry.first.begin(), entry.first.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Cached shader invalid: " + name, L"ERROR");
            errors++;
        }
    }

    for (const auto& variant : m_variants) {
        if (!variant.isCompiled) {
            std::wstring name(variant.name.begin(), variant.name.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Variant not compiled: " + name, L"ERROR");
            errors++;
        }
    }

    if (errors == 0) {
        LOG_TO_CONSOLE_IMMEDIATE(L"All shaders validated successfully", L"SUCCESS");
    } else {
        std::wstring msg = L"Shader validation found " + std::to_wstring(errors) + L" error(s)";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"WARNING");
    }

    return errors;
}

void Shader::Console_ClearCache() {
    m_shaderCache.clear();
    m_shaderVariants.clear();
    m_variants.clear();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.activeVariants = 0;
        m_metrics.shaderMemoryUsage = 0;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Shader cache cleared", L"INFO");
}

void Shader::Console_SetSearchPaths(const std::vector<std::string>& paths) {
    m_searchPaths = paths;
    std::wstring msg = L"Shader search paths updated (" + std::to_wstring(paths.size()) + L" paths)";
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

// ============================================================================
// RHI CROSS-PLATFORM COMPILATION
// ============================================================================

bool Shader::CompileWithRHI(const std::string& sourceFile, ShaderType type, int targetBackend) {
    auto startTime = std::chrono::high_resolution_clock::now();

    Spark::RHI::ShaderCompileOptions options;
    options.stage = ShaderTypeToRHIStage(type);
    options.sourceFile = sourceFile;
    options.entryPoint = m_defaultFlags.entryPoint;
    options.optimizationEnabled = m_defaultFlags.enableOptimization;
    options.debugInfoEnabled = m_defaultFlags.enableDebug;
    options.defines = m_defaultFlags.defines;
    options.includePaths = m_defaultFlags.includePaths;
    options.targetBackend = static_cast<Spark::RHI::GraphicsBackend>(targetBackend);

    // Read source if file exists
    std::string sourceCode;
    if (FileExistsLinux(sourceFile) && ReadFileContents(sourceFile, sourceCode)) {
        options.sourceCode = sourceCode;
    }

    // Auto-detect source language from file extension
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    auto endTime = std::chrono::high_resolution_clock::now();
    float compileTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.lastCompileTime = compileTimeMs;
        m_metrics.totalCompileTime += compileTimeMs;
    }

    if (!result.success) {
        std::wstring wError(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"RHI shader compilation failed: " + wError, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return false;
    }

    // Save compiled bytecode for later loading
    std::string outputPath = sourceFile;
    size_t dotPos = outputPath.find_last_of('.');
    if (dotPos != std::string::npos) {
        outputPath = outputPath.substr(0, dotPos);
    }

    // Determine output extension based on target
    Spark::RHI::GraphicsBackend backend = static_cast<Spark::RHI::GraphicsBackend>(targetBackend);
    if (backend == Spark::RHI::GraphicsBackend::Vulkan) {
        outputPath += ".spv";
    } else {
        outputPath += ".compiled";
    }

    Spark::RHI::SaveCompiledShader(outputPath, result.bytecode);

    m_isCompiled = true;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
        m_metrics.shaderMemoryUsage += result.bytecode.size();
    }

    std::wstring wFile(sourceFile.begin(), sourceFile.end());
    LOG_TO_CONSOLE_IMMEDIATE(L"RHI shader compiled successfully: " + wFile, L"SUCCESS");
    return true;
}

// ============================================================================
// STATIC COMPILATION UTILITY (LEGACY)
// ============================================================================

HRESULT Shader::CompileShaderFromFile(const std::wstring& filename,
    const std::string& entryPoint,
    const std::string& shaderModel,
    ID3DBlob** blobOut)
{
    // On Linux, D3DCompileFromFile is not available. Use RHI compilation instead.
    std::string narrowPath = WideToNarrow(filename);

    if (!FileExistsLinux(narrowPath)) {
        return E_FAIL;
    }

    std::string sourceCode;
    if (!ReadFileContents(narrowPath, sourceCode)) {
        return E_FAIL;
    }

    // Determine shader stage from shader model string
    Spark::RHI::RHIShaderStage stage = Spark::RHI::RHIShaderStage::Vertex;
    if (shaderModel.find("ps_") == 0) {
        stage = Spark::RHI::RHIShaderStage::Pixel;
    } else if (shaderModel.find("gs_") == 0) {
        stage = Spark::RHI::RHIShaderStage::Geometry;
    } else if (shaderModel.find("hs_") == 0) {
        stage = Spark::RHI::RHIShaderStage::Hull;
    } else if (shaderModel.find("ds_") == 0) {
        stage = Spark::RHI::RHIShaderStage::Domain;
    } else if (shaderModel.find("cs_") == 0) {
        stage = Spark::RHI::RHIShaderStage::Compute;
    }

    Spark::RHI::ShaderCompileOptions options;
    options.stage = stage;
    options.sourceFile = narrowPath;
    options.sourceCode = sourceCode;
    options.entryPoint = entryPoint;
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    if (!result.success) {
        return E_FAIL;
    }

    // blobOut remains null on Linux (ID3DBlob is a stub type)
    if (blobOut) {
        *blobOut = nullptr;
    }

    return S_OK;
}

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================

HRESULT Shader::CompileShaderFromFileAdvanced(const std::wstring& filename,
    ShaderType type,
    const ShaderCompilationFlags& flags,
    ID3DBlob** blobOut)
{
    // Delegate to the RHI pipeline on Linux
    std::string narrowPath = WideToNarrow(filename);

    std::string sourceCode;
    if (!FileExistsLinux(narrowPath) || !ReadFileContents(narrowPath, sourceCode)) {
        return E_FAIL;
    }

    Spark::RHI::ShaderCompileOptions options;
    options.stage = ShaderTypeToRHIStage(type);
    options.sourceFile = narrowPath;
    options.sourceCode = sourceCode;
    options.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
    options.optimizationEnabled = flags.enableOptimization;
    options.debugInfoEnabled = flags.enableDebug;
    options.defines = flags.defines;
    options.includePaths = flags.includePaths;
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    if (blobOut) {
        *blobOut = nullptr; // ID3DBlob is a stub on Linux
    }

    return result.success ? S_OK : E_FAIL;
}

HRESULT Shader::CreateInputLayout(ID3DBlob* /*vertexShaderBlob*/, ID3D11InputLayout** inputLayout) {
    // No-op on Linux: input layout is a D3D11 concept.
    // The RHI backend handles vertex input configuration.
    if (inputLayout) {
        *inputLayout = nullptr;
    }
    return S_OK;
}

void Shader::UpdateFileMonitoring() {
    if (m_hotReloadEnabled) {
        HotReloadShaders();
    }
}

void Shader::NotifyStateChange() {
    if (m_stateCallback) {
        m_stateCallback();
    }
}

Shader::ShaderMetrics Shader::GetMetricsThreadSafe() const {
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

#endif // SPARK_PLATFORM_WINDOWS
