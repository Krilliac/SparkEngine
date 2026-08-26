#pragma once

#include "Platform.h"
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace SparkBuild
{

    using OutputCallback = std::function<void(const std::string& line)>;
    using CompletionCallback = std::function<void(int exitCode, bool success)>;

    class ProcessRunner
    {
      public:
        ProcessRunner();
        ~ProcessRunner();

        bool RunAsync(const std::string& command, const std::string& workingDir, OutputCallback onOutput,
                      CompletionCallback onComplete);

        int RunSync(const std::string& command, const std::string& workingDir, std::string& output);

        void Cancel();
        bool IsRunning() const { return m_running.load(); }
        int GetExitCode() const { return m_exitCode; }

      private:
        void AsyncThreadFunc(std::string command, std::string workingDir, OutputCallback onOutput,
                             CompletionCallback onComplete);
        void CompleteAsync(int exitCode, bool success, const CompletionCallback& onComplete);

        std::thread m_thread;
        std::mutex m_threadMutex;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_cancelRequested{false};
        std::atomic<bool> m_shuttingDown{false};
        int m_exitCode = 0;
        std::mutex m_processMutex;

#ifdef SPARK_PLATFORM_WINDOWS
        HANDLE m_hProcess = nullptr;
        HANDLE m_hJob = nullptr;
        void ReadPipeOutput(HANDLE hPipe, OutputCallback& onOutput, std::string& lineBuffer);
#else
        pid_t m_childPid = -1;
        void ReadPipeOutput(int fd, OutputCallback& onOutput, std::string& lineBuffer);
#endif
    };

} // namespace SparkBuild
