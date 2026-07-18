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
#include <regex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
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

        BOOL ok = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &process);
        CloseHandle(writePipe);
        if (!ok)
        {
            CloseHandle(readPipe);
            PushLog(BuildLogLine::Level::Error, "Failed to launch subprocess");
            return -1;
        }

        {
            std::lock_guard lock(m_statusMutex);
            m_processHandle = process.hProcess;
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
            std::lock_guard lock(m_statusMutex);
            m_processHandle = nullptr;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);

        return static_cast<int>(exitCode);
#else
        int pipefd[2];
        if (pipe(pipefd) != 0)
        {
            PushLog(BuildLogLine::Level::Error, "Failed to create subprocess pipe");
            return -1;
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            close(pipefd[0]);
            close(pipefd[1]);
            PushLog(BuildLogLine::Level::Error, "Failed to fork subprocess");
            return -1;
        }

        if (pid == 0)
        {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            std::vector<char*> argv;
            argv.reserve(arguments.size() + 2);
            argv.push_back(const_cast<char*>(executable.c_str()));
            for (const auto& argument : arguments)
                argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);

            execvp(executable.c_str(), argv.data());
            _exit(127);
        }

        close(pipefd[1]);
        m_childPid = pid;

        std::array<char, 512> buffer{};
        std::string lineAccum;

        while (!m_cancelRequested.load())
        {
            ssize_t bytesRead = read(pipefd[0], buffer.data(), buffer.size() - 1);
            if (bytesRead <= 0)
                break;

            buffer[bytesRead] = '\0';
            lineAccum += buffer.data();
            // Flush complete lines
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

        // Flush remainder
        if (!lineAccum.empty())
            ParseLine(lineAccum);

        close(pipefd[0]);

        int status = 0;
        waitpid(pid, &status, 0);
        m_childPid = 0;

        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
            // Map compile progress to [0.15, 0.95] range (configure takes 0-0.15)
            m_progress.store(0.15f + pct * 0.80f);
        }
        else if (std::regex_search(line, match, progressFrac))
        {
            float current = std::stof(match[1].str());
            float total = std::stof(match[2].str());
            if (total > 0.0f)
            {
                float pct = current / total;
                m_progress.store(0.15f + pct * 0.80f);

                std::lock_guard lk(m_statusMutex);
                m_statusText = "Compiling [" + match[1].str() + "/" + match[2].str() + "]";
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
