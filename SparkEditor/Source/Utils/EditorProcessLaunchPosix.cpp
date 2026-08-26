/**
 * @file EditorProcessLaunchPosix.cpp
 * @brief POSIX editor child-process backend.
 */

#include "EditorProcessLaunch.h"
#include "EditorLaunchContext.h"

#ifndef _WIN32
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace SparkEditor
{
    namespace
    {
#include "EditorProcessLaunchPosixInternal.h"

        ProcessLaunchResult LaunchEditorProcessImpl(const std::filesystem::path& exePath,
                                                    const std::wstring& commandLine,
                                                    const std::filesystem::path& workingDir, bool ownProcessTree)
        {
            ProcessLaunchResult result;
            std::vector<std::wstring> wideArguments;
            if (!ParsePosixCommandLine(commandLine, wideArguments, result.error))
                return result;

            std::vector<std::string> arguments;
            arguments.reserve(wideArguments.size() + 2);
            for (const std::wstring& argument : wideArguments)
            {
                std::string encoded;
                if (!EncodeUtf8(argument, encoded))
                {
                    result.error = "Launch failed: command line contains invalid Unicode";
                    return result;
                }
                arguments.push_back(std::move(encoded));
            }

            const std::string executable = LaunchContext::PathToUtf8(exePath);
            arguments.front() = executable;
            const bool hasManifest = std::find(arguments.begin() + 1, arguments.end(), "-manifest") != arguments.end();
            const std::filesystem::path manifest = workingDir / "spark.modules.json";
            std::error_code filesystemError;
            if (!hasManifest && std::filesystem::is_regular_file(manifest, filesystemError) && !filesystemError)
            {
                arguments.emplace_back("-manifest");
                arguments.push_back(LaunchContext::PathToUtf8(manifest));
            }

            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1);
            for (std::string& argument : arguments)
                argv.push_back(argument.data());
            argv.push_back(nullptr);

            int errorPipe[2] = {-1, -1};
            if (pipe(errorPipe) != 0)
            {
                result.error = "Launch failed: could not create exec status pipe: " + std::string(std::strerror(errno));
                return result;
            }
            for (const int descriptor : errorPipe)
            {
                const int flags = fcntl(descriptor, F_GETFD);
                if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0)
                {
                    const int error = errno;
                    close(errorPipe[0]);
                    close(errorPipe[1]);
                    result.error =
                        "Launch failed: could not configure exec status pipe: " + std::string(std::strerror(error));
                    return result;
                }
            }

            auto handle = std::make_unique<PosixProcessHandle>();
            const pid_t child = fork();
            if (child < 0)
            {
                const int error = errno;
                close(errorPipe[0]);
                close(errorPipe[1]);
                result.error = "Launch failed: fork failed: " + std::string(std::strerror(error));
                return result;
            }
            if (child == 0)
            {
                close(errorPipe[0]);
                const auto fail = [&](int error)
                {
                    (void)write(errorPipe[1], &error, sizeof(error));
                    _exit(127);
                };
                if (ownProcessTree && setpgid(0, 0) != 0)
                    fail(errno);
                if (!workingDir.empty() && chdir(workingDir.c_str()) != 0)
                    fail(errno);
                execv(executable.c_str(), argv.data());
                fail(errno);
            }

            close(errorPipe[1]);
            if (ownProcessTree && setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH)
            {
                const int error = errno;
                (void)kill(child, SIGKILL);
                (void)waitpid(child, nullptr, 0);
                close(errorPipe[0]);
                result.error = "Launch failed: could not establish process group: " + std::string(std::strerror(error));
                return result;
            }

            int execError = 0;
            ssize_t errorBytes = -1;
            do
            {
                errorBytes = read(errorPipe[0], &execError, sizeof(execError));
            } while (errorBytes < 0 && errno == EINTR);
            close(errorPipe[0]);
            if (errorBytes > 0)
            {
                (void)waitpid(child, nullptr, 0);
                result.error = "Launch failed: exec failed: " + std::string(std::strerror(execError));
                return result;
            }
            if (errorBytes < 0)
            {
                const int error = errno;
                (void)kill(child, SIGKILL);
                (void)waitpid(child, nullptr, 0);
                result.error = "Launch failed: could not read exec status: " + std::string(std::strerror(error));
                return result;
            }

            handle->pid = child;
            handle->processGroup = ownProcessTree ? child : -1;
            result.success = true;
            result.processHandle = handle.release();
            result.pid = static_cast<unsigned long>(child);
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
        return PollPosixProcess(*static_cast<PosixProcessHandle*>(processHandle), outExitCode);
    }

    void TerminateEditorProcess(void* processHandle, unsigned int exitCode)
    {
        if (!processHandle)
            return;
        PosixProcessHandle& handle = *static_cast<PosixProcessHandle*>(processHandle);
        if (!handle.exited)
            handle.terminationRequested = SignalPosixProcess(handle, SIGKILL) == 0;
        (void)exitCode;
    }

    void CloseEditorProcessHandles(void* processHandle, void* jobHandle)
    {
        (void)jobHandle;
        if (!processHandle)
            return;
        auto* handle = static_cast<PosixProcessHandle*>(processHandle);
        if (handle->processGroup > 1)
        {
            // Closing an owned POSIX process mirrors closing a Windows
            // kill-on-close Job Object: no descendant may outlive ownership.
            (void)kill(-handle->processGroup, SIGKILL);
            if (!handle->exited)
            {
                int status = 0;
                pid_t waited = -1;
                do
                {
                    waited = waitpid(handle->pid, &status, 0);
                } while (waited < 0 && errno == EINTR);
                if (waited == handle->pid)
                {
                    handle->exited = true;
                    handle->exitCode = PosixExitCode(status);
                }
            }
        }
        else if (!handle->exited)
        {
            unsigned long ignored = 0;
            if (!PollPosixProcess(*handle, ignored) && handle->terminationRequested)
            {
                int status = 0;
                pid_t waited = -1;
                do
                {
                    waited = waitpid(handle->pid, &status, 0);
                } while (waited < 0 && errno == EINTR);
                if (waited == handle->pid)
                {
                    handle->exited = true;
                    handle->exitCode = PosixExitCode(status);
                }
            }
            else if (!handle->exited)
            {
                // Non-owned launches intentionally survive editor shutdown.
                // Keep a tiny detached waiter so the eventual child exit is
                // reaped instead of becoming a zombie.
                const pid_t child = handle->pid;
                std::thread(
                    [child]
                    {
                        pid_t waited = -1;
                        do
                        {
                            waited = waitpid(child, nullptr, 0);
                        } while (waited < 0 && errno == EINTR);
                    })
                    .detach();
            }
        }
        delete handle;
    }

    EditorProcessStopResult StopEditorProcessTree(void* processHandle, void* jobHandle, unsigned long pid,
                                                  unsigned long gracePeriodMs, unsigned int exitCode)
    {
        (void)jobHandle;
        (void)pid;
        (void)exitCode;
        if (!processHandle)
            return EditorProcessStopResult::NotRunning;

        auto* handle = static_cast<PosixProcessHandle*>(processHandle);
        unsigned long ignoredExitCode = 0;
        if (PollPosixProcess(*handle, ignoredExitCode))
        {
            CloseEditorProcessHandles(processHandle, nullptr);
            return EditorProcessStopResult::Graceful;
        }

        if (SignalPosixProcess(*handle, SIGTERM) != 0 && errno != ESRCH)
        {
            CloseEditorProcessHandles(processHandle, nullptr);
            return EditorProcessStopResult::Failed;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(gracePeriodMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (PollPosixProcess(*handle, ignoredExitCode))
            {
                CloseEditorProcessHandles(processHandle, nullptr);
                return EditorProcessStopResult::Graceful;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const bool terminated = SignalPosixProcess(*handle, SIGKILL) == 0 || errno == ESRCH;
        if (terminated && !handle->exited)
        {
            int status = 0;
            pid_t waited = -1;
            do
            {
                waited = waitpid(handle->pid, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited == handle->pid)
            {
                handle->exited = true;
                handle->exitCode = PosixExitCode(status);
            }
        }
        CloseEditorProcessHandles(processHandle, nullptr);
        return terminated ? EditorProcessStopResult::Terminated : EditorProcessStopResult::Failed;
    }
} // namespace SparkEditor
#endif
