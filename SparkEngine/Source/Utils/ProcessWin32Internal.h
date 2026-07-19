/**
 * @file ProcessWin32Internal.h
 * @brief Shared Windows-only Process::Impl definition for the ProcessWin32*.cpp split parts
 */
#pragma once
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "Process.h"

#include <string>

#include <windows.h>

namespace Spark
{

    // =========================================================================
    // Impl
    // =========================================================================

    struct Process::Impl
    {
        HANDLE processHandle = NULL;
        bool detached = false;

        HANDLE stdinWrite = NULL;
        HANDLE stdoutRead = NULL;
        HANDLE stderrRead = NULL;

        std::string stdoutBuffer; ///< Partial line buffer for TryReadLine.

        int exitStatus = -1;
        bool exited = false;

        ~Impl() { Cleanup(); }

        void Cleanup()
        {
            CloseH(stdinWrite);
            CloseH(stdoutRead);
            CloseH(stderrRead);

            if (processHandle && !detached)
            {
                DWORD ec;
                if (GetExitCodeProcess(processHandle, &ec) && ec == STILL_ACTIVE)
                {
                    TerminateProcess(processHandle, 1);
                    WaitForSingleObject(processHandle, 1000);
                }
                CloseHandle(processHandle);
                processHandle = NULL;
            }
        }

        static void CloseH(HANDLE& handle)
        {
            if (handle)
            {
                CloseHandle(handle);
                handle = NULL;
            }
        }

        void PollExitStatus()
        {
            if (exited || !processHandle || detached)
                return;
            DWORD ec;
            if (GetExitCodeProcess(processHandle, &ec) && ec != STILL_ACTIVE)
            {
                exited = true;
                exitStatus = static_cast<int>(ec);
            }
        }

        /// Read all available bytes from a pipe handle (non-blocking via PeekNamedPipe).
        static bool ReadAvailable(HANDLE h, std::string& dest)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
                return false;
            if (avail == 0)
                return true; // No data, not EOF

            char buf[4096];
            while (avail > 0)
            {
                DWORD toRead = (avail < sizeof(buf)) ? avail : static_cast<DWORD>(sizeof(buf));
                DWORD bytesRead = 0;
                if (!ReadFile(h, buf, toRead, &bytesRead, NULL) || bytesRead == 0)
                    return false;
                dest.append(buf, bytesRead);
                avail -= bytesRead;
            }
            return true;
        }

        /// Blocking read until the pipe is closed.
        static std::string ReadUntilEof(HANDLE h)
        {
            std::string result;
            char buf[4096];
            for (;;)
            {
                DWORD bytesRead = 0;
                if (!ReadFile(h, buf, sizeof(buf), &bytesRead, NULL) || bytesRead == 0)
                    break;
                result.append(buf, bytesRead);
            }
            return result;
        }
    };

} // namespace Spark

#endif // SPARK_PLATFORM_WINDOWS
