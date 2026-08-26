/**
 * @file ProcessWin32.cpp
 * @brief Windows implementation of Spark::Process
 *
 * Uses CreateProcessW for process creation and anonymous Win32 pipes
 * for stdin/stdout/stderr redirection. PeekNamedPipe provides non-blocking reads.
 * Builder configuration and launch live in ProcessWin32Launch.cpp; the shared
 * Process::Impl definition lives in ProcessWin32Internal.h.
 */

#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "Process.h"
#include "ProcessWin32Internal.h"

#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace Spark
{

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

        if (ProcessDetail::ExtractBufferedLine(m_impl->stdoutBuffer, line))
            return true;

        Impl::ReadAvailable(m_impl->stdoutRead, m_impl->stdoutBuffer);
        return ProcessDetail::ExtractBufferedLine(m_impl->stdoutBuffer, line);
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
