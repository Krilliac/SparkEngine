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

namespace Spark {

class SimpleConsole {
public:
    using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

    struct LogEntry {
        std::string message;
        std::string type;
        std::string timestamp;
    };

    struct CommandInfo {
        CommandHandler handler;
        std::string description;
        std::string category;  // Command category for organized help
    };

    // Watch variable entry for live monitoring
    struct WatchEntry {
        std::string name;
        std::function<std::string()> getter;
        std::string lastValue;
        bool active = true;
    };

private:
    HWND m_consoleWindow = nullptr;
    HANDLE m_consoleOutput = nullptr;
    HANDLE m_consoleInput = nullptr;

    std::unordered_map<std::string, CommandInfo> m_commands;
    std::deque<LogEntry> m_logHistory;
    std::deque<std::string> m_commandHistory;

    bool m_initialized = false;
    bool m_visible = true;
    std::string m_currentInput;
    int m_historyIndex = 0;
    int m_cursorPosition = 0;  // Cursor position within input line

    mutable std::mutex m_logMutex;

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
    std::string m_logFilter;       // Active type filter (e.g., "ERROR")
    std::string m_logSearchTerm;   // Active search term

    enum class Color {
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
    static SimpleConsole& GetInstance();

    bool Initialize();
    void Shutdown();
    void Update();

    void Log(const std::string& message, const std::string& type = "INFO");
    void LogInfo(const std::string& message);
    void LogWarning(const std::string& message);
    void LogError(const std::string& message);
    void LogSuccess(const std::string& message);
    void LogCritical(const std::string& message);
    void LogTrace(const std::string& message);
    void LogDebug(const std::string& message);

    void RegisterCommand(const std::string& name, CommandHandler handler,
                         const std::string& description = "", const std::string& category = "General");
    bool ExecuteCommand(const std::string& commandLine);

    void Show();
    void Hide();
    void Toggle();
    bool IsVisible() const { return m_visible; }
    void Clear();

    std::vector<LogEntry> GetLogHistory() const;
    std::vector<std::string> GetCommandHistory() const;

    // Alias management
    void SetAlias(const std::string& alias, const std::string& command);
    void RemoveAlias(const std::string& alias);

    // Watch system
    void AddWatch(const std::string& name, std::function<std::string()> getter);
    void RemoveWatch(const std::string& name);
    void UpdateWatches();

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

    // Advanced command registration methods
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

    // Specialized command categories
    void RegisterRenderingCommands();      // Advanced rendering controls
    void RegisterAudioCommands();          // Audio system management
    void RegisterNetworkingCommands();     // Network simulation and control
    void RegisterProfilingCommands();      // Performance profiling tools
    void RegisterSceneCommands();          // Scene management and manipulation
    void RegisterInputCommands();          // Input system configuration
    void RegisterConfigCommands();         // Configuration management
    void RegisterTestingCommands();        // System testing and benchmarking
    void RegisterLogCommands();            // Log filtering, search, and export
    void RegisterWatchCommands();          // Watch variable management
    void RegisterAliasCommands();          // Command alias management
    void RegisterCrashCommands();          // Crash handling and diagnostics
    void RegisterHealthCommands();         // System health checks and monitoring
};

} // namespace Spark