/**
 * @file ShaderCompilationWindowsCompileOps.cpp
 * @brief Windows/D3D11 compile utilities — split from ShaderCompilationWindows.cpp
 *
 * Advanced file-based HLSL compilation, input layout creation, the legacy
 * static compilation utility, and the RHI cross-platform compilation path.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "Shader.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include <d3dcompiler.h>
#include <windows.h>
#include "RHI/RHIFactory.h"
#include "RHI/RHITypes.h"
#include <chrono>
#include <mutex>
#include <string>

// Helper function for C++14 compatible file existence check
bool FileExists(const std::wstring& filename)
{
    DWORD attrs = GetFileAttributesW(filename.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

// Console logging — use centralized macros from LogMacros.h
#include "../Utils/LogMacros.h"

// Helper: convert ShaderType to RHI shader stage
static Spark::RHI::RHIShaderStage ShaderTypeToRHIStage(ShaderType type)
{
    switch (type)
    {
    case ShaderType::VERTEX_SHADER:
        return Spark::RHI::RHIShaderStage::Vertex;
    case ShaderType::PIXEL_SHADER:
        return Spark::RHI::RHIShaderStage::Pixel;
    case ShaderType::GEOMETRY_SHADER:
        return Spark::RHI::RHIShaderStage::Geometry;
    case ShaderType::HULL_SHADER:
        return Spark::RHI::RHIShaderStage::Hull;
    case ShaderType::DOMAIN_SHADER:
        return Spark::RHI::RHIShaderStage::Domain;
    case ShaderType::COMPUTE_SHADER:
        return Spark::RHI::RHIShaderStage::Compute;
    default:
        return Spark::RHI::RHIShaderStage::Vertex;
    }
}

// ============================================================================
// ADVANCED COMPILATION
// ============================================================================

HRESULT Shader::CompileShaderFromFileAdvanced(const std::wstring& filename, ShaderType type,
                                              const ShaderCompilationFlags& flags, ID3DBlob** shaderBlob)
{
    ASSERT(!filename.empty());
    ASSERT(shaderBlob != nullptr);
    if (!shaderBlob)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "CompileShaderFromFileAdvanced: null output blob pointer");
        return E_INVALIDARG;
    }

    std::wstring fullPath = filename;
    bool fileFound = false;

    // Try to find the file in search paths
    if (!FileExists(fullPath))
    {
        for (const auto& searchPath : m_searchPaths)
        {
            std::wstring testPath = std::wstring(searchPath.begin(), searchPath.end()) + filename;
            if (FileExists(testPath))
            {
                fullPath = testPath;
                fileFound = true;
                break;
            }
        }
    }
    else
    {
        fileFound = true;
    }

    if (!fileFound)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Shader file not found in any search path");
        LOG_TO_CONSOLE_IMMEDIATE(L"Shader file not found: " + filename, L"ERROR");
        return E_FAIL;
    }

    // Determine shader target based on type
    std::string target;
    switch (type)
    {
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
    if (flags.enableDebug)
    {
        compileFlags |= D3DCOMPILE_DEBUG;
        compileFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
    }
    if (flags.enableOptimization)
    {
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }
    if (flags.treatWarningsAsErrors)
    {
        compileFlags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
    }

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(fullPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    flags.entryPoint.c_str(), target.c_str(), compileFlags, 0, shaderBlob, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Shader compilation error: %s", errorString.c_str());
            std::wstring wErrorString(errorString.begin(), errorString.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader compilation error: " + wErrorString, L"ERROR");
        }

        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader compiled successfully (target=%s)", target.c_str());
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
    }

    return hr;
}

// ============================================================================
// INPUT LAYOUT CREATION
// ============================================================================

HRESULT Shader::CreateInputLayout(ID3DBlob* vertexShaderBlob, ID3D11InputLayout** inputLayout)
{
    // **FIXED: Input layout to match source shader VS_INPUT structure**
    // The source BasicVS.hlsl expects: POSITION, NORMAL, TEXCOORD0
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0}};

    HRESULT hr = m_device->CreateInputLayout(layoutDesc, ARRAYSIZE(layoutDesc), vertexShaderBlob->GetBufferPointer(),
                                             vertexShaderBlob->GetBufferSize(), inputLayout);

    if (FAILED(hr))
    {
        // **FALLBACK: Try with FLOAT3 positions (our Vertex structure)**
        D3D11_INPUT_ELEMENT_DESC fallbackLayoutDesc[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};

        hr = m_device->CreateInputLayout(fallbackLayoutDesc, ARRAYSIZE(fallbackLayoutDesc),
                                         vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize(),
                                         inputLayout);
    }

    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create input layout with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Input layout created successfully", L"SUCCESS");
    }

    return hr;
}

// ============================================================================
// STATIC COMPILATION UTILITY (LEGACY)
// ============================================================================

HRESULT Shader::CompileShaderFromFile(const std::wstring& filename, const std::string& entryPoint,
                                      const std::string& shaderModel, ID3DBlob** blobOut)
{
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(filename.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint.c_str(),
                                    shaderModel.c_str(), compileFlags, 0, blobOut, &errorBlob);

    if (FAILED(hr) && errorBlob)
    {
        std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
        OutputDebugStringA(errorString.c_str());
    }

    return hr;
}

// ============================================================================
// RHI CROSS-PLATFORM COMPILATION
// ============================================================================

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

    if (!result.success)
    {
        std::wstring wError(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"RHI shader compilation failed: " + wError, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return false;
    }

    // Save compiled bytecode for later loading
    std::string outputPath = sourceFile;
    size_t dotPos = outputPath.find_last_of('.');
    if (dotPos != std::string::npos)
    {
        outputPath = outputPath.substr(0, dotPos);
    }

    // Determine output extension based on target
    Spark::RHI::GraphicsBackend backend = static_cast<Spark::RHI::GraphicsBackend>(targetBackend);
    const char* ext = Spark::RHI::GetShaderExtension(backend);
    if (backend == Spark::RHI::GraphicsBackend::Vulkan)
    {
        outputPath += ".spv";
    }
    else
    {
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

#endif // SPARK_PLATFORM_WINDOWS
