#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file ShaderConsoleOps.cpp
 * @brief Console integration methods for the Shader system
 *
 * All Console_* methods for runtime shader management via console.
 * Split from Shader.cpp for maintainability.
 */

#include "Shader.h"
#include "RHI/RHI.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"
#include <sstream>
#include <filesystem>
#include <chrono>

#ifdef SPARK_PLATFORM_WINDOWS

Shader::ShaderMetrics Shader::Console_GetMetrics() const
{
    return GetMetricsThreadSafe();
}

void Shader::Console_RecompileAll()
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Recompiling all shaders (%zu watched files)", m_watchedFiles.size());
    LOG_TO_CONSOLE_IMMEDIATE(L"Recompiling all shaders...", L"INFO");

    // Reload from watched files
    for (const auto& watchedFile : m_watchedFiles)
    {
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

    std::wstring msg = L"Compilation flags updated: debug=" + std::to_wstring(enableDebug) + L", optimization=" +
                       std::to_wstring(enableOptimization);
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

std::string Shader::Console_ListShaders() const
{
    std::stringstream ss;
    ss << "=== Loaded Shaders ===" << std::endl;

    if (m_vertexShader && m_vertexShader->IsValid())
    {
        ss << "  [VS] Vertex Shader - Active" << std::endl;
    }
    else
    {
        ss << "  [VS] Vertex Shader - Not loaded" << std::endl;
    }

    if (m_pixelShader && m_pixelShader->IsValid())
    {
        ss << "  [PS] Pixel Shader - Active" << std::endl;
    }
    else
    {
        ss << "  [PS] Pixel Shader - Not loaded" << std::endl;
    }

    // List cached shaders
    for (const auto& entry : m_shaderCache)
    {
        ss << "  [Cache] " << entry.first;
        ss << (entry.second->IsValid() ? " - Valid" : " - Invalid");
        ss << std::endl;
    }

    // List variants
    for (const auto& variant : m_variants)
    {
        ss << "  [Variant] " << variant.name;
        ss << (variant.isCompiled ? " - Compiled" : " - Not compiled");
        if (variant.id == m_activeVariant)
            ss << " (ACTIVE)";
        ss << std::endl;
    }

    // Watched files
    ss << std::endl << "Watched files: " << m_watchedFiles.size() << std::endl;
    for (const auto& wf : m_watchedFiles)
    {
        std::string narrowPath(wf.begin(), wf.end());
        ss << "  " << narrowPath << std::endl;
    }

    return ss.str();
}

std::string Shader::Console_GetShaderInfo(const std::string& shaderName) const
{
    std::stringstream ss;
    ss << "=== Shader Info: " << shaderName << " ===" << std::endl;

    if (shaderName == "vertex" || shaderName == "vs")
    {
        ss << "Type: Vertex Shader" << std::endl;
        ss << "Valid: " << (m_vertexShader && m_vertexShader->IsValid() ? "Yes" : "No") << std::endl;
    }
    else if (shaderName == "pixel" || shaderName == "ps")
    {
        ss << "Type: Pixel Shader" << std::endl;
        ss << "Valid: " << (m_pixelShader && m_pixelShader->IsValid() ? "Yes" : "No") << std::endl;
    }
    else
    {
        // Check cache
        auto it = m_shaderCache.find(shaderName);
        if (it != m_shaderCache.end())
        {
            ss << "Found in cache" << std::endl;
            ss << "Valid: " << (it->second->IsValid() ? "Yes" : "No") << std::endl;
        }
        else
        {
            ss << "Shader not found" << std::endl;
        }
    }

    // General metrics
    auto metrics = GetMetricsThreadSafe();
    ss << std::endl << "Compilation Stats:" << std::endl;
    ss << "  Compiled: " << metrics.compiledShaders << std::endl;
    ss << "  Failed: " << metrics.failedCompilations << std::endl;
    ss << "  Last compile time: " << metrics.lastCompileTime << " ms" << std::endl;

    if (!m_filePath.empty())
    {
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

    if (m_vertexShader && !m_vertexShader->IsValid())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Vertex shader invalid", L"ERROR");
        errors++;
    }

    if (m_pixelShader && !m_pixelShader->IsValid())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Pixel shader invalid", L"ERROR");
        errors++;
    }

    for (const auto& entry : m_shaderCache)
    {
        if (!entry.second->IsValid())
        {
            std::wstring name(entry.first.begin(), entry.first.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Cached shader invalid: " + name, L"ERROR");
            errors++;
        }
    }

    if (errors == 0)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"All shaders validated successfully", L"SUCCESS");
    }
    else
    {
        std::wstring msg = L"Shader validation found " + std::to_wstring(errors) + L" error(s)";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"WARNING");
    }

    return errors;
}

void Shader::Console_ClearCache()
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Clearing shader cache (%zu cached, %zu variants)",
                   m_shaderCache.size(), m_variants.size());
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
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader search paths updated (%zu paths)", paths.size());
    std::wstring msg = L"Shader search paths updated (" + std::to_wstring(paths.size()) + L" paths)";
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

// ============================================================================
// RHI CROSS-PLATFORM COMPILATION
// ============================================================================

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

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "Shader.h"
#include "RHI/RHI.h"
#include "../Utils/SparkConsole.h"
#include <sstream>
#include <filesystem>
#include <string>

// Helper: convert wstring to narrow string
static std::string WideToNarrow(const std::wstring& wide)
{
    return std::string(wide.begin(), wide.end());
}

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
// Console Methods — Linux/RHI
// ============================================================================

Shader::ShaderMetrics Shader::Console_GetMetrics() const
{
    return GetMetricsThreadSafe();
}

void Shader::Console_RecompileAll()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Recompiling all shaders...", L"INFO");

    for (const auto& watchedFile : m_watchedFiles)
    {
        std::string narrowPath = WideToNarrow(watchedFile);
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

    std::wstring msg = L"Compilation flags updated: debug=" + std::to_wstring(enableDebug) + L", optimization=" +
                       std::to_wstring(enableOptimization);
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
}

std::string Shader::Console_ListShaders() const
{
    std::stringstream ss;
    ss << "=== Loaded Shaders ===" << std::endl;

    ss << "  [VS] Vertex Shader - "
       << (m_isCompiled && m_type == ShaderType::VERTEX_SHADER ? "Active (RHI)" : "Not loaded") << std::endl;
    ss << "  [PS] Pixel Shader - "
       << (m_isCompiled && m_type == ShaderType::PIXEL_SHADER ? "Active (RHI)" : "Not loaded") << std::endl;

    // List cached shaders
    for (const auto& entry : m_shaderCache)
    {
        ss << "  [Cache] " << entry.first;
        ss << (entry.second->IsValid() ? " - Valid" : " - Invalid");
        ss << std::endl;
    }

    // List variants
    for (const auto& variant : m_variants)
    {
        ss << "  [Variant] " << variant.name;
        ss << (variant.isCompiled ? " - Compiled" : " - Not compiled");
        if (variant.id == m_activeVariant)
            ss << " (ACTIVE)";
        ss << std::endl;
    }

    // Watched files
    ss << std::endl << "Watched files: " << m_watchedFiles.size() << std::endl;
    for (const auto& wf : m_watchedFiles)
    {
        std::string narrowPath = WideToNarrow(wf);
        ss << "  " << narrowPath << std::endl;
    }

    return ss.str();
}

std::string Shader::Console_GetShaderInfo(const std::string& shaderName) const
{
    std::stringstream ss;
    ss << "=== Shader Info: " << shaderName << " ===" << std::endl;

    if (shaderName == "vertex" || shaderName == "vs")
    {
        ss << "Type: Vertex Shader" << std::endl;
        ss << "Valid: " << (m_isCompiled && m_type == ShaderType::VERTEX_SHADER ? "Yes" : "No") << std::endl;
        ss << "Backend: RHI (Linux)" << std::endl;
    }
    else if (shaderName == "pixel" || shaderName == "ps")
    {
        ss << "Type: Pixel Shader" << std::endl;
        ss << "Valid: " << (m_isCompiled && m_type == ShaderType::PIXEL_SHADER ? "Yes" : "No") << std::endl;
        ss << "Backend: RHI (Linux)" << std::endl;
    }
    else
    {
        auto it = m_shaderCache.find(shaderName);
        if (it != m_shaderCache.end())
        {
            ss << "Found in cache" << std::endl;
            ss << "Valid: " << (it->second->IsValid() ? "Yes" : "No") << std::endl;
        }
        else
        {
            ss << "Shader not found" << std::endl;
        }
    }

    // General metrics
    auto metrics = GetMetricsThreadSafe();
    ss << std::endl << "Compilation Stats:" << std::endl;
    ss << "  Compiled: " << metrics.compiledShaders << std::endl;
    ss << "  Failed: " << metrics.failedCompilations << std::endl;
    ss << "  Last compile time: " << metrics.lastCompileTime << " ms" << std::endl;

    if (!m_filePath.empty())
    {
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

    if (!m_isCompiled && !m_filePath.empty())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Shader not compiled", L"ERROR");
        errors++;
    }

    for (const auto& entry : m_shaderCache)
    {
        if (!entry.second->IsValid())
        {
            std::wstring name(entry.first.begin(), entry.first.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Cached shader invalid: " + name, L"ERROR");
            errors++;
        }
    }

    for (const auto& variant : m_variants)
    {
        if (!variant.isCompiled)
        {
            std::wstring name(variant.name.begin(), variant.name.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Validation error: Variant not compiled: " + name, L"ERROR");
            errors++;
        }
    }

    if (errors == 0)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"All shaders validated successfully", L"SUCCESS");
    }
    else
    {
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

#endif // SPARK_PLATFORM_WINDOWS
