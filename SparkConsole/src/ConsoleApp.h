/**
 * @file ConsoleApp.h
 * @brief SparkConsole application — external console subprocess for the Spark Engine.
 *
 * ConsoleApp runs as a separate process, connected to the engine via
 * stdin/stdout pipes. It reads log messages from the engine (via stdin),
 * displays them with color-coded severity, and accepts user commands which
 * are either handled locally or forwarded to the engine (via stdout).
 *
 * Features:
 * - Command history (up/down arrow navigation)
 * - Tab completion for registered commands
 * - Command aliases (e.g. "q" -> "quit")
 * - Color-coded log output by severity
 */

#pragma once

#include "CommandRegistry.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <unordered_map>
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

/**
 * @brief The SparkConsole subprocess application.
 *
 * Launched by ConsoleProcessManager in the engine process. Communicates
 * over inherited stdin/stdout pipe handles. Runs two threads: one reading
 * engine output (stdin pipe), one reading user keyboard input.
 */
class ConsoleApp
{
  public:
    ConsoleApp();
    ~ConsoleApp();

    /** @brief Enter the main event loop; blocks until exit is requested. */
    void Run();

  private:
    // --- Thread entry points ---
    void ReadEngineInput(); ///< Background thread: reads log messages from engine pipe.
    void ReadUserInput();   ///< Main thread: reads keyboard input and dispatches commands.

    // --- Display ---
    void PrintLog(const std::wstring& msg);       ///< Print a raw log line to the console.
    void PrintEngineLog(const std::wstring& msg); ///< Print an engine log line with severity coloring.
    void PrintResult(const std::string& result);  ///< Print a command result string.
#ifdef SPARK_PLATFORM_WINDOWS
    void SetConsoleColor(WORD color); ///< Set Win32 console text color attribute.
#else
    void SetConsoleColor(int color); ///< Set ANSI terminal color code.
#endif

    // --- Command handling ---
    void ExecuteCommand(const std::string& cmdLine);        ///< Parse and dispatch a command line.
    void RegisterDefaultCommands();                         ///< Register built-in commands (help, quit, clear, alias).
    bool ShouldForwardToEngine(const std::string& command); ///< True if this command should be sent to the engine.

    // --- Command history (up/down arrow navigation) ---
    void AddToHistory(const std::string& cmd); ///< Append a command to the history ring buffer.
    std::string GetPreviousCommand();          ///< Navigate backward in history (up arrow).
    std::string GetNextCommand();              ///< Navigate forward in history (down arrow).

    // --- Input line editing ---
    void ClearInputLine();                         ///< Erase the current input line from the display.
    void UpdateInputLine(const std::string& text); ///< Redraw the input line with new text.

    // --- Tab completion ---
    std::vector<std::string> GetCompletions(const std::string& prefix); ///< Find commands matching a prefix.
    void HandleTabCompletion(std::string& input);                       ///< Cycle through completions on Tab press.

    // --- Alias system ---
    std::string ResolveAlias(const std::string& input); ///< Expand aliases before command dispatch.

    // --- State ---
    std::atomic<bool> m_running;     ///< False signals all threads to exit.
    std::thread m_engineInputThread; ///< Background thread reading engine pipe input.
    std::mutex m_outputMutex;        ///< Serializes console output from multiple threads.
    std::mutex m_historyMutex;       ///< Guards m_commandHistory and m_historyIndex.

    // --- Commands ---
    CommandRegistry m_commandRegistry;         ///< Locally registered console commands.
    std::vector<std::string> m_commandHistory; ///< Ordered list of previously entered commands.
    size_t m_historyIndex;                     ///< Current position in the history (for arrow keys).
    std::string m_currentInput;                ///< The user's in-progress input line.

    // --- Console I/O handles ---
#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE m_consoleOutput; ///< Win32 console screen buffer handle.
    HANDLE m_consoleInput;  ///< Win32 console input buffer handle.
#else
    int m_consoleOutput; ///< stdout file descriptor.
    int m_consoleInput;  ///< stdin file descriptor.
#endif

    // --- Output buffer ---
    std::deque<std::wstring> m_messageBuffer; ///< Rolling log buffer for display.
    const size_t MAX_BUFFER_SIZE = 1000;      ///< Maximum retained log lines before oldest are discarded.

    // --- Aliases ---
    std::unordered_map<std::string, std::string> m_aliases; ///< Shorthand -> full command mappings.

    // --- Tab completion state ---
    std::vector<std::string> m_tabCompletions; ///< Completion candidates for the current prefix.
    int m_tabIndex = -1;                       ///< Index into m_tabCompletions (-1 = no active completion).
    std::string m_tabPrefix;                   ///< The prefix being completed.
};
