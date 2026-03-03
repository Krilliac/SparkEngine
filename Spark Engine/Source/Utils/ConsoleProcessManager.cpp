#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "ConsoleProcessManager.h"
#include "Assert.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>

namespace Spark {

ConsoleProcessManager& ConsoleProcessManager::GetInstance() {
    static ConsoleProcessManager instance;
    return instance;
}

// **NEW: Global accessor function for Assert system**
ConsoleProcessManager& GetConsoleProcessManagerInstance() {
    return ConsoleProcessManager::GetInstance();
}

ConsoleProcessManager::ConsoleProcessManager() 
    : m_commandRegistry(std::make_unique<CommandRegistry>()) 
    , m_consoleThread()
    , m_shouldStopThread(false) {
    
    // Register default commands
    m_commandRegistry->RegisterCommand("help", 
        [this](const std::vector<std::string>& args) -> std::string {
            std::stringstream ss;
            ss << "Available commands:\n";
            auto commands = m_commandRegistry->GetAllCommands();
            for (const auto& cmd : commands) {
                ss << "  " << cmd.name;
                if (!cmd.description.empty()) {
                    ss << " - " << cmd.description;
                }
                ss << "\n";
                if (!cmd.usage.empty()) {
                    ss << "    Usage: " << cmd.usage << "\n";
                }
            }
            return ss.str();
        },
        "Show available commands",
        "help"
    );
    
    m_commandRegistry->RegisterCommand("quit", 
        [](const std::vector<std::string>& args) -> std::string {
            PostQuitMessage(0);
            return "Shutting down engine...";
        },
        "Quit the application",
        "quit"
    );

    // **NEW: Assert and crash handling commands**
    m_commandRegistry->RegisterCommand("assert_test", 
        [](const std::vector<std::string>& args) -> std::string {
            // Trigger a test assertion
            ASSERT_MSG(false, "Test assertion triggered from console command");
            return "This should not be reached";
        },
        "Trigger a test assertion",
        "assert_test"
    );

    m_commandRegistry->RegisterCommand("crash_test", 
        [](const std::vector<std::string>& args) -> std::string {
            // Trigger a test crash
            int* nullPtr = nullptr;
            *nullPtr = 42; // This will cause an access violation
            return "This should not be reached";
        },
        "Trigger a test crash",
        "crash_test"
    );

    m_commandRegistry->RegisterCommand("assert_mode", 
        [](const std::vector<std::string>& args) -> std::string {
            if (args.empty()) {
                return "Usage: assert_mode <on|off>\n"
                       "Controls whether assertions trigger crash dumps";
            }
            
            std::string mode = args[0];
            std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
            
            if (mode == "on" || mode == "true" || mode == "1") {
                extern void SetAssertCrashBehavior(bool);
                SetAssertCrashBehavior(true);
                return "Assert crash dumps enabled";
            } else if (mode == "off" || mode == "false" || mode == "0") {
                extern void SetAssertCrashBehavior(bool);
                SetAssertCrashBehavior(false);
                return "Assert crash dumps disabled";
            } else {
                return "Invalid mode. Use: on, off, true, false, 1, or 0";
            }
        },
        "Enable/disable crash dumps for assertions",
        "assert_mode <on|off>"
    );
}

ConsoleProcessManager::~ConsoleProcessManager() {
    Shutdown();
}

bool ConsoleProcessManager::Initialize(const std::wstring& consolePath) {
    if (m_initialized) {
        OutputDebugStringW(L"ConsoleProcessManager already initialized\n");
        return true;
    }
    
    // Output debug information for troubleshooting
    OutputDebugStringW(L"ConsoleProcessManager::Initialize starting...\n");
    
    // Get current executable directory to look for SparkConsole.exe
    wchar_t currentDir[MAX_PATH];
    GetModuleFileNameW(NULL, currentDir, MAX_PATH);
    std::wstring executableDir = std::filesystem::path(currentDir).parent_path().wstring();
    
    std::wstring debugMsg = L"SparkEngine executable directory: " + executableDir + L"\n";
    OutputDebugStringW(debugMsg.c_str());
    
    // **ENHANCED: Add more debug output**
    OutputDebugStringW(L"=== ConsoleProcessManager Debug Information ===\n");
    OutputDebugStringW((L"Executable path: " + std::wstring(currentDir) + L"\n").c_str());
    OutputDebugStringW((L"Working directory: " + executableDir + L"\n").c_str());
    OutputDebugStringW((L"Requested console path: " + consolePath + L"\n").c_str());
    
    // Comprehensive search paths for SparkConsole.exe
    std::vector<std::wstring> searchPaths = {
        consolePath,                                              // User-provided path
        executableDir + L"\\SparkConsole.exe",                   // Same directory as SparkEngine.exe
        executableDir + L"\\..\\SparkConsole\\SparkConsole.exe", // Relative to build structure
        L"bin\\Debug\\SparkConsole.exe",                         // CMake Debug build
        L"bin\\Release\\SparkConsole.exe",                       // CMake Release build
        L"bin\\SparkConsole.exe",                               // Generic bin directory
        L"..\\bin\\Debug\\SparkConsole.exe",                    // Parent directory CMake Debug
        L"..\\bin\\Release\\SparkConsole.exe",                  // Parent directory CMake Release
        L"..\\bin\\SparkConsole.exe",                           // Parent directory generic bin
        L"Debug\\SparkConsole.exe",                             // Visual Studio Debug
        L"Release\\SparkConsole.exe",                           // Visual Studio Release
        L"x64\\Debug\\SparkConsole.exe",                        // VS x64 Debug
        L"x64\\Release\\SparkConsole.exe",                      // VS x64 Release
        L"SparkConsole\\bin\\Debug\\SparkConsole.exe",          // CMake subproject Debug
        L"SparkConsole\\bin\\Release\\SparkConsole.exe",        // CMake subproject Release
        L"SparkConsole\\Debug\\SparkConsole.exe",               // VS subproject Debug
        L"SparkConsole\\Release\\SparkConsole.exe",             // VS subproject Release
        L"SparkConsole\\x64\\Debug\\SparkConsole.exe",          // VS subproject x64 Debug
        L"SparkConsole\\x64\\Release\\SparkConsole.exe",        // VS subproject x64 Release
        L".\\SparkConsole.exe",                                 // Current directory
        L"SparkConsole.exe"                                     // Simple name (PATH lookup)
    };
    
    OutputDebugStringW(L"Searching for SparkConsole.exe in the following locations:\n");
    
    std::wstring actualPath;
    for (const auto& path : searchPaths) {
        // Handle relative and absolute paths
        std::wstring fullPath;
        if (std::filesystem::path(path).is_absolute()) {
            fullPath = path;
        } else {
            fullPath = executableDir + L"\\" + path;
            // Also try from current working directory
            if (!std::filesystem::exists(fullPath)) {
                fullPath = path;
            }
        }
        
        std::wstring searchMsg = L"  Checking: " + fullPath;
        OutputDebugStringW(searchMsg.c_str());
        
        if (std::filesystem::exists(fullPath)) {
            actualPath = fullPath;
            OutputDebugStringW(L" -> FOUND!\n");
            Log(L"Found SparkConsole.exe at: " + actualPath, L"INFO");
            OutputDebugStringW((L"SUCCESS: Found SparkConsole.exe at: " + actualPath + L"\n").c_str());
            break;
        } else {
            OutputDebugStringW(L" -> NOT FOUND\n");
        }
    }
    
    if (actualPath.empty()) {
        // Console not found, fall back to OutputDebugString
        m_initialized = true;
        OutputDebugStringW(L"=== EXTERNAL CONSOLE NOT FOUND ===\n");
        OutputDebugStringW(L"SparkConsole.exe not found in any search location.\n");
        OutputDebugStringW(L"All engine output will appear in Visual Studio Output window.\n");
        OutputDebugStringW(L"External console features will not be available.\n");
        OutputDebugStringW(L"=====================================\n");
        
        Log(L"SparkConsole.exe not found in any search location. Search paths included:", L"WARNING");
        Log(L"  Current executable directory: " + executableDir, L"DEBUG");
        
        // Output first few search paths to debug log
        for (size_t i = 0; i < std::min(searchPaths.size(), size_t(5)); ++i) {
            Log(L"  Tried: " + searchPaths[i], L"DEBUG");
        }
        
        Log(L"External console not available - using fallback logging", L"WARNING");
        Log(L"All output will appear in Visual Studio Output window (Debug category)", L"INFO");
        return true; // Return true to continue engine startup
    }
    
    OutputDebugStringW(L"Attempting to launch SparkConsole.exe...\n");
    bool success = LaunchConsoleProcess(actualPath);
    m_initialized = true;
    
    if (success) {
        // **NEW: Start the background console management thread**
        m_shouldStopThread = false;
        m_consoleThread = std::thread(&ConsoleProcessManager::ConsoleThreadMain, this);
        
        OutputDebugStringW(L"=== EXTERNAL CONSOLE INITIALIZED ===\n");
        Log(L"External console system initialized successfully", L"SUCCESS");
        Log(L"Console management thread started", L"INFO");
        Log(L"Assert and crash logging integrated with external console", L"INFO");
        Log(L"SparkConsole.exe launched from: " + actualPath, L"INFO");
        Log(L"You should see a separate SparkConsole.exe window", L"INFO");
        OutputDebugStringW(L"SUCCESS: External console system initialized with multithreading\n");
        OutputDebugStringW(L"===================================\n");
    } else {
        OutputDebugStringW(L"=== EXTERNAL CONSOLE LAUNCH FAILED ===\n");
        Log(L"Failed to launch SparkConsole.exe! Communication will NOT be established.", L"ERROR");
        Log(L"Path attempted: " + actualPath, L"ERROR");
        Log(L"Engine will continue with debug output only", L"WARNING");
        OutputDebugStringW(L"ERROR: Failed to launch SparkConsole.exe\n");
        OutputDebugStringW((L"Path attempted: " + actualPath + L"\n").c_str());
        OutputDebugStringW(L"=====================================\n");
    }
    
    return success;
}

void ConsoleProcessManager::Shutdown() {
    if (!m_initialized) {
        return;
    }
    
    // **NEW: Stop the background thread first**
    m_shouldStopThread = true;
    if (m_consoleThread.joinable()) {
        m_consoleThread.join();
        Log(L"Console management thread stopped", L"INFO");
    }
    
    m_consoleRunning = false;
    
    // Send shutdown message to console
    if (m_stdInWrite) {
        Log(L"Shutting down external console connection...", L"INFO");
    }
    
    // Close pipe handles in correct order
    if (m_stdInWrite) {
        CloseHandle(m_stdInWrite);
        m_stdInWrite = NULL;
    }
    if (m_stdOutRead) {
        CloseHandle(m_stdOutRead);
        m_stdOutRead = NULL;
    }
    if (m_stdInRead) {
        CloseHandle(m_stdInRead);
        m_stdInRead = NULL;
    }
    if (m_stdOutWrite) {
        CloseHandle(m_stdOutWrite);
        m_stdOutWrite = NULL;
    }
    
    // Wait a moment for console to process final messages
    Sleep(100);
    
    // Terminate console process gracefully
    if (m_processHandle) {
        // Try to terminate gracefully first
        DWORD exitCode;
        if (GetExitCodeProcess(m_processHandle, &exitCode) && exitCode == STILL_ACTIVE) {
            // Send WM_CLOSE to console window
            DWORD processId = GetProcessId(m_processHandle);
            if (processId != 0) {
                // Find console window and close it gracefully
                HWND consoleWindow = GetConsoleWindow();
                if (consoleWindow) {
                    PostMessage(consoleWindow, WM_CLOSE, 0, 0);
                }
                
                // Wait for graceful shutdown
                if (WaitForSingleObject(m_processHandle, 2000) != WAIT_OBJECT_0) {
                    // Force terminate if graceful shutdown failed
                    TerminateProcess(m_processHandle, 0);
                    WaitForSingleObject(m_processHandle, 1000);
                }
            }
        }
        CloseHandle(m_processHandle);
        m_processHandle = NULL;
    }
    
    if (m_threadHandle) {
        CloseHandle(m_threadHandle);
        m_threadHandle = NULL;
    }
    
    m_initialized = false;
}

void ConsoleProcessManager::Log(const std::wstring& message, const std::wstring& type) {
    std::wstring formattedMessage = L"[" + type + L"] " + message;
    
    // **FIXED: Always output to debug console for debugging visibility**
    OutputDebugStringW(formattedMessage.c_str());
    OutputDebugStringW(L"\n");
    
    // Also try to send to external console if running
    if (m_consoleRunning && m_stdInWrite) {
        // Queue log message for background thread to send
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_messageQueue.push(formattedMessage);
        }
    }
}

// **NEW: Special logging method for crashes**
void ConsoleProcessManager::LogCrash(const std::string& crashInfo) {
    // Convert crash info to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, crashInfo.c_str(), -1, nullptr, 0);
    if (len > 0) {
        std::wstring wCrashInfo(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, crashInfo.c_str(), -1, &wCrashInfo[0], len);
        wCrashInfo.pop_back(); // Remove null terminator
        
        Log(wCrashInfo, L"CRASH");
    }
}

// **NEW: Main thread method - now non-blocking and thread-safe**
void ConsoleProcessManager::ProcessCommands() {
    if (!m_consoleRunning) {
        return;
    }
    
    // **NEW: Process commands from background thread queue (non-blocking)**
    std::queue<std::string> commandsToProcess;
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        if (m_commandQueue.empty()) {
            return; // No commands to process
        }
        commandsToProcess.swap(m_commandQueue);
    }
    
    while (!commandsToProcess.empty()) {
        std::string command = commandsToProcess.front();
        commandsToProcess.pop();
        
        // **DEBUG: Add debug output for command processing**
        OutputDebugStringA(("Processing command: " + command + "\n").c_str());
        
        try {
            std::string result = m_commandRegistry->ExecuteCommand(command);
            if (!result.empty()) {
                // **DEBUG: Add debug output for result**
                OutputDebugStringA(("Command result: " + result.substr(0, 100) + "...\n").c_str());
                
                // Send result back to console
                std::wstring wResult(result.begin(), result.end());
                Log(wResult, L"RESULT");
                
                // **DEBUG: Confirm result was sent**
                OutputDebugStringA("Result sent to console via Log()\n");
            } else {
                OutputDebugStringA("Command returned empty result\n");
            }
        } catch (const std::exception& e) {
            std::string error = "Command error: " + std::string(e.what());
            std::wstring wError(error.begin(), error.end());
            Log(wError, L"ERROR");
            OutputDebugStringA(("Command exception: " + std::string(e.what()) + "\n").c_str());
        }
    }
}

void ConsoleProcessManager::RegisterCommand(const std::string& name, 
                                          std::function<std::string(const std::vector<std::string>&)> handler,
                                          const std::string& description,
                                          const std::string& usage) {
    if (m_commandRegistry) {
        m_commandRegistry->RegisterCommand(name, handler, description, usage);
        Log(L"Registered command: " + std::wstring(name.begin(), name.end()), L"DEBUG");
    }
}

// **NEW: Background thread main function**
void ConsoleProcessManager::ConsoleThreadMain() {
    OutputDebugStringW(L"Console management thread started\n");
    
    while (!m_shouldStopThread && m_consoleRunning) {
        // Read commands from console (blocking operations are OK in background thread)
        if (ReadFromConsole()) {
            // Successfully read a command - continue immediately to check for more
            continue;
        }
        
        // Send queued log messages to console
        ProcessQueuedMessages();
        
        // Check if console process is still alive
        if (m_processHandle) {
            DWORD exitCode;
            if (GetExitCodeProcess(m_processHandle, &exitCode) && exitCode != STILL_ACTIVE) {
                m_consoleRunning = false;
                OutputDebugStringW(L"Console process has terminated\n");
                break;
            }
        }
        
        // Short sleep to prevent excessive CPU usage
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    OutputDebugStringW(L"Console management thread terminated\n");
}

// **NEW: Process queued messages (runs in background thread)**
void ConsoleProcessManager::ProcessQueuedMessages() {
    std::queue<std::wstring> messagesToSend;
    {
        std::lock_guard<std::mutex> lock(m_messageMutex);
        if (m_messageQueue.empty()) {
            return;
        }
        messagesToSend.swap(m_messageQueue);
    }
    
    while (!messagesToSend.empty()) {
        const std::wstring& message = messagesToSend.front();
        WriteToConsole(message);
        messagesToSend.pop();
    }
}

bool ConsoleProcessManager::LaunchConsoleProcess(const std::wstring& path) {
    // Create pipes for bidirectional communication
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    
    // Create pipes for communication
    HANDLE hChildStdInRead, hChildStdInWrite;
    HANDLE hChildStdOutRead, hChildStdOutWrite;
    
    // Create a pipe for the child process's STDIN (we write to this)
    if (!CreatePipe(&hChildStdInRead, &hChildStdInWrite, &sa, 0)) {
        DWORD err = GetLastError();
        Log(L"Failed to create stdin pipe (Error: " + std::to_wstring(err) + L")", L"ERROR");
        return false;
    }
    
    // Create a pipe for the child process's STDOUT (we read from this)
    if (!CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &sa, 0)) {
        DWORD err = GetLastError();
        Log(L"Failed to create stdout pipe (Error: " + std::to_wstring(err) + L")", L"ERROR");
        CloseHandle(hChildStdInRead);
        CloseHandle(hChildStdInWrite);
        return false;
    }
    
    // Ensure the read handle to the pipe for STDOUT is not inherited
    if (!SetHandleInformation(hChildStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
        DWORD err = GetLastError();
        Log(L"Failed to set stdout handle information (Error: " + std::to_wstring(err) + L")", L"ERROR");
        CloseHandle(hChildStdInRead);
        CloseHandle(hChildStdInWrite);
        CloseHandle(hChildStdOutRead);
        CloseHandle(hChildStdOutWrite);
        return false;
    }
    
    // Ensure the write handle to the pipe for STDIN is not inherited
    if (!SetHandleInformation(hChildStdInWrite, HANDLE_FLAG_INHERIT, 0)) {
        DWORD err = GetLastError();
        Log(L"Failed to set stdin handle information (Error: " + std::to_wstring(err) + L")", L"ERROR");
        CloseHandle(hChildStdInRead);
        CloseHandle(hChildStdInWrite);
        CloseHandle(hChildStdOutRead);
        CloseHandle(hChildStdOutWrite);
        return false;
    }
    
    // Create the child process
    STARTUPINFOW si = {};
    si.cb = sizeof(STARTUPINFOW);
    si.hStdError = hChildStdOutWrite;
    si.hStdOutput = hChildStdOutWrite;
    si.hStdInput = hChildStdInRead;
    si.dwFlags |= STARTF_USESTDHANDLES;
    
    PROCESS_INFORMATION pi = {};
    
    // Create command line
    std::wstring commandLine = L"\"" + path + L"\"";
    
    // Create the console process
    BOOL success = CreateProcessW(
        NULL,                          // Application name
        const_cast<LPWSTR>(commandLine.c_str()), // Command line
        NULL,                          // Process security attributes
        NULL,                          // Primary thread security attributes
        TRUE,                          // Handles are inherited
        CREATE_NEW_CONSOLE,            // Creation flags
        NULL,                          // Use parent's environment
        NULL,                          // Use parent's current directory
        &si,                           // STARTUPINFO
        &pi                            // PROCESS_INFORMATION
    );
    
    if (!success) {
        DWORD err = GetLastError();
        Log(L"CreateProcessW failed for SparkConsole.exe (Error: " + std::to_wstring(err) + L")", L"ERROR");
        Log(L"Command line attempted: " + commandLine, L"ERROR");
        CloseHandle(hChildStdInRead);
        CloseHandle(hChildStdInWrite);
        CloseHandle(hChildStdOutRead);
        CloseHandle(hChildStdOutWrite);
        return false;
    }
    
    // Close handles to the child process and its primary thread
    CloseHandle(hChildStdOutWrite);
    CloseHandle(hChildStdInRead);
    
    // Store our handles for communication
    m_processHandle = pi.hProcess;
    m_threadHandle = pi.hThread;
    m_stdInWrite = hChildStdInWrite;   // We write to child's stdin
    m_stdOutRead = hChildStdOutRead;   // We read from child's stdout
    
    m_consoleRunning = true;
    
    // Give the console process a moment to start up
    Sleep(250);
    
    // Send initial connection message
    Log(L"Console process launched successfully with pipes and multithreading", L"INFO");
    Log(L"External console connection established", L"INFO");
    Log(L"Process ID: " + std::to_wstring(GetProcessId(m_processHandle)), L"DEBUG");
    
    return true;
}

bool ConsoleProcessManager::ReadFromConsole() {
    if (!m_stdOutRead) {
        return false;
    }
    
    // **FIXED: Use non-blocking peek first to check for data**
    DWORD bytesAvailable = 0;
    if (!PeekNamedPipe(m_stdOutRead, NULL, 0, NULL, &bytesAvailable, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE) {
            m_consoleRunning = false;
            OutputDebugStringW(L"Console process connection lost during peek\n");
        }
        return false;
    }
    
    // **FIXED: Only try to read if data is actually available**
    if (bytesAvailable == 0) {
        return false; // No data available - this is normal, not an error
    }
    
    // **FIXED: Single, correct ReadFile call**
    char buffer[1024];
    DWORD bytesRead = 0;
    DWORD bytesToRead = std::min(bytesAvailable, static_cast<DWORD>(sizeof(buffer) - 1));
    
    if (!ReadFile(m_stdOutRead, buffer, bytesToRead, &bytesRead, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE) {
            m_consoleRunning = false;
            OutputDebugStringW(L"Console process connection lost during read\n");
        }
        return false;
    }
    
    if (bytesRead > 0) {
        buffer[bytesRead] = '\0';
        std::string commandLine(buffer);
        
        // Remove trailing newlines and carriage returns
        while (!commandLine.empty() && (commandLine.back() == '\n' || commandLine.back() == '\r')) {
            commandLine.pop_back();
        }
        
        if (!commandLine.empty()) {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandQueue.push(commandLine);
            OutputDebugStringA(("Console command received: " + commandLine + "\n").c_str());
            return true;
        }
    }
    
    return false;
}

bool ConsoleProcessManager::WriteToConsole(const std::wstring& message) {
    if (!m_stdInWrite) {
        OutputDebugStringA("WriteToConsole: No stdin write handle\n");
        return false;
    }
    
    // **DEBUG: Log what we're trying to send**
    std::string debugMsg = "WriteToConsole: Sending message (first 100 chars): ";
    std::string shortMsg(message.begin(), message.begin() + std::min(message.length(), size_t(100)));
    debugMsg += std::string(shortMsg.begin(), shortMsg.end()) + "\n";
    OutputDebugStringA(debugMsg.c_str());
    
    // Convert to UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, NULL, 0, NULL, NULL);
    if (utf8Size <= 0) {
        OutputDebugStringA("WriteToConsole: Failed to convert to UTF-8\n");
        return false;
    }
    
    std::string utf8Message(utf8Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, &utf8Message[0], utf8Size, NULL, NULL);
    utf8Message.pop_back(); // Remove null terminator
    utf8Message += "\n";
    
    DWORD bytesWritten = 0;
    BOOL result = WriteFile(m_stdInWrite, utf8Message.c_str(), static_cast<DWORD>(utf8Message.length()), &bytesWritten, NULL);
    
    if (result) {
        FlushFileBuffers(m_stdInWrite);
        OutputDebugStringA(("WriteToConsole: Successfully sent " + std::to_string(bytesWritten) + " bytes\n").c_str());
    } else {
        DWORD err = GetLastError();
        OutputDebugStringA(("WriteToConsole: WriteFile failed with error " + std::to_string(err) + "\n").c_str());
        if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE) {
            m_consoleRunning = false;
        }
    }
    
    return result != FALSE;
}

// CommandRegistry implementation
void CommandRegistry::RegisterCommand(const std::string& name, CommandHandler handler,
                                    const std::string& description,
                                    const std::string& usage) {
    CommandInfo info;
    info.name = name;
    info.handler = handler;
    info.description = description;
    info.usage = usage;
    
    m_commands[name] = info;
}

std::string CommandRegistry::ExecuteCommand(const std::string& commandLine) {
    auto args = ParseArguments(commandLine);
    if (args.empty()) {
        return "Empty command";
    }
    
    std::string commandName = args[0];
    args.erase(args.begin()); // Remove command name from args
    
    auto it = m_commands.find(commandName);
    if (it == m_commands.end()) {
        return "Unknown command: " + commandName;
    }
    
    try {
        return it->second.handler(args);
    } catch (const std::exception& e) {
        return "Command execution error: " + std::string(e.what());
    }
}

std::vector<CommandRegistry::CommandInfo> CommandRegistry::GetAllCommands() const {
    std::vector<CommandInfo> result;
    for (const auto& pair : m_commands) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<std::string> CommandRegistry::ParseArguments(const std::string& commandLine) {
    std::vector<std::string> args;
    std::istringstream iss(commandLine);
    std::string arg;
    
    while (iss >> arg) {
        args.push_back(arg);
    }
    
    return args;
}

} // namespace Spark
#endif // SPARK_PLATFORM_WINDOWS
