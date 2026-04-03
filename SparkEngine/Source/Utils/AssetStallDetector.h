/**
 * @file AssetStallDetector.h
 * @brief Monitors asset loading pipeline for stalled or excessively slow requests
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks active asset load requests and flags any that exceed configurable time
 * thresholds. Detects stalled loads, queue overflows, and provides per-type
 * load time statistics.
 *
 * Usage:
 * @code
 *   auto& detector = Spark::AssetStallDetector::GetInstance();
 *   detector.Initialize();
 *   uint64_t id = detector.OnLoadBegin("player.mesh", AssetLoadType::Mesh);
 *   // ... loading happens ...
 *   detector.OnLoadComplete(id);
 *   // Each frame:
 *   detector.Update(dt);
 * @endcode
 *
 * @note Active in Debug and Release builds. Compiled to no-ops in SPARK_SHIPPING.
 * @see FreezeDetector.h, HitchDetector.h
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Spark
{

    /**
     * @brief Classification of asset types for load tracking
     */
    enum class AssetLoadType : uint8_t
    {
        Texture,
        Mesh,
        Audio,
        Script,
        Level,
        Other
    };

    /**
     * @brief Configuration for asset stall detection
     */
    struct AssetStallConfig
    {
        /// Seconds before logging a warning for a slow load
        float warningThresholdSec = 5.0f;

        /// Seconds before marking a load as stalled
        float stallThresholdSec = 10.0f;

        /// Warn if concurrent load count exceeds this
        uint32_t maxConcurrentLoads = 32;

        /// Whether the detector is enabled
        bool enabled = true;
    };

    /**
     * @brief Snapshot of asset stall detector status for diagnostics
     */
    struct AssetStallStatus
    {
        uint32_t activeLoads = 0;
        uint32_t stalledLoads = 0;
        uint32_t completedLoads = 0;
        uint32_t failedLoads = 0;
        float averageLoadTimeSec = 0.0f;
        float worstLoadTimeSec = 0.0f;
        uint32_t queueOverflowCount = 0;
    };

    /**
     * @brief Singleton detector for asset loading pipeline stalls
     *
     * Call OnLoadBegin/OnLoadComplete/OnLoadFailed at asset load boundaries.
     * Call Update(dt) every frame to advance timers and detect stalls.
     */
    class AssetStallDetector
    {
      public:
        static AssetStallDetector& GetInstance();

        void Initialize();
        void Update(float dt);
        void Shutdown();

        void Configure(const AssetStallConfig& config);
        [[nodiscard]] const AssetStallConfig& GetConfig() const { return m_config; }

        [[nodiscard]] AssetStallStatus GetStatus() const;

        /// @brief Begin tracking an asset load. Returns a unique ID for this request.
        uint64_t OnLoadBegin(const std::string& name, AssetLoadType type);

        /// @brief Mark a tracked load as complete.
        void OnLoadComplete(uint64_t id);

        /// @brief Mark a tracked load as failed.
        void OnLoadFailed(uint64_t id);

        /// @brief Get a formatted list of currently active loads for console display
        [[nodiscard]] std::string FormatActiveLoads() const;

        void RegisterConsoleCommands();

      private:
        AssetStallDetector() = default;
        ~AssetStallDetector() = default;
        AssetStallDetector(const AssetStallDetector&) = delete;
        AssetStallDetector& operator=(const AssetStallDetector&) = delete;

        void FinishLoad(uint64_t id, bool success);

        struct ActiveLoad
        {
            std::string name;
            AssetLoadType type = AssetLoadType::Other;
            float elapsedSec = 0.0f;
            bool warned = false;
        };

        AssetStallConfig m_config;
        std::unordered_map<uint64_t, ActiveLoad> m_activeLoads;
        uint64_t m_nextId = 1;

        // Statistics
        uint32_t m_completedLoads = 0;
        uint32_t m_failedLoads = 0;
        float m_totalLoadTimeSec = 0.0f;
        float m_worstLoadTimeSec = 0.0f;
        uint32_t m_queueOverflowCount = 0;

        bool m_initialized = false;
    };

} // namespace Spark

#if !defined(SPARK_SHIPPING)
#define SPARK_ASSET_STALL_UPDATE(dt) Spark::AssetStallDetector::GetInstance().Update(dt)
#else
#define SPARK_ASSET_STALL_UPDATE(dt) ((void)0)
#endif
