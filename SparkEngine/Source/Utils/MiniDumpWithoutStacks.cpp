/**
 * @file MiniDumpWithoutStacks.cpp
 * @brief Minidump writer that omits every thread's stack memory (OPS-100).
 */

#include "MiniDumpWithoutStacks.h"

#ifdef _WIN32

#include <algorithm>
#include <cstddef>
#include <limits>

namespace Spark::CrashDump
{
    namespace
    {
        struct StackRemoval
        {
            static constexpr size_t kMaxThreads = 1024;
            ULONG64 base[kMaxThreads]{};
            ULONG size[kMaxThreads]{};
            size_t count = 0;
            size_t next = 0;
            bool overflow = false;
        };

        // A job's state word: generation in the high bits, phase in the low two. The writer only
        // runs a job whose generation matches its own, so a writer that was abandoned and starts
        // late can neither run nor corrupt a later request.
        constexpr LONG kPhasePending = 0;
        constexpr LONG kPhaseRunning = 1;
        constexpr LONG kPhaseAbandoned = 2;
        constexpr LONG kPhaseMask = 3;
        constexpr SIZE_T kWriterStackReservation = 256 * 1024;

        struct WriterJob
        {
            HANDLE file = nullptr;
            MINIDUMP_TYPE type = MiniDumpNormal;
            MINIDUMP_EXCEPTION_INFORMATION* exception = nullptr;
            bool removeStacks = true;
            DWORD writerThreadId = 0;
            BOOL result = FALSE;
            DWORD error = ERROR_SUCCESS;
            volatile LONG state = 0;
            LONG generation = 0;
            StackRemoval removal;
        };

        BOOL CALLBACK FilterDump(PVOID param, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
        {
            auto* job = static_cast<WriterJob*>(param);
            if (!input || !output || !job)
                return TRUE;

            if (input->CallbackType == IncludeThreadCallback)
            {
                // The writer thread is an implementation detail: its record and stack stay out.
                return input->IncludeThread.ThreadId == job->writerThreadId ? FALSE : TRUE;
            }
            if (input->CallbackType == ReadMemoryFailureCallback)
            {
                // A page that vanished while the process kept running (an exiting thread's stack,
                // a freed region) is left out of the dump rather than failing the whole dump with
                // ERROR_PARTIAL_COPY. Omitting memory never adds anything to the file.
                output->Status = S_OK;
                return TRUE;
            }
            if (!job->removeStacks)
                return TRUE;

            StackRemoval& removal = job->removal;
            if (input->CallbackType == ThreadCallback || input->CallbackType == ThreadExCallback)
            {
                if (removal.count == StackRemoval::kMaxThreads)
                {
                    removal.overflow = true;
                    return TRUE;
                }
                ULONG64 low = (std::min)(input->Thread.StackBase, input->Thread.StackEnd);
                const ULONG64 high = (std::max)(input->Thread.StackBase, input->Thread.StackEnd);
                // Remove the whole stack reservation, not just [StackEnd, StackBase): the pages
                // below the reported stack pointer still hold returned frames' locals.
                MEMORY_BASIC_INFORMATION region{};
                if (high > low && VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(high - 1)), &region,
                                               sizeof(region)) != 0)
                    low = (std::min)(low, reinterpret_cast<ULONG64>(region.AllocationBase));
                removal.base[removal.count] = low;
                removal.size[removal.count] =
                    static_cast<ULONG>((std::min)(high - low, ULONG64{(std::numeric_limits<ULONG>::max)()}));
                ++removal.count;
            }
            else if (input->CallbackType == RemoveMemoryCallback)
            {
                // DbgHelp calls this repeatedly until it gets an empty range.
                output->MemoryBase = 0;
                output->MemorySize = 0;
                if (removal.next < removal.count)
                {
                    output->MemoryBase = removal.base[removal.next];
                    output->MemorySize = removal.size[removal.next];
                    ++removal.next;
                }
            }
            return TRUE;
        }

        // Static: ~12 KiB is too much for a possibly exhausted faulting stack. Callers serialize
        // through StackTrace::SymbolLockLease (see the header contract).
        WriterJob g_job;

        DWORD WINAPI DumpWriterThread(LPVOID param)
        {
            // The job is static; the parameter only carries the generation this thread serves.
            const auto generation = static_cast<LONG>(reinterpret_cast<ULONG_PTR>(param));
            const LONG pending = (generation << 2) | kPhasePending;
            if (InterlockedCompareExchange(&g_job.state, (generation << 2) | kPhaseRunning, pending) != pending)
                return 0; // The requester gave up (or moved on) before this thread could start.

            g_job.writerThreadId = GetCurrentThreadId();
            MINIDUMP_CALLBACK_INFORMATION callback{FilterDump, &g_job};
            SetLastError(ERROR_SUCCESS);
            g_job.result = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), g_job.file, g_job.type,
                                             g_job.exception, nullptr, &callback);
            g_job.error = g_job.result ? ERROR_SUCCESS : GetLastError();
            return 0;
        }
    } // namespace

    BOOL WriteOnWriterThread(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* exception,
                             bool removeStacks, DWORD startTimeoutMs, DWORD* error)
    {
        WriterJob& job = g_job;
        job.file = file;
        job.type = type;
        job.exception = exception;
        job.removeStacks = removeStacks;
        job.writerThreadId = 0;
        job.result = FALSE;
        job.error = ERROR_SUCCESS;
        job.removal.count = 0;
        job.removal.next = 0;
        job.removal.overflow = false;
        job.generation = (job.generation + 1) & 0x0FFFFFFF;
        const LONG generation = job.generation;
        InterlockedExchange(&job.state, (generation << 2) | kPhasePending);

        // DbgHelp must not run on the requesting thread: its own frames would overlay that
        // thread's stale stack bytes (older DbgHelp builds copy such scratch into the file) and
        // it can fail with ERROR_PARTIAL_COPY on its own live stack. A fresh thread's stack never
        // held a caller secret, and the callback keeps that thread out of the dump.
        HANDLE thread = CreateThread(nullptr, kWriterStackReservation, DumpWriterThread,
                                     reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(generation)),
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!thread)
        {
            const DWORD failure = GetLastError();
            InterlockedExchange(&job.state, (generation << 2) | kPhaseAbandoned);
            if (error)
                *error = failure != ERROR_SUCCESS ? failure : ERROR_GEN_FAILURE;
            return FALSE;
        }

        if (WaitForSingleObject(thread, startTimeoutMs) != WAIT_OBJECT_0)
        {
            // The writer may be stuck before it starts (for example in DLL_THREAD_ATTACH while
            // this thread holds the loader lock). Fail closed: no dump, and never a same-thread
            // fallback. A writer that already started owns the job until it finishes.
            const LONG pending = (generation << 2) | kPhasePending;
            if (InterlockedCompareExchange(&job.state, (generation << 2) | kPhaseAbandoned, pending) == pending)
            {
                CloseHandle(thread);
                if (error)
                    *error = ERROR_TIMEOUT;
                return FALSE;
            }
            WaitForSingleObject(thread, INFINITE);
        }
        CloseHandle(thread);

        BOOL result = job.result;
        DWORD failure = job.error;
        if (result && job.removal.overflow)
        {
            // Some stacks could not be removed: never leave that dump on disk.
            LARGE_INTEGER start{};
            SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
            SetEndOfFile(file);
            result = FALSE;
            failure = ERROR_BUFFER_OVERFLOW;
        }
        if (error)
            *error = failure;
        return result;
    }

    BOOL WriteWithoutStacks(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* exception, DWORD* error)
    {
        return WriteOnWriterThread(file, type, exception, true, kWriterStartTimeoutMs, error);
    }
} // namespace Spark::CrashDump

#endif
