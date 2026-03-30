#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file RenderTargetConsole.cpp
 * @brief RenderTargetManager console integration and debug visualization
 *
 * All Console_* methods for the render target manager: listing, inspecting,
 * creating, resizing, saving, garbage collection, validation, memory reporting,
 * and on-screen debug visualization of render targets.
 *
 * @author Spark Engine Team
 * @date 2025
 */

#include "RenderTarget.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// RENDER TARGET MANAGER — CONSOLE INTEGRATION
// ============================================================================

RenderTargetManager::RenderTargetMetrics RenderTargetManager::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string RenderTargetManager::Console_ListRenderTargets() const
{
    std::string result = "=== Render Targets ===\n";

    for (const auto& pair : m_renderTargets)
    {
        const auto& desc = pair.second->GetDesc();
        result += pair.first + " (" + std::to_string(desc.width) + "x" + std::to_string(desc.height) + ", " +
                  FormatToString(desc.format) + ")\n";
    }

    result += "\n=== MRT Groups ===\n";
    for (const auto& pair : m_mrtGroups)
    {
        result += pair.first + " (" + std::to_string(pair.second->GetRenderTargetCount()) + " targets)\n";
    }

    return result;
}

std::string RenderTargetManager::Console_GetRenderTargetInfo(const std::string& name) const
{
    auto rt = GetRenderTarget(name);
    if (rt)
    {
        return rt->GetInfo();
    }
    return "Render target '" + name + "' not found";
}

bool RenderTargetManager::Console_SaveRenderTarget(const std::string& name, const std::string& filename)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
    {
        return false;
    }

    std::string actualFilename = filename.empty() ? (name + ".bmp") : filename;
    return rt->SaveToFile(actualFilename);
}

bool RenderTargetManager::Console_CreateRenderTarget(const std::string& name, uint32_t width, uint32_t height,
                                                     const std::string& format)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Console creating render target '%s' (%ux%u, %s)", name.c_str(), width,
                   height, format.c_str());
    RenderTargetDesc desc;
    desc.name = name;
    desc.width = width;
    desc.height = height;
    desc.format = StringToFormat(format);

    auto rt = CreateRenderTarget(desc);
    return rt != nullptr;
}

bool RenderTargetManager::Console_ResizeRenderTarget(const std::string& name, uint32_t width, uint32_t height)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
    {
        return false;
    }

    return SUCCEEDED(rt->Resize(m_device, width, height));
}

void RenderTargetManager::Console_ToggleVisualization(const std::string& name)
{
    // Toggle debug visualization for a specific render target
    // When enabled, the render target will be drawn as a screen-space quad overlay
    auto it = m_visualizationState.find(name);
    if (it != m_visualizationState.end())
    {
        it->second = !it->second;
    }
    else
    {
        // Verify render target exists before adding
        auto rt = GetRenderTarget(name);
        if (rt)
        {
            m_visualizationState[name] = true;
        }
    }
}

void RenderTargetManager::Console_SetClearColor(const std::string& name, float r, float g, float b, float a)
{
    auto rt = GetRenderTarget(name);
    if (rt)
    {
        auto& desc = rt->GetDesc();
        desc.clearColor = {r, g, b, a};
    }
}

void RenderTargetManager::Console_GarbageCollect()
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Starting render target garbage collection (%zu targets)",
                    m_renderTargets.size());
    // Remove unused render targets
    int collected = 0;
    for (auto it = m_renderTargets.begin(); it != m_renderTargets.end();)
    {
        if (it->second.use_count() == 1)
        { // Only held by manager
            it->second->Destroy();
            it = m_renderTargets.erase(it);
            collected++;
        }
        else
        {
            ++it;
        }
    }
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Garbage collected %d render targets", collected);
    UpdateMetrics();
}

int RenderTargetManager::Console_ValidateRenderTargets()
{
    int errors = 0;

    for (const auto& pair : m_renderTargets)
    {
        if (!pair.second->IsValid())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Render target '%s' failed validation", pair.first.c_str());
            errors++;
        }
    }

    return errors;
}

std::string RenderTargetManager::Console_GetMemoryInfo() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);

    std::string result = "=== Render Target Memory Usage ===\n";
    result += "Total Memory: " + std::to_string(m_metrics.totalMemoryUsage / 1024 / 1024) + " MB\n";
    result += "Color Targets: " + std::to_string(m_metrics.colorTargetMemory / 1024 / 1024) + " MB\n";
    result += "Depth Targets: " + std::to_string(m_metrics.depthTargetMemory / 1024 / 1024) + " MB\n";
    result += "Active Targets: " + std::to_string(m_metrics.activeRenderTargets) + "\n";
    result += "Total Targets: " + std::to_string(m_metrics.totalRenderTargets) + "\n";

    return result;
}

// ============================================================================
// RENDER TARGET MANAGER — DEBUG VISUALIZATION
// ============================================================================

void RenderTargetManager::RenderDebugVisualization(ID3D11DeviceContext* context, uint32_t screenWidth,
                                                   uint32_t screenHeight)
{
    if (!context || m_visualizationState.empty())
        return;

    // Calculate grid layout for debug quads
    int activeCount = 0;
    for (const auto& pair : m_visualizationState)
        if (pair.second)
            activeCount++;

    if (activeCount == 0)
        return;

    int cols = static_cast<int>(ceilf(sqrtf(static_cast<float>(activeCount))));
    int rows = (activeCount + cols - 1) / cols;

    float quadWidth = screenWidth / static_cast<float>(cols) * 0.25f;
    float quadHeight = screenHeight / static_cast<float>(rows) * 0.25f;

    int index = 0;
    for (const auto& pair : m_visualizationState)
    {
        if (!pair.second)
            continue;

        auto rt = GetRenderTarget(pair.first);
        if (!rt || !rt->GetShaderResourceView())
            continue;

        // Calculate screen position for this quad (top-right corner)
        float x = screenWidth - (cols - (index % cols)) * quadWidth;
        float y = (index / cols) * quadHeight;

        // Bind the render target's SRV for debug rendering
        // The actual quad rendering would be done by the graphics engine's
        // fullscreen quad shader with the viewport set to the small debug region
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = x;
        vp.TopLeftY = y;
        vp.Width = quadWidth;
        vp.Height = quadHeight;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        ID3D11ShaderResourceView* srv = rt->GetShaderResourceView();
        context->PSSetShaderResources(0, 1, &srv);

        // Note: caller is responsible for binding the debug visualization shader
        // and issuing the draw call (e.g., fullscreen triangle)

        index++;
    }

    // Restore the full-screen viewport
    D3D11_VIEWPORT fullVp = {};
    fullVp.Width = static_cast<float>(screenWidth);
    fullVp.Height = static_cast<float>(screenHeight);
    fullVp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &fullVp);

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
}

bool RenderTargetManager::IsVisualizationEnabled(const std::string& name) const
{
    auto it = m_visualizationState.find(name);
    return it != m_visualizationState.end() && it->second;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "RenderTarget.h"
#include "../Utils/Validate.h"
#include "../Utils/LogMacros.h"

// ============================================================================
// RenderTargetManager (Linux stub) — Console Integration
// ============================================================================

RenderTargetManager::RenderTargetMetrics RenderTargetManager::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string RenderTargetManager::Console_ListRenderTargets() const
{
    std::string result = "=== Render Targets ===\n";
    for (const auto& pair : m_renderTargets)
    {
        const auto& desc = pair.second->GetDesc();
        result += pair.first + " (" + std::to_string(desc.width) + "x" + std::to_string(desc.height) + ", " +
                  FormatToString(desc.format) + ")\n";
    }
    result += "\n=== MRT Groups ===\n";
    for (const auto& pair : m_mrtGroups)
    {
        result += pair.first + " (" + std::to_string(pair.second->GetRenderTargetCount()) + " targets)\n";
    }
    return result;
}

std::string RenderTargetManager::Console_GetRenderTargetInfo(const std::string& name) const
{
    auto rt = GetRenderTarget(name);
    if (rt)
        return rt->GetInfo();
    return "Render target '" + name + "' not found";
}

bool RenderTargetManager::Console_SaveRenderTarget(const std::string& name, const std::string& filename)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
        return false;
    std::string actualFilename = filename.empty() ? (name + ".bmp") : filename;
    return rt->SaveToFile(actualFilename);
}

bool RenderTargetManager::Console_CreateRenderTarget(const std::string& name, uint32_t width, uint32_t height,
                                                     const std::string& format)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Console creating render target '%s' (%ux%u, %s)", name.c_str(), width,
                   height, format.c_str());
    RenderTargetDesc desc;
    desc.name = name;
    desc.width = width;
    desc.height = height;
    desc.format = StringToFormat(format);
    auto rt = CreateRenderTarget(desc);
    return rt != nullptr;
}

bool RenderTargetManager::Console_ResizeRenderTarget(const std::string& name, uint32_t width, uint32_t height)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
        return false;
    return SUCCEEDED(rt->Resize(m_device, width, height));
}

void RenderTargetManager::Console_ToggleVisualization(const std::string& name)
{
    auto it = m_visualizationState.find(name);
    if (it != m_visualizationState.end())
    {
        it->second = !it->second;
    }
    else
    {
        auto rt = GetRenderTarget(name);
        if (rt)
        {
            m_visualizationState[name] = true;
        }
    }
}

void RenderTargetManager::Console_SetClearColor(const std::string& name, float r, float g, float b, float a)
{
    auto rt = GetRenderTarget(name);
    if (rt)
    {
        auto& desc = rt->GetDesc();
        desc.clearColor = {r, g, b, a};
    }
}

void RenderTargetManager::Console_GarbageCollect()
{
    for (auto it = m_renderTargets.begin(); it != m_renderTargets.end();)
    {
        if (it->second.use_count() == 1)
        {
            it->second->Destroy();
            it = m_renderTargets.erase(it);
        }
        else
        {
            ++it;
        }
    }
    UpdateMetrics();
}

int RenderTargetManager::Console_ValidateRenderTargets()
{
    int errors = 0;
    for (const auto& pair : m_renderTargets)
    {
        if (!pair.second->IsValid())
        {
            errors++;
        }
    }
    return errors;
}

std::string RenderTargetManager::Console_GetMemoryInfo() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    std::string result = "=== Render Target Memory Usage ===\n";
    result += "Total Memory: " + std::to_string(m_metrics.totalMemoryUsage / 1024 / 1024) + " MB\n";
    result += "Color Targets: " + std::to_string(m_metrics.colorTargetMemory / 1024 / 1024) + " MB\n";
    result += "Depth Targets: " + std::to_string(m_metrics.depthTargetMemory / 1024 / 1024) + " MB\n";
    result += "Active Targets: " + std::to_string(m_metrics.activeRenderTargets) + "\n";
    result += "Total Targets: " + std::to_string(m_metrics.totalRenderTargets) + "\n";
    return result;
}

void RenderTargetManager::RenderDebugVisualization(ID3D11DeviceContext* /*context*/, uint32_t /*screenWidth*/,
                                                   uint32_t /*screenHeight*/)
{
    // No-op on Linux
}

bool RenderTargetManager::IsVisualizationEnabled(const std::string& name) const
{
    auto it = m_visualizationState.find(name);
    return it != m_visualizationState.end() && it->second;
}

#endif // SPARK_PLATFORM_WINDOWS
