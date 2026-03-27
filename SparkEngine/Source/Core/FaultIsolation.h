/**
 * @file FaultIsolation.h
 * @brief Runtime fault isolation for engine subsystem updates
 *
 * Provides SubsystemFaultIsolator — a lightweight singleton that tracks which
 * subsystems have faulted at runtime and auto-disables them after repeated
 * failures. The SPARK_GUARDED_UPDATE macro wraps individual Update() calls
 * with try/catch and fault tracking so a crash in one subsystem does not
 * take down the entire engine.
 *
 * Initialization-time fault isolation is handled by EngineBootstrap.
 * This file covers the per-frame update path.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Spark
{

    /**
     * @brief Per-subsystem runtime fault record
     */
    struct SubsystemFaultRecord
    {
        uint32_t faultCount = 0;     ///< Number of times this subsystem has thrown
        uint32_t maxRetries = 0;     ///< Override for global max (0 = use global)
        bool enabled = true;         ///< false = skip Update() calls
        std::string lastError;       ///< Last exception message
        uint64_t lastFaultFrame = 0; ///< Frame number of the most recent fault
    };

    /**
     * @brief Tracks runtime faults in engine subsystems and auto-disables repeat offenders
     *
     * Thread safety: all public methods are guarded by a mutex. The IsEnabled() fast
     * path acquires the lock but the critical section is a single hash lookup.
     */
    class SubsystemFaultIsolator
    {
      public:
        static SubsystemFaultIsolator& GetInstance();

        /// Called by SPARK_GUARDED_UPDATE when an exception is caught
        void ReportFault(const char* name, const char* error);

        /// Fast check — returns false if the subsystem has been disabled
        [[nodiscard]] bool IsEnabled(const char* name) const;

        /// Re-enable a faulted subsystem (resets fault count)
        void ResetSubsystem(const std::string& name);

        /// Re-enable all faulted subsystems
        void ResetAll();

        /// Set per-subsystem max retries (0 = use global default)
        void SetMaxRetries(const std::string& name, uint32_t maxRetries);

        /// Set global default max retries (default: 3)
        void SetGlobalMaxRetries(uint32_t maxRetries);

        /// Get the effective max retries for a subsystem
        [[nodiscard]] uint32_t GetEffectiveMaxRetries(const std::string& name) const;

        /// Log the fault status of all tracked subsystems
        void LogStatus() const;

        /// Read-only access to all records (caller must not hold the lock)
        [[nodiscard]] std::unordered_map<std::string, SubsystemFaultRecord> GetRecordsSnapshot() const;

        /// Register console commands (fault.status, fault.reset, etc.)
        void RegisterConsoleCommands();

      private:
        SubsystemFaultIsolator() = default;

        mutable std::mutex m_mutex;
        std::unordered_map<std::string, SubsystemFaultRecord> m_records;
        uint32_t m_globalMaxRetries = 3;
    };

} // namespace Spark

// =============================================================================
// SPARK_GUARDED_UPDATE — wraps a subsystem update call with fault isolation
//
// Usage:
//   SPARK_GUARDED_UPDATE("Weather", "Core", { weather->Update(dt); });
//
// If the block throws, the exception is caught, logged, and the subsystem's
// fault count is incremented. After enough faults the subsystem is disabled.
// =============================================================================

#define SPARK_GUARDED_UPDATE(name, category, ...)                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        auto& _faultIso = Spark::SubsystemFaultIsolator::GetInstance();                                                \
        if (!_faultIso.IsEnabled(name))                                                                                \
            break;                                                                                                     \
        try                                                                                                            \
        {                                                                                                              \
            __VA_ARGS__;                                                                                               \
        }                                                                                                              \
        catch (const std::exception& _ex)                                                                              \
        {                                                                                                              \
            _faultIso.ReportFault(name, _ex.what());                                                                   \
        }                                                                                                              \
        catch (...)                                                                                                    \
        {                                                                                                              \
            _faultIso.ReportFault(name, "unknown exception");                                                          \
        }                                                                                                              \
    } while (0)
