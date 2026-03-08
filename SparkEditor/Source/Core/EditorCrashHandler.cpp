/**
 * @file EditorCrashHandler.cpp
 * @brief Implementation of the editor crash handler
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorCrashHandler.h"
#include "EditorLogger.h"
#include <Windows.h>
#include <dbghelp.h>
#include <TlHelp32.h>
#include <VersionHelpers.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <thread>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <sstream>

#pragma comment(lib, "dbghelp.lib")

namespace SparkEditor
{

    // Static instance for singleton
    EditorCrashHandler* EditorCrashHandler::s_instance = nullptr;

    EditorCrashHandler& EditorCrashHandler::GetInstance()
    {
        if (!s_instance)
        {
            s_instance = new EditorCrashHandler();
        }
        return *s_instance;
    }

    EditorCrashHandler::~EditorCrashHandler()
    {
        // Ensure safe shutdown
        m_shouldStopAutoSave = true;

        if (m_autoSaveThread.joinable())
        {
            try
            {
                m_autoSaveThread.join();
            }
            catch (...)
            {
                // Detach if join fails to prevent blocking
                m_autoSaveThread.detach();
            }
        }
    }

    bool EditorCrashHandler::Initialize(const std::string& crashDirectory, EditorLogger* logger)
    {
        std::cout << "EditorCrashHandler initializing...\n";

        m_crashDirectory = crashDirectory;
        m_logger = logger;
        m_initialized = true;
        m_sessionStartTime = std::chrono::steady_clock::now();

        // Don't start auto-save thread for now to avoid deadlock issues
        // if (m_autoSaveEnabled) {
        //     m_shouldStopAutoSave = false;
        //     m_autoSaveThread = std::thread(&EditorCrashHandler::AutoSaveRecoveryThread, this);
        // }

        std::cout << "EditorCrashHandler initialized successfully\n";
        return true;
    }

    void EditorCrashHandler::Shutdown()
    {
        std::cout << "EditorCrashHandler shutting down...\n";

        // Signal auto-save thread to stop
        m_shouldStopAutoSave = true;

        // Wait for thread to finish with timeout to avoid deadlock
        if (m_autoSaveThread.joinable())
        {
            try
            {
                m_autoSaveThread.join();
            }
            catch (...)
            {
                // If join fails, just detach to avoid blocking shutdown
                m_autoSaveThread.detach();
            }
        }

        m_initialized = false;
        m_logger = nullptr;
        std::cout << "EditorCrashHandler shutdown complete\n";
    }

    void EditorCrashHandler::SetCrashCallback(CrashCallback callback)
    {
        m_crashCallback = callback;
    }

    void EditorCrashHandler::SetRecoveryCallback(RecoveryCallback callback)
    {
        m_recoveryCallback = callback;
    }

    void EditorCrashHandler::SetAssertCallback(AssertCallback callback)
    {
        m_assertCallback = callback;
    }

    void EditorCrashHandler::HandleAssertion(const std::string& expression, const char* file, int line,
                                             const std::string& message)
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.assertionFailures++;

        if (m_assertCallback)
        {
            m_assertCallback(expression, file ? file : "", line, message);
        }

        if (m_logger)
        {
            std::string logMsg = "Assertion failed: " + expression;
            if (!message.empty())
            {
                logMsg += " - " + message;
            }
            if (file)
            {
                logMsg += " at " + std::string(file) + ":" + std::to_string(line);
            }
            m_logger->Log(LogLevel::ERROR_, "Assert", logMsg);
        }
    }

    void EditorCrashHandler::RecordOperation(const std::string& operation)
    {
        std::lock_guard<std::mutex> lock(m_operationsMutex);

        m_recentOperations.push_back(operation);
        if (m_recentOperations.size() > m_maxOperations)
        {
            m_recentOperations.erase(m_recentOperations.begin());
        }
    }

    void EditorCrashHandler::SetEditorState(const std::string& state)
    {
        std::lock_guard<std::mutex> lock(m_operationsMutex);
        m_currentEditorState = state;
    }

    bool EditorCrashHandler::SaveRecoveryData()
    {
        if (!m_recoveryCallback)
        {
            return false;
        }

        try
        {
            RecoveryData data = m_recoveryCallback();

            // Save recovery data to file
            std::string recoveryFile = m_crashDirectory + "/recovery.json";
            std::ofstream file(recoveryFile);
            if (!file.is_open())
            {
                return false;
            }

            // Simple JSON-like format
            file << "{\n";
            file << "  \"currentLayout\": \"" << data.currentLayout << "\",\n";
            file << "  \"currentProject\": \"" << data.currentProject << "\",\n";
            file << "  \"lastSavedScene\": \"" << data.lastSavedScene << "\",\n";
            file << "  \"openFiles\": [\n";
            for (size_t i = 0; i < data.openFiles.size(); ++i)
            {
                file << "    \"" << data.openFiles[i] << "\"";
                if (i < data.openFiles.size() - 1)
                    file << ",";
                file << "\n";
            }
            file << "  ],\n";
            file << "  \"recentOperations\": [\n";
            for (size_t i = 0; i < data.recentOperations.size(); ++i)
            {
                file << "    \"" << data.recentOperations[i] << "\"";
                if (i < data.recentOperations.size() - 1)
                    file << ",";
                file << "\n";
            }
            file << "  ]\n";
            file << "}\n";

            file.close();

            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.recoveryDataSaves++;

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::optional<RecoveryData> EditorCrashHandler::LoadRecoveryData()
    {
        try
        {
            std::string recoveryFile = m_crashDirectory + "/recovery.json";
            std::ifstream file(recoveryFile);
            if (!file.is_open())
            {
                return std::nullopt;
            }

            RecoveryData data;
            std::string line;

            // Simple parser for our JSON-like format
            while (std::getline(file, line))
            {
                if (line.find("\"currentLayout\"") != std::string::npos)
                {
                    size_t start = line.find(": \"") + 3;
                    size_t end = line.find_last_of("\"");
                    if (start < end)
                    {
                        data.currentLayout = line.substr(start, end - start);
                    }
                }
                else if (line.find("\"currentProject\"") != std::string::npos)
                {
                    size_t start = line.find(": \"") + 3;
                    size_t end = line.find_last_of("\"");
                    if (start < end)
                    {
                        data.currentProject = line.substr(start, end - start);
                    }
                }
                else if (line.find("\"lastSavedScene\"") != std::string::npos)
                {
                    size_t start = line.find(": \"") + 3;
                    size_t end = line.find_last_of("\"");
                    if (start < end)
                    {
                        data.lastSavedScene = line.substr(start, end - start);
                    }
                }
                // Parse JSON arrays for openFiles and recentOperations
                else if (line.find("\"openFiles\"") != std::string::npos)
                {
                    // Read array entries until the closing bracket
                    std::string arrayLine;
                    while (std::getline(file, arrayLine))
                    {
                        // Check for end of array
                        if (arrayLine.find(']') != std::string::npos)
                            break;
                        // Extract the quoted value from lines like:   "some/path"
                        size_t qStart = arrayLine.find('\"');
                        if (qStart == std::string::npos)
                            continue;
                        size_t qEnd = arrayLine.find('\"', qStart + 1);
                        if (qEnd == std::string::npos)
                            continue;
                        data.openFiles.push_back(arrayLine.substr(qStart + 1, qEnd - qStart - 1));
                    }
                }
                else if (line.find("\"recentOperations\"") != std::string::npos)
                {
                    std::string arrayLine;
                    while (std::getline(file, arrayLine))
                    {
                        if (arrayLine.find(']') != std::string::npos)
                            break;
                        size_t qStart = arrayLine.find('\"');
                        if (qStart == std::string::npos)
                            continue;
                        size_t qEnd = arrayLine.find('\"', qStart + 1);
                        if (qEnd == std::string::npos)
                            continue;
                        data.recentOperations.push_back(arrayLine.substr(qStart + 1, qEnd - qStart - 1));
                    }
                }
            }

            file.close();
            return data;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    bool EditorCrashHandler::HasRecoveryData()
    {
        try
        {
            std::string recoveryFile = m_crashDirectory + "/recovery.json";
            return std::filesystem::exists(recoveryFile);
        }
        catch (...)
        {
            return false;
        }
    }

    void EditorCrashHandler::ClearRecoveryData()
    {
        try
        {
            std::string recoveryFile = m_crashDirectory + "/recovery.json";
            if (std::filesystem::exists(recoveryFile))
            {
                std::filesystem::remove(recoveryFile);
            }
        }
        catch (...)
        {
            // Ignore errors when clearing recovery data
        }
    }

    EditorCrashHandler::CrashStats EditorCrashHandler::GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    std::string EditorCrashHandler::GenerateCrashReport(const CrashInfo& crashInfo)
    {
        std::string report = "=== Spark Editor Crash Report ===\n";
        report += "Exception Type: " + crashInfo.exceptionType + "\n";
        report += "Exception Message: " + crashInfo.exceptionMessage + "\n";
        report += "Editor State: " + m_currentEditorState + "\n";

        report += "\nRecent Operations:\n";
        std::lock_guard<std::mutex> lock(m_operationsMutex);
        for (const auto& op : m_recentOperations)
        {
            report += "  - " + op + "\n";
        }

        return report;
    }

    void EditorCrashHandler::TestCrashHandler()
    {
        std::cout << "Testing crash handler (this should not crash in development)\n";
        // In a real crash handler, this would trigger a controlled crash for testing
    }

    void EditorCrashHandler::TestAssertionHandler()
    {
        HandleAssertion("test_expression", __FILE__, __LINE__, "Test assertion for crash handler verification");
    }

    void EditorCrashHandler::AutoSaveRecoveryThread()
    {
        while (!m_shouldStopAutoSave)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(m_autoSaveInterval * 1000)));

            if (!m_shouldStopAutoSave)
            {
                SaveRecoveryData();
            }
        }
    }

    LONG WINAPI EditorCrashHandler::ExceptionFilter(EXCEPTION_POINTERS* exceptionPointers)
    {
        if (s_instance)
        {
            s_instance->HandleCrashInternal(exceptionPointers);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void EditorCrashHandler::HandleCrashInternal(EXCEPTION_POINTERS* exceptionPointers)
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.totalCrashes++;

        CrashInfo info;
        info.exceptionPointers = exceptionPointers;
        info.timestamp = std::chrono::system_clock::now();
        info.processId = GetCurrentProcessId();
        info.threadId = GetCurrentThreadId();
        info.editorState = m_currentEditorState;

        // Decode exception type
        if (exceptionPointers && exceptionPointers->ExceptionRecord)
        {
            DWORD code = exceptionPointers->ExceptionRecord->ExceptionCode;
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION:
                info.exceptionType = "ACCESS_VIOLATION";
                break;
            case EXCEPTION_STACK_OVERFLOW:
                info.exceptionType = "STACK_OVERFLOW";
                break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                info.exceptionType = "INT_DIVIDE_BY_ZERO";
                break;
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:
                info.exceptionType = "FLT_DIVIDE_BY_ZERO";
                break;
            case EXCEPTION_ILLEGAL_INSTRUCTION:
                info.exceptionType = "ILLEGAL_INSTRUCTION";
                break;
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
                info.exceptionType = "ARRAY_BOUNDS_EXCEEDED";
                break;
            case STATUS_FATAL_APP_EXIT:
                info.exceptionType = "FATAL_APP_EXIT (Assert)";
                break;
            default:
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "EXCEPTION_0x%08lX", static_cast<unsigned long>(code));
                info.exceptionType = buf;
            }
            break;
            }

            // Categorize for statistics
            if (code == EXCEPTION_ACCESS_VIOLATION)
                m_stats.accessViolations++;
            else if (code == EXCEPTION_STACK_OVERFLOW)
                m_stats.stackOverflows++;
            else
                m_stats.otherExceptions++;
        }

        // Gather additional context
        info.stackTrace = GenerateStackTrace(exceptionPointers);
        info.systemInfo = GetSystemInfo();
        info.threadInfo = GetThreadInfo();

        // Gather recent operations
        {
            std::lock_guard<std::mutex> opsLock(m_operationsMutex);
            std::string opsStr;
            for (const auto& op : m_recentOperations)
            {
                opsStr += "  - " + op + "\n";
            }
            info.lastOperations = opsStr;
        }

        m_stats.lastCrash = info.timestamp;
        m_stats.lastCrashType = info.exceptionType;

        // Save crash dump and log
        if (!m_crashDirectory.empty())
        {
            try
            {
                std::filesystem::create_directories(m_crashDirectory);

                auto time_t = std::chrono::system_clock::to_time_t(info.timestamp);
                char timeBuf[64];
                strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&time_t));

                std::string dumpPath = m_crashDirectory + "/editor_crash_" + timeBuf + ".dmp";
                std::string logPath = m_crashDirectory + "/editor_crash_" + timeBuf + ".log";

                SaveCrashDump(exceptionPointers, dumpPath);
                SaveCrashLog(info, logPath);
            }
            catch (...)
            {
                // Don't let file operations crash the crash handler
            }
        }

        // Save recovery data
        try
        {
            SaveRecoveryData();
        }
        catch (...)
        {
        }

        if (m_crashCallback)
        {
            m_crashCallback(info);
        }
    }

    std::string EditorCrashHandler::GenerateStackTrace(EXCEPTION_POINTERS* exceptionPointers)
    {
        if (!exceptionPointers || !exceptionPointers->ContextRecord)
        {
            return "No exception context available for stack trace\n";
        }

        std::string result = "=== Stack Trace ===\n";

        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
        {
            return result + "Failed to initialize symbol handler\n";
        }

        CONTEXT& ctx = *exceptionPointers->ContextRecord;
        STACKFRAME64 frame = {};
#ifdef _WIN64
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrStack.Offset = ctx.Rsp;
#else
        DWORD machine = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset = ctx.Eip;
        frame.AddrFrame.Offset = ctx.Ebp;
        frame.AddrStack.Offset = ctx.Esp;
#endif
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

        BYTE symBuffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;

        for (int i = 0; i < 64; ++i)
        {
            if (!StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            {
                break;
            }
            if (frame.AddrPC.Offset == 0)
                break;

            DWORD64 displacement64 = 0;
            char line[512];

            if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement64, sym))
            {
                // Try to get source file and line
                IMAGEHLP_LINE64 lineInfo = {};
                lineInfo.SizeOfStruct = sizeof(lineInfo);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(GetCurrentProcess(), frame.AddrPC.Offset, &lineDisplacement, &lineInfo))
                {
                    snprintf(line, sizeof(line), "  [%2d] %s +0x%llX (%s:%lu)\n", i, sym->Name,
                             (unsigned long long)displacement64, lineInfo.FileName, lineInfo.LineNumber);
                }
                else
                {
                    snprintf(line, sizeof(line), "  [%2d] %s +0x%llX\n", i, sym->Name,
                             (unsigned long long)displacement64);
                }
            }
            else
            {
                snprintf(line, sizeof(line), "  [%2d] 0x%016llX\n", i, (unsigned long long)frame.AddrPC.Offset);
            }
            result += line;
        }

        SymCleanup(GetCurrentProcess());
        return result;
    }

    std::string EditorCrashHandler::GetSystemInfo()
    {
        std::string result = "=== System Info ===\n";

        // OS version
        if (IsWindows10OrGreater())
            result += "OS: Windows 10+\n";
        else if (IsWindows8OrGreater())
            result += "OS: Windows 8+\n";
        else
            result += "OS: Windows (version unknown)\n";

        // CPU info
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        result += "CPU Cores: " + std::to_string(si.dwNumberOfProcessors) + "\n";

        // Memory info
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
        {
            result += "RAM Total: " + std::to_string(ms.ullTotalPhys >> 20) + " MiB\n";
            result += "RAM Available: " + std::to_string(ms.ullAvailPhys >> 20) + " MiB\n";
            result += "Memory Load: " + std::to_string(ms.dwMemoryLoad) + "%\n";
        }

        return result;
    }

    std::string EditorCrashHandler::GetThreadInfo()
    {
        std::string result = "=== Thread Info ===\n";
        result += "Current Thread ID: 0x" + std::to_string(GetCurrentThreadId()) + "\n";

        // Count threads in this process
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            DWORD pid = GetCurrentProcessId();
            THREADENTRY32 te = {};
            te.dwSize = sizeof(te);
            int threadCount = 0;
            if (Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID == pid)
                        threadCount++;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            result += "Total Threads: " + std::to_string(threadCount) + "\n";
        }

        return result;
    }

    bool EditorCrashHandler::SaveCrashDump(EXCEPTION_POINTERS* exceptionPointers, const std::string& filePath)
    {
        if (!exceptionPointers)
            return false;

        // Convert to wide string for Windows API
        std::wstring wpath(filePath.begin(), filePath.end());

        HANDLE hFile =
            CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            if (m_logger)
            {
                m_logger->Log(LogLevel::ERROR_, "CrashHandler", "Failed to create dump file: " + filePath);
            }
            return false;
        }

        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exceptionPointers;
        mei.ClientPointers = TRUE;

        BOOL success = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                                         static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData),
                                         &mei, nullptr, nullptr);

        CloseHandle(hFile);

        if (m_logger)
        {
            if (success)
            {
                m_logger->Log(LogLevel::INFO, "CrashHandler", "Crash dump saved: " + filePath);
            }
            else
            {
                m_logger->Log(LogLevel::ERROR_, "CrashHandler", "Failed to write crash dump: " + filePath);
            }
        }
        return success != FALSE;
    }

    bool EditorCrashHandler::SaveCrashLog(const CrashInfo& crashInfo, const std::string& filePath)
    {
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            if (m_logger)
            {
                m_logger->Log(LogLevel::ERROR_, "CrashHandler", "Failed to create crash log: " + filePath);
            }
            return false;
        }

        std::string report = GenerateCrashReport(crashInfo);

        file << "================================================================\n";
        file << "         SPARK EDITOR CRASH LOG\n";
        file << "================================================================\n\n";

        auto time_t = std::chrono::system_clock::to_time_t(crashInfo.timestamp);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
        file << "Timestamp  : " << timeBuf << "\n";
        file << "Process ID : " << crashInfo.processId << "\n";
        file << "Thread ID  : " << crashInfo.threadId << "\n\n";

        file << "Exception  : " << crashInfo.exceptionType << "\n";
        file << "Message    : " << crashInfo.exceptionMessage << "\n";
        file << "Editor State: " << crashInfo.editorState << "\n\n";

        file << crashInfo.stackTrace << "\n";
        file << crashInfo.systemInfo << "\n";
        file << crashInfo.threadInfo << "\n";

        if (!crashInfo.lastOperations.empty())
        {
            file << "=== Recent Operations ===\n";
            file << crashInfo.lastOperations << "\n";
        }

        file.close();

        if (m_logger)
        {
            m_logger->Log(LogLevel::INFO, "CrashHandler", "Crash log saved: " + filePath);
        }
        return true;
    }

    void EditorCrashHandler::UpdateStats(const CrashInfo& crashInfo)
    {
        // Calculate average session time
        auto now = std::chrono::steady_clock::now();
        float sessionSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - m_sessionStartTime).count();
        m_stats.averageSessionTime = sessionSeconds;
    }

} // namespace SparkEditor