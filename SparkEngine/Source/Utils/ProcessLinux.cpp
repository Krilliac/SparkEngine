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

#include <cerrno>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
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

        /// Read all available bytes from fd into dest. Returns false on EOF/error.
        static bool ReadAvailable(int fd, std::string& dest)
        {
            char buf[4096];
            for (;;)
            {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0)
                {
                    dest.append(buf, static_cast<size_t>(n));
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
                break; // EOF or error
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
            return std::unexpected(err);
        }

        if (pid == 0)
        {
            // ---- Child process ----

            if (m_detached)
                setsid(); // Detach from parent's session

            if (!m_workingDirectory.empty() && chdir(m_workingDirectory.c_str()) != 0)
                _exit(126);

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

            execvp(m_executable.c_str(), const_cast<char* const*>(argv.data()));
            _exit(127); // exec failed
        }

        // ---- Parent process ----

        // Close child-side pipe ends
        Impl::CloseFd(stdinPipe[0]);
        Impl::CloseFd(stdoutPipe[1]);
        Impl::CloseFd(stderrPipe[1]);

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
            break;
        }
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

        // Read any available data into the buffer
        Impl::ReadAvailable(m_impl->stdoutReadFd, m_impl->stdoutBuffer);

        // Check for a complete line
        auto pos = m_impl->stdoutBuffer.find('\n');
        if (pos == std::string::npos)
            return false;

        line = m_impl->stdoutBuffer.substr(0, pos);
        // Trim trailing \r if present
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        m_impl->stdoutBuffer.erase(0, pos + 1);
        return true;
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
