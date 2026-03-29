/**
 * @file ThreadDebugger.h
 * @brief Thread lifecycle tracking, mutex contention monitoring, and deadlock detection
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks thread creation/destruction, monitors mutex contention hotspots,
 * and detects potential deadlocks via lock-order inversion heuristics.
 *
 * Features:
 * - Thread lifecycle tracking with creation call-site info
 * - Thread naming for readable diagnostics
 * - Mutex wait-time recording and contention hotspot analysis
 * - Lock-order tracking for deadlock detection heuristics
 * - Per-thread held-mutex stack for lock-order inversion warnings
 * - Thread-safe (uses its own internal mutex, excluded from tracking)
 *
 * @note Active in Debug and Release builds. Compiled to no-ops only in
 *       SPARK_SHIPPING builds. The deadlock detector is heuristic-based:
 *       it detects lock-order inversions, not guaranteed actual deadlocks.
 *
 * @see MemoryDebugger.h, CpuDebugger.h
 */

#pragma once

#include "../Core/Platform.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark
{

    /**
     * @brief Record of a tracked thread
     */
    struct ThreadRecord
    {
        uint64_t threadId = 0;
        std::string name;
        std::string creationFile;
        int creationLine = 0;
        std::string creationFunction;
        std::chrono::steady_clock::time_point createdAt;
        bool alive = true;
    };

    /**
     * @brief Aggregated contention statistics for a named mutex
     */
    struct ContentionStats
    {
        std::string mutexName;
        uint64_t totalWaits = 0;
        float totalWaitTimeMs = 0.0f;
        float peakWaitTimeMs = 0.0f;
        float avgWaitTimeMs = 0.0f;
    };

    /**
     * @brief Warning generated when a potential deadlock (lock-order inversion) is detected
     */
    struct DeadlockWarning
    {
        uint64_t threadA = 0;
        uint64_t threadB = 0;
        std::string mutexA;
        std::string mutexB;
        std::string description;
        std::chrono::steady_clock::time_point timestamp;
    };

    /**
     * @brief Singleton thread debugger for lifecycle tracking, contention analysis,
     *        and deadlock detection
     */
    class ThreadDebugger
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<ThreadDebugger>() instead")]]
        static ThreadDebugger& GetInstance()
        {
            static ThreadDebugger instance;
            return instance;
        }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // ---- Thread lifecycle ----

        /**
         * @brief Record a thread starting
         */
        void RecordThreadStart(uint64_t threadId, const char* name, const char* file = nullptr, int line = 0,
                               const char* func = nullptr)
        {
            if (!m_enabled)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            ThreadRecord rec;
            rec.threadId = threadId;
            rec.name = name ? name : "";
            rec.creationFile = file ? file : "";
            rec.creationLine = line;
            rec.creationFunction = func ? func : "";
            rec.createdAt = std::chrono::steady_clock::now();
            rec.alive = true;

            m_threads[threadId] = rec;
        }

        /**
         * @brief Record a thread ending
         */
        void RecordThreadEnd(uint64_t threadId)
        {
            if (!m_enabled)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_threads.find(threadId);
            if (it != m_threads.end())
                it->second.alive = false;

            // Clean up any held mutexes for this thread
            m_heldMutexes.erase(threadId);
        }

        /**
         * @brief Set or update a thread's display name
         */
        void SetThreadName(uint64_t threadId, const char* name)
        {
            if (!m_enabled || !name)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_threads.find(threadId);
            if (it != m_threads.end())
                it->second.name = name;
        }

        // ---- Mutex contention ----

        /**
         * @brief Record the start of a mutex wait (call before acquiring)
         */
        void RecordMutexWaitBegin(const char* mutexName, const char* file = nullptr, int line = 0)
        {
            if (!m_enabled || !mutexName)
                return;

            uint64_t tid = GetCurrentThreadId();

            std::lock_guard<std::mutex> lock(m_mutex);

            PendingWait pw;
            pw.startTime = std::chrono::steady_clock::now();
            pw.file = file ? file : "";
            pw.line = line;

            // Key: thread_id + mutex_name
            std::string key = std::to_string(tid) + ":" + mutexName;
            m_pendingWaits[key] = pw;
        }

        /**
         * @brief Record the end of a mutex wait (call after acquiring)
         */
        void RecordMutexWaitEnd(const char* mutexName)
        {
            if (!m_enabled || !mutexName)
                return;

            auto endTime = std::chrono::steady_clock::now();
            uint64_t tid = GetCurrentThreadId();

            std::lock_guard<std::mutex> lock(m_mutex);

            std::string key = std::to_string(tid) + ":" + mutexName;
            auto it = m_pendingWaits.find(key);
            if (it == m_pendingWaits.end())
                return;

            float waitMs = std::chrono::duration<float, std::milli>(endTime - it->second.startTime).count();

            auto& stats = m_contentionStats[mutexName];
            stats.mutexName = mutexName;
            stats.totalWaits++;
            stats.totalWaitTimeMs += waitMs;
            if (waitMs > stats.peakWaitTimeMs)
                stats.peakWaitTimeMs = waitMs;
            stats.avgWaitTimeMs = stats.totalWaitTimeMs / static_cast<float>(stats.totalWaits);

            m_pendingWaits.erase(it);
        }

        /**
         * @brief Record that a thread acquired a mutex (for lock-order tracking)
         */
        void RecordMutexAcquire(const char* mutexName, uint64_t threadId)
        {
            if (!m_enabled || !mutexName)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            // Track lock order: record which mutex pairs are acquired in sequence
            auto& held = m_heldMutexes[threadId];

            // Check for lock-order inversions: if this thread holds A and
            // acquires B, but we've previously seen B acquired before A
            // (on any thread), that's a lock-order inversion
            for (const auto& alreadyHeld : held)
            {
                std::string reverseOrder = std::string(mutexName) + "->" + alreadyHeld;
                if (m_lockOrderPairs.count(reverseOrder) > 0)
                {
                    DeadlockWarning warning;
                    warning.threadA = threadId;
                    warning.threadB = 0; // Any thread that established the reverse order
                    warning.mutexA = alreadyHeld;
                    warning.mutexB = mutexName;
                    warning.description = "Lock-order inversion: thread " + std::to_string(threadId) + " holds " +
                                          alreadyHeld + " and acquires " + mutexName +
                                          ", but reverse order was previously observed";
                    warning.timestamp = std::chrono::steady_clock::now();
                    m_deadlockWarnings.push_back(warning);
                }
            }

            // Record the lock-order pair for future inversion detection
            for (const auto& alreadyHeld : held)
                m_lockOrderPairs.insert(alreadyHeld + "->" + std::string(mutexName));

            held.insert(mutexName);
        }

        /**
         * @brief Record that a thread released a mutex
         */
        void RecordMutexRelease(const char* mutexName, uint64_t threadId)
        {
            if (!m_enabled || !mutexName)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_heldMutexes.find(threadId);
            if (it != m_heldMutexes.end())
                it->second.erase(mutexName);
        }

        // ---- Queries ----

        /**
         * @brief Get all tracked threads (alive and dead)
         */
        std::vector<ThreadRecord> GetActiveThreads() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<ThreadRecord> result;
            for (const auto& [id, rec] : m_threads)
            {
                if (rec.alive)
                    result.push_back(rec);
            }
            return result;
        }

        /**
         * @brief Get all tracked threads including dead ones
         */
        std::vector<ThreadRecord> GetAllThreads() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<ThreadRecord> result;
            result.reserve(m_threads.size());
            for (const auto& [id, rec] : m_threads)
                result.push_back(rec);
            return result;
        }

        /**
         * @brief Get contention statistics, sorted by total wait time
         */
        std::vector<ContentionStats> GetContentionStats(int topN = 20) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<ContentionStats> result;
            result.reserve(m_contentionStats.size());
            for (const auto& [name, stats] : m_contentionStats)
                result.push_back(stats);

            std::sort(result.begin(), result.end(), [](const ContentionStats& a, const ContentionStats& b)
                      { return a.totalWaitTimeMs > b.totalWaitTimeMs; });

            if (static_cast<int>(result.size()) > topN)
                result.resize(topN);
            return result;
        }

        /**
         * @brief Get all deadlock warnings
         */
        std::vector<DeadlockWarning> GetDeadlockWarnings() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_deadlockWarnings;
        }

        uint32_t GetActiveThreadCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint32_t count = 0;
            for (const auto& [id, rec] : m_threads)
            {
                if (rec.alive)
                    ++count;
            }
            return count;
        }

        uint64_t GetTotalContentionEvents() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint64_t total = 0;
            for (const auto& [name, stats] : m_contentionStats)
                total += stats.totalWaits;
            return total;
        }

        // ---- Reports ----

        void PrintThreadReport() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            fprintf(stderr, "\n===== THREAD DEBUGGER REPORT =====\n");
            fprintf(stderr, "%-20s %-12s %-8s %s\n", "Thread ID", "Name", "Status", "Created At");
            fprintf(stderr, "%-20s %-12s %-8s %s\n", "---------", "----", "------", "----------");

            for (const auto& [id, rec] : m_threads)
            {
                fprintf(stderr, "%-20llu %-12s %-8s %s:%d\n", static_cast<unsigned long long>(rec.threadId),
                        rec.name.c_str(), rec.alive ? "ALIVE" : "ENDED", rec.creationFile.c_str(), rec.creationLine);
            }
            fprintf(stderr, "==================================\n\n");
        }

        void PrintContentionReport() const
        {
            auto stats = GetContentionStats();

            fprintf(stderr, "\n===== MUTEX CONTENTION REPORT =====\n");
            fprintf(stderr, "%-30s %8s %12s %12s %12s\n", "Mutex", "Waits", "Total(ms)", "Peak(ms)", "Avg(ms)");
            fprintf(stderr, "%-30s %8s %12s %12s %12s\n", "-----", "-----", "---------", "--------", "-------");

            for (const auto& s : stats)
            {
                fprintf(stderr, "%-30s %8llu %12.3f %12.3f %12.3f\n", s.mutexName.c_str(),
                        static_cast<unsigned long long>(s.totalWaits), s.totalWaitTimeMs, s.peakWaitTimeMs,
                        s.avgWaitTimeMs);
            }
            fprintf(stderr, "===================================\n\n");
        }

        void PrintSummary() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            uint32_t alive = 0;
            for (const auto& [id, rec] : m_threads)
            {
                if (rec.alive)
                    ++alive;
            }

            uint64_t totalWaits = 0;
            float totalWaitTime = 0.0f;
            for (const auto& [name, stats] : m_contentionStats)
            {
                totalWaits += stats.totalWaits;
                totalWaitTime += stats.totalWaitTimeMs;
            }

            fprintf(stderr, "\n===== THREAD DEBUGGER SUMMARY =====\n");
            fprintf(stderr, "Total threads:      %zu (%u alive)\n", m_threads.size(), alive);
            fprintf(stderr, "Tracked mutexes:    %zu\n", m_contentionStats.size());
            fprintf(stderr, "Total waits:        %llu\n", static_cast<unsigned long long>(totalWaits));
            fprintf(stderr, "Total wait time:    %.3f ms\n", totalWaitTime);
            fprintf(stderr, "Deadlock warnings:  %zu\n", m_deadlockWarnings.size());
            fprintf(stderr, "====================================\n\n");
        }

        void Reset()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_threads.clear();
            m_contentionStats.clear();
            m_deadlockWarnings.clear();
            m_pendingWaits.clear();
            m_heldMutexes.clear();
            m_lockOrderPairs.clear();
        }

        /**
         * @brief Utility to get a stable uint64_t thread ID from std::this_thread
         */
        static uint64_t GetCurrentThreadId()
        {
            return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }

      private:
        ThreadDebugger() = default;
        ThreadDebugger(const ThreadDebugger&) = delete;
        ThreadDebugger& operator=(const ThreadDebugger&) = delete;

        struct PendingWait
        {
            std::chrono::steady_clock::time_point startTime;
            std::string file;
            int line = 0;
        };

        mutable std::mutex m_mutex;
        std::unordered_map<uint64_t, ThreadRecord> m_threads;
        std::unordered_map<std::string, ContentionStats> m_contentionStats;
        std::vector<DeadlockWarning> m_deadlockWarnings;
        std::unordered_map<std::string, PendingWait> m_pendingWaits;

        // Lock-order tracking: per-thread set of currently held mutex names
        std::unordered_map<uint64_t, std::unordered_set<std::string>> m_heldMutexes;
        // Set of observed lock-order pairs: "mutexA->mutexB" means A was acquired before B
        std::unordered_set<std::string> m_lockOrderPairs;

        bool m_enabled = true;
    };

} // namespace Spark

// ============================================================================
// Convenience macros — active in Debug + Release, no-op in Shipping
// ============================================================================

#if !defined(SPARK_SHIPPING)

/** @brief Track a thread starting with call-site info */
#define SPARK_TRACK_THREAD_START(id, name)                                                                             \
    Spark::ThreadDebugger::GetInstance().RecordThreadStart(id, name, __FILE__, __LINE__, __FUNCTION__)

/** @brief Track a thread ending */
#define SPARK_TRACK_THREAD_END(id) Spark::ThreadDebugger::GetInstance().RecordThreadEnd(id)

/** @brief Record start of mutex wait (call before lock) */
#define SPARK_TRACK_MUTEX_WAIT(name) Spark::ThreadDebugger::GetInstance().RecordMutexWaitBegin(name, __FILE__, __LINE__)

/** @brief Record end of mutex wait (call after lock acquired) */
#define SPARK_TRACK_MUTEX_ACQUIRED(name) Spark::ThreadDebugger::GetInstance().RecordMutexWaitEnd(name)

/** @brief Record mutex acquisition for lock-order tracking */
#define SPARK_TRACK_MUTEX_ACQUIRE(name, threadId)                                                                      \
    Spark::ThreadDebugger::GetInstance().RecordMutexAcquire(name, threadId)

/** @brief Record mutex release for lock-order tracking */
#define SPARK_TRACK_MUTEX_RELEASE(name, threadId)                                                                      \
    Spark::ThreadDebugger::GetInstance().RecordMutexRelease(name, threadId)

/** @brief Print thread debugger summary */
#define SPARK_PRINT_THREAD_REPORT() Spark::ThreadDebugger::GetInstance().PrintSummary()

#else

#define SPARK_TRACK_THREAD_START(id, name) ((void)0)
#define SPARK_TRACK_THREAD_END(id) ((void)0)
#define SPARK_TRACK_MUTEX_WAIT(name) ((void)0)
#define SPARK_TRACK_MUTEX_ACQUIRED(name) ((void)0)
#define SPARK_TRACK_MUTEX_ACQUIRE(name, threadId) ((void)0)
#define SPARK_TRACK_MUTEX_RELEASE(name, threadId) ((void)0)
#define SPARK_PRINT_THREAD_REPORT() ((void)0)

#endif
