/**
 * @file SparkConsole.h
 * @brief Thread-safe in-game debug console with command registry, CVar support, and logging
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a full-featured debug console accessible at runtime via a dedicated
 * console window (Win32 on Windows, terminal I/O on Linux). The console supports:
 *
 * - **Command system**: Register, execute, and alias commands organized by category
 * - **CVar integration**: Typed console variables with automatic get/set commands
 * - **Logging**: Multi-level logging with timestamps, color coding, and filtering
 * - **Tab completion**: Press Tab to auto-complete commands and cvars
 * - **Command history**: Navigate with up/down arrows; persistent across sessions
 * - **Alias system**: Create shorthand aliases for frequently used commands
 * - **Watch variables**: Monitor live values that update every frame
 * - **Script execution**: Run command scripts from files
 * - **Thread safety**: All logging and history operations are mutex-protected
 * - **Cross-platform**: Win32 console on Windows, ANSI terminal on Linux/macOS
 *
 * Typical usage:
 * @code
 *   auto& console = Spark::SimpleConsole::GetInstance();
 *   console.Initialize();
 *   console.RegisterCommand("mycommand", [](const auto& args) {
 *       return "Hello from custom command!";
 *   }, "Description of my command", "Custom", "mycommand [arg]");
 *   console.Log("Engine started successfully", "INFO");
 * @endcode
 *
 * @see ConsoleVariable.h for the CVar system
 * @see ConsoleSink.h for Logger integration
 */

#pragma once
#include "../Core/Platform.h"
#include "Hash.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <functional>
#include <deque>
#include <mutex>
#include <map>
#include <chrono>
#include <cstdint>
#include <atomic>

namespace Spark
{

    // ========================================================================
    // Log Severity (replaces raw string-based types for internal use)
    // ========================================================================

    enum class ConsoleSeverity : uint8_t
    {
        Trace,
        Debug,
        Info,
        Success,
        Warning,
        Error,
        Critical
    };

    /** @brief Convert severity enum to display string */
    const char* ConsoleSeverityToString(ConsoleSeverity severity);

    /** @brief Convert display string to severity enum (returns Info on unknown) */
    ConsoleSeverity StringToConsoleSeverity(const std::string& str);

    // ========================================================================
    // SimpleConsole
    // ========================================================================

    /**
     * @brief Thread-safe singleton debug console with command registry, CVar support, and logging
     *
     * SimpleConsole provides a runtime-accessible debug interface that runs in a
     * separate console window. It is the central hub for all runtime debugging,
     * parameter tuning, and system inspection in the Spark Engine.
     *
     * Key improvements over the original design:
     * - Type-safe severity enum replaces string comparisons in hot paths
     * - Quoted-string-aware command parser ("arg with spaces" supported)
     * - Thread-safe command history (protected by mutex)
     * - Cross-platform color output (Win32 console API / ANSI escape codes)
     * - CVar system integration for automatic get/set commands
     * - Persistent command history saved to disk
     * - Script file execution (exec_file)
     * - Command usage strings for better help output
     * - Fuzzy command matching for typo suggestions
     *
     * @note Initialize() must be called before any logging or command operations.
     */
    // Thread safety: Thread-safe. All public methods are protected by an
    // internal mutex. Safe to call Log() from any thread.
    class SimpleConsole
    {
      public:
        /**
         * @brief Function signature for command handlers
         *
         * Command handlers receive a vector of string arguments (the command name
         * is not included) and return a string result that is displayed in the console.
         */
        using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

        /**
         * @brief Single log entry with message, severity type, and timestamp
         */
        struct LogEntry
        {
            std::string message;      ///< The log message text
            std::string type;         ///< Severity string: "INFO", "WARNING", "ERROR", etc.
            std::string timestamp;    ///< Formatted timestamp when the message was logged
            ConsoleSeverity severity; ///< Enum severity for fast filtering
            uint64_t sequenceNumber;  ///< Monotonic sequence number for ordering
        };

        /**
         * @brief Registered command metadata including handler, description, usage, and category
         */
        struct CommandInfo
        {
            CommandHandler handler;  ///< Function to invoke when the command is executed
            std::string description; ///< Human-readable description shown in help output
            std::string category;    ///< Command category (e.g., "Graphics", "Physics")
            std::string usage;       ///< Usage pattern (e.g., "player_heal [amount]")
            uint64_t nameHash = 0;   ///< FNV-1a64 hash of the command name for fast lookup
        };

        /**
         * @brief Watch variable entry for real-time value monitoring
         */
        struct WatchEntry
        {
            std::string name;                    ///< Display name of the watched variable
            std::function<std::string()> getter; ///< Callback returning current value as string
            std::string lastValue;               ///< Cached last known value for change detection
            bool active = true;                  ///< Whether this watch is currently being polled
        };

        // ========================================================================
        // Console Statistics
        // ========================================================================

        struct ConsoleStats
        {
            uint64_t totalLogsWritten = 0;
            uint64_t totalCommandsExecuted = 0;
            uint64_t totalCommandsFailed = 0;
            uint32_t registeredCommands = 0;
            uint32_t registeredAliases = 0;
            uint32_t activeWatches = 0;
        };

      private:
#ifdef SPARK_PLATFORM_WINDOWS
        HWND m_consoleWindow = nullptr;   ///< Handle to the Windows console window
        HANDLE m_consoleOutput = nullptr; ///< Handle to the console output stream
        HANDLE m_consoleInput = nullptr;  ///< Handle to the console input stream
#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
        int m_consoleOutputFd = -1; ///< File descriptor for console output
        int m_consoleInputFd = -1;  ///< File descriptor for console input
#endif

        std::unordered_map<std::string, CommandInfo> m_commands; ///< Command registry
        std::deque<LogEntry> m_logHistory;                       ///< Rolling log history
        std::deque<std::string> m_commandHistory;                ///< Rolling command history

        bool m_initialized = false;
        bool m_visible = true;
        std::string m_currentInput;
        int m_historyIndex = 0;
        int m_cursorPosition = 0;

        mutable std::mutex m_logMutex;     ///< Protects m_logHistory
        mutable std::mutex m_commandMutex; ///< Protects m_commands
        mutable std::mutex m_historyMutex; ///< Protects m_commandHistory

        // Command alias system
        std::unordered_map<std::string, std::string> m_aliases;

        // Watch variable system
        std::vector<WatchEntry> m_watchEntries;
        bool m_watchActive = false;
        std::chrono::steady_clock::time_point m_lastWatchUpdate;

        // Tab completion state
        std::vector<std::string> m_tabCompletions;
        int m_tabIndex = -1;
        std::string m_tabPrefix;

        // Log filtering state
        std::string m_logFilter;
        std::string m_logSearchTerm;

        // Statistics
        std::atomic<uint64_t> m_logSequence{0};
        ConsoleStats m_stats{};

        // Persistent history
        std::string m_historyFilePath;
        static constexpr size_t MaxLogHistory = 2000;
        static constexpr size_t MaxCommandHistory = 500;

        // ========================================================================
        // Cross-platform color abstraction
        // ========================================================================

        enum class Color : uint8_t
        {
            White,
            Red,
            Green,
            Blue,
            Yellow,
            Cyan,
            Magenta,
            DarkGray,
            BrightWhite
        };

        /** @brief Map severity to display color */
        static Color SeverityToColor(ConsoleSeverity severity);

      public:
        static SimpleConsole& GetInstance();

        /**
         * @brief Initialize the console window and register built-in commands
         * @return true if initialization succeeded
         */
        bool Initialize();

        void Shutdown();

        /**
         * @brief Process pending console input and update watch variables
         * Should be called once per frame from the main game loop.
         */
        void Update();

        // ========================================================================
        // Logging Methods
        // ========================================================================

        /**
         * @brief Log a message with a string type (backward compatible)
         * @param message The message text to log
         * @param type    Severity string (default: "INFO")
         */
        void Log(const std::string& message, const std::string& type = "INFO");

        /**
         * @brief Log a message with an enum severity (preferred for new code)
         * @param severity The severity level
         * @param message  The message text
         */
        void Log(ConsoleSeverity severity, const std::string& message);

        void LogInfo(const std::string& message);
        void LogWarning(const std::string& message);
        void LogError(const std::string& message);
        void LogSuccess(const std::string& message);
        void LogCritical(const std::string& message);
        void LogTrace(const std::string& message);
        void LogDebug(const std::string& message);

        // ========================================================================
        // Command System
        // ========================================================================

        /**
         * @brief Register a command with optional usage string
         * @param name        Unique command name
         * @param handler     Function to invoke
         * @param description Human-readable description
         * @param category    Category name (default: "General")
         * @param usage       Usage pattern string (default: empty)
         */
        void RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description = "",
                             const std::string& category = "General", const std::string& usage = "");

        /**
         * @brief Unregister a previously registered command
         * @param name The command name to remove
         * @return true if the command was found and removed
         */
        bool UnregisterCommand(const std::string& name);

        /**
         * @brief Check if a command is registered
         * @param name The command name to check
         */
        bool HasCommand(const std::string& name) const;

        /**
         * @brief Parse and execute a command string
         * Supports quoted arguments: command "arg with spaces" other_arg
         * @param commandLine Full command string including arguments
         * @return true if the command was found and executed
         */
        bool ExecuteCommand(const std::string& commandLine);

        /**
         * @brief Execute commands from a script file (one command per line)
         * Lines starting with # are treated as comments
         * @param filePath Path to the script file
         * @return Number of commands executed
         */
        int ExecuteScriptFile(const std::string& filePath);

        // ========================================================================
        // Visibility Control
        // ========================================================================

        void Show();
        void Hide();
        void Toggle();
        bool IsVisible() const { return m_visible; }
        void Clear();

        std::vector<LogEntry> GetLogHistory() const;
        std::vector<std::string> GetCommandHistory() const;

        /** @brief Get console statistics */
        ConsoleStats GetStats() const;

        // ========================================================================
        // Alias Management
        // ========================================================================

        void SetAlias(const std::string& alias, const std::string& command);
        void RemoveAlias(const std::string& alias);

        // ========================================================================
        // Watch Variable System
        // ========================================================================

        void AddWatch(const std::string& name, std::function<std::string()> getter);
        void RemoveWatch(const std::string& name);
        void UpdateWatches();

        // ========================================================================
        // CVar Integration
        // ========================================================================

        /** @brief Register console commands for all currently registered CVars */
        void RegisterCVarCommands();

        // ========================================================================
        // History Persistence
        // ========================================================================

        /** @brief Save command history to disk */
        void SaveHistory() const;

        /** @brief Load command history from disk */
        void LoadHistory();

      private:
        SimpleConsole() = default;
        ~SimpleConsole() = default;
        SimpleConsole(const SimpleConsole&) = delete;
        SimpleConsole& operator=(const SimpleConsole&) = delete;

        void SetColor(Color color);
        void Print(const std::string& text, Color color = Color::White);
        void PrintLine(const std::string& text, Color color = Color::White);
        void ProcessInput();
        void DisplayPrompt();
        void RedrawConsole();
        void RedrawInputLine();

        /**
         * @brief Tokenize a command string, respecting quoted arguments
         *
         * Supports double-quoted strings: command "multi word arg" other
         * Supports backslash escaping within quotes: "say \"hello\""
         */
        std::vector<std::string> ParseCommand(const std::string& commandLine);

        std::string GetTimestamp();
        void RegisterDefaultCommands();
        void RegisterAdvancedCommands();
        bool CreateConsoleWindow();
        void SetupConsoleHandles();

        // Tab completion
        void HandleTabCompletion();
        std::vector<std::string> GetCompletions(const std::string& prefix);

        // History navigation
        void NavigateHistoryUp();
        void NavigateHistoryDown();

        // Alias resolution
        std::string ResolveAliases(const std::string& commandLine);

        /**
         * @brief Find the closest matching command name for typo suggestions
         * Uses Levenshtein distance with a maximum edit distance threshold
         * @param input The unrecognized command name
         * @return Suggested command name, or empty string if no close match
         */
        std::string FindClosestCommand(const std::string& input) const;

        /**
         * @brief Map a log type string to its Color (replaces repeated if/else chains)
         */
        static Color TypeToColor(const std::string& type);

        // ========================================================================
        // Advanced Command Registration (organized by subsystem)
        // ========================================================================

        void RegisterEngineCommands();
        void RegisterGraphicsCommands();
        void RegisterGameCommands();
        void RegisterPlayerCommands();
        void RegisterPhysicsCommands();
        void RegisterCameraCommands();
        void RegisterSystemCommands();
        void RegisterDebugCommands();
        void RegisterFileCommands();
        void RegisterPerformanceCommands();

        void RegisterRenderingCommands();
        void RegisterAudioCommands();
        void RegisterNetworkingCommands();
        void RegisterProfilingCommands();
        void RegisterSceneCommands();
        void RegisterInputCommands();
        void RegisterConfigCommands();
        void RegisterTestingCommands();
        void RegisterLogCommands();
        void RegisterWatchCommands();
        void RegisterAliasCommands();
        void RegisterCrashCommands();
        void RegisterHealthCommands();
    };

} // namespace Spark
