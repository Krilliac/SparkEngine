/**
 * @file Process.h
 * @brief Cross-platform subprocess spawning with optional stdin/stdout/stderr piping.
 *
 * Provides a general-purpose RAII Process class for launching child processes,
 * capturing their output, and managing their lifecycle. Platform-specific
 * implementations live in ProcessWin32.cpp, ProcessLinux.cpp, and ProcessStub.cpp.
 *
 * Usage:
 * @code
 *   auto proc = Spark::Process::Builder("/usr/bin/echo")
 *       .Arg("hello world")
 *       .CaptureStdout()
 *       .Launch();
 *   if (proc)
 *   {
 *       std::string output = proc->ReadAllStdout();
 *       int exitCode = proc->WaitForExit();
 *   }
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    /// Controls how a standard stream is handled for the child process.
    enum class PipeMode
    {
        None,    ///< Stream is not redirected (inherits parent's or goes to null).
        Capture, ///< Stream is captured via a pipe for reading/writing by the parent.
        Inherit  ///< Stream is explicitly inherited from the parent process.
    };

    /**
     * @brief RAII wrapper for a child process with optional piped I/O.
     *
     * Move-only. The destructor terminates the child if still running and
     * closes all pipe handles/file descriptors.
     */
    class Process
    {
      public:
        /**
         * @brief Fluent builder for configuring and launching a child process.
         */
        class Builder
        {
          public:
            /// @param executable Path or name of the executable to launch.
            explicit Builder(std::string executable);

            /// Append a command-line argument.
            Builder& Arg(std::string arg);

            /// Set the child process working directory. An empty path inherits
            /// the parent's current directory.
            Builder& WorkingDirectory(std::string directory);

            /// Capture the child's stdout so it can be read via ReadAllStdout()/TryReadLine().
            Builder& CaptureStdout();

            /// Capture the child's stderr so it can be read via ReadAllStderr().
            Builder& CaptureStderr();

            /// Route the child's stderr to its stdout destination. When stdout
            /// is captured, both streams are drained through TryReadLine() or
            /// ReadAllStdout(), avoiding deadlocks caused by an unread stderr pipe.
            /// This takes precedence over CaptureStderr().
            Builder& MergeStderrIntoStdout();

            /// Enable writing to the child's stdin via WriteStdin()/CloseStdin().
            Builder& CaptureStdin();

            /// Launch as a detached (fire-and-forget) process. The Process object
            /// will not own or track the child after launch.
            Builder& Detached();

            /// Launch the configured child process.
            /// @return The running Process, or an error string on failure.
            std::expected<Process, std::string> Launch();

          private:
            std::string m_executable;
            std::string m_workingDirectory;
            std::vector<std::string> m_args;
            PipeMode m_stdinMode = PipeMode::None;
            PipeMode m_stdoutMode = PipeMode::None;
            PipeMode m_stderrMode = PipeMode::None;
            bool m_mergeStderrIntoStdout = false;
            bool m_detached = false;
        };

        ~Process();

        Process(Process&& other) noexcept;
        Process& operator=(Process&& other) noexcept;

        Process(const Process&) = delete;
        Process& operator=(const Process&) = delete;

        // -- Lifecycle -----------------------------------------------------------

        /// @return true if the child process is still running.
        bool IsRunning() const;

        /// @return The exit code if the child has exited, or std::nullopt if still running.
        std::optional<int> GetExitCode() const;

        /// Block until the child exits. @return The exit code.
        int WaitForExit();

        /// Block until the child exits or the timeout expires.
        /// @return true if the child exited within the timeout, false on timeout.
        bool WaitForExit(std::chrono::milliseconds timeout);

        /// Forcefully terminate the child process.
        void Kill();

        // -- Pipe I/O ------------------------------------------------------------

        /// Write data to the child's stdin (requires CaptureStdin).
        void WriteStdin(std::string_view data);

        /// Close the write end of the stdin pipe, signaling EOF to the child.
        void CloseStdin();

        /// Non-blocking read of one line from the child's stdout (requires CaptureStdout).
        /// @return true if a line was read into @p line.
        bool TryReadLine(std::string& line);

        /// Block and read all remaining stdout from the child (requires CaptureStdout).
        std::string ReadAllStdout();

        /// Block and read all remaining stderr from the child (requires CaptureStderr).
        std::string ReadAllStderr();

      private:
        Process();

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace Spark
