/**
 * @file ConsoleProcessManager.cpp
 * @brief ConsoleProcessManager — manages the external SparkConsole subprocess.
 *
 * All platform-specific process/pipe code is handled by Spark::Process.
 * This file contains the full implementation: singleton, initialization,
 * shutdown, logging, command dispatch, and background I/O thread.
 */

#include "ConsoleProcessManager.h"
#include "Core/Platform.h"
#include "Utils/Assert.h"
#include "Utils/CrashHandler.h"
#include "Validate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <utility>

#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
#include <signal.h>
#include <unistd.h>
#include <climits>
#endif

namespace Spark
{

    // =========================================================================
    // Singleton
    // =========================================================================

    ConsoleProcessManager& ConsoleProcessManager::GetInstance()
    {
        static ConsoleProcessManager instance;
        return instance;
    }

    ConsoleProcessManager& GetConsoleProcessManagerInstance()
    {
        return ConsoleProcessManager::GetInstance();
    }

    namespace
    {
        /// Set at the end of the constructor, cleared at the top of the destructor.
        /// See ConsoleProcessManager::IsInstanceAlive() for why the destroyed
        /// window matters: GetInstance() keeps returning the dead object.
        std::atomic<bool> g_instanceAlive{false};
    } // namespace

    bool ConsoleProcessManager::IsInstanceAlive()
    {
        return g_instanceAlive.load(std::memory_order_acquire);
    }

    // =========================================================================
    // Construction / destruction
    // =========================================================================

    ConsoleProcessManager::ConsoleProcessManager()
        : m_commandRegistry(std::make_unique<CommandRegistry>()), m_consoleThread(), m_shouldStopThread(false)
    {
        // Deliberately silent. SimpleConsole::Log() reaches QueueEngineLog(), and
        // SimpleConsole is where the Logger's ConsoleSink writes — so this
        // singleton can be constructed on demand from inside Logger's sink
        // dispatch, which holds a non-recursive mutex. Logging here would
        // re-enter Logger::Log on the same thread and deadlock.

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
            [this](const std::vector<std::string>& args) -> std::string
            {
                if (m_shutdownRequestHandler)
                {
                    m_shutdownRequestHandler();
                    return "Shutdown requested...";
                }
#ifdef SPARK_PLATFORM_WINDOWS
                PostQuitMessage(0);
#else
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

        // Last: everything a QueueEngineLog() caller touches is now constructed.
        g_instanceAlive.store(true, std::memory_order_release);
    }

    ConsoleProcessManager::~ConsoleProcessManager()
    {
        // First: stop new mirror traffic before any member is torn down. Static
        // destruction order runs this before SimpleConsole's, and SimpleConsole
        // keeps logging until its own destructor.
        g_instanceAlive.store(false, std::memory_order_release);
        Shutdown();
    }


    // =========================================================================
    // Executable resolution
    // =========================================================================

    // Resolve the console executable from the running binary's canonical
    // directory only (or its bin child). The launcher working directory can be a
    // project or package root that the user opened, so a SparkConsole.exe dropped
    // there must never be executed — the crash reporter enforces the same rule.
    std::string ConsoleProcessManager::ResolveConsoleExecutable(const std::string& executableDirectory,
                                                                const std::string& fileName)
    {
        namespace fs = std::filesystem;
        if (executableDirectory.empty() || fileName.empty())
            return {};

        std::error_code error;
        const fs::path canonicalDirectory = fs::canonical(executableDirectory, error);
        if (error || !fs::is_directory(canonicalDirectory, error) || error)
            return {};

        const std::array<fs::path, 2> candidates = {canonicalDirectory / fileName,
                                                    canonicalDirectory / "bin" / fileName};
        for (const fs::path& candidate : candidates)
        {
            error.clear();
            if (!fs::is_regular_file(candidate, error) || error)
                continue;

            // canonical() resolves symlinks and reparse points: a planted link
            // that points outside the trusted directory fails the parent check.
            const fs::path canonicalCandidate = fs::canonical(candidate, error);
            if (error)
                continue;

            const fs::path parent = canonicalCandidate.parent_path();
            if (parent == canonicalDirectory || parent == canonicalDirectory / "bin")
                return canonicalCandidate.string();
        }

        return {};
    }

    // =========================================================================
    // Initialize / Shutdown
    // =========================================================================


    void ConsoleProcessManager::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        m_shutdownRequestHandler = {};
        if (!m_initialized)
            return;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager shutting down");
        m_shouldStopThread = true;
        if (m_consoleThread.joinable())
            m_consoleThread.join();
        m_consoleRunning = false;

        // Close stdin pipe to signal child, then let Process destructor handle cleanup
        if (m_process)
        {
            m_process->CloseStdin();
            m_process->WaitForExit(std::chrono::milliseconds(500));
            if (m_process->IsRunning())
                m_process->Kill();
        }
        {
            // QueueEngineLog()/Log() test m_process under this mutex, and both
            // run on threads this shutdown does not join.
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_process.reset();
        }
        m_initialized = false;
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

    // =========================================================================
    // CommandRegistry implementation
    // =========================================================================

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
