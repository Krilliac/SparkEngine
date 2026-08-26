/**
 * @file EditorProcessLaunchWin32.cpp
 * @brief Windows editor child-process backend.
 */

#include "EditorProcessLaunch.h"
#include "EditorProcessLaunchText.h"

#ifdef _WIN32
#include <windows.h>

#include <string_view>
#include <vector>

namespace SparkEditor
{
    namespace
    {
        BOOL CALLBACK RequestGracefulWindowClose(HWND window, LPARAM parameter)
        {
            DWORD windowPid = 0;
            GetWindowThreadProcessId(window, &windowPid);
            if (windowPid == static_cast<DWORD>(parameter))
                PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        }

        ProcessLaunchResult LaunchEditorProcessImpl(const std::filesystem::path& exePath,
                                                    const std::wstring& commandLine,
                                                    const std::filesystem::path& workingDir, bool ownProcessTree)
        {
            ProcessLaunchResult result;
            std::wstring effectiveCommandLine = commandLine;
            if (effectiveCommandLine.find(L" -manifest ") == std::wstring::npos)
            {
                const std::filesystem::path manifest = workingDir / "spark.modules.json";
                std::error_code ec;
                if (std::filesystem::is_regular_file(manifest, ec) && !ec)
                    effectiveCommandLine += L" -manifest " + Detail::QuoteWindowsArgument(manifest.wstring());
            }

            // CreateProcessW may modify the command-line buffer — pass a writable copy.
            std::vector<wchar_t> cmdBuf(effectiveCommandLine.begin(), effectiveCommandLine.end());
            cmdBuf.push_back(L'\0');

            HANDLE job = nullptr;
            if (ownProcessTree)
            {
                job = CreateJobObjectW(nullptr, nullptr);
                if (!job)
                {
                    result.error = "Launch failed: could not create process Job Object (Win32 error " +
                                   std::to_string(GetLastError()) + ")";
                    return result;
                }

                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                {
                    result.error = "Launch failed: could not configure process Job Object (Win32 error " +
                                   std::to_string(GetLastError()) + ")";
                    CloseHandle(job);
                    return result;
                }
            }

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};

            const std::wstring exeW = exePath.wstring();
            const std::wstring workingDirW = workingDir.wstring();
            // Owned launches start suspended so they cannot create descendants
            // before assignment to the kill-on-close Job Object.
            const DWORD creationFlags = CREATE_NO_WINDOW | (ownProcessTree ? CREATE_SUSPENDED : 0);
            const BOOL ok = CreateProcessW(exeW.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, creationFlags, nullptr,
                                           workingDirW.c_str(), &startup, &process);
            if (!ok)
            {
                result.error = "Launch failed (Win32 error " + std::to_string(GetLastError()) + ")";
                if (job)
                    CloseHandle(job);
                return result;
            }

            if (ownProcessTree && !AssignProcessToJobObject(job, process.hProcess))
            {
                const DWORD error = GetLastError();
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                CloseHandle(job);
                result.error =
                    "Launch failed: could not assign process to Job Object (Win32 error " + std::to_string(error) + ")";
                return result;
            }

            if (ownProcessTree && ResumeThread(process.hThread) == static_cast<DWORD>(-1))
            {
                const DWORD error = GetLastError();
                TerminateJobObject(job, 1);
                WaitForSingleObject(process.hProcess, 1000);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                CloseHandle(job);
                result.error =
                    "Launch failed: could not resume owned process (Win32 error " + std::to_string(error) + ")";
                return result;
            }

            CloseHandle(process.hThread);
            result.success = true;
            result.processHandle = process.hProcess;
            result.jobHandle = job;
            result.pid = process.dwProcessId;
            return result;
        }
    } // namespace

    ProcessLaunchResult LaunchEditorProcess(const std::filesystem::path& exePath, const std::wstring& commandLine,
                                            const std::filesystem::path& workingDir)
    {
        return LaunchEditorProcessImpl(exePath, commandLine, workingDir, false);
    }

    ProcessLaunchResult LaunchOwnedEditorProcess(const std::filesystem::path& exePath, const std::wstring& commandLine,
                                                 const std::filesystem::path& workingDir)
    {
        return LaunchEditorProcessImpl(exePath, commandLine, workingDir, true);
    }

    bool PollProcessExited(void* processHandle, unsigned long& outExitCode)
    {
        if (!processHandle)
            return false;
        DWORD exitCode = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode) && exitCode != STILL_ACTIVE)
        {
            outExitCode = exitCode;
            return true;
        }
        return false;
    }

    void TerminateEditorProcess(void* processHandle, unsigned int exitCode)
    {
        if (!processHandle)
            return;
        // Best-effort: the process may have already exited on its own.
        TerminateProcess(static_cast<HANDLE>(processHandle), exitCode);
    }

    void CloseEditorProcessHandles(void* processHandle, void* jobHandle)
    {
        if (processHandle)
            CloseHandle(static_cast<HANDLE>(processHandle));
        if (jobHandle)
            CloseHandle(static_cast<HANDLE>(jobHandle));
    }

    EditorProcessStopResult StopEditorProcessTree(void* processHandle, void* jobHandle, unsigned long pid,
                                                  unsigned long gracePeriodMs, unsigned int exitCode)
    {
        if (!processHandle)
        {
            CloseEditorProcessHandles(nullptr, jobHandle);
            return EditorProcessStopResult::NotRunning;
        }

        HANDLE process = static_cast<HANDLE>(processHandle);
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
        {
            CloseEditorProcessHandles(processHandle, jobHandle);
            return EditorProcessStopResult::Graceful;
        }

        // Windowed games get a normal close request first. Headless children have
        // no top-level window, so they simply consume the same bounded grace time.
        if (pid != 0)
            EnumWindows(RequestGracefulWindowClose, static_cast<LPARAM>(pid));

        if (WaitForSingleObject(process, gracePeriodMs) == WAIT_OBJECT_0)
        {
            CloseEditorProcessHandles(processHandle, jobHandle);
            return EditorProcessStopResult::Graceful;
        }

        BOOL terminated = FALSE;
        if (jobHandle)
            terminated = TerminateJobObject(static_cast<HANDLE>(jobHandle), exitCode);
        else
            terminated = TerminateProcess(process, exitCode);

        if (terminated)
            (void)WaitForSingleObject(process, 1000);
        CloseEditorProcessHandles(processHandle, jobHandle);
        return terminated ? EditorProcessStopResult::Terminated : EditorProcessStopResult::Failed;
    }
} // namespace SparkEditor
#endif
