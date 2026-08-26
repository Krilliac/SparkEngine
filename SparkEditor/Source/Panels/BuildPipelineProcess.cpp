/**
 * @file BuildPipelineProcess.cpp
 * @brief Subprocess execution and output parsing for the build pipeline
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: RunCommand, ParseLine, PushLog, and the Windows argument-quoting
 * helper RunCommand uses (QuoteWindowsArgument).
 */

#include "BuildPipeline.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <regex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace SparkEditor
{
#ifdef _WIN32
    namespace
    {
        std::string QuoteWindowsArgument(const std::string& argument)
        {
            if (argument.empty())
                return "\"\"";

            const bool needsQuotes = argument.find_first_of(" \t\"") != std::string::npos;
            if (!needsQuotes)
                return argument;

            std::string quoted = "\"";
            size_t backslashes = 0;
            for (char c : argument)
            {
                if (c == '\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == '"')
                {
                    quoted.append(backslashes * 2 + 1, '\\');
                    quoted.push_back(c);
                }
                else
                {
                    quoted.append(backslashes, '\\');
                    quoted.push_back(c);
                }
                backslashes = 0;
            }
            quoted.append(backslashes * 2, '\\');
            quoted.push_back('"');
            return quoted;
        }
    } // namespace
#endif

    // ========================================================================
    // Subprocess execution
    // ========================================================================

    int BuildPipeline::RunCommand(const std::string& executable, const std::vector<std::string>& arguments)
    {
        if (m_cancelRequested.load())
            return 1;
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        {
            PushLog(BuildLogLine::Level::Error, "Failed to create subprocess pipe");
            return -1;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION process = {};
        std::string commandLine = QuoteWindowsArgument(executable);
        for (const auto& argument : arguments)
        {
            commandLine.push_back(' ');
            commandLine += QuoteWindowsArgument(argument);
        }

        BOOL ok = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
        CloseHandle(writePipe);
        if (!ok)
        {
            CloseHandle(readPipe);
            PushLog(BuildLogLine::Level::Error, "Failed to launch subprocess");
            return -1;
        }

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        DWORD containmentError = ERROR_SUCCESS;
        bool ownsJob = false;
        if (!job)
            containmentError = GetLastError();
        else if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            containmentError = GetLastError();
        else if (!AssignProcessToJobObject(job, process.hProcess))
            containmentError = GetLastError();
        else
            ownsJob = true;

        if (!ownsJob)
        {
            BOOL hostInJob = FALSE;
            BOOL childInJob = FALSE;
            const bool inheritedOuterJob = IsProcessInJob(GetCurrentProcess(), nullptr, &hostInJob) && hostInJob &&
                                           IsProcessInJob(process.hProcess, nullptr, &childInJob) && childInJob;
            if (!inheritedOuterJob)
            {
                TerminateProcess(process.hProcess, 1);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                CloseHandle(readPipe);
                if (job)
                    CloseHandle(job);
                PushLog(BuildLogLine::Level::Error, "Failed to contain subprocess tree in a Windows job");
                return -1;
            }

            if (job)
                CloseHandle(job);
            job = nullptr;
            PushLog(BuildLogLine::Level::Warning,
                    "Subprocess inherited the host Windows job; nested job assignment was unavailable (Win32 " +
                        std::to_string(containmentError) + "). Cancellation will terminate the immediate child.");
        }

        {
            std::lock_guard lock(m_processMutex);
            m_processHandle = process.hProcess;
            m_jobHandle = job;
        }
        ResumeThread(process.hThread);
        if (m_cancelRequested.load())
        {
            if (job)
                TerminateJobObject(job, 1);
            else
                TerminateProcess(process.hProcess, 1);
        }

        std::array<char, 512> buffer{};
        std::string lineAccum;
        DWORD bytesRead = 0;
        while (!m_cancelRequested.load() &&
               ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &bytesRead, nullptr) &&
               bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            lineAccum += buffer.data();
            size_t pos;
            while ((pos = lineAccum.find('\n')) != std::string::npos)
            {
                std::string line = lineAccum.substr(0, pos);
                lineAccum.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                ParseLine(line);
            }
        }

        if (!lineAccum.empty())
            ParseLine(lineAccum);

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);

        {
            std::lock_guard lock(m_processMutex);
            m_processHandle = nullptr;
            m_jobHandle = nullptr;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (job)
            CloseHandle(job);
        CloseHandle(readPipe);

        return static_cast<int>(exitCode);
#else
        // Build the complete argument vector before posix_spawn so process
        // creation does not depend on allocator state copied from other threads.
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);

        int pipefd[2];
        if (pipe(pipefd) != 0)
        {
            PushLog(BuildLogLine::Level::Error, "Failed to create subprocess pipe");
            return -1;
        }

        posix_spawn_file_actions_t fileActions;
        posix_spawnattr_t attributes;
        int spawnError = posix_spawn_file_actions_init(&fileActions);
        bool fileActionsInitialized = spawnError == 0;
        bool attributesInitialized = false;
        if (spawnError == 0)
        {
            spawnError = posix_spawnattr_init(&attributes);
            attributesInitialized = spawnError == 0;
        }
        if (spawnError == 0)
            spawnError = posix_spawn_file_actions_addclose(&fileActions, pipefd[0]);
        if (spawnError == 0)
            spawnError = posix_spawn_file_actions_adddup2(&fileActions, pipefd[1], STDOUT_FILENO);
        if (spawnError == 0)
            spawnError = posix_spawn_file_actions_adddup2(&fileActions, pipefd[1], STDERR_FILENO);
        if (spawnError == 0)
            spawnError = posix_spawn_file_actions_addclose(&fileActions, pipefd[1]);
        if (spawnError == 0)
            spawnError = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (spawnError == 0)
            spawnError = posix_spawnattr_setpgroup(&attributes, 0);

        pid_t pid = 0;
        if (spawnError == 0)
            spawnError = posix_spawnp(&pid, executable.c_str(), &fileActions, &attributes, argv.data(), environ);

        if (attributesInitialized)
            posix_spawnattr_destroy(&attributes);
        if (fileActionsInitialized)
            posix_spawn_file_actions_destroy(&fileActions);
        close(pipefd[1]);

        if (spawnError != 0)
        {
            close(pipefd[0]);
            PushLog(BuildLogLine::Level::Error,
                    "Failed to launch subprocess '" + executable + "': " + std::strerror(spawnError));
            return -1;
        }
        const int pipeFlags = fcntl(pipefd[0], F_GETFL, 0);
        if (pipeFlags == -1 || fcntl(pipefd[0], F_SETFL, pipeFlags | O_NONBLOCK) == -1)
        {
            kill(-pid, SIGKILL);
            int status = 0;
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
            {
            }
            close(pipefd[0]);
            PushLog(BuildLogLine::Level::Error, "Failed to configure subprocess output pipe");
            return -1;
        }
        {
            std::lock_guard lock(m_processMutex);
            m_childPid = pid;
        }

        std::array<char, 512> buffer{};
        std::string lineAccum;
        bool pipeOpen = true;
        bool childExited = false;
        bool waitFailed = false;
        bool terminationStarted = false;
        int status = 0;
        std::chrono::steady_clock::time_point terminationDeadline{};
        constexpr auto cancellationGrace = std::chrono::seconds(2);

        const auto consumeCompleteLines = [&]()
        {
            size_t pos;
            while ((pos = lineAccum.find('\n')) != std::string::npos)
            {
                std::string line = lineAccum.substr(0, pos);
                lineAccum.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                ParseLine(line);
            }
        };

        const auto drainAvailableOutput = [&]()
        {
            while (pipeOpen)
            {
                const ssize_t bytesRead = read(pipefd[0], buffer.data(), buffer.size());
                if (bytesRead > 0)
                {
                    lineAccum.append(buffer.data(), static_cast<size_t>(bytesRead));
                    consumeCompleteLines();
                    continue;
                }
                if (bytesRead == 0)
                {
                    pipeOpen = false;
                    break;
                }
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                pipeOpen = false;
            }
        };

        while (!childExited)
        {
            drainAvailableOutput();

            pid_t waitResult;
            do
            {
                waitResult = waitpid(pid, &status, WNOHANG);
            } while (waitResult == -1 && errno == EINTR);
            if (waitResult == pid)
            {
                childExited = true;
                break;
            }
            if (waitResult == -1)
            {
                waitFailed = true;
                break;
            }

            if (m_cancelRequested.load())
            {
                if (!terminationStarted)
                {
                    kill(-pid, SIGTERM);
                    terminationStarted = true;
                    terminationDeadline = std::chrono::steady_clock::now() + cancellationGrace;
                }
                else if (std::chrono::steady_clock::now() >= terminationDeadline)
                {
                    kill(-pid, SIGKILL);
                    do
                    {
                        waitResult = waitpid(pid, &status, 0);
                    } while (waitResult == -1 && errno == EINTR);
                    childExited = waitResult == pid;
                    waitFailed = !childExited;
                    break;
                }
            }

            pollfd outputPoll{pipefd[0], POLLIN | POLLHUP, 0};
            const int pollResult = poll(pipeOpen ? &outputPoll : nullptr, pipeOpen ? 1 : 0, 50);
            if (pollResult == -1 && errno != EINTR)
            {
                kill(-pid, SIGKILL);
                do
                {
                    waitResult = waitpid(pid, &status, 0);
                } while (waitResult == -1 && errno == EINTR);
                childExited = waitResult == pid;
                waitFailed = !childExited;
                break;
            }
        }

        drainAvailableOutput();
        if (!lineAccum.empty())
            ParseLine(lineAccum);

        close(pipefd[0]);
        {
            std::lock_guard lock(m_processMutex);
            m_childPid = 0;
        }

        return !waitFailed && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    // ========================================================================
    // Output parsing
    // ========================================================================

    void BuildPipeline::ParseLine(const std::string& line)
    {
        // Detect CMake build progress: [  X%] or [ X/Y]
        static const std::regex progressPct(R"(\[\s*(\d+)%\])");
        static const std::regex progressFrac(R"(\[\s*(\d+)/(\d+)\])");

        std::smatch match;
        if (std::regex_search(line, match, progressPct))
        {
            float pct = std::stof(match[1].str()) / 100.0f;
            m_progress.store(m_cooking.load() ? 0.10f + pct * 0.85f : 0.15f + pct * 0.80f);
        }
        else if (std::regex_search(line, match, progressFrac))
        {
            float current = std::stof(match[1].str());
            float total = std::stof(match[2].str());
            if (total > 0.0f)
            {
                float pct = current / total;
                m_progress.store(m_cooking.load() ? 0.10f + pct * 0.85f : 0.15f + pct * 0.80f);

                std::lock_guard lk(m_statusMutex);
                m_statusText = (m_cooking.load() ? "Cooking assets [" : "Compiling [") + match[1].str() + "/" +
                               match[2].str() + "]";
            }
        }

        // Classify severity
        BuildLogLine::Level level = BuildLogLine::Level::Info;
        if (line.find("error") != std::string::npos || line.find("Error") != std::string::npos ||
            line.find("FAILED") != std::string::npos)
        {
            level = BuildLogLine::Level::Error;
        }
        else if (line.find("warning") != std::string::npos || line.find("Warning") != std::string::npos)
        {
            level = BuildLogLine::Level::Warning;
        }

        PushLog(level, line);
    }

    void BuildPipeline::PushLog(BuildLogLine::Level level, std::string text)
    {
        std::lock_guard lk(m_logMutex);
        m_logQueue.push_back({level, std::move(text)});
    }

} // namespace SparkEditor
