/**
 * @file ConsoleProcessManagerLinux.cpp
 * @brief Linux/macOS implementation of ConsoleProcessManager
 *
 * Uses fork/exec, POSIX pipes, and poll() for communication with
 * the SparkConsole subprocess. macOS shares the same POSIX APIs.
 */

#include "Core/Platform.h"

#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)

#include "ConsoleProcessManager.h"
#include "Utils/Assert.h"
#include "Utils/CrashHandler.h"
#include "Utils/LogMacros.h"
#include "Validate.h"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

namespace Spark
{

    // WStrToStr is defined in ConsoleProcessManager.cpp (shared)
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
            // SparkConsole binary not found — use stderr as fallback since SimpleConsole may not be initialized yet
            std::cerr << "[ConsoleProcessManager] SparkConsole not found. Using fallback logging.\n";
            return true;
        }

        bool success = LaunchConsoleProcess(std::wstring(actualPath.begin(), actualPath.end()));
        m_initialized = true;

        if (success)
        {
            m_shouldStopThread = false;
            m_threadStarted.store(false, std::memory_order_relaxed);
            m_consoleThread = std::thread(&ConsoleProcessManager::ConsoleThreadMain, this);

            // Wait for console thread to start
            while (!m_threadStarted.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
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
        if (flags == -1)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "fcntl F_GETFL failed: %s", strerror(errno));
            return false;
        }
        if (fcntl(m_pipeFromChild[0], F_SETFL, flags | O_NONBLOCK) == -1)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "fcntl F_SETFL failed: %s", strerror(errno));
            return false;
        }

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
        m_threadStarted.store(true, std::memory_order_release);

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

} // namespace Spark

#endif // SPARK_PLATFORM_LINUX || SPARK_PLATFORM_MACOS
