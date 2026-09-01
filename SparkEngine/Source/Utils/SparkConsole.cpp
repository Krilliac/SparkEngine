#include "SparkConsole.h"
#include "ScopeGuard.h"
#include "SecureMemory.h"
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

    // Host-injected instance (see SetGlobalInstance docs in the header).
    static std::atomic<SimpleConsole*> s_injectedConsole{nullptr};

    SimpleConsole& SimpleConsole::GetInstance()
    {
        if (SimpleConsole* injected = s_injectedConsole.load(std::memory_order_acquire))
            return *injected;
        static SimpleConsole instance;
        return instance;
    }

    void SimpleConsole::SetGlobalInstance(SimpleConsole* instance)
    {
        s_injectedConsole.store(instance, std::memory_order_release);
    }

    namespace Detail
    {
        void InjectConsoleInstance(SimpleConsole* instance)
        {
            SimpleConsole::SetGlobalInstance(instance);
        }
    } // namespace Detail

    // ============================================================================
    // Lifecycle
    // ============================================================================

    bool SimpleConsole::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (m_initialized.load(std::memory_order_acquire))
        {
            return true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "SimpleConsole initializing");

        // Register built-in help command
        // Publish initialized while the lifecycle lease is exclusive. Command
        // operations also take this lease, so no other thread can observe a
        // partially registered console.
        m_initialized.store(true, std::memory_order_release);

        RegisterCommand(
            "help",
            [this](const std::vector<std::string>& args) -> std::string
            {
                // DispatchCommand releases m_commandMutex before invoking handlers, so lock here
                // to keep m_commands iteration safe against concurrent Register/UnregisterCommand.
                std::lock_guard<std::mutex> lock(m_commandMutex);
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

        SPARK_LOG_INFO(Spark::LogCategory::Core, "SimpleConsole initialized");
        return true;
    }

    bool SimpleConsole::IsInitialized() const
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        return m_initialized.load(std::memory_order_acquire);
    }

    void SimpleConsole::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (!m_initialized.load(std::memory_order_acquire))
        {
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "SimpleConsole shutting down");
        m_initialized.store(false, std::memory_order_release);
        std::scoped_lock lock(m_commandMutex, m_logMutex, m_historyMutex);
        m_commands.clear();
        m_logHistory.clear();
        for (auto& command : m_commandHistory)
            SecureClear(command);
        m_commandHistory.clear();
        m_aliases.clear();
        m_registeredCommands.store(0, std::memory_order_relaxed);
        m_registeredAliases.store(0, std::memory_order_relaxed);
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

        m_totalLogsWritten.fetch_add(1, std::memory_order_relaxed);
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
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (!m_initialized.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(m_commandMutex);
        CommandInfo info;
        info.handler = std::move(handler);
        info.description = description;
        info.category = category;
        info.usage = usage;
        info.nameHash = FNV1a64(name);
        m_commands[name] = std::move(info);
        m_registeredCommands.store(static_cast<uint32_t>(m_commands.size()), std::memory_order_relaxed);
    }

    void SimpleConsole::RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description,
                                        const std::string& category, const std::string& usage,
                                        CommandPermission permission)
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (!m_initialized.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(m_commandMutex);
        CommandInfo info;
        info.handler = std::move(handler);
        info.description = description;
        info.category = category;
        info.usage = usage;
        info.nameHash = FNV1a64(name);
        info.requiredPermission = permission;
        m_commands[name] = std::move(info);
        m_registeredCommands.store(static_cast<uint32_t>(m_commands.size()), std::memory_order_relaxed);
    }

    void SimpleConsole::RegisterSensitiveCommand(const std::string& name, CommandHandler handler,
                                                 const std::string& description, const std::string& category,
                                                 const std::string& usage)
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        std::lock_guard<std::mutex> lock(m_commandMutex);
        // Remember the classification even if lifecycle state rejects the
        // handler registration. A later unknown-command attempt must still
        // never put these arguments into history.
        m_sensitiveCommandNames.insert(name);
        if (!m_initialized.load(std::memory_order_acquire))
            return;
        CommandInfo info;
        info.handler = std::move(handler);
        info.description = description;
        info.category = category;
        info.usage = usage;
        info.nameHash = FNV1a64(name);
        m_commands[name] = std::move(info);
        m_registeredCommands.store(static_cast<uint32_t>(m_commands.size()), std::memory_order_relaxed);
    }

    bool SimpleConsole::UnregisterCommand(const std::string& name)
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (!m_initialized.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(m_commandMutex);
        bool removed = m_commands.erase(name) > 0;
        m_registeredCommands.store(static_cast<uint32_t>(m_commands.size()), std::memory_order_relaxed);
        return removed;
    }

    bool SimpleConsole::HasCommand(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        return m_commands.contains(name);
    }

    bool SimpleConsole::ExecuteCommand(const std::string& commandLine)
    {
        // Keep module-provided handler code leased until dispatch finishes.
        // Shutdown takes the same lease before clearing handlers, so module
        // unload cannot race an already-copied callback.
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (!m_initialized.load(std::memory_order_acquire))
            return false;
        std::string command;
        std::vector<std::string> args;
        const auto clearArguments = MakeScopeExit([&] { SecureClear(args); });
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
        const auto clearResolved = MakeScopeExit([&] { SecureClear(resolved); });

        auto tokens = ParseCommand(resolved);
        const auto clearTokens = MakeScopeExit([&] { SecureClear(tokens); });
        if (tokens.empty())
        {
            return false;
        }

        outCommand = tokens[0];
        outArgs.assign(tokens.begin() + 1, tokens.end());

        // Sensitive commands keep only their name in history. Never retain a
        // recoverable credential merely to support command recall.
        bool sensitiveArguments = false;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            sensitiveArguments = m_sensitiveCommandNames.contains(outCommand);
        }

        {
            std::lock_guard<std::mutex> lock(m_historyMutex);
            m_commandHistory.push_back(sensitiveArguments ? outCommand + " <arguments-redacted>" : commandLine);
            if (m_commandHistory.size() > MaxCommandHistory)
            {
                SecureClear(m_commandHistory.front());
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
                m_totalCommandsExecuted.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            else
            {
                std::string newVal = args[0];
                if (cvar->GetFlags() & CVarFlags::ReadOnly)
                {
                    LogError(command + " is read-only");
                    m_totalCommandsFailed.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                if ((cvar->GetFlags() & CVarFlags::Cheat) && !CVarRegistry::Get().AreCheatsEnabled())
                {
                    LogError(command + " requires cheats to be enabled");
                    m_totalCommandsFailed.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                if (cvar->SetFromString(newVal))
                {
                    LogSuccess(command + " = " + cvar->GetValueString());
                    if (cvar->GetFlags() & CVarFlags::RequiresRestart)
                    {
                        LogWarning("Change to " + command + " requires engine restart to take effect");
                    }
                    m_totalCommandsExecuted.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                else
                {
                    LogError("Invalid value '" + newVal + "' for " + command +
                             " (type: " + std::string(CVarTypeToString(cvar->GetType())) + ")");
                    m_totalCommandsFailed.fetch_add(1, std::memory_order_relaxed);
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
                m_totalCommandsFailed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            // Permission check — bypassing allows any user to run admin/dev commands
            SPARK_BRANCH_GUARD_BEGIN("console_permission_check")
            if (m_currentPermission.load(std::memory_order_relaxed) < it->second.requiredPermission)
            {
                LogError("Permission denied: '" + command + "' requires " +
                         CommandPermissionToString(it->second.requiredPermission) + " level");
                m_totalCommandsFailed.fetch_add(1, std::memory_order_relaxed);
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
            m_totalCommandsExecuted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        catch (const std::exception& e)
        {
            LogError("Command '" + command + "' failed: " + std::string(e.what()));
            m_totalCommandsFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // ============================================================================
    // Visibility (no-ops — external SparkConsole.exe handles its own window)
    // ============================================================================

    void SimpleConsole::Show()
    {
        m_visible.store(true, std::memory_order_relaxed);
    }
    void SimpleConsole::Hide()
    {
        m_visible.store(false, std::memory_order_relaxed);
    }
    void SimpleConsole::Toggle()
    {
        bool current = m_visible.load(std::memory_order_relaxed);
        while (!m_visible.compare_exchange_weak(current, !current, std::memory_order_relaxed))
        {
        }
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
        return ConsoleStats{
            m_totalLogsWritten.load(std::memory_order_relaxed), m_totalCommandsExecuted.load(std::memory_order_relaxed),
            m_totalCommandsFailed.load(std::memory_order_relaxed), m_registeredCommands.load(std::memory_order_relaxed),
            m_registeredAliases.load(std::memory_order_relaxed)};
    }

    // ============================================================================
    // Alias System
    // ============================================================================

    void SimpleConsole::SetAlias(const std::string& alias, const std::string& command)
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (!m_initialized.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(m_commandMutex);
        m_aliases[alias] = command;
        m_registeredAliases.store(static_cast<uint32_t>(m_aliases.size()), std::memory_order_relaxed);
    }

    void SimpleConsole::RemoveAlias(const std::string& alias)
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
        if (!m_initialized.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(m_commandMutex);
        m_aliases.erase(alias);
        m_registeredAliases.store(static_cast<uint32_t>(m_aliases.size()), std::memory_order_relaxed);
    }

    // ============================================================================
    // Permission Management
    // ============================================================================

    void SimpleConsole::SetCurrentPermissionLevel(CommandPermission level)
    {
        m_currentPermission.store(level, std::memory_order_relaxed);
    }

    CommandPermission SimpleConsole::GetCurrentPermissionLevel() const
    {
        return m_currentPermission.load(std::memory_order_relaxed);
    }

    // ============================================================================
    // Private helpers
    // ============================================================================

    std::vector<std::string> SimpleConsole::ParseCommand(const std::string& commandLine)
    {
        std::vector<std::string> args;
        std::string current;
        const auto clearCurrent = MakeScopeExit([&] { SecureClear(current); });
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
                    SecureClear(current);
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
        std::tm local{};
#ifdef _WIN32
        if (localtime_s(&local, &t) != 0)
            return "00:00:00";
#else
        if (!localtime_r(&t, &local))
            return "00:00:00";
#endif
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &local);
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
        std::lock_guard<std::mutex> lock(m_commandMutex);
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
