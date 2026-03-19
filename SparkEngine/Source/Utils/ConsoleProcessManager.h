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
#include <Windows.h>
#endif

namespace Spark
{

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
        // Windows process/pipe handles for SparkConsole.exe subprocess
        HANDLE m_processHandle = NULL; ///< Handle to the SparkConsole child process.
        HANDLE m_threadHandle = NULL;  ///< Handle to the child process's primary thread.
        HANDLE m_stdInRead = NULL;     ///< Read end of the pipe connected to child's stdin.
        HANDLE m_stdInWrite = NULL;    ///< Write end — engine writes log messages here.
        HANDLE m_stdOutRead = NULL;    ///< Read end — engine reads commands from child's stdout.
        HANDLE m_stdOutWrite = NULL;   ///< Write end of the pipe connected to child's stdout.
#elif defined(SPARK_PLATFORM_LINUX)
        // Linux process/pipe file descriptors for SparkConsole subprocess
        pid_t m_childPid = -1;             ///< PID of the child console process (-1 = not launched).
        int m_pipeToChild[2] = {-1, -1};   ///< [0]=read, [1]=write — engine writes to child stdin.
        int m_pipeFromChild[2] = {-1, -1}; ///< [0]=read, [1]=write — engine reads from child stdout.
#endif

        std::unique_ptr<CommandRegistry> m_commandRegistry; ///< Registered console commands and their handlers.

        std::atomic<bool> m_initialized{false};    ///< Whether Initialize() completed successfully.
        std::atomic<bool> m_consoleRunning{false}; ///< Whether the subprocess is alive and communicating.

        std::thread m_consoleThread;                 ///< Background thread reading from the child process.
        std::atomic<bool> m_shouldStopThread{false}; ///< Signal for the console thread to exit its loop.

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
