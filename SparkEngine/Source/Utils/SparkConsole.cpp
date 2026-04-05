#include "SparkConsole.h"
#include "../Core/Platform.h"
#include "../Engine/Security/MemoryIntegrity.h"
#include "ConsoleVariable.h"
#include "Hash.h"
#include "Validate.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <numeric>

namespace Spark
{

    // ============================================================================
    // ConsoleSeverity helpers
    // ============================================================================

    const char* ConsoleSeverityToString(ConsoleSeverity severity)
    {
        switch (severity)
        {
        case ConsoleSeverity::Trace:
            return "TRACE";
        case ConsoleSeverity::Debug:
            return "DEBUG";
        case ConsoleSeverity::Info:
            return "INFO";
        case ConsoleSeverity::Success:
            return "SUCCESS";
        case ConsoleSeverity::Warning:
            return "WARNING";
        case ConsoleSeverity::Error:
            return "ERROR";
        case ConsoleSeverity::Critical:
            return "CRITICAL";
        default:
            return "INFO";
        }
    }

    ConsoleSeverity StringToConsoleSeverity(const std::string& str)
    {
        using namespace Spark::HashLiterals;
        switch (Spark::FNV1a64(str))
        {
        case "TRACE"_hash64:
            return ConsoleSeverity::Trace;
        case "DEBUG"_hash64:
            return ConsoleSeverity::Debug;
        case "INFO"_hash64:
            return ConsoleSeverity::Info;
        case "SUCCESS"_hash64:
            return ConsoleSeverity::Success;
        case "WARNING"_hash64:
            return ConsoleSeverity::Warning;
        case "ERROR"_hash64:
            return ConsoleSeverity::Error;
        case "CRITICAL"_hash64:
            return ConsoleSeverity::Critical;
        default:
            return ConsoleSeverity::Info;
        }
    }

    // ============================================================================
    // CommandPermission helpers
    // ============================================================================

    const char* CommandPermissionToString(CommandPermission permission)
    {
        switch (permission)
        {
        case CommandPermission::Player:
            return "Player";
        case CommandPermission::Moderator:
            return "Moderator";
        case CommandPermission::Admin:
            return "Admin";
        case CommandPermission::Developer:
            return "Developer";
        default:
            return "Unknown";
        }
    }

    // ============================================================================
    // Levenshtein distance for fuzzy command matching
    // ============================================================================

    static size_t LevenshteinDistance(const std::string& a, const std::string& b)
    {
        const size_t m = a.size();
        const size_t n = b.size();

        std::vector<size_t> prev(n + 1);
        std::vector<size_t> curr(n + 1);

        std::iota(prev.begin(), prev.end(), 0);

        for (size_t i = 1; i <= m; ++i)
        {
            curr[0] = i;
            for (size_t j = 1; j <= n; ++j)
            {
                size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                curr[j] = std::min({curr[j - 1] + 1, prev[j] + 1, prev[j - 1] + cost});
            }
            std::swap(prev, curr);
        }

        return prev[n];
    }

    // ============================================================================
    // Singleton
    // ============================================================================

    SimpleConsole& SimpleConsole::GetInstance()
    {
        static SimpleConsole instance;
        return instance;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    bool SimpleConsole::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (m_initialized)
        {
            return true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "SimpleConsole initializing");

        // Register built-in help command
        RegisterCommand(
            "help",
            [this](const std::vector<std::string>& args) -> std::string
            {
                if (!args.empty())
                {
                    auto it = m_commands.find(args[0]);
                    if (it != m_commands.end())
                    {
                        std::string result = args[0] + " - " + it->second.description;
                        if (!it->second.usage.empty())
                            result += "\n  Usage: " + it->second.usage;
                        return result;
                    }
                    return "Unknown command: " + args[0];
                }

                // Group by category
                std::map<std::string, std::vector<std::string>> categories;
                for (const auto& [name, info] : m_commands)
                    categories[info.category].push_back(name);

                std::string result;
                for (const auto& [cat, cmds] : categories)
                {
                    result += "\n[" + cat + "]\n";
                    for (const auto& cmd : cmds)
                    {
                        auto cmdIt = m_commands.find(cmd);
                        if (cmdIt != m_commands.end())
                            result += "  " + cmd + " - " + cmdIt->second.description + "\n";
                    }
                }
                return result;
            },
            "Show available commands", "General", "help [command]");

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "SimpleConsole initialized");
        return true;
    }

    void SimpleConsole::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (!m_initialized)
        {
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "SimpleConsole shutting down");
        m_initialized = false;
        m_commands.clear();
        m_logHistory.clear();
        m_commandHistory.clear();
        m_aliases.clear();
    }

    void SimpleConsole::Update()
    {
        // SimpleConsole is a log sink and command registry only.
        // No local UI processing — the external SparkConsole.exe process
        // handles all interactive console I/O via ConsoleProcessManager.
    }

    // ============================================================================
    // Logging
    // ============================================================================

    void SimpleConsole::Log(const std::string& message, const std::string& type)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);

        LogEntry entry;
        entry.message = message;
        entry.type = type;
        entry.timestamp = GetTimestamp();
        entry.severity = StringToConsoleSeverity(type);
        entry.sequenceNumber = m_logSequence.fetch_add(1, std::memory_order_relaxed);

        m_logHistory.push_back(entry);
        if (m_logHistory.size() > MaxLogHistory)
        {
            m_logHistory.pop_front();
        }

        m_stats.totalLogsWritten++;
    }

    void SimpleConsole::Log(ConsoleSeverity severity, const std::string& message)
    {
        Log(message, ConsoleSeverityToString(severity));
    }

    void SimpleConsole::LogInfo(const std::string& message)
    {
        Log(message, "INFO");
    }
    void SimpleConsole::LogWarning(const std::string& message)
    {
        Log(message, "WARNING");
    }
    void SimpleConsole::LogError(const std::string& message)
    {
        Log(message, "ERROR");
    }
    void SimpleConsole::LogSuccess(const std::string& message)
    {
        Log(message, "SUCCESS");
    }
    void SimpleConsole::LogCritical(const std::string& message)
    {
        Log(message, "CRITICAL");
    }
    void SimpleConsole::LogTrace(const std::string& message)
    {
        Log(message, "TRACE");
    }
    void SimpleConsole::LogDebug(const std::string& message)
    {
        Log(message, "DEBUG");
    }

    // ============================================================================
    // Command System
    // ============================================================================

    void SimpleConsole::RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description,
                                        const std::string& category, const std::string& usage)
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        CommandInfo info;
        info.handler = std::move(handler);
        info.description = description;
        info.category = category;
        info.usage = usage;
        info.nameHash = FNV1a64(name);
        m_commands[name] = std::move(info);
        m_stats.registeredCommands = static_cast<uint32_t>(m_commands.size());
    }

    void SimpleConsole::RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description,
                                        const std::string& category, const std::string& usage,
                                        CommandPermission permission)
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        CommandInfo info;
        info.handler = std::move(handler);
        info.description = description;
        info.category = category;
        info.usage = usage;
        info.nameHash = FNV1a64(name);
        info.requiredPermission = permission;
        m_commands[name] = std::move(info);
        m_stats.registeredCommands = static_cast<uint32_t>(m_commands.size());
    }

    bool SimpleConsole::UnregisterCommand(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        bool removed = m_commands.erase(name) > 0;
        m_stats.registeredCommands = static_cast<uint32_t>(m_commands.size());
        return removed;
    }

    bool SimpleConsole::HasCommand(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        return m_commands.contains(name);
    }

    bool SimpleConsole::ExecuteCommand(const std::string& commandLine)
    {
        std::string command;
        std::vector<std::string> args;
        if (!ParseCommandLine(commandLine, command, args))
        {
            return false;
        }

        return DispatchCommand(command, args);
    }

    bool SimpleConsole::ParseCommandLine(const std::string& commandLine, std::string& outCommand,
                                         std::vector<std::string>& outArgs)
    {
        if (commandLine.empty())
        {
            return false;
        }

        // Resolve aliases before parsing
        std::string resolved = ResolveAliases(commandLine);

        auto tokens = ParseCommand(resolved);
        if (tokens.empty())
        {
            return false;
        }

        outCommand = tokens[0];
        outArgs.assign(tokens.begin() + 1, tokens.end());

        // Record in command history
        {
            std::lock_guard<std::mutex> lock(m_historyMutex);
            m_commandHistory.push_back(commandLine);
            if (m_commandHistory.size() > MaxCommandHistory)
            {
                m_commandHistory.pop_front();
            }
        }

        return true;
    }

    bool SimpleConsole::DispatchCommand(const std::string& command, const std::vector<std::string>& args)
    {
        // Check CVars first: if the command matches a cvar name, handle get/set
        auto* cvar = CVarRegistry::Get().Find(command);
        if (cvar)
        {
            if (args.empty())
            {
                std::string info = command + " = " + cvar->GetValueString();
                if (cvar->IsModified())
                {
                    info += " (default: " + cvar->GetDefaultString() + ")";
                }
                info += " [" + std::string(CVarTypeToString(cvar->GetType())) + "]";
                if (!cvar->GetDescription().empty())
                {
                    info += "\n  " + cvar->GetDescription();
                }
                LogInfo(info);
                m_stats.totalCommandsExecuted++;
                return true;
            }
            else
            {
                std::string newVal = args[0];
                if (cvar->GetFlags() & CVarFlags::ReadOnly)
                {
                    LogError(command + " is read-only");
                    m_stats.totalCommandsFailed++;
                    return false;
                }
                if ((cvar->GetFlags() & CVarFlags::Cheat) && !CVarRegistry::Get().AreCheatsEnabled())
                {
                    LogError(command + " requires cheats to be enabled");
                    m_stats.totalCommandsFailed++;
                    return false;
                }
                if (cvar->SetFromString(newVal))
                {
                    LogSuccess(command + " = " + cvar->GetValueString());
                    if (cvar->GetFlags() & CVarFlags::RequiresRestart)
                    {
                        LogWarning("Change to " + command + " requires engine restart to take effect");
                    }
                    m_stats.totalCommandsExecuted++;
                    return true;
                }
                else
                {
                    LogError("Invalid value '" + newVal + "' for " + command +
                             " (type: " + std::string(CVarTypeToString(cvar->GetType())) + ")");
                    m_stats.totalCommandsFailed++;
                    return false;
                }
            }
        }

        // Look up registered command — copy handler out so we don't hold the lock during callback
        CommandInfo entry;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            auto it = m_commands.find(command);
            if (it == m_commands.end())
            {
                std::string suggestion = FindClosestCommand(command);
                std::string errorMsg = "Unknown command: '" + command + "'.";
                if (!suggestion.empty())
                {
                    errorMsg += " Did you mean '" + suggestion + "'?";
                }
                errorMsg += " Type 'help' for available commands.";
                LogError(errorMsg);
                m_stats.totalCommandsFailed++;
                return false;
            }

            // Permission check — bypassing allows any user to run admin/dev commands
            SPARK_BRANCH_GUARD_BEGIN("console_permission_check")
            if (m_currentPermission < it->second.requiredPermission)
            {
                LogError("Permission denied: '" + command + "' requires " +
                         CommandPermissionToString(it->second.requiredPermission) + " level");
                m_stats.totalCommandsFailed++;
                return false;
            }
            SPARK_BRANCH_GUARD_END("console_permission_check")
            entry = it->second;
        }

        try
        {
            std::string result = entry.handler(args);
            if (!result.empty())
            {
                LogInfo(result);
            }
            m_stats.totalCommandsExecuted++;
            return true;
        }
        catch (const std::exception& e)
        {
            LogError("Command '" + command + "' failed: " + std::string(e.what()));
            m_stats.totalCommandsFailed++;
            return false;
        }
    }

    // ============================================================================
    // Visibility (no-ops — external SparkConsole.exe handles its own window)
    // ============================================================================

    void SimpleConsole::Show()
    {
        m_visible = true;
    }
    void SimpleConsole::Hide()
    {
        m_visible = false;
    }
    void SimpleConsole::Toggle()
    {
        m_visible = !m_visible;
    }
    void SimpleConsole::Clear()
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logHistory.clear();
    }

    // ============================================================================
    // Accessors
    // ============================================================================

    std::vector<SimpleConsole::LogEntry> SimpleConsole::GetLogHistory() const
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        return {m_logHistory.begin(), m_logHistory.end()};
    }

    std::vector<std::string> SimpleConsole::GetCommandHistory() const
    {
        std::lock_guard<std::mutex> lock(m_historyMutex);
        return {m_commandHistory.begin(), m_commandHistory.end()};
    }

    SimpleConsole::ConsoleStats SimpleConsole::GetStats() const
    {
        return m_stats;
    }

    // ============================================================================
    // Alias System
    // ============================================================================

    void SimpleConsole::SetAlias(const std::string& alias, const std::string& command)
    {
        m_aliases[alias] = command;
        m_stats.registeredAliases = static_cast<uint32_t>(m_aliases.size());
    }

    void SimpleConsole::RemoveAlias(const std::string& alias)
    {
        m_aliases.erase(alias);
        m_stats.registeredAliases = static_cast<uint32_t>(m_aliases.size());
    }

    // ============================================================================
    // Permission Management
    // ============================================================================

    void SimpleConsole::SetCurrentPermissionLevel(CommandPermission level)
    {
        m_currentPermission = level;
    }

    CommandPermission SimpleConsole::GetCurrentPermissionLevel() const
    {
        return m_currentPermission;
    }

    // ============================================================================
    // Private helpers
    // ============================================================================

    std::vector<std::string> SimpleConsole::ParseCommand(const std::string& commandLine)
    {
        std::vector<std::string> args;
        std::string current;
        bool inQuotes = false;
        bool escape = false;

        for (char c : commandLine)
        {
            if (escape)
            {
                current += c;
                escape = false;
                continue;
            }
            if (c == '\\')
            {
                escape = true;
                continue;
            }
            if (c == '"')
            {
                inQuotes = !inQuotes;
                continue;
            }
            if (c == ' ' && !inQuotes)
            {
                if (!current.empty())
                {
                    args.push_back(current);
                    current.clear();
                }
                continue;
            }
            current += c;
        }
        if (!current.empty())
        {
            args.push_back(current);
        }
        return args;
    }

    std::string SimpleConsole::GetTimestamp() const
    {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
        return buf;
    }

    std::string SimpleConsole::FindClosestCommand(const std::string& input) const
    {
        std::string closest;
        size_t bestDistance = 4; // Maximum edit distance threshold

        for (const auto& [name, info] : m_commands)
        {
            size_t dist = LevenshteinDistance(input, name);
            if (dist < bestDistance)
            {
                bestDistance = dist;
                closest = name;
            }
        }

        // Also check CVars
        auto allCvars = CVarRegistry::Get().GetAll();
        for (const auto* cv : allCvars)
        {
            size_t dist = LevenshteinDistance(input, cv->GetName());
            if (dist < bestDistance)
            {
                bestDistance = dist;
                closest = cv->GetName();
            }
        }

        return closest;
    }

    std::string SimpleConsole::ResolveAliases(const std::string& cmd)
    {
        std::string resolved = cmd;
        for (int depth = 0; depth < 10; ++depth)
        {
            auto spacePos = resolved.find(' ');
            std::string firstWord = (spacePos != std::string::npos) ? resolved.substr(0, spacePos) : resolved;
            auto it = m_aliases.find(firstWord);
            if (it == m_aliases.end())
            {
                break;
            }
            std::string rest = (spacePos != std::string::npos) ? resolved.substr(spacePos) : "";
            resolved = it->second + rest;
        }
        return resolved;
    }

} // namespace Spark
