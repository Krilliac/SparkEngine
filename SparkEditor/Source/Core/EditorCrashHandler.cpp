/**
 * @file EditorCrashHandler.cpp
 * @brief Implementation of the editor crash handler
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorCrashHandler.h"
#include "EditorLogger.h"
#include "Utils/Validate.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <thread>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <versionhelpers.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/sysinfo.h>
#endif
#include <cstring>
#include <pthread.h>
#endif

namespace SparkEditor
{
#ifndef _WIN32
    namespace
    {
        struct PosixSignalRegistration
        {
            explicit PosixSignalRegistration(int signal) : signalNumber(signal) {}

            int signalNumber = 0;
            struct sigaction previousAction = {};
            bool installed = false;
        };

        PosixSignalRegistration g_fatalSignalRegistrations[] = {
            PosixSignalRegistration(SIGSEGV), PosixSignalRegistration(SIGABRT), PosixSignalRegistration(SIGFPE),
            PosixSignalRegistration(SIGILL),  PosixSignalRegistration(SIGBUS),
        };
        volatile sig_atomic_t g_emergencyCrashLogFd = -1;
        volatile sig_atomic_t g_handlingFatalSignal = 0;

        void WriteAllAsyncSignalSafe(int fd, const char* data, size_t size) noexcept
        {
            while (size > 0)
            {
                const ssize_t written = write(fd, data, size);
                if (written > 0)
                {
                    data += written;
                    size -= static_cast<size_t>(written);
                    continue;
                }
                if (written < 0 && errno == EINTR)
                {
                    continue;
                }
                break;
            }
        }

        size_t AppendUnsignedDecimal(char* output, size_t offset, size_t capacity, uint32_t value) noexcept
        {
            char reversed[32];
            size_t digitCount = 0;
            do
            {
                reversed[digitCount++] = static_cast<char>('0' + (value % 10));
                value /= 10;
            } while (value != 0 && digitCount < sizeof(reversed));

            while (digitCount > 0 && offset < capacity)
            {
                output[offset++] = reversed[--digitCount];
            }
            return offset;
        }

        void WriteEmergencySignalRecord(int fd, int signalNumber) noexcept
        {
            if (fd < 0)
            {
                return;
            }

            constexpr char prefix[] = "SparkEditor caught fatal signal ";
            constexpr char processPrefix[] = " in process ";
            constexpr char suffix[] =
                "; detailed reporting skipped because the process is in an unsafe signal context.\n";
            char message[192];
            size_t length = 0;

            for (size_t i = 0; i + 1 < sizeof(prefix) && length < sizeof(message); ++i)
            {
                message[length++] = prefix[i];
            }
            length = AppendUnsignedDecimal(message, length, sizeof(message), static_cast<uint32_t>(signalNumber));
            for (size_t i = 0; i + 1 < sizeof(processPrefix) && length < sizeof(message); ++i)
            {
                message[length++] = processPrefix[i];
            }
            length = AppendUnsignedDecimal(message, length, sizeof(message), static_cast<uint32_t>(getpid()));
            for (size_t i = 0; i + 1 < sizeof(suffix) && length < sizeof(message); ++i)
            {
                message[length++] = suffix[i];
            }

            WriteAllAsyncSignalSafe(fd, message, length);
        }

        void RestorePosixSignalHandlers() noexcept
        {
            for (auto& registration : g_fatalSignalRegistrations)
            {
                if (registration.installed)
                {
                    if (sigaction(registration.signalNumber, &registration.previousAction, nullptr) == 0)
                    {
                        registration.installed = false;
                    }
                }
            }
        }

        void CloseEmergencyCrashLog() noexcept
        {
            const int fd = static_cast<int>(g_emergencyCrashLogFd);
            g_emergencyCrashLogFd = -1;
            if (fd >= 0)
            {
                close(fd);
            }
        }

        uint64_t GetCurrentPosixThreadId()
        {
#if defined(__APPLE__)
            uint64_t threadId = 0;
            return pthread_threadid_np(nullptr, &threadId) == 0 ? threadId : 0;
#else
            return static_cast<uint64_t>(pthread_self());
#endif
        }
    } // namespace
#endif

    // Static instance for singleton (Meyer's — no leak, thread-safe since C++11)
    EditorCrashHandler* EditorCrashHandler::s_instance = nullptr;

    EditorCrashHandler& EditorCrashHandler::GetInstance()
    {
        static EditorCrashHandler instance;
        s_instance = &instance;
        return instance;
    }

    EditorCrashHandler::~EditorCrashHandler()
    {
#ifdef _WIN32
        // The POSIX branch already restored its handlers here; the Windows filter did not, so on any
        // path that never reaches Shutdown() (early exit, a failure between Initialize and Shutdown, a
        // tool that only calls Initialize) the filter survived into static destruction still pointing at
        // ExceptionFilter, which then dereferenced this destroyed object through s_instance.
        if (m_filterInstalled)
        {
            SetUnhandledExceptionFilter(m_previousFilter);
            m_previousFilter = nullptr;
            m_filterInstalled = false;
        }
#else
        RestorePosixSignalHandlers();
        CloseEmergencyCrashLog();
        g_handlingFatalSignal = 0;
#endif
        // The filter reads s_instance; this object is going away, so no later crash may reach it.
        if (s_instance == this)
        {
            s_instance = nullptr;
        }

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
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !crashDirectory.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorCrashHandler initializing");
        std::cout << "EditorCrashHandler initializing...\n";

        m_crashDirectory = crashDirectory;
        m_logger = logger;
        m_initialized = true;
        m_sessionStartTime = std::chrono::steady_clock::now();

#ifdef _WIN32
        // Without this the whole Windows crash path below (ExceptionFilter ->
        // HandleCrashInternal -> dump/log/recovery.json) is unreachable and the
        // editor dies through the default OS handler with nothing written.
        std::error_code directoryError;
        std::filesystem::create_directories(m_crashDirectory, directoryError);
        if (directoryError)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorCrashHandler: cannot create crash directory '%s': %s",
                            m_crashDirectory.c_str(), directoryError.message().c_str());
            m_initialized = false;
            return false;
        }
        m_previousFilter = SetUnhandledExceptionFilter(&EditorCrashHandler::ExceptionFilter);
        m_filterInstalled = true;
#else
        // Open the emergency record before installing handlers. A fatal-signal
        // callback may only use async-signal-safe operations, so it cannot
        // create directories, allocate C++ objects, lock mutexes, or run the
        // normal recovery callbacks after the fault has happened.
        RestorePosixSignalHandlers();
        CloseEmergencyCrashLog();
        std::error_code directoryError;
        std::filesystem::create_directories(m_crashDirectory, directoryError);
        if (!directoryError)
        {
            const std::string emergencyLogPath = m_crashDirectory + "/editor_signal_crash.log";
#ifdef O_CLOEXEC
            constexpr int closeOnExecFlag = O_CLOEXEC;
#else
            constexpr int closeOnExecFlag = 0;
#endif
            g_emergencyCrashLogFd =
                open(emergencyLogPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | closeOnExecFlag, S_IRUSR | S_IWUSR);
        }

        struct sigaction sa = {};
        sa.sa_handler = SignalHandler;
        sigemptyset(&sa.sa_mask);
        // Reset before entering the callback so a recursive fault terminates
        // immediately instead of recursively entering compromised C++ state.
        sa.sa_flags = SA_RESTART | SA_RESETHAND;
        bool installedAllHandlers = true;
        for (auto& registration : g_fatalSignalRegistrations)
        {
            if (sigaction(registration.signalNumber, &sa, &registration.previousAction) == 0)
            {
                registration.installed = true;
            }
            else
            {
                installedAllHandlers = false;
                break;
            }
        }
        if (!installedAllHandlers)
        {
            RestorePosixSignalHandlers();
            CloseEmergencyCrashLog();
            m_initialized = false;
            return false;
        }
#endif

        std::cout << "EditorCrashHandler initialized successfully\n";
        return true;
    }

    void EditorCrashHandler::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorCrashHandler shutting down");
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
#ifdef _WIN32
        if (m_filterInstalled)
        {
            SetUnhandledExceptionFilter(m_previousFilter);
            m_previousFilter = nullptr;
            m_filterInstalled = false;
        }
#else
        RestorePosixSignalHandlers();
        CloseEmergencyCrashLog();
#endif
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
        SPARK_WARN_IF(Spark::LogCategory::Editor, expression.empty(), "HandleAssertion called with empty expression");
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
        catch (const std::exception& e)
        {
            std::cerr << "SaveRecoveryData failed: " << e.what() << "\n";
            return false;
        }
        catch (...)
        {
            std::cerr << "SaveRecoveryData failed with unknown exception\n";
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
                if (line.contains("\"currentLayout\""))
                {
                    size_t start = line.find(": \"") + 3;
                    size_t end = line.find_last_of("\"");
                    if (start < end)
                    {
                        data.currentLayout = line.substr(start, end - start);
                    }
                }
                else if (line.contains("\"currentProject\""))
                {
                    size_t start = line.find(": \"") + 3;
                    size_t end = line.find_last_of("\"");
                    if (start < end)
                    {
                        data.currentProject = line.substr(start, end - start);
                    }
                }
                else if (line.contains("\"lastSavedScene\""))
                {
                    size_t start = line.find(": \"") + 3;
                    size_t end = line.find_last_of("\"");
                    if (start < end)
                    {
                        data.lastSavedScene = line.substr(start, end - start);
                    }
                }
                else if (line.contains("\"openFiles\""))
                {
                    std::string arrayLine;
                    while (std::getline(file, arrayLine))
                    {
                        if (arrayLine.contains(']'))
                            break;
                        size_t qStart = arrayLine.find('\"');
                        if (qStart == std::string::npos)
                            continue;
                        size_t qEnd = arrayLine.find('\"', qStart + 1);
                        if (qEnd == std::string::npos)
                            continue;
                        data.openFiles.push_back(arrayLine.substr(qStart + 1, qEnd - qStart - 1));
                    }
                }
                else if (line.contains("\"recentOperations\""))
                {
                    std::string arrayLine;
                    while (std::getline(file, arrayLine))
                    {
                        if (arrayLine.contains(']'))
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
        catch (const std::exception& e)
        {
            std::cerr << "LoadRecoveryData failed: " << e.what() << "\n";
            return std::nullopt;
        }
        catch (...)
        {
            std::cerr << "LoadRecoveryData failed with unknown exception\n";
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
        catch (const std::exception& e)
        {
            std::cerr << "ClearRecoveryData failed: " << e.what() << "\n";
        }
        catch (...)
        {
            std::cerr << "ClearRecoveryData failed with unknown exception\n";
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

    void EditorCrashHandler::SetAutoSaveRecovery(bool enabled, float interval)
    {
        m_autoSaveEnabled = enabled;
        m_autoSaveInterval = interval;
    }

    // =========================================================================
    // Windows-specific crash handling
    // =========================================================================
#ifdef _WIN32

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

            if (code == EXCEPTION_ACCESS_VIOLATION)
                m_stats.accessViolations++;
            else if (code == EXCEPTION_STACK_OVERFLOW)
                m_stats.stackOverflows++;
            else
                m_stats.otherExceptions++;
        }

        info.stackTrace = GenerateStackTrace(exceptionPointers);
        info.systemInfo = GetSystemInfo();
        info.threadInfo = GetThreadInfo();

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
            }
        }

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

        if (IsWindows10OrGreater())
            result += "OS: Windows 10+\n";
        else if (IsWindows8OrGreater())
            result += "OS: Windows 8+\n";
        else
            result += "OS: Windows (version unknown)\n";

        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        result += "CPU Cores: " + std::to_string(si.dwNumberOfProcessors) + "\n";

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

    // =========================================================================
    // POSIX-specific crash handling
    // =========================================================================
#else

    void EditorCrashHandler::SignalHandler(int signal)
    {
        const int savedErrno = errno;
        if (g_handlingFatalSignal != 0)
        {
            _exit(128 + signal);
        }
        g_handlingFatalSignal = 1;

        const int emergencyLogFd = static_cast<int>(g_emergencyCrashLogFd);
        // Prefer the pre-opened regular file. stderr may be a full pipe, so it
        // is only a fallback when initialization could not open the record.
        WriteEmergencySignalRecord(emergencyLogFd >= 0 ? emergencyLogFd : STDERR_FILENO, signal);

        // SA_RESETHAND has already restored the default disposition. Queue the
        // same signal again; it is delivered with the default action as soon as
        // this handler returns, retaining normal core-dump semantics.
        errno = savedErrno;
        if (kill(getpid(), signal) != 0)
        {
            _exit(128 + signal);
        }
    }

    std::string EditorCrashHandler::GetSystemInfo()
    {
        std::string result = "=== System Info ===\n";

#if defined(__APPLE__)
        struct utsname osInfo = {};
        if (uname(&osInfo) == 0)
        {
            result += "OS: macOS " + std::string(osInfo.release) + " (" + osInfo.machine + ")\n";
        }
        else
        {
            result += "OS: macOS\n";
        }
#else
        // Linux distribution info
        std::ifstream osRelease("/etc/os-release");
        if (osRelease.is_open())
        {
            std::string line;
            while (std::getline(osRelease, line))
            {
                if (line.find("PRETTY_NAME=") == 0)
                {
                    std::string name = line.substr(13);
                    if (!name.empty() && name.front() == '"')
                        name = name.substr(1);
                    if (!name.empty() && name.back() == '"')
                        name.pop_back();
                    result += "OS: " + name + "\n";
                    break;
                }
            }
            osRelease.close();
        }
        else
        {
            result += "OS: Linux\n";
        }
#endif

        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (cores > 0)
        {
            result += "CPU Cores: " + std::to_string(cores) + "\n";
        }

#if defined(__APPLE__)
        uint64_t totalMemoryBytes = 0;
        size_t totalMemorySize = sizeof(totalMemoryBytes);
        if (sysctlbyname("hw.memsize", &totalMemoryBytes, &totalMemorySize, nullptr, 0) == 0)
        {
            result += "RAM Total: " + std::to_string(totalMemoryBytes / (1024ULL * 1024ULL)) + " MiB\n";
        }
#else
        struct sysinfo si = {};
        if (sysinfo(&si) == 0)
        {
            const uint64_t totalBytes = static_cast<uint64_t>(si.totalram) * si.mem_unit;
            const uint64_t availableBytes = static_cast<uint64_t>(si.freeram) * si.mem_unit;
            result += "RAM Total: " + std::to_string(totalBytes / (1024ULL * 1024ULL)) + " MiB\n";
            result += "RAM Available: " + std::to_string(availableBytes / (1024ULL * 1024ULL)) + " MiB\n";
            if (totalBytes > 0)
            {
                const int loadPercent = static_cast<int>(((totalBytes - availableBytes) * 100ULL) / totalBytes);
                result += "Memory Load: " + std::to_string(loadPercent) + "%\n";
            }
        }
#endif

        return result;
    }

    std::string EditorCrashHandler::GetThreadInfo()
    {
        std::string result = "=== Thread Info ===\n";
        result += "Current Thread ID: " + std::to_string(GetCurrentPosixThreadId()) + "\n";
        result += "Process ID: " + std::to_string(getpid()) + "\n";
        return result;
    }

#endif // _WIN32

    // =========================================================================
    // Shared implementation (cross-platform)
    // =========================================================================

    bool EditorCrashHandler::SaveCrashLog(const CrashInfo& crashInfo, const std::string& filePath)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !filePath.empty(), false);
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
        auto now = std::chrono::steady_clock::now();
        float sessionSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - m_sessionStartTime).count();
        m_stats.averageSessionTime = sessionSeconds;
    }

} // namespace SparkEditor
