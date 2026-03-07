#pragma once
#include "../Core/Platform.h"

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

namespace Spark {

class CommandRegistry;

/**
 * @brief Manages communication with external SparkConsole process
 *
 * The ConsoleProcessManager handles launching the SparkConsole subprocess,
 * redirecting log messages to it, and receiving commands from it.
 * Serves as a replacement for standard output logging.
 * 
 * **NEW: Now uses multithreading to prevent blocking the main engine loop**
 */
class ConsoleProcessManager {
public:
    /**
     * @brief Get the singleton instance
     */
    static ConsoleProcessManager& GetInstance();
    
    /**
     * @brief Initialize the console process manager
     * @param consolePath Path to SparkConsole executable
     * @return True if initialization succeeded
     */
    bool Initialize(const std::wstring& consolePath = L"SparkConsole.exe");
    
    /**
     * @brief Shut down the console process manager
     */
    void Shutdown();
    
    /**
     * @brief Send a log message to the console (thread-safe, non-blocking)
     * @param message Log message to send
     * @param type Log type (INFO, WARNING, ERROR, etc.)
     */
    void Log(const std::wstring& message, const std::wstring& type = L"INFO");
    
    /**
     * @brief Send crash information to the console
     * @param crashInfo Crash details in UTF-8 format
     */
    void LogCrash(const std::string& crashInfo);
    
    /**
     * @brief Check for and process pending commands (non-blocking)
     * Should be called each frame to execute commands from console
     */
    void ProcessCommands();
    
    /**
     * @brief Register a command with the console
     * @param name Command name
     * @param handler Function to execute when command is invoked
     * @param description Command description for help text
     * @param usage Usage example for help text
     */
    void RegisterCommand(const std::string& name, 
                        std::function<std::string(const std::vector<std::string>&)> handler,
                        const std::string& description = "",
                        const std::string& usage = "");

    /**
     * @brief Check if console process is running
     */
    bool IsConsoleRunning() const { return m_consoleRunning; }

private:
    ConsoleProcessManager();
    ~ConsoleProcessManager();
    
    // Prevent copying and assignment
    ConsoleProcessManager(const ConsoleProcessManager&) = delete;
    ConsoleProcessManager& operator=(const ConsoleProcessManager&) = delete;
    
    // Launch the console process
    bool LaunchConsoleProcess(const std::wstring& path);
    
    // Read from console process (blocking - runs in background thread)
    bool ReadFromConsole();
    
    // Write to console process (blocking - runs in background thread)
    bool WriteToConsole(const std::wstring& message);
    
    // **NEW: Background thread methods**
    void ConsoleThreadMain();
    void ProcessQueuedMessages();
    
    // Process handles
    HANDLE m_processHandle = NULL;
    HANDLE m_threadHandle = NULL;
    
    // Pipe handles for communication
    HANDLE m_stdInRead = NULL;
    HANDLE m_stdInWrite = NULL;
    HANDLE m_stdOutRead = NULL;
    HANDLE m_stdOutWrite = NULL;
    
    // Command registry
    std::unique_ptr<CommandRegistry> m_commandRegistry;
    
    // State
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_consoleRunning{false};
    
    // **NEW: Multithreading support**
    std::thread m_consoleThread;
    std::atomic<bool> m_shouldStopThread{false};
    
    // Thread safety for message queue
    std::mutex m_messageMutex;
    std::queue<std::wstring> m_messageQueue;
    
    // Command execution queue
    std::mutex m_commandMutex;
    std::queue<std::string> m_commandQueue;
};

/**
 * @brief Simple command registry for console commands
 */
class CommandRegistry {
public:
    using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;
    
    struct CommandInfo {
        std::string name;
        std::string description;
        std::string usage;
        CommandHandler handler;
    };
    
    void RegisterCommand(const std::string& name, CommandHandler handler,
                        const std::string& description = "",
                        const std::string& usage = "");
    
    std::string ExecuteCommand(const std::string& commandLine);
    
    std::vector<CommandInfo> GetAllCommands() const;
    
private:
    std::unordered_map<std::string, CommandInfo> m_commands;
    std::vector<std::string> ParseArguments(const std::string& commandLine);
};

// **NEW: Global accessor function for Assert system**
ConsoleProcessManager& GetConsoleProcessManagerInstance();

} // namespace Spark
