/**
 * @file TestDeadlockDetector.cpp
 * @brief Tests for the DeadlockDetector wait-for graph system
 */

#include "TestFramework.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================================
// Inline minimal DeadlockDetector reproduction for unit testing
// ============================================================================

namespace TestDeadlock
{

    struct DeadlockInfo
    {
        struct ThreadEntry
        {
            std::thread::id threadId;
            std::string mutexHeld;
            std::string mutexWaiting;
        };

        std::vector<ThreadEntry> cycle;
        std::string description;
    };

    struct ThreadLockState
    {
        std::thread::id threadId;
        std::vector<std::string> heldMutexes;
        std::string waitingFor;
    };

    class DeadlockDetector
    {
      public:
        void OnLockAttempt(const std::string& mutexName)
        {
            std::lock_guard<std::mutex> lock(m_trackingMutex);
            m_threadStates[std::this_thread::get_id()].waitingFor = mutexName;
        }

        void OnLockAcquired(const std::string& mutexName)
        {
            std::lock_guard<std::mutex> lock(m_trackingMutex);
            auto& state = m_threadStates[std::this_thread::get_id()];
            state.waitingFor.clear();
            state.heldMutexes.insert(mutexName);
        }

        void OnUnlock(const std::string& mutexName)
        {
            std::lock_guard<std::mutex> lock(m_trackingMutex);
            auto it = m_threadStates.find(std::this_thread::get_id());
            if (it != m_threadStates.end())
            {
                it->second.heldMutexes.erase(mutexName);
                if (it->second.heldMutexes.empty() && it->second.waitingFor.empty())
                    m_threadStates.erase(it);
            }
        }

        std::optional<DeadlockInfo> CheckForDeadlock() const
        {
            std::lock_guard<std::mutex> lock(m_trackingMutex);
            for (const auto& [threadId, state] : m_threadStates)
            {
                if (state.waitingFor.empty())
                    continue;
                DeadlockInfo info;
                if (FindCycle(threadId, info))
                    return info;
            }
            return std::nullopt;
        }

        std::vector<ThreadLockState> GetAllThreadStates() const
        {
            std::lock_guard<std::mutex> lock(m_trackingMutex);
            std::vector<ThreadLockState> result;
            for (const auto& [threadId, state] : m_threadStates)
            {
                ThreadLockState tls;
                tls.threadId = threadId;
                tls.heldMutexes.assign(state.heldMutexes.begin(), state.heldMutexes.end());
                tls.waitingFor = state.waitingFor;
                result.push_back(std::move(tls));
            }
            return result;
        }

        std::vector<std::string> GetHeldLocks(std::thread::id thread) const
        {
            std::lock_guard<std::mutex> lock(m_trackingMutex);
            auto it = m_threadStates.find(thread);
            if (it == m_threadStates.end())
                return {};
            return {it->second.heldMutexes.begin(), it->second.heldMutexes.end()};
        }

      private:
        struct ThreadState
        {
            std::unordered_set<std::string> heldMutexes;
            std::string waitingFor;
        };

        mutable std::mutex m_trackingMutex;
        std::unordered_map<std::thread::id, ThreadState> m_threadStates;

        bool FindCycle(std::thread::id startThread, DeadlockInfo& outInfo) const
        {
            std::unordered_set<std::thread::id> visited;
            auto currentThread = startThread;

            while (true)
            {
                auto stateIt = m_threadStates.find(currentThread);
                if (stateIt == m_threadStates.end() || stateIt->second.waitingFor.empty())
                    return false;

                if (visited.count(currentThread) > 0)
                    break;
                visited.insert(currentThread);

                const std::string& waitingForMutex = stateIt->second.waitingFor;

                bool foundHolder = false;
                for (const auto& [otherId, otherState] : m_threadStates)
                {
                    if (otherState.heldMutexes.count(waitingForMutex) > 0)
                    {
                        DeadlockInfo::ThreadEntry entry;
                        entry.threadId = currentThread;
                        entry.mutexWaiting = waitingForMutex;
                        if (!stateIt->second.heldMutexes.empty())
                            entry.mutexHeld = *stateIt->second.heldMutexes.begin();
                        outInfo.cycle.push_back(std::move(entry));
                        currentThread = otherId;
                        foundHolder = true;
                        break;
                    }
                }

                if (!foundHolder)
                    return false;
            }

            outInfo.description = "Deadlock detected";
            return true;
        }
    };

} // namespace TestDeadlock

// ============================================================================
// Tests
// ============================================================================

TEST(DeadlockDetector_NoThreadsInitially)
{
    TestDeadlock::DeadlockDetector detector;
    auto states = detector.GetAllThreadStates();
    EXPECT_TRUE(states.empty());
    EXPECT_FALSE(detector.CheckForDeadlock().has_value());
}

TEST(DeadlockDetector_TracksLockAcquisition)
{
    TestDeadlock::DeadlockDetector detector;

    detector.OnLockAttempt("MutexA");
    detector.OnLockAcquired("MutexA");

    auto held = detector.GetHeldLocks(std::this_thread::get_id());
    EXPECT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0], std::string("MutexA"));
}

TEST(DeadlockDetector_TracksUnlockAndCleansUp)
{
    TestDeadlock::DeadlockDetector detector;

    detector.OnLockAttempt("MutexA");
    detector.OnLockAcquired("MutexA");
    detector.OnUnlock("MutexA");

    auto states = detector.GetAllThreadStates();
    EXPECT_TRUE(states.empty());
}

TEST(DeadlockDetector_MultipleLocksTracked)
{
    TestDeadlock::DeadlockDetector detector;

    detector.OnLockAttempt("MutexA");
    detector.OnLockAcquired("MutexA");
    detector.OnLockAttempt("MutexB");
    detector.OnLockAcquired("MutexB");

    auto held = detector.GetHeldLocks(std::this_thread::get_id());
    EXPECT_EQ(held.size(), 2u);

    detector.OnUnlock("MutexB");
    held = detector.GetHeldLocks(std::this_thread::get_id());
    EXPECT_EQ(held.size(), 1u);

    detector.OnUnlock("MutexA");
    auto states = detector.GetAllThreadStates();
    EXPECT_TRUE(states.empty());
}

TEST(DeadlockDetector_NoDeadlockWithoutCycles)
{
    TestDeadlock::DeadlockDetector detector;

    detector.OnLockAttempt("MutexA");
    detector.OnLockAcquired("MutexA");
    detector.OnLockAttempt("MutexB");

    auto result = detector.CheckForDeadlock();
    EXPECT_FALSE(result.has_value());

    detector.OnLockAcquired("MutexB");
    detector.OnUnlock("MutexB");
    detector.OnUnlock("MutexA");
}

TEST(DeadlockDetector_DetectsDeadlockCycle)
{
    TestDeadlock::DeadlockDetector detector;

    std::mutex sync;
    bool thread1Ready = false;
    std::atomic<bool> thread2Ready{false};
    bool deadlockFound = false;

    std::thread t1(
        [&]
        {
            detector.OnLockAttempt("MutexA");
            detector.OnLockAcquired("MutexA");
            detector.OnLockAttempt("MutexB");

            {
                std::lock_guard<std::mutex> lock(sync);
                thread1Ready = true;
            }

            while (!thread2Ready.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

            detector.OnLockAcquired("MutexB");
            detector.OnUnlock("MutexB");
            detector.OnUnlock("MutexA");
        });

    std::thread t2(
        [&]
        {
            detector.OnLockAttempt("MutexB");
            detector.OnLockAcquired("MutexB");

            while (true)
            {
                std::lock_guard<std::mutex> lock(sync);
                if (thread1Ready)
                    break;
            }

            detector.OnLockAttempt("MutexA");

            auto result = detector.CheckForDeadlock();
            deadlockFound = result.has_value();

            detector.OnLockAcquired("MutexA");
            detector.OnUnlock("MutexA");
            detector.OnUnlock("MutexB");

            thread2Ready.store(true, std::memory_order_release);
        });

    t1.join();
    t2.join();

    EXPECT_TRUE(deadlockFound);
}

TEST(DeadlockDetector_GetAllThreadStatesWaitingInfo)
{
    TestDeadlock::DeadlockDetector detector;

    detector.OnLockAttempt("MutexA");
    detector.OnLockAcquired("MutexA");
    detector.OnLockAttempt("MutexB");

    auto states = detector.GetAllThreadStates();
    EXPECT_EQ(states.size(), 1u);
    EXPECT_EQ(states[0].waitingFor, std::string("MutexB"));
    EXPECT_EQ(states[0].heldMutexes.size(), 1u);

    detector.OnLockAcquired("MutexB");
    detector.OnUnlock("MutexB");
    detector.OnUnlock("MutexA");
}

TEST(DeadlockDetector_ConcurrentAccess)
{
    TestDeadlock::DeadlockDetector detector;

    auto worker = [&](const std::string& mutexName)
    {
        for (int i = 0; i < 100; ++i)
        {
            detector.OnLockAttempt(mutexName);
            detector.OnLockAcquired(mutexName);
            detector.OnUnlock(mutexName);
        }
    };

    std::thread t1(worker, "M1");
    std::thread t2(worker, "M2");
    std::thread t3(worker, "M3");

    t1.join();
    t2.join();
    t3.join();

    auto states = detector.GetAllThreadStates();
    EXPECT_TRUE(states.empty());
}
