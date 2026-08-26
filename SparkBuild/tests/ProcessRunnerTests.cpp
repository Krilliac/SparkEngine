#include "ProcessRunner.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    using namespace std::chrono_literals;

    struct ExactChild
    {
#ifdef SPARK_PLATFORM_WINDOWS
        HANDLE process = nullptr;
        DWORD pid = 0;
#else
        pid_t pid = -1;
#endif
    };

    uint64_t CurrentProcessIdValue()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return static_cast<uint64_t>(::GetCurrentProcessId());
#else
        return static_cast<uint64_t>(::getpid());
#endif
    }

    uint64_t CurrentSteadyTick()
    {
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    std::filesystem::path UniqueSentinelPath(const char* role)
    {
        return std::filesystem::temp_directory_path() /
               (std::string("sparkbuild-process-runner-") + role + "-" + std::to_string(CurrentProcessIdValue()) + "-" +
                std::to_string(CurrentSteadyTick()) + ".pid");
    }

    bool WritePidFile(const std::filesystem::path& path)
    {
        std::ofstream file(path, std::ios::trunc);
        file << CurrentProcessIdValue() << '\n';
        return file.good();
    }

    uint64_t WaitForPidFile(const std::filesystem::path& path, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::ifstream file(path);
            uint64_t pid = 0;
            if (file >> pid; pid > 1)
                return pid;
            std::this_thread::sleep_for(10ms);
        }
        return 0;
    }

#ifdef SPARK_PLATFORM_WINDOWS
    std::string QuoteNativeArgument(const std::string& argument)
    {
        std::string quoted = "\"";
        size_t backslashes = 0;
        for (char character : argument)
        {
            if (character == '\\')
            {
                ++backslashes;
                continue;
            }
            if (character == '"')
            {
                quoted.append(backslashes * 2 + 1, '\\');
                quoted.push_back(character);
            }
            else
            {
                quoted.append(backslashes, '\\');
                quoted.push_back(character);
            }
            backslashes = 0;
        }
        quoted.append(backslashes * 2, '\\');
        quoted.push_back('"');
        return quoted;
    }

    std::string BuildRunnerCommand(const std::filesystem::path& executable, const std::filesystem::path& pidFile)
    {
        // ProcessRunner prefixes `cmd /c`; the extra outer quotes preserve a
        // quoted executable path through cmd.exe's first/last-quote rules.
        return "\"" + QuoteNativeArgument(executable.string()) + " --spawn-descendant " +
               QuoteNativeArgument(pidFile.string()) + "\"";
    }

    std::string BuildSuccessCommand(const std::filesystem::path& executable)
    {
        return "\"" + QuoteNativeArgument(executable.string()) + " --exit-success\"";
    }
#else
    std::string QuoteShellArgument(const std::string& argument)
    {
        std::string quoted = "'";
        for (char character : argument)
        {
            if (character == '\'')
                quoted += "'\\''";
            else
                quoted.push_back(character);
        }
        quoted.push_back('\'');
        return quoted;
    }

    std::string BuildRunnerCommand(const std::filesystem::path& executable, const std::filesystem::path& pidFile)
    {
        return QuoteShellArgument(executable.string()) + " --spawn-descendant " + QuoteShellArgument(pidFile.string());
    }

    std::string BuildSuccessCommand(const std::filesystem::path& executable)
    {
        return QuoteShellArgument(executable.string()) + " --exit-success";
    }
#endif

    ExactChild SpawnExactChild(const std::filesystem::path& executable, const char* mode,
                               const std::filesystem::path& pidFile)
    {
        ExactChild child;
#ifdef SPARK_PLATFORM_WINDOWS
        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        std::string command =
            QuoteNativeArgument(executable.string()) + " " + mode + " " + QuoteNativeArgument(pidFile.string());
        if (!::CreateProcessA(executable.string().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &startup, &process))
            return child;
        ::CloseHandle(process.hThread);
        child.process = process.hProcess;
        child.pid = process.dwProcessId;
#else
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            ::execl(executable.string().c_str(), executable.string().c_str(), mode, pidFile.string().c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);
        }
        if (pid > 0)
            child.pid = pid;
#endif
        return child;
    }

    bool ExactChildIsValid(const ExactChild& child)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return child.process != nullptr && child.pid > 1;
#else
        return child.pid > 1;
#endif
    }

    bool ExactChildIsAlive(const ExactChild& child)
    {
        if (!ExactChildIsValid(child))
            return false;
#ifdef SPARK_PLATFORM_WINDOWS
        return ::WaitForSingleObject(child.process, 0) == WAIT_TIMEOUT;
#else
        if (::kill(child.pid, 0) == 0)
            return true;
        return errno == EPERM;
#endif
    }

    bool ProcessIdIsAlive(uint64_t pid)
    {
        if (pid <= 1)
            return false;
#ifdef SPARK_PLATFORM_WINDOWS
        HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (!process)
            return false;
        const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        ::CloseHandle(process);
        return alive;
#else
        if (::kill(static_cast<pid_t>(pid), 0) == 0)
            return true;
        return errno == EPERM;
#endif
    }

    void StopExactChild(ExactChild& child)
    {
        if (!ExactChildIsValid(child))
            return;
#ifdef SPARK_PLATFORM_WINDOWS
        if (::WaitForSingleObject(child.process, 0) == WAIT_TIMEOUT)
            (void)::TerminateProcess(child.process, 0);
        (void)::WaitForSingleObject(child.process, 5000);
        ::CloseHandle(child.process);
        child.process = nullptr;
        child.pid = 0;
#else
        (void)::kill(child.pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const pid_t waited = ::waitpid(child.pid, &status, WNOHANG);
            if (waited == child.pid || (waited < 0 && errno == ECHILD))
            {
                child.pid = -1;
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        (void)::kill(child.pid, SIGKILL);
        while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR)
        {
        }
        child.pid = -1;
#endif
    }

    int SentinelMain(const std::filesystem::path& pidFile)
    {
        if (!WritePidFile(pidFile))
            return 2;
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(100ms);
        return 0;
    }

    int DescendantSpawnerMain(const std::filesystem::path& executable, const std::filesystem::path& pidFile)
    {
        ExactChild descendant = SpawnExactChild(executable, "--sentinel", pidFile);
        if (!ExactChildIsValid(descendant))
            return 3;
#ifdef SPARK_PLATFORM_WINDOWS
        (void)::WaitForSingleObject(descendant.process, INFINITE);
        ::CloseHandle(descendant.process);
#else
        int status = 0;
        while (::waitpid(descendant.pid, &status, 0) < 0 && errno == EINTR)
        {
        }
#endif
        return 0;
    }

    int RunCancellationTreeTest(const std::filesystem::path& executable)
    {
        int failures = 0;
        auto check = [&failures](bool condition, const char* message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };

        const auto descendantPidFile = UniqueSentinelPath("descendant");
        const auto unrelatedPidFile = UniqueSentinelPath("unrelated");
        std::error_code ignored;
        std::filesystem::remove(descendantPidFile, ignored);
        std::filesystem::remove(unrelatedPidFile, ignored);

        ExactChild unrelated = SpawnExactChild(executable, "--sentinel", unrelatedPidFile);
        check(ExactChildIsValid(unrelated), "could not launch unrelated sentinel");
        const uint64_t unrelatedPid = WaitForPidFile(unrelatedPidFile, 5s);
        check(unrelatedPid > 1, "unrelated sentinel did not become ready");

        std::mutex completionMutex;
        std::condition_variable completionCondition;
        int completionCount = 0;
        int completionExitCode = 0;
        bool completionSuccess = true;

        SparkBuild::ProcessRunner runner;
        const bool launched = runner.RunAsync(
            BuildRunnerCommand(executable, descendantPidFile), {}, [](const std::string&) {},
            [&](int exitCode, bool success)
            {
                {
                    std::lock_guard<std::mutex> lock(completionMutex);
                    ++completionCount;
                    completionExitCode = exitCode;
                    completionSuccess = success;
                }
                completionCondition.notify_all();
            });
        check(launched, "ProcessRunner rejected the async launch");

        const uint64_t descendantPid = WaitForPidFile(descendantPidFile, 5s);
        check(descendantPid > 1, "descendant sentinel did not become ready");
        check(ProcessIdIsAlive(descendantPid), "descendant sentinel was not alive before cancellation");
        check(ExactChildIsAlive(unrelated), "unrelated sentinel exited before cancellation");

        runner.Cancel();
        {
            std::unique_lock<std::mutex> lock(completionMutex);
            check(completionCondition.wait_for(lock, 8s, [&] { return completionCount > 0; }),
                  "completion callback did not run after cancellation");
        }

        const auto descendantDeadline = std::chrono::steady_clock::now() + 3s;
        while (ProcessIdIsAlive(descendantPid) && std::chrono::steady_clock::now() < descendantDeadline)
            std::this_thread::sleep_for(10ms);

        {
            std::lock_guard<std::mutex> lock(completionMutex);
            check(completionCount == 1, "completion callback did not run exactly once");
            check(!completionSuccess, "cancelled process reported success");
            check(completionExitCode != 0, "cancelled process reported a zero exit code");
        }
        check(!runner.IsRunning(), "runner still reports running after completion callback");
        check(!ProcessIdIsAlive(descendantPid), "cancel left a descendant process alive");
        check(ExactChildIsAlive(unrelated), "cancel killed an unrelated process");

        StopExactChild(unrelated);
        std::filesystem::remove(descendantPidFile, ignored);
        std::filesystem::remove(unrelatedPidFile, ignored);
        return failures == 0 ? 0 : 1;
    }

    int RunCompletionReentryTest(const std::filesystem::path& executable)
    {
        std::mutex completionMutex;
        std::condition_variable completionCondition;
        int completionCount = 0;
        bool allSuccessful = true;
        bool rerunAttempted = false;
        bool rerunAccepted = false;

        SparkBuild::ProcessRunner runner;
        const std::string command = BuildSuccessCommand(executable);
        SparkBuild::CompletionCallback completion;
        completion = [&](int exitCode, bool success)
        {
            bool startAgain = false;
            {
                std::lock_guard<std::mutex> lock(completionMutex);
                ++completionCount;
                allSuccessful = allSuccessful && success && exitCode == 0;
                startAgain = completionCount == 1;
            }

            if (startAgain)
            {
                const bool accepted = runner.RunAsync(command, {}, [](const std::string&) {}, completion);
                {
                    std::lock_guard<std::mutex> lock(completionMutex);
                    rerunAttempted = true;
                    rerunAccepted = accepted;
                }
            }
            completionCondition.notify_all();
        };

        if (!runner.RunAsync(command, {}, [](const std::string&) {}, completion))
        {
            std::cerr << "FAIL: ProcessRunner rejected the initial reentry launch\n";
            return 1;
        }

        bool completed = false;
        {
            std::unique_lock<std::mutex> lock(completionMutex);
            completed = completionCondition.wait_for(
                lock, 8s, [&] { return completionCount == 2 || (rerunAttempted && !rerunAccepted); });
        }

        int failures = 0;
        auto check = [&failures](bool condition, const char* message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };
        std::lock_guard<std::mutex> lock(completionMutex);
        check(completed, "completion callback reentry timed out");
        check(rerunAttempted, "completion callback did not attempt a second run");
        check(rerunAccepted, "ProcessRunner rejected a run from its completion callback");
        check(completionCount == 2, "reentrant completion callbacks did not run exactly once each");
        check(allSuccessful, "a reentrant ProcessRunner run reported failure");
        check(!runner.IsRunning(), "runner still reports running after the reentrant completion callback");
        return failures == 0 ? 0 : 1;
    }

    int RunCompletionOwnedDestructionTest(const std::filesystem::path& executable)
    {
        std::mutex completionMutex;
        std::condition_variable completionCondition;
        bool destroyed = false;
        bool successful = false;
        auto runner = std::make_unique<SparkBuild::ProcessRunner>();

        const bool accepted = runner->RunAsync(
            BuildSuccessCommand(executable), {}, [](const std::string&) {},
            [&](int exitCode, bool success)
            {
                successful = success && exitCode == 0;
                runner.reset();
                {
                    std::lock_guard<std::mutex> lock(completionMutex);
                    destroyed = true;
                }
                completionCondition.notify_all();
            });
        if (!accepted)
        {
            std::cerr << "FAIL: ProcessRunner rejected the callback-destruction launch\n";
            return 1;
        }

        std::unique_lock<std::mutex> lock(completionMutex);
        if (!completionCondition.wait_for(lock, 8s, [&] { return destroyed; }))
        {
            std::cerr << "FAIL: destroying ProcessRunner in its completion callback deadlocked\n";
            return 1;
        }
        if (!successful || runner)
        {
            std::cerr << "FAIL: callback-owned ProcessRunner destruction did not complete successfully\n";
            return 1;
        }
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path executable = std::filesystem::absolute(argv[0]);
    if (argc == 3 && std::string(argv[1]) == "--sentinel")
        return SentinelMain(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--spawn-descendant")
        return DescendantSpawnerMain(executable, argv[2]);
    if (argc == 2 && std::string(argv[1]) == "--exit-success")
        return 0;
    if (argc != 1)
        return 64;
    const int cancellationResult = RunCancellationTreeTest(executable);
    const int reentryResult = RunCompletionReentryTest(executable);
    const int destructionResult = RunCompletionOwnedDestructionTest(executable);
    return cancellationResult == 0 && reentryResult == 0 && destructionResult == 0 ? 0 : 1;
}
