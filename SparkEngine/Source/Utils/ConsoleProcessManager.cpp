#include "Core/Platform.h"
#include "ConsoleProcessManager.h"
#include "Utils/Assert.h"
#include "Utils/CrashHandler.h"
#include "Validate.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>

#ifdef SPARK_PLATFORM_LINUX
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#endif

namespace Spark
{

    // ============================================================================
    // Cross-platform helpers
    // ============================================================================

    static std::string WStrToStr(const std::wstring& w)
    {
        std::string result;
        result.reserve(w.size());
        for (wchar_t c : w)
        {
            if (c < 0x80)
                result.push_back(static_cast<char>(c));
            else
                result.push_back('?');
        }
        return result;
    }

    ConsoleProcessManager& ConsoleProcessManager::GetInstance()
    {
        static ConsoleProcessManager instance;
        return instance;
    }

    ConsoleProcessManager& GetConsoleProcessManagerInstance()
    {
        return ConsoleProcessManager::GetInstance();
    }

    ConsoleProcessManager::ConsoleProcessManager()
        : m_commandRegistry(std::make_unique<CommandRegistry>()), m_consoleThread(), m_shouldStopThread(false)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager constructed");

        // Register default commands
        m_commandRegistry->RegisterCommand(
            "help",
            [this](const std::vector<std::string>& args) -> std::string
            {
                std::stringstream ss;
                ss << "Available commands:\n";
                auto commands = m_commandRegistry->GetAllCommands();
                for (const auto& cmd : commands)
                {
                    ss << "  " << cmd.name;
                    if (!cmd.description.empty())
                        ss << " - " << cmd.description;
                    ss << "\n";
                    if (!cmd.usage.empty())
                        ss << "    Usage: " << cmd.usage << "\n";
                }
                return ss.str();
            },
            "Show available commands", "help");

        m_commandRegistry->RegisterCommand(
            "quit",
            [](const std::vector<std::string>& args) -> std::string
            {
#ifdef SPARK_PLATFORM_WINDOWS
                PostQuitMessage(0);
#else
                // On Linux, request graceful shutdown via signal
                kill(getpid(), SIGTERM);
#endif
                return "Shutting down engine...";
            },
            "Quit the application", "quit");

        m_commandRegistry->RegisterCommand(
            "assert_test",
            [](const std::vector<std::string>& args) -> std::string
            {
                SPARK_REQUIRE_MSG(Spark::LogCategory::Core, false, "Test assertion triggered from console command");
                return "This should not be reached";
            },
            "Trigger a test assertion", "assert_test");

        m_commandRegistry->RegisterCommand(
            "crash_test",
            [](const std::vector<std::string>& args) -> std::string
            {
                int* nullPtr = nullptr;
                *nullPtr = 42;
                return "This should not be reached";
            },
            "Trigger a test crash", "crash_test");

        m_commandRegistry->RegisterCommand(
            "assert_mode",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                {
                    return "Usage: assert_mode <on|off>\nControls whether assertions trigger crash dumps";
                }
                std::string mode = args[0];
                std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                if (mode == "on" || mode == "true" || mode == "1")
                {
                    ::SetAssertCrashBehavior(true);
                    return "Assert crash dumps enabled";
                }
                else if (mode == "off" || mode == "false" || mode == "0")
                {
                    ::SetAssertCrashBehavior(false);
                    return "Assert crash dumps disabled";
                }
                return "Invalid mode. Use: on, off, true, false, 1, or 0";
            },
            "Enable/disable crash dumps for assertions", "assert_mode <on|off>");
    }

    ConsoleProcessManager::~ConsoleProcessManager()
    {
        Shutdown();
    }

    // ============================================================================
    // WINDOWS IMPLEMENTATION
    // ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

    bool ConsoleProcessManager::Initialize(const std::wstring& consolePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (m_initialized)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "ConsoleProcessManager already initialized");
            return true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager::Initialize starting");

        wchar_t currentDir[MAX_PATH];
        GetModuleFileNameW(NULL, currentDir, MAX_PATH);
        std::wstring executableDir = std::filesystem::path(currentDir).parent_path().wstring();

        std::vector<std::wstring> searchPaths = {consolePath,
                                                 executableDir + L"\\SparkConsole.exe",
                                                 executableDir + L"\\..\\SparkConsole\\SparkConsole.exe",
                                                 L"bin\\Debug\\SparkConsole.exe",
                                                 L"bin\\Release\\SparkConsole.exe",
                                                 L".\\SparkConsole.exe",
                                                 L"SparkConsole.exe"};

        std::wstring actualPath;
        for (const auto& path : searchPaths)
        {
            std::wstring fullPath;
            if (std::filesystem::path(path).is_absolute())
            {
                fullPath = path;
            }
            else
            {
                fullPath = executableDir + L"\\" + path;
                if (!std::filesystem::exists(fullPath))
                    fullPath = path;
            }
            if (std::filesystem::exists(fullPath))
            {
                actualPath = fullPath;
                break;
            }
        }

        if (actualPath.empty())
        {
            m_initialized = true;
            OutputDebugStringW(L"SparkConsole.exe not found. Using fallback logging.\n");
            return true;
        }

        bool success = LaunchConsoleProcess(actualPath);
        m_initialized = true;

        if (success)
        {
            m_shouldStopThread = false;
            m_consoleThread = std::thread(&ConsoleProcessManager::ConsoleThreadMain, this);
        }
        return success;
    }

    void ConsoleProcessManager::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (!m_initialized)
            return;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager shutting down");
        m_shouldStopThread = true;
        if (m_consoleThread.joinable())
            m_consoleThread.join();
        m_consoleRunning = false;

        if (m_stdInWrite)
        {
            CloseHandle(m_stdInWrite);
            m_stdInWrite = NULL;
        }
        if (m_stdOutRead)
        {
            CloseHandle(m_stdOutRead);
            m_stdOutRead = NULL;
        }
        if (m_stdInRead)
        {
            CloseHandle(m_stdInRead);
            m_stdInRead = NULL;
        }
        if (m_stdOutWrite)
        {
            CloseHandle(m_stdOutWrite);
            m_stdOutWrite = NULL;
        }

        Sleep(100);

        if (m_processHandle)
        {
            DWORD exitCode;
            if (GetExitCodeProcess(m_processHandle, &exitCode) && exitCode == STILL_ACTIVE)
            {
                TerminateProcess(m_processHandle, 0);
                WaitForSingleObject(m_processHandle, 1000);
            }
            CloseHandle(m_processHandle);
            m_processHandle = NULL;
        }
        if (m_threadHandle)
        {
            CloseHandle(m_threadHandle);
            m_threadHandle = NULL;
        }
        m_initialized = false;
    }

    void ConsoleProcessManager::Log(const std::wstring& message, const std::wstring& type)
    {
        std::wstring formatted = L"[" + type + L"] " + message;
        OutputDebugStringW(formatted.c_str());
        OutputDebugStringW(L"\n");
        if (m_consoleRunning && m_stdInWrite)
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_messageQueue.push(formatted);
        }
    }

    void ConsoleProcessManager::LogCrash(const std::string& crashInfo)
    {
        int len = MultiByteToWideChar(CP_UTF8, 0, crashInfo.c_str(), -1, nullptr, 0);
        if (len > 0)
        {
            std::wstring w(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, crashInfo.c_str(), -1, &w[0], len);
            w.pop_back();
            Log(w, L"CRASH");
        }
    }

    bool ConsoleProcessManager::LaunchConsoleProcess(const std::wstring& path)
    {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;

        HANDLE hChildStdInRead, hChildStdInWrite;
        HANDLE hChildStdOutRead, hChildStdOutWrite;

        if (!CreatePipe(&hChildStdInRead, &hChildStdInWrite, &sa, 0))
            return false;
        if (!CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &sa, 0))
        {
            CloseHandle(hChildStdInRead);
            CloseHandle(hChildStdInWrite);
            return false;
        }
        SetHandleInformation(hChildStdOutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(hChildStdInWrite, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {};
        si.cb = sizeof(STARTUPINFOW);
        si.hStdError = hChildStdOutWrite;
        si.hStdOutput = hChildStdOutWrite;
        si.hStdInput = hChildStdInRead;
        si.dwFlags |= STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi = {};
        std::wstring commandLine = L"\"" + path + L"\"";

        BOOL success = CreateProcessW(NULL, const_cast<LPWSTR>(commandLine.c_str()), NULL, NULL, TRUE,
                                      CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);

        if (!success)
        {
            CloseHandle(hChildStdInRead);
            CloseHandle(hChildStdInWrite);
            CloseHandle(hChildStdOutRead);
            CloseHandle(hChildStdOutWrite);
            return false;
        }

        CloseHandle(hChildStdOutWrite);
        CloseHandle(hChildStdInRead);

        m_processHandle = pi.hProcess;
        m_threadHandle = pi.hThread;
        m_stdInWrite = hChildStdInWrite;
        m_stdOutRead = hChildStdOutRead;
        m_consoleRunning = true;
        Sleep(250);
        return true;
    }

    bool ConsoleProcessManager::ReadFromConsole()
    {
        if (!m_stdOutRead)
            return false;

        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(m_stdOutRead, NULL, 0, NULL, &bytesAvailable, NULL))
        {
            m_consoleRunning = false;
            return false;
        }
        if (bytesAvailable == 0)
            return false;

        char buffer[1024];
        DWORD bytesRead = 0;
        DWORD bytesToRead = std::min(bytesAvailable, static_cast<DWORD>(sizeof(buffer) - 1));
        if (!ReadFile(m_stdOutRead, buffer, bytesToRead, &bytesRead, NULL))
        {
            m_consoleRunning = false;
            return false;
        }

        if (bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            std::string commandLine(buffer);
            while (!commandLine.empty() && (commandLine.back() == '\n' || commandLine.back() == '\r'))
                commandLine.pop_back();
            if (!commandLine.empty())
            {
                std::lock_guard<std::mutex> lock(m_commandMutex);
                m_commandQueue.push(commandLine);
                return true;
            }
        }
        return false;
    }

    bool ConsoleProcessManager::WriteToConsole(const std::wstring& message)
    {
        if (!m_stdInWrite)
            return false;

        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, NULL, 0, NULL, NULL);
        if (utf8Size <= 0)
            return false;

        std::string utf8Message(utf8Size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, &utf8Message[0], utf8Size, NULL, NULL);
        utf8Message.pop_back();
        utf8Message += "\n";

        DWORD bytesWritten = 0;
        BOOL result =
            WriteFile(m_stdInWrite, utf8Message.c_str(), static_cast<DWORD>(utf8Message.length()), &bytesWritten, NULL);
        if (result)
            FlushFileBuffers(m_stdInWrite);
        return result != FALSE;
    }

    void ConsoleProcessManager::ConsoleThreadMain()
    {
        while (!m_shouldStopThread && m_consoleRunning)
        {
            if (ReadFromConsole())
                continue;
            ProcessQueuedMessages();
            if (m_processHandle)
            {
                DWORD exitCode;
                if (GetExitCodeProcess(m_processHandle, &exitCode) && exitCode != STILL_ACTIVE)
                {
                    m_consoleRunning = false;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // ============================================================================
    // LINUX IMPLEMENTATION
    // ============================================================================

#elif defined(SPARK_PLATFORM_LINUX)

    bool ConsoleProcessManager::Initialize(const std::wstring& consolePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (m_initialized)
            return true;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager::Initialize starting (Linux)");

        // Get the executable directory
        char exePath[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        std::string executableDir;
        if (len != -1)
        {
            exePath[len] = '\0';
            executableDir = std::filesystem::path(exePath).parent_path().string();
        }
        else
        {
            executableDir = ".";
        }

        std::string consoleBaseName = WStrToStr(consolePath);
        if (consoleBaseName.empty())
            consoleBaseName = "SparkConsole";

        // Search paths for SparkConsole binary
        std::vector<std::string> searchPaths = {
            consoleBaseName,
            executableDir + "/" + consoleBaseName,
            executableDir + "/../SparkConsole/" + consoleBaseName,
            "bin/Debug/" + consoleBaseName,
            "bin/Release/" + consoleBaseName,
            "./" + consoleBaseName,
        };

        std::string actualPath;
        for (const auto& path : searchPaths)
        {
            if (std::filesystem::exists(path))
            {
                actualPath = path;
                break;
            }
        }

        if (actualPath.empty())
        {
            m_initialized = true;
            std::cerr << "[ConsoleProcessManager] SparkConsole not found. Using fallback logging.\n";
            return true;
        }

        bool success = LaunchConsoleProcess(std::wstring(actualPath.begin(), actualPath.end()));
        m_initialized = true;

        if (success)
        {
            m_shouldStopThread = false;
            m_consoleThread = std::thread(&ConsoleProcessManager::ConsoleThreadMain, this);
        }
        return success;
    }

    void ConsoleProcessManager::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (!m_initialized)
            return;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager shutting down (Linux)");
        m_shouldStopThread = true;
        if (m_consoleThread.joinable())
            m_consoleThread.join();
        m_consoleRunning = false;

        // Close pipe write end first to signal child
        if (m_pipeToChild[1] >= 0)
        {
            close(m_pipeToChild[1]);
            m_pipeToChild[1] = -1;
        }
        if (m_pipeFromChild[0] >= 0)
        {
            close(m_pipeFromChild[0]);
            m_pipeFromChild[0] = -1;
        }

        // Wait for child process
        if (m_childPid > 0)
        {
            int status;
            // Give it a moment to exit gracefully
            usleep(100000); // 100ms
            if (waitpid(m_childPid, &status, WNOHANG) == 0)
            {
                // Still running, send SIGTERM
                kill(m_childPid, SIGTERM);
                usleep(500000); // 500ms
                if (waitpid(m_childPid, &status, WNOHANG) == 0)
                {
                    // Force kill
                    kill(m_childPid, SIGKILL);
                    waitpid(m_childPid, &status, 0);
                }
            }
            m_childPid = -1;
        }

        m_initialized = false;
    }

    void ConsoleProcessManager::Log(const std::wstring& message, const std::wstring& type)
    {
        std::string formatted = "[" + WStrToStr(type) + "] " + WStrToStr(message);
        std::cerr << formatted << "\n";

        if (m_consoleRunning && m_pipeToChild[1] >= 0)
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_messageQueue.push(message);
        }
    }

    void ConsoleProcessManager::LogCrash(const std::string& crashInfo)
    {
        std::wstring w(crashInfo.begin(), crashInfo.end());
        Log(w, L"CRASH");
    }

    bool ConsoleProcessManager::LaunchConsoleProcess(const std::wstring& path)
    {
        std::string pathStr = WStrToStr(path);

        // Create pipes: parent->child stdin, child stdout->parent
        if (pipe(m_pipeToChild) == -1)
            return false;
        if (pipe(m_pipeFromChild) == -1)
        {
            close(m_pipeToChild[0]);
            close(m_pipeToChild[1]);
            return false;
        }

        m_childPid = fork();
        if (m_childPid == -1)
        {
            close(m_pipeToChild[0]);
            close(m_pipeToChild[1]);
            close(m_pipeFromChild[0]);
            close(m_pipeFromChild[1]);
            return false;
        }

        if (m_childPid == 0)
        {
            // Child process
            close(m_pipeToChild[1]);   // Close write end of parent->child pipe
            close(m_pipeFromChild[0]); // Close read end of child->parent pipe

            dup2(m_pipeToChild[0], STDIN_FILENO);
            dup2(m_pipeFromChild[1], STDOUT_FILENO);
            dup2(m_pipeFromChild[1], STDERR_FILENO);

            close(m_pipeToChild[0]);
            close(m_pipeFromChild[1]);

            execl(pathStr.c_str(), pathStr.c_str(), nullptr);
            _exit(127); // exec failed
        }

        // Parent process
        close(m_pipeToChild[0]);   // Close read end of parent->child pipe
        close(m_pipeFromChild[1]); // Close write end of child->parent pipe
        m_pipeToChild[0] = -1;
        m_pipeFromChild[1] = -1;

        // Set read end to non-blocking
        int flags = fcntl(m_pipeFromChild[0], F_GETFL, 0);
        fcntl(m_pipeFromChild[0], F_SETFL, flags | O_NONBLOCK);

        m_consoleRunning = true;
        usleep(250000); // 250ms
        return true;
    }

    bool ConsoleProcessManager::ReadFromConsole()
    {
        if (m_pipeFromChild[0] < 0)
            return false;

        // Use poll to check if data is available
        struct pollfd pfd;
        pfd.fd = m_pipeFromChild[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 0); // Non-blocking
        if (ret <= 0)
            return false;

        char buffer[1024];
        ssize_t bytesRead = read(m_pipeFromChild[0], buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0)
        {
            if (bytesRead == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                m_consoleRunning = false;
            }
            return false;
        }

        buffer[bytesRead] = '\0';
        std::string commandLine(buffer);
        while (!commandLine.empty() && (commandLine.back() == '\n' || commandLine.back() == '\r'))
            commandLine.pop_back();

        if (!commandLine.empty())
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandQueue.push(commandLine);
            return true;
        }
        return false;
    }

    bool ConsoleProcessManager::WriteToConsole(const std::wstring& message)
    {
        if (m_pipeToChild[1] < 0)
            return false;

        std::string utf8 = WStrToStr(message) + "\n";
        ssize_t written = write(m_pipeToChild[1], utf8.c_str(), utf8.length());
        return written > 0;
    }

    void ConsoleProcessManager::ConsoleThreadMain()
    {
        while (!m_shouldStopThread && m_consoleRunning)
        {
            if (ReadFromConsole())
                continue;
            ProcessQueuedMessages();

            // Check if child process is still alive
            if (m_childPid > 0)
            {
                int status;
                pid_t result = waitpid(m_childPid, &status, WNOHANG);
                if (result != 0)
                {
                    m_consoleRunning = false;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

#else
    // Unsupported platform stubs
    bool ConsoleProcessManager::Initialize(const std::wstring&)
    {
        m_initialized = true;
        return true;
    }
    void ConsoleProcessManager::Shutdown()
    {
        m_initialized = false;
    }
    void ConsoleProcessManager::Log(const std::wstring& msg, const std::wstring& type)
    {
        std::cerr << "[" << WStrToStr(type) << "] " << WStrToStr(msg) << "\n";
    }
    void ConsoleProcessManager::LogCrash(const std::string& info)
    {
        std::cerr << "[CRASH] " << info << "\n";
    }
    bool ConsoleProcessManager::LaunchConsoleProcess(const std::wstring&)
    {
        return false;
    }
    bool ConsoleProcessManager::ReadFromConsole()
    {
        return false;
    }
    bool ConsoleProcessManager::WriteToConsole(const std::wstring&)
    {
        return false;
    }
    void ConsoleProcessManager::ConsoleThreadMain() {}
#endif

    // ============================================================================
    // Shared (cross-platform) methods
    // ============================================================================

    void ConsoleProcessManager::ProcessCommands()
    {
        if (!m_consoleRunning)
            return;

        std::queue<std::string> commandsToProcess;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            if (m_commandQueue.empty())
                return;
            commandsToProcess.swap(m_commandQueue);
        }

        while (!commandsToProcess.empty())
        {
            std::string command = commandsToProcess.front();
            commandsToProcess.pop();

            try
            {
                std::string result = m_commandRegistry->ExecuteCommand(command);
                if (!result.empty())
                {
                    std::wstring wResult(result.begin(), result.end());
                    Log(wResult, L"RESULT");
                }
            }
            catch (const std::exception& e)
            {
                std::string error = "Command error: " + std::string(e.what());
                std::wstring wError(error.begin(), error.end());
                Log(wError, L"ERROR");
            }
        }
    }

    void ConsoleProcessManager::ProcessQueuedMessages()
    {
        std::queue<std::wstring> messagesToSend;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            if (m_messageQueue.empty())
                return;
            messagesToSend.swap(m_messageQueue);
        }
        while (!messagesToSend.empty())
        {
            WriteToConsole(messagesToSend.front());
            messagesToSend.pop();
        }
    }

    void ConsoleProcessManager::RegisterCommand(const std::string& name,
                                                std::function<std::string(const std::vector<std::string>&)> handler,
                                                const std::string& description, const std::string& usage)
    {
        if (m_commandRegistry)
        {
            m_commandRegistry->RegisterCommand(name, handler, description, usage);
        }
    }

    // ============================================================================
    // CommandRegistry implementation
    // ============================================================================

    void CommandRegistry::RegisterCommand(const std::string& name, CommandHandler handler,
                                          const std::string& description, const std::string& usage)
    {
        CommandInfo info;
        info.name = name;
        info.handler = handler;
        info.description = description;
        info.usage = usage;
        m_commands[name] = info;
    }

    std::string CommandRegistry::ExecuteCommand(const std::string& commandLine)
    {
        auto args = ParseArguments(commandLine);
        if (args.empty())
            return "Empty command";
        std::string commandName = args[0];
        args.erase(args.begin());
        auto it = m_commands.find(commandName);
        if (it == m_commands.end())
            return "Unknown command: " + commandName;
        try
        {
            return it->second.handler(args);
        }
        catch (const std::exception& e)
        {
            return "Command execution error: " + std::string(e.what());
        }
    }

    std::vector<CommandRegistry::CommandInfo> CommandRegistry::GetAllCommands() const
    {
        std::vector<CommandInfo> result;
        for (const auto& pair : m_commands)
            result.push_back(pair.second);
        return result;
    }

    std::vector<std::string> CommandRegistry::ParseArguments(const std::string& commandLine)
    {
        std::vector<std::string> args;
        std::istringstream iss(commandLine);
        std::string arg;
        while (iss >> arg)
            args.push_back(arg);
        return args;
    }

} // namespace Spark
