/**
 * @file VRAMBudgetMonitor.h
 * @brief GPU memory budget monitoring via DXGI adapter queries
 *
 * Tracks dedicated VRAM availability and current usage, computes a
 * memory-pressure level, and recommends a texture budget that the
 * TextureSystem can use to drive LRU eviction.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include <cstddef>
#include <cstdint>

/**
 * @brief Coarse memory-pressure classification used by subsystems to
 *        throttle quality or trigger eviction.
 */
enum class VRAMPressureLevel : uint8_t
{
    None,    ///< Usage < 60 % of budget
    Low,     ///< 60-75 %
    Medium,  ///< 75-85 %
    High,    ///< 85-95 %
    Critical ///< > 95 %
};

/**
 * @brief The source selected for a user-visible VRAM usage reading.
 *
 * A few WDDM/driver combinations can report a transient zero from
 * QueryVideoMemoryInfo while the engine already owns GPU resources.  Keep
 * that case distinguishable from a genuine live zero instead of presenting a
 * zero as authoritative telemetry.
 */
struct VRAMUsageSelection
{
    size_t bytes = 0;
    bool fromLiveQuery = false;
    bool liveQueryReportedZero = false;
};

/**
 * @brief Select a truthful reading from live DXGI data and an existing
 *        engine-side estimate.
 */
inline VRAMUsageSelection SelectVRAMUsage(size_t liveUsage, bool liveAvailable, size_t estimate) noexcept
{
    if (liveAvailable && (liveUsage != 0 || estimate == 0))
        return {liveUsage, true, false};

    return {estimate, false, liveAvailable && liveUsage == 0 && estimate != 0};
}

/**
 * @brief Monitors GPU dedicated-memory budget via DXGI and recommends a
 *        texture-cache budget to the TextureSystem.
 *
 * On Windows, queries IDXGIAdapter3::QueryVideoMemoryInfo each Update().
 * On non-Windows or when DXGI queries are unavailable, falls back to the
 * static adapter description (DedicatedVideoMemory) captured at init time.
 */
class VRAMBudgetMonitor
{
  public:
    VRAMBudgetMonitor() = default;
    ~VRAMBudgetMonitor() = default;

    // Non-copyable, non-movable (owns COM pointers)
    VRAMBudgetMonitor(const VRAMBudgetMonitor&) = delete;
    VRAMBudgetMonitor& operator=(const VRAMBudgetMonitor&) = delete;

    /**
     * @brief Initialize the monitor from a D3D11 device.
     * @param device  The D3D11 device (used to obtain the DXGI adapter).
     * @return S_OK on success, or an error HRESULT.
     */
    HRESULT Initialize(ID3D11Device* device);

    /** @brief Release DXGI references. */
    void Shutdown();

    /**
     * @brief Re-query the adapter for current VRAM usage and budget.
     *
     * Lightweight — suitable for calling once per frame.
     */
    void Update();

    // --- Accessors ---

    /** @brief Total dedicated VRAM reported by the adapter (bytes). */
    size_t GetTotalVRAM() const { return m_totalVRAM; }

    /** @brief Current VRAM usage from the last successful DXGI query (bytes). */
    size_t GetCurrentUsage() const { return m_currentUsage; }

    /** @brief Whether the current-usage value came from a successful query. */
    bool IsCurrentUsageValid() const { return m_currentUsageValid; }

    /** @brief OS-managed VRAM budget the process should stay under (bytes). */
    size_t GetBudget() const { return m_budget; }

    /** @brief Recommended texture-cache budget derived from the OS budget (bytes). */
    size_t GetRecommendedTextureBudget() const { return m_recommendedTextureBudget; }

    /** @brief Current memory-pressure level. */
    VRAMPressureLevel GetPressureLevel() const { return m_pressureLevel; }

    /** @brief Pressure as a 0.0 – 1.0 ratio (currentUsage / budget). */
    float GetPressureRatio() const { return m_pressureRatio; }

    /** @brief Whether dynamic DXGI budget queries are supported. */
    bool IsQuerySupported() const { return m_querySupported; }

  private:
    void ComputePressure();

#ifdef SPARK_PLATFORM_WINDOWS
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_adapter3;
#endif

    size_t m_totalVRAM = 0;
    size_t m_currentUsage = 0;
    size_t m_budget = 0;
    size_t m_recommendedTextureBudget = 512 * 1024 * 1024; // 512 MB default
    float m_pressureRatio = 0.0f;
    VRAMPressureLevel m_pressureLevel = VRAMPressureLevel::None;
    bool m_querySupported = false;
    bool m_currentUsageValid = false;
    bool m_initialized = false;
};
