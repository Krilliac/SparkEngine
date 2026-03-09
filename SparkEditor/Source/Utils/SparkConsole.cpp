#include "SparkConsole.h"
#include "../Integration/ExternalConsoleIntegration.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Spark
{

    // Global reference to external console (will be set by the SimpleConsolePanel)
    static SparkEditor::ExternalConsoleIntegration* g_externalConsole = nullptr;

    SimpleConsole& SimpleConsole::GetInstance()
    {
        static SimpleConsole instance;
        return instance;
    }

    bool SimpleConsole::Initialize()
    {
        if (m_initialized)
        {
            return true;
        }

        RegisterDefaultCommands();

        m_initialized = true;

        LogInfo("SparkEditor console system initialized");
        LogInfo("Engine-style logging active - all operations will be logged");

        return true;
    }

    void SimpleConsole::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        LogInfo("SparkEditor console system shutting down");

        m_initialized = false;
        g_externalConsole = nullptr;
    }

    void SimpleConsole::Update()
    {
        // No special update needed for editor console
        // External console communication is handled automatically
    }

    void SimpleConsole::Log(const std::string& message, const std::string& type)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);

        LogEntry entry;
        entry.message = message;
        entry.type = type;
        entry.timestamp = GetTimestamp();

        m_logHistory.push_back(entry);

        if (m_logHistory.size() > 1000)
        {
            m_logHistory.pop_front();
        }

        // Always output to debug console
        std::string debugMsg = "[" + entry.timestamp + "] [" + type + "] " + message;
#ifdef _WIN32
        OutputDebugStringA(debugMsg.c_str());
        OutputDebugStringA("\n");
#endif

        // Also output to stdout for debug builds
        std::cout << debugMsg << std::endl;

        // Send to external console if available (this is the KEY FIX!)
        SendToExternalConsole(message, type);
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

    void SimpleConsole::RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description)
    {
        m_commands[name] = {handler, description};
        LogTrace("Command registered: " + name);
    }

    bool SimpleConsole::ExecuteCommand(const std::string& commandLine)
    {
        if (commandLine.empty())
        {
            return false;
        }

        auto args = ParseCommand(commandLine);
        if (args.empty())
        {
            return false;
        }

        std::string command = args[0];
        args.erase(args.begin());

        auto it = m_commands.find(command);
        if (it == m_commands.end())
        {
            LogError("Unknown command: '" + command + "'. Type 'help' for available commands.");
            return false;
        }

        try
        {
            std::string result = it->second.handler(args);
            if (!result.empty())
            {
                LogInfo(result);
            }
            return true;
        }
        catch (const std::exception& e)
        {
            LogError("Command execution failed: " + std::string(e.what()));
            return false;
        }
    }

    std::vector<SimpleConsole::LogEntry> SimpleConsole::GetLogHistory() const
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        return std::vector<LogEntry>(m_logHistory.begin(), m_logHistory.end());
    }

    std::string SimpleConsole::GetTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
        return ss.str();
    }

    std::vector<std::string> SimpleConsole::ParseCommand(const std::string& commandLine)
    {
        std::vector<std::string> args;
        std::istringstream iss(commandLine);
        std::string arg;

        while (iss >> arg)
        {
            args.push_back(arg);
        }

        return args;
    }

    void SimpleConsole::RegisterDefaultCommands()
    {
        // Help command
        RegisterCommand(
            "help",
            [this](const std::vector<std::string>& args) -> std::string
            {
                std::stringstream ss;
                ss << "Available SparkEditor Commands:\n";
                ss << "==========================================\n";
                for (const auto& pair : m_commands)
                {
                    ss << "  " << std::left << std::setw(20) << pair.first << " - " << pair.second.description << "\n";
                }
                ss << "\nType 'help <command>' for detailed information about a specific command.";
                return ss.str();
            },
            "Display help information for commands");

        // Clear command
        RegisterCommand(
            "clear",
            [this](const std::vector<std::string>& args) -> std::string
            {
                std::lock_guard<std::mutex> lock(m_logMutex);
                m_logHistory.clear();
                return "Console history cleared";
            },
            "Clear the console history");

        // Version command
        RegisterCommand(
            "version", [](const std::vector<std::string>& args) -> std::string
            { return "SparkEditor v1.0.0 - Development Build\nConsole System v1.0 - Engine Integration"; },
            "Display editor version information");

        // Status command
        RegisterCommand(
            "status",
            [this](const std::vector<std::string>& args) -> std::string
            {
                std::stringstream ss;
                ss << "SparkEditor Console Status:\n";
                ss << "==========================================\n";
                ss << "Console System:   " << (m_initialized ? "ACTIVE" : "INACTIVE") << "\n";
                ss << "External Console: " << (g_externalConsole ? "CONNECTED" : "DISCONNECTED") << "\n";
                ss << "Log Entries:      " << m_logHistory.size() << "\n";
                ss << "Commands:         " << m_commands.size() << "\n";
                ss << "Engine Logging:   ACTIVE\n";
                return ss.str();
            },
            "Display console system status");

        // Test logging command (like the engine has)
        RegisterCommand(
            "test_logging",
            [this](const std::vector<std::string>& args) -> std::string
            {
                LogInfo("This is a test INFO message");
                LogSuccess("This is a test SUCCESS message");
                LogWarning("This is a test WARNING message");
                LogError("This is a test ERROR message");
                LogTrace("This is a test TRACE message");
                LogCritical("This is a test CRITICAL message");
                return "Test logging messages sent to external console";
            },
            "Send test messages to external console with different log levels");

        // External console commands
        RegisterCommand(
            "external_status",
            [this](const std::vector<std::string>& args) -> std::string
            {
                if (g_externalConsole)
                {
                    bool connected = g_externalConsole->IsConnected();
                    return "External console status: " + std::string(connected ? "CONNECTED" : "DISCONNECTED");
                }
                else
                {
                    return "External console status: NOT INITIALIZED";
                }
            },
            "Check external console connection status");
    }

    void SimpleConsole::SendToExternalConsole(const std::string& message, const std::string& type)
    {
        // THE KEY FIX: Use the global g_externalConsole that gets set by SimpleConsolePanel
        if (g_externalConsole)
        {
            try
            {
                g_externalConsole->LogToConsole(message, type);
            }
            catch (const std::exception& e)
            {
#ifdef _WIN32
                OutputDebugStringA(("Failed to send to external console: " + std::string(e.what()) + "\n").c_str());
#else
                std::cerr << "Failed to send to external console: " << e.what() << "\n";
#endif
            }
            catch (...)
            {
#ifdef _WIN32
                OutputDebugStringA("Failed to send to external console: Unknown exception\n");
#else
                std::cerr << "Failed to send to external console: Unknown exception\n";
#endif
            }
        }
    }

} // namespace Spark

// Global function to set external console reference (called by SimpleConsolePanel)
extern "C" void SetSparkConsoleExternalConsole(SparkEditor::ExternalConsoleIntegration* console)
{
    Spark::g_externalConsole = console;

    // Log that the connection has been established
    if (console)
    {
        auto& sparkConsole = Spark::SimpleConsole::GetInstance();
        sparkConsole.LogSuccess(
            "External console integration connected - all SparkEditor logging now goes to external console");
        sparkConsole.LogInfo("Console integration ready - engine-style logging is now active");
    }
}