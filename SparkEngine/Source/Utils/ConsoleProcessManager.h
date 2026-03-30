/**
 * @file ConsoleProcessManager.h
 * @brief Launches and communicates with the external SparkConsole subprocess via pipes.
 *
 * The engine writes log messages to the child's stdin and reads commands from
 * its stdout. On Windows this uses CreateProcess + Win32 pipes; on Linux it
 * uses fork/exec + POSIX pipes. A background thread continuously reads from
 * the child process so the main thread is never blocked.
 */

#pragma once
#include "../Core/Platform.h"

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Spark
{

    // =========================================================================
    // RAII wrapper for Windows HANDLE — auto-closes on destruction
    // =========================================================================
#ifdef SPARK_PLATFORM_WINDOWS
    class WinHandle
    {
      public:
        WinHandle() = default;
        explicit WinHandle(HANDLE h) : m_handle(h) {}
        ~WinHandle()
        {
            if (m_handle)
                CloseHandle(m_handle);
        }

        WinHandle(const WinHandle&) = delete;
        WinHandle& operator=(const WinHandle&) = delete;
        WinHandle(WinHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = NULL; }
        WinHandle& operator=(WinHandle&& other) noexcept
        {
            if (this != &other)
            {
                if (m_handle)
                    CloseHandle(m_handle);
                m_handle = other.m_handle;
                other.m_handle = NULL;
            }
            return *this;
        }

        HANDLE Get() const { return m_handle; }
        HANDLE* GetAddressOf() { return &m_handle; }
        explicit operator bool() const { return m_handle != NULL; }

        void Reset(HANDLE h = NULL)
        {
            if (m_handle)
                CloseHandle(m_handle);
            m_handle = h;
        }

        HANDLE Release()
        {
            HANDLE h = m_handle;
            m_handle = NULL;
            return h;
        }

      private:
        HANDLE m_handle = NULL;
    };
#endif

    class CommandRegistry;

    /**
 * @brief Manages communication with external SparkConsole process
 *
 * Cross-platform console process manager that handles launching a subprocess,
 * redirecting log messages to it, and receiving commands from it.
 * On Linux, uses fork/exec and POSIX pipes.
 * On Windows, uses CreateProcess and Win32 pipes.
 */
    class ConsoleProcessManager
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<ConsoleProcessManager>() instead")]]
        static ConsoleProcessManager& GetInstance();

        bool Initialize(const std::wstring& consolePath = L"SparkConsole");
        void Shutdown();

        void Log(const std::wstring& message, const std::wstring& type = L"INFO");
        void LogCrash(const std::string& crashInfo);
        void ProcessCommands();

        void RegisterCommand(const std::string& name,
                             std::function<std::string(const std::vector<std::string>&)> handler,
                             const std::string& description = "", const std::string& usage = "");

        bool IsConsoleRunning() const { return m_consoleRunning; }

      private:
        ConsoleProcessManager();
        ~ConsoleProcessManager();

        ConsoleProcessManager(const ConsoleProcessManager&) = delete;
        ConsoleProcessManager& operator=(const ConsoleProcessManager&) = delete;

        bool LaunchConsoleProcess(const std::wstring& path);
        bool ReadFromConsole();
        bool WriteToConsole(const std::wstring& message);

        void ConsoleThreadMain();
        void ProcessQueuedMessages();

#ifdef SPARK_PLATFORM_WINDOWS
        // Windows process/pipe handles for SparkConsole.exe subprocess (RAII-managed)
        WinHandle m_processHandle; ///< Handle to the SparkConsole child process.
        WinHandle m_threadHandle;  ///< Handle to the child process's primary thread.
        WinHandle m_stdInRead;     ///< Read end of the pipe connected to child's stdin.
        WinHandle m_stdInWrite;    ///< Write end — engine writes log messages here.
        WinHandle m_stdOutRead;    ///< Read end — engine reads commands from child's stdout.
        WinHandle m_stdOutWrite;   ///< Write end of the pipe connected to child's stdout.
#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
        // POSIX process/pipe file descriptors for SparkConsole subprocess
        pid_t m_childPid = -1;             ///< PID of the child console process (-1 = not launched).
        int m_pipeToChild[2] = {-1, -1};   ///< [0]=read, [1]=write — engine writes to child stdin.
        int m_pipeFromChild[2] = {-1, -1}; ///< [0]=read, [1]=write — engine reads from child stdout.
#endif

        std::unique_ptr<CommandRegistry> m_commandRegistry; ///< Registered console commands and their handlers.

        std::atomic<bool> m_initialized{false};    ///< Whether Initialize() completed successfully.
        std::atomic<bool> m_consoleRunning{false}; ///< Whether the subprocess is alive and communicating.

        std::thread m_consoleThread;                 ///< Background thread reading from the child process.
        std::atomic<bool> m_shouldStopThread{false}; ///< Signal for the console thread to exit its loop.
        std::atomic<bool> m_threadStarted{false};    ///< Set by console thread once it begins its run loop.

        std::mutex m_messageMutex;               ///< Guards m_messageQueue (log output to child).
        std::queue<std::wstring> m_messageQueue; ///< Outgoing log messages queued for the child process.

        std::mutex m_commandMutex;              ///< Guards m_commandQueue (commands from child).
        std::queue<std::string> m_commandQueue; ///< Incoming commands read from the child process.
    };

    /**
 * @brief Simple command registry for console commands
 */
    class CommandRegistry
    {
      public:
        using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

        struct CommandInfo
        {
            std::string name;        ///< Command name (e.g. "stats", "quit").
            std::string description; ///< One-line help text shown in command listing.
            std::string usage;       ///< Usage pattern (e.g. "stats [category]").
            CommandHandler handler;  ///< Callback invoked when the command is executed.
        };

        void RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description = "",
                             const std::string& usage = "");

        std::string ExecuteCommand(const std::string& commandLine);
        std::vector<CommandInfo> GetAllCommands() const;

      private:
        std::unordered_map<std::string, CommandInfo> m_commands;
        std::vector<std::string> ParseArguments(const std::string& commandLine);
    };

    ConsoleProcessManager& GetConsoleProcessManagerInstance();

} // namespace Spark
