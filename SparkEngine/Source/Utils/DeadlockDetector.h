/**
 * @file DeadlockDetector.h
 * @brief Debug-mode deadlock detection via wait-for graph analysis
 *
 * Provides TrackedLockGuard (RAII) and the SPARK_TRACKED_LOCK macro to
 * replace std::lock_guard in key subsystems. Tracks which mutexes each
 * thread holds and which it is waiting on, enabling cycle detection in
 * the wait-for graph when a freeze is suspected.
 *
 * Active in Debug and Release builds. Compiled to zero-overhead wrappers
 * in SPARK_SHIPPING builds.
 *
 * Integration: FreezeDetector calls CheckForDeadlock() during its
 * escalation path to diagnose whether a freeze is caused by a deadlock.
 *
 * @see FreezeDetector.h
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark
{

    /**
     * @brief Information about a detected deadlock cycle
     */
    struct DeadlockInfo
    {
        struct ThreadEntry
        {
            std::thread::id threadId;
            std::string mutexHeld;    ///< Name of the mutex this thread holds
            std::string mutexWaiting; ///< Name of the mutex this thread is waiting for
        };

        std::vector<ThreadEntry> cycle; ///< Threads involved in the deadlock cycle
        std::string description;        ///< Human-readable summary
    };

    /**
     * @brief Snapshot of a single thread's lock state
     */
    struct ThreadLockState
    {
        std::thread::id threadId;
        std::vector<std::string> heldMutexes; ///< Mutexes currently held
        std::string waitingFor;               ///< Mutex waiting to acquire (empty if none)
    };

    /**
     * @brief Singleton deadlock detector using wait-for graph analysis
     *
     * Thread safety: all public methods are protected by an internal mutex.
     * The tracking mutex is separate from any monitored mutex to avoid
     * self-deadlock.
     */
    class DeadlockDetector
    {
      public:
        static DeadlockDetector& GetInstance();

        // ====================================================================
        // Lock Tracking
        // ====================================================================

        /// Record that the calling thread is about to lock a named mutex
        void OnLockAttempt(const std::string& mutexName);

        /// Record that the calling thread has successfully acquired the mutex
        void OnLockAcquired(const std::string& mutexName);

        /// Record that the calling thread is releasing the mutex
        void OnUnlock(const std::string& mutexName);

        // ====================================================================
        // Deadlock Detection
        // ====================================================================

        /**
         * @brief Check the wait-for graph for cycles
         * @return DeadlockInfo if a cycle is found, std::nullopt otherwise
         */
        [[nodiscard]] std::optional<DeadlockInfo> CheckForDeadlock() const;

        // ====================================================================
        // Diagnostics
        // ====================================================================

        /// Get all threads' current lock states
        [[nodiscard]] std::vector<ThreadLockState> GetAllThreadStates() const;

        /// Get mutexes currently held by a specific thread
        [[nodiscard]] std::vector<std::string> GetHeldLocks(std::thread::id thread) const;

        /// Set the timeout for lock acquisition warnings (seconds)
        void SetLockTimeout(float seconds);

        /// Get the lock acquisition timeout
        [[nodiscard]] float GetLockTimeout() const { return m_lockTimeoutSec; }

        /// Log current lock states for all threads
        void LogStatus() const;

        /// Register console commands (deadlock.status, deadlock.check)
        void RegisterConsoleCommands();

        /// Get a human-readable status string
        [[nodiscard]] std::string GetStatusString() const;

      private:
        DeadlockDetector() = default;

        struct ThreadState
        {
            std::unordered_set<std::string> heldMutexes;
            std::string waitingFor;
        };

        mutable std::mutex m_trackingMutex;
        std::unordered_map<std::thread::id, ThreadState> m_threadStates;
        float m_lockTimeoutSec = 5.0f;

        /// Detect a cycle starting from the given thread
        [[nodiscard]] bool FindCycle(std::thread::id startThread, DeadlockInfo& outInfo) const;
    };

    // ========================================================================
    // TrackedLockGuard — RAII wrapper that reports to DeadlockDetector
    // ========================================================================

    /**
     * @brief RAII lock guard that tracks mutex ownership with DeadlockDetector
     *
     * Replaces std::lock_guard in monitored subsystems. Reports lock attempts,
     * acquisitions, and releases to the singleton DeadlockDetector.
     *
     * In SPARK_SHIPPING builds, this is a plain std::lock_guard with no
     * tracking overhead.
     */
    class TrackedLockGuard
    {
      public:
        /**
         * @brief Construct and lock the mutex, reporting to DeadlockDetector
         * @param mutex The mutex to lock
         * @param name Human-readable name for diagnostics
         */
        TrackedLockGuard(std::mutex& mutex, const std::string& name) : m_mutex(mutex), m_name(name)
        {
#if !defined(SPARK_SHIPPING)
            DeadlockDetector::GetInstance().OnLockAttempt(m_name);
#endif
            m_mutex.lock();
#if !defined(SPARK_SHIPPING)
            DeadlockDetector::GetInstance().OnLockAcquired(m_name);
#endif
        }

        ~TrackedLockGuard()
        {
            m_mutex.unlock();
#if !defined(SPARK_SHIPPING)
            DeadlockDetector::GetInstance().OnUnlock(m_name);
#endif
        }

        TrackedLockGuard(const TrackedLockGuard&) = delete;
        TrackedLockGuard& operator=(const TrackedLockGuard&) = delete;

      private:
        std::mutex& m_mutex;
        std::string m_name;
    };

} // namespace Spark

// ============================================================================
// SPARK_TRACKED_LOCK — drop-in replacement for std::lock_guard with tracking
//
// Usage:
//   SPARK_TRACKED_LOCK(m_mutex, "NetworkManager::m_queueMutex");
// ============================================================================

#if !defined(SPARK_SHIPPING)
#define SPARK_TRACKED_LOCK(mutex, name) Spark::TrackedLockGuard _trackedLock_##__LINE__(mutex, name)
#else
#define SPARK_TRACKED_LOCK(mutex, name) std::lock_guard<std::mutex> _trackedLock_##__LINE__(mutex)
#endif
