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

        BOOL CALLBACK OmitThreadStacks(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                       PMINIDUMP_CALLBACK_OUTPUT output)
        {
            auto* removal = static_cast<StackRemoval*>(param);
            if (!input || !output || !removal)
                return TRUE;

            if (input->CallbackType == ThreadCallback || input->CallbackType == ThreadExCallback)
            {
                if (removal->count == StackRemoval::kMaxThreads)
                {
                    removal->overflow = true;
                    return TRUE;
                }
                ULONG64 low = (std::min)(input->Thread.StackBase, input->Thread.StackEnd);
                const ULONG64 high = (std::max)(input->Thread.StackBase, input->Thread.StackEnd);
                // Remove the whole stack reservation, not just [StackEnd, StackBase). Which stack
                // pointer DbgHelp reports here and which one it copies from can differ for the
                // dumping thread (the supplied exception context versus its own live one), and
                // the pages below either still hold returned frames' locals.
                MEMORY_BASIC_INFORMATION region{};
                if (high > low && VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(high - 1)), &region,
                                               sizeof(region)) != 0)
                    low = (std::min)(low, reinterpret_cast<ULONG64>(region.AllocationBase));
                removal->base[removal->count] = low;
                removal->size[removal->count] =
                    static_cast<ULONG>((std::min)(high - low, ULONG64{(std::numeric_limits<ULONG>::max)()}));
                ++removal->count;
            }
            else if (input->CallbackType == RemoveMemoryCallback)
            {
                // DbgHelp calls this repeatedly until it gets an empty range.
                output->MemoryBase = 0;
                output->MemorySize = 0;
                if (removal->next < removal->count)
                {
                    output->MemoryBase = removal->base[removal->next];
                    output->MemorySize = removal->size[removal->next];
                    ++removal->next;
                }
            }
            return TRUE;
        }
    } // namespace

    BOOL WriteWithoutStacks(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* exception, DWORD* error)
    {
        // Static: ~12 KiB is too much for a possibly exhausted faulting stack. Callers serialize
        // through StackTrace::SymbolLockLease (see the header contract).
        static StackRemoval removal;
        removal.count = 0;
        removal.next = 0;
        removal.overflow = false;

        MINIDUMP_CALLBACK_INFORMATION callback{OmitThreadStacks, &removal};
        SetLastError(ERROR_SUCCESS);
        BOOL result =
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception, nullptr, &callback);
        DWORD failure = result ? ERROR_SUCCESS : GetLastError();
        if (result && removal.overflow)
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
} // namespace Spark::CrashDump

#endif
