/**
 * @file ShaderCompilationLinux.cpp
 * @brief Linux implementation — split from ShaderCompilation.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


// ============================================================================
// LINUX IMPLEMENTATION — Shader compilation via RHI pipeline
// ============================================================================

#include "Shader.h"
// Phase U: activated Tier 2 graphics orphan — process-wide shader file
// watcher. Linux branch needs the same include so the singleton is
// reachable from LoadVertexShader/LoadPixelShader/LoadFromFile.
#include "ShaderHotReload.h"
// Phase V: activated Tier 2 graphics orphan — persistent on-disk shader
// cache. Linux branch needs the same include so LoadShaderFromSource
// consults the cache on every backend build.
#include "ShaderDiskCache.h"
#include "RHI/RHIFactory.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <filesystem>
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

// Phase V helper: convert ShaderType to Spark::Graphics::ShaderStage for
// disk-cache key construction. The cache keys shader compilations by
// (hlslCode + defines + target + stage) so every stage must round-trip.
static Spark::Graphics::ShaderStage ShaderTypeToGraphicsStage(ShaderType type)
{
    switch (type)
    {
    case ShaderType::VERTEX_SHADER:
        return Spark::Graphics::ShaderStage::Vertex;
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

// Phase V helper: construct the ShaderSource cache key from the local
// LoadShaderFromSource inputs. The cache hash covers the hlsl code,
// preprocessor defines, target, and stage — anything that changes the
// produced bytecode must feed into the key.
static Spark::Graphics::ShaderSource MakeCacheSource(const std::string& source, ShaderType type,
                                                     const ShaderCompilationFlags& flags)
{
    Spark::Graphics::ShaderSource s;
    s.hlslCode = source;
    s.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
    s.stage = ShaderTypeToGraphicsStage(type);
    s.defines = flags.defines;
    return s;
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
    if (!Spark::ContainerUtils::Contains(m_watchedFiles, filename))
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
    // Detect source language from the file extension; set target to match
    // so GLSL files compile as GLSL→GLSL (not GLSL→HLSL which is unsupported).
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    std::string ext = std::filesystem::path(narrowPath).extension().string();
    if (ext == ".glsl" || ext == ".vert" || ext == ".frag")
    {
        options.sourceLanguage = Spark::RHI::ShaderLanguage::GLSL;
        options.targetLanguage = Spark::RHI::ShaderLanguage::GLSL;
        options.targetBackend = Spark::RHI::GraphicsBackend::OpenGL;
    }

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

    // Store the compiled source so callers can create RHI pipeline states.
    // For GLSL→GLSL passthrough, the bytecode IS the GLSL source text.
    m_compiledVertexSource.assign(reinterpret_cast<const char*>(result.bytecode.data()), result.bytecode.size());
    // Phase U: register the parent directory with the ShaderHotReload
    // singleton so runtime file-watching picks up this file.
    {
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(narrowPath).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
    }

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
    if (!Spark::ContainerUtils::Contains(m_watchedFiles, filename))
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

    // Detect source language from the file extension; set target to match
    // so GLSL files compile as GLSL→GLSL (not GLSL→HLSL which is unsupported).
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    std::string psExt = std::filesystem::path(narrowPath).extension().string();
    if (psExt == ".glsl" || psExt == ".vert" || psExt == ".frag")
    {
        options.sourceLanguage = Spark::RHI::ShaderLanguage::GLSL;
        options.targetLanguage = Spark::RHI::ShaderLanguage::GLSL;
        options.targetBackend = Spark::RHI::GraphicsBackend::OpenGL;
    }

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

    // Store the compiled source so callers can create RHI pipeline states.
    m_compiledPixelSource.assign(reinterpret_cast<const char*>(result.bytecode.data()), result.bytecode.size());

    // Phase U: register the parent directory with the ShaderHotReload
    // singleton so runtime file-watching picks up this file.
    {
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(narrowPath).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compiled (RHI): " + filename, L"SUCCESS");
    NotifyStateChange();
    return S_OK;
}

HRESULT Shader::LoadShaderFromSource(const std::string& source, ShaderType type, const ShaderCompilationFlags& flags)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    // Phase V: disk-cache lookup. Construct the cache key from the
    // hlsl source + defines + stage; on a hit we skip the RHI compile
    // and reuse the stored bytecode directly. Target is pinned to DXBC
    // as a canonical key — a future pass can switch per-backend when
    // the RHI cross-compile pipeline wires all backends through this
    // code path.
    auto& diskCache = Spark::Graphics::GetShaderDiskCache();
    const auto cacheSource = MakeCacheSource(source, type, flags);
    constexpr auto kCacheTarget = Spark::Graphics::ShaderTarget::DXBC;

    Spark::RHI::ShaderCompileResult result;
    bool cacheHit = false;
    if (diskCache.IsInitialized())
    {
        if (auto cached = diskCache.Lookup(cacheSource, kCacheTarget); cached.has_value())
        {
            result.success = true;
            result.bytecode = cached->bytecode;
            cacheHit = true;
        }
    }

    if (!cacheHit)
    {
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

        result = Spark::RHI::CompileShader(options);
    }

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

    // Phase V: store the freshly compiled bytecode in the disk cache so
    // subsequent process runs (and sibling Shader instances) skip the
    // backend compile. Cache hits skip the store entirely — the blob is
    // already on disk.
    if (!cacheHit && diskCache.IsInitialized())
    {
        Spark::Graphics::CompiledShaderBlob blob;
        blob.bytecode = result.bytecode;
        blob.target = kCacheTarget;
        blob.stage = cacheSource.stage;
        blob.entryPoint = cacheSource.entryPoint;
        blob.success = true;
        diskCache.Store(cacheSource, kCacheTarget, blob);
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
    if (!Spark::ContainerUtils::Contains(m_watchedFiles, widePath))
    {
        m_watchedFiles.push_back(widePath);
    }

    m_filePath = filePath;
    m_type = type;

    // Store modification time for hot reload
    uint64_t modTime = GetFileModTime(filePath);
    m_lastModified.dwLowDateTime = static_cast<uint32_t>(modTime & 0xFFFFFFFF);
    m_lastModified.dwHighDateTime = static_cast<uint32_t>((modTime >> 32) & 0xFFFFFFFF);

    // Phase U: register the parent directory with the ShaderHotReload
    // singleton so runtime file-watching picks up this file.
    {
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
    }

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


#endif // !SPARK_PLATFORM_WINDOWS
