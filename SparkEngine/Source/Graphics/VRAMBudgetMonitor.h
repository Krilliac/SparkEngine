/**
 * @file VRAMBudgetMonitor.h
 * @brief Real-time VRAM budget monitoring with pressure detection and event broadcasting
 * @author Spark Engine Team
 * @date 2026
 *
 * Addresses GAP-OFF01: Queries IDXGIAdapter3::QueryVideoMemoryInfo() for real-time
 * VRAM usage, defines memory pressure thresholds, broadcasts pressure events, and
 * tracks per-resource memory with priority metadata.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <functional>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <chrono>

// ============================================================================
// MEMORY PRESSURE LEVELS
// ============================================================================

/**
 * @brief Escalating memory pressure levels that trigger different offloading responses
 */
enum class MemoryPressureLevel
{
    None,     ///< Usage < 75% — no action needed
    Warning,  ///< Usage 75%-85% — begin proactive eviction of low-priority resources
    Critical, ///< Usage 85%-95% — aggressive eviction, quality reduction
    Emergency ///< Usage > 95% — emergency eviction, disable optional features
};

/**
 * @brief Convert pressure level to a human-readable string
 */
inline const char* MemoryPressureLevelToString(MemoryPressureLevel level)
{
    switch (level)
    {
    case MemoryPressureLevel::None:
        return "None";
    case MemoryPressureLevel::Warning:
        return "Warning";
    case MemoryPressureLevel::Critical:
        return "Critical";
    case MemoryPressureLevel::Emergency:
        return "Emergency";
    default:
        return "Unknown";
    }
}

// ============================================================================
// GPU MEMORY INFO
// ============================================================================

/**
 * @brief Snapshot of current GPU memory state from DXGI
 */
struct GPUMemoryInfo
{
    uint64_t budgetBytes = 0;       ///< OS-provided VRAM budget (may change at runtime)
    uint64_t currentUsageBytes = 0; ///< Current VRAM consumption (including driver overhead)
    uint64_t availableBytes = 0;    ///< Remaining budget before the OS starts paging
    float usagePercent = 0.0f;      ///< currentUsageBytes / budgetBytes * 100
    MemoryPressureLevel pressureLevel = MemoryPressureLevel::None;

    /// Non-local (shared system RAM) segment info
    uint64_t sharedBudgetBytes = 0;
    uint64_t sharedUsageBytes = 0;
    float sharedUsagePercent = 0.0f;
};

// ============================================================================
// GPU RESOURCE PRIORITY
// ============================================================================

/**
 * @brief Priority class for GPU resources — higher priority means later eviction
 */
enum class GPUResourcePriority : uint8_t
{
    Background = 0, ///< Distant textures, low-impact resources — evict first
    Low = 1,        ///< Scene textures not in immediate view
    Normal = 2,     ///< Standard scene textures, meshes
    High = 3,       ///< Shadow maps, frequently used materials
    Critical = 4,   ///< Render targets, G-buffers — evict last
    Pinned = 5      ///< Never evict (back buffer, depth buffer, essential RT)
};

// ============================================================================
// GPU RESOURCE ENTRY
// ============================================================================

/**
 * @brief Tracks a single GPU resource allocation with metadata for eviction decisions
 */
struct GPUResourceEntry
{
    uint64_t resourceId = 0; ///< Unique identifier
    std::string name;        ///< Debug name
    uint64_t sizeBytes = 0;  ///< Allocation size in bytes
    GPUResourcePriority priority = GPUResourcePriority::Normal;
    uint64_t lastUsedFrame = 0;                         ///< Frame number when last bound/used
    std::chrono::steady_clock::time_point lastUsedTime; ///< Wall-clock time of last use
    bool isResident = true;                             ///< Currently in VRAM

    /**
     * @brief Compute eviction score — lower score = evict first
     *
     * Combines priority and recency. Pinned resources return max score.
     */
    float GetEvictionScore(uint64_t currentFrame) const
    {
        if (priority == GPUResourcePriority::Pinned)
        {
            return std::numeric_limits<float>::max();
        }

        float priorityWeight = static_cast<float>(priority) * 1000.0f;
        float recencyWeight = static_cast<float>(currentFrame - lastUsedFrame);
        // Higher priority and more recent use => higher score => less likely to evict
        return priorityWeight - recencyWeight;
    }
};

// ============================================================================
// MEMORY PRESSURE CALLBACK
// ============================================================================

/**
 * @brief Callback signature for memory pressure events
 * @param level Current pressure level
 * @param info Full memory info snapshot
 */
using MemoryPressureCallback = std::function<void(MemoryPressureLevel level, const GPUMemoryInfo& info)>;

// ============================================================================
// VRAM BUDGET MONITOR
// ============================================================================

/**
 * @brief Monitors real-time VRAM usage via DXGI and broadcasts memory pressure events
 *
 * Thread safety:
 * - All public methods are thread-safe via internal mutex.
 * - DXGI queries MUST run on the main thread (the thread that created the device).
 *   Call Update() from the main render thread only.
 * - Listener callbacks are invoked under the lock; keep them lightweight.
 */
class VRAMBudgetMonitor
{
  public:
    // ========================================================================
    // CONFIGURATION
    // ========================================================================

    /**
     * @brief Configurable pressure thresholds (percentage of budget)
     */
    struct PressureThresholds
    {
        float warningPercent = 75.0f;   ///< Transition to Warning above this
        float criticalPercent = 85.0f;  ///< Transition to Critical above this
        float emergencyPercent = 95.0f; ///< Transition to Emergency above this
    };

    // ========================================================================
    // CONSTRUCTION / DESTRUCTION
    // ========================================================================

    VRAMBudgetMonitor() = default;
    ~VRAMBudgetMonitor() = default;

    // Non-copyable, non-movable
    VRAMBudgetMonitor(const VRAMBudgetMonitor&) = delete;
    VRAMBudgetMonitor& operator=(const VRAMBudgetMonitor&) = delete;

    // ========================================================================
    // INITIALIZATION
    // ========================================================================

    /**
     * @brief Initialize the monitor with a DXGI adapter
     *
     * On Windows, attempts to query IDXGIAdapter3 for real-time memory info.
     * On non-Windows platforms, falls back to internal tracking only.
     *
     * @param device  The D3D11 device (used to retrieve the DXGI adapter)
     * @return true if DXGI adapter was successfully acquired
     */
#ifdef SPARK_PLATFORM_WINDOWS
    bool Initialize(ID3D11Device* device)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!device)
        {
            return false;
        }

        // Get DXGI device -> adapter -> adapter3
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
        if (FAILED(hr))
        {
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
        if (FAILED(hr))
        {
            return false;
        }

        hr = adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(m_adapter3.GetAddressOf()));
        if (FAILED(hr))
        {
            // DXGI 1.4 not available — fall back to internal tracking
            m_adapter3.Reset();
            m_hasDXGIAdapter3 = false;
        }
        else
        {
            m_hasDXGIAdapter3 = true;
        }

        // Also store the basic adapter for static memory caps
        DXGI_ADAPTER_DESC adapterDesc = {};
        adapter->GetDesc(&adapterDesc);
        m_dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        m_sharedSystemMemory = adapterDesc.SharedSystemMemory;

        m_initialized = true;
        return true;
    }
#else
    bool Initialize(void* /*device*/)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_initialized = true;
        return true;
    }
#endif

    /**
     * @brief Shut down the monitor and release DXGI references
     */
    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

#ifdef SPARK_PLATFORM_WINDOWS
        m_adapter3.Reset();
        m_hasDXGIAdapter3 = false;
#endif

        m_resources.clear();
        m_listeners.clear();
        m_initialized = false;
    }

    // ========================================================================
    // PER-FRAME UPDATE (main thread only)
    // ========================================================================

    /**
     * @brief Query DXGI for current VRAM usage and evaluate pressure
     *
     * Must be called from the main render thread. Broadcasts pressure events
     * to all registered listeners when the pressure level changes.
     *
     * @param currentFrame  Current engine frame number (for LRU tracking)
     */
    void Update(uint64_t currentFrame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized)
        {
            return;
        }

        m_currentFrame = currentFrame;

        // Query real DXGI memory info
        GPUMemoryInfo newInfo = QueryGPUMemory();

        // Evaluate pressure level
        MemoryPressureLevel previousLevel = m_lastInfo.pressureLevel;
        newInfo.pressureLevel = EvaluatePressure(newInfo.usagePercent);

        m_lastInfo = newInfo;

        // Broadcast if pressure level changed
        if (newInfo.pressureLevel != previousLevel)
        {
            for (const auto& listener : m_listeners)
            {
                listener.callback(newInfo.pressureLevel, newInfo);
            }
        }
    }

    // ========================================================================
    // MEMORY INFO ACCESSORS
    // ========================================================================

    /**
     * @brief Get the most recent GPU memory snapshot
     */
    GPUMemoryInfo GetMemoryInfo() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastInfo;
    }

    /**
     * @brief Get the current memory pressure level
     */
    MemoryPressureLevel GetPressureLevel() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastInfo.pressureLevel;
    }

    /**
     * @brief Get the current frame number
     */
    uint64_t GetCurrentFrame() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentFrame;
    }

    // ========================================================================
    // PRESSURE THRESHOLD CONFIGURATION
    // ========================================================================

    /**
     * @brief Set custom pressure thresholds
     */
    void SetThresholds(const PressureThresholds& thresholds)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_thresholds = thresholds;
    }

    /**
     * @brief Get current pressure thresholds
     */
    PressureThresholds GetThresholds() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_thresholds;
    }

    // ========================================================================
    // PER-RESOURCE TRACKING
    // ========================================================================

    /**
     * @brief Register a GPU resource for tracking
     * @param resourceId  Unique identifier for the resource
     * @param name        Debug name
     * @param sizeBytes   Allocation size
     * @param priority    Eviction priority
     */
    void RegisterResource(uint64_t resourceId, const std::string& name, uint64_t sizeBytes,
                          GPUResourcePriority priority = GPUResourcePriority::Normal)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        GPUResourceEntry entry;
        entry.resourceId = resourceId;
        entry.name = name;
        entry.sizeBytes = sizeBytes;
        entry.priority = priority;
        entry.lastUsedFrame = m_currentFrame;
        entry.lastUsedTime = std::chrono::steady_clock::now();
        entry.isResident = true;

        m_resources[resourceId] = entry;
        m_trackedMemoryBytes += sizeBytes;
    }

    /**
     * @brief Unregister a GPU resource (e.g., when destroyed)
     */
    void UnregisterResource(uint64_t resourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_resources.find(resourceId);
        if (it != m_resources.end())
        {
            if (it->second.isResident)
            {
                m_trackedMemoryBytes -= it->second.sizeBytes;
            }
            m_resources.erase(it);
        }
    }

    /**
     * @brief Mark a resource as used this frame (updates LRU timestamp)
     */
    void TouchResource(uint64_t resourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_resources.find(resourceId);
        if (it != m_resources.end())
        {
            it->second.lastUsedFrame = m_currentFrame;
            it->second.lastUsedTime = std::chrono::steady_clock::now();
        }
    }

    /**
     * @brief Update a resource's priority
     */
    void SetResourcePriority(uint64_t resourceId, GPUResourcePriority priority)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_resources.find(resourceId);
        if (it != m_resources.end())
        {
            it->second.priority = priority;
        }
    }

    /**
     * @brief Mark a resource as evicted from VRAM
     */
    void MarkResourceEvicted(uint64_t resourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_resources.find(resourceId);
        if (it != m_resources.end() && it->second.isResident)
        {
            it->second.isResident = false;
            m_trackedMemoryBytes -= it->second.sizeBytes;
        }
    }

    /**
     * @brief Mark a resource as resident in VRAM
     */
    void MarkResourceResident(uint64_t resourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_resources.find(resourceId);
        if (it != m_resources.end() && !it->second.isResident)
        {
            it->second.isResident = true;
            m_trackedMemoryBytes += it->second.sizeBytes;
        }
    }

    /**
     * @brief Get eviction candidates sorted by eviction score (lowest first)
     * @param maxCount  Maximum number of candidates to return
     * @return Vector of resource entries sorted by eviction priority (evict first = front)
     */
    std::vector<GPUResourceEntry> GetEvictionCandidates(size_t maxCount = 64) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<GPUResourceEntry> candidates;
        candidates.reserve(m_resources.size());

        for (const auto& [id, entry] : m_resources)
        {
            if (entry.isResident && entry.priority != GPUResourcePriority::Pinned)
            {
                candidates.push_back(entry);
            }
        }

        // Sort by eviction score ascending — lowest score = evict first
        std::sort(candidates.begin(), candidates.end(), [this](const GPUResourceEntry& a, const GPUResourceEntry& b)
                  { return a.GetEvictionScore(m_currentFrame) < b.GetEvictionScore(m_currentFrame); });

        if (candidates.size() > maxCount)
        {
            candidates.resize(maxCount);
        }

        return candidates;
    }

    /**
     * @brief Get total tracked memory usage (internally tracked, not DXGI)
     */
    uint64_t GetTrackedMemoryBytes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_trackedMemoryBytes;
    }

    /**
     * @brief Get number of tracked resources
     */
    size_t GetTrackedResourceCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_resources.size();
    }

    // ========================================================================
    // EVENT LISTENERS
    // ========================================================================

    /**
     * @brief Listener registration entry
     */
    struct PressureListener
    {
        uint32_t id = 0;
        std::string name;
        MemoryPressureCallback callback;
    };

    /**
     * @brief Register a listener for memory pressure events
     * @param name      Debug name for the listener
     * @param callback  Function to call when pressure level changes
     * @return Listener ID for later removal
     */
    uint32_t AddPressureListener(const std::string& name, MemoryPressureCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint32_t id = m_nextListenerId++;

        PressureListener listener;
        listener.id = id;
        listener.name = name;
        listener.callback = std::move(callback);
        m_listeners.push_back(std::move(listener));

        return id;
    }

    /**
     * @brief Remove a previously registered listener
     */
    void RemovePressureListener(uint32_t listenerId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_listeners.erase(std::remove_if(m_listeners.begin(), m_listeners.end(),
                                         [listenerId](const PressureListener& l) { return l.id == listenerId; }),
                          m_listeners.end());
    }

    // ========================================================================
    // STATIC ADAPTER INFO
    // ========================================================================

    /**
     * @brief Get dedicated video memory reported at init (static, does not change)
     */
    uint64_t GetDedicatedVideoMemory() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_dedicatedVideoMemory;
    }

    /**
     * @brief Get shared system memory reported at init
     */
    uint64_t GetSharedSystemMemory() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sharedSystemMemory;
    }

    /**
     * @brief Check if real DXGI 1.4 memory queries are available
     */
    bool HasDXGIAdapter3() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
#ifdef SPARK_PLATFORM_WINDOWS
        return m_hasDXGIAdapter3;
#else
        return false;
#endif
    }

  private:
    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    /**
     * @brief Query DXGI adapter for real-time VRAM info
     *
     * Falls back to internal tracking if DXGI 1.4 is not available.
     */
    GPUMemoryInfo QueryGPUMemory() const
    {
        GPUMemoryInfo info;

#ifdef SPARK_PLATFORM_WINDOWS
        if (m_hasDXGIAdapter3 && m_adapter3)
        {
            // Query local (dedicated VRAM) segment
            DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
            HRESULT hr = m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);

            if (SUCCEEDED(hr))
            {
                info.budgetBytes = localInfo.Budget;
                info.currentUsageBytes = localInfo.CurrentUsage;
                info.availableBytes =
                    (localInfo.Budget > localInfo.CurrentUsage) ? (localInfo.Budget - localInfo.CurrentUsage) : 0;
                info.usagePercent =
                    (info.budgetBytes > 0)
                        ? (static_cast<float>(info.currentUsageBytes) / static_cast<float>(info.budgetBytes) * 100.0f)
                        : 0.0f;
            }

            // Query non-local (shared system RAM) segment
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo = {};
            hr = m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo);

            if (SUCCEEDED(hr))
            {
                info.sharedBudgetBytes = nonLocalInfo.Budget;
                info.sharedUsageBytes = nonLocalInfo.CurrentUsage;
                info.sharedUsagePercent = (nonLocalInfo.Budget > 0) ? (static_cast<float>(nonLocalInfo.CurrentUsage) /
                                                                       static_cast<float>(nonLocalInfo.Budget) * 100.0f)
                                                                    : 0.0f;
            }

            return info;
        }
#endif

        // Fallback: use internal tracking + static adapter memory
        info.budgetBytes = (m_dedicatedVideoMemory > 0) ? m_dedicatedVideoMemory : (512ULL * 1024ULL * 1024ULL);
        info.currentUsageBytes = m_trackedMemoryBytes;
        info.availableBytes =
            (info.budgetBytes > info.currentUsageBytes) ? (info.budgetBytes - info.currentUsageBytes) : 0;
        info.usagePercent =
            (info.budgetBytes > 0)
                ? (static_cast<float>(info.currentUsageBytes) / static_cast<float>(info.budgetBytes) * 100.0f)
                : 0.0f;

        return info;
    }

    /**
     * @brief Evaluate the pressure level for a given usage percentage
     */
    MemoryPressureLevel EvaluatePressure(float usagePercent) const
    {
        if (usagePercent >= m_thresholds.emergencyPercent)
        {
            return MemoryPressureLevel::Emergency;
        }
        if (usagePercent >= m_thresholds.criticalPercent)
        {
            return MemoryPressureLevel::Critical;
        }
        if (usagePercent >= m_thresholds.warningPercent)
        {
            return MemoryPressureLevel::Warning;
        }
        return MemoryPressureLevel::None;
    }

    // ========================================================================
    // MEMBER DATA
    // ========================================================================

    mutable std::mutex m_mutex;
    bool m_initialized = false;

#ifdef SPARK_PLATFORM_WINDOWS
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_adapter3;
    bool m_hasDXGIAdapter3 = false;
#endif

    /// Static adapter memory caps (read once at init)
    uint64_t m_dedicatedVideoMemory = 0;
    uint64_t m_sharedSystemMemory = 0;

    /// Per-resource tracking
    std::unordered_map<uint64_t, GPUResourceEntry> m_resources;
    uint64_t m_trackedMemoryBytes = 0;
    uint64_t m_currentFrame = 0;

    /// Pressure thresholds and state
    PressureThresholds m_thresholds;
    GPUMemoryInfo m_lastInfo;

    /// Event listeners
    std::vector<PressureListener> m_listeners;
    uint32_t m_nextListenerId = 1;
};
