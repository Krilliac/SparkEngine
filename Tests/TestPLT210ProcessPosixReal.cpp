/**
 * @file TestPLT210ProcessPosixReal.cpp
 * @brief PLT-210 Linux defects in the production POSIX Spark::Process (ProcessLinux.cpp).
 *
 * Each test drives the real fork/exec implementation and pins a Linux-only
 * behaviour that diverged from the Windows CreateProcess path:
 *  - a missing executable or working directory must fail Launch() instead of
 *    "succeeding" with a child that immediately exits 127/126;
 *  - writing to the stdin of a child that has exited must not raise SIGPIPE in
 *    the launching process (default action: terminate the editor/tool/runner);
 *  - a detached child must not be left as a zombie of the launching process.
 */

#include "TestFramework.h"
#include "Utils/Process.h"

#if defined(__linux__)

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace
{
    /// Count zombie children of this process whose command name is @p comm.
    int CountZombieChildren(const std::string& comm)
    {
        const pid_t self = getpid();
        int zombies = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", ec))
        {
            std::ifstream statFile(entry.path() / "stat");
            std::string stat;
            if (!statFile || !std::getline(statFile, stat))
                continue;

            // Format: "pid (comm) state ppid ...". comm may contain spaces, so split on the last ')'.
            const size_t open = stat.find('(');
            const size_t close = stat.rfind(')');
            if (open == std::string::npos || close == std::string::npos || close + 4 >= stat.size())
                continue;
            const std::string name = stat.substr(open + 1, close - open - 1);
            const char state = stat[close + 2];
            const long ppid = std::strtol(stat.c_str() + close + 4, nullptr, 10);
            if (name == comm && state == 'Z' && ppid == static_cast<long>(self))
                ++zombies;
        }
        return zombies;
    }
} // namespace

TEST(PLT210_ProcessPosix_MissingExecutableFailsLaunch)
{
    auto result = Spark::Process::Builder("/nonexistent/plt210/binary").Arg("x").Launch();
    ASSERT_FALSE(result.has_value());
    EXPECT_STR_CONTAINS(result.error(), "exec()");
    EXPECT_STR_CONTAINS(result.error(), "/nonexistent/plt210/binary");
}

TEST(PLT210_ProcessPosix_MissingWorkingDirectoryFailsLaunch)
{
    auto result =
        Spark::Process::Builder("/bin/true").WorkingDirectory("/nonexistent/plt210/dir").CaptureStdout().Launch();
    ASSERT_FALSE(result.has_value());
    EXPECT_STR_CONTAINS(result.error(), "chdir()");
    EXPECT_STR_CONTAINS(result.error(), "/nonexistent/plt210/dir");
}

TEST(PLT210_ProcessPosix_DetachedMissingExecutableFailsLaunch)
{
    auto result = Spark::Process::Builder("/nonexistent/plt210/daemon").Detached().Launch();
    ASSERT_FALSE(result.has_value());
    EXPECT_STR_CONTAINS(result.error(), "exec()");
}

TEST(PLT210_ProcessPosix_ValidLaunchStillSucceeds)
{
    auto result = Spark::Process::Builder("/bin/sh").Arg("-c").Arg("echo plt210").CaptureStdout().Launch();
    ASSERT_TRUE(result.has_value());
    EXPECT_STR_CONTAINS(result->ReadAllStdout(), "plt210");
    EXPECT_EQ(result->WaitForExit(), 0);
}

TEST(PLT210_ProcessPosix_WriteToExitedChildDoesNotRaiseSigpipe)
{
    // Default SIGPIPE disposition, as in the editor and the test runner.
    struct sigaction previousAction
    {
    };
    struct sigaction defaultAction
    {
    };
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset(&defaultAction.sa_mask);
    ASSERT_EQ(sigaction(SIGPIPE, &defaultAction, &previousAction), 0);

    sigset_t maskBefore;
    pthread_sigmask(SIG_SETMASK, nullptr, &maskBefore);

    auto result = Spark::Process::Builder("/bin/true").CaptureStdin().Launch();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->WaitForExit(), 0);

    // The reader is gone; before the fix this write killed the whole process.
    result->WriteStdin(std::string(256 * 1024, 'x'));
    result->WriteStdin("second write after EPIPE\n");

    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    EXPECT_EQ(sigismember(&pending, SIGPIPE), 0);

    sigset_t maskAfter;
    pthread_sigmask(SIG_SETMASK, nullptr, &maskAfter);
    EXPECT_EQ(sigismember(&maskAfter, SIGPIPE), sigismember(&maskBefore, SIGPIPE));

    sigaction(SIGPIPE, &previousAction, nullptr);
}

TEST(PLT210_ProcessPosix_DetachedChildLeavesNoZombie)
{
    const int zombiesBefore = CountZombieChildren("sleep");
    auto result = Spark::Process::Builder("/bin/sleep").Arg("0").Detached().Launch();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->IsRunning());

    // Give the detached child ample time to exit; a direct child would now be a zombie of this process.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(CountZombieChildren("sleep"), zombiesBefore);
}

TEST(PLT210_ProcessPosix_DetachedChildDoesNotHoldLauncherStdout)
{
    // Stand in for a supervisor reading this process's stdout: point fd 1 at
    // a pipe while launching a long-lived detached child, then check that the
    // pipe reaches EOF as soon as this process's own copy is closed.
    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(pipe(pipeFds), 0);
    std::cout.flush();
    std::fflush(stdout);
    const int savedStdout = dup(STDOUT_FILENO);
    ASSERT_TRUE(savedStdout >= 0);
    dup2(pipeFds[1], STDOUT_FILENO);
    close(pipeFds[1]);

    auto result = Spark::Process::Builder("/bin/sleep").Arg("3").Detached().Launch();

    dup2(savedStdout, STDOUT_FILENO);
    close(savedStdout);
    ASSERT_TRUE(result.has_value());

    pollfd readEnd{pipeFds[0], POLLIN, 0};
    const int ready = poll(&readEnd, 1, 1000);
    char byte = 0;
    const ssize_t readResult = ready > 0 ? read(pipeFds[0], &byte, 1) : -1;
    close(pipeFds[0]);

    // Before the fix the detached sleep inherited the write end, so poll timed
    // out (ready == 0) and a reader blocked until the child exited.
    EXPECT_EQ(ready, 1);
    EXPECT_EQ(readResult, static_cast<ssize_t>(0));
}

#endif // __linux__
