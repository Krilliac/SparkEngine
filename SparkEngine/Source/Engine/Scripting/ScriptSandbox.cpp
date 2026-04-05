/**
 * @file ScriptSandbox.cpp
 * @brief Runtime sandboxing implementation for AngelScript execution
 */

#include "ScriptSandbox.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"
#include "../Security/MemoryIntegrity.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT
#include <angelscript.h>
#endif

#include <algorithm>
#include <format>

namespace Spark
{

    ScriptSandbox::ScriptSandbox()
    {
        ApplySecurityDefaults(ScriptSecurityLevel::Standard);
    }

    // ========================================================================
    // Configuration
    // ========================================================================

    void ScriptSandbox::SetSecurityLevel(ScriptSecurityLevel level)
    {
        m_securityLevel = level;
        ApplySecurityDefaults(level);
        SPARK_LOG_INFO(Spark::LogCategory::Scripting, "Script sandbox security level set to %s",
                       level == ScriptSecurityLevel::Unrestricted ? "Unrestricted"
                       : level == ScriptSecurityLevel::Standard   ? "Standard"
                                                                  : "Strict");
    }

    void ScriptSandbox::SetInstructionLimit(uint32_t maxInstructions)
    {
        m_maxInstructions = maxInstructions;
    }

    void ScriptSandbox::SetExecutionTimeout(float seconds)
    {
        m_maxExecutionTimeSec = seconds;
    }

    void ScriptSandbox::SetMemoryLimit(size_t maxBytes)
    {
        m_maxMemoryBytes = maxBytes;
    }

    void ScriptSandbox::ApplySecurityDefaults(ScriptSecurityLevel level)
    {
        switch (level)
        {
        case ScriptSecurityLevel::Unrestricted:
            m_maxInstructions = 0;     // unlimited
            m_maxExecutionTimeSec = 0; // unlimited
            m_maxMemoryBytes = 0;      // unlimited
            break;

        case ScriptSecurityLevel::Standard:
            m_maxInstructions = 1'000'000;
            m_maxExecutionTimeSec = 0.1f;
            m_maxMemoryBytes = 16 * 1024 * 1024; // 16 MB
            break;

        case ScriptSecurityLevel::Strict:
            m_maxInstructions = 100'000;
            m_maxExecutionTimeSec = 0.05f;
            m_maxMemoryBytes = 4 * 1024 * 1024; // 4 MB
            break;
        }
    }

    // ========================================================================
    // API Access Control
    // ========================================================================

    void ScriptSandbox::AddAllowedFunction(const std::string& name)
    {
        m_allowedFunctions.insert(name);
    }

    void ScriptSandbox::AddBlockedFunction(const std::string& name)
    {
        m_blockedFunctions.insert(name);
    }

    void ScriptSandbox::RemoveAllowedFunction(const std::string& name)
    {
        m_allowedFunctions.erase(name);
    }

    void ScriptSandbox::RemoveBlockedFunction(const std::string& name)
    {
        m_blockedFunctions.erase(name);
    }

    bool ScriptSandbox::IsFunctionAllowed(const std::string& name) const
    {
        switch (m_securityLevel)
        {
        case ScriptSecurityLevel::Unrestricted:
            return true;

        case ScriptSecurityLevel::Standard:
            // Blacklist mode: everything allowed except explicitly blocked
            return m_blockedFunctions.find(name) == m_blockedFunctions.end();

        case ScriptSecurityLevel::Strict:
            // Whitelist mode: only explicitly allowed functions
            return m_allowedFunctions.find(name) != m_allowedFunctions.end();
        }

        return false;
    }

    // ========================================================================
    // Execution Monitoring
    // ========================================================================

    void ScriptSandbox::BeginExecution(const std::string& scriptName)
    {
        m_currentScript = scriptName;
        m_instructionCount = 0;
        m_wasTerminated = false;
        m_executionStart = std::chrono::steady_clock::now();
    }

    void ScriptSandbox::EndExecution()
    {
        m_currentScript.clear();
    }

    void ScriptSandbox::TrackAllocation(size_t bytes)
    {
        m_currentMemoryUsage += bytes;

        if (m_maxMemoryBytes > 0 && m_currentMemoryUsage > m_maxMemoryBytes)
        {
            RecordViolation(
                SandboxViolationType::MemoryLimit,
                std::format("Memory limit exceeded: {} bytes (limit: {})", m_currentMemoryUsage, m_maxMemoryBytes));
        }
    }

    void ScriptSandbox::TrackDeallocation(size_t bytes)
    {
        if (bytes <= m_currentMemoryUsage)
        {
            m_currentMemoryUsage -= bytes;
        }
        else
        {
            m_currentMemoryUsage = 0;
        }
    }

#ifdef SPARK_ANGELSCRIPT_SUPPORT
    void ScriptSandbox::LineCallback(asIScriptContext* ctx, void* param)
    {
        auto* sandbox = static_cast<ScriptSandbox*>(param);

        sandbox->m_instructionCount += LINE_CALLBACK_INTERVAL;

        // Memory integrity: prove sandbox enforcement checks actually run.
        // Bypassing these allows scripts to run indefinitely, exhaust memory,
        // or escape the sandbox entirely.
        SPARK_INTEGRITY_CHECKPOINT("sandbox_enforcement");

        // Check instruction limit
        SPARK_BRANCH_GUARD_BEGIN("sandbox_instruction_limit")
        if (sandbox->m_maxInstructions > 0 && sandbox->m_instructionCount >= sandbox->m_maxInstructions)
        {
            sandbox->m_wasTerminated = true;
            sandbox->RecordViolation(SandboxViolationType::InstructionLimit,
                                     std::format("Instruction limit reached: {} (limit: {})",
                                                 sandbox->m_instructionCount, sandbox->m_maxInstructions));
            ctx->Abort();
            return;
        }
        SPARK_BRANCH_GUARD_END("sandbox_instruction_limit")

        // Check execution timeout
        SPARK_BRANCH_GUARD_BEGIN("sandbox_timeout")
        if (sandbox->m_maxExecutionTimeSec > 0)
        {
            auto elapsed = std::chrono::steady_clock::now() - sandbox->m_executionStart;
            float elapsedSec = std::chrono::duration<float>(elapsed).count();

            if (elapsedSec >= sandbox->m_maxExecutionTimeSec)
            {
                sandbox->m_wasTerminated = true;
                sandbox->RecordViolation(SandboxViolationType::TimeoutLimit,
                                         std::format("Execution timeout: {:.3f}s (limit: {:.3f}s)", elapsedSec,
                                                     sandbox->m_maxExecutionTimeSec));
                ctx->Abort();
                return;
            }
        }
        SPARK_BRANCH_GUARD_END("sandbox_timeout")

        // Check memory limit
        SPARK_BRANCH_GUARD_BEGIN("sandbox_memory_limit")
        if (sandbox->m_maxMemoryBytes > 0 && sandbox->m_currentMemoryUsage > sandbox->m_maxMemoryBytes)
        {
            sandbox->m_wasTerminated = true;
            sandbox->RecordViolation(SandboxViolationType::MemoryLimit,
                                     std::format("Memory limit exceeded during execution: {} bytes (limit: {})",
                                                 sandbox->m_currentMemoryUsage, sandbox->m_maxMemoryBytes));
            ctx->Abort();
            return;
        }
        SPARK_BRANCH_GUARD_END("sandbox_memory_limit")

        SPARK_VERIFY_CHECKPOINT("sandbox_enforcement");
    }
#endif

    // ========================================================================
    // Violation Tracking
    // ========================================================================

    void ScriptSandbox::RecordViolation(SandboxViolationType type, const std::string& details)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Scripting, "Sandbox violation [%s]: %s", m_currentScript.c_str(),
                       details.c_str());

        std::lock_guard<std::mutex> lock(m_violationMutex);
        m_totalViolations++;

        if (m_violations.size() >= MAX_VIOLATION_HISTORY)
        {
            m_violations.erase(m_violations.begin());
        }

        SandboxViolation violation;
        violation.type = type;
        violation.scriptName = m_currentScript;
        violation.details = details;
        m_violations.push_back(std::move(violation));
    }

    std::vector<SandboxViolation> ScriptSandbox::GetViolations() const
    {
        std::lock_guard<std::mutex> lock(m_violationMutex);
        return m_violations;
    }

    uint32_t ScriptSandbox::GetTotalViolations() const
    {
        std::lock_guard<std::mutex> lock(m_violationMutex);
        return m_totalViolations;
    }

    void ScriptSandbox::ClearViolations()
    {
        std::lock_guard<std::mutex> lock(m_violationMutex);
        m_violations.clear();
        m_totalViolations = 0;
    }

    // ========================================================================
    // Console & Diagnostics
    // ========================================================================

    std::string ScriptSandbox::GetStatusString() const
    {
        const char* levelStr = m_securityLevel == ScriptSecurityLevel::Unrestricted ? "Unrestricted"
                               : m_securityLevel == ScriptSecurityLevel::Standard   ? "Standard"
                                                                                    : "Strict";

        std::string instrLimit = m_maxInstructions > 0 ? std::to_string(m_maxInstructions) : "unlimited";
        std::string timeLimit = m_maxExecutionTimeSec > 0 ? std::format("{:.3f}s", m_maxExecutionTimeSec) : "unlimited";
        std::string memLimit =
            m_maxMemoryBytes > 0 ? std::format("{} MB", m_maxMemoryBytes / (1024 * 1024)) : "unlimited";

        return std::format("Script Sandbox: {} | Instructions: {} | Timeout: {} | Memory: {}\n"
                           "Allowed functions: {} | Blocked functions: {} | Total violations: {}",
                           levelStr, instrLimit, timeLimit, memLimit, m_allowedFunctions.size(),
                           m_blockedFunctions.size(), m_totalViolations);
    }

    void ScriptSandbox::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "sandbox.status",
            [](const std::vector<std::string>&) -> std::string
            {
                // ScriptSandbox is owned by AngelScriptEngine — this accesses
                // a static helper that returns the status string.
                return "Use sandbox.level to view/change security level";
            },
            "Show script sandbox status");

        console.RegisterCommand(
            "sandbox.level",
            [this](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                {
                    return GetStatusString();
                }

                const auto& level = args[0];
                if (level == "unrestricted")
                {
                    SetSecurityLevel(ScriptSecurityLevel::Unrestricted);
                }
                else if (level == "standard")
                {
                    SetSecurityLevel(ScriptSecurityLevel::Standard);
                }
                else if (level == "strict")
                {
                    SetSecurityLevel(ScriptSecurityLevel::Strict);
                }
                else
                {
                    return "Usage: sandbox.level [unrestricted|standard|strict]";
                }

                return GetStatusString();
            },
            "Get/set script sandbox security level");

        console.RegisterCommand(
            "sandbox.violations",
            [this](const std::vector<std::string>&) -> std::string
            {
                auto violations = GetViolations();
                if (violations.empty())
                {
                    return "No sandbox violations recorded.";
                }

                std::string result = std::format("Total violations: {}\nRecent:\n", m_totalViolations);
                for (const auto& v : violations)
                {
                    result += std::format("  [{}] {}\n", v.scriptName, v.details);
                }
                return result;
            },
            "Show recent sandbox violations");
    }

} // namespace Spark
