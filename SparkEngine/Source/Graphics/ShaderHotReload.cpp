#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file ShaderHotReload.cpp
 * @brief Shader hot-reload system and variant management
 *
 * Monitors shader files for changes via Win32 file timestamps, triggers
 * automatic recompilation on modification, and manages shader variant
 * permutations (different #define combinations). On Linux, uses stat()
 * for file monitoring and the RHI pipeline for recompilation.
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
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// Console logging — use centralized macros from LogMacros.h
#include "../Utils/LogMacros.h"

// ============================================================================
// SHADER VARIANT MANAGEMENT
// ============================================================================

int Shader::CreateShaderVariant(const std::string& baseName, const std::vector<std::string>& defines)
{
    ShaderVariant variant;
    variant.id = static_cast<int>(m_variants.size());
    variant.name = baseName;
    for (size_t i = 0; i < defines.size(); ++i)
    {
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
    if (variantId >= 0 && variantId < static_cast<int>(m_variants.size()))
    {
        m_activeVariant = variantId;
        LOG_TO_CONSOLE_IMMEDIATE(L"Active shader variant changed", L"DEBUG");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid shader variant ID", L"WARNING");
    }
}

// ============================================================================
// HOT RELOAD
// ============================================================================

int Shader::HotReloadShaders()
{
    if (!m_hotReloadEnabled)
    {
        return 0;
    }

    int reloadCount = 0;

    for (const auto& watchedFile : m_watchedFiles)
    {
        HANDLE hFile =
            CreateFileW(watchedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            continue;

        FILETIME currentModified;
        GetFileTime(hFile, nullptr, nullptr, &currentModified);
        CloseHandle(hFile);

        if (CompareFileTime(&currentModified, &m_lastModified) > 0)
        {
            // File has been modified, reload
            m_lastModified = currentModified;

            std::string narrowPath(watchedFile.begin(), watchedFile.end());
            HRESULT hr = LoadFromFile(narrowPath, m_type, m_defaultFlags);
            if (SUCCEEDED(hr))
            {
                reloadCount++;
                LOG_TO_CONSOLE_IMMEDIATE(L"Hot-reloaded shader: " + watchedFile, L"SUCCESS");
            }
            else
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to hot-reload shader: " + watchedFile, L"ERROR");
            }
        }
    }

    if (reloadCount > 0)
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.hotReloadCount += reloadCount;
        NotifyStateChange();
    }

    return reloadCount;
}

void Shader::UpdateFileMonitoring()
{
    // Check watched files for changes (called from game loop)
    if (m_hotReloadEnabled)
    {
        HotReloadShaders();
    }
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// LINUX IMPLEMENTATION — Hot-reload and variant management via RHI
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
#include <algorithm>

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
// SHADER VARIANTS (Linux)
// ============================================================================

int Shader::CreateShaderVariant(const std::string& baseName, const std::vector<std::string>& defines)
{
    ShaderVariant variant;
    variant.id = static_cast<int>(m_variants.size());
    variant.baseName = baseName;
    variant.name = baseName;
    variant.defines = defines;
    variant.isCompiled = false;
    variant.lastModified.dwLowDateTime = 0;
    variant.lastModified.dwHighDateTime = 0;

    // Build variant name from defines
    for (const auto& def : defines)
    {
        variant.name += "_" + def;
    }

    // Attempt to compile the variant through RHI
    ShaderCompilationFlags flags = m_defaultFlags;
    flags.defines.insert(flags.defines.end(), defines.begin(), defines.end());

    // Try to find and compile the base shader file
    std::string sourceFile = baseName;
    if (FileExistsLinux(sourceFile))
    {
        std::string sourceCode;
        if (ReadFileContents(sourceFile, sourceCode))
        {
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

void Shader::SetActiveVariant(int variantId)
{
    if (variantId >= 0 && variantId < static_cast<int>(m_variants.size()))
    {
        m_activeVariant = variantId;
        NotifyStateChange();
    }
}

// ============================================================================
// HOT RELOAD (Linux)
// ============================================================================

int Shader::HotReloadShaders()
{
    int reloadCount = 0;

    for (const auto& watchedFile : m_watchedFiles)
    {
        std::string narrowPath = WideToNarrow(watchedFile);
        uint64_t currentModTime = GetFileModTime(narrowPath);
        uint64_t storedModTime = static_cast<uint64_t>(m_lastModified.dwLowDateTime) |
                                 (static_cast<uint64_t>(m_lastModified.dwHighDateTime) << 32);

        if (currentModTime > storedModTime && currentModTime != 0)
        {
            HRESULT hr = LoadFromFile(narrowPath, m_type, m_defaultFlags);
            if (SUCCEEDED(hr))
            {
                reloadCount++;
            }
        }
    }

    if (reloadCount > 0)
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.hotReloadCount += reloadCount;
    }

    return reloadCount;
}

void Shader::UpdateFileMonitoring()
{
    if (m_hotReloadEnabled)
    {
        HotReloadShaders();
    }
}

#endif // SPARK_PLATFORM_WINDOWS
