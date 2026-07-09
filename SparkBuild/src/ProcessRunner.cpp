#include "ProcessRunner.h"
#include <cctype>
#include <cstring>
#include <vector>

#ifdef SPARK_PLATFORM_UNIX
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#endif

namespace SparkBuild
{
    namespace
    {
        std::vector<std::string> SplitCommandLine(const std::string& command)
        {
            std::vector<std::string> args;
            std::string current;
            bool inQuotes = false;

            for (size_t i = 0; i < command.size(); ++i)
            {
                char c = command[i];
                if (c == '\\')
                {
                    if (i + 1 < command.size() && (command[i + 1] == '"' || command[i + 1] == '\\'))
                    {
                        current.push_back(command[i + 1]);
                        ++i;
                    }
                    else
                    {
                        current.push_back(c);
                    }
                    continue;
                }

                if (c == '"')
                {
                    inQuotes = !inQuotes;
                    continue;
                }

                if (std::isspace(static_cast<unsigned char>(c)) && !inQuotes)
                {
                    if (!current.empty())
                    {
                        args.push_back(current);
                        current.clear();
                    }
                    continue;
                }

                current.push_back(c);
            }

            if (!current.empty())
                args.push_back(current);

            return args;
        }

#ifdef SPARK_PLATFORM_WINDOWS
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
#endif
    } // namespace

    ProcessRunner::ProcessRunner() {}

    ProcessRunner::~ProcessRunner()
    {
        Cancel();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    bool ProcessRunner::RunAsync(const std::string& command, const std::string& workingDir, OutputCallback onOutput,
                                 CompletionCallback onComplete)
    {
        if (m_running.load())
            return false;

        if (m_thread.joinable())
        {
            m_thread.join();
        }

        m_cancelRequested.store(false);
        m_running.store(true);
        m_thread = std::thread(&ProcessRunner::AsyncThreadFunc, this, command, workingDir, onOutput, onComplete);
        return true;
    }

    void ProcessRunner::Cancel()
    {
        m_cancelRequested.store(true);
        std::lock_guard<std::mutex> lock(m_processMutex);
#ifdef SPARK_PLATFORM_WINDOWS
        if (m_hProcess)
        {
            TerminateProcess(m_hProcess, 1);
        }
#else
        if (m_childPid > 0)
        {
            kill(m_childPid, SIGTERM);
        }
#endif
    }

// ============================================================================
// Windows implementation
// ============================================================================
#ifdef SPARK_PLATFORM_WINDOWS

    int ProcessRunner::RunSync(const std::string& command, const std::string& workingDir, std::string& output)
    {
        std::vector<std::string> args = SplitCommandLine(command);
        if (args.empty())
            return -1;

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
            return -1;
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        std::string cmdLine;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (i > 0)
                cmdLine.push_back(' ');
            cmdLine += QuoteWindowsArgument(args[i]);
        }
        const char* dir = workingDir.empty() ? nullptr : workingDir.c_str();

        BOOL ok =
            CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
        CloseHandle(hWritePipe);

        if (!ok)
        {
            CloseHandle(hReadPipe);
            return -1;
        }

        output.clear();
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0)
        {
            buf[bytesRead] = '\0';
            output += buf;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);

        return static_cast<int>(exitCode);
    }

    void ProcessRunner::AsyncThreadFunc(std::string command, std::string workingDir, OutputCallback onOutput,
                                        CompletionCallback onComplete)
    {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        {
            m_running.store(false);
            if (onComplete)
                onComplete(-1, false);
            return;
        }
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        const char* dir = workingDir.empty() ? nullptr : workingDir.c_str();
        std::string cmdLine = "cmd /c " + command;

        BOOL ok =
            CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
        CloseHandle(hWritePipe);

        if (!ok)
        {
            CloseHandle(hReadPipe);
            m_running.store(false);
            if (onComplete)
                onComplete(-1, false);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_processMutex);
            m_hProcess = pi.hProcess;
        }

        std::string lineBuffer;
        ReadPipeOutput(hReadPipe, onOutput, lineBuffer);

        if (!lineBuffer.empty() && onOutput)
        {
            onOutput(lineBuffer);
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        m_exitCode = static_cast<int>(exitCode);

        {
            std::lock_guard<std::mutex> lock(m_processMutex);
            m_hProcess = nullptr;
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);

        m_running.store(false);
        bool success = (exitCode == 0) && !m_cancelRequested.load();
        if (onComplete)
            onComplete(m_exitCode, success);
    }

    void ProcessRunner::ReadPipeOutput(HANDLE hPipe, OutputCallback& onOutput, std::string& lineBuffer)
    {
        char buf[1024];
        DWORD bytesRead;

        while (!m_cancelRequested.load())
        {
            BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
            if (!ok || bytesRead == 0)
                break;

            buf[bytesRead] = '\0';
            lineBuffer += buf;

            size_t pos;
            while ((pos = lineBuffer.find('\n')) != std::string::npos)
            {
                std::string line = lineBuffer.substr(0, pos);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (onOutput)
                    onOutput(line);
                lineBuffer = lineBuffer.substr(pos + 1);
            }
        }
    }

// ============================================================================
// Unix implementation (Linux / macOS)
// ============================================================================
#else

    int ProcessRunner::RunSync(const std::string& command, const std::string& workingDir, std::string& output)
    {
        std::vector<std::string> args = SplitCommandLine(command);
        if (args.empty())
            return -1;

        int pipefd[2];
        if (pipe(pipefd) != 0)
            return -1;

        pid_t pid = fork();
        if (pid == -1)
        {
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }

        if (pid == 0)
        {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            if (!workingDir.empty() && chdir(workingDir.c_str()) != 0)
                _exit(127);

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (auto& arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            execvp(args[0].c_str(), argv.data());
            _exit(127);
        }

        close(pipefd[1]);
        output.clear();
        char buf[4096];
        while (true)
        {
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            if (n <= 0)
                break;
            buf[n] = '\0';
            output += buf;
        }
        close(pipefd[0]);

        int status = 0;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status))
        {
            return WEXITSTATUS(status);
        }
        return -1;
    }

    void ProcessRunner::AsyncThreadFunc(std::string command, std::string workingDir, OutputCallback onOutput,
                                        CompletionCallback onComplete)
    {
        int pipefd[2];
        if (pipe(pipefd) == -1)
        {
            m_running.store(false);
            if (onComplete)
                onComplete(-1, false);
            return;
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            close(pipefd[0]);
            close(pipefd[1]);
            m_running.store(false);
            if (onComplete)
                onComplete(-1, false);
            return;
        }

        if (pid == 0)
        {
            // Child process
            close(pipefd[0]); // Close read end
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            if (!workingDir.empty())
            {
                if (chdir(workingDir.c_str()) != 0)
                {
                    _exit(127);
                }
            }

            // Execute via shell
            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            _exit(127);
        }

        // Parent process
        close(pipefd[1]); // Close write end

        {
            std::lock_guard<std::mutex> lock(m_processMutex);
            m_childPid = pid;
        }

        std::string lineBuffer;
        ReadPipeOutput(pipefd[0], onOutput, lineBuffer);

        if (!lineBuffer.empty() && onOutput)
        {
            onOutput(lineBuffer);
        }

        close(pipefd[0]);

        int status = 0;
        waitpid(pid, &status, 0);

        {
            std::lock_guard<std::mutex> lock(m_processMutex);
            m_childPid = -1;
        }

        if (WIFEXITED(status))
        {
            m_exitCode = WEXITSTATUS(status);
        }
        else
        {
            m_exitCode = -1;
        }

        m_running.store(false);
        bool success = (m_exitCode == 0) && !m_cancelRequested.load();
        if (onComplete)
            onComplete(m_exitCode, success);
    }

    void ProcessRunner::ReadPipeOutput(int fd, OutputCallback& onOutput, std::string& lineBuffer)
    {
        char buf[1024];

        while (!m_cancelRequested.load())
        {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n <= 0)
                break;

            buf[n] = '\0';
            lineBuffer += buf;

            size_t pos;
            while ((pos = lineBuffer.find('\n')) != std::string::npos)
            {
                std::string line = lineBuffer.substr(0, pos);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (onOutput)
                    onOutput(line);
                lineBuffer = lineBuffer.substr(pos + 1);
            }
        }
    }

#endif // SPARK_PLATFORM_UNIX

} // namespace SparkBuild
