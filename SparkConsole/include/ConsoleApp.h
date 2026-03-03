#pragma once

#include "CommandRegistry.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <windows.h>

class ConsoleApp {
public:
    ConsoleApp();
    ~ConsoleApp();

    // Main execution loop
    void Run();

private:
    // Thread functions for reading input/output
    void ReadEngineInput();
    void ReadUserInput();

    // Display functions
    void PrintLog(const std::wstring& msg);
    void PrintEngineLog(const std::wstring& msg);
    void PrintResult(const std::string& result);
    void SetConsoleColor(WORD color);

    // Command handling
    void ExecuteCommand(const std::string& cmdLine);
    void RegisterDefaultCommands();
    bool ShouldForwardToEngine(const std::string& command);

    // Command history management
    void AddToHistory(const std::string& cmd);
    std::string GetPreviousCommand();
    std::string GetNextCommand();

    // Buffer management
    void ClearInputLine();
    void UpdateInputLine(const std::string& text);

    // Tab completion
    std::vector<std::string> GetCompletions(const std::string& prefix);
    void HandleTabCompletion(std::string& input);

    // Alias system
    std::string ResolveAlias(const std::string& input);

    // Thread safety and state
    std::atomic<bool> m_running;
    std::mutex m_outputMutex;
    std::mutex m_historyMutex;

    // Command registry and history
    CommandRegistry m_commandRegistry;
    std::vector<std::string> m_commandHistory;
    size_t m_historyIndex;
    std::string m_currentInput;

    // Console handles
    HANDLE m_consoleOutput;
    HANDLE m_consoleInput;

    // Message buffer for performance
    std::deque<std::wstring> m_messageBuffer;
    const size_t MAX_BUFFER_SIZE = 1000;

    // Command aliases
    std::unordered_map<std::string, std::string> m_aliases;

    // Tab completion state
    std::vector<std::string> m_tabCompletions;
    int m_tabIndex = -1;
    std::string m_tabPrefix;
};