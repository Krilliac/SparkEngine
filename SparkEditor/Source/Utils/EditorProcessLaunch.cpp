/**
 * @file EditorProcessLaunch.cpp
 * @brief Shared CreateProcessW helper implementation.
 */

#include "EditorProcessLaunch.h"
#include "EditorLaunchContext.h"

#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SparkEditor
{

#ifdef _WIN32
    namespace
    {
        std::filesystem::path GetCurrentExecutablePath()
        {
            constexpr size_t kInitialPathCapacity = 512;
            constexpr size_t kMaximumPathCapacity = 32768;

            for (size_t capacity = kInitialPathCapacity; capacity <= kMaximumPathCapacity; capacity *= 2)
            {
                std::vector<wchar_t> buffer(capacity);
                const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0)
                    return {};
                if (length < buffer.size())
                    return std::filesystem::path(std::wstring_view(buffer.data(), length));
            }
            return {};
        }

        /// @brief Quote one argument using the CommandLineToArgvW backslash rules.
        std::wstring QuoteWindowsArgument(std::wstring_view argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
                return std::wstring(argument);

            std::wstring quoted;
            quoted.reserve(argument.size() + 2);
            quoted.push_back(L'"');
            size_t backslashes = 0;
            for (const wchar_t ch : argument)
            {
                if (ch == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (ch == L'"')
                {
                    quoted.append(backslashes * 2 + 1, L'\\');
                    quoted.push_back(L'"');
                }
                else
                {
                    quoted.append(backslashes, L'\\');
                    quoted.push_back(ch);
                }
                backslashes = 0;
            }
            quoted.append(backslashes * 2, L'\\');
            quoted.push_back(L'"');
            return quoted;
        }

        BOOL CALLBACK RequestGracefulWindowClose(HWND window, LPARAM parameter)
        {
            DWORD windowPid = 0;
            GetWindowThreadProcessId(window, &windowPid);
            if (windowPid == static_cast<DWORD>(parameter))
                PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        }
    } // namespace
#endif

    namespace
    {
        ProcessLaunchResult LaunchEditorProcessImpl(const std::filesystem::path& exePath,
                                                    const std::wstring& commandLine,
                                                    const std::filesystem::path& workingDir, bool ownProcessTree)
        {
            ProcessLaunchResult result;
#ifdef _WIN32
            std::wstring effectiveCommandLine = commandLine;
            if (effectiveCommandLine.find(L" -manifest ") == std::wstring::npos)
            {
                const std::filesystem::path manifest = workingDir / "spark.modules.json";
                std::error_code ec;
                if (std::filesystem::is_regular_file(manifest, ec) && !ec)
                    effectiveCommandLine += L" -manifest " + QuoteWindowsArgument(manifest.wstring());
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
#else
            (void)exePath;
            (void)commandLine;
            (void)workingDir;
            (void)ownProcessTree;
            result.error = "Process launch is available on Windows builds only.";
#endif
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
#ifdef _WIN32
        if (!processHandle)
            return false;
        DWORD exitCode = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode) && exitCode != STILL_ACTIVE)
        {
            outExitCode = exitCode;
            return true;
        }
        return false;
#else
        (void)processHandle;
        (void)outExitCode;
        return false;
#endif
    }

    void TerminateEditorProcess(void* processHandle, unsigned int exitCode)
    {
#ifdef _WIN32
        if (!processHandle)
            return;
        // Best-effort: the process may have already exited on its own.
        TerminateProcess(static_cast<HANDLE>(processHandle), exitCode);
#else
        (void)processHandle;
        (void)exitCode;
#endif
    }

    void CloseEditorProcessHandles(void* processHandle, void* jobHandle)
    {
#ifdef _WIN32
        if (processHandle)
            CloseHandle(static_cast<HANDLE>(processHandle));
        if (jobHandle)
            CloseHandle(static_cast<HANDLE>(jobHandle));
#else
        (void)processHandle;
        (void)jobHandle;
#endif
    }

    EditorProcessStopResult StopEditorProcessTree(void* processHandle, void* jobHandle, unsigned long pid,
                                                  unsigned long gracePeriodMs, unsigned int exitCode)
    {
#ifdef _WIN32
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
#else
        (void)processHandle;
        (void)jobHandle;
        (void)pid;
        (void)gracePeriodMs;
        (void)exitCode;
        return EditorProcessStopResult::NotRunning;
#endif
    }

    OwnedEditorProcess::OwnedEditorProcess(EditorProcessOperations operations) : m_operations(std::move(operations))
    {
        if (!m_operations.poll)
            m_operations.poll = [](void* handle, unsigned long& exitCode)
            { return PollProcessExited(handle, exitCode); };
        if (!m_operations.stopAndClose)
            m_operations.stopAndClose =
                [](void* processHandle, void* jobHandle, unsigned long pid, unsigned long gracePeriodMs)
            { return StopEditorProcessTree(processHandle, jobHandle, pid, gracePeriodMs); };
        if (!m_operations.close)
            m_operations.close = [](void* processHandle, void* jobHandle)
            { CloseEditorProcessHandles(processHandle, jobHandle); };
    }

    OwnedEditorProcess::~OwnedEditorProcess()
    {
        (void)Stop();
    }

    bool OwnedEditorProcess::Adopt(ProcessLaunchResult launch)
    {
        if (!launch.success || !launch.processHandle)
        {
            if (launch.processHandle || launch.jobHandle)
                m_operations.close(launch.processHandle, launch.jobHandle);
            return false;
        }

        if (IsRunning())
            (void)Stop();

        m_processHandle = launch.processHandle;
        m_jobHandle = launch.jobHandle;
        m_pid = launch.pid;
        return true;
    }

    bool OwnedEditorProcess::Poll(unsigned long& outExitCode)
    {
        if (!IsRunning() || !m_operations.poll(m_processHandle, outExitCode))
            return false;

        m_operations.close(m_processHandle, m_jobHandle);
        Clear();
        return true;
    }

    EditorProcessStopResult OwnedEditorProcess::Stop(unsigned long gracePeriodMs)
    {
        if (!IsRunning())
            return EditorProcessStopResult::NotRunning;

        const EditorProcessStopResult result =
            m_operations.stopAndClose(m_processHandle, m_jobHandle, m_pid, gracePeriodMs);
        Clear();
        return result;
    }

    void OwnedEditorProcess::Clear() noexcept
    {
        m_processHandle = nullptr;
        m_jobHandle = nullptr;
        m_pid = 0;
    }

    std::string GetEditorExecutableDirectory()
    {
#ifdef _WIN32
        const std::filesystem::path executablePath = GetCurrentExecutablePath();
        return executablePath.empty() ? std::string{} : LaunchContext::PathToUtf8(executablePath.parent_path());
#else
        return LaunchContext::PathToUtf8(std::filesystem::canonical("/proc/self/exe").parent_path());
#endif
    }

    bool FindEngineExecutable(std::filesystem::path& outExePath, std::string& outError)
    {
        namespace fs = std::filesystem;
        const fs::path exeDir = LaunchContext::PathFromUtf8(GetEditorExecutableDirectory());
        if (exeDir.empty())
        {
            outError = "Could not determine the SparkEditor executable directory";
            return false;
        }
        const fs::path engineExe = exeDir / "SparkEngine.exe";

        std::error_code ec;
        if (!fs::exists(engineExe, ec) || ec)
        {
            outError = "SparkEngine.exe not found next to the editor (" + LaunchContext::PathToUtf8(exeDir) + ")";
            return false;
        }

        outExePath = engineExe;
        return true;
    }

    std::wstring BuildGameLaunchCommandLine(const std::filesystem::path& engineExe, const std::filesystem::path& dll,
                                            bool headless, const std::filesystem::path& execCfg,
                                            const std::filesystem::path& manifest, const std::wstring& extraArgs,
                                            std::string& outError)
    {
#ifdef _WIN32
        outError.clear();
        std::wstring cmd = QuoteWindowsArgument(engineExe.wstring()) + L" -game " + QuoteWindowsArgument(dll.wstring());
        if (headless)
            cmd += L" -headless";

        if (!execCfg.empty())
            cmd += L" -exec " + QuoteWindowsArgument(execCfg.wstring());

        if (!manifest.empty())
            cmd += L" -manifest " + QuoteWindowsArgument(manifest.wstring());

        if (!extraArgs.empty())
            cmd += L" " + extraArgs;

        return cmd;
#else
        (void)engineExe;
        (void)dll;
        (void)headless;
        (void)execCfg;
        (void)manifest;
        (void)extraArgs;
        outError = "Process launch is available on Windows builds only.";
        return L"";
#endif
    }

    std::wstring BuildGameLaunchCommandLine(const std::filesystem::path& engineExe, const std::filesystem::path& dll,
                                            bool headless, const std::filesystem::path& execCfg,
                                            const std::wstring& extraArgs, std::string& outError)
    {
        return BuildGameLaunchCommandLine(engineExe, dll, headless, execCfg, {}, extraArgs, outError);
    }

} // namespace SparkEditor
