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
 * - Command history (via the "history" command)
 * - Command aliases (e.g. "q" -> "quit")
 * - Color-coded log output by severity
 */

#pragma once

#include "CommandRegistry.h"
#include "PipeMessageFramer.h"
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
    explicit ConsoleApp(bool enginePipeRequested = false);
    ~ConsoleApp();

    /** @brief Enter the main event loop; blocks until exit is requested. */
    void Run();

#ifdef SPARK_PLATFORM_WINDOWS
    /**
     * @brief Handle for everything a human reads — never the engine channel.
     *
     * In engine-pipe mode STD_OUTPUT_HANDLE is the command pipe back to the
     * engine: WriteConsoleW against it fails outright, and std::wcout against it
     * makes the engine execute the banner text as console commands. This opens
     * CONOUT$ instead (allocating a console first, because the engine that
     * launched us is a GUI process that owns none), so stdout stays reserved for
     * commands. Falls back to STD_OUTPUT_HANDLE only if no console can be had.
     */
    static HANDLE DisplayHandle();

    /** @brief Write human-facing text to DisplayHandle(). */
    static void WriteDisplay(const std::wstring& text);

    /** @brief Clear the display console buffer (replaces a `system("cls")`). */
    static void ClearDisplay();
#endif // SPARK_PLATFORM_WINDOWS

  private:
    // --- Thread entry points ---
    void ReadEngineInput(); ///< Background thread: reads log messages from engine pipe.

    // --- Run() helpers ---
    void PrintBanner();    ///< Clear screen and print the startup banner.
    bool DetectPipeMode(); ///< Detect if stdin is a pipe; print connection status. Returns true if pipe mode.
    void PipeKeyboardThreadFunc(std::string& input,
                                std::atomic<bool>& keyboardThreadRunning); ///< Keyboard input loop for pipe mode.
    void PollPipeModeInput(std::string& input, int& noInputCounter, bool& pipeMode,
                           std::atomic<bool>& keyboardThreadRunning); ///< One iteration of pipe-mode input polling.
    void PollStandaloneInput(std::string& input); ///< One iteration of standalone-mode input polling.

    // --- ReadEngineInput() helpers ---
    void ProcessPipeMessages(const std::string& message); ///< Frame newline-delimited pipe data into log lines.
#ifdef SPARK_PLATFORM_WINDOWS
    bool PollWindowsPipeData(HANDLE hStdin); ///< Read one batch from pipe; returns false if pipe lost.
    void ReadEngineInputWindows();           ///< Windows pipe reading loop.
#else
    void ReadEngineInputPosix(); ///< POSIX (Linux/macOS) pipe reading loop.
#endif

    // --- Keyboard input helpers (pipe-mode line editing) ---
    void HandleBackspaceKey(std::string& input);           ///< Process Backspace/DEL keypress.
    void HandlePrintableChar(std::string& input, char ch); ///< Process a printable character keypress.

    // --- Display ---
    void PrintLog(const std::wstring& msg);          ///< Print a raw log line to the console.
    void PrintEngineLog(const std::wstring& msg);    ///< Print an engine log line with severity coloring.
    void PrintDuplicateSkipNotice(int skippedCount); ///< Print a notice about skipped duplicate engine messages.
    void PrintResult(const std::string& result);     ///< Print a command result string.
#ifdef SPARK_PLATFORM_WINDOWS
    void SetConsoleColor(WORD color); ///< Set Win32 console text color attribute.
#else
    void SetConsoleColor(int color); ///< Set ANSI terminal color code.
#endif

    // --- Command handling ---
    void ExecuteCommand(const std::string& cmdLine); ///< Parse and dispatch a command line.
    void RegisterDefaultCommands();                  ///< Register all built-in commands.
    void RegisterCoreCommands();                     ///< Register core commands (help, clear, echo, version).
    void RegisterDiagnosticCommands();               ///< Register diagnostic commands (status, diag, pipe_test, etc).
    void RegisterAliasCommands();                    ///< Register alias/history commands and default aliases.
    bool ShouldForwardToEngine(const std::string& command); ///< True if this command should be sent to the engine.

    // --- Command history ---
    void AddToHistory(const std::string& cmd); ///< Append a command to the history ring buffer.

    // --- Alias system ---
    std::string ResolveAlias(const std::string& input); ///< Expand aliases before command dispatch.

    // --- State ---
    std::atomic<bool> m_running;     ///< False signals all threads to exit.
    bool m_enginePipeRequested;      ///< True only when launched by an engine/editor IPC parent.
    std::thread m_engineInputThread; ///< Background thread reading engine pipe input.
    std::mutex m_outputMutex;        ///< Serializes console output from multiple threads.
    std::mutex m_historyMutex;       ///< Guards m_commandHistory.

    // --- Commands ---
    CommandRegistry m_commandRegistry;         ///< Locally registered console commands.
    std::vector<std::string> m_commandHistory; ///< Ordered list of previously entered commands.

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
    PipeMessageFramer m_pipeMessageFramer;    ///< Retains partial records across byte-stream reads.

    // --- Aliases ---
    std::unordered_map<std::string, std::string> m_aliases; ///< Shorthand -> full command mappings.
};
