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

        // Build command line string (Windows-style: executable + space-separated args)
        std::string cmdLine = "\"" + m_executable + "\"";
        for (const auto& a : m_args)
        {
            cmdLine += " \"";
            cmdLine += a;
            cmdLine += "\"";
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

        DWORD flags = 0;
        if (m_detached)
            flags |= CREATE_NO_WINDOW | DETACHED_PROCESS;

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessA(NULL, cmdLine.data(), NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi);
        if (!ok)
            return std::unexpected("CreateProcessA failed (error " + std::to_string(GetLastError()) + ")");

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
