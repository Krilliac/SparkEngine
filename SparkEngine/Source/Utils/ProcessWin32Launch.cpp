/**
 * @file ProcessWin32Launch.cpp
 * @brief Windows implementation of Spark::Process::Builder (configuration and launch)
 *
 * Uses CreateProcessW for process creation and anonymous Win32 pipes
 * for stdin/stdout/stderr redirection. The shared Process::Impl definition
 * lives in ProcessWin32Internal.h.
 */

#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "Process.h"
#include "ProcessWin32Internal.h"
#include "ProcessWin32JobPolicy.h"

#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
        std::expected<std::wstring, std::string> Utf8ToWide(std::string_view text, std::string_view field)
        {
            if (text.empty())
                return std::wstring{};
            if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                return std::unexpected(std::string(field) + " exceeds the Win32 UTF-8 conversion limit");

            const int inputLength = static_cast<int>(text.size());
            const int outputLength =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), inputLength, nullptr, 0);
            if (outputLength <= 0)
                return std::unexpected(std::string(field) + " is not valid UTF-8 (error " +
                                       std::to_string(GetLastError()) + ")");

            std::wstring result(static_cast<size_t>(outputLength), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), inputLength, result.data(),
                                    outputLength) != outputLength)
                return std::unexpected("failed to convert " + std::string(field) + " to UTF-16 (error " +
                                       std::to_string(GetLastError()) + ")");
            return result;
        }

        void AppendQuotedArg(std::wstring& cmdLine, const std::wstring& arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            {
                // No characters that require quoting.
                cmdLine += arg;
                return;
            }

            cmdLine += L'"';
            for (auto it = arg.begin();; ++it)
            {
                unsigned numBackslashes = 0;
                while (it != arg.end() && *it == L'\\')
                {
                    ++it;
                    ++numBackslashes;
                }

                if (it == arg.end())
                {
                    // Escape all backslashes, since they immediately precede
                    // the closing quote we're about to append.
                    cmdLine.append(numBackslashes * 2, L'\\');
                    break;
                }
                else if (*it == L'"')
                {
                    // Escape all backslashes and the following quote.
                    cmdLine.append(numBackslashes * 2 + 1, L'\\');
                    cmdLine.push_back(*it);
                }
                else
                {
                    // A regular character; backslashes before it are literal.
                    cmdLine.append(numBackslashes, L'\\');
                    cmdLine.push_back(*it);
                }
            }
            cmdLine += L'"';
        }
    } // namespace

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
        auto executable = Utf8ToWide(m_executable, "process executable");
        if (!executable)
            return std::unexpected(executable.error());

        std::vector<std::wstring> arguments;
        arguments.reserve(m_args.size());
        for (const std::string& argument : m_args)
        {
            auto wide = Utf8ToWide(argument, "process argument");
            if (!wide)
                return std::unexpected(wide.error());
            arguments.push_back(std::move(*wide));
        }

        auto workingDirectory = Utf8ToWide(m_workingDirectory, "process working directory");
        if (!workingDirectory)
            return std::unexpected(workingDirectory.error());

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

        if (m_stderrMode == PipeMode::Capture && !m_mergeStderrIntoStdout)
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
        std::wstring cmdLine;
        AppendQuotedArg(cmdLine, *executable);
        for (const auto& argument : arguments)
        {
            cmdLine += L' ';
            AppendQuotedArg(cmdLine, argument);
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (m_stdinMode == PipeMode::Capture || m_stdoutMode == PipeMode::Capture ||
            m_stderrMode == PipeMode::Capture || m_mergeStderrIntoStdout)
        {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = stdinReadH.h ? stdinReadH.h : GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = stdoutWriteH.h ? stdoutWriteH.h : GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = m_mergeStderrIntoStdout ? si.hStdOutput
                                                   : (stderrWriteH.h ? stderrWriteH.h : GetStdHandle(STD_ERROR_HANDLE));
        }

        // CREATE_SUSPENDED so the child is assigned to the kill-on-close job
        // BEFORE it runs — otherwise it could spawn its own children (which
        // would escape the job) in the gap between create and assign.
        DWORD flags = CREATE_SUSPENDED;
        if (m_noWindow)
            flags |= CREATE_NO_WINDOW;
        if (m_detached)
            flags |= CREATE_NO_WINDOW | DETACHED_PROCESS;

        PROCESS_INFORMATION pi{};
        const wchar_t* workingDirectoryValue = workingDirectory->empty() ? nullptr : workingDirectory->c_str();
        BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, flags, nullptr, workingDirectoryValue,
                                 &si, &pi);
        if (!ok)
            return std::unexpected("CreateProcessW failed (error " + std::to_string(GetLastError()) + ")");

        // Reap tracked children automatically if this process dies
        // unexpectedly. Detached means fire-and-forget: assigning those
        // children to this kill-on-close job would silently terminate a
        // SparkDaemon as soon as its launching engine/editor exits.
        // Best-effort: on the rare platform where the job can't be created or
        // assigned (e.g. an outer job that forbids nesting), fall through — the
        // child simply loses the auto-reap guarantee, same as before.
        if (ProcessDetail::ShouldAssignToKillOnCloseJob(m_detached))
        {
            if (HANDLE job = GetChildKillJob())
                AssignProcessToJobObject(job, pi.hProcess);
        }

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

} // namespace Spark

#endif // SPARK_PLATFORM_WINDOWS
