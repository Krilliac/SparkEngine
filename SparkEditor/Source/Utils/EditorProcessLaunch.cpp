/**
 * @file EditorProcessLaunch.cpp
 * @brief Process ownership, executable discovery, and command-line construction.
 */

#include "EditorProcessLaunch.h"
#include "EditorLaunchContext.h"
#include "EditorProcessLaunchText.h"

#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
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
    } // namespace
#endif

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
#elif defined(__APPLE__)
        uint32_t requiredSize = 0;
        (void)_NSGetExecutablePath(nullptr, &requiredSize);
        if (requiredSize == 0)
            return {};

        std::vector<char> buffer(requiredSize);
        if (_NSGetExecutablePath(buffer.data(), &requiredSize) != 0)
            return {};

        std::error_code error;
        std::filesystem::path executablePath = std::filesystem::weakly_canonical(buffer.data(), error);
        if (error)
            executablePath = std::filesystem::path(buffer.data()).lexically_normal();
        return LaunchContext::PathToUtf8(executablePath.parent_path());
#else
        std::error_code error;
        const std::filesystem::path executablePath = std::filesystem::canonical("/proc/self/exe", error);
        return error ? std::string{} : LaunchContext::PathToUtf8(executablePath.parent_path());
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
#ifdef _WIN32
        const fs::path engineExe = exeDir / "SparkEngine.exe";
        constexpr std::string_view executableName = "SparkEngine.exe";
#else
        const fs::path engineExe = exeDir / "SparkEngine";
        constexpr std::string_view executableName = "SparkEngine";
#endif

        std::error_code ec;
        if (!fs::exists(engineExe, ec) || ec)
        {
            outError = std::string(executableName) + " not found next to the editor (" +
                       LaunchContext::PathToUtf8(exeDir) + ")";
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
        std::wstring cmd = Detail::QuoteWindowsArgument(engineExe.wstring()) + L" -game " +
                           Detail::QuoteWindowsArgument(dll.wstring());
        if (headless)
            cmd += L" -headless";

        if (!execCfg.empty())
            cmd += L" -exec " + Detail::QuoteWindowsArgument(execCfg.wstring());

        if (!manifest.empty())
            cmd += L" -manifest " + Detail::QuoteWindowsArgument(manifest.wstring());

        if (!extraArgs.empty())
            cmd += L" " + extraArgs;

        return cmd;
#else
        outError.clear();
        std::wstring engineArgument;
        std::wstring moduleArgument;
        if (!Detail::DecodeUtf8(LaunchContext::PathToUtf8(engineExe), engineArgument) ||
            !Detail::DecodeUtf8(LaunchContext::PathToUtf8(dll), moduleArgument))
        {
            outError = "Launch path contains invalid UTF-8";
            return {};
        }

        std::wstring command =
            Detail::QuotePosixArgument(engineArgument) + L" -game " + Detail::QuotePosixArgument(moduleArgument);
        if (headless)
            command += L" -headless";

        if (!execCfg.empty())
        {
            std::wstring execArgument;
            if (!Detail::DecodeUtf8(LaunchContext::PathToUtf8(execCfg), execArgument))
            {
                outError = "Exec path contains invalid UTF-8";
                return {};
            }
            command += L" -exec " + Detail::QuotePosixArgument(execArgument);
        }

        if (!manifest.empty())
        {
            std::wstring manifestArgument;
            if (!Detail::DecodeUtf8(LaunchContext::PathToUtf8(manifest), manifestArgument))
            {
                outError = "Manifest path contains invalid UTF-8";
                return {};
            }
            command += L" -manifest " + Detail::QuotePosixArgument(manifestArgument);
        }

        if (!extraArgs.empty())
            command += L" " + extraArgs;
        return command;
#endif
    }

    std::wstring BuildGameLaunchCommandLine(const std::filesystem::path& engineExe, const std::filesystem::path& dll,
                                            bool headless, const std::filesystem::path& execCfg,
                                            const std::wstring& extraArgs, std::string& outError)
    {
        return BuildGameLaunchCommandLine(engineExe, dll, headless, execCfg, {}, extraArgs, outError);
    }


} // namespace SparkEditor
