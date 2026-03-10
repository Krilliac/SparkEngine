/**
 * @file ResourceResidencyManager.h
 * @brief GPU resource residency management with LRU eviction and system RAM staging
 * @author Spark Engine Team
 * @date 2026
 *
 * Addresses GAP-OFF02: Defines residency states (Resident, Evicted, Streaming, Pinned),
 * tracks last-used frame per resource with LRU eviction, maintains a system RAM staging
 * pool for evicted resources, and provides an async re-upload pipeline.
 */

#pragma once

#include "../Core/Platform.h"
#include "VRAMBudgetMonitor.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <functional>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <memory>

// ============================================================================
// RESIDENCY STATES
// ============================================================================

/**
 * @brief Residency state of a GPU resource
 */
enum class ResidencyState : uint8_t
{
    Resident,   ///< Data is in VRAM, GPU handle is valid
    Evicted,    ///< Data is in system RAM staging pool, GPU handle released
    Streaming,  ///< Transfer in progress (evicting or re-uploading)
    Pinned      ///< Never evict — render targets, G-buffers, back buffer
};

/**
 * @brief Convert residency state to string
 */
inline const char* ResidencyStateToString(ResidencyState state)
{
    switch (state)
    {
    case ResidencyState::Resident:
        return "Resident";
    case ResidencyState::Evicted:
        return "Evicted";
    case ResidencyState::Streaming:
        return "Streaming";
    case ResidencyState::Pinned:
        return "Pinned";
    default:
        return "Unknown";
    }
}

// ============================================================================
// RESOURCE TYPE CLASSIFICATION
// ============================================================================

/**
 * @brief Type of GPU resource for eviction priority ordering
 *
 * Priority ordering for eviction (evict first at top):
 *   DistantTexture > SceneTexture > ShadowMap > RenderTarget
 */
enum class GPUResourceType : uint8_t
{
    DistantTexture,   ///< Textures far from camera — evict first
    SceneTexture,     ///< Standard scene textures
    MeshBuffer,       ///< Vertex/index buffers
    ShadowMap,        ///< Shadow map textures
    ConstantBuffer,   ///< Per-frame / per-object constant buffers
    RenderTarget      ///< Intermediate render targets — evict last (prefer Pinned)
};

// ============================================================================
// STAGING BUFFER — CPU-SIDE COPY OF EVICTED DATA
// ============================================================================

/**
 * @brief Holds a CPU-side copy of a GPU resource's data for fast re-upload
 */
struct StagingBuffer
{
    std::vector<uint8_t> data;          ///< Raw pixel/vertex data
    uint32_t width = 0;                 ///< Texture width (0 for buffers)
    uint32_t height = 0;               ///< Texture height (0 for buffers)
    uint32_t rowPitch = 0;             ///< Bytes per row (for textures)
    uint32_t mipLevels = 1;            ///< Number of mip levels stored
    uint32_t format = 0;               ///< DXGI_FORMAT cast to uint32_t
    GPUResourceType resourceType = GPUResourceType::SceneTexture;

    bool IsValid() const { return !data.empty(); }
    size_t GetSizeBytes() const { return data.size(); }
};

// ============================================================================
// RESIDENCY RECORD — PER-RESOURCE TRACKING
// ============================================================================

/**
 * @brief Complete residency record for a managed GPU resource
 */
struct ResidencyRecord
{
    uint64_t resourceId = 0;
    std::string name;
    uint64_t sizeBytes = 0;
    GPUResourceType type = GPUResourceType::SceneTexture;
    GPUResourcePriority priority = GPUResourcePriority::Normal;
    ResidencyState state = ResidencyState::Resident;

    /// LRU tracking
    uint64_t lastUsedFrame = 0;
    float screenCoverage = 0.0f;     ///< Approximate screen-space coverage (0..1)
    float distanceToCamera = 0.0f;   ///< Distance to the active camera

    /// CPU staging data (populated when evicted, cleared when re-uploaded)
    std::shared_ptr<StagingBuffer> stagingData;

    /// Callback to recreate the GPU resource from staging data
    std::function<bool(const StagingBuffer&)> reuploadCallback;

    /// Callback to read back GPU data into a staging buffer before eviction
    std::function<std::shared_ptr<StagingBuffer>()> readbackCallback;

    /**
     * @brief Compute composite eviction score (lower = evict first)
     */
    float ComputeEvictionScore(uint64_t currentFrame) const
    {
        if (state == ResidencyState::Pinned)
        {
            return std::numeric_limits<float>::max();
        }

        // Weight factors
        constexpr float PRIORITY_WEIGHT = 10000.0f;
        constexpr float RECENCY_WEIGHT = 1.0f;
        constexpr float COVERAGE_WEIGHT = 5000.0f;
        constexpr float TYPE_WEIGHT = 2000.0f;

        float score = 0.0f;

        // Priority contribution
        score += static_cast<float>(priority) * PRIORITY_WEIGHT;

        // Recency contribution (more recent = higher score)
        uint64_t frameAge = currentFrame - lastUsedFrame;
        score -= static_cast<float>(frameAge) * RECENCY_WEIGHT;

        // Screen coverage contribution (more visible = higher score)
        score += screenCoverage * COVERAGE_WEIGHT;

        // Resource type contribution (render targets > shadow maps > textures)
        score += static_cast<float>(type) * TYPE_WEIGHT;

        return score;
    }
};

// ============================================================================
// RE-UPLOAD REQUEST
// ============================================================================

/**
 * @brief Queued request to re-upload an evicted resource to the GPU
 */
struct ReuploadRequest
{
    uint64_t resourceId = 0;
    GPUResourcePriority priority = GPUResourcePriority::Normal;
    bool urgent = false;  ///< Skip queue and upload immediately

    /// Ordering: higher priority and urgent requests first
    bool operator<(const ReuploadRequest& other) const
    {
        if (urgent != other.urgent)
        {
            return !urgent;  // urgent items have higher priority in a max-heap
        }
        return priority < other.priority;
    }
};

// ============================================================================
// RESOURCE RESIDENCY MANAGER
// ============================================================================

/**
 * @brief Manages GPU resource residency with LRU eviction and async re-upload
 *
 * Thread safety:
 * - Public methods are thread-safe via internal mutex.
 * - Actual GPU operations (eviction, re-upload) must occur on the main render thread.
 *   Call ProcessPendingOperations() from the main thread each frame.
 * - The staging pool lives in system RAM and can be accessed from any thread.
 */
class ResourceResidencyManager
{
public:
    // ========================================================================
    // CONFIGURATION
    // ========================================================================

    struct Config
    {
        /// Maximum system RAM for staging pool (bytes). Default 1 GB.
        uint64_t maxStagingPoolBytes = 1024ULL * 1024ULL * 1024ULL;

        /// Maximum number of re-uploads per frame to avoid upload storms
        uint32_t maxReuploadsPerFrame = 4;

        /// Minimum frame age before a resource becomes eligible for eviction
        uint64_t minEvictionAge = 120;

        /// Target VRAM usage percentage after eviction pass
        float evictionTargetPercent = 70.0f;
    };

    // ========================================================================
    // CONSTRUCTION / DESTRUCTION
    // ========================================================================

    ResourceResidencyManager() = default;

    ~ResourceResidencyManager()
    {
        Shutdown();
    }

    // Non-copyable
    ResourceResidencyManager(const ResourceResidencyManager&) = delete;
    ResourceResidencyManager& operator=(const ResourceResidencyManager&) = delete;

    // ========================================================================
    // INITIALIZATION
    // ========================================================================

    /**
     * @brief Initialize with a reference to the VRAM budget monitor (default config)
     */
    void Initialize(VRAMBudgetMonitor* budgetMonitor)
    {
        Config defaultConfig;
        Initialize(budgetMonitor, defaultConfig);
    }

    /**
     * @brief Initialize with a reference to the VRAM budget monitor
     * @param budgetMonitor  The engine's VRAM budget monitor (must outlive this object)
     * @param config         Configuration parameters
     */
    void Initialize(VRAMBudgetMonitor* budgetMonitor, const Config& config)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_budgetMonitor = budgetMonitor;
        m_config = config;
        m_initialized = true;

        // Register as a pressure listener
        if (m_budgetMonitor)
        {
            m_pressureListenerId = m_budgetMonitor->AddPressureListener(
                "ResourceResidencyManager",
                [this](MemoryPressureLevel level, const GPUMemoryInfo& info)
                {
                    OnMemoryPressure(level, info);
                });
        }
    }

    /**
     * @brief Shut down and release all staging data
     */
    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_budgetMonitor && m_pressureListenerId != 0)
        {
            m_budgetMonitor->RemovePressureListener(m_pressureListenerId);
            m_pressureListenerId = 0;
        }

        m_records.clear();
        // Clear the reupload queue
        while (!m_reuploadQueue.empty())
        {
            m_reuploadQueue.pop();
        }
        m_stagingPoolBytes = 0;
        m_initialized = false;
    }

    // ========================================================================
    // RESOURCE REGISTRATION
    // ========================================================================

    /**
     * @brief Register a GPU resource for residency management
     */
    void RegisterResource(uint64_t resourceId, const std::string& name,
                          uint64_t sizeBytes, GPUResourceType type,
                          GPUResourcePriority priority = GPUResourcePriority::Normal)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        ResidencyRecord record;
        record.resourceId = resourceId;
        record.name = name;
        record.sizeBytes = sizeBytes;
        record.type = type;
        record.priority = priority;
        record.state = ResidencyState::Resident;
        record.lastUsedFrame = (m_budgetMonitor) ? m_budgetMonitor->GetCurrentFrame() : 0;

        m_records[resourceId] = std::move(record);
    }

    /**
     * @brief Unregister a resource (when it is destroyed)
     */
    void UnregisterResource(uint64_t resourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end())
        {
            // Free staging data if present
            if (it->second.stagingData)
            {
                m_stagingPoolBytes -= it->second.stagingData->GetSizeBytes();
            }
            m_records.erase(it);
        }
    }

    /**
     * @brief Pin a resource so it is never evicted
     */
    void PinResource(uint64_t resourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end())
        {
            it->second.state = ResidencyState::Pinned;
            it->second.priority = GPUResourcePriority::Pinned;
        }
    }

    /**
     * @brief Unpin a resource, making it eligible for eviction
     */
    void UnpinResource(uint64_t resourceId, GPUResourcePriority newPriority = GPUResourcePriority::Normal)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end() && it->second.state == ResidencyState::Pinned)
        {
            it->second.state = ResidencyState::Resident;
            it->second.priority = newPriority;
        }
    }

    /**
     * @brief Set readback callback for a resource (used during eviction to save data)
     */
    void SetReadbackCallback(uint64_t resourceId,
                             std::function<std::shared_ptr<StagingBuffer>()> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end())
        {
            it->second.readbackCallback = std::move(callback);
        }
    }

    /**
     * @brief Set re-upload callback for a resource (used to restore from staging)
     */
    void SetReuploadCallback(uint64_t resourceId,
                             std::function<bool(const StagingBuffer&)> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end())
        {
            it->second.reuploadCallback = std::move(callback);
        }
    }

    // ========================================================================
    // PER-FRAME UPDATE
    // ========================================================================

    /**
     * @brief Mark a resource as used this frame
     */
    void TouchResource(uint64_t resourceId, float screenCoverage = 0.0f,
                       float distanceToCamera = 0.0f)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end())
        {
            uint64_t currentFrame = (m_budgetMonitor) ? m_budgetMonitor->GetCurrentFrame() : 0;
            it->second.lastUsedFrame = currentFrame;
            it->second.screenCoverage = screenCoverage;
            it->second.distanceToCamera = distanceToCamera;
        }
    }

    /**
     * @brief Request a resource to be made resident (re-uploaded from staging)
     *
     * If the resource is already resident, this is a no-op.
     * If evicted, it queues a re-upload request.
     *
     * @return true if the resource is currently resident
     */
    bool EnsureResident(uint64_t resourceId, bool urgent = false)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it == m_records.end())
        {
            return false;
        }

        if (it->second.state == ResidencyState::Resident ||
            it->second.state == ResidencyState::Pinned)
        {
            return true;
        }

        if (it->second.state == ResidencyState::Evicted && it->second.stagingData)
        {
            // Queue for re-upload
            ReuploadRequest request;
            request.resourceId = resourceId;
            request.priority = it->second.priority;
            request.urgent = urgent;
            m_reuploadQueue.push(request);

            it->second.state = ResidencyState::Streaming;
        }

        return false;
    }

    /**
     * @brief Process pending evictions and re-uploads (call from main render thread)
     *
     * This performs the actual GPU operations: reading back data for eviction
     * and re-uploading staging data for restoration.
     */
    void ProcessPendingOperations()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized)
        {
            return;
        }

        // Process re-uploads (up to maxReuploadsPerFrame)
        uint32_t reuploadsProcessed = 0;
        while (!m_reuploadQueue.empty() && reuploadsProcessed < m_config.maxReuploadsPerFrame)
        {
            ReuploadRequest request = m_reuploadQueue.top();
            m_reuploadQueue.pop();

            auto it = m_records.find(request.resourceId);
            if (it == m_records.end() || it->second.state != ResidencyState::Streaming)
            {
                continue;
            }

            ResidencyRecord& record = it->second;
            if (record.stagingData && record.reuploadCallback)
            {
                bool success = record.reuploadCallback(*record.stagingData);
                if (success)
                {
                    // Free staging data and mark as resident
                    m_stagingPoolBytes -= record.stagingData->GetSizeBytes();
                    record.stagingData.reset();
                    record.state = ResidencyState::Resident;

                    // Update budget monitor
                    if (m_budgetMonitor)
                    {
                        m_budgetMonitor->MarkResourceResident(record.resourceId);
                    }

                    reuploadsProcessed++;
                }
                else
                {
                    // Re-upload failed — keep in evicted state
                    record.state = ResidencyState::Evicted;
                }
            }
            else
            {
                // No staging data or callback — cannot restore
                record.state = ResidencyState::Evicted;
            }
        }

        // Process pending evictions
        ProcessPendingEvictions();
    }

    // ========================================================================
    // EVICTION
    // ========================================================================

    /**
     * @brief Trigger an eviction pass to free VRAM
     *
     * Evicts lowest-priority, least-recently-used resources until usage
     * drops below the target percentage or no more candidates remain.
     *
     * @param targetFreeBytes  Minimum bytes to free (0 = use config target)
     * @return Number of resources evicted
     */
    uint32_t EvictResources(uint64_t targetFreeBytes = 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return EvictResourcesInternal(targetFreeBytes);
    }

    // ========================================================================
    // QUERIES
    // ========================================================================

    /**
     * @brief Get the residency state of a resource
     */
    ResidencyState GetResourceState(uint64_t resourceId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end())
        {
            return it->second.state;
        }
        return ResidencyState::Evicted;  // Unknown resource treated as evicted
    }

    /**
     * @brief Get the full residency record for a resource
     */
    bool GetResidencyRecord(uint64_t resourceId, ResidencyRecord& outRecord) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(resourceId);
        if (it != m_records.end())
        {
            outRecord = it->second;
            return true;
        }
        return false;
    }

    /**
     * @brief Get total system RAM used by staging pool
     */
    uint64_t GetStagingPoolUsage() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stagingPoolBytes;
    }

    /**
     * @brief Get total VRAM tracked as resident by this manager
     */
    uint64_t GetResidentMemoryBytes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint64_t total = 0;
        for (const auto& [id, record] : m_records)
        {
            if (record.state == ResidencyState::Resident ||
                record.state == ResidencyState::Pinned)
            {
                total += record.sizeBytes;
            }
        }
        return total;
    }

    /**
     * @brief Get total number of managed resources
     */
    size_t GetManagedResourceCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_records.size();
    }

    /**
     * @brief Get count of resources in each state
     */
    struct ResidencyStats
    {
        uint32_t residentCount = 0;
        uint32_t evictedCount = 0;
        uint32_t streamingCount = 0;
        uint32_t pinnedCount = 0;
        uint64_t residentBytes = 0;
        uint64_t evictedBytes = 0;
        uint64_t stagingBytes = 0;
    };

    ResidencyStats GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        ResidencyStats stats;
        for (const auto& [id, record] : m_records)
        {
            switch (record.state)
            {
            case ResidencyState::Resident:
                stats.residentCount++;
                stats.residentBytes += record.sizeBytes;
                break;
            case ResidencyState::Evicted:
                stats.evictedCount++;
                stats.evictedBytes += record.sizeBytes;
                break;
            case ResidencyState::Streaming:
                stats.streamingCount++;
                break;
            case ResidencyState::Pinned:
                stats.pinnedCount++;
                stats.residentBytes += record.sizeBytes;
                break;
            }
        }
        stats.stagingBytes = m_stagingPoolBytes;
        return stats;
    }

    /**
     * @brief Get current config
     */
    const Config& GetConfig() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config;
    }

    /**
     * @brief Update config at runtime
     */
    void SetConfig(const Config& config)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
    }

private:
    // ========================================================================
    // INTERNAL METHODS
    // ========================================================================

    /**
     * @brief Memory pressure callback — triggers eviction at appropriate levels
     */
    void OnMemoryPressure(MemoryPressureLevel level, const GPUMemoryInfo& info)
    {
        // This runs under the budget monitor's lock, so we must NOT lock m_mutex
        // here to avoid deadlock. Instead, set a flag and process in the next
        // ProcessPendingOperations() call.
        m_pendingPressureLevel.store(static_cast<int>(level), std::memory_order_release);
    }

    /**
     * @brief Process pending evictions triggered by memory pressure
     *
     * Called from ProcessPendingOperations() under m_mutex.
     */
    void ProcessPendingEvictions()
    {
        int pressureInt = m_pendingPressureLevel.exchange(-1, std::memory_order_acquire);
        if (pressureInt < 0)
        {
            return;  // No pending pressure event
        }

        auto level = static_cast<MemoryPressureLevel>(pressureInt);

        switch (level)
        {
        case MemoryPressureLevel::Warning:
            // Evict ~10% of tracked resident memory
            EvictResourcesInternal(GetResidentMemoryBytesInternal() / 10);
            break;

        case MemoryPressureLevel::Critical:
            // Evict ~25% of tracked resident memory
            EvictResourcesInternal(GetResidentMemoryBytesInternal() / 4);
            break;

        case MemoryPressureLevel::Emergency:
            // Evict ~50% of tracked resident memory
            EvictResourcesInternal(GetResidentMemoryBytesInternal() / 2);
            break;

        case MemoryPressureLevel::None:
        default:
            break;
        }
    }

    /**
     * @brief Internal eviction logic (must be called under m_mutex)
     */
    uint32_t EvictResourcesInternal(uint64_t targetFreeBytes)
    {
        if (m_records.empty())
        {
            return 0;
        }

        uint64_t currentFrame = (m_budgetMonitor) ? m_budgetMonitor->GetCurrentFrame() : 0;

        // Build sorted eviction candidate list
        std::vector<std::pair<uint64_t, float>> candidates;  // {resourceId, score}
        candidates.reserve(m_records.size());

        for (const auto& [id, record] : m_records)
        {
            if (record.state == ResidencyState::Resident &&
                record.priority != GPUResourcePriority::Pinned)
            {
                // Check minimum age
                if (currentFrame - record.lastUsedFrame >= m_config.minEvictionAge)
                {
                    float score = record.ComputeEvictionScore(currentFrame);
                    candidates.emplace_back(id, score);
                }
            }
        }

        // Sort ascending by score — lowest score = evict first
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });

        uint64_t freedBytes = 0;
        uint32_t evictedCount = 0;

        for (const auto& [candidateId, score] : candidates)
        {
            if (targetFreeBytes > 0 && freedBytes >= targetFreeBytes)
            {
                break;
            }

            auto it = m_records.find(candidateId);
            if (it == m_records.end())
            {
                continue;
            }

            ResidencyRecord& record = it->second;

            // Check staging pool capacity
            if (m_stagingPoolBytes + record.sizeBytes > m_config.maxStagingPoolBytes)
            {
                // Try to trim old staging data to make room
                TrimStagingPool(record.sizeBytes);
                if (m_stagingPoolBytes + record.sizeBytes > m_config.maxStagingPoolBytes)
                {
                    continue;  // Cannot stage this resource
                }
            }

            // Read back data to staging if callback is available
            if (record.readbackCallback)
            {
                auto staging = record.readbackCallback();
                if (staging && staging->IsValid())
                {
                    m_stagingPoolBytes += staging->GetSizeBytes();
                    record.stagingData = std::move(staging);
                }
            }

            // Mark as evicted
            record.state = ResidencyState::Evicted;
            freedBytes += record.sizeBytes;
            evictedCount++;

            // Notify budget monitor
            if (m_budgetMonitor)
            {
                m_budgetMonitor->MarkResourceEvicted(candidateId);
            }
        }

        return evictedCount;
    }

    /**
     * @brief Get resident memory total (internal, no lock)
     */
    uint64_t GetResidentMemoryBytesInternal() const
    {
        uint64_t total = 0;
        for (const auto& [id, record] : m_records)
        {
            if (record.state == ResidencyState::Resident ||
                record.state == ResidencyState::Pinned)
            {
                total += record.sizeBytes;
            }
        }
        return total;
    }

    /**
     * @brief Trim the staging pool by removing oldest evicted entries
     *
     * Frees staging data for the oldest evicted resources to make room
     * for new evictions. This means those resources will need disk I/O
     * to restore, but prevents unbounded system RAM growth.
     */
    void TrimStagingPool(uint64_t bytesNeeded)
    {
        // Gather evicted records with staging data, sorted by lastUsedFrame ascending
        std::vector<uint64_t> trimCandidates;
        for (const auto& [id, record] : m_records)
        {
            if (record.state == ResidencyState::Evicted && record.stagingData)
            {
                trimCandidates.push_back(id);
            }
        }

        // Sort by last used frame ascending (oldest first)
        std::sort(trimCandidates.begin(), trimCandidates.end(),
                  [this](uint64_t a, uint64_t b)
                  {
                      return m_records.at(a).lastUsedFrame < m_records.at(b).lastUsedFrame;
                  });

        uint64_t freedBytes = 0;
        for (uint64_t candidateId : trimCandidates)
        {
            if (freedBytes >= bytesNeeded)
            {
                break;
            }

            auto it = m_records.find(candidateId);
            if (it != m_records.end() && it->second.stagingData)
            {
                freedBytes += it->second.stagingData->GetSizeBytes();
                m_stagingPoolBytes -= it->second.stagingData->GetSizeBytes();
                it->second.stagingData.reset();
            }
        }
    }

    // ========================================================================
    // MEMBER DATA
    // ========================================================================

    mutable std::mutex m_mutex;
    bool m_initialized = false;

    VRAMBudgetMonitor* m_budgetMonitor = nullptr;
    uint32_t m_pressureListenerId = 0;
    Config m_config;

    /// Per-resource residency records
    std::unordered_map<uint64_t, ResidencyRecord> m_records;

    /// Re-upload priority queue (max-heap by priority)
    std::priority_queue<ReuploadRequest> m_reuploadQueue;

    /// Staging pool tracking
    uint64_t m_stagingPoolBytes = 0;

    /// Atomic flag for deferred pressure handling (avoids deadlock with budget monitor)
    std::atomic<int> m_pendingPressureLevel{-1};
};
