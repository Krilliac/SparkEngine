/**
 * @file ConsoleProcessManagerIO.cpp
 * @brief SparkConsole logging, pipe I/O, and background dispatch
 */

#include "ConsoleProcessManager.h"
#include "Core/Platform.h"
#include "LogMacros.h"
#include "Validate.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
#include <climits>
#include <unistd.h>
#endif

namespace Spark
{

    // =========================================================================
    // Helper: wstring → string (ASCII-only, matches previous behavior)
    // =========================================================================

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

    // =========================================================================
    // Initialize
    // =========================================================================

    bool ConsoleProcessManager::Initialize(const std::wstring& consolePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (m_initialized)
            return true;

#if defined(SPARK_BUILD_SHIPPING) && !defined(SPARK_CONSOLE_IN_SHIPPING)
        // ENABLE_CONSOLE_IN_SHIPPING=OFF: Shipping builds never spawn the external
        // console; logging stays on the Logger sinks.
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "SparkConsole disabled in this Shipping build");
        return true;
#else
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConsoleProcessManager::Initialize starting");

        std::string consoleBaseName = WStrToStr(consolePath);
        if (consoleBaseName.empty())
            consoleBaseName = "SparkConsole";

        // Determine executable directory. An empty result means "unknown", never
        // the working directory — see ResolveConsoleExecutable.
        std::string executableDir;
#ifdef SPARK_PLATFORM_WINDOWS
        wchar_t currentDir[MAX_PATH];
        const DWORD moduleNameLength = GetModuleFileNameW(NULL, currentDir, MAX_PATH);
        if (moduleNameLength > 0 && moduleNameLength < MAX_PATH)
            executableDir = std::filesystem::path(currentDir).parent_path().string();
#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
        char exePath[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len != -1)
        {
            exePath[len] = '\0';
            executableDir = std::filesystem::path(exePath).parent_path().string();
        }
#endif

#ifdef SPARK_PLATFORM_WINDOWS
        std::string ext = ".exe";
#else
        std::string ext;
#endif

        const std::string actualPath = ResolveConsoleExecutable(executableDir, consoleBaseName + ext);

        if (actualPath.empty())
        {
            m_initialized = true;
            std::cerr << "[ConsoleProcessManager] SparkConsole not found. Using fallback logging.\n";
            return true;
        }

        bool success = LaunchConsoleProcess(actualPath);
        m_initialized = true;

        if (success)
        {
            m_shouldStopThread = false;
            m_threadStarted.store(false, std::memory_order_relaxed);
            m_consoleThread = std::thread(&ConsoleProcessManager::ConsoleThreadMain, this);

            while (!m_threadStarted.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        return success;
#endif
    }

    // =========================================================================
    // Logging
    // =========================================================================

    void ConsoleProcessManager::Log(const std::wstring& message, const std::wstring& type)
    {
        std::string formatted = "[" + WStrToStr(type) + "] " + WStrToStr(message);

#ifdef SPARK_PLATFORM_WINDOWS
        OutputDebugStringA(formatted.c_str());
        OutputDebugStringA("\n");
#else
        std::cerr << formatted << "\n";
#endif

        if (m_consoleRunning)
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            EnqueueForConsole(std::move(formatted));
        }
    }

    void ConsoleProcessManager::QueueEngineLog(const std::string& message, const std::string& type)
    {
        if (!m_consoleRunning)
            return;

        std::lock_guard<std::mutex> lock(m_messageMutex);
        EnqueueForConsole("[" + type + "] " + message);
    }

    void ConsoleProcessManager::EnqueueForConsole(std::string line)
    {
        // Caller holds m_messageMutex. m_process is read here, under the same
        // mutex Shutdown() takes to reset it.
        if (!m_process)
            return;

        if (m_messageQueue.size() >= kMaxQueuedMessages)
        {
            // Drop-oldest: the newest lines describe why the child stopped
            // draining. ProcessQueuedMessages() reports the count once the pipe
            // accepts data again.
            m_messageQueue.pop();
            ++m_droppedMessages;
        }
        m_messageQueue.push(std::move(line));
    }

    void ConsoleProcessManager::LogCrash(const std::string& crashInfo)
    {
        std::wstring w(crashInfo.begin(), crashInfo.end());
        Log(w, L"CRASH");
    }

    // =========================================================================
    // Process launch and I/O (delegated to Spark::Process)
    // =========================================================================

    bool ConsoleProcessManager::LaunchConsoleProcess(const std::string& path)
    {
        auto result = Process::Builder(path).Arg("--engine-pipe").CaptureStdin().CaptureStdout().Launch();

        if (!result)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to launch SparkConsole: %s", result.error().c_str());
            return false;
        }

        m_process.emplace(std::move(*result));
        m_consoleRunning = true;

        // Give the child a moment to start up
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return true;
    }

    bool ConsoleProcessManager::ReadFromConsole()
    {
        if (!m_process)
            return false;

        std::string line;
        if (!m_process->TryReadLine(line))
            return false;

        if (!line.empty())
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandQueue.push(line);
            return true;
        }
        return false;
    }

    bool ConsoleProcessManager::WriteToConsole(const std::string& message)
    {
        if (!m_process)
            return false;

        std::string data = message + "\n";
        m_process->WriteStdin(data);
        return true;
    }

    // =========================================================================
    // Background thread
    // =========================================================================

    void ConsoleProcessManager::ConsoleThreadMain()
    {
        m_threadStarted.store(true, std::memory_order_release);

        while (!m_shouldStopThread && m_consoleRunning)
        {
            if (ReadFromConsole())
                continue;
            ProcessQueuedMessages();

            // Check if child is still alive
            if (m_process && !m_process->IsRunning())
            {
                m_consoleRunning = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // =========================================================================
    // Shared methods (cross-platform)
    // =========================================================================

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
        std::queue<std::string> messagesToSend;
        uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            if (m_messageQueue.empty() && m_droppedMessages == 0)
                return;
            messagesToSend.swap(m_messageQueue);
            dropped = std::exchange(m_droppedMessages, static_cast<uint64_t>(0));
        }
        if (dropped > 0)
        {
            // One notice per burst, not one per dropped line — the same shape
            // SimpleConsole uses for its duplicate-suppression notice.
            WriteToConsole("[WARN] SparkConsole mirror dropped " + std::to_string(dropped) +
                           " log line(s): outgoing queue full (" + std::to_string(kMaxQueuedMessages) + ")");
        }
        while (!messagesToSend.empty())
        {
            WriteToConsole(messagesToSend.front());
            messagesToSend.pop();
        }
    }

} // namespace Spark
