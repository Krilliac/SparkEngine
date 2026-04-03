/**
 * @file GPUResourceLeakDetector.h
 * @brief Tracks GPU resource lifecycle to detect leaks and budget violations
 * @author Spark Engine Team
 * @date 2026
 *
 * Monitors GPU resource allocations and deallocations (textures, buffers,
 * render targets, shaders, pipeline states, samplers). Detects resources
 * that remain alive without access longer than a configurable threshold,
 * and warns when estimated GPU memory usage exceeds a budget.
 *
 * Usage:
 * @code
 *   auto& detector = Spark::GPUResourceLeakDetector::GetInstance();
 *   detector.Initialize();
 *   detector.OnResourceCreated(GPUResourceType::Texture, id, sizeBytes);
 *   // ... resource used ...
 *   detector.OnResourceDestroyed(GPUResourceType::Texture, id);
 *   // Each frame:
 *   detector.Update(dt);
 * @endcode
 *
 * @note Active in Debug and Release builds. Compiled to no-ops in SPARK_SHIPPING.
 * @see FreezeDetector.h, HitchDetector.h
 */

#pragma once

#include "../Core/Platform.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Spark
{

    /**
     * @brief GPU resource type classification for leak tracking
     */
    enum class GPUResourceType : uint8_t
    {
        Texture,
        Buffer,
        RenderTarget,
        Shader,
        PipelineState,
        Sampler,
        Count ///< Sentinel — number of resource types
    };

    static constexpr uint32_t GPU_RESOURCE_TYPE_COUNT = static_cast<uint32_t>(GPUResourceType::Count);

    /**
     * @brief Configuration for GPU resource leak detection
     */
    struct GPUResourceLeakConfig
    {
        /// Seconds a resource can be alive without access before being flagged as a suspected leak
        float leakAgeThresholdSec = 30.0f;

        /// Only scan for leaks every N frames (performance guard)
        uint32_t checkIntervalFrames = 60;

        /// Total GPU memory budget (MB) — warn when exceeded
        float budgetMB = 512.0f;

        /// Whether the detector is enabled
        bool enabled = true;
    };

    /**
     * @brief Snapshot of GPU resource leak detector status for diagnostics
     */
    struct GPUResourceLeakStatus
    {
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> allocatedByType{};
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> freedByType{};
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> outstandingByType{};
        uint32_t suspectedLeaks = 0;
        uint32_t totalAllocations = 0;
        uint32_t totalFrees = 0;
        float estimatedMemoryMB = 0.0f;
        float peakMemoryMB = 0.0f;
    };

    /**
     * @brief Singleton detector for GPU resource leaks and budget violations
     *
     * Instrument RHI resource creation/destruction with OnResourceCreated/Destroyed.
     * Call Update(dt) every frame to advance timers and scan for leaks.
     */
    class GPUResourceLeakDetector
    {
      public:
        static GPUResourceLeakDetector& GetInstance();

        void Initialize();
        void Update(float dt);
        void Shutdown();

        void Configure(const GPUResourceLeakConfig& config);
        [[nodiscard]] const GPUResourceLeakConfig& GetConfig() const { return m_config; }

        [[nodiscard]] GPUResourceLeakStatus GetStatus() const;

        /// @brief Track a newly created GPU resource
        void OnResourceCreated(GPUResourceType type, uint64_t id, size_t sizeBytes);

        /// @brief Track a destroyed GPU resource
        void OnResourceDestroyed(GPUResourceType type, uint64_t id);

        /// @brief Mark a resource as recently accessed (resets leak timer)
        void OnResourceAccessed(uint64_t id);

        /// @brief Get a formatted per-type breakdown for console display
        [[nodiscard]] std::string FormatDetails() const;

        void RegisterConsoleCommands();

      private:
        GPUResourceLeakDetector() = default;
        ~GPUResourceLeakDetector() = default;
        GPUResourceLeakDetector(const GPUResourceLeakDetector&) = delete;
        GPUResourceLeakDetector& operator=(const GPUResourceLeakDetector&) = delete;

        void ScanForLeaks();

        struct TrackedResource
        {
            GPUResourceType type = GPUResourceType::Texture;
            size_t sizeBytes = 0;
            float ageSec = 0.0f;
            float lastAccessSec = 0.0f;
        };

        GPUResourceLeakConfig m_config;
        std::unordered_map<uint64_t, TrackedResource> m_resources;

        // Per-type counters
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> m_allocatedByType{};
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> m_freedByType{};

        // Memory tracking
        size_t m_currentMemoryBytes = 0;
        size_t m_peakMemoryBytes = 0;

        // Leak scanning
        uint32_t m_frameCounter = 0;
        uint32_t m_suspectedLeaks = 0;

        bool m_initialized = false;
    };

} // namespace Spark

#if !defined(SPARK_SHIPPING)
#define SPARK_GPU_LEAK_UPDATE(dt) Spark::GPUResourceLeakDetector::GetInstance().Update(dt)
#else
#define SPARK_GPU_LEAK_UPDATE(dt) ((void)0)
#endif
