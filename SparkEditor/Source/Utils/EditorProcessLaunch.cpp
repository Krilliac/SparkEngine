/**
 * @file EditorProcessLaunch.cpp
 * @brief Shared CreateProcessW helper implementation.
 */

#include "EditorProcessLaunch.h"
#include "EditorLaunchContext.h"

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
    } // namespace
#endif

    ProcessLaunchResult LaunchEditorProcess(const std::filesystem::path& exePath, const std::wstring& commandLine,
                                            const std::filesystem::path& workingDir)
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
