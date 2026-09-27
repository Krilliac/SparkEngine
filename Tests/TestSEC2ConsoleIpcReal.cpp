/**
 * @file TestSEC2ConsoleIpcReal.cpp
 * @brief SEC2 console-IPC security regressions, against the real code paths.
 *
 * Every test here failed (or hung past its own deadline) before its fix:
 * - Win32 launches inherited every inheritable handle in the process, so an
 *   unrelated pipe's write end stayed open inside the child.
 * - The SparkConsole mirror thread wrote to the child's stdin with an
 *   unbounded blocking write that Shutdown() then joined.
 * - Lines typed into the external console reached only a private five-command
 *   registry, never SimpleConsole.
 * - SparkConsole's history kept credential arguments verbatim.
 * - On POSIX, SparkConsole ignored engine-pipe EOF and never exited.
 * - SparkGameFPS let "nan" through as a time scale / coordinate.
 * - The Shipping gate removed only god/noclip.
 *
 * Registered as the SEC2ConsoleIpcReal CTest with a pinned count.
 */

#include "TestFramework.h"

#include "Utils/Process.h"

#include <chrono>
#include <string>

#ifdef _WIN32
#include "Utils/ProcessWin32HandleList.h"

#include <windows.h>

using namespace std::chrono_literals;
#endif

// =============================================================================
// Finding 3 - Win32 children inherit only their own standard handles
// =============================================================================

#ifdef _WIN32

TEST(SEC2Console_Win32ChildInheritsOnlyItsOwnPipeEnds)
{
    // Stand-in for another launcher's pipe (the build pipeline, a git capture):
    // its write end is inheritable while this launch happens.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = static_cast<DWORD>(sizeof(inheritable));
    inheritable.bInheritHandle = TRUE;
    HANDLE foreignRead = nullptr;
    HANDLE foreignWrite = nullptr;
    ASSERT_TRUE(CreatePipe(&foreignRead, &foreignWrite, &inheritable, 0) != FALSE);
    SetHandleInformation(foreignRead, HANDLE_FLAG_INHERIT, 0);

    // cmd.exe waits on its captured stdin, so it stays alive while we look.
    auto child = Spark::Process::Builder("cmd.exe").CaptureStdin().CaptureStdout().NoWindow().Launch();
    ASSERT_TRUE(child.has_value());

    // With the only writer closed here, the pipe is broken — unless the child
    // received a copy of the write end, which keeps it open (and a reader of it
    // would never see EOF until the unrelated child exits).
    CloseHandle(foreignWrite);
    DWORD available = 0;
    const BOOL stillOpen = PeekNamedPipe(foreignRead, nullptr, 0, nullptr, &available, nullptr);
    const DWORD peekError = stillOpen ? ERROR_SUCCESS : GetLastError();

    child->CloseStdin();
    if (!child->WaitForExit(5000ms))
        child->Kill();
    CloseHandle(foreignRead);

    EXPECT_FALSE(stillOpen != FALSE);
    EXPECT_EQ(peekError, static_cast<DWORD>(ERROR_BROKEN_PIPE));
}

TEST(SEC2Console_Win32HandleListSkipsNonInheritableAndDuplicates)
{
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    ASSERT_TRUE(CreatePipe(&readEnd, &writeEnd, nullptr, 0) != FALSE);

    Spark::ProcessDetail::InheritedHandleList list;
    // Not inheritable: listing it would make CreateProcess fail outright.
    list.AddIfInheritable(readEnd);
    list.AddIfInheritable(nullptr);
    list.AddIfInheritable(INVALID_HANDLE_VALUE);
    EXPECT_TRUE(list.Empty());

    // AddInheritable marks the handle and lists it once, however often it is named
    // (merged stdout/stderr share one handle; a duplicate fails the attribute).
    EXPECT_TRUE(list.AddInheritable(writeEnd));
    list.AddIfInheritable(writeEnd);
    EXPECT_EQ(list.Handles().size(), static_cast<size_t>(1));
    DWORD flags = 0;
    EXPECT_TRUE(GetHandleInformation(writeEnd, &flags) != FALSE);
    EXPECT_TRUE((flags & HANDLE_FLAG_INHERIT) != 0);
    EXPECT_EQ(list.Build(), static_cast<DWORD>(ERROR_SUCCESS));
    EXPECT_TRUE(list.Attributes() != nullptr);

    EXPECT_FALSE(list.AddInheritable(nullptr));

    CloseHandle(readEnd);
    CloseHandle(writeEnd);
}

#endif // _WIN32
