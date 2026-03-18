#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file ShaderCompilation.cpp
 * @brief D3D11 shader compilation, input layout creation, and RHI cross-compilation
 *
 * Handles all HLSL compilation via D3DCompile/D3DCompileFromFile, automatic
 * input layout generation based on shader reflection, source and file-based
 * shader loading, and RHI cross-platform compilation paths.
 * Split from Shader.cpp for maintainability.
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
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

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
// SHADER LOADING — VERTEX AND PIXEL
// ============================================================================

HRESULT Shader::LoadVertexShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, !filename.empty(), "LoadVertexShader: filename empty");
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, m_device);

    auto startTime = std::chrono::high_resolution_clock::now();

    ComPtr<ID3DBlob> vsBlob;

    // **FIX: Explicitly specify vertex shader compilation**
    ShaderCompilationFlags vsFlags = flags;
    vsFlags.target = "vs_5_0"; // Force vertex shader target

    HRESULT hr = CompileShaderFromFileAdvanced(filename, ShaderType::VERTEX_SHADER, vsFlags, &vsBlob);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compilation failed, trying fallback method", L"WARNING");

        // **FALLBACK: Try direct D3DCompileFromFile**
        ComPtr<ID3DBlob> errorBlob;
        hr = D3DCompileFromFile(filename.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob)
            {
                std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
                std::wstring wErrorString(errorString.begin(), errorString.end());
                LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compilation error: " + wErrorString, L"ERROR");
            }
            return hr;
        }
    }

    m_vertexShader = std::make_unique<VertexShaderResource>();
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                      m_vertexShader->m_vertexShader.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateVertexShader failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    m_vertexShader->m_shaderBlob = vsBlob;

    hr = CreateInputLayout(vsBlob.Get(), m_vertexShader->m_inputLayout.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader and input layout loaded successfully", L"SUCCESS");
    }
    else
    {
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
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compilation failed, trying fallback method", L"WARNING");

        // **FALLBACK: Try direct D3DCompileFromFile**
        ComPtr<ID3DBlob> errorBlob;
        hr = D3DCompileFromFile(filename.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob)
            {
                std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
                std::wstring wErrorString(errorString.begin(), errorString.end());
                LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compilation error: " + wErrorString, L"ERROR");
            }
            return hr;
        }
    }

    m_pixelShader = std::make_unique<PixelShaderResource>();
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                     m_pixelShader->m_pixelShader.GetAddressOf());

    if (SUCCEEDED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader loaded successfully", L"SUCCESS");
    }
    else
    {
        std::wstring errorMsg = L"CreatePixelShader failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
    }

    return hr;
}

// ============================================================================
// ADVANCED COMPILATION
// ============================================================================

HRESULT Shader::CompileShaderFromFileAdvanced(const std::wstring& filename, ShaderType type,
                                              const ShaderCompilationFlags& flags, ID3DBlob** shaderBlob)
{
    ASSERT(!filename.empty());
    ASSERT(shaderBlob != nullptr);

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
            std::wstring wErrorString(errorString.begin(), errorString.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader compilation error: " + wErrorString, L"ERROR");
        }

        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
    }
    else
    {
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
// SHADER LOADING FROM SOURCE AND FILE
// ============================================================================

HRESULT Shader::LoadShaderFromSource(const std::string& source, ShaderType type, const ShaderCompilationFlags& flags)
{
    ASSERT_MSG(!source.empty(), "LoadShaderFromSource: source is empty");
    ASSERT_MSG(m_device != nullptr, "LoadShaderFromSource: device is null");

    auto startTime = std::chrono::high_resolution_clock::now();

    // Determine shader target
    std::string target = flags.target;
    if (target.empty())
    {
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
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
    if (flags.enableDebug)
    {
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }
    if (flags.enableOptimization)
    {
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }
    if (flags.treatWarningsAsErrors)
    {
        compileFlags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
    }

    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(source.c_str(), source.size(), nullptr, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                            flags.entryPoint.c_str(), target.c_str(), compileFlags, 0, &shaderBlob, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorString(errorString.begin(), errorString.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader source compilation error: " + wErrorString, L"ERROR");
        }
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return hr;
    }

    // Create the appropriate shader object
    switch (type)
    {
    case ShaderType::VERTEX_SHADER:
    {
        m_vertexShader = std::make_unique<VertexShaderResource>();
        hr = m_device->CreateVertexShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                          m_vertexShader->m_vertexShader.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            m_vertexShader->m_shaderBlob = shaderBlob;
            hr = CreateInputLayout(shaderBlob.Get(), m_vertexShader->m_inputLayout.GetAddressOf());
        }
        break;
    }
    case ShaderType::PIXEL_SHADER:
    {
        m_pixelShader = std::make_unique<PixelShaderResource>();
        hr = m_device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                         m_pixelShader->m_pixelShader.GetAddressOf());
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

    if (SUCCEEDED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Shader compiled from source successfully", L"SUCCESS");
        m_isCompiled = true;
    }

    return hr;
}

HRESULT Shader::LoadFromFile(const std::string& filePath, ShaderType type, const ShaderCompilationFlags& flags)
{
    ASSERT_MSG(!filePath.empty(), "LoadFromFile: filePath is empty");

    // Try reading via LocalFileCache first, fall back to direct I/O
    std::string source;
    if (m_fileCache)
    {
        auto result = m_fileCache->ReadText(filePath);
        if (result.IsOk())
        {
            source = result.Value();
            m_filePath = filePath;
        }
        else
        {
            // Try search paths via cache
            for (const auto& searchPath : m_searchPaths)
            {
                std::string fullPath = searchPath + filePath;
                auto searchResult = m_fileCache->ReadText(fullPath);
                if (searchResult.IsOk())
                {
                    source = searchResult.Value();
                    m_filePath = fullPath;
                    break;
                }
            }
        }
    }

    if (source.empty())
    {
        // Fallback: direct ifstream read
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            for (const auto& searchPath : m_searchPaths)
            {
                std::string fullPath = searchPath + filePath;
                file.open(fullPath);
                if (file.is_open())
                {
                    m_filePath = fullPath;
                    break;
                }
            }
            if (!file.is_open())
            {
                std::wstring wPath(filePath.begin(), filePath.end());
                LOG_TO_CONSOLE_IMMEDIATE(L"Shader file not found: " + wPath, L"ERROR");
                return E_FAIL;
            }
        }
        else
        {
            m_filePath = filePath;
        }

        source.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
    }

    m_type = type;

    // Get file modification time for hot reload
    std::wstring wFilePath(m_filePath.begin(), m_filePath.end());
    HANDLE hFile = CreateFileW(wFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        GetFileTime(hFile, nullptr, nullptr, &m_lastModified);
        CloseHandle(hFile);
    }

    // Add to watched files for hot reload
    m_watchedFiles.push_back(wFilePath);

    return LoadShaderFromSource(source, type, flags);
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

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// LINUX IMPLEMENTATION — Shader compilation via RHI pipeline
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

// Helper: check file existence on Linux
static bool FileExistsLinux(const std::string& path)
{
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

// Helper: read file contents into string
static bool ReadFileContents(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Helper: get file modification time as uint64
static uint64_t GetFileModTime(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        return static_cast<uint64_t>(st.st_mtime);
    }
    return 0;
}

// Helper: convert wstring to narrow string
static std::string WideToNarrow(const std::wstring& wide)
{
    return std::string(wide.begin(), wide.end());
}

// Helper: convert ShaderType to RHI stage
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
// SHADER LOADING — VERTEX AND PIXEL (Linux RHI)
// ============================================================================

HRESULT Shader::LoadVertexShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    std::string narrowPath = WideToNarrow(filename);

    if (!FileExistsLinux(narrowPath))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader file not found: " + filename, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Add to watched files for hot reload
    if (std::find(m_watchedFiles.begin(), m_watchedFiles.end(), filename) == m_watchedFiles.end())
    {
        m_watchedFiles.push_back(filename);
    }

    // Read source code
    std::string sourceCode;
    if (!ReadFileContents(narrowPath, sourceCode))
    {
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

    if (!result.success)
    {
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

HRESULT Shader::LoadPixelShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    std::string narrowPath = WideToNarrow(filename);

    if (!FileExistsLinux(narrowPath))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader file not found: " + filename, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Add to watched files for hot reload
    if (std::find(m_watchedFiles.begin(), m_watchedFiles.end(), filename) == m_watchedFiles.end())
    {
        m_watchedFiles.push_back(filename);
    }

    // Read source code
    std::string sourceCode;
    if (!ReadFileContents(narrowPath, sourceCode))
    {
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

    if (!result.success)
    {
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

HRESULT Shader::LoadShaderFromSource(const std::string& source, ShaderType type, const ShaderCompilationFlags& flags)
{
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

    if (!result.success)
    {
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

HRESULT Shader::LoadFromFile(const std::string& filePath, ShaderType type, const ShaderCompilationFlags& flags)
{
    if (!FileExistsLinux(filePath))
    {
        // Try search paths
        for (const auto& searchPath : m_searchPaths)
        {
            std::string fullPath = searchPath + "/" + filePath;
            if (FileExistsLinux(fullPath))
            {
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
    bool readOk = false;
    if (m_fileCache)
    {
        auto cacheResult = m_fileCache->ReadText(filePath);
        if (cacheResult.IsOk())
        {
            sourceCode = cacheResult.Value();
            readOk = true;
        }
    }
    if (!readOk)
    {
        readOk = ReadFileContents(filePath, sourceCode);
    }
    if (!readOk)
    {
        std::wstring wPath(filePath.begin(), filePath.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to read shader file: " + wPath, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return E_FAIL;
    }

    // Track file for hot reload
    std::wstring widePath(filePath.begin(), filePath.end());
    if (std::find(m_watchedFiles.begin(), m_watchedFiles.end(), widePath) == m_watchedFiles.end())
    {
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
// STATIC COMPILATION UTILITY (LEGACY — Linux)
// ============================================================================

HRESULT Shader::CompileShaderFromFile(const std::wstring& filename, const std::string& entryPoint,
                                      const std::string& shaderModel, ID3DBlob** blobOut)
{
    // On Linux, D3DCompileFromFile is not available. Use RHI compilation instead.
    std::string narrowPath = WideToNarrow(filename);

    if (!FileExistsLinux(narrowPath))
    {
        return E_FAIL;
    }

    std::string sourceCode;
    if (!ReadFileContents(narrowPath, sourceCode))
    {
        return E_FAIL;
    }

    // Determine shader stage from shader model string
    Spark::RHI::RHIShaderStage stage = Spark::RHI::RHIShaderStage::Vertex;
    if (shaderModel.find("ps_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Pixel;
    }
    else if (shaderModel.find("gs_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Geometry;
    }
    else if (shaderModel.find("hs_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Hull;
    }
    else if (shaderModel.find("ds_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Domain;
    }
    else if (shaderModel.find("cs_") == 0)
    {
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

    if (!result.success)
    {
        return E_FAIL;
    }

    // blobOut remains null on Linux (ID3DBlob is a stub type)
    if (blobOut)
    {
        *blobOut = nullptr;
    }

    return S_OK;
}

// ============================================================================
// PRIVATE HELPER METHODS (Linux)
// ============================================================================

HRESULT Shader::CompileShaderFromFileAdvanced(const std::wstring& filename, ShaderType type,
                                              const ShaderCompilationFlags& flags, ID3DBlob** blobOut)
{
    // Delegate to the RHI pipeline on Linux
    std::string narrowPath = WideToNarrow(filename);

    std::string sourceCode;
    if (!FileExistsLinux(narrowPath) || !ReadFileContents(narrowPath, sourceCode))
    {
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

    if (blobOut)
    {
        *blobOut = nullptr; // ID3DBlob is a stub on Linux
    }

    return result.success ? S_OK : E_FAIL;
}

HRESULT Shader::CreateInputLayout(ID3DBlob* /*vertexShaderBlob*/, ID3D11InputLayout** inputLayout)
{
    // No-op on Linux: input layout is a D3D11 concept.
    // The RHI backend handles vertex input configuration.
    if (inputLayout)
    {
        *inputLayout = nullptr;
    }
    return S_OK;
}

// ============================================================================
// RHI CROSS-PLATFORM COMPILATION (Linux)
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

    // Read source if file exists
    std::string sourceCode;
    if (FileExistsLinux(sourceFile) && ReadFileContents(sourceFile, sourceCode))
    {
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
    if (backend == Spark::RHI::GraphicsBackend::Vulkan)
    {
        outputPath += ".spv";
    }
    else
    {
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

#endif // SPARK_PLATFORM_WINDOWS
