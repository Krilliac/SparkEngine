/**
 * @file SparkConsole.h
 * @brief Thread-safe in-game debug console with 200+ runtime commands
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements a full-featured debug console accessible at runtime via a dedicated
 * Windows console window. The console supports:
 *
 * - **Command system**: Register, execute, and alias commands organized by category
 * - **Logging**: Multi-level logging (Info, Warning, Error, Critical, Trace, Debug)
 *   with timestamps, color coding, type filtering, and text search
 * - **Tab completion**: Press Tab to auto-complete commands from the registry
 * - **Command history**: Navigate previous commands with up/down arrow keys
 * - **Alias system**: Create shorthand aliases for frequently used commands
 * - **Watch variables**: Monitor live values that update every frame
 * - **Thread safety**: All logging operations are mutex-protected
 *
 * The console registers 200+ built-in commands across categories including
 * engine control, graphics, physics, audio, input, networking, profiling,
 * scene management, and more.
 *
 * Typical usage:
 * @code
 *   auto& console = Spark::SimpleConsole::GetInstance();
 *   console.Initialize();
 *   console.RegisterCommand("mycommand", [](const auto& args) {
 *       return "Hello from custom command!";
 *   }, "Description of my command", "Custom");
 *   console.Log("Engine started successfully", "INFO");
 * @endcode
 *
 * @see AudioEngine, GraphicsEngine, PhysicsSystem (for console integration)
 */

#pragma once
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <deque>
#include <mutex>
#include <map>
#include <chrono>

namespace Spark
{

    /**
 * @brief Thread-safe singleton debug console with command registry and logging
 *
 * SimpleConsole provides a runtime-accessible debug interface that runs in a
 * separate Windows console window. It is the central hub for all runtime
 * debugging, parameter tuning, and system inspection in the Spark Engine.
 *
 * The console uses a singleton pattern (GetInstance()) and is thread-safe for
 * all logging operations. Command execution happens on the console's own update
 * thread, so registered command handlers must be careful with shared state.
 *
 * @note Initialize() must be called before any logging or command operations.
 * @warning The console window is created via AllocConsole() on Windows, which
 *          limits the application to a single console window at a time.
 */
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
            std::string message; ///< The log message text
            std::string type;    ///< Severity type: "INFO", "WARNING", "ERROR", "CRITICAL", "TRACE", "DEBUG", "SUCCESS"
            std::string timestamp; ///< ISO 8601 formatted timestamp when the message was logged
        };

        /**
     * @brief Registered command metadata including handler, description, and category
     */
        struct CommandInfo
        {
            CommandHandler handler;  ///< Function to invoke when the command is executed
            std::string description; ///< Human-readable description shown in help output
            std::string category;    ///< Command category for organized help display (e.g., "Graphics", "Physics")
        };

        /**
     * @brief Watch variable entry for real-time value monitoring
     *
     * Watch entries are polled every frame and their current values are displayed
     * in the console's watch panel, allowing developers to monitor live game state.
     */
        struct WatchEntry
        {
            std::string name;                    ///< Display name of the watched variable
            std::function<std::string()> getter; ///< Callback that returns the current value as a string
            std::string lastValue;               ///< Cached last known value for change detection
            bool active = true;                  ///< Whether this watch is currently being polled
        };

      private:
#ifdef SPARK_PLATFORM_WINDOWS
        HWND m_consoleWindow = nullptr;   ///< Handle to the Windows console window
        HANDLE m_consoleOutput = nullptr; ///< Handle to the console output stream
        HANDLE m_consoleInput = nullptr;  ///< Handle to the console input stream
#elif defined(SPARK_PLATFORM_LINUX)
        int m_consoleOutputFd = -1; ///< File descriptor for console output (STDOUT_FILENO)
        int m_consoleInputFd = -1;  ///< File descriptor for console input (STDIN_FILENO)
#endif

        std::unordered_map<std::string, CommandInfo> m_commands; ///< Registry of all available commands
        std::deque<LogEntry> m_logHistory;                       ///< Rolling history of log messages
        std::deque<std::string> m_commandHistory;                ///< Rolling history of executed commands

        bool m_initialized = false; ///< Whether Initialize() has been called successfully
        bool m_visible = true;      ///< Whether the console window is currently visible
        std::string m_currentInput; ///< Current text in the input line buffer
        int m_historyIndex = 0;     ///< Current position in command history for up/down navigation
        int m_cursorPosition = 0;   ///< Cursor position within the current input line

        mutable std::mutex m_logMutex; ///< Mutex protecting m_logHistory for thread-safe logging

        // Command alias system
        std::unordered_map<std::string, std::string> m_aliases; ///< Map of alias names to their expanded commands

        // Watch variable system
        std::vector<WatchEntry> m_watchEntries;                  ///< List of registered watch variables
        bool m_watchActive = false;                              ///< Whether the watch display is currently active
        std::chrono::steady_clock::time_point m_lastWatchUpdate; ///< Timestamp of the last watch poll cycle

        // Tab completion state
        std::vector<std::string> m_tabCompletions; ///< Current list of tab completion candidates
        int m_tabIndex = -1;                       ///< Index into m_tabCompletions for cycling
        std::string m_tabPrefix;                   ///< The prefix text used to generate completions

        // Log filtering state
        std::string m_logFilter;     ///< Active type filter (e.g., "ERROR" to show only errors)
        std::string m_logSearchTerm; ///< Active text search term for log message filtering

        /**
     * @brief Console text colors mapped to Windows console color attributes
     */
        enum class Color
        {
            White = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
            Red = FOREGROUND_RED | FOREGROUND_INTENSITY,
            Green = FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            Blue = FOREGROUND_BLUE | FOREGROUND_INTENSITY,
            Yellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            Cyan = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
            Magenta = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
            DarkGray = FOREGROUND_INTENSITY,
            BrightWhite = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
        };

      public:
        /**
     * @brief Get the singleton instance of the console
     * @return Reference to the global SimpleConsole instance
     */
        static SimpleConsole& GetInstance();

        /**
     * @brief Initialize the console window and register built-in commands
     *
     * Creates a Windows console window, sets up input/output handles,
     * and registers all default and advanced commands (~200+).
     *
     * @return true if initialization succeeded, false on failure
     */
        bool Initialize();

        /**
     * @brief Shut down the console and release all resources
     *
     * Closes the console window, frees Win32 handles, and clears all
     * registered commands, history, and watch entries.
     */
        void Shutdown();

        /**
     * @brief Process pending console input and update watch variables
     *
     * Should be called once per frame from the main game loop. Checks for
     * keyboard input, processes commands, and updates watch variable displays.
     */
        void Update();

        // ========================================================================
        // Logging Methods
        // ========================================================================

        /**
     * @brief Log a message with a custom type/severity
     * @param message The message text to log
     * @param type    Severity category string (default: "INFO")
     */
        void Log(const std::string& message, const std::string& type = "INFO");

        /** @brief Log an informational message @param message Message text */
        void LogInfo(const std::string& message);

        /** @brief Log a warning message (displayed in yellow) @param message Message text */
        void LogWarning(const std::string& message);

        /** @brief Log an error message (displayed in red) @param message Message text */
        void LogError(const std::string& message);

        /** @brief Log a success message (displayed in green) @param message Message text */
        void LogSuccess(const std::string& message);

        /** @brief Log a critical error message (displayed in magenta) @param message Message text */
        void LogCritical(const std::string& message);

        /** @brief Log a trace-level message (displayed in dark gray) @param message Message text */
        void LogTrace(const std::string& message);

        /** @brief Log a debug-level message (displayed in cyan) @param message Message text */
        void LogDebug(const std::string& message);

        // ========================================================================
        // Command System
        // ========================================================================

        /**
     * @brief Register a new command with the console
     *
     * @param name        Unique command name (case-insensitive)
     * @param handler     Function to invoke when the command is executed
     * @param description Human-readable description for help output
     * @param category    Category name for organized help display (default: "General")
     */
        void RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description = "",
                             const std::string& category = "General");

        /**
     * @brief Parse and execute a command string
     *
     * The command string is split into tokens, aliases are resolved, and the
     * appropriate handler is invoked. The result is displayed in the console.
     *
     * @param commandLine Full command string including arguments
     * @return true if the command was found and executed, false otherwise
     */
        bool ExecuteCommand(const std::string& commandLine);

        // ========================================================================
        // Visibility Control
        // ========================================================================

        /** @brief Show the console window */
        void Show();
        /** @brief Hide the console window */
        void Hide();
        /** @brief Toggle console window visibility */
        void Toggle();
        /** @brief Check if the console window is visible @return true if visible */
        bool IsVisible() const { return m_visible; }
        /** @brief Clear all log entries from the console */
        void Clear();

        /**
     * @brief Get a copy of the log history
     * @return Vector of all LogEntry records (thread-safe copy)
     */
        std::vector<LogEntry> GetLogHistory() const;

        /**
     * @brief Get a copy of the command history
     * @return Vector of previously executed command strings
     */
        std::vector<std::string> GetCommandHistory() const;

        // ========================================================================
        // Alias Management
        // ========================================================================

        /**
     * @brief Create a command alias
     * @param alias   The shorthand name
     * @param command The full command string to expand to
     */
        void SetAlias(const std::string& alias, const std::string& command);

        /**
     * @brief Remove a command alias
     * @param alias The alias name to remove
     */
        void RemoveAlias(const std::string& alias);

        // ========================================================================
        // Watch Variable System
        // ========================================================================

        /**
     * @brief Register a watch variable for live monitoring
     * @param name   Display name for the watch
     * @param getter Callback returning the current value as a string
     */
        void AddWatch(const std::string& name, std::function<std::string()> getter);

        /**
     * @brief Remove a watch variable by name
     * @param name The display name of the watch to remove
     */
        void RemoveWatch(const std::string& name);

        /**
     * @brief Poll all active watch variables and update their display
     */
        void UpdateWatches();

      private:
        SimpleConsole() = default;
        ~SimpleConsole() = default;
        SimpleConsole(const SimpleConsole&) = delete;
        SimpleConsole& operator=(const SimpleConsole&) = delete;

        /** @brief Set the console text output color */
        void SetColor(Color color);
        /** @brief Print text without a newline */
        void Print(const std::string& text, Color color = Color::White);
        /** @brief Print text followed by a newline */
        void PrintLine(const std::string& text, Color color = Color::White);
        /** @brief Read and process keyboard input from the console */
        void ProcessInput();
        /** @brief Display the command prompt (e.g., "> ") */
        void DisplayPrompt();
        /** @brief Redraw the entire console output */
        void RedrawConsole();
        /** @brief Redraw just the current input line */
        void RedrawInputLine();
        /** @brief Tokenize a command string into individual arguments */
        std::vector<std::string> ParseCommand(const std::string& commandLine);
        /** @brief Get a formatted timestamp string for the current time */
        std::string GetTimestamp();
        /** @brief Register the core set of built-in commands */
        void RegisterDefaultCommands();
        /** @brief Register extended/advanced built-in commands */
        void RegisterAdvancedCommands();
        /** @brief Create the Win32 console window */
        bool CreateConsoleWindow();
        /** @brief Set up input/output handle references */
        void SetupConsoleHandles();

        // Tab completion
        /** @brief Handle Tab key press for command auto-completion */
        void HandleTabCompletion();
        /** @brief Get matching command names for a given prefix */
        std::vector<std::string> GetCompletions(const std::string& prefix);

        // History navigation
        /** @brief Navigate to the previous command in history (Up arrow) */
        void NavigateHistoryUp();
        /** @brief Navigate to the next command in history (Down arrow) */
        void NavigateHistoryDown();

        // Alias resolution
        /** @brief Resolve aliases in a command string before execution */
        std::string ResolveAliases(const std::string& commandLine);

        // ========================================================================
        // Advanced Command Registration (organized by subsystem)
        // ========================================================================

        void RegisterEngineCommands();      ///< Core engine control commands
        void RegisterGraphicsCommands();    ///< Graphics/rendering control commands
        void RegisterGameCommands();        ///< Gameplay and world manipulation commands
        void RegisterPlayerCommands();      ///< Player state and movement commands
        void RegisterPhysicsCommands();     ///< Physics simulation control commands
        void RegisterCameraCommands();      ///< Camera position and mode commands
        void RegisterSystemCommands();      ///< OS and system-level commands
        void RegisterDebugCommands();       ///< Debug visualization and inspection commands
        void RegisterFileCommands();        ///< File I/O and asset management commands
        void RegisterPerformanceCommands(); ///< FPS, timing, and performance commands

        // Specialized command categories
        void RegisterRenderingCommands();  ///< Advanced rendering pipeline controls
        void RegisterAudioCommands();      ///< Audio system management commands
        void RegisterNetworkingCommands(); ///< Network simulation and control commands
        void RegisterProfilingCommands();  ///< CPU/GPU profiling tool commands
        void RegisterSceneCommands();      ///< Scene loading and manipulation commands
        void RegisterInputCommands();      ///< Input system configuration commands
        void RegisterConfigCommands();     ///< Engine configuration management commands
        void RegisterTestingCommands();    ///< Automated testing and benchmarking commands
        void RegisterLogCommands();        ///< Log filtering, search, and export commands
        void RegisterWatchCommands();      ///< Watch variable management commands
        void RegisterAliasCommands();      ///< Command alias management commands
        void RegisterCrashCommands();      ///< Crash handling and diagnostic commands
        void RegisterHealthCommands();     ///< System health monitoring commands
    };

} // namespace Spark
