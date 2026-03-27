/**
 * @file FaultIsolation.cpp
 * @brief Runtime fault isolation for engine subsystem updates
 */

#include "FaultIsolation.h"
#include "Utils/DebugHookManager.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include "Utils/StackTrace.h"

#include <sstream>

namespace Spark
{

    SubsystemFaultIsolator& SubsystemFaultIsolator::GetInstance()
    {
        static SubsystemFaultIsolator instance;
        return instance;
    }

    void SubsystemFaultIsolator::ReportFault(const char* name, const char* error)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto& record = m_records[name];
        record.faultCount++;
        record.lastError = error ? error : "unknown";
        record.lastFaultFrame = DebugHookManager::GetInstance().GetFrameNumber();

        uint32_t maxRetries = record.maxRetries > 0 ? record.maxRetries : m_globalMaxRetries;

        auto stackTrace = StackTrace::Capture(2);
        std::string traceStr = stackTrace.ToCompactString(5);

        if (record.faultCount >= maxRetries)
        {
            record.enabled = false;
            SPARK_LOG_ERROR(LogCategory::Core,
                            "FAULT ISOLATION: Subsystem '%s' DISABLED after %u faults (max %u). "
                            "Last error: %s | Stack: %s",
                            name, record.faultCount, maxRetries, record.lastError.c_str(), traceStr.c_str());

            SPARK_DEBUG_HOOK_SYSTEM(SubsystemFaulted, name, 0.0);
        }
        else
        {
            SPARK_LOG_WARN(LogCategory::Core,
                           "FAULT ISOLATION: Subsystem '%s' fault %u/%u — will retry next frame. "
                           "Error: %s | Stack: %s",
                           name, record.faultCount, maxRetries, record.lastError.c_str(), traceStr.c_str());
        }
    }

    bool SubsystemFaultIsolator::IsEnabled(const char* name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(name);
        if (it == m_records.end())
            return true; // Unknown subsystem = not faulted

        return it->second.enabled;
    }

    void SubsystemFaultIsolator::ResetSubsystem(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(name);
        if (it == m_records.end())
            return;

        bool wasFaulted = !it->second.enabled;
        it->second.faultCount = 0;
        it->second.enabled = true;
        it->second.lastError.clear();

        if (wasFaulted)
        {
            SPARK_LOG_INFO(LogCategory::Core, "FAULT ISOLATION: Subsystem '%s' re-enabled", name.c_str());
            SPARK_DEBUG_HOOK_SYSTEM(SubsystemRecovered, name.c_str(), 0.0);
        }
    }

    void SubsystemFaultIsolator::ResetAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& [name, record] : m_records)
        {
            if (!record.enabled)
            {
                SPARK_LOG_INFO(LogCategory::Core, "FAULT ISOLATION: Subsystem '%s' re-enabled", name.c_str());
            }
            record.faultCount = 0;
            record.enabled = true;
            record.lastError.clear();
        }
    }

    void SubsystemFaultIsolator::SetMaxRetries(const std::string& name, uint32_t maxRetries)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_records[name].maxRetries = maxRetries;
    }

    void SubsystemFaultIsolator::SetGlobalMaxRetries(uint32_t maxRetries)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_globalMaxRetries = maxRetries;
    }

    uint32_t SubsystemFaultIsolator::GetEffectiveMaxRetries(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_records.find(name);
        if (it != m_records.end() && it->second.maxRetries > 0)
            return it->second.maxRetries;

        return m_globalMaxRetries;
    }

    void SubsystemFaultIsolator::LogStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_records.empty())
        {
            SPARK_LOG_INFO(LogCategory::Core, "FAULT ISOLATION: No subsystem faults recorded");
            return;
        }

        SPARK_LOG_INFO(LogCategory::Core, "FAULT ISOLATION: %zu subsystem(s) tracked (global max retries: %u)",
                       m_records.size(), m_globalMaxRetries);

        for (const auto& [name, record] : m_records)
        {
            uint32_t maxRetries = record.maxRetries > 0 ? record.maxRetries : m_globalMaxRetries;
            SPARK_LOG_INFO(LogCategory::Core, "  %-24s | %s | faults: %u/%u | frame: %llu | %s", name.c_str(),
                           record.enabled ? "ENABLED " : "DISABLED", record.faultCount, maxRetries,
                           static_cast<unsigned long long>(record.lastFaultFrame),
                           record.lastError.empty() ? "(no error)" : record.lastError.c_str());
        }
    }

    std::unordered_map<std::string, SubsystemFaultRecord> SubsystemFaultIsolator::GetRecordsSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_records;
    }

    void SubsystemFaultIsolator::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "fault.status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto records = SubsystemFaultIsolator::GetInstance().GetRecordsSnapshot();
                if (records.empty())
                    return "No subsystem faults recorded.";

                uint32_t globalMax = SubsystemFaultIsolator::GetInstance().GetEffectiveMaxRetries("");
                std::ostringstream ss;
                ss << "=== Fault Isolation Status (global max retries: " << globalMax << ") ===\n";
                for (const auto& [name, rec] : records)
                {
                    uint32_t maxRetries = rec.maxRetries > 0 ? rec.maxRetries : globalMax;
                    ss << "  " << name << ": " << (rec.enabled ? "ENABLED" : "DISABLED")
                       << " | faults: " << rec.faultCount << "/" << maxRetries;
                    if (!rec.lastError.empty())
                        ss << " | last: " << rec.lastError;
                    ss << "\n";
                }
                return ss.str();
            },
            "Show fault isolation status for all subsystems", "Engine");

        console.RegisterCommand(
            "fault.reset",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: fault.reset <subsystem_name>";

                SubsystemFaultIsolator::GetInstance().ResetSubsystem(args[0]);
                return "Reset fault state for: " + args[0];
            },
            "Re-enable a faulted subsystem", "Engine", "fault.reset <name>");

        console.RegisterCommand(
            "fault.reset_all",
            [](const std::vector<std::string>&) -> std::string
            {
                SubsystemFaultIsolator::GetInstance().ResetAll();
                return "All subsystems re-enabled.";
            },
            "Re-enable all faulted subsystems", "Engine");

        console.RegisterCommand(
            "fault.retries",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: fault.retries <subsystem_name> <count>";

                uint32_t count = 0;
                try
                {
                    count = static_cast<uint32_t>(std::stoul(args[1]));
                }
                catch (...)
                {
                    return "Invalid count: " + args[1];
                }

                if (count == 0)
                    return "Count must be > 0";

                SubsystemFaultIsolator::GetInstance().SetMaxRetries(args[0], count);
                return "Set max retries for " + args[0] + " to " + std::to_string(count);
            },
            "Set max fault retries for a subsystem", "Engine", "fault.retries <name> <count>");
    }

} // namespace Spark
