/**
 * @file ProcessLinux.cpp
 * @brief Linux/macOS implementation of Spark::Process
 *
 * Uses fork/exec for process creation and POSIX pipes for I/O redirection.
 * poll() is used for non-blocking reads.
 */

#include "Core/Platform.h"

#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)

#include "Process.h"
#include "ProcessPipeBuffer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Spark
{

    // =========================================================================
    // Impl
    // =========================================================================

    struct Process::Impl
    {
        pid_t childPid = -1;
        bool detached = false;

        int stdinWriteFd = -1; ///< Parent writes to child's stdin.
        int stdoutReadFd = -1; ///< Parent reads from child's stdout.
        int stderrReadFd = -1; ///< Parent reads from child's stderr.

        std::string stdoutBuffer; ///< Partial line buffer for TryReadLine.

        /// Cached exit status. -1 means not yet collected.
        int exitStatus = -1;
        bool exited = false;

        ~Impl() { Cleanup(); }

        void Cleanup()
        {
            CloseFd(stdinWriteFd);
            CloseFd(stdoutReadFd);
            CloseFd(stderrReadFd);

            if (childPid > 0 && !detached)
            {
                // Reap zombie if already dead, else kill.
                int status = 0;
                pid_t result = waitpid(childPid, &status, WNOHANG);
                if (result == 0)
                {
                    kill(childPid, SIGKILL);
                    waitpid(childPid, &status, 0);
                }
                childPid = -1;
            }
        }

        static void CloseFd(int& fd)
        {
            if (fd >= 0)
            {
                close(fd);
                fd = -1;
            }
        }

        static bool SetNonBlocking(int fd)
        {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags == -1)
                return false;
            return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
        }

        void PollExitStatus()
        {
            if (exited || childPid <= 0 || detached)
                return;
            int status = 0;
            pid_t result = waitpid(childPid, &status, WNOHANG);
            if (result > 0)
            {
                exited = true;
                exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        }

        /// Drain a bounded amount of available data. Returns false on EOF/error.
        static bool ReadAvailable(int fd, std::string& dest)
        {
            char buf[4096];
            size_t drainedBytes = 0;
            for (;;)
            {
                const size_t capacity = ProcessDetail::RemainingReadCapacity(dest.size(), drainedBytes);
                if (capacity == 0)
                    return true;
                const size_t requested = std::min(capacity, sizeof(buf));
                ssize_t n = read(fd, buf, requested);
                if (n > 0)
                {
                    dest.append(buf, static_cast<size_t>(n));
                    drainedBytes += static_cast<size_t>(n);
                    continue;
                }
                if (n == 0)
                    return false; // EOF
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return true; // No more data right now
                return false;    // Error
            }
        }

        /// Blocking read until EOF. Used by ReadAllStdout/ReadAllStderr.
        static std::string ReadUntilEof(int fd)
        {
            std::string result;
            char buf[4096];
            for (;;)
            {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0)
                {
                    result.append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue; // A signal interrupted the read; the pipe is still open
                break;        // EOF or error
            }
            return result;
        }
    };

    // =========================================================================
    // Builder
    // =========================================================================

    Process::Builder::Builder(std::string executable) : m_executable(std::move(executable)) {}

    Process::Builder& Process::Builder::Arg(std::string arg)
    {
        m_args.push_back(std::move(arg));
        return *this;
    }

    Process::Builder& Process::Builder::WorkingDirectory(std::string directory)
    {
        m_workingDirectory = std::move(directory);
        return *this;
    }

    Process::Builder& Process::Builder::CaptureStdout()
    {
        m_stdoutMode = PipeMode::Capture;
        return *this;
    }

    Process::Builder& Process::Builder::CaptureStderr()
    {
        m_stderrMode = PipeMode::Capture;
        return *this;
    }

    Process::Builder& Process::Builder::MergeStderrIntoStdout()
    {
        m_mergeStderrIntoStdout = true;
        return *this;
    }

    Process::Builder& Process::Builder::CaptureStdin()
    {
        m_stdinMode = PipeMode::Capture;
        return *this;
    }

    Process::Builder& Process::Builder::NoWindow()
    {
        m_noWindow = true;
        return *this;
    }

    Process::Builder& Process::Builder::Detached()
    {
        m_detached = true;
        return *this;
    }

    std::expected<Process, std::string> Process::Builder::Launch()
    {
        // Build argv for execvp: [executable, arg0, arg1, ..., nullptr]
        std::vector<const char*> argv;
        argv.push_back(m_executable.c_str());
        for (const auto& a : m_args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);

        // Create pipes as needed
        int stdinPipe[2] = {-1, -1};  // [0]=child reads, [1]=parent writes
        int stdoutPipe[2] = {-1, -1}; // [0]=parent reads, [1]=child writes
        int stderrPipe[2] = {-1, -1}; // [0]=parent reads, [1]=child writes

        auto makePipe = [](int fds[2], const char* name) -> std::expected<void, std::string>
        {
#ifdef __linux__
            if (pipe2(fds, O_CLOEXEC) == -1)
                return std::unexpected(std::string("pipe() failed for ") + name + ": " + strerror(errno));
#else
            // macOS lacks pipe2(); use pipe() + fcntl() instead.
            if (pipe(fds) == -1)
                return std::unexpected(std::string("pipe() failed for ") + name + ": " + strerror(errno));
            for (int i = 0; i < 2; ++i)
                fcntl(fds[i], F_SETFD, fcntl(fds[i], F_GETFD) | FD_CLOEXEC);
#endif
            return {};
        };

        if (m_stdinMode == PipeMode::Capture)
        {
            if (auto r = makePipe(stdinPipe, "stdin"); !r)
                return std::unexpected(r.error());
        }
        if (m_stdoutMode == PipeMode::Capture)
        {
            if (auto r = makePipe(stdoutPipe, "stdout"); !r)
            {
                Impl::CloseFd(stdinPipe[0]);
                Impl::CloseFd(stdinPipe[1]);
                return std::unexpected(r.error());
            }
        }
        if (m_stderrMode == PipeMode::Capture && !m_mergeStderrIntoStdout)
        {
            if (auto r = makePipe(stderrPipe, "stderr"); !r)
            {
                Impl::CloseFd(stdinPipe[0]);
                Impl::CloseFd(stdinPipe[1]);
                Impl::CloseFd(stdoutPipe[0]);
                Impl::CloseFd(stdoutPipe[1]);
                return std::unexpected(r.error());
            }
        }

        // The child reports a failed chdir()/exec through this close-on-exec
        // pipe. A successful exec closes the write end, so the parent reads EOF;
        // a failure delivers the child's errno first. Without it, a missing
        // executable "launches" successfully and only surfaces as exit code 127,
        // unlike CreateProcess on Windows which fails Launch() outright.
        int execErrorPipe[2] = {-1, -1};
        if (auto r = makePipe(execErrorPipe, "exec status"); !r)
        {
            Impl::CloseFd(stdinPipe[0]);
            Impl::CloseFd(stdinPipe[1]);
            Impl::CloseFd(stdoutPipe[0]);
            Impl::CloseFd(stdoutPipe[1]);
            Impl::CloseFd(stderrPipe[0]);
            Impl::CloseFd(stderrPipe[1]);
            return std::unexpected(r.error());
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            std::string err = std::string("fork() failed: ") + strerror(errno);
            Impl::CloseFd(stdinPipe[0]);
            Impl::CloseFd(stdinPipe[1]);
            Impl::CloseFd(stdoutPipe[0]);
            Impl::CloseFd(stdoutPipe[1]);
            Impl::CloseFd(stderrPipe[0]);
            Impl::CloseFd(stderrPipe[1]);
            Impl::CloseFd(execErrorPipe[0]);
            Impl::CloseFd(execErrorPipe[1]);
            return std::unexpected(err);
        }

        if (pid == 0)
        {
            // ---- Child process ----
            // Only async-signal-safe calls from here on: the parent may be multithreaded.

            const int errorFd = execErrorPipe[1];
            auto reportFailureAndExit = [errorFd](int stage, int exitCode)
            {
                const int report[2] = {stage, errno};
                (void)!write(errorFd, report, sizeof(report));
                _exit(exitCode);
            };

            if (m_detached)
            {
                // Detach from the parent's session, then fork again so the
                // launcher never owns the long-lived child. The intermediate
                // exits immediately (the parent reaps it below) and the
                // grandchild is re-parented to init, so no zombie accumulates
                // in the launching process when the detached child exits.
                setsid();
                const pid_t grandchild = fork();
                if (grandchild == -1)
                    reportFailureAndExit(0, 127);
                if (grandchild > 0)
                    _exit(0);
            }

            if (!m_workingDirectory.empty() && chdir(m_workingDirectory.c_str()) != 0)
                reportFailureAndExit(1, 126);

            // Redirect stdin/stdout/stderr via dup2.
            // Pipes are O_CLOEXEC so originals auto-close on exec;
            // we only need dup2 (which clears CLOEXEC on the target fd).
            // Guard against the case where a pipe fd IS already the
            // target fd (e.g. stderrPipe[0]==2) — dup2(fd,fd) is a no-op
            // that would leave the original CLOEXEC, so use fcntl instead.
            auto redirectFd = [](int srcFd, int targetFd)
            {
                if (srcFd == targetFd)
                {
                    // Already the right fd — just clear CLOEXEC so it survives exec
                    int flags = fcntl(srcFd, F_GETFD);
                    if (flags != -1)
                        fcntl(srcFd, F_SETFD, flags & ~FD_CLOEXEC);
                }
                else
                {
                    dup2(srcFd, targetFd); // target fd gets CLOEXEC cleared
                }
            };

            if (stdinPipe[0] >= 0)
                redirectFd(stdinPipe[0], STDIN_FILENO);
            if (stdoutPipe[1] >= 0)
                redirectFd(stdoutPipe[1], STDOUT_FILENO);
            if (m_mergeStderrIntoStdout)
                redirectFd(stdoutPipe[1] >= 0 ? stdoutPipe[1] : STDOUT_FILENO, STDERR_FILENO);
            else if (stderrPipe[1] >= 0)
                redirectFd(stderrPipe[1], STDERR_FILENO);

            // A detached child outlives its launcher, so it must not keep the
            // launcher's stdio open. Otherwise a supervisor reading the
            // launcher's stdout to EOF never sees EOF while the detached child
            // (e.g. SparkCrashReporter watching the engine) runs, and a child
            // that waits for the un-reaped launcher to disappear deadlocks it.
            if (m_detached)
            {
                const int devNull = open("/dev/null", O_RDWR);
                if (devNull >= 0)
                {
                    if (stdinPipe[0] < 0)
                        dup2(devNull, STDIN_FILENO);
                    if (stdoutPipe[1] < 0)
                        dup2(devNull, STDOUT_FILENO);
                    if (stderrPipe[1] < 0 && !(m_mergeStderrIntoStdout && stdoutPipe[1] >= 0))
                        dup2(devNull, STDERR_FILENO);
                    if (devNull > STDERR_FILENO)
                        close(devNull);
                }
            }

            execvp(m_executable.c_str(), const_cast<char* const*>(argv.data()));
            reportFailureAndExit(2, 127); // exec failed
        }

        // ---- Parent process ----

        // Close child-side pipe ends
        Impl::CloseFd(stdinPipe[0]);
        Impl::CloseFd(stdoutPipe[1]);
        Impl::CloseFd(stderrPipe[1]);
        Impl::CloseFd(execErrorPipe[1]);

        // Blocks only until the (grand)child execs or fails, never for its lifetime.
        int childReport[2] = {0, 0};
        size_t reportBytes = 0;
        while (reportBytes < sizeof(childReport))
        {
            const ssize_t n = read(execErrorPipe[0], reinterpret_cast<char*>(childReport) + reportBytes,
                                   sizeof(childReport) - reportBytes);
            if (n > 0)
                reportBytes += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR)
                continue;
            else
                break;
        }
        Impl::CloseFd(execErrorPipe[0]);

        if (m_detached)
        {
            // Reap the short-lived intermediate child; the grandchild is not ours.
            int status = 0;
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
            {
            }
        }

        if (reportBytes == sizeof(childReport))
        {
            if (!m_detached)
            {
                int status = 0;
                while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
                {
                }
            }
            Impl::CloseFd(stdinPipe[1]);
            Impl::CloseFd(stdoutPipe[0]);
            Impl::CloseFd(stderrPipe[0]);

            static constexpr const char* kStages[] = {"fork() of detached child", "chdir()", "exec()"};
            const int stage = childReport[0] >= 0 && childReport[0] <= 2 ? childReport[0] : 2;
            std::string err = std::string(kStages[stage]) + " failed for '" +
                              (stage == 1 ? m_workingDirectory : m_executable) + "': " + strerror(childReport[1]);
            return std::unexpected(err);
        }

        Process proc;
        proc.m_impl = std::make_unique<Impl>();
        proc.m_impl->childPid = pid;
        proc.m_impl->detached = m_detached;
        proc.m_impl->stdinWriteFd = stdinPipe[1];
        proc.m_impl->stdoutReadFd = stdoutPipe[0];
        proc.m_impl->stderrReadFd = stderrPipe[0];

        // Set read ends to non-blocking for TryReadLine
        if (stdoutPipe[0] >= 0)
            Impl::SetNonBlocking(stdoutPipe[0]);
        if (stderrPipe[0] >= 0)
            Impl::SetNonBlocking(stderrPipe[0]);

        return proc;
    }

    // =========================================================================
    // Process
    // =========================================================================

    Process::Process() = default;
    Process::~Process() = default;
    Process::Process(Process&& other) noexcept = default;
    Process& Process::operator=(Process&& other) noexcept = default;

    bool Process::IsRunning() const
    {
        if (!m_impl || m_impl->childPid <= 0 || m_impl->detached)
            return false;
        m_impl->PollExitStatus();
        return !m_impl->exited;
    }

    std::optional<int> Process::GetExitCode() const
    {
        if (!m_impl || m_impl->detached)
            return std::nullopt;
        m_impl->PollExitStatus();
        if (m_impl->exited)
            return m_impl->exitStatus;
        return std::nullopt;
    }

    int Process::WaitForExit()
    {
        if (!m_impl || m_impl->childPid <= 0 || m_impl->detached)
            return -1;
        if (m_impl->exited)
            return m_impl->exitStatus;

        int status = 0;
        pid_t result;
        do
        {
            result = waitpid(m_impl->childPid, &status, 0);
        } while (result == -1 && errno == EINTR);
        if (result != m_impl->childPid)
            return -1;
        m_impl->exited = true;
        m_impl->exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return m_impl->exitStatus;
    }

    bool Process::WaitForExit(std::chrono::milliseconds timeout)
    {
        if (!m_impl || m_impl->childPid <= 0 || m_impl->detached)
            return true;
        if (m_impl->exited)
            return true;

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            m_impl->PollExitStatus();
            if (m_impl->exited)
                return true;
            // Sleep briefly then retry (1ms resolution)
            usleep(1000);
        }
        return m_impl->exited;
    }

    void Process::Kill()
    {
        if (!m_impl || m_impl->childPid <= 0 || m_impl->detached || m_impl->exited)
            return;
        kill(m_impl->childPid, SIGKILL);
        int status = 0;
        waitpid(m_impl->childPid, &status, 0);
        m_impl->exited = true;
        m_impl->exitStatus = -1;
    }

    void Process::WriteStdin(std::string_view data)
    {
        if (!m_impl || m_impl->stdinWriteFd < 0)
            return;

        // Writing to a pipe whose reader has exited raises SIGPIPE, whose
        // default action terminates the *launching* process (the editor, a
        // tool, the test runner). Block it on this thread for the duration of
        // the write so the failure surfaces as EPIPE instead, then discard the
        // SIGPIPE we generated so it is not delivered once the mask is restored.
        sigset_t sigpipeMask;
        sigemptyset(&sigpipeMask);
        sigaddset(&sigpipeMask, SIGPIPE);
        sigset_t pendingBefore;
        sigemptyset(&pendingBefore);
        sigpending(&pendingBefore);
        const bool sigpipeAlreadyPending = sigismember(&pendingBefore, SIGPIPE) == 1;
        sigset_t previousMask;
        const bool masked = pthread_sigmask(SIG_BLOCK, &sigpipeMask, &previousMask) == 0;
        bool brokenPipe = false;

        const char* cursor = data.data();
        size_t remaining = data.size();
        while (remaining > 0)
        {
            const ssize_t written = write(m_impl->stdinWriteFd, cursor, remaining);
            if (written > 0)
            {
                cursor += written;
                remaining -= static_cast<size_t>(written);
                continue;
            }

            if (written < 0 && errno == EINTR)
                continue;

            // EPIPE (child closed stdin) and any other write failure: stop writing.
            brokenPipe = written < 0 && errno == EPIPE;
            break;
        }

        if (brokenPipe && !sigpipeAlreadyPending)
        {
#if defined(__linux__)
            const timespec noWait{};
            while (sigtimedwait(&sigpipeMask, nullptr, &noWait) == -1 && errno == EINTR)
            {
            }
#else
            // macOS lacks sigtimedwait(); sigwait() is safe because the signal is pending.
            sigset_t pendingNow;
            sigemptyset(&pendingNow);
            int consumed = 0;
            if (sigpending(&pendingNow) == 0 && sigismember(&pendingNow, SIGPIPE) == 1)
                sigwait(&sigpipeMask, &consumed);
#endif
        }
        if (masked)
            pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
    }

    void Process::CloseStdin()
    {
        if (m_impl)
            Impl::CloseFd(m_impl->stdinWriteFd);
    }

    bool Process::TryReadLine(std::string& line)
    {
        if (!m_impl || m_impl->stdoutReadFd < 0)
            return false;

        if (ProcessDetail::ExtractBufferedLine(m_impl->stdoutBuffer, line))
            return true;

        // Read any available data into the buffer
        Impl::ReadAvailable(m_impl->stdoutReadFd, m_impl->stdoutBuffer);
        return ProcessDetail::ExtractBufferedLine(m_impl->stdoutBuffer, line);
    }

    std::string Process::ReadAllStdout()
    {
        if (!m_impl || m_impl->stdoutReadFd < 0)
            return {};

        // Switch to blocking mode for a clean read-until-EOF
        int flags = fcntl(m_impl->stdoutReadFd, F_GETFL, 0);
        if (flags != -1)
            fcntl(m_impl->stdoutReadFd, F_SETFL, flags & ~O_NONBLOCK);

        std::string result = std::move(m_impl->stdoutBuffer);
        result += Impl::ReadUntilEof(m_impl->stdoutReadFd);
        Impl::CloseFd(m_impl->stdoutReadFd);
        return result;
    }

    std::string Process::ReadAllStderr()
    {
        if (!m_impl || m_impl->stderrReadFd < 0)
            return {};

        // Switch to blocking mode
        int flags = fcntl(m_impl->stderrReadFd, F_GETFL, 0);
        if (flags != -1)
            fcntl(m_impl->stderrReadFd, F_SETFL, flags & ~O_NONBLOCK);

        std::string result = Impl::ReadUntilEof(m_impl->stderrReadFd);
        Impl::CloseFd(m_impl->stderrReadFd);
        return result;
    }

} // namespace Spark

#endif // SPARK_PLATFORM_LINUX || SPARK_PLATFORM_MACOS
