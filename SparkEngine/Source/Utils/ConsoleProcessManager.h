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
#include "Process.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

        /**
         * @brief Attempt to bring up the external console subprocess.
         *
         * @return true when initialization completed without error — which
         *         covers three distinct outcomes: the child launched, no trusted
         *         console executable was found, or the console is compiled out of
         *         this Shipping build. It is NOT a statement that a console is
         *         running; ask IsConsoleRunning() for that.
         */
        bool Initialize(const std::wstring& consolePath = L"SparkConsole");
        void Shutdown();

        void Log(const std::wstring& message, const std::wstring& type = L"INFO");
        void LogCrash(const std::string& crashInfo);

        /**
         * @brief Queue an already-reported engine log line for the console window.
         *
         * Unlike Log(), this does not echo to the debugger or stderr: the caller
         * is the Logger's SimpleConsole bridge and the Logger's StderrSink has
         * already written that copy. Never log through SimpleConsole from here —
         * that would recurse.
         */
        void QueueEngineLog(const std::string& message, const std::string& type);
        void ProcessCommands();

        /**
         * @brief Whether the process-wide instance is constructed and not yet destroyed.
         *
         * Both this manager and SimpleConsole are function-local statics, so the
         * one constructed last is destroyed first. SimpleConsole::Log() mirrors
         * into QueueEngineLog(), and a SPARK_LOG_* emitted during static teardown
         * would otherwise reach a destroyed object — GetInstance() hands back the
         * storage regardless. Callers outside this class must test this first.
         */
        static bool IsInstanceAlive();

        /// Route the built-in quit command into the active platform loop.
        /// Headless loops use an atomic request; windowed loops post WM_QUIT.
        void SetShutdownRequestHandler(std::function<void()> handler) { m_shutdownRequestHandler = std::move(handler); }

        void RegisterCommand(const std::string& name,
                             std::function<std::string(const std::vector<std::string>&)> handler,
                             const std::string& description = "", const std::string& usage = "");

        bool IsConsoleRunning() const { return m_consoleRunning; }

        /**
         * @brief Resolve the console executable from a trusted directory only.
         *
         * Searches the canonical @p executableDirectory and its @c bin child and
         * nothing else — never the working directory, which for a launcher- or
         * editor-started engine is a user project root where a planted
         * SparkConsole.exe would otherwise be launched with the user's privileges.
         *
         * @return Canonical path of the executable, or an empty string when no
         *         trusted candidate exists.
         */
        static std::string ResolveConsoleExecutable(const std::string& executableDirectory,
                                                    const std::string& fileName);

      private:
        ConsoleProcessManager();
        ~ConsoleProcessManager();

        ConsoleProcessManager(const ConsoleProcessManager&) = delete;
        ConsoleProcessManager& operator=(const ConsoleProcessManager&) = delete;

        bool LaunchConsoleProcess(const std::string& path);
        bool ReadFromConsole();
        bool WriteToConsole(const std::string& message);

        void ConsoleThreadMain();
        void ProcessQueuedMessages();

        /// Append one outgoing line, dropping the oldest past kMaxQueuedMessages.
        /// @note The caller must already hold m_messageMutex.
        void EnqueueForConsole(std::string line);

        std::optional<Process> m_process; ///< The SparkConsole subprocess (piped stdin/stdout).

        std::unique_ptr<CommandRegistry> m_commandRegistry; ///< Registered console commands and their handlers.

        std::atomic<bool> m_initialized{false};    ///< Whether Initialize() completed successfully.
        std::atomic<bool> m_consoleRunning{false}; ///< Whether the subprocess is alive and communicating.

        std::thread m_consoleThread;                 ///< Background thread reading from the child process.
        std::atomic<bool> m_shouldStopThread{false}; ///< Signal for the console thread to exit its loop.
        std::atomic<bool> m_threadStarted{false};    ///< Set by console thread once it begins its run loop.

        /// Cap on outgoing log lines held for the child. The only drain is the
        /// console thread's blocking pipe write, so a child that stops reading
        /// its stdin would otherwise grow this queue by one entry per engine log
        /// line for the life of the process. Over the cap the oldest line is
        /// dropped and counted; the count is reported once when the queue drains.
        static constexpr size_t kMaxQueuedMessages = 4096;

        std::mutex m_messageMutex;              ///< Guards m_messageQueue, m_droppedMessages and m_process teardown.
        std::queue<std::string> m_messageQueue; ///< Outgoing log messages queued for the child process.
        uint64_t m_droppedMessages = 0; ///< Lines dropped since the last drop notice (guarded by m_messageMutex).

        std::mutex m_commandMutex;                      ///< Guards m_commandQueue (commands from child).
        std::queue<std::string> m_commandQueue;         ///< Incoming commands read from the child process.
        std::function<void()> m_shutdownRequestHandler; ///< Main-thread quit routing hook.
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
