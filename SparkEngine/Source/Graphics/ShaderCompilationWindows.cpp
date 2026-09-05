/**
 * @file ShaderCompilationWindows.cpp
 * @brief Windows/D3D11 implementation — split from ShaderCompilation.cpp
 */
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
#include "../Utils/ContainerUtils.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
// Phase U: activated Tier 2 graphics orphan — process-wide shader file
// watcher. Each successful LoadFromFile / LoadVertexShader /
// LoadPixelShader call registers the parent directory with the singleton
// so runtime file-watching covers every shader that is actually loaded.
#include "ShaderHotReload.h"
// Phase V: activated Tier 2 graphics orphan — persistent on-disk shader
// cache. LoadShaderFromSource consults the cache before compiling and
// stores the compiled bytecode on success so subsequent process runs
// pick up the cached blob.
#include "ShaderDiskCache.h"
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
#include <cstring>
#include <optional>
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// Console logging — use centralized macros from LogMacros.h
#include "../Utils/LogMacros.h"

// File-based compile utilities (CompileShaderFromFileAdvanced, CreateInputLayout,
// CompileShaderFromFile, CompileWithRHI) live in ShaderCompilationWindowsCompileOps.cpp.

// ============================================================================
// SHADER LOADING — VERTEX AND PIXEL
// ============================================================================

HRESULT Shader::LoadVertexShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, !filename.empty(), "LoadVertexShader: filename empty");
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, m_device);

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Compiling vertex shader: %ls", filename.c_str());

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

    // Phase U: register the parent directory with the ShaderHotReload
    // singleton so runtime file-watching picks up this file.
    {
        std::string narrowFile(filename.begin(), filename.end());
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(narrowFile).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
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

    // Phase U: register the parent directory with the ShaderHotReload
    // singleton so runtime file-watching picks up this file.
    {
        std::string narrowFile(filename.begin(), filename.end());
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(narrowFile).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
    }

    return hr;
}

// ============================================================================
// SHADER LOADING FROM SOURCE AND FILE
// ============================================================================

namespace
{
    Spark::Graphics::ShaderStage ToCacheStage(ShaderType type)
    {
        switch (type)
        {
        case ShaderType::PIXEL_SHADER:
            return Spark::Graphics::ShaderStage::Pixel;
        case ShaderType::GEOMETRY_SHADER:
            return Spark::Graphics::ShaderStage::Geometry;
        case ShaderType::HULL_SHADER:
            return Spark::Graphics::ShaderStage::Hull;
        case ShaderType::DOMAIN_SHADER:
            return Spark::Graphics::ShaderStage::Domain;
        case ShaderType::COMPUTE_SHADER:
            return Spark::Graphics::ShaderStage::Compute;
        default:
            return Spark::Graphics::ShaderStage::Vertex;
        }
    }

    /// True when @p bytecode starts with a Direct3D container magic ('DXBC' for
    /// FXC/DXBC, 'DXIL' for DXC). The write side (RHIFactory::SaveCompiledShader)
    /// enforces the same rule; without it here, whatever bytes happen to be in the
    /// user-writable cache directory would be handed straight to the D3D11 runtime.
    bool IsDirect3DBytecode(const std::vector<uint8_t>& bytecode)
    {
        if (bytecode.size() < 4)
            return false;

        const bool isDXBC =
            bytecode[0] == 'D' && bytecode[1] == 'X' && bytecode[2] == 'B' && bytecode[3] == 'C';
        const bool isDXIL =
            bytecode[0] == 'D' && bytecode[1] == 'X' && bytecode[2] == 'I' && bytecode[3] == 'L';
        return isDXBC || isDXIL;
    }

    /// Build the disk-cache key for a source compile.
    ///
    /// ShaderDiskCache::HashSourceForDisk already mixes hlslCode, defines, target,
    /// stage and entryPoint into the key. It does not see the target profile string
    /// or the D3DCompile flag word, and both change the emitted bytecode, so those
    /// two are folded into the defines list here. Without that, a debug and a
    /// release build of the same source would share one cache entry and the second
    /// run would load the wrong blob.
    Spark::Graphics::ShaderSource MakeDiskCacheKey(const std::string& source, ShaderType type,
                                                   const ShaderCompilationFlags& flags, const std::string& target,
                                                   UINT compileFlags)
    {
        Spark::Graphics::ShaderSource key;
        key.hlslCode = source;
        key.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
        key.stage = ToCacheStage(type);
        key.shaderModel = target;
        key.defines = flags.defines;
        key.defines.push_back("__spark_target=" + target);
        key.defines.push_back("__spark_flags=" + std::to_string(compileFlags));
        return key;
    }
} // namespace

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

    // Phase V: consult the shared disk cache before invoking the compiler. A
    // hit is wrapped in a D3DCreateBlob buffer so CreateVertexShader and
    // CreateInputLayout take exactly the same path as a fresh compile.
    auto& diskCache = Spark::Graphics::GetShaderDiskCache();
    const Spark::Graphics::ShaderSource cacheSource = MakeDiskCacheKey(source, type, flags, target, compileFlags);

    ComPtr<ID3DBlob> shaderBlob;
    bool servedFromDiskCache = false;
    if (diskCache.IsInitialized())
    {
        const std::optional<Spark::Graphics::CompiledShaderBlob> cached =
            diskCache.Lookup(cacheSource, Spark::Graphics::ShaderTarget::DXBC);
        if (cached && cached->success && IsDirect3DBytecode(cached->bytecode))
        {
            if (SUCCEEDED(D3DCreateBlob(cached->bytecode.size(), shaderBlob.GetAddressOf())))
            {
                std::memcpy(shaderBlob->GetBufferPointer(), cached->bytecode.data(), cached->bytecode.size());
                servedFromDiskCache = true;
            }
        }
        else if (cached && cached->success && !cached->bytecode.empty())
        {
            // The cache lives in a user-writable directory and can also be filled by
            // an external daemon, so a hit is not proof of valid bytecode. Handing a
            // truncated or tampered payload to CreateVertexShader/CreateInputLayout
            // would fail the shader permanently; fall through to a fresh compile.
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader disk-cache entry rejected: payload is not DXBC/DXIL", L"WARNING");
        }
    }

    HRESULT hr = S_OK;
    if (!servedFromDiskCache)
    {
        ComPtr<ID3DBlob> errorBlob;
        hr = D3DCompile(source.c_str(), source.size(), nullptr, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
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
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader %s in %.2f ms",
                       servedFromDiskCache ? "loaded from disk cache" : "compiled from source", compileTimeMs);
        LOG_TO_CONSOLE_IMMEDIATE(servedFromDiskCache ? L"Shader loaded from disk cache"
                                                     : L"Shader compiled from source successfully",
                                 L"SUCCESS");
        m_isCompiled = true;

        // Phase V: store the freshly compiled DXBC bytecode in the shared disk
        // cache so the next run hits the lookup above. A blob that came from
        // the cache is not written back.
        if (diskCache.IsInitialized() && shaderBlob && !servedFromDiskCache)
        {
            Spark::Graphics::CompiledShaderBlob blob;
            const uint8_t* bytes = static_cast<const uint8_t*>(shaderBlob->GetBufferPointer());
            blob.bytecode.assign(bytes, bytes + shaderBlob->GetBufferSize());
            blob.target = Spark::Graphics::ShaderTarget::DXBC;
            blob.stage = cacheSource.stage;
            blob.entryPoint = cacheSource.entryPoint;
            blob.success = true;
            diskCache.Store(cacheSource, Spark::Graphics::ShaderTarget::DXBC, blob);
        }
    }

    return hr;
}

HRESULT Shader::LoadFromFile(const std::string& filePath, ShaderType type, const ShaderCompilationFlags& flags)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loading shader from file: %s (type=%d)", filePath.c_str(),
                   static_cast<int>(type));
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

    // Phase U: register the parent directory with the
    // Spark::Graphics::ShaderHotReload singleton so runtime file-watching
    // picks up this file and its siblings.
    {
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(m_filePath).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
    }

    return LoadShaderFromSource(source, type, flags);
}

#endif // inner SPARK_PLATFORM_WINDOWS


#endif // SPARK_PLATFORM_WINDOWS
