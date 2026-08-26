/**
 * @file ProcessStub.cpp
 * @brief Fallback stubs for platforms without subprocess support.
 *
 * All operations return errors or no-ops. This allows the engine to compile
 * on platforms where process spawning is not yet implemented.
 */

#include "Core/Platform.h"

#if !defined(SPARK_PLATFORM_WINDOWS) && !defined(SPARK_PLATFORM_LINUX) && !defined(SPARK_PLATFORM_MACOS)

#include "Process.h"

namespace Spark
{

    struct Process::Impl
    {
    };

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
    Process::Builder& Process::Builder::Detached()
    {
        m_detached = true;
        return *this;
    }
    std::expected<Process, std::string> Process::Builder::Launch()
    {
        return std::unexpected(std::string("Process spawning not supported on this platform"));
    }

    Process::Process() = default;
    Process::~Process() = default;
    Process::Process(Process&&) noexcept = default;
    Process& Process::operator=(Process&&) noexcept = default;

    bool Process::IsRunning() const
    {
        return false;
    }
    std::optional<int> Process::GetExitCode() const
    {
        return std::nullopt;
    }
    int Process::WaitForExit()
    {
        return -1;
    }
    bool Process::WaitForExit(std::chrono::milliseconds)
    {
        return true;
    }
    void Process::Kill() {}
    void Process::WriteStdin(std::string_view) {}
    void Process::CloseStdin() {}
    bool Process::TryReadLine(std::string&)
    {
        return false;
    }
    std::string Process::ReadAllStdout()
    {
        return {};
    }
    std::string Process::ReadAllStderr()
    {
        return {};
    }

} // namespace Spark

#endif
