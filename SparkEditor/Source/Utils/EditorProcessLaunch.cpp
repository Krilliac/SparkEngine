/**
 * @file EditorProcessLaunch.cpp
 * @brief Shared CreateProcessW helper implementation.
 */

#include "EditorProcessLaunch.h"

#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SparkEditor
{

#ifdef _WIN32
    namespace
    {
        /// @brief Converts `p` to its 8.3 short form if it contains a space (the
        /// engine's -game/-exec cmdline parsers split on the first space and
        /// understand no quoting). Returns the original path unchanged if it has no
        /// space. Sets *outError and returns an empty wstring on failure.
        std::wstring ToShortPathIfNeeded(const std::filesystem::path& p, const char* argLabel, std::string& outError)
        {
            std::wstring w = p.wstring();
            if (w.find(L' ') == std::wstring::npos)
                return w;

            wchar_t shortBuf[MAX_PATH];
            const DWORD len = GetShortPathNameW(w.c_str(), shortBuf, MAX_PATH);
            if (len == 0 || len >= MAX_PATH || std::wstring_view(shortBuf, len).find(L' ') != std::wstring_view::npos)
            {
                outError = std::string(argLabel) +
                           " path contains spaces and has no short form — the engine's cmdline parser cannot "
                           "receive it. Move the build to a space-free path.";
                return L"";
            }
            return std::wstring(shortBuf, len);
        }
    } // namespace
#endif

    ProcessLaunchResult LaunchEditorProcess(const std::filesystem::path& exePath, const std::wstring& commandLine,
                                            const std::filesystem::path& workingDir)
    {
        ProcessLaunchResult result;
#ifdef _WIN32
        // CreateProcessW may modify the command-line buffer — pass a writable copy.
        std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        const std::wstring exeW = exePath.wstring();
        const std::wstring workingDirW = workingDir.wstring();
        const BOOL ok = CreateProcessW(exeW.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                       workingDirW.c_str(), &startup, &process);
        if (!ok)
        {
            result.success = false;
            result.error = "Launch failed (Win32 error " + std::to_string(GetLastError()) + ")";
            return result;
        }

        CloseHandle(process.hThread);
        result.success = true;
        result.processHandle = process.hProcess;
        result.pid = process.dwProcessId;
#else
        (void)exePath;
        (void)commandLine;
        (void)workingDir;
        result.success = false;
        result.error = "Process launch is available on Windows builds only.";
#endif
        return result;
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

    std::wstring BuildGameLaunchCommandLine(const std::filesystem::path& engineExe, const std::filesystem::path& dll,
                                            bool headless, const std::filesystem::path& execCfg,
                                            const std::wstring& extraArgs, std::string& outError)
    {
#ifdef _WIN32
        const std::wstring dllArg = ToShortPathIfNeeded(dll, "Module DLL", outError);
        if (dllArg.empty() && !outError.empty())
            return L"";

        std::wstring cmd = L"\"" + engineExe.wstring() + L"\" -game " + dllArg;
        if (headless)
            cmd += L" -headless";

        if (!execCfg.empty())
        {
            const std::wstring cfgArg = ToShortPathIfNeeded(execCfg, "Exec cfg", outError);
            if (cfgArg.empty() && !outError.empty())
                return L"";
            cmd += L" -exec " + cfgArg;
        }

        if (!extraArgs.empty())
            cmd += L" " + extraArgs;

        return cmd;
#else
        (void)engineExe;
        (void)dll;
        (void)headless;
        (void)execCfg;
        (void)extraArgs;
        outError = "Process launch is available on Windows builds only.";
        return L"";
#endif
    }

} // namespace SparkEditor
