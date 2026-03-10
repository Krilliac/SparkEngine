/**
 * @file GPUDebugMarkers.h
 * @brief GPU annotation system for PIX, RenderDoc, and VS Graphics Debugger
 * @author Spark Engine Team
 * @date 2026
 *
 * Wraps ID3DUserDefinedAnnotation to provide named GPU event regions and
 * instant markers visible in graphics debuggers. Includes an RAII scoped
 * event helper and convenience macros.
 *
 * ## Usage
 * @code
 *   GPUDebugMarkers markers;
 *   markers.Initialize(context);
 *
 *   {
 *       GPU_SCOPED_EVENT(markers, L"Shadow Pass");
 *       // ... shadow rendering ...
 *       GPU_MARKER(markers, L"Cascade 0 done");
 *   }
 * @endcode
 *
 * @see GraphicsEngine, GPUTimestampQuery
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11_1.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

/**
 * @brief GPU debug annotation system
 *
 * Provides named event regions and markers for GPU debugging tools.
 * Falls back gracefully to no-ops when the annotation interface is
 * unavailable (e.g., no debugger attached).
 */
class GPUDebugMarkers
{
public:
    GPUDebugMarkers() = default;
    ~GPUDebugMarkers() { Shutdown(); }

    // Non-copyable, non-movable
    GPUDebugMarkers(const GPUDebugMarkers&) = delete;
    GPUDebugMarkers& operator=(const GPUDebugMarkers&) = delete;
    GPUDebugMarkers(GPUDebugMarkers&&) = delete;
    GPUDebugMarkers& operator=(GPUDebugMarkers&&) = delete;

    /**
     * @brief Initialize by querying for the annotation interface
     * @param context D3D11 device context
     * @return true if annotation interface is available
     */
    bool Initialize(ID3D11DeviceContext* context)
    {
        if (!context)
        {
            return false;
        }

        HRESULT hr = context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation),
                                              reinterpret_cast<void**>(m_annotation.ReleaseAndGetAddressOf()));
        if (FAILED(hr) || !m_annotation)
        {
            m_annotation.Reset();
            m_initialized = true; // Initialized but without annotation support
            return false;
        }

        m_initialized = true;
        return true;
    }

    /**
     * @brief Release the annotation interface
     */
    void Shutdown()
    {
        m_annotation.Reset();
        m_eventDepth = 0;
        m_initialized = false;
    }

    /**
     * @brief Begin a named GPU event region
     * @param name Wide-string name visible in the debugger
     */
    void BeginEvent(const wchar_t* name)
    {
        if (m_annotation && m_annotation->GetStatus())
        {
            m_annotation->BeginEvent(name);
        }
        m_eventDepth++;
    }

    /**
     * @brief End the current GPU event region
     */
    void EndEvent()
    {
        if (m_eventDepth == 0)
        {
            return; // Mismatched end — do nothing
        }

        if (m_annotation && m_annotation->GetStatus())
        {
            m_annotation->EndEvent();
        }
        m_eventDepth--;
    }

    /**
     * @brief Place an instant marker at the current point in the command stream
     * @param name Wide-string name visible in the debugger
     */
    void SetMarker(const wchar_t* name)
    {
        if (m_annotation && m_annotation->GetStatus())
        {
            m_annotation->SetMarker(name);
        }
    }

    /** @brief Check if the annotation interface is available and active */
    bool IsAvailable() const { return m_annotation && m_annotation->GetStatus(); }

    /** @brief Check if initialized (may still lack annotation support) */
    bool IsInitialized() const { return m_initialized; }

    /** @brief Get current event nesting depth (for validation) */
    uint32_t GetEventDepth() const { return m_eventDepth; }

    /**
     * @brief Get a console-friendly status string
     */
    std::wstring Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return L"[GPUDebugMarkers] Not initialized";
        }

        if (!m_annotation)
        {
            return L"[GPUDebugMarkers] Initialized (annotation interface unavailable)";
        }

        bool active = m_annotation->GetStatus() != 0;
        wchar_t buf[128];
        swprintf(buf, 128, L"[GPUDebugMarkers] Active: %s | Depth: %u",
                 active ? L"Yes" : L"No", m_eventDepth);
        return std::wstring(buf);
    }

private:
    ComPtr<ID3DUserDefinedAnnotation> m_annotation;
    uint32_t m_eventDepth = 0;
    bool m_initialized = false;
};

/**
 * @brief RAII scoped GPU event — begins on construction, ends on destruction
 */
class ScopedGPUEvent
{
public:
    ScopedGPUEvent(GPUDebugMarkers& markers, const wchar_t* name)
        : m_markers(markers)
    {
        m_markers.BeginEvent(name);
    }

    ~ScopedGPUEvent()
    {
        m_markers.EndEvent();
    }

    // Non-copyable, non-movable
    ScopedGPUEvent(const ScopedGPUEvent&) = delete;
    ScopedGPUEvent& operator=(const ScopedGPUEvent&) = delete;
    ScopedGPUEvent(ScopedGPUEvent&&) = delete;
    ScopedGPUEvent& operator=(ScopedGPUEvent&&) = delete;

private:
    GPUDebugMarkers& m_markers;
};

} // namespace Spark::Graphics

// ============================================================================
// Convenience macros
// ============================================================================

/** @brief Create a scoped GPU event that lasts for the enclosing block */
#define GPU_SCOPED_EVENT(markers, name) \
    ::Spark::Graphics::ScopedGPUEvent _gpuEvent_##__LINE__((markers), (name))

/** @brief Place an instant GPU marker */
#define GPU_MARKER(markers, name) \
    (markers).SetMarker(name)

#endif // SPARK_PLATFORM_WINDOWS
