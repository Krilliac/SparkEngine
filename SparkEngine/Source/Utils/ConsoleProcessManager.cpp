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
    // Helper: wstring → string (ASCII-only, matches previous behavior)
    // =========================================================================

    static std::string WStrToStr(const std::wstring& w)
    {
        std::string result;
        result.reserve(w.size());
        for (wchar_t c : w)
        {
            if (c < 0x80)
                result.push_back(static_cast<char>(c));
            else
                result.push_back('?');
        }
        return result;
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

    bool ConsoleProcessManager::Initialize(const std::wstring& consolePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (m_initialized)
            return true;

#if defined(SPARK_BUILD_SHIPPING) && !defined(SPARK_CONSOLE_IN_SHIPPING)
        // ENABLE_CONSOLE_IN_SHIPPING=OFF: Shipping builds never spawn the external
        // console; logging stays on the Logger sinks.
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "SparkConsole disabled in this Shipping build");
        return true;
#else
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager::Initialize starting");

        std::string consoleBaseName = WStrToStr(consolePath);
        if (consoleBaseName.empty())
            consoleBaseName = "SparkConsole";

        // Determine executable directory. An empty result means "unknown", never
        // the working directory — see ResolveConsoleExecutable.
        std::string executableDir;
#ifdef SPARK_PLATFORM_WINDOWS
        wchar_t currentDir[MAX_PATH];
        const DWORD moduleNameLength = GetModuleFileNameW(NULL, currentDir, MAX_PATH);
        if (moduleNameLength > 0 && moduleNameLength < MAX_PATH)
            executableDir = std::filesystem::path(currentDir).parent_path().string();
#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
        char exePath[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len != -1)
        {
            exePath[len] = '\0';
            executableDir = std::filesystem::path(exePath).parent_path().string();
        }
#endif

#ifdef SPARK_PLATFORM_WINDOWS
        std::string ext = ".exe";
#else
        std::string ext;
#endif

        const std::string actualPath = ResolveConsoleExecutable(executableDir, consoleBaseName + ext);

        if (actualPath.empty())
        {
            m_initialized = true;
            std::cerr << "[ConsoleProcessManager] SparkConsole not found. Using fallback logging.\n";
            return true;
        }

        bool success = LaunchConsoleProcess(actualPath);
        m_initialized = true;

        if (success)
        {
            m_shouldStopThread = false;
            m_threadStarted.store(false, std::memory_order_relaxed);
            m_consoleThread = std::thread(&ConsoleProcessManager::ConsoleThreadMain, this);

            while (!m_threadStarted.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        return success;
#endif
    }

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

    // =========================================================================
    // Logging
    // =========================================================================

    void ConsoleProcessManager::Log(const std::wstring& message, const std::wstring& type)
    {
        std::string formatted = "[" + WStrToStr(type) + "] " + WStrToStr(message);

#ifdef SPARK_PLATFORM_WINDOWS
        OutputDebugStringA(formatted.c_str());
        OutputDebugStringA("\n");
#else
        std::cerr << formatted << "\n";
#endif

        if (m_consoleRunning)
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            EnqueueForConsole(std::move(formatted));
        }
    }

    void ConsoleProcessManager::QueueEngineLog(const std::string& message, const std::string& type)
    {
        if (!m_consoleRunning)
            return;

        std::lock_guard<std::mutex> lock(m_messageMutex);
        EnqueueForConsole("[" + type + "] " + message);
    }

    void ConsoleProcessManager::EnqueueForConsole(std::string line)
    {
        // Caller holds m_messageMutex. m_process is read here, under the same
        // mutex Shutdown() takes to reset it.
        if (!m_process)
            return;

        if (m_messageQueue.size() >= kMaxQueuedMessages)
        {
            // Drop-oldest: the newest lines describe why the child stopped
            // draining. ProcessQueuedMessages() reports the count once the pipe
            // accepts data again.
            m_messageQueue.pop();
            ++m_droppedMessages;
        }
        m_messageQueue.push(std::move(line));
    }

    void ConsoleProcessManager::LogCrash(const std::string& crashInfo)
    {
        std::wstring w(crashInfo.begin(), crashInfo.end());
        Log(w, L"CRASH");
    }

    // =========================================================================
    // Process launch and I/O (delegated to Spark::Process)
    // =========================================================================

    bool ConsoleProcessManager::LaunchConsoleProcess(const std::string& path)
    {
        auto result = Process::Builder(path).Arg("--engine-pipe").CaptureStdin().CaptureStdout().Launch();

        if (!result)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to launch SparkConsole: %s", result.error().c_str());
            return false;
        }

        m_process.emplace(std::move(*result));
        m_consoleRunning = true;

        // Give the child a moment to start up
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return true;
    }

    bool ConsoleProcessManager::ReadFromConsole()
    {
        if (!m_process)
            return false;

        std::string line;
        if (!m_process->TryReadLine(line))
            return false;

        if (!line.empty())
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandQueue.push(line);
            return true;
        }
        return false;
    }

    bool ConsoleProcessManager::WriteToConsole(const std::string& message)
    {
        if (!m_process)
            return false;

        std::string data = message + "\n";
        m_process->WriteStdin(data);
        return true;
    }

    // =========================================================================
    // Background thread
    // =========================================================================

    void ConsoleProcessManager::ConsoleThreadMain()
    {
        m_threadStarted.store(true, std::memory_order_release);

        while (!m_shouldStopThread && m_consoleRunning)
        {
            if (ReadFromConsole())
                continue;
            ProcessQueuedMessages();

            // Check if child is still alive
            if (m_process && !m_process->IsRunning())
            {
                m_consoleRunning = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // =========================================================================
    // Shared methods (cross-platform)
    // =========================================================================

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
        std::queue<std::string> messagesToSend;
        uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            if (m_messageQueue.empty() && m_droppedMessages == 0)
                return;
            messagesToSend.swap(m_messageQueue);
            dropped = std::exchange(m_droppedMessages, static_cast<uint64_t>(0));
        }
        if (dropped > 0)
        {
            // One notice per burst, not one per dropped line — the same shape
            // SimpleConsole uses for its duplicate-suppression notice.
            WriteToConsole("[WARN] SparkConsole mirror dropped " + std::to_string(dropped) +
                           " log line(s): outgoing queue full (" + std::to_string(kMaxQueuedMessages) + ")");
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
