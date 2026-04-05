/**
 * @file MemoryIntegrity.h
 * @brief Runtime memory integrity and branch protection system
 * @author Spark Engine Team
 * @date 2026
 *
 * Detects runtime code tampering (NOP-patched branches, modified function
 * bodies) and validates that critical execution paths actually run. Inspired
 * by Warden-style code page scanning.
 *
 * Two layers of protection:
 * 1. **Code region scanning** — Snapshots FNV-1a hashes of executable memory
 *    at startup, periodically re-verifies them to detect patches.
 * 2. **Branch guards** — Lightweight macros that prove a critical branch
 *    executed, catching NOP slides and redirected jumps.
 *
 * Usage:
 * @code
 *   // Code region protection (manual)
 *   auto& mis = Spark::Security::MemoryIntegritySystem::GetInstance();
 *   mis.RegisterCodeRegion("DamageCalc", &CalcDamage, 256);
 *
 *   // Branch guard macros
 *   SPARK_BRANCH_GUARD_BEGIN("damage_validation")
 *   if (damage > 0.0f && damage < MAX_DAMAGE)
 *       ApplyDamage(target, damage);
 *   SPARK_BRANCH_GUARD_END("damage_validation")
 *
 *   // Simple checkpoint
 *   SPARK_INTEGRITY_CHECKPOINT("speed_check")
 *   // ... critical validation code ...
 *   SPARK_VERIFY_CHECKPOINT("speed_check")
 * @endcode
 *
 * @see Hash.h (FNV-1a hashing used for all checksums)
 */

#pragma once

#include "../../Core/Platform.h"
#include "../../Utils/Hash.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Security
{

    // =========================================================================
    // Violation classification
    // =========================================================================

    /// @brief Type of integrity violation detected
    enum class ViolationType : uint8_t
    {
        CodeModified,    ///< A registered code region's hash no longer matches
        BranchBypassed,  ///< A branch guard was never executed between verify calls
        BranchWrongPath, ///< A branch guard recorded an unexpected path value
        FunctionModified ///< A registered function body was modified
    };

    /// @brief Severity of an integrity violation
    enum class ViolationSeverity : uint8_t
    {
        Warning, ///< Suspicious but may be benign (e.g. first-time branch miss)
        Critical ///< Strong evidence of tampering
    };

    /// @brief Record of a single integrity violation
    struct Violation
    {
        ViolationType type = ViolationType::CodeModified;
        ViolationSeverity severity = ViolationSeverity::Warning;
        std::string regionName;
        uint64_t expectedHash = 0;
        uint64_t actualHash = 0;
        std::chrono::steady_clock::time_point timestamp;
    };

    // =========================================================================
    // Protected region tracking
    // =========================================================================

    /// @brief A registered region of code memory to monitor
    struct ProtectedRegion
    {
        std::string name;
        const void* address = nullptr;
        size_t size = 0;
        uint64_t expectedHash = 0;
    };

    /// @brief State for a single branch guard
    struct BranchGuardEntry
    {
        std::string name;
        uint32_t lastPathTaken = 0;  ///< Which branch path was recorded (0=none, 1=true, 2=else)
        uint64_t executionCount = 0; ///< Times RecordBranchExecution was called
        uint64_t verifyCount = 0;    ///< Times VerifyBranchExecuted was called
        uint64_t bypassCount = 0;    ///< Times verify found no execution since last verify
    };

    // =========================================================================
    // Diagnostics snapshot
    // =========================================================================

    /// @brief Summary metrics for the integrity system
    struct IntegrityMetrics
    {
        uint32_t regionsMonitored = 0;
        uint32_t branchGuardsRegistered = 0;
        uint32_t violationsDetected = 0;
        uint32_t scanCount = 0;
        float lastScanTimeSec = 0.0f;
        float timeSinceLastScanSec = 0.0f;
    };

    // =========================================================================
    // Configuration
    // =========================================================================

    /// @brief Configuration for scan timing and behavior
    struct IntegrityConfig
    {
        float scanIntervalSec = 5.0f;    ///< Seconds between periodic scans
        size_t batchSize = 8;            ///< Regions scanned per batch
        bool autoDiscoverRegions = true; ///< Use platform APIs to find code pages
        bool enabled = true;             ///< Master enable switch
    };

    // =========================================================================
    // Callback type
    // =========================================================================

    /// @brief Callback invoked on each violation (game code decides the response)
    using ViolationCallback = std::function<void(const Violation&)>;

    // =========================================================================
    // MemoryIntegritySystem
    // =========================================================================

    /**
     * @brief Singleton system that monitors code memory integrity and branch execution
     *
     * Feed it code regions and branch guards at startup. Call Update(dt) each
     * frame. It periodically re-hashes regions and flags violations via
     * EventBus + registered callback.
     */
    class MemoryIntegritySystem
    {
      public:
        static MemoryIntegritySystem& GetInstance();

        // ----- Lifecycle -----
        void Initialize();
        void Update(float dt);
        void Shutdown();

        // ----- Configuration -----
        void Configure(const IntegrityConfig& config);
        [[nodiscard]] const IntegrityConfig& GetConfig() const { return m_config; }

        // ----- Code region protection -----

        /// Register a region of memory to monitor for modifications
        void RegisterCodeRegion(std::string_view name, const void* address, size_t size);

        /// Convenience: register a function pointer with an estimated body size
        void RegisterFunction(std::string_view name, const void* funcPtr, size_t estimatedSize);

        /// Immediately scan all registered regions (returns true if all clean)
        bool ScanAllRegions();

        /// Scan a batch of regions (returns true if batch was clean)
        bool ScanRegionBatch(size_t batchSize);

        // ----- Branch guards -----

        /// Register a named branch guard (called automatically by macros)
        void RegisterBranchGuard(uint64_t id, std::string_view name);

        /// Record that a branch was taken with the given path ID
        void RecordBranchExecution(uint64_t id, uint32_t pathTaken);

        /// Verify that the branch was executed since the last verify call
        void VerifyBranchExecuted(uint64_t id);

        /// Check if a specific branch guard has been bypassed
        [[nodiscard]] bool WasBranchBypassed(uint64_t id) const;

        // ----- Violation callback -----
        void SetViolationCallback(ViolationCallback callback);

        // ----- Reporting -----
        [[nodiscard]] IntegrityMetrics GetMetrics() const;
        [[nodiscard]] const std::vector<Violation>& GetViolations() const { return m_violations; }
        [[nodiscard]] std::string GetStatusString() const;

        void RegisterConsoleCommands();

      private:
        MemoryIntegritySystem() = default;
        ~MemoryIntegritySystem() = default;
        MemoryIntegritySystem(const MemoryIntegritySystem&) = delete;
        MemoryIntegritySystem& operator=(const MemoryIntegritySystem&) = delete;

        /// Hash a region of memory using FNV-1a 64-bit
        [[nodiscard]] static uint64_t HashRegion(const void* address, size_t size);

        /// Report a violation (log, callback, event)
        void ReportViolation(const Violation& violation);

        /// Platform-specific: discover executable code pages
        void AutoDiscoverCodeRegions();

        IntegrityConfig m_config;

        // Protected regions
        std::vector<ProtectedRegion> m_regions;
        size_t m_scanCursor = 0;

        // Branch guards
        std::unordered_map<uint64_t, BranchGuardEntry> m_branchGuards;

        // Violations
        std::vector<Violation> m_violations;
        static constexpr size_t MAX_VIOLATIONS = 256;

        // Timing
        float m_timeSinceLastScan = 0.0f;
        uint32_t m_scanCount = 0;
        float m_lastScanDurationSec = 0.0f;

        // Callback
        ViolationCallback m_violationCallback;

        // Thread safety for branch guards (may be called from multiple threads)
        mutable std::mutex m_mutex;

        bool m_initialized = false;
    };

} // namespace Spark::Security

// =============================================================================
// Macros — developer-facing API for branch protection
// =============================================================================

/**
 * @brief Begin a protected branch scope. Place before the if/switch.
 * @param name  A compile-time string literal identifying this branch guard.
 *
 * The macro registers the guard (once) and records that the true-path executed.
 */
#define SPARK_BRANCH_GUARD_BEGIN(name)                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        constexpr uint64_t _sparkBranchId##__LINE__ = Spark::FNV1a64(name);                                            \
        static const bool _sparkBranchReg##__LINE__ = []                                                               \
        {                                                                                                              \
            Spark::Security::MemoryIntegritySystem::GetInstance().RegisterBranchGuard(_sparkBranchId##__LINE__, name); \
            return true;                                                                                               \
        }();                                                                                                           \
        (void)_sparkBranchReg##__LINE__;                                                                               \
        Spark::Security::MemoryIntegritySystem::GetInstance().RecordBranchExecution(_sparkBranchId##__LINE__, 1);      \
    } while (0);                                                                                                       \
    {

/**
 * @brief Record the else-path of a protected branch.
 * @param name  Must match the corresponding SPARK_BRANCH_GUARD_BEGIN name.
 */
#define SPARK_BRANCH_GUARD_ELSE(name)                                                                                  \
    Spark::Security::MemoryIntegritySystem::GetInstance().RecordBranchExecution(Spark::FNV1a64(name), 2);              \
    }                                                                                                                  \
    {

/**
 * @brief End a protected branch scope and verify it executed.
 * @param name  Must match the corresponding SPARK_BRANCH_GUARD_BEGIN name.
 */
#define SPARK_BRANCH_GUARD_END(name)                                                                                   \
    }                                                                                                                  \
    Spark::Security::MemoryIntegritySystem::GetInstance().VerifyBranchExecuted(Spark::FNV1a64(name));

/**
 * @brief Record that a critical code path executed (one-shot checkpoint).
 * @param name  A compile-time string literal identifying this checkpoint.
 */
#define SPARK_INTEGRITY_CHECKPOINT(name)                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        constexpr uint64_t _sparkCpId = Spark::FNV1a64(name);                                                          \
        static const bool _sparkCpReg = []                                                                             \
        {                                                                                                              \
            Spark::Security::MemoryIntegritySystem::GetInstance().RegisterBranchGuard(_sparkCpId, name);               \
            return true;                                                                                               \
        }();                                                                                                           \
        (void)_sparkCpReg;                                                                                             \
        Spark::Security::MemoryIntegritySystem::GetInstance().RecordBranchExecution(_sparkCpId, 1);                    \
    } while (0)

/**
 * @brief Verify that the matching SPARK_INTEGRITY_CHECKPOINT actually ran.
 * @param name  Must match the corresponding checkpoint name.
 */
#define SPARK_VERIFY_CHECKPOINT(name)                                                                                  \
    Spark::Security::MemoryIntegritySystem::GetInstance().VerifyBranchExecuted(Spark::FNV1a64(name))
