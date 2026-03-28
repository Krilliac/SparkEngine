/**
 * @file ConsoleProcessManager.cpp
 * @brief Cross-platform shared logic for ConsoleProcessManager
 *
 * Contains the constructor (command registration), destructor, singleton accessor,
 * and shared methods (ProcessCommands, ProcessQueuedMessages, RegisterCommand).
 *
 * Platform-specific implementations (Initialize, Shutdown, Log, pipe I/O, etc.)
 * live in separate files:
 *   - ConsoleProcessManagerWin32.cpp  (Windows)
 *   - ConsoleProcessManagerLinux.cpp  (Linux)
 *   - ConsoleProcessManagerStub.cpp   (unsupported platforms)
 */

#include "Core/Platform.h"
#include "ConsoleProcessManager.h"
#include "Utils/Assert.h"
#include "Utils/CrashHandler.h"
#include "Validate.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
#include <signal.h>
#include <unistd.h>
#endif

namespace Spark
{

    ConsoleProcessManager& ConsoleProcessManager::GetInstance()
    {
        static ConsoleProcessManager instance;
        return instance;
    }

    ConsoleProcessManager& GetConsoleProcessManagerInstance()
    {
        return ConsoleProcessManager::GetInstance();
    }

    ConsoleProcessManager::ConsoleProcessManager()
        : m_commandRegistry(std::make_unique<CommandRegistry>()), m_consoleThread(), m_shouldStopThread(false)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager constructed");

        // Register default commands
        m_commandRegistry->RegisterCommand(
            "help",
            [this](const std::vector<std::string>& args) -> std::string
            {
                std::stringstream ss;
                ss << "Available commands:\n";
                auto commands = m_commandRegistry->GetAllCommands();
                for (const auto& cmd : commands)
                {
                    ss << "  " << cmd.name;
                    if (!cmd.description.empty())
                        ss << " - " << cmd.description;
                    ss << "\n";
                    if (!cmd.usage.empty())
                        ss << "    Usage: " << cmd.usage << "\n";
                }
                return ss.str();
            },
            "Show available commands", "help");

        m_commandRegistry->RegisterCommand(
            "quit",
            [](const std::vector<std::string>& args) -> std::string
            {
#ifdef SPARK_PLATFORM_WINDOWS
                PostQuitMessage(0);
#else
                // On Linux, request graceful shutdown via signal
                kill(getpid(), SIGTERM);
#endif
                return "Shutting down engine...";
            },
            "Quit the application", "quit");

        m_commandRegistry->RegisterCommand(
            "assert_test",
            [](const std::vector<std::string>& args) -> std::string
            {
                SPARK_REQUIRE_MSG(Spark::LogCategory::Core, false, "Test assertion triggered from console command");
                return "This should not be reached";
            },
            "Trigger a test assertion", "assert_test");

#ifndef NDEBUG
        m_commandRegistry->RegisterCommand(
            "crash_test",
            [](const std::vector<std::string>& args) -> std::string
            {
                int* nullPtr = nullptr;
                *nullPtr = 42;
                return "This should not be reached";
            },
            "Trigger a test crash (debug builds only)", "crash_test");
#endif

        m_commandRegistry->RegisterCommand(
            "assert_mode",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                {
                    return "Usage: assert_mode <on|off>\nControls whether assertions trigger crash dumps";
                }
                std::string mode = args[0];
                std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                if (mode == "on" || mode == "true" || mode == "1")
                {
                    ::SetAssertCrashBehavior(true);
                    return "Assert crash dumps enabled";
                }
                else if (mode == "off" || mode == "false" || mode == "0")
                {
                    ::SetAssertCrashBehavior(false);
                    return "Assert crash dumps disabled";
                }
                return "Invalid mode. Use: on, off, true, false, 1, or 0";
            },
            "Enable/disable crash dumps for assertions", "assert_mode <on|off>");
    }

    ConsoleProcessManager::~ConsoleProcessManager()
    {
        Shutdown();
    }

    // ============================================================================
    // Shared (cross-platform) methods
    // ============================================================================

    void ConsoleProcessManager::ProcessCommands()
    {
        if (!m_consoleRunning)
            return;

        std::queue<std::string> commandsToProcess;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            if (m_commandQueue.empty())
                return;
            commandsToProcess.swap(m_commandQueue);
        }

        while (!commandsToProcess.empty())
        {
            std::string command = commandsToProcess.front();
            commandsToProcess.pop();

            try
            {
                std::string result = m_commandRegistry->ExecuteCommand(command);
                if (!result.empty())
                {
                    std::wstring wResult(result.begin(), result.end());
                    Log(wResult, L"RESULT");
                }
            }
            catch (const std::exception& e)
            {
                std::string error = "Command error: " + std::string(e.what());
                std::wstring wError(error.begin(), error.end());
                Log(wError, L"ERROR");
            }
        }
    }

    void ConsoleProcessManager::ProcessQueuedMessages()
    {
        std::queue<std::wstring> messagesToSend;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            if (m_messageQueue.empty())
                return;
            messagesToSend.swap(m_messageQueue);
        }
        while (!messagesToSend.empty())
        {
            WriteToConsole(messagesToSend.front());
            messagesToSend.pop();
        }
    }

    void ConsoleProcessManager::RegisterCommand(const std::string& name,
                                                std::function<std::string(const std::vector<std::string>&)> handler,
                                                const std::string& description, const std::string& usage)
    {
        if (m_commandRegistry)
        {
            m_commandRegistry->RegisterCommand(name, handler, description, usage);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "RegisterCommand('%s') dropped — command registry not initialized",
                           name.c_str());
        }
    }

    // ============================================================================
    // CommandRegistry implementation
    // ============================================================================

    void CommandRegistry::RegisterCommand(const std::string& name, CommandHandler handler,
                                          const std::string& description, const std::string& usage)
    {
        CommandInfo info;
        info.name = name;
        info.handler = handler;
        info.description = description;
        info.usage = usage;
        m_commands[name] = info;
    }

    std::string CommandRegistry::ExecuteCommand(const std::string& commandLine)
    {
        auto args = ParseArguments(commandLine);
        if (args.empty())
            return "Empty command";
        std::string commandName = args[0];
        args.erase(args.begin());
        auto it = m_commands.find(commandName);
        if (it == m_commands.end())
            return "Unknown command: " + commandName;
        try
        {
            return it->second.handler(args);
        }
        catch (const std::exception& e)
        {
            return "Command execution error: " + std::string(e.what());
        }
    }

    std::vector<CommandRegistry::CommandInfo> CommandRegistry::GetAllCommands() const
    {
        std::vector<CommandInfo> result;
        for (const auto& pair : m_commands)
            result.push_back(pair.second);
        return result;
    }

    std::vector<std::string> CommandRegistry::ParseArguments(const std::string& commandLine)
    {
        std::vector<std::string> args;
        std::istringstream iss(commandLine);
        std::string arg;
        while (iss >> arg)
            args.push_back(arg);
        return args;
    }

} // namespace Spark
