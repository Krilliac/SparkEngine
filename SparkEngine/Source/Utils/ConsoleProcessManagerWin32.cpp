/**
 * @file ConsoleProcessManagerWin32.cpp
 * @brief Windows implementation of ConsoleProcessManager
 *
 * Uses Win32 CreateProcess, named pipes, and ReadFile/WriteFile for
 * communication with the SparkConsole subprocess.
 */

#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "ConsoleProcessManager.h"
#include "Utils/Assert.h"
#include "Utils/CrashHandler.h"
#include "Validate.h"
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>

#include <Windows.h>

namespace Spark
{

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
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "CreatePipe failed for stdin (error %lu)", GetLastError());
            return false;
        }
        if (!CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &sa, 0))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "CreatePipe failed for stdout (error %lu)", GetLastError());
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
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "CreateProcessW failed for SparkConsole (error %lu)",
                            GetLastError());
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
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "PeekNamedPipe failed (error %lu) — marking console as stopped",
                            GetLastError());
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
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "ReadFile failed on console pipe (error %lu)", GetLastError());
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
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "WideCharToMultiByte failed for console message (error %lu)",
                           GetLastError());
            return false;
        }

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
        m_threadStarted.store(true, std::memory_order_release);

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

} // namespace Spark

#endif // SPARK_PLATFORM_WINDOWS
