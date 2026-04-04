/**
 * @file DeadlockDetector.cpp
 * @brief Wait-for graph deadlock detection implementation
 */

#include "Utils/DeadlockDetector.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <format>
#include <sstream>

namespace Spark
{

    DeadlockDetector& DeadlockDetector::GetInstance()
    {
        static DeadlockDetector instance;
        return instance;
    }

    // ========================================================================
    // Lock Tracking
    // ========================================================================

    void DeadlockDetector::OnLockAttempt(const std::string& mutexName)
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);
        auto threadId = std::this_thread::get_id();
        m_threadStates[threadId].waitingFor = mutexName;
    }

    void DeadlockDetector::OnLockAcquired(const std::string& mutexName)
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);
        auto threadId = std::this_thread::get_id();
        auto& state = m_threadStates[threadId];
        state.waitingFor.clear();
        state.heldMutexes.insert(mutexName);
    }

    void DeadlockDetector::OnUnlock(const std::string& mutexName)
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);
        auto threadId = std::this_thread::get_id();
        auto it = m_threadStates.find(threadId);
        if (it != m_threadStates.end())
        {
            it->second.heldMutexes.erase(mutexName);
            // Clean up thread entry if no locks held and not waiting
            if (it->second.heldMutexes.empty() && it->second.waitingFor.empty())
            {
                m_threadStates.erase(it);
            }
        }
    }

    // ========================================================================
    // Deadlock Detection
    // ========================================================================

    std::optional<DeadlockInfo> DeadlockDetector::CheckForDeadlock() const
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);

        // Try to find a cycle starting from each thread that is waiting
        for (const auto& [threadId, state] : m_threadStates)
        {
            if (state.waitingFor.empty())
            {
                continue;
            }

            DeadlockInfo info;
            if (FindCycle(threadId, info))
            {
                return info;
            }
        }

        return std::nullopt;
    }

    bool DeadlockDetector::FindCycle(std::thread::id startThread, DeadlockInfo& outInfo) const
    {
        // Walk the wait-for chain: thread waits for mutex -> who holds that mutex?
        std::unordered_set<std::thread::id> visited;
        auto currentThread = startThread;

        while (true)
        {
            auto stateIt = m_threadStates.find(currentThread);
            if (stateIt == m_threadStates.end() || stateIt->second.waitingFor.empty())
            {
                return false; // Chain ended without cycle
            }

            if (visited.count(currentThread) > 0)
            {
                // Found a cycle — build the info
                break;
            }
            visited.insert(currentThread);

            const std::string& waitingForMutex = stateIt->second.waitingFor;

            // Find which thread holds the mutex we're waiting for
            bool foundHolder = false;
            for (const auto& [otherId, otherState] : m_threadStates)
            {
                if (otherState.heldMutexes.count(waitingForMutex) > 0)
                {
                    DeadlockInfo::ThreadEntry entry;
                    entry.threadId = currentThread;
                    entry.mutexWaiting = waitingForMutex;
                    // Record one of the mutexes this thread holds
                    if (!stateIt->second.heldMutexes.empty())
                    {
                        entry.mutexHeld = *stateIt->second.heldMutexes.begin();
                    }
                    outInfo.cycle.push_back(std::move(entry));

                    currentThread = otherId;
                    foundHolder = true;
                    break;
                }
            }

            if (!foundHolder)
            {
                return false; // Mutex not held by any tracked thread
            }
        }

        // Build description
        std::ostringstream desc;
        desc << "Deadlock detected! Cycle involves " << outInfo.cycle.size() << " thread(s):\n";
        for (const auto& entry : outInfo.cycle)
        {
            desc << "  Thread " << entry.threadId << " holds '" << entry.mutexHeld << "', waiting for '"
                 << entry.mutexWaiting << "'\n";
        }
        outInfo.description = desc.str();

        return true;
    }

    // ========================================================================
    // Diagnostics
    // ========================================================================

    std::vector<ThreadLockState> DeadlockDetector::GetAllThreadStates() const
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);

        std::vector<ThreadLockState> result;
        result.reserve(m_threadStates.size());

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

    std::vector<std::string> DeadlockDetector::GetHeldLocks(std::thread::id thread) const
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);

        auto it = m_threadStates.find(thread);
        if (it == m_threadStates.end())
        {
            return {};
        }

        return {it->second.heldMutexes.begin(), it->second.heldMutexes.end()};
    }

    void DeadlockDetector::SetLockTimeout(float seconds)
    {
        m_lockTimeoutSec = seconds;
    }

    void DeadlockDetector::LogStatus() const
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "%s", GetStatusString().c_str());
    }

    std::string DeadlockDetector::GetStatusString() const
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);

        std::ostringstream oss;
        oss << "Deadlock Detector: " << m_threadStates.size() << " tracked thread(s)\n";

        for (const auto& [threadId, state] : m_threadStates)
        {
            oss << "  Thread " << threadId << ": holds " << state.heldMutexes.size() << " mutex(es)";
            if (!state.waitingFor.empty())
            {
                oss << ", WAITING for '" << state.waitingFor << "'";
            }
            oss << "\n";

            for (const auto& m : state.heldMutexes)
            {
                oss << "    - " << m << "\n";
            }
        }

        return oss.str();
    }

    void DeadlockDetector::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "deadlock.status", [](const std::vector<std::string>&) -> std::string
            { return DeadlockDetector::GetInstance().GetStatusString(); },
            "Show deadlock detector status and tracked threads");

        console.RegisterCommand(
            "deadlock.check",
            [](const std::vector<std::string>&) -> std::string
            {
                auto result = DeadlockDetector::GetInstance().CheckForDeadlock();
                if (result)
                {
                    return result->description;
                }
                return "No deadlock detected.";
            },
            "Check for deadlock cycles in the wait-for graph");

        console.RegisterCommand(
            "deadlock.timeout",
            [](const std::vector<std::string>& args) -> std::string
            {
                auto& detector = DeadlockDetector::GetInstance();
                if (args.empty())
                {
                    return std::format("Lock timeout: {:.1f}s", detector.GetLockTimeout());
                }

                try
                {
                    float timeout = std::stof(args[0]);
                    detector.SetLockTimeout(timeout);
                    return std::format("Lock timeout set to {:.1f}s", timeout);
                }
                catch (...)
                {
                    return "Usage: deadlock.timeout [seconds]";
                }
            },
            "Get/set lock acquisition timeout");
    }

} // namespace Spark
