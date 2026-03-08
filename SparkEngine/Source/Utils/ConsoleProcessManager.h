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
        // Windows process/pipe handles
        HANDLE m_processHandle = NULL;
        HANDLE m_threadHandle = NULL;
        HANDLE m_stdInRead = NULL;
        HANDLE m_stdInWrite = NULL;
        HANDLE m_stdOutRead = NULL;
        HANDLE m_stdOutWrite = NULL;
#elif defined(SPARK_PLATFORM_LINUX)
        // Linux process/pipe file descriptors
        pid_t m_childPid = -1;
        int m_pipeToChild[2] = {-1, -1};   // [0]=read, [1]=write — we write to child stdin
        int m_pipeFromChild[2] = {-1, -1}; // [0]=read, [1]=write — we read from child stdout
#endif

        std::unique_ptr<CommandRegistry> m_commandRegistry;

        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_consoleRunning{false};

        std::thread m_consoleThread;
        std::atomic<bool> m_shouldStopThread{false};

        std::mutex m_messageMutex;
        std::queue<std::wstring> m_messageQueue;

        std::mutex m_commandMutex;
        std::queue<std::string> m_commandQueue;
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
            std::string name;
            std::string description;
            std::string usage;
            CommandHandler handler;
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
