/**
 * @file ProcessWin32.cpp
 * @brief Windows implementation of Spark::Process
 *
 * Uses CreateProcessW for process creation and anonymous Win32 pipes
 * for stdin/stdout/stderr redirection. PeekNamedPipe provides non-blocking reads.
 */

#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "Process.h"

#include <sstream>

#include <windows.h>

namespace Spark
{

    // =========================================================================
    // RAII handle helper (local to this TU — WinHandle in ConsoleProcessManager.h
    // is specific to that class; this is independent)
    // =========================================================================

    namespace
    {
        struct AutoHandle
        {
            HANDLE h = NULL;

            AutoHandle() = default;
            explicit AutoHandle(HANDLE handle) : h(handle) {}
            ~AutoHandle()
            {
                if (h)
                    CloseHandle(h);
            }

            AutoHandle(const AutoHandle&) = delete;
            AutoHandle& operator=(const AutoHandle&) = delete;

            AutoHandle(AutoHandle&& o) noexcept : h(o.h) { o.h = NULL; }
            AutoHandle& operator=(AutoHandle&& o) noexcept
            {
                if (this != &o)
                {
                    if (h)
                        CloseHandle(h);
                    h = o.h;
                    o.h = NULL;
                }
                return *this;
            }

            void Close()
            {
                if (h)
                {
                    CloseHandle(h);
                    h = NULL;
                }
            }

            HANDLE Release()
            {
                HANDLE tmp = h;
                h = NULL;
                return tmp;
            }
        };

        // Process-global "kill on close" job object. Every child we launch is
        // assigned to it; because the job carries
        // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE and this process holds the only
        // handle, the OS terminates all assigned children the instant this
        // process exits — INCLUDING a force-kill / crash where destructors and
        // ConsoleProcessManager::Shutdown() never run. Without it, a
        // force-killed engine orphaned its SparkConsole child every time
        // (they accumulated across debugging/crash runs). Created lazily and
        // intentionally never closed (owned for the process lifetime).
        HANDLE GetChildKillJob()
        {
            static HANDLE s_job = []() -> HANDLE
            {
                HANDLE job = CreateJobObjectW(nullptr, nullptr);
                if (!job)
                    return nullptr;
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
                {
                    CloseHandle(job);
                    return nullptr;
                }
                return job;
            }();
            return s_job;
        }
        // Appends `arg` to `cmdLine` using the quoting/escaping rules that
        // CommandLineToArgvW (and therefore every well-behaved Win32 argv
        // parser, including CRT startup code) uses to split a command line
        // back into argv. This is the standard algorithm published by
        // Microsoft ("Everyone quotes command line arguments the wrong
        // way"): a literal `"` in the argument must be preceded by a
        // backslash, and a run of backslashes must be doubled if it is
        // immediately followed by a `"` (either an embedded one or the
        // closing quote) so it isn't misinterpreted as escaping that quote.
        // Without this, an argument containing `"` (or a trailing run of
        // `\`) can break out of its quoted field and inject additional
        // command-line tokens into the child process.
        void AppendQuotedArg(std::string& cmdLine, const std::string& arg)
        {
            if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
            {
                // No characters that require quoting.
                cmdLine += arg;
                return;
            }

            cmdLine += '"';
            for (auto it = arg.begin();; ++it)
            {
                unsigned numBackslashes = 0;
                while (it != arg.end() && *it == '\\')
                {
                    ++it;
                    ++numBackslashes;
                }

                if (it == arg.end())
                {
                    // Escape all backslashes, since they immediately precede
                    // the closing quote we're about to append.
                    cmdLine.append(numBackslashes * 2, '\\');
                    break;
                }
                else if (*it == '"')
                {
                    // Escape all backslashes and the following quote.
                    cmdLine.append(numBackslashes * 2 + 1, '\\');
                    cmdLine.push_back(*it);
                }
                else
                {
                    // A regular character; backslashes before it are literal.
                    cmdLine.append(numBackslashes, '\\');
                    cmdLine.push_back(*it);
                }
            }
            cmdLine += '"';
        }
    } // namespace

    // =========================================================================
    // Impl
    // =========================================================================

    struct Process::Impl
    {
        HANDLE processHandle = NULL;
        bool detached = false;

        HANDLE stdinWrite = NULL;
        HANDLE stdoutRead = NULL;
        HANDLE stderrRead = NULL;

        std::string stdoutBuffer; ///< Partial line buffer for TryReadLine.

        int exitStatus = -1;
        bool exited = false;

        ~Impl() { Cleanup(); }

        void Cleanup()
        {
            CloseH(stdinWrite);
            CloseH(stdoutRead);
            CloseH(stderrRead);

            if (processHandle && !detached)
            {
                DWORD ec;
                if (GetExitCodeProcess(processHandle, &ec) && ec == STILL_ACTIVE)
                {
                    TerminateProcess(processHandle, 1);
                    WaitForSingleObject(processHandle, 1000);
                }
                CloseHandle(processHandle);
                processHandle = NULL;
            }
        }

        static void CloseH(HANDLE& handle)
        {
            if (handle)
            {
                CloseHandle(handle);
                handle = NULL;
            }
        }

        void PollExitStatus()
        {
            if (exited || !processHandle || detached)
                return;
            DWORD ec;
            if (GetExitCodeProcess(processHandle, &ec) && ec != STILL_ACTIVE)
            {
                exited = true;
                exitStatus = static_cast<int>(ec);
            }
        }

        /// Read all available bytes from a pipe handle (non-blocking via PeekNamedPipe).
        static bool ReadAvailable(HANDLE h, std::string& dest)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
                return false;
            if (avail == 0)
                return true; // No data, not EOF

            char buf[4096];
            while (avail > 0)
            {
                DWORD toRead = (avail < sizeof(buf)) ? avail : static_cast<DWORD>(sizeof(buf));
                DWORD bytesRead = 0;
                if (!ReadFile(h, buf, toRead, &bytesRead, NULL) || bytesRead == 0)
                    return false;
                dest.append(buf, bytesRead);
                avail -= bytesRead;
            }
            return true;
        }

        /// Blocking read until the pipe is closed.
        static std::string ReadUntilEof(HANDLE h)
        {
            std::string result;
            char buf[4096];
            for (;;)
            {
                DWORD bytesRead = 0;
                if (!ReadFile(h, buf, sizeof(buf), &bytesRead, NULL) || bytesRead == 0)
                    break;
                result.append(buf, bytesRead);
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

    Process::Builder& Process::Builder::CaptureStdin()
    {
        m_stdinMode = PipeMode::Capture;
        return *this;
    }

    Process::Builder& Process::Builder::Detached()
    {
        m_detached = true;
        return *this;
    }

    std::expected<Process, std::string> Process::Builder::Launch()
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;

        // Create pipes
        AutoHandle stdinReadH, stdinWriteH;
        AutoHandle stdoutReadH, stdoutWriteH;
        AutoHandle stderrReadH, stderrWriteH;

        if (m_stdinMode == PipeMode::Capture)
        {
            HANDLE r, w;
            if (!CreatePipe(&r, &w, &sa, 0))
                return std::unexpected("CreatePipe failed for stdin (error " + std::to_string(GetLastError()) + ")");
            stdinReadH = AutoHandle(r);
            stdinWriteH = AutoHandle(w);
            SetHandleInformation(stdinWriteH.h, HANDLE_FLAG_INHERIT, 0);
        }

        if (m_stdoutMode == PipeMode::Capture)
        {
            HANDLE r, w;
            if (!CreatePipe(&r, &w, &sa, 0))
                return std::unexpected("CreatePipe failed for stdout (error " + std::to_string(GetLastError()) + ")");
            stdoutReadH = AutoHandle(r);
            stdoutWriteH = AutoHandle(w);
            SetHandleInformation(stdoutReadH.h, HANDLE_FLAG_INHERIT, 0);
        }

        if (m_stderrMode == PipeMode::Capture)
        {
            HANDLE r, w;
            if (!CreatePipe(&r, &w, &sa, 0))
                return std::unexpected("CreatePipe failed for stderr (error " + std::to_string(GetLastError()) + ")");
            stderrReadH = AutoHandle(r);
            stderrWriteH = AutoHandle(w);
            SetHandleInformation(stderrReadH.h, HANDLE_FLAG_INHERIT, 0);
        }

        // Build command line string (Windows-style: executable + space-separated
        // args), quoting/escaping each field so it round-trips correctly
        // through CommandLineToArgvW-compatible argv parsing in the child.
        std::string cmdLine;
        AppendQuotedArg(cmdLine, m_executable);
        for (const auto& a : m_args)
        {
            cmdLine += ' ';
            AppendQuotedArg(cmdLine, a);
        }

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        if (m_stdinMode == PipeMode::Capture || m_stdoutMode == PipeMode::Capture || m_stderrMode == PipeMode::Capture)
        {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = stdinReadH.h ? stdinReadH.h : GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = stdoutWriteH.h ? stdoutWriteH.h : GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = stderrWriteH.h ? stderrWriteH.h : GetStdHandle(STD_ERROR_HANDLE);
        }

        // CREATE_SUSPENDED so the child is assigned to the kill-on-close job
        // BEFORE it runs — otherwise it could spawn its own children (which
        // would escape the job) in the gap between create and assign.
        DWORD flags = CREATE_SUSPENDED;
        if (m_detached)
            flags |= CREATE_NO_WINDOW | DETACHED_PROCESS;

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessA(NULL, cmdLine.data(), NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi);
        if (!ok)
            return std::unexpected("CreateProcessA failed (error " + std::to_string(GetLastError()) + ")");

        // Reap the child automatically if this process dies unexpectedly.
        // Best-effort: on the rare platform where the job can't be created or
        // assigned (e.g. an outer job that forbids nesting), fall through — the
        // child simply loses the auto-reap guarantee, same as before.
        if (HANDLE job = GetChildKillJob())
            AssignProcessToJobObject(job, pi.hProcess);

        ResumeThread(pi.hThread);

        // Close child-side handles
        CloseHandle(pi.hThread);
        stdinReadH.Close();
        stdoutWriteH.Close();
        stderrWriteH.Close();

        Process proc;
        proc.m_impl = std::make_unique<Impl>();
        proc.m_impl->processHandle = pi.hProcess;
        proc.m_impl->detached = m_detached;
        proc.m_impl->stdinWrite = stdinWriteH.Release();
        proc.m_impl->stdoutRead = stdoutReadH.Release();
        proc.m_impl->stderrRead = stderrReadH.Release();

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
        if (!m_impl || !m_impl->processHandle || m_impl->detached)
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
        if (!m_impl || !m_impl->processHandle || m_impl->detached)
            return -1;
        if (m_impl->exited)
            return m_impl->exitStatus;

        WaitForSingleObject(m_impl->processHandle, INFINITE);
        DWORD ec;
        GetExitCodeProcess(m_impl->processHandle, &ec);
        m_impl->exited = true;
        m_impl->exitStatus = static_cast<int>(ec);
        return m_impl->exitStatus;
    }

    bool Process::WaitForExit(std::chrono::milliseconds timeout)
    {
        if (!m_impl || !m_impl->processHandle || m_impl->detached)
            return true;
        if (m_impl->exited)
            return true;

        DWORD result = WaitForSingleObject(m_impl->processHandle, static_cast<DWORD>(timeout.count()));
        if (result == WAIT_OBJECT_0)
        {
            DWORD ec;
            GetExitCodeProcess(m_impl->processHandle, &ec);
            m_impl->exited = true;
            m_impl->exitStatus = static_cast<int>(ec);
            return true;
        }
        return false;
    }

    void Process::Kill()
    {
        if (!m_impl || !m_impl->processHandle || m_impl->detached || m_impl->exited)
            return;
        TerminateProcess(m_impl->processHandle, 1);
        WaitForSingleObject(m_impl->processHandle, 5000);
        m_impl->exited = true;
        m_impl->exitStatus = -1;
    }

    void Process::WriteStdin(std::string_view data)
    {
        if (!m_impl || !m_impl->stdinWrite)
            return;
        DWORD written;
        WriteFile(m_impl->stdinWrite, data.data(), static_cast<DWORD>(data.size()), &written, NULL);
    }

    void Process::CloseStdin()
    {
        if (m_impl)
            Impl::CloseH(m_impl->stdinWrite);
    }

    bool Process::TryReadLine(std::string& line)
    {
        if (!m_impl || !m_impl->stdoutRead)
            return false;

        Impl::ReadAvailable(m_impl->stdoutRead, m_impl->stdoutBuffer);

        auto pos = m_impl->stdoutBuffer.find('\n');
        if (pos == std::string::npos)
            return false;

        line = m_impl->stdoutBuffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        m_impl->stdoutBuffer.erase(0, pos + 1);
        return true;
    }

    std::string Process::ReadAllStdout()
    {
        if (!m_impl || !m_impl->stdoutRead)
            return {};
        std::string result = std::move(m_impl->stdoutBuffer);
        result += Impl::ReadUntilEof(m_impl->stdoutRead);
        Impl::CloseH(m_impl->stdoutRead);
        return result;
    }

    std::string Process::ReadAllStderr()
    {
        if (!m_impl || !m_impl->stderrRead)
            return {};
        std::string result = Impl::ReadUntilEof(m_impl->stderrRead);
        Impl::CloseH(m_impl->stderrRead);
        return result;
    }

} // namespace Spark

#endif // SPARK_PLATFORM_WINDOWS
