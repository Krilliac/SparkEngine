/**
 * @file ExternalConsoleIntegration.cpp
 * @brief Enhanced external console integration with engine-style logging
 * @author Spark Engine Team
 * @date 2025
 */

#include "ExternalConsoleIntegration.h"
#include "Utils/Validate.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <cstring>
#endif

namespace SparkEditor
{

    ExternalConsoleIntegration::ExternalConsoleIntegration() {}

    ExternalConsoleIntegration::~ExternalConsoleIntegration()
    {
        Shutdown();
    }

    bool ExternalConsoleIntegration::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ExternalConsoleIntegration initializing");
        std::cout << "Initializing Enhanced External Console Integration with Engine-Style Logging\n";

        m_running = true;

        // Initialize message callback to display in console logs
        SetMessageCallback(
            [this](const ConsoleMessage& msg)
            { std::cout << "[" << msg.timestamp << "] [" << msg.level << "] " << msg.message << std::endl; });

        // Start network thread for communication
        try
        {
            m_networkThread = std::thread(&ExternalConsoleIntegration::NetworkThread, this);
            std::cout << "Network thread started successfully\n";
        }
        catch (const std::exception& e)
        {
            std::cout << "Failed to start network thread: " << e.what() << "\n";
            return false;
        }

        return true;
    }

    void ExternalConsoleIntegration::Shutdown()
    {
        if (m_running)
        {
            SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "ExternalConsoleIntegration shutting down");
            std::cout << "Shutting down Enhanced External Console Integration\n";

            try
            {
                m_running = false;

                // Disconnect first
                if (m_connected)
                {
                    Disconnect();
                }

                // Wait for network thread
                if (m_networkThread.joinable())
                {
                    std::cout << "Waiting for network thread to finish...\n";

                    auto start = std::chrono::steady_clock::now();
                    while (m_networkThread.joinable())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        auto elapsed = std::chrono::steady_clock::now() - start;
                        if (elapsed > std::chrono::seconds(2))
                        {
                            std::cout << "Network thread join timeout - forcing termination\n";
                            break;
                        }
                    }

                    if (m_networkThread.joinable())
                    {
                        try
                        {
                            m_networkThread.join();
                            std::cout << "Network thread joined successfully\n";
                        }
                        catch (const std::exception& e)
                        {
                            std::cout << "Exception joining network thread: " << e.what() << "\n";
                        }
                    }
                }

                // Close console process
#ifdef _WIN32
                if (m_consoleProcess != nullptr)
                {
                    try
                    {
                        std::cout << "Terminating console process...\n";

                        DWORD exitCode;
                        if (GetExitCodeProcess(m_consoleProcess, &exitCode) && exitCode == STILL_ACTIVE)
                        {
                            // Send exit command
                            if (m_stdinWrite)
                            {
                                std::string exitMsg = "exit\n";
                                DWORD bytesWritten = 0;
                                BOOL writeOk = WriteFile(m_stdinWrite, exitMsg.c_str(),
                                                         static_cast<DWORD>(exitMsg.length()), &bytesWritten, NULL);
                                if (writeOk)
                                {
                                    FlushFileBuffers(m_stdinWrite);
                                }
                            }

                            // Wait for graceful shutdown
                            if (WaitForSingleObject(m_consoleProcess, 1000) != WAIT_OBJECT_0)
                            {
                                TerminateProcess(m_consoleProcess, 0);
                                WaitForSingleObject(m_consoleProcess, 500);
                            }
                        }

                        CloseHandle(m_consoleProcess);
                        m_consoleProcess = nullptr;
                    }
                    catch (const std::exception& e)
                    {
                        std::cout << "Exception during console process termination: " << e.what() << "\n";
                    }

                    // Close pipes
                    try
                    {
                        if (m_stdinWrite)
                        {
                            CloseHandle(m_stdinWrite);
                            m_stdinWrite = nullptr;
                        }
                        if (m_stdoutRead)
                        {
                            CloseHandle(m_stdoutRead);
                            m_stdoutRead = nullptr;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::cout << "Exception closing pipe handles: " << e.what() << "\n";
                    }
                }
#else
                if (m_consolePid > 0)
                {
                    // Send exit command via stdin pipe
                    if (m_stdinWriteFd >= 0)
                    {
                        const char* exitMsg = "exit\n";
                        ssize_t written = write(m_stdinWriteFd, exitMsg, strlen(exitMsg));
                        if (written < 0)
                        {
                            SPARK_LOG_WARN(Spark::LogCategory::Core,
                                           "ExternalConsole: failed to send exit command to subprocess");
                        }
                    }

                    // Wait briefly for graceful shutdown
                    int status = 0;
                    pid_t result = waitpid(m_consolePid, &status, WNOHANG);
                    if (result == 0)
                    {
                        usleep(500000); // 500ms grace period
                        result = waitpid(m_consolePid, &status, WNOHANG);
                        if (result == 0)
                        {
                            kill(m_consolePid, SIGTERM);
                            usleep(200000);
                            waitpid(m_consolePid, &status, WNOHANG);
                        }
                    }
                    m_consolePid = -1;
                }

                if (m_stdinWriteFd >= 0)
                {
                    close(m_stdinWriteFd);
                    m_stdinWriteFd = -1;
                }
                if (m_stdoutReadFd >= 0)
                {
                    close(m_stdoutReadFd);
                    m_stdoutReadFd = -1;
                }
#endif

                std::cout << "Enhanced External Console Integration shutdown complete\n";
            }
            catch (const std::exception& e)
            {
                std::cout << "Exception during shutdown: " << e.what() << "\n";
            }
            catch (...)
            {
                std::cout << "Unknown exception during shutdown\n";
            }
        }
    }

    bool ExternalConsoleIntegration::ConnectToEngine(const std::string& host, int port)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !host.empty(), false);
        std::cout << "Connecting to external Spark Console with Engine-Style Logging...\n";

        // Try to launch SparkConsole.exe
        try
        {
            if (!LaunchExternalConsole())
            {
                std::cout << "Failed to launch external console\n";
                return false;
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception during external console launch: " << e.what() << "\n";
            return false;
        }

        m_host = host;
        m_port = port;
        m_connected = true;

        // Add connection message
        ConsoleMessage msg;
        msg.level = "INFO";
        msg.message = "Connected to Spark Console (SparkConsole.exe) - Engine-Style Logging Enabled";
        msg.timestamp = GetCurrentTimestamp();

        {
            std::lock_guard<std::mutex> lock(m_messagesMutex);
            m_messages.push_back(msg);
        }

        if (m_messageCallback)
        {
            try
            {
                m_messageCallback(msg);
            }
            catch (...)
            {
                std::cout << "Exception in message callback\n";
            }
        }

        // Send welcome message and start logging engine-style messages
        try
        {
            SendMessageToConsole("Editor connected - Engine-Style Logging Active");

            // Send some test messages to verify console communication
            LogToConsole("External console integration initialized", "SUCCESS");
            LogToConsole("Console communication established", "INFO");
            LogToConsole("Ready to receive engine commands and display logs", "INFO");
        }
        catch (...)
        {
            std::cout << "Warning: Failed to send initial messages to console\n";
        }

        return true;
    }

    void ExternalConsoleIntegration::Disconnect()
    {
        if (m_connected)
        {
            std::cout << "Disconnecting from external console\n";

            try
            {
                SendMessageToConsole("Editor disconnecting...");
            }
            catch (...)
            {
                std::cout << "Failed to send disconnect message\n";
            }

            ConsoleMessage msg;
            msg.level = "INFO";
            msg.message = "Disconnected from Spark Console";
            msg.timestamp = GetCurrentTimestamp();

            {
                std::lock_guard<std::mutex> lock(m_messagesMutex);
                m_messages.push_back(msg);
            }

            if (m_messageCallback)
            {
                try
                {
                    m_messageCallback(msg);
                }
                catch (...)
                {
                    std::cout << "Exception in disconnect callback\n";
                }
            }

            m_connected = false;
        }
    }

    bool ExternalConsoleIntegration::SendCommand(const std::string& command)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !command.empty(), false);
        if (!m_connected)
        {
            std::cout << "Cannot send command: not connected to console\n";
            return false;
        }

        std::cout << "Sending command to console: " << command << "\n";

        try
        {
            ConsoleMessage cmdMsg;
            cmdMsg.level = "COMMAND";
            cmdMsg.message = "> " + command;
            cmdMsg.timestamp = GetCurrentTimestamp();

            {
                std::lock_guard<std::mutex> lock(m_messagesMutex);
                m_messages.push_back(cmdMsg);
            }

            if (m_messageCallback)
            {
                m_messageCallback(cmdMsg);
            }

            // Send command to external console
            SendMessageToConsole("[EDITOR_CMD] " + command);

            return true;
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception sending command: " << e.what() << "\n";
            return false;
        }
    }

    // NEW: Engine-style logging method
    void ExternalConsoleIntegration::LogToConsole(const std::string& message, const std::string& level)
    {
        if (!m_connected)
        {
            // Still log to stdout even if not connected
            std::cout << "[" << GetCurrentTimestamp() << "] [" << level << "] " << message << std::endl;
            return;
        }

        try
        {
            // Create console message
            ConsoleMessage logMsg;
            logMsg.level = level;
            logMsg.message = message;
            logMsg.timestamp = GetCurrentTimestamp();

            {
                std::lock_guard<std::mutex> lock(m_messagesMutex);
                m_messages.push_back(logMsg);
            }

            if (m_messageCallback)
            {
                m_messageCallback(logMsg);
            }

            // Send to external console in engine format
            std::string formattedMessage = "[" + level + "] " + message;
            SendMessageToConsole(formattedMessage);
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception in LogToConsole: " << e.what() << "\n";
        }
    }

    void ExternalConsoleIntegration::SetMessageCallback(MessageCallback callback)
    {
        m_messageCallback = callback;
    }

    std::vector<ExternalConsoleIntegration::ConsoleMessage> ExternalConsoleIntegration::GetRecentMessages() const
    {
        try
        {
            std::lock_guard<std::mutex> lock(m_messagesMutex);
            return m_messages;
        }
        catch (...)
        {
            return std::vector<ConsoleMessage>();
        }
    }

    void ExternalConsoleIntegration::ClearMessages()
    {
        try
        {
            std::lock_guard<std::mutex> lock(m_messagesMutex);
            m_messages.clear();
        }
        catch (...)
        {
            std::cout << "Exception clearing messages\n";
        }
    }

    bool ExternalConsoleIntegration::LaunchExternalConsole()
    {
#ifdef _WIN32
        try
        {
            // Get current executable directory
            char currentDir[MAX_PATH];
            DWORD result = GetModuleFileNameA(NULL, currentDir, MAX_PATH);
            if (result == 0 || result == MAX_PATH)
            {
                DWORD err = GetLastError();
                std::cout << "Failed to get module filename (Error: " << err << ")\n";
                return false;
            }

            std::string executableDir = std::filesystem::path(currentDir).parent_path().string();
            std::cout << "SparkEditor executable directory: " << executableDir << "\n";

            // FIXED: Corrected search paths - the console is in the SAME directory as SparkEditor.exe
            std::vector<std::string> searchPaths = {
                executableDir + "\\SparkConsole.exe",              // Same directory as SparkEditor.exe (MOST LIKELY)
                ".\\SparkConsole.exe",                             // Current working directory
                "SparkConsole.exe",                                // Simple name (PATH lookup)
                executableDir + "\\..\\bin\\SparkConsole.exe",     // Up one level then bin
                executableDir + "\\..\\Debug\\SparkConsole.exe",   // Debug build location
                executableDir + "\\..\\Release\\SparkConsole.exe", // Release build location
                "bin\\SparkConsole.exe",                           // Relative bin
                "Debug\\SparkConsole.exe",                         // Relative debug
                "Release\\SparkConsole.exe"                        // Relative release
            };

            std::cout << "=== SEARCHING FOR SPARKCONSOLE.EXE ===" << "\n";
            std::cout << "SparkEditor path: " << currentDir << "\n";
            std::cout << "Search directory: " << executableDir << "\n";

            std::string consolePath;
            for (const auto& path : searchPaths)
            {
                std::string fullPath;
                if (std::filesystem::path(path).is_absolute())
                {
                    fullPath = path;
                }
                else
                {
                    // Try relative to executable directory first
                    fullPath = executableDir + "\\" + path;
                    if (!std::filesystem::exists(fullPath))
                    {
                        // Try relative to current working directory
                        fullPath = path;
                    }
                }

                std::cout << "  Checking: " << fullPath;

                if (std::filesystem::exists(fullPath))
                {
                    consolePath = fullPath;
                    std::cout << " -> FOUND!\n";
                    break;
                }
                else
                {
                    std::cout << " -> NOT FOUND\n";
                }
            }

            if (consolePath.empty())
            {
                std::cout << "=== SPARKCONSOLE.EXE NOT FOUND ===" << "\n";
                std::cout << "ERROR: SparkConsole.exe not found in any search location" << "\n";
                std::cout
                    << "Please ensure SparkConsole.exe is built and located in the same directory as SparkEditor.exe"
                    << "\n";
                std::cout << "=================================" << "\n";
                return false;
            }

            std::cout << "=== LAUNCHING SPARKCONSOLE.EXE ===" << "\n";
            std::cout << "Console path: " << consolePath << "\n";

            // Create pipes for communication
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = TRUE;
            sa.lpSecurityDescriptor = NULL;

            HANDLE hChildStdInRead = INVALID_HANDLE_VALUE;
            HANDLE hChildStdInWrite = INVALID_HANDLE_VALUE;
            HANDLE hChildStdOutRead = INVALID_HANDLE_VALUE;
            HANDLE hChildStdOutWrite = INVALID_HANDLE_VALUE;

            // Create pipes
            if (!CreatePipe(&hChildStdInRead, &hChildStdInWrite, &sa, 0))
            {
                DWORD err = GetLastError();
                std::cout << "Failed to create stdin pipe (Error: " << err << ")\n";
                return false;
            }

            if (!CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &sa, 0))
            {
                DWORD err = GetLastError();
                std::cout << "Failed to create stdout pipe (Error: " << err << ")\n";
                CloseHandle(hChildStdInRead);
                CloseHandle(hChildStdInWrite);
                return false;
            }

            // Set handle inheritance
            SetHandleInformation(hChildStdOutRead, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(hChildStdInWrite, HANDLE_FLAG_INHERIT, 0);

            // Create the console process
            STARTUPINFOA si = {};
            si.cb = sizeof(STARTUPINFOA);
            si.hStdError = hChildStdOutWrite;
            si.hStdOutput = hChildStdOutWrite;
            si.hStdInput = hChildStdInRead;
            si.dwFlags |= STARTF_USESTDHANDLES;

            PROCESS_INFORMATION pi = {};

            std::string commandLine = "\"" + consolePath + "\"";
            DWORD creationFlags = CREATE_NEW_CONSOLE;

            std::cout << "Creating console process: " << commandLine << "\n";

            BOOL success = CreateProcessA(NULL, const_cast<char*>(commandLine.c_str()), NULL, NULL, TRUE, creationFlags,
                                          NULL, NULL, &si, &pi);

            if (!success)
            {
                DWORD err = GetLastError();
                std::cout << "Failed to create SparkConsole.exe process (Error: " << err << ")\n";

                CloseHandle(hChildStdInRead);
                CloseHandle(hChildStdInWrite);
                CloseHandle(hChildStdOutRead);
                CloseHandle(hChildStdOutWrite);
                return false;
            }

            // Close handles we don't need
            CloseHandle(hChildStdOutWrite);
            CloseHandle(hChildStdInRead);
            CloseHandle(pi.hThread);

            // Store handles for communication
            m_consoleProcess = pi.hProcess;
            m_stdinWrite = hChildStdInWrite;
            m_stdoutRead = hChildStdOutRead;

            std::cout << "=== SPARKCONSOLE.EXE LAUNCHED SUCCESSFULLY! ===" << "\n";
            std::cout << "Process ID: " << pi.dwProcessId << "\n";
            std::cout << "Console communication pipes established" << "\n";
            std::cout << "===========================================" << "\n";

            // Give console time to start
            Sleep(1000);

            return true;
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception in LaunchExternalConsole: " << e.what() << "\n";
            return false;
        }
#else
        // Linux: launch SparkConsole via fork/exec with pipe-based IPC
        try
        {
            // Find SparkConsole binary
            std::string consolePath;
            std::vector<std::string> searchPaths = {"./SparkConsole", "../SparkConsole/SparkConsole",
                                                    "./build/SparkConsole/SparkConsole"};
            for (const auto& p : searchPaths)
            {
                if (std::filesystem::exists(p))
                {
                    consolePath = std::filesystem::absolute(p).string();
                    break;
                }
            }

            if (consolePath.empty())
            {
                std::cout << "SparkConsole binary not found on Linux\n";
                return false;
            }

            int stdinPipe[2];
            int stdoutPipe[2];

            if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0)
            {
                std::cout << "Failed to create pipes for SparkConsole\n";
                return false;
            }

            pid_t pid = fork();
            if (pid < 0)
            {
                std::cout << "Failed to fork for SparkConsole\n";
                close(stdinPipe[0]);
                close(stdinPipe[1]);
                close(stdoutPipe[0]);
                close(stdoutPipe[1]);
                return false;
            }

            if (pid == 0)
            {
                // Child process
                close(stdinPipe[1]);
                close(stdoutPipe[0]);

                dup2(stdinPipe[0], STDIN_FILENO);
                dup2(stdoutPipe[1], STDOUT_FILENO);
                dup2(stdoutPipe[1], STDERR_FILENO);

                close(stdinPipe[0]);
                close(stdoutPipe[1]);

                execl(consolePath.c_str(), consolePath.c_str(), nullptr);
                _exit(127);
            }

            // Parent process
            close(stdinPipe[0]);
            close(stdoutPipe[1]);

            m_consolePid = pid;
            m_stdinWriteFd = stdinPipe[1];
            m_stdoutReadFd = stdoutPipe[0];

            // Set stdout read to non-blocking
            int flags = fcntl(m_stdoutReadFd, F_GETFL, 0);
            fcntl(m_stdoutReadFd, F_SETFL, flags | O_NONBLOCK);

            std::cout << "=== SPARKCONSOLE LAUNCHED SUCCESSFULLY (Linux) ===" << "\n";
            std::cout << "Process ID: " << pid << "\n";
            std::cout << "Console communication pipes established" << "\n";
            std::cout << "=================================================" << "\n";

            usleep(500000); // Give console 500ms to start

            return true;
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception in LaunchExternalConsole (Linux): " << e.what() << "\n";
            return false;
        }
#endif
    }

    bool ExternalConsoleIntegration::SendMessageToConsole(const std::string& message)
    {
#ifdef _WIN32
        if (!m_stdinWrite)
        {
            return false;
        }

        try
        {
            std::string fullMessage = message + "\n";
            DWORD bytesWritten = 0;

            BOOL result = WriteFile(m_stdinWrite, fullMessage.c_str(), static_cast<DWORD>(fullMessage.length()),
                                    &bytesWritten, NULL);

            if (result)
            {
                FlushFileBuffers(m_stdinWrite);
            }

            return result != FALSE;
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception in SendMessageToConsole: " << e.what() << "\n";
            return false;
        }
#else
        if (m_stdinWriteFd < 0)
            return false;

        std::string fullMessage = message + "\n";
        ssize_t written = write(m_stdinWriteFd, fullMessage.c_str(), fullMessage.length());
        return written > 0;
#endif
    }

    bool ExternalConsoleIntegration::ReadFromConsole()
    {
#ifdef _WIN32
        if (!m_stdoutRead)
        {
            return false;
        }

        try
        {
            // Check if data is available
            DWORD bytesAvailable = 0;
            if (!PeekNamedPipe(m_stdoutRead, NULL, 0, NULL, &bytesAvailable, NULL))
            {
                return false;
            }

            if (bytesAvailable == 0)
            {
                return false;
            }

            // Read data
            char buffer[1024];
            DWORD bytesRead = 0;
            DWORD bytesToRead = std::min(bytesAvailable, static_cast<DWORD>(sizeof(buffer) - 1));

            if (!ReadFile(m_stdoutRead, buffer, bytesToRead, &bytesRead, NULL))
            {
                return false;
            }

            if (bytesRead > 0)
            {
                buffer[bytesRead] = '\0';
                std::string response(buffer);

                // Remove trailing newlines
                while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
                {
                    response.pop_back();
                }

                if (!response.empty())
                {
                    ConsoleMessage msg;
                    msg.level = "CONSOLE";
                    msg.message = response;
                    msg.timestamp = GetCurrentTimestamp();

                    {
                        std::lock_guard<std::mutex> lock(m_messagesMutex);
                        m_messages.push_back(msg);
                    }

                    if (m_messageCallback)
                    {
                        m_messageCallback(msg);
                    }

                    return true;
                }
            }

            return false;
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception in ReadFromConsole: " << e.what() << "\n";
            return false;
        }
#else
        if (m_stdoutReadFd < 0)
            return false;

        char buffer[1024];
        ssize_t bytesRead = read(m_stdoutReadFd, buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0)
            return false;

        buffer[bytesRead] = '\0';
        std::string response(buffer);

        while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
            response.pop_back();

        if (!response.empty())
        {
            ConsoleMessage msg;
            msg.level = "CONSOLE";
            msg.message = response;
            msg.timestamp = GetCurrentTimestamp();

            {
                std::lock_guard<std::mutex> lock(m_messagesMutex);
                m_messages.push_back(msg);
            }

            if (m_messageCallback)
                m_messageCallback(msg);

            return true;
        }

        return false;
#endif
    }

    void ExternalConsoleIntegration::NetworkThread()
    {
        try
        {
            std::cout << "Enhanced external console network thread started\n";

            while (m_running)
            {
                try
                {
                    if (m_connected)
                    {
                        // Read from console
                        ReadFromConsole();

                        // Check if console process is still alive
#ifdef _WIN32
                        if (m_consoleProcess)
                        {
                            DWORD exitCode;
                            if (GetExitCodeProcess(m_consoleProcess, &exitCode) && exitCode != STILL_ACTIVE)
                            {
                                std::cout << "Console process has terminated (exit code: " << exitCode << ")\n";
                                m_connected = false;
                            }
                        }
#endif
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                catch (const std::exception& e)
                {
                    std::cout << "Exception in network thread loop: " << e.what() << "\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
            }

            std::cout << "Enhanced external console network thread finished\n";
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception in NetworkThread: " << e.what() << "\n";
        }
    }

    void ExternalConsoleIntegration::ProcessIncomingData()
    {
        // Processing handled in NetworkThread
    }

    void ExternalConsoleIntegration::SimulateEngineConnection()
    {
        // Not needed - we have real console connection
    }

    std::string ExternalConsoleIntegration::GetCurrentTimestamp()
    {
        try
        {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

            std::stringstream ss;
            ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
            ss << "." << std::setfill('0') << std::setw(3) << ms.count();

            return ss.str();
        }
        catch (...)
        {
            return "ERROR";
        }
    }

} // namespace SparkEditor